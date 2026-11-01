#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/dataplane/route_kernel.hpp"
#include "sorx/routing_policy.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::dataplane;

constexpr std::uint64_t kCorpusSeed = 0x534f525845515631ULL;
constexpr InstrumentId kInstrument{11U};
constexpr std::size_t kVenueCount = 6U;

[[noreturn]] void fail_setup(const char* const detail) {
    std::fprintf(
        stderr,
        "dataplane equivalence failure seed=0x%llx detail=%s\n",
        static_cast<unsigned long long>(kCorpusSeed),
        detail);
    std::abort();
}

void require(const bool condition, const char* const detail) {
    if (!condition) {
        fail_setup(detail);
    }
}

class FixedPrng final {
public:
    explicit FixedPrng(const std::uint64_t seed) noexcept
        : state_(seed == 0U ? 1U : seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t value = state_;
        value ^= value >> 12U;
        value ^= value << 25U;
        value ^= value >> 27U;
        state_ = value;
        return value * 2'685'821'657'736'338'717ULL;
    }

    [[nodiscard]] std::uint64_t bounded(const std::uint64_t bound) noexcept {
        return bound == 0U ? 0U : next() % bound;
    }

private:
    std::uint64_t state_;
};

struct Fixture final {
    std::size_t variant{};
    CoreConfiguration configuration;
    std::shared_ptr<const CandidateTable> candidates;
    std::unique_ptr<RouteKernel> kernel;
};

[[nodiscard]] RationalConversion conversion(
    const std::uint64_t numerator,
    const std::uint64_t denominator) {
    const auto result = RationalConversion::create(numerator, denominator);
    require(result.has_value(), "invalid test conversion");
    return *result;
}

[[nodiscard]] Fixture make_fixture(const std::size_t variant) {
    std::vector<RouteConfig> routes;
    routes.reserve(kVenueCount);
    for (std::size_t index = 0U; index < kVenueCount; ++index) {
        const auto venue_number = static_cast<std::uint32_t>(index + 1U);
        RouteConfig route;
        route.venue = VenueId{venue_number};
        route.instrument = kInstrument;
        route.venue_name = "VENUE_" + std::to_string(venue_number);
        route.canonical_symbol = "EQV";
        route.wire_symbol = "EQV" + std::to_string(venue_number);
        route.capabilities = VenueCapabilities{true, true};
        if (index == 3U) {
            route.capabilities.passive_gtc = false;
        }
        if (index == 4U) {
            route.capabilities.aggressive_ioc = false;
        }
        route.allocation_cap = Quantity{
            35U + static_cast<std::uint64_t>(index * 17U) +
            static_cast<std::uint64_t>(variant * 9U)};
        route.aggressive_priority = static_cast<std::uint32_t>(
            1U + ((index * 3U + variant) % 4U));
        route.passive_priority = static_cast<std::uint32_t>(
            1U + ((index + variant * 2U) % 4U));
        route.configured_latency_ns =
            75U + static_cast<std::uint64_t>(index * 13U + variant * 7U);
        route.admin_state = index == 5U ? AdminState::Down : AdminState::Up;

        if ((variant == 1U && index == 1U) ||
            (variant == 3U && index == 2U)) {
            route.price_conversion = conversion(3U, 2U);
        }
        if ((variant == 2U && index == 2U) ||
            (variant == 3U && index == 1U)) {
            route.quantity_conversion = conversion(2U, 3U);
        }
        routes.push_back(std::move(route));
    }

    RouteRib rib;
    std::vector<RouteValidationError> errors;
    require(rib.replace_all(std::move(routes), errors), "RIB rejected fixture");
    require(errors.empty(), "fixture RIB produced errors");
    FibCompiler compiler;
    auto candidates = compiler.compile_at_generation(
        rib, 100U + static_cast<std::uint64_t>(variant));

    CoreConfiguration configuration;
    configuration.maximum_aggressive_venues = 2U + variant;
    configuration.risk.generation = 700U + static_cast<std::uint64_t>(variant);
    InstrumentRiskLimit instrument;
    instrument.instrument = kInstrument;
    instrument.allowed = true;
    instrument.maximum_parent_quantity = Quantity{1'000U};
    instrument.maximum_child_quantity = Quantity{1'000U};
    instrument.maximum_parent_notional =
        Notional{std::numeric_limits<std::uint64_t>::max()};
    instrument.maximum_child_notional =
        Notional{std::numeric_limits<std::uint64_t>::max()};
    instrument.minimum_price = Price{1};
    instrument.maximum_price = Price{5'000};
    instrument.maximum_open_parents = 2'000U;
    instrument.maximum_open_children = 10'000U;
    instrument.maximum_gross_outstanding = Quantity{1'000'000U};
    configuration.risk.instruments.push_back(instrument);
    for (std::size_t index = 0U; index < kVenueCount; ++index) {
        configuration.risk.venues.push_back(VenueRiskLimit{
            VenueId{static_cast<std::uint32_t>(index + 1U)},
            true,
            Quantity{1'000'000U},
        });
    }

    Fixture result;
    result.variant = variant;
    result.configuration = configuration;
    result.candidates = std::move(candidates);
    result.kernel = std::make_unique<RouteKernel>(
        result.configuration,
        *result.candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric);
    return result;
}

enum class ViewMode : std::uint8_t {
    Live,
    Omitted,
    MissingBook,
    MarketDataNotLive,
    Stale,
    Halted,
    OrderEntryNotReady,
    EgressCongested,
    ConversionIncompatible,
    RiskProhibited,
    NoDepth,
    SourceUnhealthy,
};

struct ViewPair final {
    BookSlotView fast;
    CandidateRuntimeView reference;
};

[[nodiscard]] BookSlotView make_view(
    const VenueId venue,
    const std::uint64_t case_number,
    const std::uint64_t now,
    const std::int64_t best_bid,
    const std::uint64_t bid_quantity,
    const std::uint64_t ask_quantity,
    const ViewMode mode,
    const bool truncated) {
    BookSlotView output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.adjacency_generation = 1'000U + case_number + venue.value();
    output.market_data_state = MarketDataState::Live;
    output.order_entry_state = OrderEntryState::Ready;
    output.egress_state = EgressState::Ready;
    output.instrument_state = InstrumentState::Trading;
    output.source_health_generation = 2'000U + case_number + venue.value();
    output.maximum_quote_age_ns = 200U;
    output.tx_credit_generation = 3'000U + case_number;
    output.throttle_generation = 4'000U + case_number;
    output.normal_tx_credits = 1'000U;
    output.cancel_reserved_tx_credits = 64U;
    output.throttle_credits = 1'000U;
    output.source_healthy = true;
    output.conversion_compatible = true;
    output.risk_allowed = true;
    output.monotonic_receive_time_ns = now - 30U;
    output.monotonic_publication_time_ns = now - 20U;

    if (mode == ViewMode::MissingBook) {
        output.has_book = false;
        output.source_healthy = false;
        require(output.valid(), "missing-book slot invalid");
        return output;
    }

    output.has_book = true;
    output.feed_epoch = 17U;
    md::BookView& book = output.book;
    book.venue = venue;
    book.channel = ChannelId{venue.value()};
    book.profile = ProfileId{1U};
    book.instrument = kInstrument;
    if (mode != ViewMode::NoDepth) {
        for (std::uint32_t depth = 0U; depth < 3U; ++depth) {
            require(
                book.push_bid(md::Level{
                    Price{best_bid - static_cast<std::int64_t>(depth)},
                    Quantity{bid_quantity + depth * 3U},
                    1U + depth,
                }),
                "bid depth overflow");
            require(
                book.push_ask(md::Level{
                    Price{best_bid + 2 + static_cast<std::int64_t>(depth)},
                    Quantity{ask_quantity + depth * 5U},
                    1U + depth,
                }),
                "ask depth overflow");
        }
    }
    book.feed_session_epoch = output.feed_epoch;
    book.recovery_generation = 3U;
    book.view_generation = 5'000U + case_number + venue.value();
    book.last_applied_sequence = 6'000U + case_number + venue.value();
    book.next_sequence = book.last_applied_sequence + 1U;
    book.exchange_timestamp_ns = static_cast<std::int64_t>(now - 50U);
    book.local_receive_time_ns = output.monotonic_receive_time_ns;
    book.local_publication_time_ns = output.monotonic_publication_time_ns;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    book.depth_truncated = truncated;

    switch (mode) {
        case ViewMode::MarketDataNotLive:
            output.market_data_state = MarketDataState::Recovering;
            book.feed_state = MarketDataState::Recovering;
            break;
        case ViewMode::Stale:
            output.monotonic_receive_time_ns = now - 500U;
            output.monotonic_publication_time_ns = now - 490U;
            book.local_receive_time_ns = output.monotonic_receive_time_ns;
            book.local_publication_time_ns = output.monotonic_publication_time_ns;
            break;
        case ViewMode::Halted:
            output.instrument_state = InstrumentState::Halted;
            book.trading_state = InstrumentState::Halted;
            break;
        case ViewMode::OrderEntryNotReady:
            output.order_entry_state = OrderEntryState::Degraded;
            break;
        case ViewMode::EgressCongested:
            output.egress_state = EgressState::Congested;
            break;
        case ViewMode::ConversionIncompatible:
            output.conversion_compatible = false;
            break;
        case ViewMode::RiskProhibited:
            output.risk_allowed = false;
            break;
        case ViewMode::SourceUnhealthy:
            output.source_healthy = false;
            break;
        case ViewMode::Live:
        case ViewMode::Omitted:
        case ViewMode::MissingBook:
        case ViewMode::NoDepth:
            break;
    }
    require(output.valid(), "generated book slot invalid");
    return output;
}

[[nodiscard]] CandidateRuntimeView reference_view(
    const BookSlotView& input,
    const std::uint64_t now) {
    CandidateRuntimeView output;
    output.venue = input.venue;
    output.adjacency_generation = input.adjacency_generation;
    if (input.has_book) {
        output.book = input.book;
    }
    output.market_data_state = input.market_data_state;
    output.order_entry_state = input.order_entry_state;
    output.egress_state = input.egress_state;
    output.instrument_state = input.instrument_state;
    output.feed_sufficiently_fresh = input.fresh_at(now);
    output.conversion_compatible = input.conversion_compatible;
    output.risk_allowed = input.risk_allowed;
    return output;
}

struct GeneratedViews final {
    std::vector<BookSlotView> fast;
    std::vector<CandidateRuntimeView> reference;
};

[[nodiscard]] GeneratedViews generate_views(
    const Fixture& fixture,
    FixedPrng& random,
    const std::uint64_t case_number,
    const std::uint64_t now) {
    std::vector<ViewPair> pairs;
    const CandidateSet* const set = fixture.candidates->find(kInstrument);
    require(set != nullptr, "fixture candidate set absent");
    pairs.reserve(set->candidates.size());
    for (const CandidateHandle& candidate : set->candidates) {
        const std::uint64_t roll = random.bounded(100U);
        ViewMode mode = ViewMode::Live;
        if (roll >= 52U) {
            mode = static_cast<ViewMode>(
                1U + static_cast<std::uint8_t>(random.bounded(11U)));
        }
        if (mode == ViewMode::Omitted) {
            continue;
        }
        const auto venue_offset = static_cast<std::int64_t>(
            candidate.venue.value() * 3U);
        const std::int64_t best_bid =
            980 + static_cast<std::int64_t>(random.bounded(70U)) + venue_offset;
        const BookSlotView fast = make_view(
            candidate.venue,
            case_number,
            now,
            best_bid,
            4U + random.bounded(60U),
            4U + random.bounded(60U),
            mode,
            random.bounded(23U) == 0U);
        pairs.push_back(ViewPair{fast, reference_view(fast, now)});
    }

    for (std::size_t index = pairs.size(); index > 1U; --index) {
        const std::size_t selected = static_cast<std::size_t>(
            random.bounded(static_cast<std::uint64_t>(index)));
        std::swap(pairs[index - 1U], pairs[selected]);
    }

    GeneratedViews result;
    result.fast.reserve(pairs.size());
    result.reference.reserve(pairs.size());
    for (ViewPair& pair : pairs) {
        result.fast.push_back(std::move(pair.fast));
        result.reference.push_back(std::move(pair.reference));
    }
    return result;
}

[[nodiscard]] RouteDecision reference_decision(
    const ParentOrder& parent,
    const Fixture& fixture,
    const std::span<const CandidateRuntimeView> views) {
    return parent.intent == RoutingIntent::AggressiveIOC
        ? RoutingPolicy::aggressive_ioc(
              parent,
              *fixture.candidates,
              views,
              fixture.configuration.maximum_aggressive_venues)
        : RoutingPolicy::passive_gtc(parent, *fixture.candidates, views);
}

void print_reference(const RouteDecision& decision) {
    std::fprintf(
        stderr,
        "reference reject=%u allocated=%llu remainder=%llu remainder_reason=%u "
        "allocations=%zu exclusions=%zu truncated=%u\n",
        static_cast<unsigned>(decision.reject_reason),
        static_cast<unsigned long long>(decision.allocated.units),
        static_cast<unsigned long long>(decision.remainder.units),
        static_cast<unsigned>(decision.remainder_reason),
        decision.allocations.size(),
        decision.exclusions.size(),
        decision.observed_truncated_depth ? 1U : 0U);
    for (const ChildAllocation& allocation : decision.allocations) {
        std::fprintf(
            stderr,
            "  ref allocation venue=%u qty=%llu price=%lld venue_qty=%llu "
            "venue_price=%lld order=%u\n",
            allocation.venue.value(),
            static_cast<unsigned long long>(allocation.quantity.units),
            static_cast<long long>(allocation.limit_price.units),
            static_cast<unsigned long long>(allocation.venue_quantity.units),
            static_cast<long long>(allocation.venue_limit_price.units),
            allocation.allocation_order);
    }
    for (const CandidateExclusion& exclusion : decision.exclusions) {
        std::fprintf(
            stderr,
            "  ref exclusion venue=%u reason=%u\n",
            exclusion.venue.value(),
            static_cast<unsigned>(exclusion.reason));
    }
}

void print_fast(const PlanResult& result) {
    std::fprintf(
        stderr,
        "fast status=%u hard=%u reject=%u allocated=%llu remainder=%llu "
        "remainder_reason=%u allocations=%u exclusions=%u truncated=%u\n",
        static_cast<unsigned>(result.status),
        static_cast<unsigned>(result.hard_error),
        static_cast<unsigned>(result.plan.reject_reason),
        static_cast<unsigned long long>(result.plan.allocated.units),
        static_cast<unsigned long long>(result.plan.remainder.units),
        static_cast<unsigned>(result.plan.remainder_reason),
        static_cast<unsigned>(result.plan.allocation_count),
        static_cast<unsigned>(result.plan.exclusion_count),
        result.plan.observed_truncated_depth ? 1U : 0U);
    for (const ChildAllocation& allocation : result.plan.child_allocations()) {
        std::fprintf(
            stderr,
            "  fast allocation venue=%u qty=%llu price=%lld venue_qty=%llu "
            "venue_price=%lld order=%u\n",
            allocation.venue.value(),
            static_cast<unsigned long long>(allocation.quantity.units),
            static_cast<long long>(allocation.limit_price.units),
            static_cast<unsigned long long>(allocation.venue_quantity.units),
            static_cast<long long>(allocation.venue_limit_price.units),
            allocation.allocation_order);
    }
    for (const CandidateExclusion& exclusion :
         result.plan.candidate_exclusions()) {
        std::fprintf(
            stderr,
            "  fast exclusion venue=%u reason=%u\n",
            exclusion.venue.value(),
            static_cast<unsigned>(exclusion.reason));
    }
}

[[noreturn]] void fail_equivalence(
    const std::size_t fixture_variant,
    const std::uint64_t case_number,
    const ParentOrder& parent,
    const char* const detail,
    const RouteDecision& reference,
    const PlanResult& fast) {
    std::fprintf(
        stderr,
        "dataplane equivalence mismatch seed=0x%llx fixture=%zu case=%llu "
        "detail=%s parent={id=%llu instrument=%u side=%u qty=%llu price=%lld "
        "intent=%u}\n",
        static_cast<unsigned long long>(kCorpusSeed),
        fixture_variant,
        static_cast<unsigned long long>(case_number),
        detail,
        static_cast<unsigned long long>(parent.id.value()),
        parent.instrument.value(),
        static_cast<unsigned>(parent.side),
        static_cast<unsigned long long>(parent.quantity.units),
        static_cast<long long>(parent.limit_price.units),
        static_cast<unsigned>(parent.intent));
    print_reference(reference);
    print_fast(fast);
    std::abort();
}

void compare_shared_semantics(
    const Fixture& fixture,
    const std::uint64_t case_number,
    const ParentOrder& parent,
    const RouteDecision& reference,
    const PlanResult& fast) {
    const auto mismatch = [&](const char* const detail) {
        fail_equivalence(
            fixture.variant, case_number, parent, detail, reference, fast);
    };
    const PlanStatus expected_status = reference.reject_reason == RouteRejectReason::None
        ? PlanStatus::Planned
        : PlanStatus::RouteRejected;
    if (fast.status != expected_status || fast.hard_error != CommandError::None) {
        mismatch("plan disposition");
    }
    if (reference.accepted() != fast.accepted() ||
        reference.reject_reason != fast.plan.reject_reason ||
        reference.allocated != fast.plan.allocated ||
        reference.remainder != fast.plan.remainder ||
        reference.remainder_reason != fast.plan.remainder_reason ||
        reference.observed_truncated_depth !=
            fast.plan.observed_truncated_depth) {
        mismatch("decision summary");
    }
    if (reference.candidate_table_generation !=
            fast.plan.candidate_table_generation ||
        reference.allocations.size() != fast.plan.allocation_count ||
        reference.exclusions.size() != fast.plan.exclusion_count ||
        reference.candidate_pins.size() != fast.plan.candidate_pin_count) {
        mismatch("decision cardinality or generation");
    }
    for (std::size_t index = 0U; index < reference.allocations.size(); ++index) {
        if (reference.allocations[index] != fast.plan.allocations[index]) {
            mismatch("allocation ordering or values");
        }
    }
    for (std::size_t index = 0U; index < reference.exclusions.size(); ++index) {
        if (reference.exclusions[index] != fast.plan.exclusions[index]) {
            mismatch("exclusion ordering or reason");
        }
    }
    for (std::size_t index = 0U; index < reference.candidate_pins.size(); ++index) {
        if (reference.candidate_pins[index] !=
            fast.plan.candidate_pins[index].market) {
            mismatch("market generation pin");
        }
    }
}

[[nodiscard]] bool equivalent_fast_plans(
    const RoutePlan& lhs,
    const RoutePlan& rhs) noexcept {
    if (lhs.parent != rhs.parent ||
        lhs.kernel_state_generation != rhs.kernel_state_generation ||
        lhs.candidate_table_generation != rhs.candidate_table_generation ||
        lhs.risk_generation != rhs.risk_generation ||
        lhs.monotonic_decision_time_ns != rhs.monotonic_decision_time_ns ||
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
        if (lhs.allocations[index] != rhs.allocations[index] ||
            lhs.planned_children[index] != rhs.planned_children[index]) {
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

struct Coverage final {
    std::uint64_t cases{};
    std::uint64_t aggressive{};
    std::uint64_t passive{};
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t partial{};
    std::uint64_t converted_allocations{};
    std::array<bool, 32U> exclusion_reasons{};
    std::array<bool, 16U> remainder_reasons{};
};

void observe(Coverage& coverage, const RouteDecision& decision) {
    ++coverage.cases;
    if (decision.accepted()) {
        ++coverage.accepted;
    } else {
        ++coverage.rejected;
    }
    if (decision.accepted() && decision.remainder.units != 0U) {
        ++coverage.partial;
    }
    const auto remainder_index =
        static_cast<std::size_t>(decision.remainder_reason);
    if (remainder_index < coverage.remainder_reasons.size()) {
        coverage.remainder_reasons[remainder_index] = true;
    }
    for (const CandidateExclusion& exclusion : decision.exclusions) {
        const auto index = static_cast<std::size_t>(exclusion.reason);
        if (index < coverage.exclusion_reasons.size()) {
            coverage.exclusion_reasons[index] = true;
        }
    }
    for (const ChildAllocation& allocation : decision.allocations) {
        if (allocation.quantity != allocation.venue_quantity ||
            allocation.limit_price != allocation.venue_limit_price) {
            ++coverage.converted_allocations;
        }
    }
}

void deterministic_corpus_tests(std::vector<Fixture>& fixtures) {
    FixedPrng random{kCorpusSeed};
    Coverage coverage;
    constexpr std::size_t kCasesPerFixture = 160U;
    for (Fixture& fixture : fixtures) {
        for (std::size_t local_case = 0U;
             local_case < kCasesPerFixture;
             ++local_case) {
            const std::uint64_t case_number =
                static_cast<std::uint64_t>(fixture.variant * kCasesPerFixture +
                                           local_case + 1U);
            const std::uint64_t now = 1'000'000U + case_number * 1'000U;
            GeneratedViews views =
                generate_views(fixture, random, case_number, now);
            ParentOrder parent;
            parent.id = ParentOrderId{case_number};
            parent.instrument = kInstrument;
            parent.side = random.bounded(2U) == 0U ? Side::Buy : Side::Sell;
            parent.quantity = Quantity{1U + random.bounded(250U)};
            parent.limit_price = Price{
                940 + static_cast<std::int64_t>(random.bounded(180U))};
            parent.intent = local_case % 2U == 0U
                ? RoutingIntent::AggressiveIOC
                : RoutingIntent::PassiveGTC;
            if (parent.intent == RoutingIntent::AggressiveIOC) {
                ++coverage.aggressive;
            } else {
                ++coverage.passive;
            }

            const RouteDecision reference = reference_decision(
                parent, fixture, views.reference);
            const PlanResult first = fixture.kernel->plan(parent, views.fast, now);
            compare_shared_semantics(
                fixture, case_number, parent, reference, first);
            const PlanResult replay = fixture.kernel->plan(parent, views.fast, now);
            if (first.status != replay.status ||
                first.hard_error != replay.hard_error ||
                !equivalent_fast_plans(first.plan, replay.plan)) {
                fail_equivalence(
                    fixture.variant,
                    case_number,
                    parent,
                    "deterministic plan replay",
                    reference,
                    replay);
            }
            observe(coverage, reference);
        }
    }

    require(coverage.cases == fixtures.size() * kCasesPerFixture,
            "corpus case count");
    require(coverage.aggressive != 0U && coverage.passive != 0U,
            "both intents not covered");
    require(coverage.accepted != 0U && coverage.rejected != 0U,
            "accept and reject outcomes not covered");
    require(coverage.partial != 0U, "partial allocation not covered");
    require(coverage.converted_allocations != 0U,
            "converted allocation not covered");
    require(
        coverage.exclusion_reasons[static_cast<std::size_t>(
            CandidateExclusionReason::AdminDown)] &&
        coverage.exclusion_reasons[static_cast<std::size_t>(
            CandidateExclusionReason::UnsupportedIntent)] &&
        coverage.exclusion_reasons[static_cast<std::size_t>(
            CandidateExclusionReason::MissingRuntimeState)] &&
        coverage.exclusion_reasons[static_cast<std::size_t>(
            CandidateExclusionReason::StaleBook)] &&
        coverage.exclusion_reasons[static_cast<std::size_t>(
            CandidateExclusionReason::RiskProhibited)] &&
        coverage.exclusion_reasons[static_cast<std::size_t>(
            CandidateExclusionReason::InexactPriceConversion)],
        "key exclusion reasons not covered");
}

[[nodiscard]] GeneratedViews all_live_views(
    const Fixture& fixture,
    const std::uint64_t case_number,
    const std::uint64_t now,
    const std::uint32_t normal_credits = 1'000U) {
    GeneratedViews result;
    const CandidateSet* const set = fixture.candidates->find(kInstrument);
    require(set != nullptr, "all-live candidate set absent");
    for (const CandidateHandle& candidate : set->candidates) {
        BookSlotView slot = make_view(
            candidate.venue,
            case_number,
            now,
            995 + static_cast<std::int64_t>(candidate.venue.value() * 2U),
            10U,
            10U,
            ViewMode::Live,
            false);
        slot.maximum_quote_age_ns = 1'000'000U;
        slot.normal_tx_credits = normal_credits;
        slot.throttle_credits = normal_credits;
        require(slot.valid(), "all-live slot invalid");
        result.reference.push_back(reference_view(slot, now));
        result.fast.push_back(slot);
    }
    return result;
}

[[nodiscard]] bool has_exclusion(
    const RouteDecision& decision,
    const VenueId venue,
    const CandidateExclusionReason reason) noexcept {
    return std::find(
               decision.exclusions.begin(),
               decision.exclusions.end(),
               CandidateExclusion{venue, reason}) != decision.exclusions.end();
}

[[nodiscard]] bool has_exclusion(
    const RoutePlan& plan,
    const VenueId venue,
    const CandidateExclusionReason reason) noexcept {
    for (const CandidateExclusion& exclusion : plan.candidate_exclusions()) {
        if (exclusion == CandidateExclusion{venue, reason}) {
            return true;
        }
    }
    return false;
}

void explicit_contract_difference_tests(const Fixture& fixture) {
    constexpr std::uint64_t now = 9'000'000U;
    const ParentOrder parent{
        ParentOrderId{90'001U},
        kInstrument,
        Side::Buy,
        Quantity{20U},
        Price{2'000},
        RoutingIntent::AggressiveIOC,
    };

    GeneratedViews no_credits = all_live_views(fixture, 90'001U, now, 0U);
    const RouteDecision credit_reference =
        reference_decision(parent, fixture, no_credits.reference);
    const PlanResult credit_fast =
        fixture.kernel->plan(parent, no_credits.fast, now);
    require(credit_reference.accepted(), "reference credit scenario not accepted");
    require(credit_fast.status == PlanStatus::RouteRejected,
            "fast kernel did not fail closed on zero credits");
    require(has_exclusion(
                credit_fast.plan,
                VenueId{1U},
                CandidateExclusionReason::EgressCongested),
            "zero credit was not an explicit egress exclusion");

    GeneratedViews stale = all_live_views(fixture, 90'002U, now);
    for (std::size_t index = 0U; index < stale.fast.size(); ++index) {
        stale.fast[index].monotonic_receive_time_ns = now - 1'000U;
        stale.fast[index].monotonic_publication_time_ns = now - 990U;
        stale.fast[index].maximum_quote_age_ns = 10U;
        stale.fast[index].book.local_receive_time_ns =
            stale.fast[index].monotonic_receive_time_ns;
        stale.fast[index].book.local_publication_time_ns =
            stale.fast[index].monotonic_publication_time_ns;
        require(stale.fast[index].valid(), "stale slot invalid");
        stale.reference[index] = reference_view(stale.fast[index], now);
        // This deliberately models the old caller claiming freshness despite
        // objective monotonic evidence to the contrary.
        stale.reference[index].feed_sufficiently_fresh = true;
    }
    ParentOrder stale_parent = parent;
    stale_parent.id = ParentOrderId{90'002U};
    const RouteDecision stale_reference =
        reference_decision(stale_parent, fixture, stale.reference);
    const PlanResult stale_fast =
        fixture.kernel->plan(stale_parent, stale.fast, now);
    require(stale_reference.accepted(), "reference age scenario not accepted");
    require(stale_fast.status == PlanStatus::RouteRejected,
            "fast kernel trusted stale caller health");
    require(has_exclusion(
                stale_fast.plan,
                VenueId{1U},
                CandidateExclusionReason::StaleBook),
            "objective age was not an explicit stale exclusion");

    GeneratedViews invalid_book = all_live_views(fixture, 90'003U, now);
    invalid_book.fast.resize(1U);
    invalid_book.reference.resize(1U);
    invalid_book.fast[0U].book.asks[0U].quantity = Quantity{};
    invalid_book.reference[0U].book = invalid_book.fast[0U].book;
    ParentOrder invalid_book_parent = parent;
    invalid_book_parent.id = ParentOrderId{90'003U};
    const RouteDecision invalid_reference = reference_decision(
        invalid_book_parent, fixture, invalid_book.reference);
    const PlanResult invalid_fast = fixture.kernel->plan(
        invalid_book_parent, invalid_book.fast, now);
    require(has_exclusion(
                invalid_reference,
                VenueId{1U},
                CandidateExclusionReason::InvalidBook),
            "reference did not classify invalid book");
    require(invalid_fast.status == PlanStatus::InvalidInput,
            "fast publication boundary accepted invalid book");
}

void invalid_parent_tests(const Fixture& fixture) {
    constexpr std::uint64_t now = 10'000'000U;
    const GeneratedViews views = all_live_views(fixture, 100'000U, now);
    const ParentOrder valid{
        ParentOrderId{100'001U},
        kInstrument,
        Side::Buy,
        Quantity{20U},
        Price{2'000},
        RoutingIntent::AggressiveIOC,
    };
    const auto check = [&](const ParentOrder& parent,
                           const CommandError expected_error,
                           const std::optional<RouteRejectReason> reference_reject) {
        const PlanResult fast = fixture.kernel->plan(parent, views.fast, now);
        require(fast.status == PlanStatus::HardRejected,
                "invalid parent was not hard rejected");
        require(fast.hard_error == expected_error,
                "invalid parent hard error mismatch");
        if (reference_reject.has_value()) {
            const RouteDecision reference =
                reference_decision(parent, fixture, views.reference);
            require(reference.reject_reason == *reference_reject,
                    "reference invalid-parent classification mismatch");
        }
    };

    ParentOrder changed = valid;
    changed.id = ParentOrderId{};
    check(changed, CommandError::InvalidCommand, RouteRejectReason::InvalidParent);
    changed = valid;
    changed.instrument = InstrumentId{};
    check(changed, CommandError::InvalidCommand, RouteRejectReason::InvalidParent);
    changed = valid;
    changed.quantity = Quantity{};
    check(changed, CommandError::InvalidCommand, RouteRejectReason::InvalidParent);
    changed = valid;
    changed.side = static_cast<Side>(99U);
    check(changed, CommandError::InvalidCommand, RouteRejectReason::InvalidParent);

    changed = valid;
    changed.id = ParentOrderId{100'002U};
    changed.limit_price = Price{};
    check(changed, CommandError::InvalidCommand, std::nullopt);
    changed = valid;
    changed.id = ParentOrderId{100'003U};
    changed.instrument = InstrumentId{999U};
    check(
        changed,
        CommandError::InstrumentProhibited,
        RouteRejectReason::InstrumentNotConfigured);
    changed = valid;
    changed.id = ParentOrderId{100'004U};
    changed.quantity = Quantity{1'001U};
    check(changed, CommandError::ParentQuantityLimit, std::nullopt);
    changed = valid;
    changed.id = ParentOrderId{100'005U};
    changed.limit_price = Price{5'001};
    check(changed, CommandError::PriceCollar, std::nullopt);
    changed = valid;
    changed.id = ParentOrderId{100'006U};
    changed.intent = static_cast<RoutingIntent>(99U);
    check(changed, CommandError::InvalidCommand, std::nullopt);
}

struct CommittedParent final {
    ParentOrderId parent{};
    std::vector<ChildAllocation> allocations;
};

void verify_counters(
    const RouteKernel& kernel,
    const Quantity instrument_quantity,
    const std::uint64_t open_parents,
    const std::uint64_t open_children,
    const std::array<Quantity, kVenueCount>& venue_quantities,
    const std::array<std::uint32_t, kVenueCount>& consumed_dispatch_credits) {
    const InstrumentRiskCounters instrument =
        kernel.instrument_counters(kInstrument);
    require(instrument.found, "instrument counter missing");
    require(instrument.gross_outstanding == instrument_quantity,
            "instrument outstanding conservation failure");
    require(instrument.open_parents == open_parents,
            "open-parent conservation failure");
    require(instrument.open_children == open_children,
            "open-child conservation failure");
    require(kernel.active_parent_count() == open_parents,
            "active parent table diverged from counters");
    for (std::size_t index = 0U; index < kVenueCount; ++index) {
        const VenueRiskCounters venue =
            kernel.venue_counters(VenueId{static_cast<std::uint32_t>(index + 1U)});
        require(venue.found, "venue counter missing");
        require(venue.outstanding == venue_quantities[index],
                "venue outstanding conservation failure");
        require(
            venue.reserved_normal_tx_credits ==
                consumed_dispatch_credits[index],
            "normal-credit consumption conservation failure");
        require(venue.reserved_cancel_tx_credits == 0U,
                "new order consumed reserved cancel capacity");
        require(
            venue.reserved_throttle_credits ==
                consumed_dispatch_credits[index],
            "throttle-credit consumption conservation failure");
    }
}

void commit_conservation_and_replay_tests(const Fixture& source_fixture) {
    RouteKernel kernel{
        source_fixture.configuration,
        *source_fixture.candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric};
    constexpr std::uint64_t base_now = 20'000'000U;
    GeneratedViews views =
        all_live_views(source_fixture, 200'000U, base_now, 1'000U);
    Quantity expected_instrument{};
    std::uint64_t expected_open_parents{};
    std::uint64_t expected_open_children{};
    std::array<Quantity, kVenueCount> expected_venues{
        Quantity{0U},
        Quantity{0U},
        Quantity{0U},
        Quantity{0U},
        Quantity{0U},
        Quantity{0U},
    };
    // Despite their legacy field names, these kernel counters represent
    // dispatch capacity consumed from the currently observed generation. A
    // child becoming terminal releases order/risk exposure, but must never
    // synthesize a venue credit. Only a newer venue generation can rebase it.
    std::array<std::uint32_t, kVenueCount> expected_consumed_credits{};
    std::vector<CommittedParent> committed;
    bool observed_multi_venue_parent{};

    constexpr std::size_t kCommitCount = 12U;
    for (std::size_t index = 0U; index < kCommitCount; ++index) {
        const std::uint64_t now = base_now + static_cast<std::uint64_t>(index);
        ParentOrder parent{
            ParentOrderId{200'001U + static_cast<std::uint64_t>(index)},
            kInstrument,
            index % 2U == 0U ? Side::Buy : Side::Sell,
            Quantity{45U + static_cast<std::uint64_t>(index % 5U)},
            index % 2U == 0U ? Price{2'000} : Price{1},
            RoutingIntent::AggressiveIOC,
        };
        const RouteDecision reference =
            reference_decision(parent, source_fixture, views.reference);
        const PlanResult first = kernel.plan(parent, views.fast, now);
        compare_shared_semantics(
            source_fixture,
            200'001U + static_cast<std::uint64_t>(index),
            parent,
            reference,
            first);
        require(first.accepted(), "commit corpus unexpectedly rejected");
        const PlanResult identical = kernel.plan(parent, views.fast, now);
        require(identical.accepted() &&
                    equivalent_fast_plans(first.plan, identical.plan),
                "pre-commit deterministic replay diverged");

        const CommitResult commit = kernel.commit(first.plan, views.fast, now + 1U);
        require(commit.committed(), "valid equivalent plan did not commit");
        const InstrumentRiskCounters after_first_commit =
            kernel.instrument_counters(kInstrument);
        const CommitResult replay_commit =
            kernel.commit(first.plan, views.fast, now + 1U);
        require(replay_commit.status == CommitStatus::StalePlan,
                "duplicate commit replay was not rejected");
        require(kernel.instrument_counters(kInstrument).gross_outstanding ==
                    after_first_commit.gross_outstanding,
                "duplicate commit replay mutated counters");

        CommittedParent record;
        record.parent = parent.id;
        observed_multi_venue_parent = observed_multi_venue_parent ||
            first.plan.allocation_count > 1U;
        for (const ChildAllocation& allocation : first.plan.child_allocations()) {
            record.allocations.push_back(allocation);
            expected_instrument = *checked_add(
                expected_instrument, allocation.quantity);
            const std::size_t venue_index = allocation.venue.value() - 1U;
            expected_venues[venue_index] = *checked_add(
                expected_venues[venue_index], allocation.quantity);
            ++expected_consumed_credits[venue_index];
            ++expected_open_children;
        }
        ++expected_open_parents;
        committed.push_back(std::move(record));
        verify_counters(
            kernel,
            expected_instrument,
            expected_open_parents,
            expected_open_children,
            expected_venues,
            expected_consumed_credits);
    }

    for (const CommittedParent& parent : committed) {
        for (std::size_t allocation_index = 0U;
             allocation_index < parent.allocations.size();
             ++allocation_index) {
            const ChildAllocation& allocation =
                parent.allocations[allocation_index];
            require(
                kernel.release_child(parent.parent, allocation.venue) ==
                    ReleaseStatus::Released,
                "committed child release failed");
            expected_instrument = *checked_subtract(
                expected_instrument, allocation.quantity);
            const std::size_t venue_index = allocation.venue.value() - 1U;
            expected_venues[venue_index] = *checked_subtract(
                expected_venues[venue_index], allocation.quantity);
            --expected_open_children;
            if (allocation_index + 1U == parent.allocations.size()) {
                --expected_open_parents;
            }
            verify_counters(
                kernel,
                expected_instrument,
                expected_open_parents,
                expected_open_children,
                expected_venues,
                expected_consumed_credits);
        }
        require(
            kernel.release_child(parent.parent, parent.allocations[0U].venue) ==
                ReleaseStatus::ParentNotFound,
            "released parent replay did not remain terminal");
    }
    require(expected_instrument == Quantity{} && expected_open_parents == 0U &&
                expected_open_children == 0U,
            "final conservation expectation not zero");
    require(observed_multi_venue_parent,
            "commit corpus did not exercise multiple child reservations");

    std::array<bool, kVenueCount> consumed_before_refresh{};
    for (std::size_t index = 0U; index < kVenueCount; ++index) {
        consumed_before_refresh[index] = expected_consumed_credits[index] != 0U;
    }
    require(
        std::count(
            consumed_before_refresh.begin(),
            consumed_before_refresh.end(),
            true) >= 2,
        "commit corpus did not consume credits at multiple venues");

    // Publish a newer credit/throttle generation one venue at a time. Planning
    // observes the new availability without mutating state. The successful
    // commit atomically rebases that venue's aggregate consumption to zero and
    // consumes exactly one credit for the newly authorized dispatch.
    for (std::size_t index = 0U; index < kVenueCount; ++index) {
        if (!consumed_before_refresh[index]) {
            continue;
        }
        const std::uint64_t now =
            base_now + 10'000U + static_cast<std::uint64_t>(index);
        BookSlotView refreshed = views.fast[index];
        refreshed.tx_credit_generation += 100U;
        refreshed.throttle_generation += 100U;
        refreshed.normal_tx_credits = 32U;
        refreshed.throttle_credits = 32U;
        require(refreshed.valid(), "refreshed generation slot invalid");
        std::array<BookSlotView, 1U> fast_view{refreshed};
        std::array<CandidateRuntimeView, 1U> golden_view{
            reference_view(refreshed, now)};
        ParentOrder parent{
            ParentOrderId{300'001U + static_cast<std::uint64_t>(index)},
            kInstrument,
            Side::Buy,
            Quantity{5U},
            Price{2'000},
            RoutingIntent::AggressiveIOC,
        };
        const RouteDecision reference = reference_decision(
            parent, source_fixture, golden_view);
        const PlanResult plan = kernel.plan(parent, fast_view, now);
        compare_shared_semantics(
            source_fixture,
            parent.id.value(),
            parent,
            reference,
            plan);
        require(plan.accepted() && plan.plan.allocation_count == 1U &&
                    plan.plan.allocations[0U].venue ==
                        VenueId{static_cast<std::uint32_t>(index + 1U)},
                "refreshed generation did not target expected venue");

        const std::uint32_t old_consumption =
            expected_consumed_credits[index];
        require(old_consumption != 0U,
                "refresh scenario lacked old-generation consumption");
        const VenueRiskCounters before_refresh_commit = kernel.venue_counters(
            VenueId{static_cast<std::uint32_t>(index + 1U)});
        require(
            before_refresh_commit.reserved_normal_tx_credits ==
                    old_consumption &&
                before_refresh_commit.reserved_throttle_credits ==
                    old_consumption,
            "planning mutated old-generation credit consumption");
        const CommitResult committed_refresh =
            kernel.commit(plan.plan, fast_view, now + 1U);
        require(committed_refresh.committed(),
                "new-generation dispatch did not commit");
        expected_consumed_credits[index] = 1U;
        expected_instrument = *checked_add(
            expected_instrument, plan.plan.allocations[0U].quantity);
        expected_venues[index] = *checked_add(
            expected_venues[index], plan.plan.allocations[0U].quantity);
        expected_open_parents = 1U;
        expected_open_children = 1U;
        verify_counters(
            kernel,
            expected_instrument,
            expected_open_parents,
            expected_open_children,
            expected_venues,
            expected_consumed_credits);
        require(
            kernel.release_child(parent.id, plan.plan.allocations[0U].venue) ==
                ReleaseStatus::Released,
            "new-generation child release failed");
        expected_instrument = *checked_subtract(
            expected_instrument, plan.plan.allocations[0U].quantity);
        expected_venues[index] = *checked_subtract(
            expected_venues[index], plan.plan.allocations[0U].quantity);
        expected_open_parents = 0U;
        expected_open_children = 0U;
        verify_counters(
            kernel,
            expected_instrument,
            expected_open_parents,
            expected_open_children,
            expected_venues,
            expected_consumed_credits);
    }
}

}  // namespace

int main() {
    std::vector<Fixture> fixtures;
    fixtures.reserve(4U);
    for (std::size_t variant = 0U; variant < 4U; ++variant) {
        fixtures.push_back(make_fixture(variant));
    }
    deterministic_corpus_tests(fixtures);
    explicit_contract_difference_tests(fixtures[0U]);
    invalid_parent_tests(fixtures[0U]);
    commit_conservation_and_replay_tests(fixtures[0U]);
    return 0;
}
