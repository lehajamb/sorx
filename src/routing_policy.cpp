#include "sorx/routing_policy.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace sorx {
namespace {

[[nodiscard]] const CandidateRuntimeView* find_runtime_view(
    const std::span<const CandidateRuntimeView> views,
    const VenueId venue) noexcept {
    const auto found = std::find_if(
        views.begin(),
        views.end(),
        [venue](const CandidateRuntimeView& view) { return view.venue == venue; });
    return found == views.end() ? nullptr : &*found;
}

void exclude_once(
    RouteDecision& decision,
    const VenueId venue,
    const CandidateExclusionReason reason) {
    const auto found = std::find_if(
        decision.exclusions.begin(),
        decision.exclusions.end(),
        [venue](const CandidateExclusion& exclusion) { return exclusion.venue == venue; });
    if (found == decision.exclusions.end()) {
        decision.exclusions.push_back(CandidateExclusion{venue, reason});
    }
}

void pin_candidate_versions(
    RouteDecision& decision,
    const CandidateSet& candidates,
    const std::span<const CandidateRuntimeView> runtime_views) {
    decision.candidate_pins.reserve(candidates.candidates.size());
    for (const auto& candidate : candidates.candidates) {
        const CandidateRuntimeView* runtime =
            find_runtime_view(runtime_views, candidate.venue);
        CandidateVersionPin pin;
        pin.venue = candidate.venue;
        if (runtime != nullptr) {
            pin.adjacency_generation = runtime->adjacency_generation;
            if (runtime->book.has_value()) {
                pin.book_version = runtime->book->version();
            }
        }
        decision.candidate_pins.push_back(std::move(pin));
    }
}

[[nodiscard]] std::optional<CandidateExclusionReason> common_exclusion(
    const ParentOrder& parent,
    const CandidateHandle& candidate,
    const CandidateRuntimeView* runtime) {
    if (candidate.admin_state != AdminState::Up) {
        return CandidateExclusionReason::AdminDown;
    }
    if (runtime == nullptr) {
        return CandidateExclusionReason::MissingRuntimeState;
    }
    if (!runtime->book) {
        return CandidateExclusionReason::MissingBook;
    }
    if (!runtime->book->valid()) {
        return CandidateExclusionReason::InvalidBook;
    }
    if (runtime->book->venue != candidate.venue ||
        runtime->book->instrument != parent.instrument) {
        return CandidateExclusionReason::BookIdentityMismatch;
    }
    if (runtime->market_data_state != MarketDataState::Live ||
        runtime->book->feed_state != MarketDataState::Live) {
        return CandidateExclusionReason::MarketDataNotLive;
    }
    if (!runtime->feed_sufficiently_fresh) {
        return CandidateExclusionReason::StaleBook;
    }
    if (runtime->instrument_state != InstrumentState::Trading ||
        runtime->book->trading_state != InstrumentState::Trading) {
        return CandidateExclusionReason::InstrumentNotTrading;
    }
    if (runtime->order_entry_state != OrderEntryState::Ready) {
        return CandidateExclusionReason::OrderEntryNotReady;
    }
    if (runtime->egress_state != EgressState::Ready) {
        return CandidateExclusionReason::EgressCongested;
    }
    if (!runtime->conversion_compatible) {
        return CandidateExclusionReason::ConversionIncompatible;
    }
    if (!candidate.price_conversion.convert_exact(parent.limit_price)) {
        return CandidateExclusionReason::InexactPriceConversion;
    }
    if (!runtime->risk_allowed) {
        return CandidateExclusionReason::RiskProhibited;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_parent(const ParentOrder& parent) noexcept {
    return static_cast<bool>(parent.id) &&
           static_cast<bool>(parent.instrument) &&
           static_cast<bool>(parent.quantity) &&
           (parent.side == Side::Buy || parent.side == Side::Sell);
}

[[nodiscard]] bool price_is_executable(
    const Side side,
    const Price level_price,
    const Price protection_limit) noexcept {
    return side == Side::Buy ? level_price <= protection_limit
                             : level_price >= protection_limit;
}

[[nodiscard]] bool price_precedes(
    const Side side,
    const Price lhs,
    const Price rhs) noexcept {
    return side == Side::Buy ? lhs < rhs : lhs > rhs;
}

[[nodiscard]] const md::Level& opposite_level(
    const md::BookView& book,
    const Side side,
    const std::size_t index) noexcept {
    return side == Side::Buy ? book.asks[index] : book.bids[index];
}

[[nodiscard]] std::size_t opposite_level_count(
    const md::BookView& book,
    const Side side) noexcept {
    return side == Side::Buy ? book.ask_count : book.bid_count;
}

struct FillSlice final {
    Price price{};
    std::uint64_t quantity{};
};

struct AggressiveCursor final {
    const CandidateHandle* candidate{};
    const CandidateRuntimeView* runtime{};
    const md::BookView* book{};
    std::size_t level_index{};
    std::uint64_t cap_remaining{};
    std::uint64_t total{};
    std::size_t selection_order{std::numeric_limits<std::size_t>::max()};
    bool selected{};
    bool blocked_by_maximum{};
    bool exhausted_by_limit{};
    bool exhausted_by_cap{};
    bool conversion_failure{};
    std::vector<FillSlice> fills{};
};

[[nodiscard]] bool cursor_precedes(
    const Side side,
    const AggressiveCursor& lhs,
    const AggressiveCursor& rhs) noexcept {
    const auto& lhs_level = opposite_level(*lhs.book, side, lhs.level_index);
    const auto& rhs_level = opposite_level(*rhs.book, side, rhs.level_index);
    if (lhs_level.price != rhs_level.price) {
        return price_precedes(side, lhs_level.price, rhs_level.price);
    }
    if (lhs.candidate->aggressive_priority != rhs.candidate->aggressive_priority) {
        return lhs.candidate->aggressive_priority < rhs.candidate->aggressive_priority;
    }
    return lhs.candidate->venue < rhs.candidate->venue;
}

void trim_to_exact_quantity(AggressiveCursor& cursor, bool& inexact_conversion) {
    const auto denominator = cursor.candidate->quantity_conversion.denominator();
    auto trim = cursor.total % denominator;
    if (trim == 0) {
        return;
    }

    inexact_conversion = true;
    cursor.total -= trim;
    for (auto slice = cursor.fills.rbegin(); slice != cursor.fills.rend() && trim != 0; ++slice) {
        const auto reduction = std::min(slice->quantity, trim);
        slice->quantity -= reduction;
        trim -= reduction;
    }
}

[[nodiscard]] const FillSlice* worst_retained_slice(const AggressiveCursor& cursor) noexcept {
    const auto found = std::find_if(
        cursor.fills.rbegin(),
        cursor.fills.rend(),
        [](const FillSlice& slice) { return slice.quantity != 0; });
    return found == cursor.fills.rend() ? nullptr : &*found;
}

}  // namespace

RouteDecision RoutingPolicy::aggressive_ioc(
    const ParentOrder& parent,
    const CandidateTable& candidates,
    const std::span<const CandidateRuntimeView> runtime_views,
    const std::size_t maximum_venues) {
    RouteDecision decision;
    decision.candidate_table_generation = candidates.generation();
    decision.remainder = parent.quantity;

    if (!valid_parent(parent) || maximum_venues == 0) {
        decision.reject_reason = RouteRejectReason::InvalidParent;
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
        return decision;
    }
    if (parent.intent != RoutingIntent::AggressiveIOC) {
        decision.reject_reason = RouteRejectReason::IntentMismatch;
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
        return decision;
    }

    const auto* candidate_set = candidates.find(parent.instrument);
    if (candidate_set == nullptr) {
        decision.reject_reason = RouteRejectReason::InstrumentNotConfigured;
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
        return decision;
    }

    pin_candidate_versions(decision, *candidate_set, runtime_views);

    std::vector<AggressiveCursor> cursors;
    cursors.reserve(candidate_set->candidates.size());
    for (const auto& candidate : candidate_set->candidates) {
        if (!candidate.capabilities.aggressive_ioc) {
            exclude_once(decision, candidate.venue, CandidateExclusionReason::UnsupportedIntent);
            continue;
        }
        const auto* runtime = find_runtime_view(runtime_views, candidate.venue);
        if (const auto exclusion = common_exclusion(parent, candidate, runtime)) {
            exclude_once(decision, candidate.venue, *exclusion);
            continue;
        }

        const auto* book = &*runtime->book;
        decision.observed_truncated_depth =
            decision.observed_truncated_depth || book->depth_truncated;
        cursors.push_back(AggressiveCursor{
            &candidate,
            runtime,
            book,
            0,
            candidate.allocation_cap.units,
        });
    }

    std::uint64_t remaining = parent.quantity.units;
    std::size_t selected_venues = 0;
    bool maximum_blocked = false;
    bool limit_blocked = false;
    bool cap_blocked = false;
    bool inexact_conversion = false;

    while (remaining != 0) {
        AggressiveCursor* best = nullptr;
        for (auto& cursor : cursors) {
            if (cursor.cap_remaining == 0 || cursor.blocked_by_maximum ||
                cursor.conversion_failure) {
                continue;
            }
            if (cursor.level_index >= opposite_level_count(*cursor.book, parent.side)) {
                continue;
            }

            const auto& level = opposite_level(*cursor.book, parent.side, cursor.level_index);
            if (!price_is_executable(parent.side, level.price, parent.limit_price)) {
                cursor.exhausted_by_limit = true;
                limit_blocked = true;
                continue;
            }
            if (!cursor.candidate->price_conversion.convert_exact(level.price)) {
                cursor.conversion_failure = true;
                inexact_conversion = true;
                exclude_once(
                    decision,
                    cursor.candidate->venue,
                    CandidateExclusionReason::InexactPriceConversion);
                continue;
            }
            if (!cursor.selected && selected_venues >= maximum_venues) {
                cursor.blocked_by_maximum = true;
                maximum_blocked = true;
                exclude_once(
                    decision,
                    cursor.candidate->venue,
                    CandidateExclusionReason::MaximumVenueCount);
                continue;
            }
            if (best == nullptr || cursor_precedes(parent.side, cursor, *best)) {
                best = &cursor;
            }
        }

        if (best == nullptr) {
            break;
        }
        if (!best->selected) {
            best->selected = true;
            best->selection_order = selected_venues++;
        }

        const auto& level = opposite_level(*best->book, parent.side, best->level_index);
        const auto take = std::min({level.quantity.units, remaining, best->cap_remaining});
        best->fills.push_back(FillSlice{level.price, take});
        best->total += take;
        best->cap_remaining -= take;
        remaining -= take;
        ++best->level_index;

        if (best->cap_remaining == 0 && remaining != 0) {
            best->exhausted_by_cap = true;
            cap_blocked = true;
        }
    }

    for (auto& cursor : cursors) {
        if (!cursor.selected &&
            cursor.level_index >= opposite_level_count(*cursor.book, parent.side)) {
            exclude_once(
                decision,
                cursor.candidate->venue,
                CandidateExclusionReason::NoExecutableDepth);
        }
        trim_to_exact_quantity(cursor, inexact_conversion);
    }

    std::vector<AggressiveCursor*> selected;
    selected.reserve(cursors.size());
    for (auto& cursor : cursors) {
        if (cursor.total != 0) {
            selected.push_back(&cursor);
        } else if (cursor.selected) {
            exclude_once(
                decision,
                cursor.candidate->venue,
                CandidateExclusionReason::InexactQuantityConversion);
        }
    }
    std::sort(
        selected.begin(),
        selected.end(),
        [](const AggressiveCursor* lhs, const AggressiveCursor* rhs) {
            return lhs->selection_order < rhs->selection_order;
        });

    std::uint32_t allocation_order = 0;
    for (const auto* cursor : selected) {
        const auto* worst = worst_retained_slice(*cursor);
        if (worst == nullptr) {
            continue;
        }
        const auto canonical_quantity = Quantity{cursor->total};
        const auto venue_quantity = cursor->candidate->quantity_conversion.convert_exact(
            canonical_quantity);
        const auto venue_price = cursor->candidate->price_conversion.convert_exact(worst->price);
        if (!venue_quantity || !venue_price) {
            inexact_conversion = true;
            exclude_once(
                decision,
                cursor->candidate->venue,
                !venue_quantity ? CandidateExclusionReason::InexactQuantityConversion
                                : CandidateExclusionReason::InexactPriceConversion);
            continue;
        }

        decision.allocations.push_back(ChildAllocation{
            cursor->candidate->venue,
            canonical_quantity,
            worst->price,
            *venue_quantity,
            *venue_price,
            allocation_order++,
            cursor->runtime->adjacency_generation,
            cursor->book->version(),
        });
        decision.allocated.units += canonical_quantity.units;
    }

    decision.remainder = Quantity{parent.quantity.units - decision.allocated.units};
    if (decision.allocations.empty()) {
        decision.reject_reason = cursors.empty() ? RouteRejectReason::NoEligibleCandidate
                                                 : RouteRejectReason::NoExecutableLiquidity;
    }

    if (!decision.remainder) {
        decision.remainder_reason = RemainderReason::None;
    } else if (inexact_conversion) {
        decision.remainder_reason = RemainderReason::InexactConversion;
    } else if (maximum_blocked) {
        decision.remainder_reason = RemainderReason::MaximumVenueCount;
    } else if (cap_blocked) {
        decision.remainder_reason = RemainderReason::VenueAllocationCaps;
    } else if (limit_blocked) {
        decision.remainder_reason = RemainderReason::ParentLimitReached;
    } else if (decision.observed_truncated_depth) {
        decision.remainder_reason = RemainderReason::TruncatedDepthUnknown;
    } else if (cursors.empty()) {
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
    } else {
        decision.remainder_reason = RemainderReason::InsufficientDisplayedDepth;
    }
    return decision;
}

RouteDecision RoutingPolicy::passive_gtc(
    const ParentOrder& parent,
    const CandidateTable& candidates,
    const std::span<const CandidateRuntimeView> runtime_views) {
    RouteDecision decision;
    decision.candidate_table_generation = candidates.generation();
    decision.remainder = parent.quantity;

    if (!valid_parent(parent)) {
        decision.reject_reason = RouteRejectReason::InvalidParent;
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
        return decision;
    }
    if (parent.intent != RoutingIntent::PassiveGTC) {
        decision.reject_reason = RouteRejectReason::IntentMismatch;
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
        return decision;
    }

    const auto* candidate_set = candidates.find(parent.instrument);
    if (candidate_set == nullptr) {
        decision.reject_reason = RouteRejectReason::InstrumentNotConfigured;
        decision.remainder_reason = RemainderReason::NoEligibleCandidate;
        return decision;
    }

    pin_candidate_versions(decision, *candidate_set, runtime_views);

    struct Eligible final {
        const CandidateHandle* candidate;
        const CandidateRuntimeView* runtime;
    };
    std::vector<Eligible> common_eligible;
    common_eligible.reserve(candidate_set->candidates.size());
    bool marketable = false;

    for (const auto& candidate : candidate_set->candidates) {
        const auto* runtime = find_runtime_view(runtime_views, candidate.venue);
        if (const auto exclusion = common_exclusion(parent, candidate, runtime)) {
            exclude_once(decision, candidate.venue, *exclusion);
            continue;
        }

        const auto& book = *runtime->book;
        decision.observed_truncated_depth =
            decision.observed_truncated_depth || book.depth_truncated;
        common_eligible.push_back(Eligible{&candidate, runtime});
        if (parent.side == Side::Buy && book.ask_count != 0 &&
            book.asks[0].price <= parent.limit_price) {
            marketable = true;
        }
        if (parent.side == Side::Sell && book.bid_count != 0 &&
            book.bids[0].price >= parent.limit_price) {
            marketable = true;
        }
    }

    if (marketable) {
        decision.reject_reason = RouteRejectReason::PassiveMarketable;
        decision.remainder_reason = RemainderReason::PassiveFullQuantityUnavailable;
        return decision;
    }

    std::vector<Eligible> passive_eligible;
    passive_eligible.reserve(common_eligible.size());
    for (const auto& eligible : common_eligible) {
        const auto& candidate = *eligible.candidate;
        if (!candidate.capabilities.passive_gtc) {
            exclude_once(decision, candidate.venue, CandidateExclusionReason::UnsupportedIntent);
            continue;
        }
        if (candidate.allocation_cap < parent.quantity) {
            exclude_once(
                decision,
                candidate.venue,
                CandidateExclusionReason::AllocationCapTooSmall);
            continue;
        }
        if (!candidate.quantity_conversion.convert_exact(parent.quantity)) {
            exclude_once(
                decision,
                candidate.venue,
                CandidateExclusionReason::InexactQuantityConversion);
            continue;
        }
        passive_eligible.push_back(eligible);
    }

    if (passive_eligible.empty()) {
        decision.reject_reason = common_eligible.empty()
                                     ? RouteRejectReason::NoEligibleCandidate
                                     : RouteRejectReason::PassiveFullQuantityUnavailable;
        decision.remainder_reason = common_eligible.empty()
                                        ? RemainderReason::NoEligibleCandidate
                                        : RemainderReason::PassiveFullQuantityUnavailable;
        return decision;
    }

    const auto selected = std::min_element(
        passive_eligible.begin(),
        passive_eligible.end(),
        [](const Eligible& lhs, const Eligible& rhs) {
            return std::tie(
                       lhs.candidate->passive_priority,
                       lhs.candidate->configured_latency_ns,
                       lhs.candidate->venue) <
                   std::tie(
                       rhs.candidate->passive_priority,
                       rhs.candidate->configured_latency_ns,
                       rhs.candidate->venue);
        });

    const auto venue_quantity = selected->candidate->quantity_conversion.convert_exact(
        parent.quantity);
    const auto venue_price = selected->candidate->price_conversion.convert_exact(
        parent.limit_price);
    if (!venue_quantity || !venue_price) {
        decision.reject_reason = RouteRejectReason::PassiveFullQuantityUnavailable;
        decision.remainder_reason = RemainderReason::InexactConversion;
        return decision;
    }

    decision.allocations.push_back(ChildAllocation{
        selected->candidate->venue,
        parent.quantity,
        parent.limit_price,
        *venue_quantity,
        *venue_price,
        0,
        selected->runtime->adjacency_generation,
        selected->runtime->book->version(),
    });
    decision.allocated = parent.quantity;
    decision.remainder = Quantity{};
    decision.remainder_reason = RemainderReason::None;
    return decision;
}

}  // namespace sorx




