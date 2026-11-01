#pragma once

#include "sorx/dataplane/route_kernel.hpp"
#include "sorx/fastpath/risk_dispatch_pipeline.hpp"
#include "sorx/recorder/flight_recorder.hpp"
#include "sorx/risk/risk_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sorx::recovery {

inline constexpr std::uint16_t kRiskDispatchRecoveryMinimumCheckpointVersion =
    1U;
inline constexpr std::uint16_t kRiskDispatchRecoveryCheckpointVersion = 2U;

struct RiskDispatchReplayAnchor final {
    std::uint64_t through_batch_id{};
    std::uint64_t producer_sequence_high_water{};
    recorder::HashChainDigest chain_hash{};
};

enum class RiskDispatchCheckpointTrustPolicy : std::uint8_t {
    RequireExternalProvenance,
    ReferenceSelfChecked,
};

struct RiskDispatchCheckpointProvenanceContext final {
    recorder::HashChainDigest checkpoint_hash{};
    std::uint64_t expected_leadership_epoch{};
    std::uint64_t expected_configuration_generation{};
    recorder::DurabilityDomain expected_domain{
        recorder::DurabilityDomain::HardwarePersistent};
};

class IRiskDispatchCheckpointProvenanceVerifier {
public:
    virtual ~IRiskDispatchCheckpointProvenanceVerifier() = default;

    [[nodiscard]] virtual bool verify_checkpoint(
        std::span<const std::byte> encoded_checkpoint,
        const RiskDispatchCheckpointProvenanceContext& context)
        const noexcept = 0;
};

struct RiskDispatchRecoveryConfiguration final {
    std::size_t active_parent_capacity{};
    std::size_t replay_dedupe_capacity{};
    std::size_t risk_account_capacity{};
    std::size_t risk_client_capacity{};
    std::size_t risk_dedupe_capacity{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint64_t candidate_table_generation{};
    std::uint64_t risk_generation{};
    recorder::DurabilityDomain required_domain{
        recorder::DurabilityDomain::HardwarePersistent};
    RiskDispatchReplayAnchor replay_anchor{};
    RiskDispatchCheckpointTrustPolicy checkpoint_trust_policy{
        RiskDispatchCheckpointTrustPolicy::RequireExternalProvenance};
    const IRiskDispatchCheckpointProvenanceVerifier*
        checkpoint_provenance_verifier{};
};

enum class RiskDispatchReplayStatus : std::uint8_t {
    Applied,
    AlreadyApplied,
    NotReady,
    FailStopped,
    InvalidBatch,
    InvalidRecordSet,
    CanonicalDecodeFailed,
    TokenMismatch,
    IntegrityFailure,
    ChainDiscontinuity,
    BatchSequenceRegression,
    ProducerSequenceRegression,
    LeadershipEpochMismatch,
    ConfigurationMismatch,
    GenerationMismatch,
    DomainMismatch,
    CapacityExceeded,
    DuplicateConflict,
    RiskKernelRejected,
    RouteKernelRejected,
    PartialStateFailStop,
    ProvenanceRequired,
    ProvenanceRejected,
};

struct RiskDispatchReplayResult final {
    RiskDispatchReplayStatus status{RiskDispatchReplayStatus::NotReady};
    risk::RiskRecoveryImportStatus risk_import_status{
        risk::RiskRecoveryImportStatus::InvalidState};
    dataplane::RecoveryImportStatus route_import_status{
        dataplane::RecoveryImportStatus::InvalidReservation};
    fastpath::RecoveryCommitPhase observed_crash_phase{
        fastpath::RecoveryCommitPhase::None};
    std::uint64_t batch_id{};
    ParentOrderId parent{};
    std::uint16_t child_count{};

    [[nodiscard]] constexpr bool applied() const noexcept {
        return status == RiskDispatchReplayStatus::Applied ||
               status == RiskDispatchReplayStatus::AlreadyApplied;
    }
};

enum class RiskDispatchAccountingStatus : std::uint8_t {
    Applied,
    AlreadyApplied,
    NotReady,
    ParentNotFound,
    ChildNotFound,
    AttemptMismatch,
    InvalidQuantity,
    RiskKernelRejected,
    RouteKernelRejected,
    PartialStateFailStop,
};

struct RiskDispatchAccountingResult final {
    RiskDispatchAccountingStatus status{
        RiskDispatchAccountingStatus::NotReady};
    risk::RiskAccountingStatus risk_status{
        risk::RiskAccountingStatus::InvariantViolation};
    dataplane::RecoveryAccountingStatus route_status{
        dataplane::RecoveryAccountingStatus::InvariantViolation};
};

enum class RiskDispatchCheckpointEncodeStatus : std::uint8_t {
    Encoded,
    NotReady,
    BufferTooSmall,
    StateInvalid,
};

struct RiskDispatchCheckpointEncodeResult final {
    RiskDispatchCheckpointEncodeStatus status{
        RiskDispatchCheckpointEncodeStatus::NotReady};
    std::size_t required_size{};
    std::size_t encoded_size{};
    std::uint32_t crc32c{};
    recorder::HashChainDigest checkpoint_hash{};
};

enum class RiskDispatchCheckpointRestoreStatus : std::uint8_t {
    Restored,
    NotReady,
    LedgerNotEmpty,
    InvalidEncoding,
    IntegrityFailure,
    ConfigurationMismatch,
    GenerationMismatch,
    CapacityExceeded,
    RiskKernelRejected,
    RouteKernelRejected,
    PartialStateFailStop,
    ProvenanceRequired,
    ProvenanceRejected,
};

struct RiskDispatchCheckpointRestoreResult final {
    RiskDispatchCheckpointRestoreStatus status{
        RiskDispatchCheckpointRestoreStatus::NotReady};
    std::uint64_t replay_through_batch_id{};
    std::size_t restored_parents{};
    std::size_t restored_risk_clients{};
    std::size_t restored_risk_dedupe_entries{};
};

struct RiskDispatchRecoveryMetadata final {
    bool ready{};
    bool fail_stopped{};
    bool checkpoint_restored{};
    bool recovery_only{};
    std::uint64_t replayed_through_batch_id{};
    std::uint64_t producer_sequence_high_water{};
    recorder::HashChainDigest latest_chain_hash{};
    std::size_t active_parents{};
    std::size_t active_children{};
    std::size_t unreconciled_children{};
    std::size_t replay_dedupe_entries{};
    dataplane::RecoveryIdentifierState identifiers{};
    risk::RiskKernelMetadata risk{};
};

// Recovery-only coordinator for canonical RiskDispatchPipeline batches. It
// deliberately has no encoder, venue registry, queue, transport or recorder
// authorization interface; consequently replay can reconstruct state but can
// never emit or authorize an external effect.
class RiskDispatchRecoveryLedger final {
public:
    RiskDispatchRecoveryLedger(
        risk::RiskKernel& account_risk,
        dataplane::RouteKernel& route_kernel,
        RiskDispatchRecoveryConfiguration configuration);
    ~RiskDispatchRecoveryLedger();

    RiskDispatchRecoveryLedger(const RiskDispatchRecoveryLedger&) = delete;
    RiskDispatchRecoveryLedger& operator=(
        const RiskDispatchRecoveryLedger&) = delete;
    RiskDispatchRecoveryLedger(RiskDispatchRecoveryLedger&&) noexcept;
    RiskDispatchRecoveryLedger& operator=(
        RiskDispatchRecoveryLedger&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool fail_stopped() const noexcept;

    [[nodiscard]] RiskDispatchReplayResult replay_dispatch_batch(
        const recorder::DurableToken& token,
        const recorder::DurableBatch& batch,
        fastpath::RecoveryCommitPhase observed_crash_phase) noexcept;

    [[nodiscard]] RiskDispatchAccountingResult apply_recovered_fill(
        ParentOrderId parent,
        ChildOrderId child,
        std::uint64_t dispatch_attempt_id,
        Quantity filled_quantity) noexcept;
    [[nodiscard]] RiskDispatchAccountingResult note_recovered_cancel(
        ParentOrderId parent,
        ChildOrderId child,
        std::uint64_t prior_dispatch_attempt_id,
        std::uint64_t cancel_dispatch_attempt_id,
        std::uint64_t client_request_id,
        std::uint64_t client_sequence,
        std::uint64_t client_session_epoch) noexcept;
    [[nodiscard]] RiskDispatchAccountingResult reconcile_recovered_child(
        ParentOrderId parent,
        ChildOrderId child,
        std::uint64_t dispatch_attempt_id,
        dataplane::RecoveredChildDisposition disposition) noexcept;

    [[nodiscard]] std::size_t checkpoint_size_required() const noexcept;
    [[nodiscard]] RiskDispatchCheckpointEncodeResult encode_checkpoint(
        std::span<std::byte> destination) const noexcept;
    [[nodiscard]] RiskDispatchCheckpointRestoreResult restore_checkpoint(
        std::span<const std::byte> encoded) noexcept;

    [[nodiscard]] bool complete_reconciliation() noexcept;
    [[nodiscard]] RiskDispatchRecoveryMetadata metadata() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::recovery
