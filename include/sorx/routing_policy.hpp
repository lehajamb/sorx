#pragma once

#include "sorx/book_view.hpp"
#include "sorx/route_rib.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sorx {

struct CandidateRuntimeView final {
    VenueId venue{};
    std::uint64_t adjacency_generation{};
    std::optional<md::BookView> book;
    MarketDataState market_data_state{MarketDataState::Snapshotting};
    OrderEntryState order_entry_state{OrderEntryState::Disconnected};
    EgressState egress_state{EgressState::Ready};
    InstrumentState instrument_state{InstrumentState::Unknown};
    bool feed_sufficiently_fresh{};
    bool conversion_compatible{true};
    bool risk_allowed{true};
};

enum class CandidateExclusionReason : std::uint8_t {
    AdminDown,
    UnsupportedIntent,
    MissingRuntimeState,
    MissingBook,
    InvalidBook,
    BookIdentityMismatch,
    MarketDataNotLive,
    StaleBook,
    InstrumentNotTrading,
    OrderEntryNotReady,
    EgressCongested,
    ConversionIncompatible,
    InexactPriceConversion,
    InexactQuantityConversion,
    RiskProhibited,
    AllocationCapTooSmall,
    NoExecutableDepth,
    MaximumVenueCount,
};

struct CandidateExclusion final {
    VenueId venue{};
    CandidateExclusionReason reason{CandidateExclusionReason::MissingRuntimeState};

    constexpr auto operator<=>(const CandidateExclusion&) const noexcept = default;
};

enum class RouteRejectReason : std::uint8_t {
    None,
    InvalidParent,
    IntentMismatch,
    InstrumentNotConfigured,
    NoEligibleCandidate,
    NoExecutableLiquidity,
    PassiveMarketable,
    PassiveFullQuantityUnavailable,
};

enum class RemainderReason : std::uint8_t {
    None,
    NoEligibleCandidate,
    ParentLimitReached,
    InsufficientDisplayedDepth,
    TruncatedDepthUnknown,
    VenueAllocationCaps,
    MaximumVenueCount,
    InexactConversion,
    PassiveFullQuantityUnavailable,
};

struct ChildAllocation final {
    VenueId venue{};
    Quantity quantity{};
    Price limit_price{};
    Quantity venue_quantity{};
    Price venue_limit_price{};
    std::uint32_t allocation_order{};
    std::uint64_t adjacency_generation{};
    md::BookVersion book_version{};

    constexpr auto operator<=>(const ChildAllocation&) const noexcept = default;
};

struct CandidateVersionPin final {
    VenueId venue{};
    std::uint64_t adjacency_generation{};
    std::optional<md::BookVersion> book_version;

    constexpr auto operator<=>(const CandidateVersionPin&) const noexcept = default;
};

struct RouteDecision final {
    std::uint64_t candidate_table_generation{};
    std::uint64_t risk_generation{};
    std::vector<CandidateVersionPin> candidate_pins;
    std::vector<ChildAllocation> allocations;
    std::vector<CandidateExclusion> exclusions;
    Quantity allocated{};
    Quantity remainder{};
    RemainderReason remainder_reason{RemainderReason::None};
    RouteRejectReason reject_reason{RouteRejectReason::None};
    bool observed_truncated_depth{};

    [[nodiscard]] bool accepted() const noexcept {
        return reject_reason == RouteRejectReason::None;
    }

    [[nodiscard]] bool fully_allocated() const noexcept {
        return remainder.units == 0;
    }
};

class RoutingPolicy final {
public:
    [[nodiscard]] static RouteDecision aggressive_ioc(
        const ParentOrder& parent,
        const CandidateTable& candidates,
        std::span<const CandidateRuntimeView> runtime_views,
        std::size_t maximum_venues);

    [[nodiscard]] static RouteDecision passive_gtc(
        const ParentOrder& parent,
        const CandidateTable& candidates,
        std::span<const CandidateRuntimeView> runtime_views);
};

}  // namespace sorx


