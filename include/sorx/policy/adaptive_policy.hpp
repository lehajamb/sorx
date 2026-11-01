#pragma once

#include "sorx/types.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace sorx::policy {

inline constexpr std::size_t kMaximumAdaptiveCandidates = 32U;
inline constexpr std::uint32_t kMetricPartsPerMillion = 1'000'000U;

// This policy is deterministic arithmetic only. It contains no online model,
// clock access, allocation, or legal-date constants. RouteKernel composition
// and regulatory validation/certification remain mandatory integration gates.
enum class ProtectedPriceMode : std::uint8_t {
    Disabled = 0U,
    EnforceBestProtectedPrice = 1U,
};

struct ComplianceRulePack final {
    std::uint64_t rule_pack_id{0U};
    std::uint64_t rule_pack_version{0U};
    std::uint64_t generation{0U};
    std::uint64_t effective_from_utc_ns{0U};
    // Zero means no configured end. The caller supplies decision_utc_ns.
    std::uint64_t effective_until_utc_ns{0U};
    ProtectedPriceMode protected_price_mode{ProtectedPriceMode::Disabled};
    bool require_protected_quote{false};
    bool require_protected_access{false};

    [[nodiscard]] constexpr bool structurally_valid() const noexcept {
        const bool valid_mode =
            protected_price_mode == ProtectedPriceMode::Disabled ||
            protected_price_mode == ProtectedPriceMode::EnforceBestProtectedPrice;
        return rule_pack_id != 0U && rule_pack_version != 0U && generation != 0U &&
               valid_mode &&
               (effective_until_utc_ns == 0U ||
                effective_until_utc_ns > effective_from_utc_ns) &&
               (!require_protected_access ||
                protected_price_mode == ProtectedPriceMode::EnforceBestProtectedPrice);
    }

    [[nodiscard]] constexpr bool effective_at(
        const std::uint64_t decision_utc_ns) const noexcept {
        return structurally_valid() && decision_utc_ns != 0U &&
               decision_utc_ns >= effective_from_utc_ns &&
               (effective_until_utc_ns == 0U ||
                decision_utc_ns < effective_until_utc_ns);
    }

    constexpr auto operator<=>(const ComplianceRulePack&) const noexcept = default;
};

struct PolicyWeights final {
    std::uint64_t displayed_depth_reward{0U};
    std::uint64_t fee_economics{0U};
    std::uint64_t fill_quality_reward{0U};
    std::uint64_t adverse_selection_penalty{0U};
    std::uint64_t latency_penalty{0U};
    std::uint64_t reject_penalty{0U};

    constexpr auto operator<=>(const PolicyWeights&) const noexcept = default;
};

struct PolicySnapshot final {
    std::uint64_t policy_generation{0U};
    std::uint64_t fee_generation{0U};
    std::uint64_t metric_generation{0U};
    std::uint64_t weight_generation{0U};
    std::uint64_t compliance_generation{0U};
    std::uint64_t maximum_metric_latency_ns{0U};
    PolicyWeights weights{};
    ComplianceRulePack compliance{};

    [[nodiscard]] constexpr bool structurally_valid() const noexcept {
        return policy_generation != 0U && fee_generation != 0U &&
               metric_generation != 0U && weight_generation != 0U &&
               compliance_generation != 0U && maximum_metric_latency_ns != 0U &&
               compliance.structurally_valid() &&
               compliance.generation == compliance_generation;
    }

    [[nodiscard]] constexpr bool valid_at(
        const std::uint64_t decision_utc_ns) const noexcept {
        return structurally_valid() && compliance.effective_at(decision_utc_ns);
    }

    constexpr auto operator<=>(const PolicySnapshot&) const noexcept = default;
};

struct PolicyVersionPin final {
    std::uint64_t policy_generation{0U};
    std::uint64_t fee_generation{0U};
    std::uint64_t metric_generation{0U};
    std::uint64_t weight_generation{0U};
    std::uint64_t compliance_generation{0U};
    std::uint64_t compliance_rule_pack_id{0U};
    std::uint64_t compliance_rule_pack_version{0U};
    std::uint64_t compliance_effective_from_utc_ns{0U};
    std::uint64_t compliance_effective_until_utc_ns{0U};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return policy_generation != 0U && fee_generation != 0U &&
               metric_generation != 0U && weight_generation != 0U &&
               compliance_generation != 0U && compliance_rule_pack_id != 0U &&
               compliance_rule_pack_version != 0U &&
               (compliance_effective_until_utc_ns == 0U ||
                compliance_effective_until_utc_ns >
                    compliance_effective_from_utc_ns);
    }

    constexpr auto operator<=>(const PolicyVersionPin&) const noexcept = default;
};

struct PolicyOrder final {
    ParentOrderId parent_order_id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    RoutingIntent intent{RoutingIntent::AggressiveIOC};
    Quantity quantity{};
    Price limit_price{};
    std::uint64_t decision_monotonic_ns{0U};
    std::uint64_t decision_utc_ns{0U};

    [[nodiscard]] constexpr bool valid() const noexcept {
        const bool valid_side = side == Side::Buy || side == Side::Sell;
        const bool valid_intent = intent == RoutingIntent::AggressiveIOC ||
                                  intent == RoutingIntent::PassiveGTC;
        return static_cast<bool>(parent_order_id) &&
               static_cast<bool>(instrument) && valid_side && valid_intent &&
               static_cast<bool>(quantity) && limit_price.units > 0 &&
               decision_monotonic_ns != 0U && decision_utc_ns != 0U;
    }

    constexpr auto operator<=>(const PolicyOrder&) const noexcept = default;
};

struct AdaptiveVenueCapabilities final {
    bool aggressive_ioc{false};
    bool passive_gtc{false};
    bool post_only{false};

    constexpr auto operator<=>(const AdaptiveVenueCapabilities&) const noexcept = default;
};

struct VenueFactVersions final {
    std::uint64_t route_generation{0U};
    std::uint64_t market_generation{0U};
    std::uint64_t order_entry_generation{0U};
    std::uint64_t risk_generation{0U};
    std::uint64_t throttle_generation{0U};
    std::uint64_t tx_credit_generation{0U};
    std::uint64_t protected_quote_generation{0U};
    std::uint64_t fee_generation{0U};
    std::uint64_t metric_generation{0U};

    [[nodiscard]] constexpr bool base_valid() const noexcept {
        return route_generation != 0U && market_generation != 0U &&
               order_entry_generation != 0U && risk_generation != 0U &&
               throttle_generation != 0U && tx_credit_generation != 0U &&
               fee_generation != 0U && metric_generation != 0U;
    }

    constexpr auto operator<=>(const VenueFactVersions&) const noexcept = default;
};

struct ProtectedQuoteFacts final {
    bool view_healthy{false};
    bool best_opposite_present{false};
    Price best_opposite_price{};

    constexpr auto operator<=>(const ProtectedQuoteFacts&) const noexcept = default;
};

struct VenueFees final {
    // Positive is a cost; negative is a rebate. Units are parts per million.
    std::int32_t maker_fee_ppm{0};
    std::int32_t taker_fee_ppm{0};

    [[nodiscard]] constexpr bool valid() const noexcept {
        constexpr std::int32_t limit =
            static_cast<std::int32_t>(kMetricPartsPerMillion);
        return maker_fee_ppm >= -limit && maker_fee_ppm <= limit &&
               taker_fee_ppm >= -limit && taker_fee_ppm <= limit;
    }

    constexpr auto operator<=>(const VenueFees&) const noexcept = default;
};

struct BoundedVenueMetrics final {
    std::uint32_t fill_quality_ppm{0U};
    std::uint32_t adverse_selection_ppm{0U};
    std::uint32_t reject_rate_ppm{0U};
    std::uint64_t measured_latency_ns{0U};

    [[nodiscard]] constexpr bool valid(
        const std::uint64_t maximum_latency_ns) const noexcept {
        return fill_quality_ppm <= kMetricPartsPerMillion &&
               adverse_selection_ppm <= kMetricPartsPerMillion &&
               reject_rate_ppm <= kMetricPartsPerMillion &&
               measured_latency_ns <= maximum_latency_ns;
    }

    constexpr auto operator<=>(const BoundedVenueMetrics&) const noexcept = default;
};

struct VenueRoutingFacts final {
    VenueId venue{};
    InstrumentId instrument{};
    VenueFactVersions versions{};
    AdaptiveVenueCapabilities capabilities{};
    AdminState admin_state{AdminState::Down};
    InstrumentState trading_state{InstrumentState::Unknown};
    MarketDataState feed_state{MarketDataState::Failed};
    OrderEntryState order_entry_state{OrderEntryState::Disconnected};
    EgressState egress_state{EgressState::Congested};
    bool permission_allowed{false};
    bool feed_healthy{false};
    bool risk_allowed{false};
    bool protected_access_allowed{false};
    bool post_only_eligible{false};
    bool local_opposite_present{false};
    std::uint64_t quote_age_ns{0U};
    std::uint64_t maximum_quote_age_ns{0U};
    std::uint32_t throttle_credits{0U};
    std::uint32_t normal_tx_credits{0U};
    Price executable_price{};
    Quantity available_depth{};
    Price local_opposite_price{};
    ProtectedQuoteFacts protected_quote{};
    VenueFees fees{};
    BoundedVenueMetrics metrics{};
    std::uint32_t aggressive_priority{0U};
    std::uint32_t passive_priority{0U};

    constexpr auto operator<=>(const VenueRoutingFacts&) const noexcept = default;
};

enum class PolicyExclusionReason : std::uint8_t {
    None = 0U,
    InvalidFacts,
    DuplicateVenue,
    InstrumentMismatch,
    RouteVersionMissing,
    FeeGenerationMismatch,
    MetricGenerationMismatch,
    UnsupportedIntent,
    PermissionDenied,
    AdminDown,
    InstrumentNotTrading,
    FeedNotLive,
    FeedUnhealthy,
    InvalidQuoteAge,
    QuoteStale,
    OrderEntryNotReady,
    EgressUnavailable,
    RiskDenied,
    ThrottleUnavailable,
    TxCreditUnavailable,
    InvalidEconomics,
    InvalidMetrics,
    InvalidExecutablePrice,
    PriceOutsideLimit,
    NoExecutableDepth,
    ProtectedQuoteUnavailable,
    ProtectedQuoteVersionMissing,
    ProtectedQuoteInconsistent,
    ProtectedAccessDenied,
    ProtectedPriceViolation,
    PostOnlyUnsupported,
    PostOnlyIneligible,
    PassiveMarketableLocal,
    PassiveMarketableProtected,
};

struct VenueFactPin final {
    VenueId venue{};
    VenueFactVersions versions{};

    constexpr auto operator<=>(const VenueFactPin&) const noexcept = default;
};

struct PolicyExclusion final {
    VenueId venue{};
    PolicyExclusionReason reason{PolicyExclusionReason::InvalidFacts};
    VenueFactPin pin{};

    constexpr auto operator<=>(const PolicyExclusion&) const noexcept = default;
};

struct ScoreBreakdown final {
    std::int64_t depth_reward{0};
    std::int64_t fee_economics{0};
    std::int64_t fill_quality_reward{0};
    std::int64_t adverse_selection_penalty{0};
    std::int64_t latency_penalty{0};
    std::int64_t reject_penalty{0};
    std::int64_t total{0};
    bool saturated{false};

    constexpr auto operator<=>(const ScoreBreakdown&) const noexcept = default;
};

struct RankedCandidate final {
    VenueId venue{};
    VenueFactPin pin{};
    Price executable_price{};
    Quantity available_depth{};
    std::uint64_t quote_age_ns{0U};
    std::uint64_t maximum_quote_age_ns{0U};
    std::uint32_t throttle_credits{0U};
    std::uint32_t normal_tx_credits{0U};
    ProtectedQuoteFacts protected_quote{};
    std::int32_t fee_ppm_used{0};
    BoundedVenueMetrics metrics{};
    ScoreBreakdown score{};
    std::uint32_t configured_priority{0U};

    constexpr auto operator<=>(const RankedCandidate&) const noexcept = default;
};

enum class PolicyEvaluationStatus : std::uint8_t {
    Ranked = 0U,
    NoEligibleCandidate,
    InvalidOrder,
    InvalidSnapshot,
    CapacityExceeded,
};

struct PolicyDecision final {
    PolicyEvaluationStatus status{PolicyEvaluationStatus::InvalidOrder};
    PolicyVersionPin versions{};
    PolicyOrder order{};
    RoutingIntent intent{RoutingIntent::AggressiveIOC};
    std::array<RankedCandidate, kMaximumAdaptiveCandidates> ranked{};
    std::array<PolicyExclusion, kMaximumAdaptiveCandidates> exclusions{};
    std::uint16_t ranked_count{0U};
    std::uint16_t exclusion_count{0U};

    [[nodiscard]] constexpr std::span<const RankedCandidate>
    ranked_candidates() const noexcept {
        return {ranked.data(), ranked_count};
    }

    [[nodiscard]] constexpr std::span<const PolicyExclusion>
    candidate_exclusions() const noexcept {
        return {exclusions.data(), exclusion_count};
    }

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == PolicyEvaluationStatus::Ranked && ranked_count != 0U;
    }
};

class AdaptivePolicyKernel final {
public:
    explicit constexpr AdaptivePolicyKernel(const PolicySnapshot& snapshot) noexcept
        : snapshot_(snapshot) {}

    [[nodiscard]] constexpr bool ready() const noexcept {
        return snapshot_.structurally_valid();
    }

    [[nodiscard]] constexpr const PolicySnapshot& snapshot() const noexcept {
        return snapshot_;
    }

    [[nodiscard]] PolicyDecision evaluate(
        const PolicyOrder& order,
        std::span<const VenueRoutingFacts> candidates) const noexcept;

private:
    PolicySnapshot snapshot_{};
};

static_assert(std::is_trivially_copyable_v<PolicySnapshot>);
static_assert(std::is_trivially_copyable_v<VenueRoutingFacts>);
static_assert(std::is_trivially_copyable_v<PolicyDecision>);

}  // namespace sorx::policy
