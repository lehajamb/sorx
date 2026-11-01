#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/dataplane/route_kernel.hpp"
#include "sorx/fastpath/dispatch_pipeline.hpp"
#include "sorx/policy/adaptive_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace {

std::atomic<std::uint64_t> allocation_calls{};

}  // namespace

void* operator new(const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void operator delete(void* const value) noexcept {
    std::free(value);
}

void operator delete[](void* const value) noexcept {
    std::free(value);
}

void operator delete(void* const value, const std::size_t) noexcept {
    std::free(value);
}

void operator delete[](void* const value, const std::size_t) noexcept {
    std::free(value);
}

namespace {

using namespace sorx;
using namespace sorx::dataplane;
using namespace sorx::fastpath;
using namespace sorx::policy;

constexpr VenueId kVenueOne{1U};
constexpr VenueId kVenueTwo{2U};
constexpr InstrumentId kInstrument{9U};
constexpr std::uint64_t kDecisionTimeNs = 1'050U;
constexpr std::uint64_t kDecisionUtcNs = 500U;
constexpr std::uint64_t kLeadershipEpoch = 7U;
constexpr std::uint64_t kConfigurationGeneration = 13U;

[[nodiscard]] RouteConfig route(
    const VenueId venue,
    const std::uint32_t priority) {
    RouteConfig output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.venue_name = venue == kVenueOne ? "VENUE_ONE" : "VENUE_TWO";
    output.canonical_symbol = "TEST";
    output.wire_symbol = venue == kVenueOne ? "TEST1" : "TEST2";
    output.capabilities = VenueCapabilities{true, true};
    output.allocation_cap = Quantity{100U};
    output.aggressive_priority = priority;
    output.passive_priority = priority;
    output.configured_latency_ns = 100U;
    output.admin_state = AdminState::Up;
    return output;
}

[[nodiscard]] std::shared_ptr<const CandidateTable> candidate_table() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(
        std::vector<RouteConfig>{route(kVenueOne, 1U), route(kVenueTwo, 2U)},
        errors));
    assert(errors.empty());
    FibCompiler compiler;
    return compiler.compile(rib);
}

[[nodiscard]] CoreConfiguration configuration() {
    CoreConfiguration output;
    output.maximum_aggressive_venues = 1U;
    output.risk.generation = 17U;
    InstrumentRiskLimit instrument;
    instrument.instrument = kInstrument;
    instrument.maximum_parent_quantity = Quantity{1'000U};
    instrument.maximum_child_quantity = Quantity{1'000U};
    instrument.maximum_parent_notional = Notional{1'000'000'000U};
    instrument.maximum_child_notional = Notional{1'000'000'000U};
    instrument.minimum_price = Price{1};
    instrument.maximum_price = Price{1'000'000};
    instrument.maximum_open_parents = 100U;
    instrument.maximum_open_children = 100U;
    instrument.maximum_gross_outstanding = Quantity{10'000U};
    output.risk.instruments.push_back(instrument);
    output.risk.venues.push_back(
        VenueRiskLimit{kVenueOne, true, Quantity{10'000U}});
    output.risk.venues.push_back(
        VenueRiskLimit{kVenueTwo, true, Quantity{10'000U}});
    return output;
}

[[nodiscard]] BookSlotView view(const VenueId venue) noexcept {
    md::BookView book;
    book.venue = venue;
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = kInstrument;
    assert(book.push_bid(md::Level{Price{99}, Quantity{10U}, 1U}));
    assert(book.push_ask(md::Level{Price{100}, Quantity{10U}, 1U}));
    book.feed_session_epoch = 3U;
    book.recovery_generation = 1U;
    book.view_generation = 4U;
    book.last_applied_sequence = 10U;
    book.next_sequence = 11U;
    book.exchange_timestamp_ns = 900;
    book.local_receive_time_ns = 1'000U;
    book.local_publication_time_ns = 1'010U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.valid());

    BookSlotView output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.adjacency_generation = 9U;
    output.book = book;
    output.market_data_state = MarketDataState::Live;
    output.order_entry_state = OrderEntryState::Ready;
    output.egress_state = EgressState::Ready;
    output.instrument_state = InstrumentState::Trading;
    output.feed_epoch = book.feed_session_epoch;
    output.source_health_generation = 5U;
    output.monotonic_receive_time_ns = book.local_receive_time_ns;
    output.monotonic_publication_time_ns = book.local_publication_time_ns;
    output.maximum_quote_age_ns = 100U;
    output.tx_credit_generation = 6U;
    output.throttle_generation = 7U;
    output.normal_tx_credits = 8U;
    output.cancel_reserved_tx_credits = 2U;
    output.throttle_credits = 4U;
    output.has_book = true;
    output.source_healthy = true;
    output.conversion_compatible = true;
    output.risk_allowed = true;
    assert(output.valid());
    return output;
}

[[nodiscard]] std::array<BookSlotView, 2U> views() noexcept {
    return {view(kVenueOne), view(kVenueTwo)};
}

[[nodiscard]] ParentOrder parent(const std::uint64_t id = 1U) noexcept {
    return ParentOrder{
        ParentOrderId{id},
        kInstrument,
        Side::Buy,
        Quantity{5U},
        Price{100},
        RoutingIntent::AggressiveIOC,
    };
}

[[nodiscard]] PolicySnapshot policy_snapshot() noexcept {
    PolicySnapshot output;
    output.policy_generation = 10U;
    output.fee_generation = 20U;
    output.metric_generation = 30U;
    output.weight_generation = 40U;
    output.compliance_generation = 50U;
    output.maximum_metric_latency_ns = 1'000U;
    output.weights = PolicyWeights{1U, 1U, 1U, 1U, 1U, 1U};
    output.compliance = ComplianceRulePack{
        611U,
        3U,
        50U,
        100U,
        1'000U,
        ProtectedPriceMode::EnforceBestProtectedPrice,
        true,
        true,
    };
    assert(output.structurally_valid());
    return output;
}

[[nodiscard]] PolicyOrder policy_order(const ParentOrder& order) noexcept {
    return PolicyOrder{
        order.id,
        order.instrument,
        order.side,
        order.intent,
        order.quantity,
        order.limit_price,
        kDecisionTimeNs,
        kDecisionUtcNs,
    };
}

[[nodiscard]] VenueRoutingFacts facts(
    const BookSlotView& source,
    const std::uint64_t latency_ns) noexcept {
    VenueRoutingFacts output;
    output.venue = source.venue;
    output.instrument = source.instrument;
    output.versions = VenueFactVersions{
        source.adjacency_generation,
        source.book.view_generation,
        1U,
        17U,
        source.throttle_generation,
        source.tx_credit_generation,
        1U,
        20U,
        30U,
    };
    output.capabilities = AdaptiveVenueCapabilities{true, true, true};
    output.admin_state = AdminState::Up;
    output.trading_state = source.instrument_state;
    output.feed_state = source.market_data_state;
    output.order_entry_state = source.order_entry_state;
    output.egress_state = source.egress_state;
    output.permission_allowed = true;
    output.feed_healthy = source.source_healthy;
    output.risk_allowed = true;
    output.protected_access_allowed = true;
    output.post_only_eligible = true;
    output.local_opposite_present = true;
    output.quote_age_ns =
        kDecisionTimeNs - source.monotonic_receive_time_ns;
    output.maximum_quote_age_ns = source.maximum_quote_age_ns;
    output.throttle_credits = source.throttle_credits;
    output.normal_tx_credits = source.normal_tx_credits;
    output.executable_price = Price{100};
    output.available_depth = Quantity{10U};
    output.local_opposite_price = Price{100};
    output.protected_quote = ProtectedQuoteFacts{true, true, Price{100}};
    output.fees = VenueFees{0, 0};
    output.metrics = BoundedVenueMetrics{500'000U, 10U, 0U, latency_ns};
    output.aggressive_priority = source.venue == kVenueOne ? 1U : 2U;
    output.passive_priority = output.aggressive_priority;
    return output;
}

[[nodiscard]] PolicyDecision apply_policy(
    const ParentOrder& order,
    std::array<BookSlotView, 2U>& target,
    const bool exclude_venue_one = false) noexcept {
    std::array candidate_facts{
        facts(target[0U], 100U), facts(target[1U], 1U)};
    if (exclude_venue_one) {
        candidate_facts[0U].risk_allowed = false;
    }
    const AdaptivePolicyKernel policy{policy_snapshot()};
    const PolicyDecision decision =
        policy.evaluate(policy_order(order), candidate_facts);
    assert(decision.accepted());
    const AdaptiveOverlayApplyResult applied =
        apply_adaptive_policy_decision(decision, target);
    assert(applied.applied());
    assert(applied.applied_count == target.size());
    return decision;
}

[[nodiscard]] const DataplaneCandidatePin& pin_for(
    const RoutePlan& plan,
    const VenueId venue) {
    for (const DataplaneCandidatePin& pin : plan.pins()) {
        if (pin.market.venue == venue) {
            return pin;
        }
    }
    assert(false);
    return plan.candidate_pins[0U];
}

void test_optional_mode_preserves_legacy_and_required_fails_closed() {
    const auto table = candidate_table();
    const CoreConfiguration config = configuration();
    const auto current_views = views();
    const ParentOrder order = parent();

    RouteKernel optional{
        config,
        *table,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric};
    assert(optional.adaptive_policy_mode() == AdaptivePolicyMode::Optional);
    const PlanResult legacy =
        optional.plan(order, current_views, kDecisionTimeNs);
    assert(legacy.accepted());
    assert(legacy.plan.allocations[0U].venue == kVenueOne);
    for (const DataplaneCandidatePin& pin : legacy.plan.pins()) {
        assert(!pin.adaptive_policy.applied);
    }

    RouteKernel required{
        config, *table, AdaptivePolicyMode::Required,
        WireIdentityMode::ReferenceGeneric};
    assert(required.adaptive_policy_mode() == AdaptivePolicyMode::Required);
    const PlanResult rejected =
        required.plan(order, current_views, kDecisionTimeNs);
    assert(rejected.status == PlanStatus::AdaptivePolicyRejected);
    assert(!rejected.accepted());
}

void test_dynamic_rank_overrides_static_tie_and_hard_exclusion() {
    const auto table = candidate_table();
    const CoreConfiguration config = configuration();
    const ParentOrder order = parent();
    {
        auto ranked_views = views();
        const PolicyDecision decision = apply_policy(order, ranked_views);
        assert(decision.ranked[0U].venue == kVenueTwo);
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        const std::uint64_t before =
            allocation_calls.load(std::memory_order_relaxed);
        const PlanResult planned =
            kernel.plan(order, ranked_views, kDecisionTimeNs);
        const std::uint64_t after =
            allocation_calls.load(std::memory_order_relaxed);
        assert(after == before);
        assert(planned.accepted());
        assert(planned.plan.allocations[0U].venue == kVenueTwo);
        assert(pin_for(planned.plan, kVenueTwo).adaptive_policy.rank == 0U);
        assert(pin_for(planned.plan, kVenueOne).adaptive_policy.rank == 1U);
    }
    {
        auto ranked_views = views();
        const PolicyDecision decision = apply_policy(order, ranked_views, true);
        assert(decision.exclusion_count == 1U);
        assert(decision.exclusions[0U].venue == kVenueOne);
        assert(decision.exclusions[0U].reason ==
               PolicyExclusionReason::RiskDenied);
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        const PlanResult planned =
            kernel.plan(order, ranked_views, kDecisionTimeNs);
        assert(planned.accepted());
        assert(planned.plan.allocations[0U].venue == kVenueTwo);
        const AdaptivePolicyOverlay& excluded =
            pin_for(planned.plan, kVenueOne).adaptive_policy;
        assert(excluded.applied);
        assert(!excluded.eligible);
        assert(excluded.exclusion_reason == PolicyExclusionReason::RiskDenied);
    }
}

void test_partial_or_inconsistent_overlay_fails_closed() {
    const auto table = candidate_table();
    const CoreConfiguration config = configuration();
    const ParentOrder order = parent();
    auto ranked_views = views();
    (void)apply_policy(order, ranked_views);
    {
        auto partial = ranked_views;
        partial[1U].adaptive_policy = {};
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        assert(kernel.plan(order, partial, kDecisionTimeNs).status ==
               PlanStatus::AdaptivePolicyRejected);
    }
    {
        auto inconsistent = ranked_views;
        ++inconsistent[1U].adaptive_policy.versions.weight_generation;
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        assert(kernel.plan(order, inconsistent, kDecisionTimeNs).status ==
               PlanStatus::AdaptivePolicyRejected);
    }
    {
        auto inconsistent_score = ranked_views;
        inconsistent_score[0U].adaptive_policy.evidence.score.total = 1'000;
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        assert(kernel.plan(order, inconsistent_score, kDecisionTimeNs).status ==
               PlanStatus::AdaptivePolicyRejected);
    }
    {
        auto replayed_for_other_parent = ranked_views;
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        assert(kernel.plan(parent(2U), replayed_for_other_parent, kDecisionTimeNs).status ==
               PlanStatus::AdaptivePolicyRejected);
    }
}

void test_commit_rejects_version_and_score_tamper() {
    const auto table = candidate_table();
    const CoreConfiguration config = configuration();
    const ParentOrder order = parent();
    auto ranked_views = views();
    (void)apply_policy(order, ranked_views);
    {
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        const PlanResult planned =
            kernel.plan(order, ranked_views, kDecisionTimeNs);
        assert(planned.accepted());
        auto changed = ranked_views;
        ++changed[0U].adaptive_policy.versions.weight_generation;
        const CommitResult committed =
            kernel.commit(planned.plan, changed, kDecisionTimeNs + 10U);
        assert(committed.status == CommitStatus::AdaptivePolicyMismatch);
    }
    {
        RouteKernel kernel{
            config, *table, AdaptivePolicyMode::Required,
            WireIdentityMode::ReferenceGeneric};
        const PlanResult planned =
            kernel.plan(order, ranked_views, kDecisionTimeNs);
        assert(planned.accepted());
        auto changed = ranked_views;
        ++changed[0U].adaptive_policy.evidence.score.total;
        const CommitResult committed =
            kernel.commit(planned.plan, changed, kDecisionTimeNs + 10U);
        assert(committed.status == CommitStatus::AdaptivePolicyMismatch);
    }
}

void test_deterministic_replay_with_overlay() {
    const auto table = candidate_table();
    const CoreConfiguration config = configuration();
    const ParentOrder order = parent();
    auto ranked_views = views();
    (void)apply_policy(order, ranked_views);
    RouteKernel first{
        config, *table, AdaptivePolicyMode::Required,
        WireIdentityMode::ReferenceGeneric};
    RouteKernel second{
        config, *table, AdaptivePolicyMode::Required,
        WireIdentityMode::ReferenceGeneric};
    const PlanResult first_plan =
        first.plan(order, ranked_views, kDecisionTimeNs);
    std::reverse(ranked_views.begin(), ranked_views.end());
    const PlanResult replay =
        second.plan(order, ranked_views, kDecisionTimeNs);
    assert(first_plan.accepted());
    assert(replay.accepted());
    assert(first_plan.plan.allocations[0U] == replay.plan.allocations[0U]);
    assert(first_plan.plan.planned_children[0U] ==
           replay.plan.planned_children[0U]);
    assert(first_plan.plan.candidate_pin_count ==
           replay.plan.candidate_pin_count);
    for (std::size_t index{}; index < first_plan.plan.candidate_pin_count; ++index) {
        assert(first_plan.plan.candidate_pins[index] ==
               replay.plan.candidate_pins[index]);
    }
}

class DeterministicEncoder final : public IChildFrameEncoder {
public:
    [[nodiscard]] FrameEncodeResult encode_new(
        const ParentOrder&,
        const ChildAllocation& allocation,
        const PlannedChild& child,
        const std::span<std::byte> destination) noexcept override {
        if (destination.size() < 3U || !static_cast<bool>(allocation.venue) ||
            !static_cast<bool>(child.child_order_id)) {
            return {FrameEncodeStatus::InvalidInput, 0U};
        }
        destination[0U] = std::byte{'N'};
        destination[1U] = static_cast<std::byte>(allocation.venue.value());
        destination[2U] = static_cast<std::byte>(child.dispatch_attempt_id);
        return {FrameEncodeStatus::Encoded, 3U};
    }
};

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
        std::to_integer<std::uint16_t>(bytes[offset + 1U]));
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index{}; index < 4U; ++index) {
        value = (value << 8U) |
                std::to_integer<std::uint32_t>(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t index{}; index < 8U; ++index) {
        value = (value << 8U) |
                std::to_integer<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

void test_dispatch_record_contains_exact_policy_pin_rank_and_score() {
    const auto table = candidate_table();
    const CoreConfiguration config = configuration();
    const ParentOrder order = parent();
    auto ranked_views = views();
    (void)apply_policy(order, ranked_views);

    RouteKernel kernel{
        config, *table, AdaptivePolicyMode::Required,
        WireIdentityMode::ReferenceGeneric};
    recorder::InMemoryMirroredTier0 tier0({32U, 4U, 4U, false});
    VenueTxQueues venue_one;
    VenueTxQueues venue_two;
    DeterministicEncoder encoder_one;
    DeterministicEncoder encoder_two;
    FixedVenueRegistry registry;
    assert(registry.register_venue(kVenueOne, venue_one, encoder_one) ==
           VenueRegistrationStatus::Registered);
    assert(registry.register_venue(kVenueTwo, venue_two, encoder_two) ==
           VenueRegistrationStatus::Registered);
    assert(registry.seal());
    DispatchPipeline pipeline(
        kernel,
        tier0,
        registry,
        DispatchPipelinePolicy{
            kLeadershipEpoch,
            kConfigurationGeneration,
            recorder::DurabilityDomain::VolatileSoftwareReference});
    const DispatchPipelineResult dispatched = pipeline.try_dispatch(
        order,
        ranked_views,
        DispatchRequestMetadata{91U, kDecisionTimeNs, 1'060U, 1'055U});
    assert(dispatched.dispatched());

    recorder::DurableBatch batch;
    recorder::DurableToken token;
    assert(tier0.read(dispatched.token.batch_id, batch, token) ==
           recorder::ReadStatus::Ok);
    const auto record = batch.record(1U);
    assert(record.has_value());
    assert(record->type ==
           static_cast<std::uint16_t>(DispatchRecordType::PinnedRoutingInputs));
    assert(record->version == kDispatchRecordVersion);
    const std::span<const std::byte> payload = record->payload;
    assert(payload.size() > 445U);
    assert(read_u16(payload, 0U) == 2U);
    const VenueId serialized_venue{read_u32(payload, 2U)};
    assert(serialized_venue == kVenueOne || serialized_venue == kVenueTwo);
    assert(std::to_integer<std::uint8_t>(payload[107U]) == 1U);
    assert(std::to_integer<std::uint8_t>(payload[108U]) == 1U);
    const std::uint16_t expected_rank =
        serialized_venue == kVenueTwo ? 0U : 1U;
    assert(read_u16(payload, 109U) == expected_rank);
    assert(std::to_integer<std::uint8_t>(payload[111U]) ==
           static_cast<std::uint8_t>(PolicyExclusionReason::None));
    assert(read_u64(payload, 112U) == 10U);
    assert(read_u64(payload, 120U) == 20U);
    assert(read_u64(payload, 128U) == 30U);
    assert(read_u64(payload, 136U) == 40U);
    assert(read_u64(payload, 144U) == 50U);
    assert(read_u64(payload, 152U) == 611U);
    assert(read_u64(payload, 160U) == 3U);
    assert(read_u64(payload, 184U) == order.id.value());
    assert(read_u32(payload, 230U) == serialized_venue.value());
    assert(read_u64(payload, 234U) == 9U);
    assert(read_u32(payload, 306U) == serialized_venue.value());
    assert(read_u64(payload, 310U) == 100U);
    assert(read_u64(payload, 318U) == 10U);
    const std::int64_t expected_score =
        serialized_venue == kVenueTwo ? 9 : -90;
    assert(read_u64(payload, 432U) ==
           static_cast<std::uint64_t>(expected_score));
}

}  // namespace

int main() {
    test_optional_mode_preserves_legacy_and_required_fails_closed();
    test_dynamic_rank_overrides_static_tie_and_hard_exclusion();
    test_partial_or_inconsistent_overlay_fails_closed();
    test_commit_rejects_version_and_score_tamper();
    test_deterministic_replay_with_overlay();
    test_dispatch_record_contains_exact_policy_pin_rank_and_score();
    return 0;
}
