#include "sorx/dataplane/route_kernel.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sorx::dataplane {
namespace {

inline constexpr std::size_t kInstrumentMapCapacity = 512U;
inline constexpr std::size_t kVenueMapCapacity = 128U;
inline constexpr std::size_t kChildMapCapacity =
    kMaximumActiveParents * kMaximumCandidatesPerInstrument * 2U;
inline constexpr std::size_t kInvalidIndex =
    std::numeric_limits<std::size_t>::max();

[[nodiscard]] constexpr bool valid_side(const Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] constexpr bool valid_intent(const RoutingIntent intent) noexcept {
    return intent == RoutingIntent::AggressiveIOC ||
           intent == RoutingIntent::PassiveGTC;
}

[[nodiscard]] constexpr std::uint64_t mix_id(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

struct WireIdentityKey final {
    std::uint64_t raw{};

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return raw; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return raw != 0U;
    }
    constexpr auto operator<=>(const WireIdentityKey&) const noexcept = default;
};

[[nodiscard]] WireIdentityKey make_wire_identity_key(
    const VenueId venue,
    const std::uint64_t venue_session_id,
    const std::uint64_t venue_session_epoch,
    const InlineWireIdentity& identity) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto absorb = [&hash](const std::uint64_t value) noexcept {
        for (std::size_t byte{}; byte < sizeof(value); ++byte) {
            hash ^= (value >> (byte * 8U)) & 0xffU;
            hash *= 1099511628211ULL;
        }
    };
    absorb(venue.value());
    absorb(venue_session_id);
    absorb(venue_session_epoch);
    absorb(identity.size);
    for (const std::byte value : identity.value()) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return WireIdentityKey{hash == 0U ? 1U : hash};
}

template <typename Id, std::size_t Capacity>
class FixedIndexMap final {
    static_assert((Capacity & (Capacity - 1U)) == 0U);

public:
    [[nodiscard]] bool insert(const Id id, const std::size_t index) noexcept {
        std::size_t slot = static_cast<std::size_t>(mix_id(id.value())) &
                           (Capacity - 1U);
        std::size_t first_tombstone = Capacity;
        for (std::size_t probe = 0U; probe < Capacity; ++probe) {
            Entry& entry = entries_[slot];
            if (entry.state == EntryState::Tombstone &&
                first_tombstone == Capacity) {
                first_tombstone = slot;
            }
            if (entry.state == EntryState::Empty) {
                Entry& destination = first_tombstone == Capacity
                    ? entry
                    : entries_[first_tombstone];
                destination.id = id;
                destination.index = index;
                destination.state = EntryState::Occupied;
                return true;
            }
            if (entry.state == EntryState::Occupied && entry.id == id) {
                return false;
            }
            slot = (slot + 1U) & (Capacity - 1U);
        }
        if (first_tombstone != Capacity) {
            Entry& destination = entries_[first_tombstone];
            destination.id = id;
            destination.index = index;
            destination.state = EntryState::Occupied;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::size_t> find(const Id id) const noexcept {
        if (!id) {
            return std::nullopt;
        }
        std::size_t slot = static_cast<std::size_t>(mix_id(id.value())) &
                           (Capacity - 1U);
        for (std::size_t probe = 0U; probe < Capacity; ++probe) {
            const Entry& entry = entries_[slot];
            if (entry.state == EntryState::Empty) {
                return std::nullopt;
            }
            if (entry.state == EntryState::Occupied && entry.id == id) {
                return entry.index;
            }
            slot = (slot + 1U) & (Capacity - 1U);
        }
        return std::nullopt;
    }

    [[nodiscard]] bool erase(const Id id) noexcept {
        if (!id) {
            return false;
        }
        std::size_t slot = static_cast<std::size_t>(mix_id(id.value())) &
                           (Capacity - 1U);
        for (std::size_t probe = 0U; probe < Capacity; ++probe) {
            Entry& entry = entries_[slot];
            if (entry.state == EntryState::Empty) {
                return false;
            }
            if (entry.state == EntryState::Occupied && entry.id == id) {
                entry.state = EntryState::Tombstone;
                return true;
            }
            slot = (slot + 1U) & (Capacity - 1U);
        }
        return false;
    }

private:
    enum class EntryState : std::uint8_t {
        Empty,
        Occupied,
        Tombstone,
    };

    struct Entry final {
        Id id{};
        std::size_t index{};
        EntryState state{EntryState::Empty};
    };

    std::array<Entry, Capacity> entries_{};
};

struct NumericCandidate final {
    VenueId venue{};
    RationalConversion price_conversion{};
    RationalConversion quantity_conversion{};
    VenueCapabilities capabilities{};
    Quantity allocation_cap{};
    std::uint32_t aggressive_priority{};
    std::uint32_t passive_priority{};
    std::uint64_t configured_latency_ns{};
    AdminState admin_state{AdminState::Up};
};

struct FixedCandidateSet final {
    InstrumentId instrument{};
    std::array<NumericCandidate, kMaximumCandidatesPerInstrument> candidates{};
    std::uint16_t count{};
};

struct InstrumentSlot final {
    InstrumentRiskLimit limit{};
    Quantity outstanding{};
    std::uint64_t open_parents{};
    std::uint64_t open_children{};
};

struct VenueSlot final {
    VenueRiskLimit limit{};
    Quantity outstanding{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t reported_normal_tx_credits{};
    std::uint32_t reported_cancel_tx_credits{};
    std::uint32_t reported_throttle_credits{};
    std::uint32_t reserved_normal_tx_credits{};
    std::uint32_t reserved_cancel_tx_credits{};
    std::uint32_t reserved_throttle_credits{};
};

struct FixedViews final {
    std::array<const BookSlotView*, kMaximumCandidatesPerInstrument>
        by_candidate{};
};

[[nodiscard]] bool map_views(
    const FixedCandidateSet& candidates,
    const InstrumentId instrument,
    const std::span<const BookSlotView> views,
    FixedViews& output) noexcept {
    if (views.size() > candidates.count) {
        return false;
    }
    for (const BookSlotView& view : views) {
        if (!view.valid() || view.instrument != instrument) {
            return false;
        }
        std::size_t candidate_index = kInvalidIndex;
        for (std::size_t index = 0U; index < candidates.count; ++index) {
            if (candidates.candidates[index].venue == view.venue) {
                candidate_index = index;
                break;
            }
        }
        if (candidate_index == kInvalidIndex ||
            output.by_candidate[candidate_index] != nullptr) {
            return false;
        }
        output.by_candidate[candidate_index] = &view;
    }
    return true;
}

void exclude_once(
    RoutePlan& plan,
    const VenueId venue,
    const CandidateExclusionReason reason) noexcept {
    for (std::size_t index = 0U; index < plan.exclusion_count; ++index) {
        if (plan.exclusions[index].venue == venue) {
            return;
        }
    }
    if (plan.exclusion_count < plan.exclusions.size()) {
        plan.exclusions[plan.exclusion_count++] = CandidateExclusion{venue, reason};
    }
}

[[nodiscard]] constexpr bool price_is_executable(
    const Side side,
    const Price price,
    const Price limit) noexcept {
    return side == Side::Buy ? price <= limit : price >= limit;
}

[[nodiscard]] constexpr bool price_precedes(
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

[[nodiscard]] bool same_pin(
    const DataplaneCandidatePin& lhs,
    const DataplaneCandidatePin& rhs) noexcept {
    return lhs == rhs;
}

[[nodiscard]] bool equivalent_plan(
    const RoutePlan& lhs,
    const RoutePlan& rhs) noexcept {
    if (lhs.parent != rhs.parent ||
        lhs.kernel_state_generation != rhs.kernel_state_generation ||
        lhs.candidate_table_generation != rhs.candidate_table_generation ||
        lhs.risk_generation != rhs.risk_generation ||
        lhs.next_child_order_id != rhs.next_child_order_id ||
        lhs.next_dispatch_attempt_id != rhs.next_dispatch_attempt_id ||
        lhs.candidate_pin_count != rhs.candidate_pin_count ||
        lhs.allocation_count != rhs.allocation_count ||
        lhs.exclusion_count != rhs.exclusion_count ||
        lhs.allocated != rhs.allocated || lhs.remainder != rhs.remainder ||
        lhs.remainder_reason != rhs.remainder_reason ||
        lhs.reject_reason != rhs.reject_reason ||
        lhs.observed_truncated_depth != rhs.observed_truncated_depth) {
        return false;
    }
    for (std::size_t index = 0U; index < lhs.candidate_pin_count; ++index) {
        if (lhs.candidate_pins[index] != rhs.candidate_pins[index]) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < lhs.allocation_count; ++index) {
        if (lhs.allocations[index] != rhs.allocations[index]) {
            return false;
        }
        if (lhs.planned_children[index] != rhs.planned_children[index]) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < lhs.exclusion_count; ++index) {
        if (lhs.exclusions[index] != rhs.exclusions[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool equivalent_wire_ownership(
    const RoutePlan& lhs,
    const RoutePlan& rhs) noexcept {
    if (lhs.allocation_count != rhs.allocation_count) {
        return false;
    }
    for (std::size_t index{}; index < lhs.allocation_count; ++index) {
        const PlannedChild& left = lhs.planned_children[index];
        const PlannedChild& right = rhs.planned_children[index];
        if (left.child_order_id != right.child_order_id ||
            left.dispatch_attempt_id != right.dispatch_attempt_id ||
            left.wire_identity != right.wire_identity ||
            left.venue_session_id != right.venue_session_id ||
            left.venue_session_epoch != right.venue_session_epoch ||
            left.ownership_generation != right.ownership_generation) {
            return false;
        }
    }
    return true;
}

}  // namespace

struct RouteKernel::Impl final {
    struct Reservation final {
        ChildOrderId child{};
        std::uint64_t dispatch_attempt_id{};
        std::uint64_t last_dispatch_attempt_id{};
        VenueId venue{};
        std::uint64_t venue_session_id{};
        std::uint64_t venue_session_epoch{};
        std::uint64_t ownership_generation{};
        InlineWireIdentity wire_identity{};
        WireIdentityKey wire_identity_key{};
        Quantity quantity{};
        std::uint16_t venue_index{};
        std::uint64_t tx_credit_generation{};
        std::uint64_t throttle_generation{};
        std::uint16_t normal_tx_credit_delta{};
        std::uint16_t cancel_tx_credit_delta{};
        std::uint16_t throttle_credit_delta{};
        bool released{};
        bool recovered{};
        bool reconciled{};
        bool cancel_pending{};
        std::uint64_t pending_client_request_id{};
        std::uint64_t pending_client_sequence{};
        std::uint64_t pending_client_session_epoch{};
        std::uint64_t pending_cancel_attempt_id{};
    };

    enum class ParentSlotState : std::uint8_t {
        Empty,
        Occupied,
        Tombstone,
    };

    struct ParentReservation final {
        ParentSlotState state{ParentSlotState::Empty};
        ParentOrderId parent{};
        std::uint16_t instrument_index{};
        std::uint16_t child_count{};
        std::uint16_t released_count{};
        std::array<Reservation, kMaximumCandidatesPerInstrument> children{};
    };

    std::array<InstrumentSlot, kMaximumDataplaneInstruments> instruments{};
    std::array<VenueSlot, kMaximumDataplaneVenues> venues{};
    std::array<FixedCandidateSet, kMaximumDataplaneInstruments> candidate_sets{};
    FixedIndexMap<InstrumentId, kInstrumentMapCapacity> instrument_index{};
    FixedIndexMap<VenueId, kVenueMapCapacity> venue_index{};
    FixedIndexMap<InstrumentId, kInstrumentMapCapacity> candidate_set_index{};
    FixedIndexMap<ChildOrderId, kChildMapCapacity> child_index{};
    FixedIndexMap<WireIdentityKey, kChildMapCapacity> wire_identity_index{};
    std::array<ParentReservation, kMaximumActiveParents> parents{};
    std::array<WireIdentityPlannerBinding, kMaximumDataplaneVenues>
        wire_identity_planners{};
    std::size_t instrument_count{};
    std::size_t venue_count{};
    std::size_t candidate_set_count{};
    std::size_t active_parents{};
    std::size_t maximum_aggressive_venues{};
    std::uint64_t candidate_generation{};
    std::uint64_t configured_risk_generation{};
    std::uint64_t state_generation{1U};
    std::uint64_t next_child_order_id{1U};
    std::uint64_t next_dispatch_attempt_id{1U};
    std::size_t unreconciled_children{};
    AdaptivePolicyMode adaptive_policy_mode{AdaptivePolicyMode::Required};
    WireIdentityMode wire_identity_mode{WireIdentityMode::RequireRegistered};
    std::size_t wire_identity_planner_count{};
    bool kill_switch{};
    bool routing_enabled{true};
    bool recovery_only{};

    Impl(
        const CoreConfiguration& configuration,
        const CandidateTable& candidate_table,
        const AdaptivePolicyMode configured_adaptive_policy_mode,
        const WireIdentityMode configured_wire_identity_mode,
        const std::span<const WireIdentityPlannerBinding>
            configured_wire_identity_planners)
        : maximum_aggressive_venues(configuration.maximum_aggressive_venues),
          candidate_generation(candidate_table.generation()),
          configured_risk_generation(configuration.risk.generation),
          adaptive_policy_mode(configured_adaptive_policy_mode),
          wire_identity_mode(configured_wire_identity_mode) {
        if (maximum_aggressive_venues == 0U ||
            maximum_aggressive_venues > kMaximumCandidatesPerInstrument ||
            candidate_generation == 0U || configured_risk_generation == 0U ||
            configuration.risk.instruments.empty() ||
            configuration.risk.venues.empty() ||
            configuration.risk.instruments.size() > instruments.size() ||
            configuration.risk.venues.size() > venues.size() ||
            candidate_table.entries().size() > candidate_sets.size() ||
             (adaptive_policy_mode != AdaptivePolicyMode::Optional &&
              adaptive_policy_mode != AdaptivePolicyMode::Required) ||
             (wire_identity_mode != WireIdentityMode::ReferenceGeneric &&
              wire_identity_mode != WireIdentityMode::RequireRegistered) ||
             configured_wire_identity_planners.size() >
                 wire_identity_planners.size()) {
            throw std::invalid_argument("dataplane configuration exceeds bounds");
        }

        for (const InstrumentRiskLimit& limit : configuration.risk.instruments) {
            if (!limit.instrument || !limit.maximum_parent_quantity ||
                !limit.maximum_child_quantity ||
                limit.maximum_parent_notional.units == 0U ||
                limit.maximum_child_notional.units == 0U ||
                limit.minimum_price.units <= 0 ||
                limit.maximum_price < limit.minimum_price ||
                limit.maximum_open_parents == 0U ||
                limit.maximum_open_children == 0U ||
                !limit.maximum_gross_outstanding ||
                !instrument_index.insert(limit.instrument, instrument_count)) {
                throw std::invalid_argument("invalid or duplicate instrument risk limit");
            }
            instruments[instrument_count++].limit = limit;
        }

        for (const VenueRiskLimit& limit : configuration.risk.venues) {
            if (!limit.venue || !limit.maximum_outstanding ||
                !venue_index.insert(limit.venue, venue_count)) {
                throw std::invalid_argument("invalid or duplicate venue risk limit");
            }
            venues[venue_count++].limit = limit;
        }

        for (const CandidateSet& source_set : candidate_table.entries()) {
            if (!source_set.instrument || source_set.candidates.empty() ||
                source_set.candidates.size() > kMaximumCandidatesPerInstrument ||
                !candidate_set_index.insert(
                    source_set.instrument,
                    candidate_set_count)) {
                throw std::invalid_argument("invalid or duplicate bounded candidate set");
            }
            FixedCandidateSet& target_set = candidate_sets[candidate_set_count++];
            target_set.instrument = source_set.instrument;
            target_set.count = static_cast<std::uint16_t>(
                source_set.candidates.size());
            for (std::size_t index = 0U; index < source_set.candidates.size(); ++index) {
                const CandidateHandle& source = source_set.candidates[index];
                if (!source.venue || !source.allocation_cap ||
                    (source.admin_state != AdminState::Up &&
                     source.admin_state != AdminState::Down) ||
                    (!source.capabilities.aggressive_ioc &&
                     !source.capabilities.passive_gtc)) {
                    throw std::invalid_argument("invalid bounded candidate");
                }
                for (std::size_t prior = 0U; prior < index; ++prior) {
                    if (target_set.candidates[prior].venue == source.venue) {
                        throw std::invalid_argument("duplicate candidate venue");
                    }
                }
                target_set.candidates[index] = NumericCandidate{
                    source.venue,
                    source.price_conversion,
                    source.quantity_conversion,
                    source.capabilities,
                    source.allocation_cap,
                    source.aggressive_priority,
                    source.passive_priority,
                    source.configured_latency_ns,
                    source.admin_state,
                };
            }
        }

        for (const WireIdentityPlannerBinding& binding :
             configured_wire_identity_planners) {
            if (!binding.venue || binding.venue_session_id == 0U ||
                binding.venue_session_epoch == 0U || binding.planner == nullptr ||
                !venue_index.find(binding.venue)) {
                throw std::invalid_argument("invalid wire-identity planner binding");
            }
            for (std::size_t prior{}; prior < wire_identity_planner_count;
                 ++prior) {
                if (wire_identity_planners[prior].venue == binding.venue) {
                    throw std::invalid_argument("duplicate wire-identity planner binding");
                }
            }
            wire_identity_planners[wire_identity_planner_count++] = binding;
        }

        if (wire_identity_mode == WireIdentityMode::RequireRegistered) {
            for (std::size_t set_index{};
                 set_index < candidate_set_count;
                 ++set_index) {
                const FixedCandidateSet& candidate_set =
                    candidate_sets[set_index];
                for (std::size_t candidate_index{};
                     candidate_index < candidate_set.count;
                     ++candidate_index) {
                    const VenueId candidate_venue =
                        candidate_set.candidates[candidate_index].venue;
                    bool covered{};
                    for (std::size_t planner_index{};
                         planner_index < wire_identity_planner_count;
                         ++planner_index) {
                        if (wire_identity_planners[planner_index].venue ==
                            candidate_venue) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered) {
                        throw std::invalid_argument(
                            "missing wire-identity planner for candidate venue");
                    }
                }
            }
        }
    }

    [[nodiscard]] const FixedCandidateSet* find_candidates(
        const InstrumentId instrument) const noexcept {
        const auto index = candidate_set_index.find(instrument);
        return index ? &candidate_sets[*index] : nullptr;
    }

    [[nodiscard]] const InstrumentSlot* find_instrument(
        const InstrumentId instrument) const noexcept {
        const auto index = instrument_index.find(instrument);
        return index ? &instruments[*index] : nullptr;
    }

    [[nodiscard]] InstrumentSlot* find_instrument(
        const InstrumentId instrument) noexcept {
        const auto index = instrument_index.find(instrument);
        return index ? &instruments[*index] : nullptr;
    }

    [[nodiscard]] const VenueSlot* find_venue(
        const VenueId venue) const noexcept {
        const auto index = venue_index.find(venue);
        return index ? &venues[*index] : nullptr;
    }

    [[nodiscard]] VenueSlot* find_venue(const VenueId venue) noexcept {
        const auto index = venue_index.find(venue);
        return index ? &venues[*index] : nullptr;
    }

    [[nodiscard]] const WireIdentityPlannerBinding* find_wire_identity_planner(
        const VenueId venue) const noexcept {
        for (std::size_t index{}; index < wire_identity_planner_count; ++index) {
            if (wire_identity_planners[index].venue == venue) {
                return &wire_identity_planners[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t parent_slot(
        const ParentOrderId parent) const noexcept {
        std::size_t slot = static_cast<std::size_t>(mix_id(parent.value())) &
                           (parents.size() - 1U);
        for (std::size_t probe = 0U; probe < parents.size(); ++probe) {
            const ParentReservation& record = parents[slot];
            if (record.state == ParentSlotState::Empty) {
                return kInvalidIndex;
            }
            if (record.state == ParentSlotState::Occupied &&
                record.parent == parent) {
                return slot;
            }
            slot = (slot + 1U) & (parents.size() - 1U);
        }
        return kInvalidIndex;
    }

    [[nodiscard]] std::size_t insertion_slot(
        const ParentOrderId parent,
        bool& duplicate) const noexcept {
        duplicate = false;
        std::size_t first_tombstone = kInvalidIndex;
        std::size_t slot = static_cast<std::size_t>(mix_id(parent.value())) &
                           (parents.size() - 1U);
        for (std::size_t probe = 0U; probe < parents.size(); ++probe) {
            const ParentReservation& record = parents[slot];
            if (record.state == ParentSlotState::Occupied &&
                record.parent == parent) {
                duplicate = true;
                return slot;
            }
            if (record.state == ParentSlotState::Tombstone &&
                first_tombstone == kInvalidIndex) {
                first_tombstone = slot;
            }
            if (record.state == ParentSlotState::Empty) {
                return first_tombstone == kInvalidIndex ? slot : first_tombstone;
            }
            slot = (slot + 1U) & (parents.size() - 1U);
        }
        return first_tombstone;
    }
};

namespace {

enum class AdaptiveOverlaySetStatus : std::uint8_t {
    Legacy,
    Active,
    Invalid,
};

[[nodiscard]] bool adaptive_candidate_precedes(
    const policy::RankedCandidate& lhs,
    const policy::RankedCandidate& rhs,
    const Side side,
    const RoutingIntent intent) noexcept {
    if (intent == RoutingIntent::AggressiveIOC &&
        lhs.executable_price != rhs.executable_price) {
        return side == Side::Buy
            ? lhs.executable_price.units < rhs.executable_price.units
            : lhs.executable_price.units > rhs.executable_price.units;
    }
    if (lhs.score.total != rhs.score.total) {
        return lhs.score.total > rhs.score.total;
    }
    if (intent == RoutingIntent::AggressiveIOC &&
        lhs.available_depth != rhs.available_depth) {
        return lhs.available_depth.units > rhs.available_depth.units;
    }
    if (lhs.configured_priority != rhs.configured_priority) {
        return lhs.configured_priority < rhs.configured_priority;
    }
    return lhs.venue < rhs.venue;
}

template <typename ImplType>
[[nodiscard]] AdaptiveOverlaySetStatus validate_adaptive_overlays(
    const ImplType& impl,
    const ParentOrder& parent,
    const FixedCandidateSet& candidates,
    const FixedViews& views,
    const std::uint64_t monotonic_decision_time_ns) noexcept {
    std::size_t present_count{};
    std::size_t applied_count{};
    for (std::size_t index{}; index < candidates.count; ++index) {
        const BookSlotView* const view = views.by_candidate[index];
        if (view == nullptr) {
            continue;
        }
        ++present_count;
        applied_count += view->adaptive_policy.applied ? 1U : 0U;
    }
    if (applied_count == 0U) {
        return impl.adaptive_policy_mode == AdaptivePolicyMode::Required
            ? AdaptiveOverlaySetStatus::Invalid
            : AdaptiveOverlaySetStatus::Legacy;
    }
    if (present_count != candidates.count || applied_count != candidates.count) {
        return AdaptiveOverlaySetStatus::Invalid;
    }

    const AdaptivePolicyOverlay& reference =
        views.by_candidate[0U]->adaptive_policy;
    if (!reference.valid_shape()) {
        return AdaptiveOverlaySetStatus::Invalid;
    }
    std::array<bool, kMaximumCandidatesPerInstrument> rank_seen{};
    std::array<const policy::RankedCandidate*,
               kMaximumCandidatesPerInstrument>
        evidence_by_rank{};
    std::size_t eligible_count{};
    for (std::size_t index{}; index < candidates.count; ++index) {
        const BookSlotView& view = *views.by_candidate[index];
        const AdaptivePolicyOverlay& overlay = view.adaptive_policy;
        const NumericCandidate& configured = candidates.candidates[index];
        if (!overlay.valid_shape() || overlay.versions != reference.versions ||
            overlay.order != reference.order ||
            overlay.fact_pin.venue != view.venue ||
            overlay.order.parent_order_id != parent.id ||
            overlay.order.instrument != parent.instrument ||
            overlay.order.side != parent.side ||
            overlay.order.intent != parent.intent ||
            overlay.order.quantity != parent.quantity ||
            overlay.order.limit_price != parent.limit_price ||
            overlay.order.decision_monotonic_ns == 0U ||
            overlay.order.decision_monotonic_ns > monotonic_decision_time_ns ||
            overlay.order.decision_utc_ns <
                overlay.versions.compliance_effective_from_utc_ns ||
            (overlay.versions.compliance_effective_until_utc_ns != 0U &&
             overlay.order.decision_utc_ns >=
                 overlay.versions.compliance_effective_until_utc_ns)) {
            return AdaptiveOverlaySetStatus::Invalid;
        }
        if (!overlay.eligible) {
            continue;
        }
        const std::uint32_t expected_priority =
            parent.intent == RoutingIntent::AggressiveIOC
            ? configured.aggressive_priority
            : configured.passive_priority;
        if (!view.has_book ||
            overlay.fact_pin.versions.route_generation !=
                view.adjacency_generation ||
            overlay.fact_pin.versions.market_generation !=
                view.book.view_generation ||
            overlay.fact_pin.versions.risk_generation !=
                impl.configured_risk_generation ||
            overlay.fact_pin.versions.throttle_generation !=
                view.throttle_generation ||
            overlay.fact_pin.versions.tx_credit_generation !=
                view.tx_credit_generation ||
            overlay.fact_pin.versions.fee_generation !=
                overlay.versions.fee_generation ||
            overlay.fact_pin.versions.metric_generation !=
                overlay.versions.metric_generation ||
            overlay.evidence.configured_priority != expected_priority ||
            overlay.rank >= candidates.count || rank_seen[overlay.rank] ||
            overlay.evidence.quote_age_ns >
                overlay.evidence.maximum_quote_age_ns ||
            overlay.evidence.maximum_quote_age_ns != view.maximum_quote_age_ns ||
            overlay.evidence.normal_tx_credits != view.normal_tx_credits ||
            overlay.evidence.throttle_credits != view.throttle_credits ||
            overlay.order.decision_monotonic_ns <
                view.monotonic_receive_time_ns ||
            overlay.evidence.quote_age_ns !=
                overlay.order.decision_monotonic_ns -
                    view.monotonic_receive_time_ns) {
            return AdaptiveOverlaySetStatus::Invalid;
        }
        if (parent.intent == RoutingIntent::AggressiveIOC) {
            const bool consume_asks = parent.side == Side::Buy;
            const std::uint16_t level_count =
                consume_asks ? view.book.ask_count : view.book.bid_count;
            const md::Level& best =
                consume_asks ? view.book.asks[0U] : view.book.bids[0U];
            if (level_count == 0U ||
                overlay.evidence.executable_price != best.price ||
                overlay.evidence.available_depth != best.quantity) {
                return AdaptiveOverlaySetStatus::Invalid;
            }
        } else if (overlay.evidence.executable_price != parent.limit_price) {
            return AdaptiveOverlaySetStatus::Invalid;
        }
        rank_seen[overlay.rank] = true;
        evidence_by_rank[overlay.rank] = &overlay.evidence;
        ++eligible_count;
    }
    for (std::size_t rank{}; rank < eligible_count; ++rank) {
        if (!rank_seen[rank]) {
            return AdaptiveOverlaySetStatus::Invalid;
        }
        if (rank != 0U && adaptive_candidate_precedes(
                *evidence_by_rank[rank],
                *evidence_by_rank[rank - 1U],
                parent.side,
                parent.intent)) {
            return AdaptiveOverlaySetStatus::Invalid;
        }
    }
    return AdaptiveOverlaySetStatus::Active;
}

template <typename ImplType>
[[nodiscard]] RoutePlan base_plan(
    const ImplType& impl,
    const ParentOrder& parent,
    const FixedCandidateSet& candidates,
    const FixedViews& views,
    const std::uint64_t monotonic_decision_time_ns) noexcept {
    RoutePlan plan;
    plan.parent = parent;
    plan.kernel_state_generation = impl.state_generation;
    plan.candidate_table_generation = impl.candidate_generation;
    plan.risk_generation = impl.configured_risk_generation;
    plan.monotonic_decision_time_ns = monotonic_decision_time_ns;
    plan.next_child_order_id = impl.next_child_order_id;
    plan.next_dispatch_attempt_id = impl.next_dispatch_attempt_id;
    plan.remainder = parent.quantity;
    plan.candidate_pin_count = candidates.count;
    for (std::size_t index = 0U; index < candidates.count; ++index) {
        DataplaneCandidatePin& pin = plan.candidate_pins[index];
        pin.market.venue = candidates.candidates[index].venue;
        const BookSlotView* view = views.by_candidate[index];
        if (view != nullptr) {
            pin.market.adjacency_generation = view->adjacency_generation;
            if (view->has_book) {
                pin.market.book_version = view->book.version();
            }
            pin.feed_epoch = view->feed_epoch;
            pin.source_health_generation = view->source_health_generation;
            pin.tx_credit_generation = view->tx_credit_generation;
            pin.throttle_generation = view->throttle_generation;
            pin.normal_tx_credits = view->normal_tx_credits;
            pin.cancel_reserved_tx_credits =
                view->cancel_reserved_tx_credits;
            pin.throttle_credits = view->throttle_credits;
            pin.adaptive_policy = view->adaptive_policy;
        }
    }
    return plan;
}

template <typename ImplType>
[[nodiscard]] std::uint32_t effective_normal_tx_credits(
    const ImplType& impl,
    const BookSlotView& view) noexcept {
    const VenueSlot* venue = impl.find_venue(view.venue);
    if (venue == nullptr) {
        return 0U;
    }
    if (venue->tx_credit_generation != view.tx_credit_generation) {
        return view.normal_tx_credits;
    }
    if (venue->reported_normal_tx_credits != view.normal_tx_credits ||
        venue->reported_cancel_tx_credits !=
            view.cancel_reserved_tx_credits ||
        venue->reserved_normal_tx_credits > view.normal_tx_credits) {
        return 0U;
    }
    return view.normal_tx_credits - venue->reserved_normal_tx_credits;
}

template <typename ImplType>
[[nodiscard]] std::uint32_t effective_throttle_credits(
    const ImplType& impl,
    const BookSlotView& view) noexcept {
    const VenueSlot* venue = impl.find_venue(view.venue);
    if (venue == nullptr) {
        return 0U;
    }
    if (venue->throttle_generation != view.throttle_generation) {
        return view.throttle_credits;
    }
    if (venue->reported_throttle_credits != view.throttle_credits ||
        venue->reserved_throttle_credits > view.throttle_credits) {
        return 0U;
    }
    return view.throttle_credits - venue->reserved_throttle_credits;
}

template <typename ImplType>
[[nodiscard]] std::optional<CandidateExclusionReason> common_exclusion(
    const ImplType& impl,
    const ParentOrder& parent,
    const NumericCandidate& candidate,
    const BookSlotView* runtime,
    const bool risk_disabled,
    const std::uint64_t monotonic_decision_time_ns) noexcept {
    if (candidate.admin_state != AdminState::Up) {
        return CandidateExclusionReason::AdminDown;
    }
    if (runtime == nullptr) {
        return CandidateExclusionReason::MissingRuntimeState;
    }
    if (runtime->adaptive_policy.applied &&
        !runtime->adaptive_policy.eligible) {
        return CandidateExclusionReason::RiskProhibited;
    }
    if (!runtime->has_book) {
        return CandidateExclusionReason::MissingBook;
    }
    if (!runtime->book.valid()) {
        return CandidateExclusionReason::InvalidBook;
    }
    if (runtime->book.venue != candidate.venue ||
        runtime->book.instrument != parent.instrument) {
        return CandidateExclusionReason::BookIdentityMismatch;
    }
    if (runtime->market_data_state != MarketDataState::Live ||
        runtime->book.feed_state != MarketDataState::Live) {
        return CandidateExclusionReason::MarketDataNotLive;
    }
    if (!runtime->fresh_at(monotonic_decision_time_ns)) {
        return CandidateExclusionReason::StaleBook;
    }
    if (runtime->instrument_state != InstrumentState::Trading ||
        runtime->book.trading_state != InstrumentState::Trading) {
        return CandidateExclusionReason::InstrumentNotTrading;
    }
    if (runtime->order_entry_state != OrderEntryState::Ready) {
        return CandidateExclusionReason::OrderEntryNotReady;
    }
    if (runtime->egress_state != EgressState::Ready) {
        return CandidateExclusionReason::EgressCongested;
    }
    if (effective_normal_tx_credits(impl, *runtime) == 0U ||
        effective_throttle_credits(impl, *runtime) == 0U) {
        return CandidateExclusionReason::EgressCongested;
    }
    if (!runtime->conversion_compatible) {
        return CandidateExclusionReason::ConversionIncompatible;
    }
    if (!candidate.price_conversion.convert_exact(parent.limit_price)) {
        return CandidateExclusionReason::InexactPriceConversion;
    }
    const auto* venue = impl.find_venue(candidate.venue);
    if (!runtime->risk_allowed || risk_disabled || venue == nullptr ||
        !venue->limit.allowed) {
        return CandidateExclusionReason::RiskProhibited;
    }
    return std::nullopt;
}

struct FillSlice final {
    Price price{};
    std::uint64_t quantity{};
};

struct AggressiveCursor final {
    const NumericCandidate* candidate{};
    const BookSlotView* runtime{};
    const md::BookView* book{};
    std::size_t level_index{};
    std::uint64_t cap_remaining{};
    std::uint64_t total{};
    std::size_t selection_order{kInvalidIndex};
    std::uint16_t fill_count{};
    bool selected{};
    bool blocked_by_maximum{};
    bool exhausted_by_limit{};
    bool exhausted_by_cap{};
    bool conversion_failure{};
    std::array<FillSlice, md::kMaxDepthLevels> fills{};
};

[[nodiscard]] bool cursor_precedes(
    const Side side,
    const AggressiveCursor& lhs,
    const AggressiveCursor& rhs) noexcept {
    const md::Level& lhs_level =
        opposite_level(*lhs.book, side, lhs.level_index);
    const md::Level& rhs_level =
        opposite_level(*rhs.book, side, rhs.level_index);
    if (lhs_level.price != rhs_level.price) {
        return price_precedes(side, lhs_level.price, rhs_level.price);
    }
    if (lhs.runtime->adaptive_policy.applied &&
        rhs.runtime->adaptive_policy.applied &&
        lhs.runtime->adaptive_policy.rank !=
            rhs.runtime->adaptive_policy.rank) {
        return lhs.runtime->adaptive_policy.rank <
               rhs.runtime->adaptive_policy.rank;
    }
    if (lhs.candidate->aggressive_priority !=
        rhs.candidate->aggressive_priority) {
        return lhs.candidate->aggressive_priority <
               rhs.candidate->aggressive_priority;
    }
    return lhs.candidate->venue < rhs.candidate->venue;
}

void trim_to_exact_quantity(
    AggressiveCursor& cursor,
    bool& inexact_conversion) noexcept {
    const std::uint64_t denominator =
        cursor.candidate->quantity_conversion.denominator();
    std::uint64_t trim = cursor.total % denominator;
    if (trim == 0U) {
        return;
    }
    inexact_conversion = true;
    cursor.total -= trim;
    for (std::size_t index = cursor.fill_count; index != 0U && trim != 0U;) {
        FillSlice& slice = cursor.fills[--index];
        const std::uint64_t reduction = std::min(slice.quantity, trim);
        slice.quantity -= reduction;
        trim -= reduction;
    }
}

[[nodiscard]] const FillSlice* worst_retained_slice(
    const AggressiveCursor& cursor) noexcept {
    for (std::size_t index = cursor.fill_count; index != 0U;) {
        const FillSlice& slice = cursor.fills[--index];
        if (slice.quantity != 0U) {
            return &slice;
        }
    }
    return nullptr;
}

template <typename ImplType>
[[nodiscard]] RoutePlan aggressive_plan(
    const ImplType& impl,
    const ParentOrder& parent,
    const FixedCandidateSet& candidates,
    const FixedViews& views,
    const std::array<bool, kMaximumCandidatesPerInstrument>&
        risk_disabled,
    const std::uint64_t monotonic_decision_time_ns) noexcept {
    RoutePlan plan = base_plan(
        impl, parent, candidates, views, monotonic_decision_time_ns);
    std::array<AggressiveCursor, kMaximumCandidatesPerInstrument> cursors{};
    std::uint16_t cursor_count{};

    for (std::size_t index = 0U; index < candidates.count; ++index) {
        const NumericCandidate& candidate = candidates.candidates[index];
        if (!candidate.capabilities.aggressive_ioc) {
            exclude_once(
                plan,
                candidate.venue,
                CandidateExclusionReason::UnsupportedIntent);
            continue;
        }
        const BookSlotView* runtime = views.by_candidate[index];
        if (const auto exclusion = common_exclusion(
                impl,
                parent,
                candidate,
                runtime,
                risk_disabled[index],
                monotonic_decision_time_ns)) {
            exclude_once(plan, candidate.venue, *exclusion);
            continue;
        }
        plan.observed_truncated_depth =
            plan.observed_truncated_depth || runtime->book.depth_truncated;
        AggressiveCursor& cursor = cursors[cursor_count++];
        cursor.candidate = &candidate;
        cursor.runtime = runtime;
        cursor.book = &runtime->book;
        cursor.cap_remaining = candidate.allocation_cap.units;
    }

    std::uint64_t remaining = parent.quantity.units;
    std::size_t selected_venues{};
    bool maximum_blocked{};
    bool limit_blocked{};
    bool cap_blocked{};
    bool inexact_conversion{};

    while (remaining != 0U) {
        AggressiveCursor* best = nullptr;
        for (std::size_t index = 0U; index < cursor_count; ++index) {
            AggressiveCursor& cursor = cursors[index];
            if (cursor.cap_remaining == 0U || cursor.blocked_by_maximum ||
                cursor.conversion_failure) {
                continue;
            }
            if (cursor.level_index >=
                opposite_level_count(*cursor.book, parent.side)) {
                continue;
            }
            const md::Level& level = opposite_level(
                *cursor.book,
                parent.side,
                cursor.level_index);
            if (!price_is_executable(
                    parent.side,
                    level.price,
                    parent.limit_price)) {
                cursor.exhausted_by_limit = true;
                limit_blocked = true;
                continue;
            }
            if (!cursor.candidate->price_conversion.convert_exact(level.price)) {
                cursor.conversion_failure = true;
                inexact_conversion = true;
                exclude_once(
                    plan,
                    cursor.candidate->venue,
                    CandidateExclusionReason::InexactPriceConversion);
                continue;
            }
            if (!cursor.selected &&
                selected_venues >= impl.maximum_aggressive_venues) {
                cursor.blocked_by_maximum = true;
                maximum_blocked = true;
                exclude_once(
                    plan,
                    cursor.candidate->venue,
                    CandidateExclusionReason::MaximumVenueCount);
                continue;
            }
            if (best == nullptr ||
                cursor_precedes(parent.side, cursor, *best)) {
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
        const md::Level& level = opposite_level(
            *best->book,
            parent.side,
            best->level_index);
        const std::uint64_t take = std::min(
            {level.quantity.units, remaining, best->cap_remaining});
        best->fills[best->fill_count++] = FillSlice{level.price, take};
        best->total += take;
        best->cap_remaining -= take;
        remaining -= take;
        ++best->level_index;
        if (best->cap_remaining == 0U && remaining != 0U) {
            best->exhausted_by_cap = true;
            cap_blocked = true;
        }
    }

    for (std::size_t index = 0U; index < cursor_count; ++index) {
        AggressiveCursor& cursor = cursors[index];
        if (!cursor.selected &&
            cursor.level_index >=
                opposite_level_count(*cursor.book, parent.side)) {
            exclude_once(
                plan,
                cursor.candidate->venue,
                CandidateExclusionReason::NoExecutableDepth);
        }
        trim_to_exact_quantity(cursor, inexact_conversion);
    }

    std::uint32_t allocation_order{};
    for (std::size_t order = 0U; order < selected_venues; ++order) {
        AggressiveCursor* cursor = nullptr;
        for (std::size_t index = 0U; index < cursor_count; ++index) {
            if (cursors[index].selection_order == order) {
                cursor = &cursors[index];
                break;
            }
        }
        if (cursor == nullptr) {
            continue;
        }
        if (cursor->total == 0U) {
            exclude_once(
                plan,
                cursor->candidate->venue,
                CandidateExclusionReason::InexactQuantityConversion);
            continue;
        }
        const FillSlice* worst = worst_retained_slice(*cursor);
        const Quantity canonical_quantity{cursor->total};
        const auto venue_quantity =
            cursor->candidate->quantity_conversion.convert_exact(
                canonical_quantity);
        const auto venue_price = worst == nullptr ? std::optional<Price>{} :
            cursor->candidate->price_conversion.convert_exact(worst->price);
        if (!venue_quantity || !venue_price) {
            inexact_conversion = true;
            exclude_once(
                plan,
                cursor->candidate->venue,
                !venue_quantity ?
                    CandidateExclusionReason::InexactQuantityConversion :
                    CandidateExclusionReason::InexactPriceConversion);
            continue;
        }
        plan.allocations[plan.allocation_count++] = ChildAllocation{
            cursor->candidate->venue,
            canonical_quantity,
            worst->price,
            *venue_quantity,
            *venue_price,
            allocation_order++,
            cursor->runtime->adjacency_generation,
            cursor->book->version(),
        };
        plan.allocated.units += canonical_quantity.units;
    }

    plan.remainder = Quantity{parent.quantity.units - plan.allocated.units};
    if (plan.allocation_count == 0U) {
        plan.reject_reason = cursor_count == 0U ?
            RouteRejectReason::NoEligibleCandidate :
            RouteRejectReason::NoExecutableLiquidity;
    }
    if (!plan.remainder) {
        plan.remainder_reason = RemainderReason::None;
    } else if (inexact_conversion) {
        plan.remainder_reason = RemainderReason::InexactConversion;
    } else if (maximum_blocked) {
        plan.remainder_reason = RemainderReason::MaximumVenueCount;
    } else if (cap_blocked) {
        plan.remainder_reason = RemainderReason::VenueAllocationCaps;
    } else if (limit_blocked) {
        plan.remainder_reason = RemainderReason::ParentLimitReached;
    } else if (plan.observed_truncated_depth) {
        plan.remainder_reason = RemainderReason::TruncatedDepthUnknown;
    } else if (cursor_count == 0U) {
        plan.remainder_reason = RemainderReason::NoEligibleCandidate;
    } else {
        plan.remainder_reason = RemainderReason::InsufficientDisplayedDepth;
    }
    return plan;
}

template <typename ImplType>
[[nodiscard]] RoutePlan passive_plan(
    const ImplType& impl,
    const ParentOrder& parent,
    const FixedCandidateSet& candidates,
    const FixedViews& views,
    const std::array<bool, kMaximumCandidatesPerInstrument>&
        risk_disabled,
    const std::uint64_t monotonic_decision_time_ns) noexcept {
    RoutePlan plan = base_plan(
        impl, parent, candidates, views, monotonic_decision_time_ns);
    std::array<std::size_t, kMaximumCandidatesPerInstrument> common_eligible{};
    std::uint16_t common_count{};
    bool marketable{};

    for (std::size_t index = 0U; index < candidates.count; ++index) {
        const NumericCandidate& candidate = candidates.candidates[index];
        const BookSlotView* runtime = views.by_candidate[index];
        if (const auto exclusion = common_exclusion(
                impl,
                parent,
                candidate,
                runtime,
                risk_disabled[index],
                monotonic_decision_time_ns)) {
            exclude_once(plan, candidate.venue, *exclusion);
            continue;
        }
        plan.observed_truncated_depth =
            plan.observed_truncated_depth || runtime->book.depth_truncated;
        common_eligible[common_count++] = index;
        const md::BookView& book = runtime->book;
        if (parent.side == Side::Buy && book.ask_count != 0U &&
            book.asks[0].price <= parent.limit_price) {
            marketable = true;
        }
        if (parent.side == Side::Sell && book.bid_count != 0U &&
            book.bids[0].price >= parent.limit_price) {
            marketable = true;
        }
    }

    if (marketable) {
        plan.reject_reason = RouteRejectReason::PassiveMarketable;
        plan.remainder_reason = RemainderReason::PassiveFullQuantityUnavailable;
        return plan;
    }

    std::size_t selected = kInvalidIndex;
    for (std::size_t eligible_index = 0U;
         eligible_index < common_count;
         ++eligible_index) {
        const std::size_t index = common_eligible[eligible_index];
        const NumericCandidate& candidate = candidates.candidates[index];
        if (!candidate.capabilities.passive_gtc) {
            exclude_once(
                plan,
                candidate.venue,
                CandidateExclusionReason::UnsupportedIntent);
            continue;
        }
        if (candidate.allocation_cap < parent.quantity) {
            exclude_once(
                plan,
                candidate.venue,
                CandidateExclusionReason::AllocationCapTooSmall);
            continue;
        }
        if (!candidate.quantity_conversion.convert_exact(parent.quantity)) {
            exclude_once(
                plan,
                candidate.venue,
                CandidateExclusionReason::InexactQuantityConversion);
            continue;
        }
        if (selected == kInvalidIndex) {
            selected = index;
            continue;
        }
        const NumericCandidate& incumbent = candidates.candidates[selected];
        const BookSlotView& candidate_view = *views.by_candidate[index];
        const BookSlotView& incumbent_view = *views.by_candidate[selected];
        if (candidate_view.adaptive_policy.applied &&
            incumbent_view.adaptive_policy.applied &&
            candidate_view.adaptive_policy.rank !=
                incumbent_view.adaptive_policy.rank) {
            if (candidate_view.adaptive_policy.rank <
                incumbent_view.adaptive_policy.rank) {
                selected = index;
            }
        } else if (std::tie(
                candidate.passive_priority,
                candidate.configured_latency_ns,
                candidate.venue) <
            std::tie(
                incumbent.passive_priority,
                incumbent.configured_latency_ns,
                incumbent.venue)) {
            selected = index;
        }
    }

    if (selected == kInvalidIndex) {
        plan.reject_reason = common_count == 0U ?
            RouteRejectReason::NoEligibleCandidate :
            RouteRejectReason::PassiveFullQuantityUnavailable;
        plan.remainder_reason = common_count == 0U ?
            RemainderReason::NoEligibleCandidate :
            RemainderReason::PassiveFullQuantityUnavailable;
        return plan;
    }

    const NumericCandidate& candidate = candidates.candidates[selected];
    const BookSlotView& runtime = *views.by_candidate[selected];
    const auto venue_quantity =
        candidate.quantity_conversion.convert_exact(parent.quantity);
    const auto venue_price =
        candidate.price_conversion.convert_exact(parent.limit_price);
    if (!venue_quantity || !venue_price) {
        plan.reject_reason = RouteRejectReason::PassiveFullQuantityUnavailable;
        plan.remainder_reason = RemainderReason::InexactConversion;
        return plan;
    }
    plan.allocations[0] = ChildAllocation{
        candidate.venue,
        parent.quantity,
        parent.limit_price,
        *venue_quantity,
        *venue_price,
        0U,
        runtime.adjacency_generation,
        runtime.book.version(),
    };
    plan.allocation_count = 1U;
    plan.allocated = parent.quantity;
    plan.remainder = Quantity{};
    plan.remainder_reason = RemainderReason::None;
    return plan;
}

template <typename ImplType>
[[nodiscard]] bool proposal_exceeds_risk(
    const ImplType& impl,
    const ParentOrder& parent,
    const RoutePlan& plan,
    VenueId& prohibited_venue) noexcept {
    const InstrumentSlot* instrument = impl.find_instrument(parent.instrument);
    if (instrument == nullptr) {
        prohibited_venue = plan.allocation_count == 0U ? VenueId{} :
            plan.allocations[0].venue;
        return true;
    }
    if (plan.allocation_count >
        instrument->limit.maximum_open_children -
            std::min(
                instrument->limit.maximum_open_children,
                instrument->open_children)) {
        prohibited_venue = plan.allocation_count == 0U ? VenueId{} :
            plan.allocations[plan.allocation_count - 1U].venue;
        return true;
    }

    Quantity instrument_total = instrument->outstanding;
    for (std::size_t index = 0U; index < plan.allocation_count; ++index) {
        const ChildAllocation& allocation = plan.allocations[index];
        if (allocation.quantity > instrument->limit.maximum_child_quantity) {
            prohibited_venue = allocation.venue;
            return true;
        }
        const auto notional = checked_notional(
            allocation.limit_price,
            allocation.quantity);
        if (!notional || *notional > instrument->limit.maximum_child_notional) {
            prohibited_venue = allocation.venue;
            return true;
        }
        const auto next_instrument = checked_add(
            instrument_total,
            allocation.quantity);
        if (!next_instrument ||
            *next_instrument > instrument->limit.maximum_gross_outstanding) {
            prohibited_venue = allocation.venue;
            return true;
        }
        instrument_total = *next_instrument;

        const VenueSlot* venue = impl.find_venue(allocation.venue);
        if (venue == nullptr || !venue->limit.allowed) {
            prohibited_venue = allocation.venue;
            return true;
        }
        const auto next_venue = checked_add(
            venue->outstanding,
            allocation.quantity);
        if (!next_venue || *next_venue > venue->limit.maximum_outstanding) {
            prohibited_venue = allocation.venue;
            return true;
        }
    }
    return false;
}

template <typename ImplType>
[[nodiscard]] CommandError validate_parent(
    const ImplType& impl,
    const ParentOrder& parent) noexcept {
    if (!parent.id || !parent.instrument || !parent.quantity ||
        parent.limit_price.units <= 0 || !valid_side(parent.side) ||
        !valid_intent(parent.intent)) {
        return CommandError::InvalidCommand;
    }
    if (!impl.routing_enabled) {
        return CommandError::RoutingDisabled;
    }
    if (impl.kill_switch) {
        return CommandError::KillSwitchActive;
    }
    if (impl.parent_slot(parent.id) != kInvalidIndex) {
        return CommandError::ConflictingParentId;
    }
    const InstrumentSlot* instrument = impl.find_instrument(parent.instrument);
    if (instrument == nullptr || !instrument->limit.allowed) {
        return CommandError::InstrumentProhibited;
    }
    if (parent.quantity > instrument->limit.maximum_parent_quantity) {
        return CommandError::ParentQuantityLimit;
    }
    const auto notional = checked_notional(parent.limit_price, parent.quantity);
    if (!notional || *notional > instrument->limit.maximum_parent_notional) {
        return CommandError::ParentNotionalLimit;
    }
    if (parent.limit_price < instrument->limit.minimum_price ||
        parent.limit_price > instrument->limit.maximum_price) {
        return CommandError::PriceCollar;
    }
    if (instrument->open_parents >=
        instrument->limit.maximum_open_parents) {
        return CommandError::OpenParentLimit;
    }
    return CommandError::None;
}

[[nodiscard]] DataplaneCandidatePin current_pin(
    const NumericCandidate& candidate,
    const BookSlotView* view) noexcept {
    DataplaneCandidatePin pin;
    pin.market.venue = candidate.venue;
    if (view != nullptr) {
        pin.market.adjacency_generation = view->adjacency_generation;
        if (view->has_book) {
            pin.market.book_version = view->book.version();
        }
        pin.feed_epoch = view->feed_epoch;
        pin.source_health_generation = view->source_health_generation;
        pin.tx_credit_generation = view->tx_credit_generation;
        pin.throttle_generation = view->throttle_generation;
        pin.normal_tx_credits = view->normal_tx_credits;
        pin.cancel_reserved_tx_credits = view->cancel_reserved_tx_credits;
        pin.throttle_credits = view->throttle_credits;
        pin.adaptive_policy = view->adaptive_policy;
    }
    return pin;
}

void put_big_endian(
    std::array<std::byte, kInlineWireIdentityBytes>& output,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        output[offset + index] = static_cast<std::byte>(value >> shift);
    }
}

[[nodiscard]] InlineWireIdentity make_wire_identity(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t attempt) noexcept {
    InlineWireIdentity output;
    output.size = static_cast<std::uint8_t>(output.bytes.size());
    output.bytes[0] = std::byte{'S'};
    output.bytes[1] = std::byte{'R'};
    output.bytes[2] = std::byte{'X'};
    output.bytes[3] = std::byte{1U};
    put_big_endian(output.bytes, 4U, parent.value());
    put_big_endian(output.bytes, 12U, child.value());
    put_big_endian(output.bytes, 20U, attempt);
    const std::uint64_t checksum = mix_id(
        parent.value() ^ child.value() ^ attempt);
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((3U - index) * 8U);
        output.bytes[28U + index] = static_cast<std::byte>(checksum >> shift);
    }
    return output;
}

template <typename ImplType>
[[nodiscard]] WireIdentityPlanStatus assign_planned_children(
    const ImplType& impl,
    const FixedCandidateSet& candidates,
    const FixedViews& views,
    RoutePlan& plan) noexcept {
    const std::uint64_t count = plan.allocation_count;
    if (count == 0U || impl.next_child_order_id == 0U ||
        impl.next_dispatch_attempt_id == 0U ||
        impl.state_generation == std::numeric_limits<std::uint64_t>::max() ||
        count >
            std::numeric_limits<std::uint64_t>::max() -
                impl.next_child_order_id ||
        count >
            std::numeric_limits<std::uint64_t>::max() -
                impl.next_dispatch_attempt_id) {
        return WireIdentityPlanStatus::IdentifierExhausted;
    }
    plan.next_child_order_id = impl.next_child_order_id;
    plan.next_dispatch_attempt_id = impl.next_dispatch_attempt_id;
    for (std::size_t index = 0U; index < plan.allocation_count; ++index) {
        const ChildAllocation& allocation = plan.allocations[index];
        const BookSlotView* view = nullptr;
        for (std::size_t candidate_index = 0U;
             candidate_index < candidates.count;
             ++candidate_index) {
            if (candidates.candidates[candidate_index].venue ==
                allocation.venue) {
                view = views.by_candidate[candidate_index];
                break;
            }
        }
        if (view == nullptr) {
            return WireIdentityPlanStatus::InvalidInput;
        }
        const ChildOrderId child{impl.next_child_order_id + index};
        const std::uint64_t attempt =
            impl.next_dispatch_attempt_id + index;
        PlannedChild& planned = plan.planned_children[index];
        planned.child_order_id = child;
        planned.dispatch_attempt_id = attempt;
        planned.ownership_generation = impl.state_generation + 1U;
        planned.tx_credit_generation = view->tx_credit_generation;
        planned.throttle_generation = view->throttle_generation;
        planned.normal_tx_credit_delta = 1U;
        planned.cancel_tx_credit_delta = 0U;
        planned.throttle_credit_delta = 1U;
        if (impl.wire_identity_mode == WireIdentityMode::ReferenceGeneric) {
            planned.venue_session_id = 1U;
            planned.venue_session_epoch = 1U;
            planned.wire_identity =
                make_wire_identity(plan.parent.id, child, attempt);
        } else {
            const WireIdentityPlannerBinding* const binding =
                impl.find_wire_identity_planner(allocation.venue);
            if (binding == nullptr) {
                return WireIdentityPlanStatus::MissingPlanner;
            }
            planned.venue_session_id = binding->venue_session_id;
            planned.venue_session_epoch = binding->venue_session_epoch;
            const WireIdentityPlanRequest request{
                allocation.venue,
                child,
                attempt,
                binding->venue_session_id,
                binding->venue_session_epoch,
            };
            const WireIdentityPlanStatus planned_status =
                binding->planner(request, planned.wire_identity);
            if (planned_status != WireIdentityPlanStatus::Planned) {
                return planned_status;
            }
        }
        if (planned.wire_identity.size == 0U ||
            planned.wire_identity.size > planned.wire_identity.bytes.size()) {
            return WireIdentityPlanStatus::InvalidOutput;
        }
        const WireIdentityKey identity_key = make_wire_identity_key(
            allocation.venue,
            planned.venue_session_id,
            planned.venue_session_epoch,
            planned.wire_identity);
        if (impl.wire_identity_index.find(identity_key)) {
            return WireIdentityPlanStatus::InvalidOutput;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            const PlannedChild& prior_child = plan.planned_children[prior];
            if (make_wire_identity_key(
                    plan.allocations[prior].venue,
                    prior_child.venue_session_id,
                    prior_child.venue_session_epoch,
                    prior_child.wire_identity) == identity_key) {
                return WireIdentityPlanStatus::InvalidOutput;
            }
        }
    }
    return WireIdentityPlanStatus::Planned;
}

}  // namespace

RouteKernel::RouteKernel(
    const CoreConfiguration& configuration,
    const CandidateTable& candidates,
    const AdaptivePolicyMode adaptive_policy_mode,
    const WireIdentityMode wire_identity_mode,
    const std::span<const WireIdentityPlannerBinding> wire_identity_planners)
    : impl_(std::make_unique<Impl>(
          configuration,
          candidates,
          adaptive_policy_mode,
          wire_identity_mode,
          wire_identity_planners)) {}

RouteKernel::~RouteKernel() = default;
RouteKernel::RouteKernel(RouteKernel&&) noexcept = default;
RouteKernel& RouteKernel::operator=(RouteKernel&&) noexcept = default;

PlanResult RouteKernel::plan(
    const ParentOrder& parent,
    const std::span<const BookSlotView> views,
    const std::uint64_t monotonic_decision_time_ns) const noexcept {
    PlanResult result;
    if (!impl_) {
        result.status = PlanStatus::InvalidInput;
        return result;
    }
    if (impl_->recovery_only) {
        result.status = PlanStatus::RecoveryOnly;
        return result;
    }
    result.plan.parent = parent;
    result.plan.kernel_state_generation = impl_->state_generation;
    result.plan.candidate_table_generation = impl_->candidate_generation;
    result.plan.risk_generation = impl_->configured_risk_generation;
    result.plan.monotonic_decision_time_ns = monotonic_decision_time_ns;
    result.plan.next_child_order_id = impl_->next_child_order_id;
    result.plan.next_dispatch_attempt_id =
        impl_->next_dispatch_attempt_id;
    result.plan.remainder = parent.quantity;

    if (monotonic_decision_time_ns == 0U) {
        result.status = PlanStatus::InvalidInput;
        return result;
    }

    result.hard_error = validate_parent(*impl_, parent);
    if (result.hard_error != CommandError::None) {
        result.status = PlanStatus::HardRejected;
        return result;
    }
    if (impl_->active_parents >= impl_->parents.size()) {
        result.status = PlanStatus::CapacityExceeded;
        result.hard_error = CommandError::IngressSaturated;
        return result;
    }

    const FixedCandidateSet* candidates =
        impl_->find_candidates(parent.instrument);
    if (candidates == nullptr) {
        result.plan.reject_reason = RouteRejectReason::InstrumentNotConfigured;
        result.plan.remainder_reason = RemainderReason::NoEligibleCandidate;
        result.status = PlanStatus::RouteRejected;
        return result;
    }
    FixedViews fixed_views;
    if (!map_views(*candidates, parent.instrument, views, fixed_views)) {
        result.status = PlanStatus::InvalidInput;
        return result;
    }
    const AdaptiveOverlaySetStatus adaptive_status =
        validate_adaptive_overlays(
            *impl_,
            parent,
            *candidates,
            fixed_views,
            monotonic_decision_time_ns);
    if (adaptive_status == AdaptiveOverlaySetStatus::Invalid) {
        result.plan.reject_reason = RouteRejectReason::NoEligibleCandidate;
        result.plan.remainder_reason = RemainderReason::NoEligibleCandidate;
        result.status = PlanStatus::AdaptivePolicyRejected;
        return result;
    }

    std::array<bool, kMaximumCandidatesPerInstrument> risk_disabled{};
    for (std::size_t attempt = 0U; attempt <= candidates->count; ++attempt) {
        result.plan = parent.intent == RoutingIntent::AggressiveIOC ?
            aggressive_plan(
                *impl_,
                parent,
                *candidates,
                fixed_views,
                risk_disabled,
                monotonic_decision_time_ns) :
            passive_plan(
                *impl_,
                parent,
                *candidates,
                fixed_views,
                risk_disabled,
                monotonic_decision_time_ns);
        if (!result.plan.accepted()) {
            result.status = PlanStatus::RouteRejected;
            return result;
        }
        VenueId prohibited_venue;
        if (!proposal_exceeds_risk(
                *impl_, parent, result.plan, prohibited_venue)) {
            result.wire_identity_status = assign_planned_children(
                *impl_, *candidates, fixed_views, result.plan);
            if (result.wire_identity_status ==
                WireIdentityPlanStatus::IdentifierExhausted) {
                result.status = PlanStatus::IdentifierExhausted;
                return result;
            }
            if (result.wire_identity_status !=
                WireIdentityPlanStatus::Planned) {
                result.status = PlanStatus::WireIdentityRejected;
                return result;
            }
            result.status = PlanStatus::Planned;
            return result;
        }

        bool disabled{};
        if (prohibited_venue) {
            for (std::size_t index = 0U; index < candidates->count; ++index) {
                if (candidates->candidates[index].venue == prohibited_venue &&
                    !risk_disabled[index]) {
                    risk_disabled[index] = true;
                    disabled = true;
                    break;
                }
            }
        }
        if (!disabled) {
            for (std::size_t index = 0U; index < candidates->count; ++index) {
                risk_disabled[index] = true;
            }
        }
    }
    result.status = PlanStatus::RouteRejected;
    return result;
}

CommitResult RouteKernel::commit(
    const RoutePlan& proposed,
    const std::span<const BookSlotView> current_views,
    const std::uint64_t monotonic_commit_time_ns) noexcept {
    CommitResult result;
    result.kernel_state_generation = impl_ ? impl_->state_generation : 0U;
    if (!impl_ || !proposed.accepted()) {
        result.status = CommitStatus::PlanNotAccepted;
        return result;
    }
    if (impl_->recovery_only) {
        result.status = CommitStatus::RecoveryOnly;
        return result;
    }
    if (proposed.monotonic_decision_time_ns == 0U ||
        monotonic_commit_time_ns < proposed.monotonic_decision_time_ns ||
        proposed.next_child_order_id != impl_->next_child_order_id ||
        proposed.next_dispatch_attempt_id !=
            impl_->next_dispatch_attempt_id) {
        result.status = CommitStatus::StalePlan;
        return result;
    }
    if (proposed.candidate_table_generation != impl_->candidate_generation) {
        result.status = CommitStatus::CandidateGenerationMismatch;
        return result;
    }
    if (proposed.risk_generation != impl_->configured_risk_generation) {
        result.status = CommitStatus::RiskGenerationMismatch;
        return result;
    }
    if (proposed.kernel_state_generation != impl_->state_generation) {
        result.status = CommitStatus::StalePlan;
        return result;
    }
    if (impl_->parent_slot(proposed.parent.id) != kInvalidIndex) {
        result.status = CommitStatus::DuplicateParent;
        return result;
    }
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        result.status = CommitStatus::GenerationExhausted;
        return result;
    }

    const FixedCandidateSet* candidates =
        impl_->find_candidates(proposed.parent.instrument);
    if (candidates == nullptr ||
        proposed.candidate_pin_count != candidates->count ||
        proposed.allocation_count == 0U ||
        proposed.allocation_count > candidates->count ||
        proposed.exclusion_count > candidates->count) {
        result.status = CommitStatus::InvalidPlan;
        return result;
    }
    FixedViews fixed_views;
    if (!map_views(
            *candidates,
            proposed.parent.instrument,
            current_views,
            fixed_views)) {
        result.status = CommitStatus::StaleMarketView;
        return result;
    }
    for (std::size_t index = 0U; index < candidates->count; ++index) {
        if (proposed.candidate_pins[index].market.venue !=
            candidates->candidates[index].venue) {
            result.status = CommitStatus::InvalidPlan;
            return result;
        }
        const DataplaneCandidatePin observed = current_pin(
            candidates->candidates[index], fixed_views.by_candidate[index]);
        if (proposed.candidate_pins[index].adaptive_policy !=
            observed.adaptive_policy) {
            result.status = CommitStatus::AdaptivePolicyMismatch;
            return result;
        }
        if (!same_pin(proposed.candidate_pins[index], observed)) {
            result.status = CommitStatus::StaleMarketView;
            return result;
        }
    }

    const PlanResult expected = plan(
        proposed.parent,
        current_views,
        monotonic_commit_time_ns);
    if (!expected.accepted()) {
        if (expected.status == PlanStatus::WireIdentityRejected) {
            result.status = CommitStatus::WireIdentityMismatch;
        } else if (expected.hard_error == CommandError::ConflictingParentId) {
            result.status = CommitStatus::DuplicateParent;
        } else if (expected.hard_error != CommandError::None) {
            result.status = CommitStatus::RiskViolation;
        } else {
            result.status = CommitStatus::StaleMarketView;
        }
        return result;
    }
    if (!equivalent_plan(proposed, expected.plan)) {
        result.status = equivalent_wire_ownership(proposed, expected.plan)
            ? CommitStatus::InvalidPlan
            : CommitStatus::WireIdentityMismatch;
        return result;
    }

    VenueId prohibited;
    if (proposal_exceeds_risk(
            *impl_, proposed.parent, proposed, prohibited)) {
        result.status = CommitStatus::RiskViolation;
        return result;
    }
    for (std::size_t index = 0U; index < proposed.allocation_count; ++index) {
        const ChildAllocation& allocation = proposed.allocations[index];
        const PlannedChild& child = proposed.planned_children[index];
        const BookSlotView* view = nullptr;
        for (std::size_t candidate_index = 0U;
             candidate_index < candidates->count;
             ++candidate_index) {
            if (candidates->candidates[candidate_index].venue ==
                allocation.venue) {
                view = fixed_views.by_candidate[candidate_index];
                break;
            }
        }
        if (view == nullptr || child.cancel_tx_credit_delta != 0U ||
            child.normal_tx_credit_delta != 1U ||
            child.throttle_credit_delta != 1U ||
            child.tx_credit_generation != view->tx_credit_generation ||
            child.throttle_generation != view->throttle_generation ||
            effective_normal_tx_credits(*impl_, *view) <
                child.normal_tx_credit_delta ||
            effective_throttle_credits(*impl_, *view) <
                child.throttle_credit_delta) {
            result.status = CommitStatus::CapacityViolation;
            return result;
        }
    }
    bool duplicate{};
    const std::size_t parent_slot =
        impl_->insertion_slot(proposed.parent.id, duplicate);
    if (duplicate) {
        result.status = CommitStatus::DuplicateParent;
        return result;
    }
    if (parent_slot == kInvalidIndex ||
        impl_->active_parents >= impl_->parents.size()) {
        result.status = CommitStatus::ReservationCapacityExceeded;
        return result;
    }
    const auto instrument_index =
        impl_->instrument_index.find(proposed.parent.instrument);
    if (!instrument_index ||
        *instrument_index > std::numeric_limits<std::uint16_t>::max()) {
        result.status = CommitStatus::RiskViolation;
        return result;
    }

    Impl::ParentReservation record;
    record.state = Impl::ParentSlotState::Occupied;
    record.parent = proposed.parent.id;
    record.instrument_index = static_cast<std::uint16_t>(*instrument_index);
    record.child_count = proposed.allocation_count;
    for (std::size_t index = 0U; index < proposed.allocation_count; ++index) {
        const ChildAllocation& allocation = proposed.allocations[index];
        const auto venue_index = impl_->venue_index.find(allocation.venue);
        if (!venue_index ||
            *venue_index > std::numeric_limits<std::uint16_t>::max()) {
            result.status = CommitStatus::RiskViolation;
            return result;
        }
        Impl::Reservation& child = record.children[index];
        child.child = proposed.planned_children[index].child_order_id;
        child.dispatch_attempt_id =
            proposed.planned_children[index].dispatch_attempt_id;
        child.last_dispatch_attempt_id = child.dispatch_attempt_id;
        child.venue = allocation.venue;
        child.venue_session_id =
            proposed.planned_children[index].venue_session_id;
        child.venue_session_epoch =
            proposed.planned_children[index].venue_session_epoch;
        child.ownership_generation =
            proposed.planned_children[index].ownership_generation;
        child.wire_identity = proposed.planned_children[index].wire_identity;
        child.wire_identity_key = make_wire_identity_key(
            allocation.venue,
            child.venue_session_id,
            child.venue_session_epoch,
            child.wire_identity);
        child.quantity = allocation.quantity;
        child.venue_index = static_cast<std::uint16_t>(*venue_index);
        child.tx_credit_generation =
            proposed.planned_children[index].tx_credit_generation;
        child.throttle_generation =
            proposed.planned_children[index].throttle_generation;
        child.normal_tx_credit_delta =
            proposed.planned_children[index].normal_tx_credit_delta;
        child.cancel_tx_credit_delta =
            proposed.planned_children[index].cancel_tx_credit_delta;
        child.throttle_credit_delta =
            proposed.planned_children[index].throttle_credit_delta;
    }

    for (std::size_t index{}; index < record.child_count; ++index) {
        if (impl_->child_index.find(record.children[index].child) ||
            impl_->wire_identity_index.find(
                record.children[index].wire_identity_key)) {
            result.status = CommitStatus::ReservationCapacityExceeded;
            return result;
        }
    }
    std::size_t inserted_children{};
    for (std::size_t index{}; index < record.child_count; ++index) {
        const std::size_t location =
            parent_slot * kMaximumCandidatesPerInstrument + index;
        if (!impl_->child_index.insert(record.children[index].child, location) ||
            !impl_->wire_identity_index.insert(
                record.children[index].wire_identity_key, location)) {
            (void)impl_->child_index.erase(record.children[index].child);
            for (std::size_t rollback{}; rollback < inserted_children;
                 ++rollback) {
                (void)impl_->child_index.erase(record.children[rollback].child);
                (void)impl_->wire_identity_index.erase(
                    record.children[rollback].wire_identity_key);
            }
            result.status = CommitStatus::ReservationCapacityExceeded;
            return result;
        }
        ++inserted_children;
    }

    InstrumentSlot& instrument = impl_->instruments[*instrument_index];
    Quantity next_instrument = instrument.outstanding;
    for (std::size_t index = 0U; index < proposed.allocation_count; ++index) {
        next_instrument = *checked_add(
            next_instrument,
            proposed.allocations[index].quantity);
    }
    instrument.outstanding = next_instrument;
    ++instrument.open_parents;
    instrument.open_children += proposed.allocation_count;
    for (std::size_t index = 0U; index < proposed.allocation_count; ++index) {
        VenueSlot& venue = impl_->venues[record.children[index].venue_index];
        venue.outstanding = *checked_add(
            venue.outstanding,
            record.children[index].quantity);

        const BookSlotView* view = nullptr;
        for (std::size_t candidate_index = 0U;
             candidate_index < candidates->count;
             ++candidate_index) {
            if (candidates->candidates[candidate_index].venue ==
                record.children[index].venue) {
                view = fixed_views.by_candidate[candidate_index];
                break;
            }
        }
        if (venue.tx_credit_generation != view->tx_credit_generation) {
            venue.tx_credit_generation = view->tx_credit_generation;
            venue.reported_normal_tx_credits = view->normal_tx_credits;
            venue.reported_cancel_tx_credits =
                view->cancel_reserved_tx_credits;
            venue.reserved_normal_tx_credits = 0U;
            venue.reserved_cancel_tx_credits = 0U;
        }
        if (venue.throttle_generation != view->throttle_generation) {
            venue.throttle_generation = view->throttle_generation;
            venue.reported_throttle_credits = view->throttle_credits;
            venue.reserved_throttle_credits = 0U;
        }
        venue.reserved_normal_tx_credits +=
            proposed.planned_children[index].normal_tx_credit_delta;
        venue.reserved_cancel_tx_credits +=
            proposed.planned_children[index].cancel_tx_credit_delta;
        venue.reserved_throttle_credits +=
            proposed.planned_children[index].throttle_credit_delta;
    }
    impl_->parents[parent_slot] = record;
    ++impl_->active_parents;
    impl_->next_child_order_id += proposed.allocation_count;
    impl_->next_dispatch_attempt_id += proposed.allocation_count;
    ++impl_->state_generation;
    result.status = CommitStatus::Committed;
    result.kernel_state_generation = impl_->state_generation;
    return result;
}

ReleaseStatus RouteKernel::release_child(
    const ParentOrderId parent,
    const VenueId venue) noexcept {
    if (!impl_) {
        return ReleaseStatus::ParentNotFound;
    }
    if (impl_->recovery_only) {
        return ReleaseStatus::InvariantViolation;
    }
    const std::size_t slot = impl_->parent_slot(parent);
    if (slot == kInvalidIndex) {
        return ReleaseStatus::ParentNotFound;
    }
    Impl::ParentReservation& record = impl_->parents[slot];
    std::size_t child_index = kInvalidIndex;
    for (std::size_t index = 0U; index < record.child_count; ++index) {
        if (record.children[index].venue == venue &&
            !record.children[index].released) {
            child_index = index;
            break;
        }
    }
    if (child_index == kInvalidIndex) {
        return ReleaseStatus::VenueNotReserved;
    }
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return ReleaseStatus::GenerationExhausted;
    }
    InstrumentSlot& instrument =
        impl_->instruments[record.instrument_index];
    Impl::Reservation& reservation = record.children[child_index];
    const auto child_location = impl_->child_index.find(reservation.child);
    const auto wire_location =
        impl_->wire_identity_index.find(reservation.wire_identity_key);
    if (!child_location || !wire_location ||
        *child_location !=
            slot * kMaximumCandidatesPerInstrument + child_index ||
        *wire_location != *child_location) {
        return ReleaseStatus::InvariantViolation;
    }
    VenueSlot& venue_slot = impl_->venues[reservation.venue_index];
    if (instrument.outstanding < reservation.quantity ||
        venue_slot.outstanding < reservation.quantity ||
        instrument.open_children == 0U || impl_->active_parents == 0U ||
        record.released_count >= record.child_count ||
        (record.released_count + 1U == record.child_count &&
         instrument.open_parents == 0U)) {
        return ReleaseStatus::InvariantViolation;
    }
    instrument.outstanding = *checked_subtract(
        instrument.outstanding,
        reservation.quantity);
    venue_slot.outstanding = *checked_subtract(
        venue_slot.outstanding,
        reservation.quantity);
    --instrument.open_children;
    if (!impl_->child_index.erase(reservation.child)) {
        return ReleaseStatus::InvariantViolation;
    }
    if (!impl_->wire_identity_index.erase(reservation.wire_identity_key)) {
        return ReleaseStatus::InvariantViolation;
    }
    record.children[child_index].released = true;
    ++record.released_count;
    if (record.released_count == record.child_count) {
        --instrument.open_parents;
        --impl_->active_parents;
        record.state = Impl::ParentSlotState::Tombstone;
    }
    ++impl_->state_generation;
    return ReleaseStatus::Released;
}

ActiveChildLookupResult RouteKernel::lookup_active_child(
    const ChildOrderId child) const noexcept {
    ActiveChildLookupResult result;
    if (!impl_) {
        result.status = ActiveChildLookupStatus::Unavailable;
        return result;
    }
    const auto location = impl_->child_index.find(child);
    if (!location) {
        return result;
    }
    const std::size_t parent_slot =
        *location / kMaximumCandidatesPerInstrument;
    const std::size_t child_slot =
        *location % kMaximumCandidatesPerInstrument;
    if (parent_slot >= impl_->parents.size()) {
        result.status = ActiveChildLookupStatus::Unavailable;
        return result;
    }
    const Impl::ParentReservation& parent = impl_->parents[parent_slot];
    if (parent.state != Impl::ParentSlotState::Occupied ||
        child_slot >= parent.child_count) {
        result.status = ActiveChildLookupStatus::Unavailable;
        return result;
    }
    const Impl::Reservation& reservation = parent.children[child_slot];
    if (reservation.child != child || reservation.released) {
        result.status = ActiveChildLookupStatus::Unavailable;
        return result;
    }
    result.snapshot.parent_order_id = parent.parent;
    result.snapshot.child_id = reservation.child;
    result.snapshot.venue_id = reservation.venue;
    result.snapshot.venue_session_id = reservation.venue_session_id;
    result.snapshot.venue_session_epoch = reservation.venue_session_epoch;
    result.snapshot.ownership_generation = reservation.ownership_generation;
    result.snapshot.last_dispatch_attempt_id =
        reservation.last_dispatch_attempt_id;
    result.snapshot.wire_identity = reservation.wire_identity;
    result.snapshot.state = reservation.cancel_pending
        ? ActiveChildOwnershipState::CancelPending
        : ActiveChildOwnershipState::Working;
    result.snapshot.pending_client_request_id =
        reservation.pending_client_request_id;
    result.snapshot.pending_client_sequence =
        reservation.pending_client_sequence;
    result.snapshot.pending_client_session_epoch =
        reservation.pending_client_session_epoch;
    result.snapshot.pending_cancel_attempt_id =
        reservation.pending_cancel_attempt_id;
    result.status = ActiveChildLookupStatus::Found;
    return result;
}

ActiveChildTransitionStatus RouteKernel::mark_cancel_pending(
    const ActiveChildCancelTransition& transition) noexcept {
    if (!impl_ || impl_->recovery_only || !transition.expected.child_id ||
        transition.client_request_id == 0U ||
        transition.client_sequence == 0U ||
        transition.client_session_epoch == 0U ||
        transition.dispatch_attempt_id == 0U) {
        return ActiveChildTransitionStatus::Rejected;
    }
    const ActiveChildLookupResult lookup =
        lookup_active_child(transition.expected.child_id);
    if (lookup.status != ActiveChildLookupStatus::Found) {
        return ActiveChildTransitionStatus::Terminal;
    }
    if (lookup.snapshot != transition.expected) {
        return ActiveChildTransitionStatus::StaleSnapshot;
    }
    if (lookup.snapshot.state == ActiveChildOwnershipState::CancelPending) {
        return lookup.snapshot.pending_client_request_id ==
                   transition.client_request_id &&
               lookup.snapshot.pending_client_sequence ==
                   transition.client_sequence &&
               lookup.snapshot.pending_client_session_epoch ==
                   transition.client_session_epoch
            ? ActiveChildTransitionStatus::AlreadyPendingSame
            : ActiveChildTransitionStatus::AlreadyPendingOther;
    }
    if (transition.dispatch_attempt_id <=
            lookup.snapshot.last_dispatch_attempt_id ||
        transition.dispatch_attempt_id < impl_->next_dispatch_attempt_id) {
        return ActiveChildTransitionStatus::Rejected;
    }
    if (transition.dispatch_attempt_id ==
            std::numeric_limits<std::uint64_t>::max() ||
        lookup.snapshot.ownership_generation ==
            std::numeric_limits<std::uint64_t>::max() ||
        impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return ActiveChildTransitionStatus::GenerationExhausted;
    }
    const auto location = impl_->child_index.find(transition.expected.child_id);
    if (!location) {
        return ActiveChildTransitionStatus::Rejected;
    }
    Impl::ParentReservation& parent =
        impl_->parents[*location / kMaximumCandidatesPerInstrument];
    Impl::Reservation& reservation =
        parent.children[*location % kMaximumCandidatesPerInstrument];
    reservation.cancel_pending = true;
    reservation.pending_client_request_id = transition.client_request_id;
    reservation.pending_client_sequence = transition.client_sequence;
    reservation.pending_client_session_epoch =
        transition.client_session_epoch;
    reservation.pending_cancel_attempt_id = transition.dispatch_attempt_id;
    reservation.last_dispatch_attempt_id = transition.dispatch_attempt_id;
    ++reservation.ownership_generation;
    impl_->next_dispatch_attempt_id = transition.dispatch_attempt_id + 1U;
    ++impl_->state_generation;
    return ActiveChildTransitionStatus::Marked;
}

bool RouteKernel::begin_recovery() noexcept {
    if (!impl_ || impl_->recovery_only || impl_->active_parents != 0U) {
        return false;
    }
    impl_->kill_switch = true;
    impl_->routing_enabled = false;
    impl_->recovery_only = true;
    impl_->unreconciled_children = 0U;
    return true;
}

RecoveryImportStatus RouteKernel::restore_recovered_parent(
    const RecoveredParentReservation& source) noexcept {
    if (!impl_ || !impl_->recovery_only) {
        return RecoveryImportStatus::NotRecoveryOnly;
    }
    if (!source.parent.id || !source.parent.instrument ||
        !source.parent.quantity || source.parent.limit_price.units <= 0 ||
        !valid_side(source.parent.side) || !valid_intent(source.parent.intent) ||
        source.child_count == 0U ||
        source.child_count > source.children.size()) {
        return RecoveryImportStatus::InvalidReservation;
    }
    if (source.candidate_table_generation != impl_->candidate_generation ||
        source.risk_generation != impl_->configured_risk_generation) {
        return RecoveryImportStatus::ConfigurationMismatch;
    }
    if (source.kernel_state_generation != 0U &&
        source.kernel_state_generation != impl_->state_generation) {
        return RecoveryImportStatus::StateGenerationMismatch;
    }

    const std::size_t existing_slot = impl_->parent_slot(source.parent.id);
    if (existing_slot != kInvalidIndex) {
        const Impl::ParentReservation& existing =
            impl_->parents[existing_slot];
        if (existing.child_count != source.child_count) {
            return RecoveryImportStatus::DuplicateParent;
        }
        for (std::size_t index{}; index < source.child_count; ++index) {
            const Impl::Reservation& left = existing.children[index];
            const RecoveredChildReservation& right = source.children[index];
            if (left.child != right.child_order_id ||
                left.dispatch_attempt_id != right.dispatch_attempt_id ||
                left.last_dispatch_attempt_id !=
                    right.last_dispatch_attempt_id ||
                left.venue != right.venue ||
                left.venue_session_id != right.venue_session_id ||
                left.venue_session_epoch != right.venue_session_epoch ||
                left.ownership_generation != right.ownership_generation ||
                left.wire_identity != right.wire_identity ||
                left.quantity != right.remaining_quantity ||
                left.cancel_pending != right.cancel_pending ||
                left.pending_client_request_id !=
                    right.pending_client_request_id ||
                left.pending_client_sequence !=
                    right.pending_client_sequence ||
                left.pending_client_session_epoch !=
                    right.pending_client_session_epoch ||
                left.pending_cancel_attempt_id !=
                    right.pending_cancel_attempt_id) {
                return RecoveryImportStatus::DuplicateParent;
            }
        }
        return RecoveryImportStatus::AlreadyImported;
    }
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return RecoveryImportStatus::GenerationExhausted;
    }
    bool duplicate{};
    const std::size_t parent_slot =
        impl_->insertion_slot(source.parent.id, duplicate);
    if (duplicate) {
        return RecoveryImportStatus::DuplicateParent;
    }
    if (parent_slot == kInvalidIndex ||
        impl_->active_parents >= impl_->parents.size()) {
        return RecoveryImportStatus::CapacityExceeded;
    }
    const auto instrument_index =
        impl_->instrument_index.find(source.parent.instrument);
    if (!instrument_index ||
        *instrument_index > std::numeric_limits<std::uint16_t>::max()) {
        return RecoveryImportStatus::RiskViolation;
    }
    InstrumentSlot& instrument = impl_->instruments[*instrument_index];
    const auto parent_notional = checked_notional(
        source.parent.limit_price, source.parent.quantity);
    if (!instrument.limit.allowed ||
        source.parent.quantity > instrument.limit.maximum_parent_quantity ||
        !parent_notional ||
        *parent_notional > instrument.limit.maximum_parent_notional ||
        source.parent.limit_price < instrument.limit.minimum_price ||
        source.parent.limit_price > instrument.limit.maximum_price ||
        instrument.outstanding != source.instrument_outstanding_before ||
        instrument.open_parents != source.instrument_open_parents_before ||
        instrument.open_children != source.instrument_open_children_before) {
        return RecoveryImportStatus::CounterMismatch;
    }

    Quantity added{};
    std::uint64_t maximum_child{};
    std::uint64_t maximum_attempt{};
    for (std::size_t index{}; index < source.child_count; ++index) {
        const RecoveredChildReservation& child = source.children[index];
        const bool pending_shape = child.cancel_pending
            ? child.pending_client_request_id != 0U &&
                  child.pending_client_sequence != 0U &&
                  child.pending_client_session_epoch != 0U &&
                  child.pending_cancel_attempt_id != 0U &&
                  child.last_dispatch_attempt_id ==
                      child.pending_cancel_attempt_id &&
                  child.pending_cancel_attempt_id > child.dispatch_attempt_id
            : child.pending_client_request_id == 0U &&
                  child.pending_client_sequence == 0U &&
                  child.pending_client_session_epoch == 0U &&
                  child.pending_cancel_attempt_id == 0U &&
                  child.last_dispatch_attempt_id == child.dispatch_attempt_id;
        if (!child.child_order_id || child.dispatch_attempt_id == 0U ||
            child.last_dispatch_attempt_id == 0U || !child.venue ||
            child.venue_session_id == 0U || child.venue_session_epoch == 0U ||
            child.ownership_generation == 0U ||
            child.wire_identity.size == 0U ||
            child.wire_identity.size > child.wire_identity.bytes.size() ||
            !pending_shape || !child.remaining_quantity ||
            child.tx_credit_generation == 0U ||
            child.throttle_generation == 0U ||
            child.normal_tx_credit_delta == 0U ||
            child.throttle_credit_delta == 0U ||
            child.normal_tx_credit_delta > child.reported_normal_tx_credits ||
            child.cancel_tx_credit_delta > child.reported_cancel_tx_credits ||
            child.throttle_credit_delta > child.reported_throttle_credits) {
            return RecoveryImportStatus::InvalidReservation;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (source.children[prior].child_order_id == child.child_order_id ||
                source.children[prior].dispatch_attempt_id ==
                    child.dispatch_attempt_id ||
                source.children[prior].venue == child.venue) {
                return RecoveryImportStatus::InvalidReservation;
            }
        }
        if (impl_->child_index.find(child.child_order_id)) {
            return RecoveryImportStatus::DuplicateParent;
        }
        const WireIdentityKey identity_key = make_wire_identity_key(
            child.venue,
            child.venue_session_id,
            child.venue_session_epoch,
            child.wire_identity);
        if (impl_->wire_identity_index.find(identity_key)) {
            return RecoveryImportStatus::InvalidReservation;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            const RecoveredChildReservation& previous = source.children[prior];
            if (make_wire_identity_key(
                    previous.venue,
                    previous.venue_session_id,
                    previous.venue_session_epoch,
                    previous.wire_identity) == identity_key) {
                return RecoveryImportStatus::InvalidReservation;
            }
        }
        if (impl_->wire_identity_mode == WireIdentityMode::RequireRegistered) {
            const WireIdentityPlannerBinding* const binding =
                impl_->find_wire_identity_planner(child.venue);
            InlineWireIdentity expected_identity;
            if (binding == nullptr ||
                binding->venue_session_id != child.venue_session_id ||
                binding->venue_session_epoch != child.venue_session_epoch) {
                return RecoveryImportStatus::ConfigurationMismatch;
            }
            const WireIdentityPlanRequest request{
                child.venue,
                child.child_order_id,
                child.dispatch_attempt_id,
                child.venue_session_id,
                child.venue_session_epoch,
            };
            if (binding->planner(request, expected_identity) !=
                    WireIdentityPlanStatus::Planned ||
                expected_identity != child.wire_identity) {
                return RecoveryImportStatus::InvalidReservation;
            }
        }
        const auto venue_index = impl_->venue_index.find(child.venue);
        if (!venue_index ||
            *venue_index > std::numeric_limits<std::uint16_t>::max()) {
            return RecoveryImportStatus::RiskViolation;
        }
        const VenueSlot& venue = impl_->venues[*venue_index];
        if (!venue.limit.allowed ||
            venue.outstanding != source.venue_outstanding_before[index] ||
            source.venue_outstanding_after[index] !=
                checked_add(
                    source.venue_outstanding_before[index],
                    child.remaining_quantity).value_or(Quantity{}) ||
            source.venue_outstanding_after[index] >
                venue.limit.maximum_outstanding) {
            return RecoveryImportStatus::CounterMismatch;
        }
        const bool same_tx_generation =
            venue.tx_credit_generation == child.tx_credit_generation;
        const bool same_throttle_generation =
            venue.throttle_generation == child.throttle_generation;
        const std::uint32_t reserved_normal = same_tx_generation ?
            venue.reserved_normal_tx_credits : 0U;
        const std::uint32_t reserved_cancel = same_tx_generation ?
            venue.reserved_cancel_tx_credits : 0U;
        const std::uint32_t reserved_throttle = same_throttle_generation ?
            venue.reserved_throttle_credits : 0U;
        if ((same_tx_generation &&
             (venue.reported_normal_tx_credits !=
                  child.reported_normal_tx_credits ||
              venue.reported_cancel_tx_credits !=
                  child.reported_cancel_tx_credits)) ||
            (same_throttle_generation &&
             venue.reported_throttle_credits !=
                 child.reported_throttle_credits) ||
            child.normal_tx_credit_delta >
                child.reported_normal_tx_credits -
                    std::min(
                        child.reported_normal_tx_credits,
                        reserved_normal) ||
            child.cancel_tx_credit_delta >
                child.reported_cancel_tx_credits -
                    std::min(
                        child.reported_cancel_tx_credits,
                        reserved_cancel) ||
            child.throttle_credit_delta >
                child.reported_throttle_credits -
                    std::min(
                        child.reported_throttle_credits,
                        reserved_throttle)) {
            return RecoveryImportStatus::CreditGenerationMismatch;
        }
        const auto next_added = checked_add(added, child.remaining_quantity);
        if (!next_added ||
            child.remaining_quantity > instrument.limit.maximum_child_quantity) {
            return RecoveryImportStatus::RiskViolation;
        }
        added = *next_added;
        maximum_child = std::max(
            maximum_child, child.child_order_id.value());
        maximum_attempt = std::max(
            maximum_attempt, child.last_dispatch_attempt_id);
    }
    const auto expected_instrument_after = checked_add(
        source.instrument_outstanding_before, added);
    if (!expected_instrument_after ||
        *expected_instrument_after != source.instrument_outstanding_after ||
        source.instrument_outstanding_after >
            instrument.limit.maximum_gross_outstanding ||
        source.instrument_open_parents_before ==
            std::numeric_limits<std::uint64_t>::max() ||
        source.instrument_open_parents_after !=
            source.instrument_open_parents_before + 1U ||
        source.child_count >
            std::numeric_limits<std::uint64_t>::max() -
                source.instrument_open_children_before ||
        source.instrument_open_children_after !=
            source.instrument_open_children_before + source.child_count ||
        source.instrument_open_parents_after >
            instrument.limit.maximum_open_parents ||
        source.instrument_open_children_after >
            instrument.limit.maximum_open_children ||
        maximum_child == std::numeric_limits<std::uint64_t>::max() ||
        maximum_attempt == std::numeric_limits<std::uint64_t>::max()) {
        return RecoveryImportStatus::RiskViolation;
    }

    Impl::ParentReservation target;
    target.state = Impl::ParentSlotState::Occupied;
    target.parent = source.parent.id;
    target.instrument_index = static_cast<std::uint16_t>(*instrument_index);
    target.child_count = source.child_count;
    for (std::size_t index{}; index < source.child_count; ++index) {
        const RecoveredChildReservation& source_child = source.children[index];
        const auto venue_index = impl_->venue_index.find(source_child.venue);
        Impl::Reservation& child = target.children[index];
        child.child = source_child.child_order_id;
        child.dispatch_attempt_id = source_child.dispatch_attempt_id;
        child.last_dispatch_attempt_id =
            source_child.last_dispatch_attempt_id;
        child.venue = source_child.venue;
        child.venue_session_id = source_child.venue_session_id;
        child.venue_session_epoch = source_child.venue_session_epoch;
        child.ownership_generation = source_child.ownership_generation;
        child.wire_identity = source_child.wire_identity;
        child.wire_identity_key = make_wire_identity_key(
            source_child.venue,
            source_child.venue_session_id,
            source_child.venue_session_epoch,
            source_child.wire_identity);
        child.quantity = source_child.remaining_quantity;
        child.venue_index = static_cast<std::uint16_t>(*venue_index);
        child.tx_credit_generation = source_child.tx_credit_generation;
        child.throttle_generation = source_child.throttle_generation;
        child.normal_tx_credit_delta = source_child.normal_tx_credit_delta;
        child.cancel_tx_credit_delta = source_child.cancel_tx_credit_delta;
        child.throttle_credit_delta = source_child.throttle_credit_delta;
        child.recovered = true;
        child.reconciled = source_child.reconciled;
        child.cancel_pending = source_child.cancel_pending;
        child.pending_client_request_id =
            source_child.pending_client_request_id;
        child.pending_client_sequence = source_child.pending_client_sequence;
        child.pending_client_session_epoch =
            source_child.pending_client_session_epoch;
        child.pending_cancel_attempt_id =
            source_child.pending_cancel_attempt_id;
    }

    for (std::size_t index{}; index < source.child_count; ++index) {
        if (impl_->child_index.find(target.children[index].child) ||
            impl_->wire_identity_index.find(
                target.children[index].wire_identity_key)) {
            return RecoveryImportStatus::CapacityExceeded;
        }
    }
    std::size_t inserted_children{};
    for (std::size_t index{}; index < source.child_count; ++index) {
        const std::size_t location =
            parent_slot * kMaximumCandidatesPerInstrument + index;
        if (!impl_->child_index.insert(target.children[index].child, location) ||
            !impl_->wire_identity_index.insert(
                target.children[index].wire_identity_key, location)) {
            (void)impl_->child_index.erase(target.children[index].child);
            for (std::size_t rollback{}; rollback < inserted_children;
                 ++rollback) {
                (void)impl_->child_index.erase(target.children[rollback].child);
                (void)impl_->wire_identity_index.erase(
                    target.children[rollback].wire_identity_key);
            }
            return RecoveryImportStatus::CapacityExceeded;
        }
        ++inserted_children;
    }

    instrument.outstanding = source.instrument_outstanding_after;
    instrument.open_parents = source.instrument_open_parents_after;
    instrument.open_children = source.instrument_open_children_after;
    for (std::size_t index{}; index < source.child_count; ++index) {
        const RecoveredChildReservation& source_child = source.children[index];
        VenueSlot& venue = impl_->venues[target.children[index].venue_index];
        if (venue.tx_credit_generation != source_child.tx_credit_generation) {
            venue.tx_credit_generation = source_child.tx_credit_generation;
            venue.reported_normal_tx_credits =
                source_child.reported_normal_tx_credits;
            venue.reported_cancel_tx_credits =
                source_child.reported_cancel_tx_credits;
            venue.reserved_normal_tx_credits = 0U;
            venue.reserved_cancel_tx_credits = 0U;
        }
        if (venue.throttle_generation != source_child.throttle_generation) {
            venue.throttle_generation = source_child.throttle_generation;
            venue.reported_throttle_credits =
                source_child.reported_throttle_credits;
            venue.reserved_throttle_credits = 0U;
        }
        venue.outstanding = source.venue_outstanding_after[index];
        venue.reserved_normal_tx_credits +=
            source_child.normal_tx_credit_delta;
        venue.reserved_cancel_tx_credits +=
            source_child.cancel_tx_credit_delta;
        venue.reserved_throttle_credits +=
            source_child.throttle_credit_delta;
        if (!source_child.reconciled) {
            ++impl_->unreconciled_children;
        }
    }
    impl_->parents[parent_slot] = target;
    ++impl_->active_parents;
    impl_->next_child_order_id = std::max(
        impl_->next_child_order_id, maximum_child + 1U);
    impl_->next_dispatch_attempt_id = std::max(
        impl_->next_dispatch_attempt_id, maximum_attempt + 1U);
    ++impl_->state_generation;
    return RecoveryImportStatus::Imported;
}

RecoveryAccountingStatus RouteKernel::apply_recovered_fill(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t dispatch_attempt_id,
    const Quantity filled_quantity) noexcept {
    if (!impl_ || !impl_->recovery_only) {
        return RecoveryAccountingStatus::NotRecoveryOnly;
    }
    if (!parent || !child || dispatch_attempt_id == 0U || !filled_quantity) {
        return RecoveryAccountingStatus::InvalidQuantity;
    }
    const std::size_t parent_slot = impl_->parent_slot(parent);
    if (parent_slot == kInvalidIndex) {
        return RecoveryAccountingStatus::ParentNotFound;
    }
    Impl::ParentReservation& parent_record = impl_->parents[parent_slot];
    Impl::Reservation* reservation = nullptr;
    for (std::size_t index{}; index < parent_record.child_count; ++index) {
        if (parent_record.children[index].child == child) {
            reservation = &parent_record.children[index];
            break;
        }
    }
    if (reservation == nullptr || reservation->released) {
        return RecoveryAccountingStatus::ChildNotFound;
    }
    if (reservation->dispatch_attempt_id != dispatch_attempt_id) {
        return RecoveryAccountingStatus::AttemptMismatch;
    }
    if (filled_quantity > reservation->quantity) {
        return RecoveryAccountingStatus::InvalidQuantity;
    }
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return RecoveryAccountingStatus::GenerationExhausted;
    }
    InstrumentSlot& instrument =
        impl_->instruments[parent_record.instrument_index];
    VenueSlot& venue = impl_->venues[reservation->venue_index];
    const bool terminal = filled_quantity == reservation->quantity;
    if (terminal) {
        const auto child_location = impl_->child_index.find(reservation->child);
        const auto wire_location =
            impl_->wire_identity_index.find(reservation->wire_identity_key);
        if (!child_location || !wire_location ||
            *child_location != *wire_location) {
            return RecoveryAccountingStatus::InvariantViolation;
        }
    }
    if (instrument.outstanding < filled_quantity ||
        venue.outstanding < filled_quantity ||
        (terminal &&
         (instrument.open_children == 0U ||
          (parent_record.released_count + 1U == parent_record.child_count &&
           (instrument.open_parents == 0U || impl_->active_parents == 0U))))) {
        return RecoveryAccountingStatus::InvariantViolation;
    }
    instrument.outstanding = *checked_subtract(
        instrument.outstanding, filled_quantity);
    venue.outstanding = *checked_subtract(venue.outstanding, filled_quantity);
    reservation->quantity = *checked_subtract(
        reservation->quantity, filled_quantity);
    if (terminal) {
        --instrument.open_children;
        if (!impl_->child_index.erase(reservation->child)) {
            return RecoveryAccountingStatus::InvariantViolation;
        }
        if (!impl_->wire_identity_index.erase(
                reservation->wire_identity_key)) {
            return RecoveryAccountingStatus::InvariantViolation;
        }
        reservation->released = true;
        ++parent_record.released_count;
        if (!reservation->reconciled) {
            --impl_->unreconciled_children;
            reservation->reconciled = true;
        }
        if (parent_record.released_count == parent_record.child_count) {
            --instrument.open_parents;
            --impl_->active_parents;
            parent_record.state = Impl::ParentSlotState::Tombstone;
        }
    }
    ++impl_->state_generation;
    return RecoveryAccountingStatus::Applied;
}

RecoveryAccountingStatus RouteKernel::note_recovered_cancel(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t prior_dispatch_attempt_id,
    const std::uint64_t cancel_dispatch_attempt_id,
    const std::uint64_t client_request_id,
    const std::uint64_t client_sequence,
    const std::uint64_t client_session_epoch) noexcept {
    if (!impl_ || !impl_->recovery_only) {
        return RecoveryAccountingStatus::NotRecoveryOnly;
    }
    if (prior_dispatch_attempt_id == 0U || cancel_dispatch_attempt_id == 0U ||
        cancel_dispatch_attempt_id <= prior_dispatch_attempt_id ||
        client_request_id == 0U || client_sequence == 0U ||
        client_session_epoch == 0U) {
        return RecoveryAccountingStatus::InvariantViolation;
    }
    const std::size_t parent_slot = impl_->parent_slot(parent);
    if (parent_slot == kInvalidIndex) {
        return RecoveryAccountingStatus::ParentNotFound;
    }
    Impl::ParentReservation& parent_record = impl_->parents[parent_slot];
    for (std::size_t index{}; index < parent_record.child_count; ++index) {
        Impl::Reservation& reservation = parent_record.children[index];
        if (reservation.child != child || reservation.released) {
            continue;
        }
        if (reservation.cancel_pending) {
            return reservation.pending_client_request_id == client_request_id &&
                    reservation.pending_client_sequence == client_sequence &&
                    reservation.pending_client_session_epoch ==
                        client_session_epoch &&
                    reservation.pending_cancel_attempt_id ==
                        cancel_dispatch_attempt_id
                ? RecoveryAccountingStatus::AlreadyApplied
                : RecoveryAccountingStatus::InvariantViolation;
        }
        if (reservation.last_dispatch_attempt_id !=
            prior_dispatch_attempt_id) {
            return RecoveryAccountingStatus::AttemptMismatch;
        }
        if (impl_->state_generation ==
                std::numeric_limits<std::uint64_t>::max() ||
            reservation.ownership_generation ==
                std::numeric_limits<std::uint64_t>::max() ||
            cancel_dispatch_attempt_id ==
                std::numeric_limits<std::uint64_t>::max()) {
            return RecoveryAccountingStatus::GenerationExhausted;
        }
        reservation.cancel_pending = true;
        reservation.pending_client_request_id = client_request_id;
        reservation.pending_client_sequence = client_sequence;
        reservation.pending_client_session_epoch = client_session_epoch;
        reservation.pending_cancel_attempt_id = cancel_dispatch_attempt_id;
        reservation.last_dispatch_attempt_id = cancel_dispatch_attempt_id;
        ++reservation.ownership_generation;
        impl_->next_dispatch_attempt_id = std::max(
            impl_->next_dispatch_attempt_id,
            cancel_dispatch_attempt_id + 1U);
        ++impl_->state_generation;
        return RecoveryAccountingStatus::Applied;
    }
    return RecoveryAccountingStatus::ChildNotFound;
}

RecoveryAccountingStatus RouteKernel::reconcile_recovered_child(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t dispatch_attempt_id,
    const RecoveredChildDisposition disposition) noexcept {
    if (!impl_ || !impl_->recovery_only) {
        return RecoveryAccountingStatus::NotRecoveryOnly;
    }
    if (disposition != RecoveredChildDisposition::Working &&
        disposition != RecoveredChildDisposition::Terminal) {
        return RecoveryAccountingStatus::InvariantViolation;
    }
    const std::size_t parent_slot = impl_->parent_slot(parent);
    if (parent_slot == kInvalidIndex) {
        return RecoveryAccountingStatus::ParentNotFound;
    }
    Impl::ParentReservation& parent_record = impl_->parents[parent_slot];
    Impl::Reservation* reservation = nullptr;
    for (std::size_t index{}; index < parent_record.child_count; ++index) {
        if (parent_record.children[index].child == child) {
            reservation = &parent_record.children[index];
            break;
        }
    }
    if (reservation == nullptr || reservation->released) {
        return RecoveryAccountingStatus::ChildNotFound;
    }
    if (reservation->dispatch_attempt_id != dispatch_attempt_id) {
        return RecoveryAccountingStatus::AttemptMismatch;
    }
    if (disposition == RecoveredChildDisposition::Working) {
        if (reservation->reconciled) {
            return RecoveryAccountingStatus::AlreadyApplied;
        }
        if (impl_->state_generation ==
            std::numeric_limits<std::uint64_t>::max() ||
            impl_->unreconciled_children == 0U) {
            return RecoveryAccountingStatus::GenerationExhausted;
        }
        reservation->reconciled = true;
        --impl_->unreconciled_children;
        ++impl_->state_generation;
        return RecoveryAccountingStatus::Applied;
    }

    InstrumentSlot& instrument =
        impl_->instruments[parent_record.instrument_index];
    VenueSlot& venue = impl_->venues[reservation->venue_index];
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return RecoveryAccountingStatus::GenerationExhausted;
    }
    if (instrument.outstanding < reservation->quantity ||
        venue.outstanding < reservation->quantity ||
        !impl_->child_index.find(reservation->child) ||
        !impl_->wire_identity_index.find(reservation->wire_identity_key) ||
        instrument.open_children == 0U ||
        (parent_record.released_count + 1U == parent_record.child_count &&
         (instrument.open_parents == 0U || impl_->active_parents == 0U)) ||
        (!reservation->reconciled && impl_->unreconciled_children == 0U)) {
        return RecoveryAccountingStatus::InvariantViolation;
    }
    instrument.outstanding = *checked_subtract(
        instrument.outstanding, reservation->quantity);
    venue.outstanding = *checked_subtract(
        venue.outstanding, reservation->quantity);
    --instrument.open_children;
    reservation->quantity = Quantity{};
    if (!impl_->child_index.erase(reservation->child)) {
        return RecoveryAccountingStatus::InvariantViolation;
    }
    if (!impl_->wire_identity_index.erase(reservation->wire_identity_key)) {
        return RecoveryAccountingStatus::InvariantViolation;
    }
    reservation->released = true;
    if (!reservation->reconciled) {
        --impl_->unreconciled_children;
        reservation->reconciled = true;
    }
    ++parent_record.released_count;
    if (parent_record.released_count == parent_record.child_count) {
        --instrument.open_parents;
        --impl_->active_parents;
        parent_record.state = Impl::ParentSlotState::Tombstone;
    }
    ++impl_->state_generation;
    return RecoveryAccountingStatus::Applied;
}

bool RouteKernel::restore_recovery_kernel_state(
    const RecoveryKernelState state) noexcept {
    if (!impl_ || !impl_->recovery_only || state.state_generation == 0U ||
        state.identifiers.next_child_order_id == 0U ||
        state.identifiers.next_dispatch_attempt_id == 0U ||
        state.identifiers.next_child_order_id < impl_->next_child_order_id ||
        state.identifiers.next_dispatch_attempt_id <
            impl_->next_dispatch_attempt_id) {
        return false;
    }
    impl_->state_generation = state.state_generation;
    impl_->next_child_order_id = state.identifiers.next_child_order_id;
    impl_->next_dispatch_attempt_id =
        state.identifiers.next_dispatch_attempt_id;
    return true;
}

bool RouteKernel::restore_recovery_venue_credit_state(
    const RecoveryVenueCreditState& state) noexcept {
    if (!impl_ || !impl_->recovery_only || !state.found || !state.venue ||
        state.tx_credit_generation == 0U ||
        state.throttle_generation == 0U ||
        state.reserved_normal_tx_credits >
            state.reported_normal_tx_credits ||
        state.reserved_cancel_tx_credits >
            state.reported_cancel_tx_credits ||
        state.reserved_throttle_credits >
            state.reported_throttle_credits) {
        return false;
    }
    VenueSlot* const venue = impl_->find_venue(state.venue);
    if (venue == nullptr) {
        return false;
    }
    if ((venue->tx_credit_generation == state.tx_credit_generation &&
         (venue->reserved_normal_tx_credits >
              state.reserved_normal_tx_credits ||
          venue->reserved_cancel_tx_credits >
              state.reserved_cancel_tx_credits)) ||
        (venue->throttle_generation == state.throttle_generation &&
         venue->reserved_throttle_credits >
             state.reserved_throttle_credits)) {
        return false;
    }
    venue->tx_credit_generation = state.tx_credit_generation;
    venue->throttle_generation = state.throttle_generation;
    venue->reported_normal_tx_credits = state.reported_normal_tx_credits;
    venue->reported_cancel_tx_credits = state.reported_cancel_tx_credits;
    venue->reported_throttle_credits = state.reported_throttle_credits;
    venue->reserved_normal_tx_credits = state.reserved_normal_tx_credits;
    venue->reserved_cancel_tx_credits = state.reserved_cancel_tx_credits;
    venue->reserved_throttle_credits = state.reserved_throttle_credits;
    return true;
}

bool RouteKernel::complete_recovery() noexcept {
    if (!impl_ || !impl_->recovery_only ||
        impl_->unreconciled_children != 0U) {
        return false;
    }
    impl_->recovery_only = false;
    return true;
}

bool RouteKernel::set_kill_switch(const bool active) noexcept {
    if (!impl_) {
        return false;
    }
    if (impl_->kill_switch == active) {
        return true;
    }
    if (impl_->recovery_only && !active) {
        return false;
    }
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    impl_->kill_switch = active;
    ++impl_->state_generation;
    return true;
}

bool RouteKernel::set_routing_enabled(const bool enabled) noexcept {
    if (!impl_) {
        return false;
    }
    if (impl_->routing_enabled == enabled) {
        return true;
    }
    if (impl_->recovery_only && enabled) {
        return false;
    }
    if (impl_->state_generation == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    impl_->routing_enabled = enabled;
    ++impl_->state_generation;
    return true;
}

bool RouteKernel::kill_switch() const noexcept {
    return impl_ && impl_->kill_switch;
}

bool RouteKernel::routing_enabled() const noexcept {
    return impl_ && impl_->routing_enabled;
}

bool RouteKernel::recovery_only() const noexcept {
    return impl_ && impl_->recovery_only;
}

std::size_t RouteKernel::unreconciled_child_count() const noexcept {
    return impl_ ? impl_->unreconciled_children : 0U;
}

RecoveryIdentifierState RouteKernel::recovery_identifiers() const noexcept {
    if (!impl_) {
        return {};
    }
    return RecoveryIdentifierState{
        impl_->next_child_order_id,
        impl_->next_dispatch_attempt_id,
    };
}

RecoveryVenueCreditState RouteKernel::recovery_venue_credit_state(
    const VenueId venue) const noexcept {
    RecoveryVenueCreditState output;
    output.venue = venue;
    if (!impl_) {
        return output;
    }
    const VenueSlot* const slot = impl_->find_venue(venue);
    if (slot == nullptr) {
        return output;
    }
    output.tx_credit_generation = slot->tx_credit_generation;
    output.throttle_generation = slot->throttle_generation;
    output.reported_normal_tx_credits = slot->reported_normal_tx_credits;
    output.reported_cancel_tx_credits = slot->reported_cancel_tx_credits;
    output.reported_throttle_credits = slot->reported_throttle_credits;
    output.reserved_normal_tx_credits = slot->reserved_normal_tx_credits;
    output.reserved_cancel_tx_credits = slot->reserved_cancel_tx_credits;
    output.reserved_throttle_credits = slot->reserved_throttle_credits;
    output.found = true;
    return output;
}

std::uint64_t RouteKernel::state_generation() const noexcept {
    return impl_ ? impl_->state_generation : 0U;
}

std::uint64_t RouteKernel::candidate_table_generation() const noexcept {
    return impl_ ? impl_->candidate_generation : 0U;
}

std::uint64_t RouteKernel::risk_generation() const noexcept {
    return impl_ ? impl_->configured_risk_generation : 0U;
}

std::size_t RouteKernel::active_parent_count() const noexcept {
    return impl_ ? impl_->active_parents : 0U;
}

AdaptivePolicyMode RouteKernel::adaptive_policy_mode() const noexcept {
    return impl_ ? impl_->adaptive_policy_mode : AdaptivePolicyMode::Required;
}

InstrumentRiskCounters RouteKernel::instrument_counters(
    const InstrumentId instrument) const noexcept {
    InstrumentRiskCounters output;
    if (!impl_) {
        return output;
    }
    const InstrumentSlot* slot = impl_->find_instrument(instrument);
    if (slot != nullptr) {
        output.found = true;
        output.gross_outstanding = slot->outstanding;
        output.open_parents = slot->open_parents;
        output.open_children = slot->open_children;
    }
    return output;
}

VenueRiskCounters RouteKernel::venue_counters(const VenueId venue) const noexcept {
    VenueRiskCounters output;
    if (!impl_) {
        return output;
    }
    const VenueSlot* slot = impl_->find_venue(venue);
    if (slot != nullptr) {
        output.found = true;
        output.outstanding = slot->outstanding;
        output.reserved_normal_tx_credits =
            slot->reserved_normal_tx_credits;
        output.reserved_cancel_tx_credits =
            slot->reserved_cancel_tx_credits;
        output.reserved_throttle_credits =
            slot->reserved_throttle_credits;
    }
    return output;
}

}  // namespace sorx::dataplane
