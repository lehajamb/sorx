#pragma once

#include "sorx/fastpath/dispatch_pipeline.hpp"
#include "sorx/risk/risk_kernel.hpp"

#include <cstdint>
#include <span>

namespace sorx::fastpath {

enum class RiskDispatchRecordType : std::uint16_t {
    ClientAccountCommand = 0x1201U,
    AccountClientRiskDelta = 0x1202U,
    PinnedRouteInputs = 0x1203U,
    RouteDecision = 0x1204U,
    ExactChildFrames = 0x1205U,
};

inline constexpr std::uint16_t kRiskDispatchRecordVersion = 3U;
inline constexpr std::uint32_t kRiskDispatchSchemaMagic = 0x53524450U;  // SRDP

enum class RiskDispatchStatus : std::uint8_t {
    Dispatched,
    InvalidRequest,
    FailStopped,
    RegistryNotReady,
    RiskDuplicateReplay,
    RiskDuplicateConflict,
    RiskRejected,
    GenerationMismatch,
    RouteRejected,
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
    RiskCommitRejectedFailStop,
    RouteCommitRejectedFailStop,
    QueueOwnershipInvariantFailStop,
    RemainderReleaseRejectedFailStop,
    RemainderAccountingPreflightRejected,
};

enum class RecoveryCommitPhase : std::uint8_t {
    None = 0U,
    DurableOnly = 1U,
    RiskCommitted = 2U,
    RiskAndRouteCommitted = 3U,
    // Appended without renumbering the recovery phases persisted by v1.
    RiskAndRemainderReleased = 4U,
};

enum class RemainderAccountingStatus : std::uint8_t {
    NotRequired = 0U,
    Pending = 1U,
    Applied = 2U,
    ParentNotFound = 3U,
    InvalidQuantity = 4U,
    QuantityExceedsReservation = 5U,
    InvariantViolation = 6U,
    GenerationExhausted = 7U,
    RecoveryOnly = 8U,
};

// A non-empty value means the process must remain fenced and restart through
// durable replay. The batch is the authority: software commits are deliberately
// not described as transactional and no in-process rollback is attempted.
struct RiskDispatchRecoveryRequirement final {
    bool required{};
    // False means the recorder reported Committed but did not publish enough
    // token material to identify the durable batch by id and hash. Recovery
    // must then locate it by producer_sequence and schema/command identity.
    bool durable_locator_complete{};
    std::uint64_t durable_batch_id{};
    std::uint64_t producer_sequence{};
    RecoveryCommitPhase applied_phase{RecoveryCommitPhase::None};
    risk::RiskCommitStatus risk_commit_status{
        risk::RiskCommitStatus::NotCommittable};
    dataplane::CommitStatus route_commit_status{
        dataplane::CommitStatus::InvalidPlan};
    Quantity unrouted_remainder{};
    RemainderAccountingStatus remainder_accounting_status{
        RemainderAccountingStatus::NotRequired};
    recorder::HashChainDigest batch_hash{};
};

struct RiskDispatchResult final {
    RiskDispatchStatus status{RiskDispatchStatus::InvalidRequest};
    risk::RiskPlanDisposition risk_disposition{
        risk::RiskPlanDisposition::Invalid};
    risk::RiskVerdict risk_verdict{risk::RiskVerdict::InvalidCommand};
    risk::RiskCommitStatus risk_commit_status{
        risk::RiskCommitStatus::NotCommittable};
    dataplane::PlanStatus route_plan_status{
        dataplane::PlanStatus::InvalidInput};
    dataplane::CommitStatus route_commit_status{
        dataplane::CommitStatus::InvalidPlan};
    FrameEncodeStatus encode_status{FrameEncodeStatus::Failed};
    recorder::AppendStatus append_status{recorder::AppendStatus::InvalidBatch};
    ha::DispatchFenceError fence_error{ha::DispatchFenceError::None};
    recorder::DurableToken token{};
    std::uint16_t child_count{};
    Quantity unrouted_remainder{};
    RemainderAccountingStatus remainder_accounting_status{
        RemainderAccountingStatus::NotRequired};
    RiskDispatchRecoveryRequirement recovery{};

    [[nodiscard]] constexpr bool dispatched() const noexcept {
        return status == RiskDispatchStatus::Dispatched;
    }
};

// Single-owner composition of account/client risk, route/risk reservation,
// one durable dispatch batch and native TX queues. All dependencies are fixed
// before READY. try_dispatch() is bounded, noexcept, and allocation-free in
// pipeline-owned code.
class RiskDispatchPipeline final {
public:
    RiskDispatchPipeline(
        risk::RiskKernel& account_risk,
        dataplane::RouteKernel& route_kernel,
        recorder::IFlightRecorder& recorder,
        const FixedVenueRegistry& venues,
        DispatchPipelinePolicy policy,
        ha::ReplicationGate* replication_gate = nullptr,
        IDualDurableReplicator* dual_durable_replicator = nullptr) noexcept;

    [[nodiscard]] RiskDispatchResult try_dispatch(
        const risk::RiskCommand& command,
        const ParentOrder& parent,
        std::span<const dataplane::BookSlotView> pinned_views,
        DispatchRequestMetadata metadata) noexcept;

    [[nodiscard]] bool fail_stopped() const noexcept;
    [[nodiscard]] RiskDispatchRecoveryRequirement recovery_requirement()
        const noexcept;

private:
    [[nodiscard]] bool build_durable_batch(
        const risk::RiskDelta& risk_delta,
        const dataplane::RoutePlan& route_plan,
        std::span<const dataplane::BookSlotView> pinned_views,
        std::span<const EncodedFrame> frames,
        DispatchRequestMetadata metadata,
        recorder::DurableBatch& batch) const noexcept;

    void require_recovery(
        const recorder::DurableToken& token,
        std::uint64_t fallback_producer_sequence,
        RecoveryCommitPhase applied_phase,
        risk::RiskCommitStatus risk_status,
        dataplane::CommitStatus route_status,
        Quantity unrouted_remainder,
        RemainderAccountingStatus remainder_status) noexcept;

    risk::RiskKernel* account_risk_{};
    dataplane::RouteKernel* route_kernel_{};
    recorder::IFlightRecorder* recorder_{};
    const FixedVenueRegistry* venues_{};
    DispatchPipelinePolicy policy_{};
    ha::ReplicationGate* replication_gate_{};
    IDualDurableReplicator* dual_durable_replicator_{};
    recorder::CompletionSlot completion_{};
    RiskDispatchRecoveryRequirement recovery_{};
};

}  // namespace sorx::fastpath
