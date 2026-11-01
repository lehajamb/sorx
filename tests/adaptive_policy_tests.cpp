#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/policy/adaptive_policy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

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
using namespace sorx::policy;

constexpr InstrumentId kInstrument{9U};

[[nodiscard]] PolicySnapshot snapshot() noexcept {
    PolicySnapshot value;
    value.policy_generation = 10U;
    value.fee_generation = 20U;
    value.metric_generation = 30U;
    value.weight_generation = 40U;
    value.compliance_generation = 50U;
    value.maximum_metric_latency_ns = 1'000'000U;
    value.weights = PolicyWeights{2U, 3U, 5U, 7U, 11U, 13U};
    value.compliance = ComplianceRulePack{
        611U,
        3U,
        50U,
        100U,
        1'000U,
        ProtectedPriceMode::EnforceBestProtectedPrice,
        true,
        true,
    };
    assert(value.structurally_valid());
    return value;
}

[[nodiscard]] PolicyOrder aggressive_order(
    const Side side = Side::Buy) noexcept {
    return PolicyOrder{
        ParentOrderId{77U},
        kInstrument,
        side,
        RoutingIntent::AggressiveIOC,
        Quantity{1'000U},
        Price{side == Side::Buy ? 101 : 99},
        5'000U,
        500U,
    };
}

[[nodiscard]] PolicyOrder passive_order() noexcept {
    return PolicyOrder{
        ParentOrderId{77U},
        kInstrument,
        Side::Buy,
        RoutingIntent::PassiveGTC,
        Quantity{1'000U},
        Price{99},
        5'000U,
        500U,
    };
}

[[nodiscard]] VenueRoutingFacts aggressive_facts(
    const std::uint32_t venue = 1U) noexcept {
    VenueRoutingFacts value;
    value.venue = VenueId{venue};
    value.instrument = kInstrument;
    value.versions = VenueFactVersions{
        101U,
        102U,
        103U,
        104U,
        105U,
        106U,
        107U,
        20U,
        30U,
    };
    value.capabilities = AdaptiveVenueCapabilities{true, true, true};
    value.admin_state = AdminState::Up;
    value.trading_state = InstrumentState::Trading;
    value.feed_state = MarketDataState::Live;
    value.order_entry_state = OrderEntryState::Ready;
    value.egress_state = EgressState::Ready;
    value.permission_allowed = true;
    value.feed_healthy = true;
    value.risk_allowed = true;
    value.protected_access_allowed = true;
    value.post_only_eligible = true;
    value.local_opposite_present = true;
    value.quote_age_ns = 10U;
    value.maximum_quote_age_ns = 100U;
    value.throttle_credits = 4U;
    value.normal_tx_credits = 8U;
    value.executable_price = Price{100};
    value.available_depth = Quantity{10U};
    value.local_opposite_price = Price{100};
    value.protected_quote = ProtectedQuoteFacts{true, true, Price{100}};
    value.fees = VenueFees{-4, 4};
    value.metrics = BoundedVenueMetrics{10U, 2U, 3U, 2U};
    value.aggressive_priority = 10U;
    value.passive_priority = 20U;
    return value;
}

[[nodiscard]] VenueRoutingFacts passive_facts(
    const std::uint32_t venue = 1U) noexcept {
    VenueRoutingFacts value = aggressive_facts(venue);
    value.executable_price = Price{99};
    value.local_opposite_price = Price{100};
    return value;
}

void expect_exclusion(
    const PolicySnapshot& policy_snapshot,
    const PolicyOrder& order,
    const VenueRoutingFacts& facts,
    const PolicyExclusionReason expected) {
    const AdaptivePolicyKernel kernel{policy_snapshot};
    const std::array candidates{facts};
    const PolicyDecision result = kernel.evaluate(order, candidates);
    assert(result.status == PolicyEvaluationStatus::NoEligibleCandidate);
    assert(result.ranked_count == 0U);
    assert(result.exclusion_count == 1U);
    assert(result.exclusions[0U].venue == facts.venue);
    assert(result.exclusions[0U].reason == expected);
    assert(result.exclusions[0U].pin.versions == facts.versions);
}

void test_version_pins_and_exact_score_components() {
    const PolicySnapshot configured = snapshot();
    const AdaptivePolicyKernel kernel{configured};
    const VenueRoutingFacts facts = aggressive_facts();
    const std::array candidates{facts};

    const std::uint64_t before =
        allocation_calls.load(std::memory_order_relaxed);
    const PolicyDecision result = kernel.evaluate(aggressive_order(), candidates);
    const std::uint64_t after =
        allocation_calls.load(std::memory_order_relaxed);

    assert(after == before);
    assert(result.accepted());
    assert(result.ranked_count == 1U);
    assert(result.exclusion_count == 0U);
    const PolicyVersionPin expected{
        10U, 20U, 30U, 40U, 50U, 611U, 3U, 100U, 1'000U};
    assert(result.versions == expected);
    const VenueFactPin expected_fact_pin{facts.venue, facts.versions};
    assert(result.ranked[0U].pin == expected_fact_pin);
    assert(result.ranked[0U].quote_age_ns == facts.quote_age_ns);
    assert(result.ranked[0U].maximum_quote_age_ns ==
           facts.maximum_quote_age_ns);
    assert(result.ranked[0U].throttle_credits == facts.throttle_credits);
    assert(result.ranked[0U].normal_tx_credits == facts.normal_tx_credits);
    assert(result.ranked[0U].protected_quote == facts.protected_quote);
    assert(result.ranked[0U].fee_ppm_used == 4);
    assert(result.ranked[0U].score.depth_reward == 20);
    assert(result.ranked[0U].score.fee_economics == -12);
    assert(result.ranked[0U].score.latency_penalty == -22);
    assert(result.ranked[0U].score.reject_penalty == -39);
    assert(result.ranked[0U].score.total == -53);
    assert(!result.ranked[0U].score.saturated);

    const VenueRoutingFacts passive = passive_facts();
    const std::array passive_candidates{passive};
    const PolicyDecision passive_result =
        kernel.evaluate(passive_order(), passive_candidates);
    assert(passive_result.accepted());
    assert(passive_result.ranked[0U].fee_ppm_used == -4);
    assert(passive_result.ranked[0U].score.fee_economics == 12);
    assert(passive_result.ranked[0U].score.fill_quality_reward == 50);
    assert(passive_result.ranked[0U].score.adverse_selection_penalty == -14);
    assert(passive_result.ranked[0U].score.latency_penalty == -22);
    assert(passive_result.ranked[0U].score.reject_penalty == -39);
    assert(passive_result.ranked[0U].score.total == -13);
}

void test_all_common_hard_gates() {
    const PolicySnapshot configured = snapshot();
    const PolicyOrder order = aggressive_order();

    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.venue = VenueId{};
        expect_exclusion(configured, order, facts, PolicyExclusionReason::InvalidFacts);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.instrument = InstrumentId{99U};
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InstrumentMismatch);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.versions.route_generation = 0U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::RouteVersionMissing);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.versions.fee_generation = 19U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::FeeGenerationMismatch);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.versions.metric_generation = 29U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::MetricGenerationMismatch);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.capabilities.aggressive_ioc = false;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::UnsupportedIntent);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.permission_allowed = false;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::PermissionDenied);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.admin_state = AdminState::Down;
        expect_exclusion(configured, order, facts, PolicyExclusionReason::AdminDown);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.trading_state = InstrumentState::Halted;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InstrumentNotTrading);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.feed_state = MarketDataState::Recovering;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::FeedNotLive);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.feed_healthy = false;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::FeedUnhealthy);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.maximum_quote_age_ns = 0U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InvalidQuoteAge);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.quote_age_ns = 101U;
        expect_exclusion(configured, order, facts, PolicyExclusionReason::QuoteStale);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.order_entry_state = OrderEntryState::Degraded;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::OrderEntryNotReady);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.egress_state = EgressState::Congested;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::EgressUnavailable);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.risk_allowed = false;
        expect_exclusion(configured, order, facts, PolicyExclusionReason::RiskDenied);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.throttle_credits = 0U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::ThrottleUnavailable);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.normal_tx_credits = 0U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::TxCreditUnavailable);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.fees.taker_fee_ppm = 1'000'001;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InvalidEconomics);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.metrics.fill_quality_ppm = 1'000'001U;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InvalidMetrics);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.executable_price = Price{0};
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InvalidExecutablePrice);
    }
}

void test_aggressive_and_protected_price_hard_gates() {
    const PolicySnapshot configured = snapshot();
    const PolicyOrder order = aggressive_order();
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.executable_price = Price{102};
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::PriceOutsideLimit);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.available_depth = Quantity{0U};
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::NoExecutableDepth);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.versions.protected_quote_generation = 0U;
        expect_exclusion(
            configured,
            order,
            facts,
            PolicyExclusionReason::ProtectedQuoteVersionMissing);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.protected_quote.view_healthy = false;
        expect_exclusion(
            configured,
            order,
            facts,
            PolicyExclusionReason::ProtectedQuoteUnavailable);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        facts.protected_access_allowed = false;
        expect_exclusion(
            configured,
            order,
            facts,
            PolicyExclusionReason::ProtectedAccessDenied);
    }
    {
        VenueRoutingFacts facts = aggressive_facts();
        PolicyOrder permissive_limit = order;
        permissive_limit.limit_price = Price{102};
        facts.executable_price = Price{101};
        expect_exclusion(
            configured,
            permissive_limit,
            facts,
            PolicyExclusionReason::ProtectedPriceViolation);
    }
    {
        const AdaptivePolicyKernel kernel{configured};
        std::array candidates{aggressive_facts(1U), aggressive_facts(2U)};
        candidates[1U].protected_quote.best_opposite_price = Price{99};
        const PolicyDecision result = kernel.evaluate(order, candidates);
        assert(result.status == PolicyEvaluationStatus::NoEligibleCandidate);
        assert(result.exclusion_count == 2U);
        assert(result.exclusions[0U].reason ==
               PolicyExclusionReason::ProtectedQuoteInconsistent);
        assert(result.exclusions[1U].reason ==
               PolicyExclusionReason::ProtectedQuoteInconsistent);
    }
}

void test_passive_post_only_and_non_marketable_gates() {
    const PolicySnapshot configured = snapshot();
    const PolicyOrder order = passive_order();
    {
        VenueRoutingFacts facts = passive_facts();
        facts.capabilities.post_only = false;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::PostOnlyUnsupported);
    }
    {
        VenueRoutingFacts facts = passive_facts();
        facts.post_only_eligible = false;
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::PostOnlyIneligible);
    }
    {
        VenueRoutingFacts facts = passive_facts();
        facts.executable_price = Price{98};
        expect_exclusion(
            configured, order, facts, PolicyExclusionReason::InvalidExecutablePrice);
    }
    {
        VenueRoutingFacts facts = passive_facts();
        facts.local_opposite_price = Price{99};
        expect_exclusion(
            configured,
            order,
            facts,
            PolicyExclusionReason::PassiveMarketableLocal);
    }
    {
        VenueRoutingFacts facts = passive_facts();
        facts.local_opposite_present = false;
        facts.protected_quote.best_opposite_price = Price{99};
        expect_exclusion(
            configured,
            order,
            facts,
            PolicyExclusionReason::PassiveMarketableProtected);
    }
}

void test_duplicate_invalid_snapshot_order_and_capacity_fail_closed() {
    const PolicySnapshot configured = snapshot();
    const PolicyOrder order = aggressive_order();
    {
        const AdaptivePolicyKernel kernel{configured};
        const std::array duplicates{
            aggressive_facts(1U), aggressive_facts(1U)};
        const PolicyDecision result = kernel.evaluate(order, duplicates);
        assert(result.status == PolicyEvaluationStatus::NoEligibleCandidate);
        assert(result.exclusion_count == 2U);
        assert(result.exclusions[0U].reason ==
               PolicyExclusionReason::DuplicateVenue);
        assert(result.exclusions[1U].reason ==
               PolicyExclusionReason::DuplicateVenue);
    }
    {
        PolicySnapshot stale = configured;
        stale.compliance.effective_until_utc_ns = order.decision_utc_ns;
        const AdaptivePolicyKernel kernel{stale};
        const std::array candidates{aggressive_facts()};
        assert(kernel.evaluate(order, candidates).status ==
               PolicyEvaluationStatus::InvalidSnapshot);
    }
    {
        PolicySnapshot invalid = configured;
        invalid.weight_generation = 0U;
        const AdaptivePolicyKernel kernel{invalid};
        const std::array candidates{aggressive_facts()};
        assert(!kernel.ready());
        assert(kernel.evaluate(order, candidates).status ==
               PolicyEvaluationStatus::InvalidSnapshot);
    }
    {
        PolicyOrder invalid = order;
        invalid.side = static_cast<Side>(0xffU);
        const AdaptivePolicyKernel kernel{configured};
        const std::array candidates{aggressive_facts()};
        assert(kernel.evaluate(invalid, candidates).status ==
               PolicyEvaluationStatus::InvalidOrder);
    }
    {
        const AdaptivePolicyKernel kernel{configured};
        std::array<VenueRoutingFacts, kMaximumAdaptiveCandidates + 1U> candidates{};
        assert(kernel.evaluate(order, candidates).status ==
               PolicyEvaluationStatus::CapacityExceeded);
    }
}

void test_deterministic_ordering_ties_and_side_direction() {
    const PolicySnapshot configured = snapshot();
    const AdaptivePolicyKernel kernel{configured};
    std::array<VenueRoutingFacts, 5U> candidates{
        aggressive_facts(5U),
        aggressive_facts(4U),
        aggressive_facts(3U),
        aggressive_facts(2U),
        aggressive_facts(1U),
    };
    candidates[0U].executable_price = Price{99};
    candidates[0U].metrics.measured_latency_ns = 999'999U;
    candidates[1U].aggressive_priority = 10U;
    candidates[2U].aggressive_priority = 1U;
    candidates[3U].aggressive_priority = 10U;
    candidates[4U].available_depth = Quantity{20U};

    const PolicyDecision first = kernel.evaluate(aggressive_order(), candidates);
    assert(first.accepted());
    assert(first.ranked_count == candidates.size());
    assert(first.ranked[0U].venue == VenueId{5U});
    assert(first.ranked[1U].venue == VenueId{1U});
    assert(first.ranked[2U].venue == VenueId{3U});
    assert(first.ranked[3U].venue == VenueId{2U});
    assert(first.ranked[4U].venue == VenueId{4U});

    std::reverse(candidates.begin(), candidates.end());
    const PolicyDecision replay = kernel.evaluate(aggressive_order(), candidates);
    assert(replay.ranked_count == first.ranked_count);
    for (std::size_t index{}; index < first.ranked_count; ++index) {
        assert(replay.ranked[index] == first.ranked[index]);
    }

    std::array sell_candidates{aggressive_facts(1U), aggressive_facts(2U)};
    sell_candidates[0U].executable_price = Price{100};
    sell_candidates[0U].protected_quote.best_opposite_price = Price{100};
    sell_candidates[1U].executable_price = Price{101};
    sell_candidates[1U].protected_quote.best_opposite_price = Price{100};
    const PolicyDecision sell =
        kernel.evaluate(aggressive_order(Side::Sell), sell_candidates);
    assert(sell.accepted());
    assert(sell.ranked[0U].venue == VenueId{2U});
}

void test_passive_economics_metrics_and_stable_venue_ordering() {
    const AdaptivePolicyKernel kernel{snapshot()};
    std::array candidates{
        passive_facts(3U), passive_facts(1U), passive_facts(2U)};
    candidates[0U].fees.maker_fee_ppm = 1;
    candidates[1U].fees.maker_fee_ppm = -5;
    candidates[2U].fees.maker_fee_ppm = -5;
    candidates[1U].metrics.fill_quality_ppm = 20U;
    candidates[2U].metrics.fill_quality_ppm = 20U;
    candidates[1U].passive_priority = 7U;
    candidates[2U].passive_priority = 7U;
    const PolicyDecision result = kernel.evaluate(passive_order(), candidates);
    assert(result.accepted());
    assert(result.ranked[0U].venue == VenueId{1U});
    assert(result.ranked[1U].venue == VenueId{2U});
    assert(result.ranked[2U].venue == VenueId{3U});
}

void test_saturating_arithmetic_never_wraps() {
    PolicySnapshot configured = snapshot();
    configured.maximum_metric_latency_ns =
        std::numeric_limits<std::uint64_t>::max();
    configured.weights = PolicyWeights{
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
    };
    VenueRoutingFacts first = aggressive_facts(2U);
    first.available_depth = Quantity{std::numeric_limits<std::uint64_t>::max()};
    first.fees.taker_fee_ppm = -1'000'000;
    first.metrics.measured_latency_ns =
        std::numeric_limits<std::uint64_t>::max();
    first.metrics.reject_rate_ppm = kMetricPartsPerMillion;
    VenueRoutingFacts second = first;
    second.venue = VenueId{1U};
    const std::array candidates{first, second};
    const AdaptivePolicyKernel kernel{configured};
    const PolicyDecision result = kernel.evaluate(aggressive_order(), candidates);
    assert(result.accepted());
    assert(result.ranked_count == 2U);
    assert(result.ranked[0U].venue == VenueId{1U});
    for (const RankedCandidate& ranked : result.ranked_candidates()) {
        assert(ranked.score.saturated);
        assert(ranked.score.depth_reward ==
               std::numeric_limits<std::int64_t>::max());
        assert(ranked.score.fee_economics ==
               std::numeric_limits<std::int64_t>::max());
        assert(ranked.score.latency_penalty ==
               -std::numeric_limits<std::int64_t>::max());
        assert(ranked.score.reject_penalty ==
               -std::numeric_limits<std::int64_t>::max());
        assert(ranked.score.total ==
               -std::numeric_limits<std::int64_t>::max());
    }
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
    state = state * 6'364'136'223'846'793'005ULL +
            1'442'695'040'888'963'407ULL;
    return state;
}

[[nodiscard]] std::uint64_t corpus_hash(
    const PolicyDecision& decision) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const RankedCandidate& candidate : decision.ranked_candidates()) {
        hash ^= candidate.venue.value();
        hash *= 1'099'511'628'211ULL;
        hash ^= static_cast<std::uint64_t>(candidate.executable_price.units);
        hash *= 1'099'511'628'211ULL;
        hash ^= static_cast<std::uint64_t>(candidate.score.total);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

void test_fixed_seed_corpus_replays_independent_of_input_order() {
    std::array<VenueRoutingFacts, kMaximumAdaptiveCandidates> corpus{};
    std::uint64_t random = 0x5eed'6110'15c3'0001ULL;
    for (std::size_t index{}; index < corpus.size(); ++index) {
        VenueRoutingFacts facts = aggressive_facts(
            static_cast<std::uint32_t>(index + 1U));
        facts.executable_price = Price{
            98 + static_cast<std::int64_t>(next_random(random) % 3U)};
        facts.available_depth = Quantity{1U + next_random(random) % 10'000U};
        facts.fees.taker_fee_ppm = static_cast<std::int32_t>(
            next_random(random) % 2'001U) - 1'000;
        facts.metrics.measured_latency_ns = next_random(random) % 1'000'001U;
        facts.metrics.reject_rate_ppm = static_cast<std::uint32_t>(
            next_random(random) % (kMetricPartsPerMillion + 1U));
        facts.aggressive_priority = static_cast<std::uint32_t>(
            next_random(random) % 8U);
        corpus[index] = facts;
    }
    const AdaptivePolicyKernel kernel{snapshot()};
    const PolicyDecision first = kernel.evaluate(aggressive_order(), corpus);
    assert(first.accepted());
    assert(first.ranked_count == corpus.size());
    const std::uint64_t first_hash = corpus_hash(first);
    assert(first_hash == 17'273'239'647'134'645'586ULL);

    std::reverse(corpus.begin(), corpus.end());
    const PolicyDecision replay = kernel.evaluate(aggressive_order(), corpus);
    assert(replay.accepted());
    assert(corpus_hash(replay) == first_hash);
    for (std::size_t index{}; index < first.ranked_count; ++index) {
        assert(replay.ranked[index] == first.ranked[index]);
    }
}

}  // namespace

int main() {
    test_version_pins_and_exact_score_components();
    test_all_common_hard_gates();
    test_aggressive_and_protected_price_hard_gates();
    test_passive_post_only_and_non_marketable_gates();
    test_duplicate_invalid_snapshot_order_and_capacity_fail_closed();
    test_deterministic_ordering_ties_and_side_direction();
    test_passive_economics_metrics_and_stable_venue_ordering();
    test_saturating_arithmetic_never_wraps();
    test_fixed_seed_corpus_replays_independent_of_input_order();
    return 0;
}
