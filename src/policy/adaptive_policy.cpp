#include "sorx/policy/adaptive_policy.hpp"

#include <array>
#include <limits>

namespace sorx::policy {
namespace {

struct SaturatingValue final {
    std::int64_t value{0};
    bool saturated{false};
};

struct Eligibility final {
    bool eligible{false};
    PolicyExclusionReason reason{PolicyExclusionReason::None};
};

[[nodiscard]] constexpr bool valid_admin(const AdminState state) noexcept {
    return state == AdminState::Up || state == AdminState::Down;
}

[[nodiscard]] constexpr bool valid_trading(
    const InstrumentState state) noexcept {
    return state == InstrumentState::Unknown || state == InstrumentState::Trading ||
           state == InstrumentState::Halted || state == InstrumentState::Closed;
}

[[nodiscard]] constexpr bool valid_feed(const MarketDataState state) noexcept {
    return state == MarketDataState::Snapshotting ||
           state == MarketDataState::Stitching || state == MarketDataState::Live ||
           state == MarketDataState::Recovering || state == MarketDataState::Failed;
}

[[nodiscard]] constexpr bool valid_order_entry(
    const OrderEntryState state) noexcept {
    return state == OrderEntryState::Disconnected ||
           state == OrderEntryState::Connecting || state == OrderEntryState::Ready ||
           state == OrderEntryState::Degraded;
}

[[nodiscard]] constexpr bool valid_egress(const EgressState state) noexcept {
    return state == EgressState::Ready || state == EgressState::Congested;
}

[[nodiscard]] constexpr PolicyVersionPin pin(
    const PolicySnapshot& snapshot) noexcept {
    return PolicyVersionPin{
        snapshot.policy_generation,
        snapshot.fee_generation,
        snapshot.metric_generation,
        snapshot.weight_generation,
        snapshot.compliance_generation,
        snapshot.compliance.rule_pack_id,
        snapshot.compliance.rule_pack_version,
        snapshot.compliance.effective_from_utc_ns,
        snapshot.compliance.effective_until_utc_ns,
    };
}

[[nodiscard]] constexpr VenueFactPin pin(
    const VenueRoutingFacts& facts) noexcept {
    return VenueFactPin{facts.venue, facts.versions};
}

[[nodiscard]] constexpr bool executable_at_limit(
    const Side side,
    const Price executable,
    const Price limit) noexcept {
    return side == Side::Buy ? executable.units <= limit.units
                             : executable.units >= limit.units;
}

[[nodiscard]] constexpr bool marketable(
    const Side side,
    const Price order_limit,
    const Price opposite) noexcept {
    return side == Side::Buy ? order_limit.units >= opposite.units
                             : order_limit.units <= opposite.units;
}

[[nodiscard]] SaturatingValue positive_product(
    const std::uint64_t lhs,
    const std::uint64_t rhs) noexcept {
    constexpr auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    if (lhs != 0U && rhs > maximum / lhs) {
        return {std::numeric_limits<std::int64_t>::max(), true};
    }
    return {static_cast<std::int64_t>(lhs * rhs), false};
}

[[nodiscard]] SaturatingValue negative_product(
    const std::uint64_t lhs,
    const std::uint64_t rhs) noexcept {
    const SaturatingValue positive = positive_product(lhs, rhs);
    return {-positive.value, positive.saturated};
}

[[nodiscard]] SaturatingValue fee_economics(
    const std::int32_t fee_ppm,
    const std::uint64_t weight) noexcept {
    const auto signed_fee = static_cast<std::int64_t>(fee_ppm);
    const std::uint64_t magnitude = static_cast<std::uint64_t>(
        signed_fee < 0 ? -signed_fee : signed_fee);
    return fee_ppm > 0 ? negative_product(magnitude, weight)
                       : positive_product(magnitude, weight);
}

void add_saturating(
    std::int64_t& total,
    const std::int64_t value,
    bool& saturated) noexcept {
    constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
    if (value > 0 && total > maximum - value) {
        total = maximum;
        saturated = true;
        return;
    }
    if (value < 0 && total < minimum - value) {
        total = minimum;
        saturated = true;
        return;
    }
    total += value;
}

[[nodiscard]] ScoreBreakdown aggressive_score(
    const PolicySnapshot& snapshot,
    const VenueRoutingFacts& facts) noexcept {
    ScoreBreakdown output;
    const SaturatingValue depth = positive_product(
        facts.available_depth.units,
        snapshot.weights.displayed_depth_reward);
    const SaturatingValue fee = fee_economics(
        facts.fees.taker_fee_ppm, snapshot.weights.fee_economics);
    const SaturatingValue latency = negative_product(
        facts.metrics.measured_latency_ns, snapshot.weights.latency_penalty);
    const SaturatingValue rejects = negative_product(
        facts.metrics.reject_rate_ppm, snapshot.weights.reject_penalty);
    output.depth_reward = depth.value;
    output.fee_economics = fee.value;
    output.latency_penalty = latency.value;
    output.reject_penalty = rejects.value;
    output.saturated = depth.saturated || fee.saturated || latency.saturated ||
                       rejects.saturated;
    add_saturating(output.total, output.depth_reward, output.saturated);
    add_saturating(output.total, output.fee_economics, output.saturated);
    add_saturating(output.total, output.latency_penalty, output.saturated);
    add_saturating(output.total, output.reject_penalty, output.saturated);
    return output;
}

[[nodiscard]] ScoreBreakdown passive_score(
    const PolicySnapshot& snapshot,
    const VenueRoutingFacts& facts) noexcept {
    ScoreBreakdown output;
    const SaturatingValue fee = fee_economics(
        facts.fees.maker_fee_ppm, snapshot.weights.fee_economics);
    const SaturatingValue fill = positive_product(
        facts.metrics.fill_quality_ppm, snapshot.weights.fill_quality_reward);
    const SaturatingValue adverse = negative_product(
        facts.metrics.adverse_selection_ppm,
        snapshot.weights.adverse_selection_penalty);
    const SaturatingValue latency = negative_product(
        facts.metrics.measured_latency_ns, snapshot.weights.latency_penalty);
    const SaturatingValue rejects = negative_product(
        facts.metrics.reject_rate_ppm, snapshot.weights.reject_penalty);
    output.fee_economics = fee.value;
    output.fill_quality_reward = fill.value;
    output.adverse_selection_penalty = adverse.value;
    output.latency_penalty = latency.value;
    output.reject_penalty = rejects.value;
    output.saturated = fee.saturated || fill.saturated || adverse.saturated ||
                       latency.saturated || rejects.saturated;
    add_saturating(output.total, output.fee_economics, output.saturated);
    add_saturating(output.total, output.fill_quality_reward, output.saturated);
    add_saturating(
        output.total, output.adverse_selection_penalty, output.saturated);
    add_saturating(output.total, output.latency_penalty, output.saturated);
    add_saturating(output.total, output.reject_penalty, output.saturated);
    return output;
}

[[nodiscard]] Eligibility hard_eligibility(
    const PolicySnapshot& snapshot,
    const PolicyOrder& order,
    const VenueRoutingFacts& facts,
    const bool duplicate_venue,
    const bool protected_quote_consistent) noexcept {
    if (!static_cast<bool>(facts.venue) || !static_cast<bool>(facts.instrument) ||
        !valid_admin(facts.admin_state) || !valid_trading(facts.trading_state) ||
        !valid_feed(facts.feed_state) ||
        !valid_order_entry(facts.order_entry_state) ||
        !valid_egress(facts.egress_state) ||
        (facts.local_opposite_present && facts.local_opposite_price.units <= 0)) {
        return {false, PolicyExclusionReason::InvalidFacts};
    }
    if (duplicate_venue) {
        return {false, PolicyExclusionReason::DuplicateVenue};
    }
    if (facts.instrument != order.instrument) {
        return {false, PolicyExclusionReason::InstrumentMismatch};
    }
    if (facts.versions.route_generation == 0U ||
        facts.versions.market_generation == 0U ||
        facts.versions.order_entry_generation == 0U ||
        facts.versions.risk_generation == 0U ||
        facts.versions.throttle_generation == 0U ||
        facts.versions.tx_credit_generation == 0U) {
        return {false, PolicyExclusionReason::RouteVersionMissing};
    }
    if (facts.versions.fee_generation != snapshot.fee_generation) {
        return {false, PolicyExclusionReason::FeeGenerationMismatch};
    }
    if (facts.versions.metric_generation != snapshot.metric_generation) {
        return {false, PolicyExclusionReason::MetricGenerationMismatch};
    }
    const bool aggressive = order.intent == RoutingIntent::AggressiveIOC;
    if ((aggressive && !facts.capabilities.aggressive_ioc) ||
        (!aggressive && !facts.capabilities.passive_gtc)) {
        return {false, PolicyExclusionReason::UnsupportedIntent};
    }
    if (!facts.permission_allowed) {
        return {false, PolicyExclusionReason::PermissionDenied};
    }
    if (facts.admin_state != AdminState::Up) {
        return {false, PolicyExclusionReason::AdminDown};
    }
    if (facts.trading_state != InstrumentState::Trading) {
        return {false, PolicyExclusionReason::InstrumentNotTrading};
    }
    if (facts.feed_state != MarketDataState::Live) {
        return {false, PolicyExclusionReason::FeedNotLive};
    }
    if (!facts.feed_healthy) {
        return {false, PolicyExclusionReason::FeedUnhealthy};
    }
    if (facts.maximum_quote_age_ns == 0U) {
        return {false, PolicyExclusionReason::InvalidQuoteAge};
    }
    if (facts.quote_age_ns > facts.maximum_quote_age_ns) {
        return {false, PolicyExclusionReason::QuoteStale};
    }
    if (facts.order_entry_state != OrderEntryState::Ready) {
        return {false, PolicyExclusionReason::OrderEntryNotReady};
    }
    if (facts.egress_state != EgressState::Ready) {
        return {false, PolicyExclusionReason::EgressUnavailable};
    }
    if (!facts.risk_allowed) {
        return {false, PolicyExclusionReason::RiskDenied};
    }
    if (facts.throttle_credits == 0U) {
        return {false, PolicyExclusionReason::ThrottleUnavailable};
    }
    if (facts.normal_tx_credits == 0U) {
        return {false, PolicyExclusionReason::TxCreditUnavailable};
    }
    if (!facts.fees.valid()) {
        return {false, PolicyExclusionReason::InvalidEconomics};
    }
    if (!facts.metrics.valid(snapshot.maximum_metric_latency_ns)) {
        return {false, PolicyExclusionReason::InvalidMetrics};
    }
    if (facts.executable_price.units <= 0) {
        return {false, PolicyExclusionReason::InvalidExecutablePrice};
    }

    const ComplianceRulePack& compliance = snapshot.compliance;
    const bool enforce_protected = compliance.protected_price_mode ==
        ProtectedPriceMode::EnforceBestProtectedPrice;
    const bool needs_protected_quote =
        compliance.require_protected_quote || enforce_protected;
    if (needs_protected_quote) {
        if (!protected_quote_consistent) {
            return {false, PolicyExclusionReason::ProtectedQuoteInconsistent};
        }
        if (facts.versions.protected_quote_generation == 0U) {
            return {false, PolicyExclusionReason::ProtectedQuoteVersionMissing};
        }
        if (!facts.protected_quote.view_healthy ||
            !facts.protected_quote.best_opposite_present ||
            facts.protected_quote.best_opposite_price.units <= 0) {
            return {false, PolicyExclusionReason::ProtectedQuoteUnavailable};
        }
    }
    if (compliance.require_protected_access &&
        !facts.protected_access_allowed) {
        return {false, PolicyExclusionReason::ProtectedAccessDenied};
    }

    if (aggressive) {
        if (!executable_at_limit(
                order.side, facts.executable_price, order.limit_price)) {
            return {false, PolicyExclusionReason::PriceOutsideLimit};
        }
        if (!static_cast<bool>(facts.available_depth)) {
            return {false, PolicyExclusionReason::NoExecutableDepth};
        }
        if (enforce_protected &&
            !executable_at_limit(
                order.side,
                facts.executable_price,
                facts.protected_quote.best_opposite_price)) {
            return {false, PolicyExclusionReason::ProtectedPriceViolation};
        }
        return {true, PolicyExclusionReason::None};
    }

    if (!facts.capabilities.post_only) {
        return {false, PolicyExclusionReason::PostOnlyUnsupported};
    }
    if (!facts.post_only_eligible) {
        return {false, PolicyExclusionReason::PostOnlyIneligible};
    }
    if (facts.executable_price != order.limit_price) {
        return {false, PolicyExclusionReason::InvalidExecutablePrice};
    }
    if (facts.local_opposite_present && marketable(
            order.side, order.limit_price, facts.local_opposite_price)) {
        return {false, PolicyExclusionReason::PassiveMarketableLocal};
    }
    if (enforce_protected && marketable(
            order.side,
            order.limit_price,
            facts.protected_quote.best_opposite_price)) {
        return {false, PolicyExclusionReason::PassiveMarketableProtected};
    }
    return {true, PolicyExclusionReason::None};
}

[[nodiscard]] bool candidate_precedes(
    const RankedCandidate& lhs,
    const RankedCandidate& rhs,
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

void insertion_sort(
    std::span<RankedCandidate> values,
    const Side side,
    const RoutingIntent intent) noexcept {
    for (std::size_t index = 1U; index < values.size(); ++index) {
        const RankedCandidate value = values[index];
        std::size_t insertion = index;
        while (insertion != 0U && candidate_precedes(
                value, values[insertion - 1U], side, intent)) {
            values[insertion] = values[insertion - 1U];
            --insertion;
        }
        values[insertion] = value;
    }
}

}  // namespace

PolicyDecision AdaptivePolicyKernel::evaluate(
    const PolicyOrder& order,
    const std::span<const VenueRoutingFacts> candidates) const noexcept {
    PolicyDecision output;
    output.versions = pin(snapshot_);
    output.order = order;
    output.intent = order.intent;
    if (!order.valid()) {
        output.status = PolicyEvaluationStatus::InvalidOrder;
        return output;
    }
    if (!snapshot_.valid_at(order.decision_utc_ns)) {
        output.status = PolicyEvaluationStatus::InvalidSnapshot;
        return output;
    }
    if (candidates.size() > kMaximumAdaptiveCandidates) {
        output.status = PolicyEvaluationStatus::CapacityExceeded;
        return output;
    }

    std::array<bool, kMaximumAdaptiveCandidates> duplicate{};
    for (std::size_t left{}; left < candidates.size(); ++left) {
        if (!static_cast<bool>(candidates[left].venue)) {
            continue;
        }
        for (std::size_t right = left + 1U; right < candidates.size(); ++right) {
            if (candidates[left].venue == candidates[right].venue) {
                duplicate[left] = true;
                duplicate[right] = true;
            }
        }
    }

    bool protected_quote_consistent = true;
    const bool protected_quote_required =
        snapshot_.compliance.require_protected_quote ||
        snapshot_.compliance.protected_price_mode ==
            ProtectedPriceMode::EnforceBestProtectedPrice;
    if (protected_quote_required && candidates.size() > 1U) {
        const VenueRoutingFacts& reference = candidates[0U];
        for (std::size_t index = 1U; index < candidates.size(); ++index) {
            if (candidates[index].versions.protected_quote_generation !=
                    reference.versions.protected_quote_generation ||
                candidates[index].protected_quote != reference.protected_quote) {
                protected_quote_consistent = false;
                break;
            }
        }
    }

    for (std::size_t index{}; index < candidates.size(); ++index) {
        const VenueRoutingFacts& facts = candidates[index];
        const Eligibility eligibility = hard_eligibility(
            snapshot_,
            order,
            facts,
            duplicate[index],
            protected_quote_consistent);
        if (!eligibility.eligible) {
            output.exclusions[output.exclusion_count++] = PolicyExclusion{
                facts.venue, eligibility.reason, pin(facts)};
            continue;
        }

        const bool aggressive = order.intent == RoutingIntent::AggressiveIOC;
        output.ranked[output.ranked_count++] = RankedCandidate{
            facts.venue,
            pin(facts),
            facts.executable_price,
            facts.available_depth,
            facts.quote_age_ns,
            facts.maximum_quote_age_ns,
            facts.throttle_credits,
            facts.normal_tx_credits,
            facts.protected_quote,
            aggressive ? facts.fees.taker_fee_ppm : facts.fees.maker_fee_ppm,
            facts.metrics,
            aggressive ? aggressive_score(snapshot_, facts)
                       : passive_score(snapshot_, facts),
            aggressive ? facts.aggressive_priority : facts.passive_priority,
        };
    }

    insertion_sort(
        {output.ranked.data(), output.ranked_count}, order.side, order.intent);
    output.status = output.ranked_count == 0U
        ? PolicyEvaluationStatus::NoEligibleCandidate
        : PolicyEvaluationStatus::Ranked;
    return output;
}

}  // namespace sorx::policy
