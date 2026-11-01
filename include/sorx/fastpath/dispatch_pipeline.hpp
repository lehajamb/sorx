#pragma once

#include "sorx/dataplane/route_kernel.hpp"
#include "sorx/dataplane/spsc_ring.hpp"
#include "sorx/fastpath/contracts.hpp"
#include "sorx/ha/replication.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <span>
#include <type_traits>

namespace sorx::fastpath {

inline constexpr std::size_t kMaximumDispatchVenues =
    dataplane::kMaximumDataplaneVenues;
inline constexpr std::size_t kNormalVenueQueueCapacity = 64U;
inline constexpr std::size_t kCancelVenueQueueCapacity = 16U;
inline constexpr std::size_t kMaximumRecordedDepthSlices = 64U;

enum class DispatchRecordType : std::uint16_t {
    ParentCommand = 0x1001U,
    PinnedRoutingInputs = 0x1002U,
    RouteAndRiskDecision = 0x1003U,
    ChildDispatches = 0x1004U,
};

// Version 2 adds adaptive-policy order binding, complete policy/fact pins,
// rank, score evidence, and exact hard-exclusion reason to PinnedRoutingInputs.
inline constexpr std::uint16_t kDispatchRecordVersion = 3U;

struct AuthorizedFrame final {
    EncodedFrame frame{};
    recorder::DurableEffectPermit permit{};
};

static_assert(std::is_trivially_copyable_v<AuthorizedFrame>);
static_assert(sizeof(AuthorizedFrame) <= 12U * 64U);

// One coordinator is the sole producer and one pinned TxOwner is the sole
// consumer. Cancellation/reconciliation capacity is physically independent
// from new/replacement capacity, so saturation of the latter cannot block the
// former.
class VenueTxQueues final {
public:
    [[nodiscard]] bool try_enqueue_normal(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept;
    [[nodiscard]] bool try_enqueue_cancel(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept;

    [[nodiscard]] bool try_pop_normal(AuthorizedFrame& output) noexcept;
    [[nodiscard]] bool try_pop_cancel(AuthorizedFrame& output) noexcept;

    [[nodiscard]] std::size_t normal_size_approx() const noexcept;
    [[nodiscard]] std::size_t cancel_size_approx() const noexcept;
    [[nodiscard]] std::size_t normal_available_approx() const noexcept;
    [[nodiscard]] std::size_t cancel_available_approx() const noexcept;

private:
    dataplane::SpscRing<AuthorizedFrame, kNormalVenueQueueCapacity> normal_{};
    dataplane::SpscRing<AuthorizedFrame, kCancelVenueQueueCapacity> cancel_{};
};

enum class FrameEncodeStatus : std::uint8_t {
    Encoded,
    InvalidInput,
    Unsupported,
    BufferTooSmall,
    Failed,
};

struct FrameEncodeResult final {
    FrameEncodeStatus status{FrameEncodeStatus::Failed};
    std::uint16_t size{};

    [[nodiscard]] constexpr bool encoded() const noexcept {
        return status == FrameEncodeStatus::Encoded && size != 0U;
    }
};

// A connector writes only the native venue payload. The pipeline owns and
// validates the venue, child, attempt, epoch, generation, and dispatch-class
// envelope so a connector cannot broaden a durable authorization.
class IChildFrameEncoder {
public:
    virtual ~IChildFrameEncoder() = default;
    [[nodiscard]] virtual FrameEncodeResult encode_new(
        const ParentOrder& parent,
        const ChildAllocation& allocation,
        const dataplane::PlannedChild& child,
        std::span<std::byte> destination) noexcept = 0;
};

struct VenueDispatchBinding final {
    VenueId venue{};
    VenueTxQueues* queues{};
    IChildFrameEncoder* encoder{};
};

enum class VenueRegistrationStatus : std::uint8_t {
    Registered,
    InvalidBinding,
    DuplicateVenue,
    CapacityExceeded,
    RegistrySealed,
};

struct VenueLookup final {
    const VenueDispatchBinding* binding{};
    std::uint16_t index{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return binding != nullptr;
    }
};

// Startup-only registry. Connector code and queue ownership are immutable once
// seal() succeeds; no dynamic connector replacement is possible while READY.
class FixedVenueRegistry final {
public:
    [[nodiscard]] VenueRegistrationStatus register_venue(
        VenueId venue,
        VenueTxQueues& queues,
        IChildFrameEncoder& encoder) noexcept;
    [[nodiscard]] bool seal() noexcept;

    [[nodiscard]] VenueLookup find(VenueId venue) const noexcept;
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::array<VenueDispatchBinding, kMaximumDispatchVenues> bindings_{};
    std::uint16_t count_{};
    bool sealed_{};
};

// The transport-specific HA replicator must not report success until the
// standby recorder has returned its durable token for this exact batch.
class IDualDurableReplicator {
public:
    virtual ~IDualDurableReplicator() = default;
    [[nodiscard]] virtual bool replicate_and_wait_durable(
        const recorder::DurableBatch& batch,
        const recorder::DurableToken& local_token) noexcept = 0;
};

struct DispatchPipelinePolicy final {
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    recorder::DurabilityDomain required_domain{
        recorder::DurabilityDomain::HardwarePersistent};
};

struct DispatchRequestMetadata final {
    std::uint64_t producer_sequence{};
    std::uint64_t decision_time_ns{};
    std::uint64_t commit_time_ns{};
    std::uint64_t fence_time_ns{};
};

enum class DispatchPipelineStatus : std::uint8_t {
    Dispatched,
    FailStopped,
    InvalidRequest,
    RegistryNotReady,
    PlanRejected,
    VenueUnavailable,
    NormalQueueCapacityUnavailable,
    FrameEncodingFailed,
    AuthorizationNotContiguous,
    DurableBatchCapacityExceeded,
    RecorderAppendRejected,
    DurableTokenUnavailable,
    DurableTokenRejected,
    DualDurabilityRejected,
    FenceRejected,
    KernelCommitRejected,
    QueueOwnershipInvariantViolated,
};

enum class DispatchRecoveryPhase : std::uint8_t {
    None,
    DurableOnly,
    KernelCommitted,
};

// Once a local durable authorization exists, any later failure is resolved by
// replay/reconciliation, never by continuing to route in the same process.
// A zero batch_id means append reported success but failed to publish a token,
// which is itself a recorder invariant violation requiring operator recovery.
struct DispatchRecoveryRequirement final {
    bool required{};
    std::uint64_t durable_batch_id{};
    std::uint64_t producer_sequence{};
    DispatchRecoveryPhase applied_phase{DispatchRecoveryPhase::None};
    dataplane::CommitStatus commit_status{
        dataplane::CommitStatus::InvalidPlan};
    recorder::HashChainDigest batch_hash{};

    constexpr auto operator<=>(
        const DispatchRecoveryRequirement&) const noexcept = default;
};

struct DispatchPipelineResult final {
    DispatchPipelineStatus status{DispatchPipelineStatus::InvalidRequest};
    dataplane::PlanStatus plan_status{dataplane::PlanStatus::InvalidInput};
    FrameEncodeStatus encode_status{FrameEncodeStatus::Failed};
    recorder::AppendStatus append_status{recorder::AppendStatus::InvalidBatch};
    dataplane::CommitStatus commit_status{dataplane::CommitStatus::InvalidPlan};
    ha::DispatchFenceError fence_error{ha::DispatchFenceError::None};
    recorder::DurableToken token{};
    std::uint16_t child_count{};
    DispatchRecoveryRequirement recovery{};

    [[nodiscard]] constexpr bool dispatched() const noexcept {
        return status == DispatchPipelineStatus::Dispatched;
    }
};

// Single-coordinator bounded route -> record -> commit -> enqueue pipeline.
// Construct and register dependencies before READY. Pipeline-owned code in
// try_dispatch() performs no general-heap allocation and never sleeps or polls
// a timer; production recorder/connector backends must honor the same hot-path
// contract.
class DispatchPipeline final {
public:
    DispatchPipeline(
        dataplane::RouteKernel& kernel,
        recorder::IFlightRecorder& recorder,
        const FixedVenueRegistry& venues,
        DispatchPipelinePolicy policy,
        ha::ReplicationGate* replication_gate = nullptr,
        IDualDurableReplicator* dual_durable_replicator = nullptr) noexcept;

    [[nodiscard]] DispatchPipelineResult try_dispatch(
        const ParentOrder& parent,
        std::span<const dataplane::BookSlotView> pinned_views,
        DispatchRequestMetadata metadata) noexcept;

    [[nodiscard]] bool fail_stopped() const noexcept;
    [[nodiscard]] DispatchRecoveryRequirement recovery_requirement()
        const noexcept;

private:
    [[nodiscard]] bool build_durable_batch(
        const dataplane::RoutePlan& plan,
        std::span<const dataplane::BookSlotView> pinned_views,
        std::span<const EncodedFrame> frames,
        DispatchRequestMetadata metadata,
        recorder::DurableBatch& batch) const noexcept;

    void require_recovery(
        std::uint64_t producer_sequence,
        const recorder::DurableToken* token,
        DispatchRecoveryPhase applied_phase,
        dataplane::CommitStatus commit_status) noexcept;

    dataplane::RouteKernel* kernel_{};
    recorder::IFlightRecorder* recorder_{};
    const FixedVenueRegistry* venues_{};
    DispatchPipelinePolicy policy_{};
    ha::ReplicationGate* replication_gate_{};
    IDualDurableReplicator* dual_durable_replicator_{};
    recorder::CompletionSlot completion_{};
    DispatchRecoveryRequirement recovery_{};
};

enum class TxPollResult : std::uint8_t {
    Idle,
    SentCancel,
    SentNormal,
    Backpressured,
    Rejected,
    RecoveryRequired,
};

struct TxRecoveryRequirement final {
    bool required{};
    bool delivery_ambiguous{};
    bool session_fatal{};
    bool cancel_or_reconcile{};
    std::uint64_t durable_batch_id{};
    std::uint64_t producer_sequence{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint32_t venue_id{};
    std::uint64_t child_id{};
    std::uint64_t dispatch_attempt_id{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    std::uint64_t ownership_generation{};
    std::uint16_t effect_ordinal{};
    recorder::DurabilityLane lane{recorder::DurabilityLane::Normal};
    recorder::DurabilityDomain domain{
        recorder::DurabilityDomain::VolatileSoftwareReference};
    SendResult rejection{SendResult::Sent};
    recorder::HashChainDigest frame_hash{};
    recorder::HashChainDigest batch_hash{};

    constexpr auto operator<=>(
        const TxRecoveryRequirement&) const noexcept = default;
};

struct TxOwnerStatistics final {
    std::uint64_t poll_iterations{};
    std::uint64_t sent_cancel{};
    std::uint64_t sent_normal{};
    std::uint64_t backpressured{};
    std::uint64_t rejected{};
    SendResult last_rejection{SendResult::Sent};
};

// Dedicated pinned owner. run_until_stopped() is an intentional busy loop:
// it never sleeps, waits on a condition variable, or inserts a timer delay.
class TxOwner final {
public:
    TxOwner(
        VenueTxQueues& queues,
        TokenGatedOrderEntrySession& session) noexcept;

    [[nodiscard]] TxPollResult poll_once() noexcept;
    void run_until_stopped(
        const std::atomic<bool>& stop_requested) noexcept;

    [[nodiscard]] TxOwnerStatistics statistics() const noexcept;
    [[nodiscard]] bool recovery_required() const noexcept;
    [[nodiscard]] TxRecoveryRequirement recovery_requirement() const noexcept;

private:
    [[nodiscard]] TxPollResult try_pending(
        AuthorizedFrame& pending,
        bool& has_pending,
        bool cancel) noexcept;
    void require_recovery(
        const AuthorizedFrame& frame,
        SendResult rejection,
        bool cancel) noexcept;

    VenueTxQueues* queues_{};
    TokenGatedOrderEntrySession* session_{};
    AuthorizedFrame pending_cancel_{};
    AuthorizedFrame pending_normal_{};
    bool has_pending_cancel_{};
    bool has_pending_normal_{};
    TxOwnerStatistics statistics_{};
    TxRecoveryRequirement recovery_{};
};

}  // namespace sorx::fastpath
