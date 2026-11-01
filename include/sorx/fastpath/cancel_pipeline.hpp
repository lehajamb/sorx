#pragma once

#include "sorx/fastpath/dispatch_pipeline.hpp"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <span>
#include <type_traits>

namespace sorx::fastpath {

// This is an allocation-free coordination contract, not a certified venue
// implementation. Native cancel encoders, venue session ownership, full order-state
// composition, reconciliation, and exchange certification/UAT remain external.
inline constexpr bool kCancelPipelineVenueCertified = false;
inline constexpr std::size_t kMaxCancelVenues = 64U;
inline constexpr std::uint16_t kCancelDispatchRecordType = 0x1101U;
inline constexpr std::uint16_t kCancelDispatchRecordVersion = 1U;

enum class CancelRequestKind : std::uint8_t {
    Cancel = 1U,
    Retry = 2U,
};

struct CancelCommand final {
    std::uint64_t client_request_id{0U};
    std::uint64_t client_sequence{0U};
    std::uint64_t client_session_epoch{0U};
    std::uint64_t account_context_id{0U};
    ParentOrderId parent_order_id{0U};
    ChildOrderId child_id{0U};
    VenueId venue_id{0U};
    std::uint64_t expected_ownership_generation{0U};
    std::uint64_t expected_venue_session_id{0U};
    std::uint64_t expected_venue_session_epoch{0U};
    std::uint16_t reason_code{0U};
    CancelRequestKind kind{CancelRequestKind::Cancel};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return client_request_id != 0U && client_sequence != 0U &&
               client_session_epoch != 0U && account_context_id != 0U &&
               static_cast<bool>(parent_order_id) && static_cast<bool>(child_id) &&
               static_cast<bool>(venue_id) &&
               expected_ownership_generation != 0U && expected_venue_session_id != 0U &&
               expected_venue_session_epoch != 0U &&
               (kind == CancelRequestKind::Cancel || kind == CancelRequestKind::Retry);
    }
};

enum class ActiveChildState : std::uint8_t {
    Working = 1U,
    CancelPending = 2U,
    Terminal = 3U,
    Reconciled = 4U,
};

struct ActiveChildSnapshot final {
    std::uint64_t account_context_id{0U};
    ParentOrderId parent_order_id{0U};
    ChildOrderId child_id{0U};
    VenueId venue_id{0U};
    std::uint64_t venue_session_id{0U};
    std::uint64_t venue_session_epoch{0U};
    std::uint64_t ownership_generation{0U};
    std::uint64_t last_dispatch_attempt_id{0U};
    dataplane::InlineWireIdentity wire_identity{};
    ActiveChildState state{ActiveChildState::Working};
    std::uint64_t pending_client_request_id{0U};
    std::uint64_t pending_client_sequence{0U};
    std::uint64_t pending_client_session_epoch{0U};
    std::uint64_t pending_cancel_attempt_id{0U};

    [[nodiscard]] constexpr bool valid() const noexcept {
        const bool pending_shape = state != ActiveChildState::CancelPending ||
            (pending_client_request_id != 0U && pending_client_sequence != 0U &&
             pending_client_session_epoch != 0U && pending_cancel_attempt_id != 0U &&
             pending_cancel_attempt_id == last_dispatch_attempt_id);
        return account_context_id != 0U && static_cast<bool>(parent_order_id) &&
               static_cast<bool>(child_id) && static_cast<bool>(venue_id) &&
               venue_session_id != 0U && venue_session_epoch != 0U &&
               ownership_generation != 0U && last_dispatch_attempt_id != 0U &&
               wire_identity.size != 0U &&
               wire_identity.size <= wire_identity.bytes.size() &&
               pending_shape &&
               (state == ActiveChildState::Working || state == ActiveChildState::CancelPending ||
                state == ActiveChildState::Terminal || state == ActiveChildState::Reconciled);
    }

    [[nodiscard]] constexpr bool same_cancel_identity(const CancelCommand& command) const noexcept {
        return state == ActiveChildState::CancelPending &&
               pending_client_request_id == command.client_request_id &&
               pending_client_sequence == command.client_sequence &&
               pending_client_session_epoch == command.client_session_epoch;
    }
};

enum class OwnershipLookupStatus : std::uint8_t {
    Found = 0U,
    NotFound,
    Unavailable,
};

struct OwnershipLookupResult final {
    OwnershipLookupStatus status{OwnershipLookupStatus::NotFound};
    ActiveChildSnapshot snapshot{};
};

struct CancelPendingTransition final {
    ActiveChildSnapshot expected{};
    std::uint64_t client_request_id{0U};
    std::uint64_t client_sequence{0U};
    std::uint64_t client_session_epoch{0U};
    std::uint64_t dispatch_attempt_id{0U};
    std::uint64_t recorder_batch_id{0U};
    std::uint64_t leadership_epoch{0U};
    std::uint64_t configuration_generation{0U};
};

enum class OwnershipTransitionStatus : std::uint8_t {
    Marked = 0U,
    StaleSnapshot,
    AlreadyPendingSame,
    AlreadyPendingOther,
    Terminal,
    Rejected,
};

class IActiveChildOwnership {
public:
    virtual ~IActiveChildOwnership() = default;

    [[nodiscard]] virtual OwnershipLookupResult lookup(ChildOrderId child_id) const noexcept = 0;

    // Must compare the complete expected ownership snapshot before changing state.
    [[nodiscard]] virtual OwnershipTransitionStatus mark_cancel_pending(
        const CancelPendingTransition& transition) noexcept = 0;
};

// Allocation-free adapter over the single-owner RouteKernel reservation table.
// account_context_id is fixed for the appliance routing context and is never
// sourced from an untrusted cancel command.
class RouteKernelActiveChildOwnership final : public IActiveChildOwnership {
public:
    explicit RouteKernelActiveChildOwnership(
        dataplane::RouteKernel& kernel,
        std::uint64_t account_context_id) noexcept;

    [[nodiscard]] OwnershipLookupResult lookup(
        ChildOrderId child_id) const noexcept override;
    [[nodiscard]] OwnershipTransitionStatus mark_cancel_pending(
        const CancelPendingTransition& transition) noexcept override;

private:
    dataplane::RouteKernel* kernel_{nullptr};
    std::uint64_t account_context_id_{};
};

enum class AttemptAssignmentStatus : std::uint8_t {
    Assigned = 0U,
    Exhausted,
    InvalidFloor,
};

struct AttemptAssignment final {
    AttemptAssignmentStatus status{AttemptAssignmentStatus::Exhausted};
    std::uint64_t attempt_id{0U};
};

// Single-owner only. It deliberately has no atomic operations: sharing it across
// coordinators is a configuration error. IDs are never rolled back or reused.
class BoundedDispatchAttemptSource final {
public:
    constexpr BoundedDispatchAttemptSource(
        std::uint64_t first_attempt_id,
        std::uint64_t maximum_attempt_id) noexcept
        : next_attempt_id_(first_attempt_id),
          maximum_attempt_id_(maximum_attempt_id),
          exhausted_(first_attempt_id == 0U || maximum_attempt_id == 0U ||
                     first_attempt_id > maximum_attempt_id) {}

    BoundedDispatchAttemptSource(const BoundedDispatchAttemptSource&) = delete;
    BoundedDispatchAttemptSource& operator=(const BoundedDispatchAttemptSource&) = delete;
    BoundedDispatchAttemptSource(BoundedDispatchAttemptSource&&) = delete;
    BoundedDispatchAttemptSource& operator=(BoundedDispatchAttemptSource&&) = delete;

    [[nodiscard]] AttemptAssignment assign_after(std::uint64_t exclusive_floor) noexcept;

    [[nodiscard]] constexpr std::uint64_t next_candidate() const noexcept {
        return exhausted_ ? 0U : next_attempt_id_;
    }

private:
    std::uint64_t next_attempt_id_{0U};
    std::uint64_t maximum_attempt_id_{0U};
    bool exhausted_{false};
};

class INativeCancelFrameEncoder {
public:
    virtual ~INativeCancelFrameEncoder() = default;

    // Writes an exact native venue/session frame into caller-owned storage.
    [[nodiscard]] virtual FrameEncodeResult encode_cancel(
        const CancelCommand& command,
        const ActiveChildSnapshot& child,
        std::uint64_t dispatch_attempt_id,
        std::span<std::byte> destination) noexcept = 0;
};

class ICancelTxQueue {
public:
    virtual ~ICancelTxQueue() = default;

    [[nodiscard]] virtual std::size_t cancel_available_approx() const noexcept = 0;
    [[nodiscard]] virtual bool try_enqueue_cancel(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept = 0;
};

// Optional bridge to the existing cancel-first VenueTxQueues. The coordinator
// itself depends only on ICancelTxQueue, which keeps standalone tests small.
class VenueCancelQueueAdapter final : public ICancelTxQueue {
public:
    explicit constexpr VenueCancelQueueAdapter(VenueTxQueues& queues) noexcept : queues_(&queues) {}

    [[nodiscard]] std::size_t cancel_available_approx() const noexcept override {
        return queues_->cancel_available_approx();
    }

    [[nodiscard]] bool try_enqueue_cancel(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept override {
        return queues_->try_enqueue_cancel(frame, permit);
    }

private:
    VenueTxQueues* queues_{nullptr};
};

struct CancelVenueBinding final {
    VenueId venue_id{0U};
    ICancelTxQueue* cancel_queue{nullptr};
    INativeCancelFrameEncoder* encoder{nullptr};
};

class FixedCancelVenueRegistry final {
public:
    [[nodiscard]] bool bind(const CancelVenueBinding& binding) noexcept;
    void seal() noexcept { sealed_ = true; }

    [[nodiscard]] const CancelVenueBinding* find(VenueId venue_id) const noexcept;
    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::array<CancelVenueBinding, kMaxCancelVenues> bindings_{};
    std::size_t size_{0U};
    bool sealed_{false};
};

struct CancelRequestMetadata final {
    std::uint64_t producer_sequence{0U};
    std::uint64_t fence_time_ns{0U};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return producer_sequence != 0U;
    }
};

enum class CancelPipelineStatus : std::uint8_t {
    Enqueued = 0U,
    FailStopped,
    DuplicateAlreadyPending,
    AlreadyTerminal,
    InvalidCommand,
    PolicyInvalid,
    LeadershipFenceRejected,
    VenueUnavailable,
    OwnershipNotFound,
    OwnershipUnavailable,
    OwnershipMismatch,
    CancelAlreadyPending,
    RetryNotPending,
    CancelQueueFull,
    AttemptUnavailable,
    EncodeRejected,
    RecorderBusy,
    RecorderRejected,
    CompletionMissing,
    DurableTokenRejected,
    DualDurabilityRejected,
    OwnershipTransitionRejected,
    QueueInvariantViolation,
};

enum class CancelRecoveryPhase : std::uint8_t {
    None = 0U,
    DurableOnly,
    OwnershipMarked,
};

struct CancelRecoveryRequirement final {
    bool required{};
    std::uint64_t durable_batch_id{};
    std::uint64_t producer_sequence{};
    std::uint64_t child_id{};
    std::uint64_t dispatch_attempt_id{};
    CancelRecoveryPhase applied_phase{CancelRecoveryPhase::None};
    recorder::HashChainDigest batch_hash{};

    constexpr auto operator<=>(
        const CancelRecoveryRequirement&) const noexcept = default;
};

struct CancelPipelineResult final {
    CancelPipelineStatus status{CancelPipelineStatus::InvalidCommand};
    std::uint64_t dispatch_attempt_id{0U};
    recorder::DurableToken token{};
    CancelRecoveryRequirement recovery{};

    [[nodiscard]] constexpr bool enqueued() const noexcept {
        return status == CancelPipelineStatus::Enqueued;
    }
};

class CancelPipeline final {
public:
    CancelPipeline(
        recorder::IFlightRecorder& recorder,
        IActiveChildOwnership& ownership,
        BoundedDispatchAttemptSource& attempts,
        const FixedCancelVenueRegistry& venues,
        DispatchPipelinePolicy policy,
        ha::ReplicationGate* replication_gate = nullptr,
        IDualDurableReplicator* dual_replicator = nullptr) noexcept;

    [[nodiscard]] CancelPipelineResult try_cancel(
        const CancelCommand& command,
        const CancelRequestMetadata& metadata) noexcept;

    [[nodiscard]] bool fail_stopped() const noexcept;
    [[nodiscard]] CancelRecoveryRequirement recovery_requirement()
        const noexcept;

private:
    void require_recovery(
        const CancelCommand& command,
        std::uint64_t producer_sequence,
        const recorder::DurableToken* token,
        std::uint64_t dispatch_attempt_id,
        CancelRecoveryPhase applied_phase) noexcept;

    recorder::IFlightRecorder* recorder_{nullptr};
    IActiveChildOwnership* ownership_{nullptr};
    BoundedDispatchAttemptSource* attempts_{nullptr};
    const FixedCancelVenueRegistry* venues_{nullptr};
    DispatchPipelinePolicy policy_{};
    ha::ReplicationGate* replication_gate_{nullptr};
    IDualDurableReplicator* dual_replicator_{nullptr};
    recorder::CompletionSlot completion_{};
    CancelRecoveryRequirement recovery_{};
};

struct MassCancelCursor final {
    std::uint32_t next_slot{0U};
    std::uint32_t visited{0U};
    std::uint32_t emitted{0U};
    std::uint32_t maximum_visits{0U};
    std::uint32_t maximum_emits{0U};
};

enum class MassCancelNextStatus : std::uint8_t {
    Item = 0U,
    Done,
    VisitLimit,
    EmitLimit,
    Unavailable,
};

// A caller owns the cursor and submits each returned child through try_cancel.
// Implementations must advance monotonically and honor both limits.
class IBoundedMassCancelSource {
public:
    virtual ~IBoundedMassCancelSource() = default;
    [[nodiscard]] virtual MassCancelNextStatus next(
        MassCancelCursor& cursor,
        ActiveChildSnapshot& child) const noexcept = 0;
};

enum class CancelThenReplaceState : std::uint8_t {
    Idle = 0U,
    AwaitingCancelDispatch,
    AwaitingTerminalOrReconciliation,
    ReplacementEligible,
    ReplacementReleased,
    Aborted,
};

// This gate never encodes or transmits a replacement. It only releases an
// external replacement coordinator after terminal cancel evidence or explicit
// venue reconciliation for the exact owned child generation.
class CancelThenReplaceGate final {
public:
    [[nodiscard]] bool begin(
        const ActiveChildSnapshot& child,
        std::uint64_t replacement_request_id) noexcept;
    [[nodiscard]] bool note_cancel_dispatched(
        ChildOrderId child_id,
        std::uint64_t ownership_generation,
        std::uint64_t cancel_attempt_id) noexcept;
    [[nodiscard]] bool observe_terminal_or_reconciled(
        ChildOrderId child_id,
        std::uint64_t ownership_generation,
        ActiveChildState observed_state) noexcept;
    [[nodiscard]] bool try_release_replacement() noexcept;
    void abort() noexcept;

    [[nodiscard]] constexpr CancelThenReplaceState state() const noexcept { return state_; }
    [[nodiscard]] constexpr std::uint64_t replacement_request_id() const noexcept {
        return replacement_request_id_;
    }
    [[nodiscard]] constexpr std::uint64_t cancel_attempt_id() const noexcept {
        return cancel_attempt_id_;
    }

private:
    ChildOrderId child_id_{0U};
    std::uint64_t ownership_generation_{0U};
    std::uint64_t replacement_request_id_{0U};
    std::uint64_t cancel_attempt_id_{0U};
    CancelThenReplaceState state_{CancelThenReplaceState::Idle};
};

static_assert(std::is_trivially_copyable_v<CancelCommand>);
static_assert(std::is_trivially_copyable_v<ActiveChildSnapshot>);
static_assert(std::is_trivially_copyable_v<CancelPendingTransition>);
static_assert(std::is_trivially_copyable_v<MassCancelCursor>);

}  // namespace sorx::fastpath
