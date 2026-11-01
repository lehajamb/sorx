#pragma once

#include "sorx/dataplane/book_slot.hpp"
#include "sorx/order_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace sorx::dataplane {

namespace detail {

template <std::size_t... Indices>
[[nodiscard]] constexpr std::array<Quantity, sizeof...(Indices)>
zero_quantities(std::index_sequence<Indices...>) noexcept {
    return {((void)Indices, Quantity{0U})...};
}

template <std::size_t Count>
[[nodiscard]] constexpr std::array<Quantity, Count>
zero_quantities() noexcept {
    return zero_quantities(std::make_index_sequence<Count>{});
}

}  // namespace detail

inline constexpr std::size_t kMaximumDataplaneVenues = 64U;
inline constexpr std::size_t kMaximumDataplaneInstruments = 256U;
inline constexpr std::size_t kMaximumCandidatesPerInstrument = 32U;
inline constexpr std::size_t kMaximumActiveParents = 4096U;
inline constexpr std::size_t kInlineWireIdentityBytes = 32U;

struct DataplaneCandidatePin final {
    CandidateVersionPin market{};
    std::uint64_t feed_epoch{};
    std::uint64_t source_health_generation{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t normal_tx_credits{};
    std::uint32_t cancel_reserved_tx_credits{};
    std::uint32_t throttle_credits{};
    AdaptivePolicyOverlay adaptive_policy{};

    constexpr auto operator<=>(const DataplaneCandidatePin&) const noexcept = default;
};

struct InlineWireIdentity final {
    std::array<std::byte, kInlineWireIdentityBytes> bytes{};
    std::uint8_t size{};

    [[nodiscard]] constexpr std::span<const std::byte> value() const noexcept {
        return {bytes.data(), size};
    }

    constexpr auto operator<=>(const InlineWireIdentity&) const noexcept = default;
};

enum class WireIdentityMode : std::uint8_t {
    ReferenceGeneric = 0U,
    RequireRegistered = 1U,
};

enum class WireIdentityPlanStatus : std::uint8_t {
    NotAttempted = 0U,
    Planned,
    IdentifierExhausted,
    MissingPlanner,
    InvalidInput,
    SequenceExhausted,
    InvalidOutput,
};

struct WireIdentityPlanRequest final {
    VenueId venue{};
    ChildOrderId child_order_id{};
    std::uint64_t dispatch_attempt_id{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
};

using WireIdentityPlanner = WireIdentityPlanStatus (*)(
    const WireIdentityPlanRequest&,
    InlineWireIdentity&) noexcept;

struct WireIdentityPlannerBinding final {
    VenueId venue{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    WireIdentityPlanner planner{};
};

// Metadata parallel to RoutePlan::allocations. Connectors may map the opaque
// inline identity to a venue-specific ClOrdID without changing the durable
// child/attempt identity authorized by the plan.
struct PlannedChild final {
    ChildOrderId child_order_id{};
    std::uint64_t dispatch_attempt_id{};
    InlineWireIdentity wire_identity{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    std::uint64_t ownership_generation{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint16_t normal_tx_credit_delta{1U};
    std::uint16_t cancel_tx_credit_delta{};
    std::uint16_t throttle_credit_delta{1U};

    constexpr auto operator<=>(const PlannedChild&) const noexcept = default;
};

struct RoutePlan final {
    ParentOrder parent{};
    std::uint64_t kernel_state_generation{};
    std::uint64_t candidate_table_generation{};
    std::uint64_t risk_generation{};
    std::uint64_t monotonic_decision_time_ns{};
    std::uint64_t next_child_order_id{};
    std::uint64_t next_dispatch_attempt_id{};
    std::array<DataplaneCandidatePin, kMaximumCandidatesPerInstrument>
        candidate_pins{};
    std::array<ChildAllocation, kMaximumCandidatesPerInstrument> allocations{};
    std::array<PlannedChild, kMaximumCandidatesPerInstrument> planned_children{};
    std::array<CandidateExclusion, kMaximumCandidatesPerInstrument> exclusions{};
    std::uint16_t candidate_pin_count{};
    std::uint16_t allocation_count{};
    std::uint16_t exclusion_count{};
    Quantity allocated{};
    Quantity remainder{};
    RemainderReason remainder_reason{RemainderReason::None};
    RouteRejectReason reject_reason{RouteRejectReason::None};
    bool observed_truncated_depth{};

    [[nodiscard]] constexpr std::span<const DataplaneCandidatePin> pins() const noexcept {
        return {candidate_pins.data(), candidate_pin_count};
    }

    [[nodiscard]] constexpr std::span<const ChildAllocation>
    child_allocations() const noexcept {
        return {allocations.data(), allocation_count};
    }

    [[nodiscard]] constexpr std::span<const PlannedChild> children() const noexcept {
        return {planned_children.data(), allocation_count};
    }

    [[nodiscard]] constexpr std::span<const CandidateExclusion>
    candidate_exclusions() const noexcept {
        return {exclusions.data(), exclusion_count};
    }

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return reject_reason == RouteRejectReason::None &&
               allocation_count != 0U;
    }

    [[nodiscard]] constexpr bool fully_allocated() const noexcept {
        return remainder.units == 0U;
    }
};

enum class PlanStatus : std::uint8_t {
    Planned,
    HardRejected,
    RouteRejected,
    InvalidInput,
    CapacityExceeded,
    IdentifierExhausted,
    RecoveryOnly,
    AdaptivePolicyRejected,
    WireIdentityRejected,
};

struct PlanResult final {
    PlanStatus status{PlanStatus::InvalidInput};
    CommandError hard_error{CommandError::None};
    WireIdentityPlanStatus wire_identity_status{
        WireIdentityPlanStatus::NotAttempted};
    RoutePlan plan{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == PlanStatus::Planned && plan.accepted();
    }
};

enum class CommitStatus : std::uint8_t {
    Committed,
    PlanNotAccepted,
    InvalidPlan,
    StalePlan,
    StaleMarketView,
    CandidateGenerationMismatch,
    RiskGenerationMismatch,
    DuplicateParent,
    ReservationCapacityExceeded,
    RiskViolation,
    CapacityViolation,
    GenerationExhausted,
    RecoveryOnly,
    AdaptivePolicyMismatch,
    WireIdentityMismatch,
};

struct CommitResult final {
    CommitStatus status{CommitStatus::InvalidPlan};
    std::uint64_t kernel_state_generation{};

    [[nodiscard]] constexpr bool committed() const noexcept {
        return status == CommitStatus::Committed;
    }
};

enum class ReleaseStatus : std::uint8_t {
    Released,
    ParentNotFound,
    VenueNotReserved,
    GenerationExhausted,
    InvariantViolation,
};

struct InstrumentRiskCounters final {
    bool found{};
    Quantity gross_outstanding{};
    std::uint64_t open_parents{};
    std::uint64_t open_children{};
};

struct VenueRiskCounters final {
    bool found{};
    Quantity outstanding{};
    std::uint32_t reserved_normal_tx_credits{};
    std::uint32_t reserved_cancel_tx_credits{};
    std::uint32_t reserved_throttle_credits{};
};

// Recovery imports are deliberately narrower than RoutePlan. They contain
// only state which was already authorized by a validated durable dispatch
// batch. Market data is intentionally absent: recovery must never re-route or
// re-price an order after a crash.
struct RecoveredChildReservation final {
    ChildOrderId child_order_id{};
    std::uint64_t dispatch_attempt_id{};
    std::uint64_t last_dispatch_attempt_id{};
    VenueId venue{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    std::uint64_t ownership_generation{};
    InlineWireIdentity wire_identity{};
    Quantity remaining_quantity{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t reported_normal_tx_credits{};
    std::uint32_t reported_cancel_tx_credits{};
    std::uint32_t reported_throttle_credits{};
    std::uint16_t normal_tx_credit_delta{};
    std::uint16_t cancel_tx_credit_delta{};
    std::uint16_t throttle_credit_delta{};
    bool reconciled{};
    bool cancel_pending{};
    std::uint64_t pending_client_request_id{};
    std::uint64_t pending_client_sequence{};
    std::uint64_t pending_client_session_epoch{};
    std::uint64_t pending_cancel_attempt_id{};

    constexpr auto operator<=>(
        const RecoveredChildReservation&) const noexcept = default;
};

struct RecoveredParentReservation final {
    ParentOrder parent{};
    std::uint64_t kernel_state_generation{};
    std::uint64_t candidate_table_generation{};
    std::uint64_t risk_generation{};
    Quantity instrument_outstanding_before{};
    Quantity instrument_outstanding_after{};
    std::uint64_t instrument_open_parents_before{};
    std::uint64_t instrument_open_parents_after{};
    std::uint64_t instrument_open_children_before{};
    std::uint64_t instrument_open_children_after{};
    std::array<RecoveredChildReservation, kMaximumCandidatesPerInstrument>
        children{};
    std::array<Quantity, kMaximumCandidatesPerInstrument>
        venue_outstanding_before =
            detail::zero_quantities<kMaximumCandidatesPerInstrument>();
    std::array<Quantity, kMaximumCandidatesPerInstrument>
        venue_outstanding_after =
            detail::zero_quantities<kMaximumCandidatesPerInstrument>();
    std::uint16_t child_count{};

    [[nodiscard]] constexpr std::span<const RecoveredChildReservation>
    child_reservations() const noexcept {
        return {children.data(), child_count};
    }
};

enum class RecoveryImportStatus : std::uint8_t {
    Imported,
    AlreadyImported,
    NotRecoveryOnly,
    InvalidReservation,
    ConfigurationMismatch,
    StateGenerationMismatch,
    CounterMismatch,
    DuplicateParent,
    CapacityExceeded,
    RiskViolation,
    CreditGenerationMismatch,
    IdentifierRegression,
    GenerationExhausted,
};

enum class RecoveryAccountingStatus : std::uint8_t {
    Applied,
    AlreadyApplied,
    NotRecoveryOnly,
    ParentNotFound,
    ChildNotFound,
    AttemptMismatch,
    InvalidQuantity,
    InvariantViolation,
    GenerationExhausted,
};

enum class RecoveredChildDisposition : std::uint8_t {
    Working,
    Terminal,
};

enum class ActiveChildOwnershipState : std::uint8_t {
    Working = 1U,
    CancelPending = 2U,
    Terminal = 3U,
    Reconciled = 4U,
};

struct ActiveChildOwnershipSnapshot final {
    ParentOrderId parent_order_id{};
    ChildOrderId child_id{};
    VenueId venue_id{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    std::uint64_t ownership_generation{};
    std::uint64_t last_dispatch_attempt_id{};
    InlineWireIdentity wire_identity{};
    ActiveChildOwnershipState state{ActiveChildOwnershipState::Working};
    std::uint64_t pending_client_request_id{};
    std::uint64_t pending_client_sequence{};
    std::uint64_t pending_client_session_epoch{};
    std::uint64_t pending_cancel_attempt_id{};

    constexpr auto operator<=> (
        const ActiveChildOwnershipSnapshot&) const noexcept = default;
};

enum class ActiveChildLookupStatus : std::uint8_t {
    Found = 0U,
    NotFound,
    Unavailable,
};

struct ActiveChildLookupResult final {
    ActiveChildLookupStatus status{ActiveChildLookupStatus::NotFound};
    ActiveChildOwnershipSnapshot snapshot{};
};

struct ActiveChildCancelTransition final {
    ActiveChildOwnershipSnapshot expected{};
    std::uint64_t client_request_id{};
    std::uint64_t client_sequence{};
    std::uint64_t client_session_epoch{};
    std::uint64_t dispatch_attempt_id{};
};

enum class ActiveChildTransitionStatus : std::uint8_t {
    Marked = 0U,
    StaleSnapshot,
    AlreadyPendingSame,
    AlreadyPendingOther,
    Terminal,
    Rejected,
    GenerationExhausted,
};

struct RecoveryIdentifierState final {
    std::uint64_t next_child_order_id{};
    std::uint64_t next_dispatch_attempt_id{};
};

struct RecoveryKernelState final {
    std::uint64_t state_generation{};
    RecoveryIdentifierState identifiers{};
};

struct RecoveryVenueCreditState final {
    VenueId venue{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t reported_normal_tx_credits{};
    std::uint32_t reported_cancel_tx_credits{};
    std::uint32_t reported_throttle_credits{};
    std::uint32_t reserved_normal_tx_credits{};
    std::uint32_t reserved_cancel_tx_credits{};
    std::uint32_t reserved_throttle_credits{};
    bool found{};
};

enum class AdaptivePolicyMode : std::uint8_t {
    Optional = 0U,
    Required = 1U,
};

// Deterministic, bounded routing and reservation kernel. Construct it on a
// control thread; then one coordinator thread owns plan/commit/release and gate
// changes. Market-data/session threads cross into the coordinator through
// BookSlotPublisher or an SpscRing. Hot methods are noexcept and allocate no
// memory.
class RouteKernel final {
public:
    RouteKernel(
        const CoreConfiguration& configuration,
        const CandidateTable& candidates,
        AdaptivePolicyMode adaptive_policy_mode =
            AdaptivePolicyMode::Required,
        WireIdentityMode wire_identity_mode =
            WireIdentityMode::RequireRegistered,
        std::span<const WireIdentityPlannerBinding> wire_identity_planners = {});
    ~RouteKernel();

    RouteKernel(const RouteKernel&) = delete;
    RouteKernel& operator=(const RouteKernel&) = delete;
    RouteKernel(RouteKernel&&) noexcept;
    RouteKernel& operator=(RouteKernel&&) noexcept;

    [[nodiscard]] PlanResult plan(
        const ParentOrder& parent,
        std::span<const BookSlotView> views,
        std::uint64_t monotonic_decision_time_ns) const noexcept;

    [[nodiscard]] CommitResult commit(
        const RoutePlan& plan,
        std::span<const BookSlotView> current_views,
        std::uint64_t monotonic_commit_time_ns) noexcept;

    [[nodiscard]] ReleaseStatus release_child(
        ParentOrderId parent,
        VenueId venue) noexcept;

    [[nodiscard]] ActiveChildLookupResult lookup_active_child(
        ChildOrderId child) const noexcept;
    [[nodiscard]] ActiveChildTransitionStatus mark_cancel_pending(
        const ActiveChildCancelTransition& transition) noexcept;

    // Cold-start recovery API. begin_recovery() is accepted only on an empty
    // kernel. It forces kill/routing gates closed and does not advance the
    // durable kernel-state generation. Imports and accounting operations are
    // bounded/noexcept and allocate no memory. complete_recovery() clears only
    // the recovery-only bit; kill remains set and routing remains disabled.
    [[nodiscard]] bool begin_recovery() noexcept;
    [[nodiscard]] RecoveryImportStatus restore_recovered_parent(
        const RecoveredParentReservation& reservation) noexcept;
    [[nodiscard]] RecoveryAccountingStatus apply_recovered_fill(
        ParentOrderId parent,
        ChildOrderId child,
        std::uint64_t dispatch_attempt_id,
        Quantity filled_quantity) noexcept;
    [[nodiscard]] RecoveryAccountingStatus note_recovered_cancel(
        ParentOrderId parent,
        ChildOrderId child,
        std::uint64_t prior_dispatch_attempt_id,
        std::uint64_t cancel_dispatch_attempt_id,
        std::uint64_t client_request_id,
        std::uint64_t client_sequence,
        std::uint64_t client_session_epoch) noexcept;
    [[nodiscard]] RecoveryAccountingStatus reconcile_recovered_child(
        ParentOrderId parent,
        ChildOrderId child,
        std::uint64_t dispatch_attempt_id,
        RecoveredChildDisposition disposition) noexcept;
    [[nodiscard]] bool restore_recovery_kernel_state(
        RecoveryKernelState state) noexcept;
    [[nodiscard]] bool restore_recovery_venue_credit_state(
        const RecoveryVenueCreditState& state) noexcept;
    [[nodiscard]] bool complete_recovery() noexcept;

    [[nodiscard]] bool set_kill_switch(bool active) noexcept;
    [[nodiscard]] bool set_routing_enabled(bool enabled) noexcept;

    [[nodiscard]] bool kill_switch() const noexcept;
    [[nodiscard]] bool routing_enabled() const noexcept;
    [[nodiscard]] bool recovery_only() const noexcept;
    [[nodiscard]] std::size_t unreconciled_child_count() const noexcept;
    [[nodiscard]] RecoveryIdentifierState recovery_identifiers() const noexcept;
    [[nodiscard]] RecoveryVenueCreditState recovery_venue_credit_state(
        VenueId venue) const noexcept;
    [[nodiscard]] std::uint64_t state_generation() const noexcept;
    [[nodiscard]] std::uint64_t candidate_table_generation() const noexcept;
    [[nodiscard]] std::uint64_t risk_generation() const noexcept;
    [[nodiscard]] std::size_t active_parent_count() const noexcept;
    [[nodiscard]] AdaptivePolicyMode adaptive_policy_mode() const noexcept;

    [[nodiscard]] InstrumentRiskCounters instrument_counters(
        InstrumentId instrument) const noexcept;
    [[nodiscard]] VenueRiskCounters venue_counters(VenueId venue) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::dataplane
