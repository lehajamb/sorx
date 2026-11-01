#pragma once

#include "sorx/dataplane/route_kernel.hpp"
#include "sorx/recorder/flight_recorder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sorx::recovery {

inline constexpr std::uint16_t kRecoveryCheckpointVersion = 2U;
inline constexpr std::size_t kRecoveryRequestDigestBytes = 16U;

using RecoveryRequestDigest =
    std::array<std::byte, kRecoveryRequestDigestBytes>;

struct RecoveryReplayAnchor final {
    std::uint64_t through_batch_id{};
    std::uint64_t producer_sequence_high_water{};
    recorder::HashChainDigest chain_hash{};
};

struct RecoveryConfiguration final {
    std::size_t active_parent_capacity{};
    std::size_t client_capacity{};
    std::size_t client_dedupe_capacity{};
    std::size_t replay_dedupe_capacity{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint64_t candidate_table_generation{};
    std::uint64_t risk_generation{};
    RecoveryReplayAnchor replay_anchor{};
};

enum class DispatchReplayStatus : std::uint8_t {
    Applied,
    AlreadyApplied,
    NotReady,
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
    CapacityExceeded,
    RouteKernelRejected,
};

struct DispatchReplayResult final {
    DispatchReplayStatus status{DispatchReplayStatus::NotReady};
    dataplane::RecoveryImportStatus import_status{
        dataplane::RecoveryImportStatus::InvalidReservation};
    std::uint64_t batch_id{};
    ParentOrderId parent{};
    std::uint16_t child_count{};

    [[nodiscard]] constexpr bool applied() const noexcept {
        return status == DispatchReplayStatus::Applied ||
               status == DispatchReplayStatus::AlreadyApplied;
    }
};

enum class RecoveryRecordType : std::uint8_t {
    Fill,
    CancelIntent,
    Terminal,
    ReconcileWorking,
    ReconcileTerminal,
};

// source_sequence is the monotonically increasing sequence in the durable
// source stream. client_id/client_sequence/request_digest provide a bounded
// duplicate-command window: equal payload replays are idempotent, while a
// changed payload at the same client sequence fails closed.
struct RecoveryRecord final {
    RecoveryRecordType type{RecoveryRecordType::Fill};
    std::uint64_t source_sequence{};
    std::uint64_t client_id{};
    std::uint64_t client_sequence{};
    RecoveryRequestDigest request_digest{};
    ParentOrderId parent{};
    ChildOrderId child{};
    std::uint64_t dispatch_attempt_id{};
    Quantity quantity{};
    std::uint64_t prior_dispatch_attempt_id{};
    std::uint64_t client_request_id{};
    std::uint64_t client_session_epoch{};
};

enum class RecoveryRecordStatus : std::uint8_t {
    Applied,
    DuplicateReplay,
    DuplicateConflict,
    TooOld,
    NotReady,
    InvalidRecord,
    CapacityExceeded,
    ParentNotFound,
    ChildNotFound,
    AttemptMismatch,
    InvariantViolation,
};

struct RecoveryRecordResult final {
    RecoveryRecordStatus status{RecoveryRecordStatus::NotReady};
    dataplane::RecoveryAccountingStatus accounting_status{
        dataplane::RecoveryAccountingStatus::InvariantViolation};

    [[nodiscard]] constexpr bool applied() const noexcept {
        return status == RecoveryRecordStatus::Applied ||
               status == RecoveryRecordStatus::DuplicateReplay;
    }
};

enum class CheckpointEncodeStatus : std::uint8_t {
    Encoded,
    NotReady,
    BufferTooSmall,
    StateInvalid,
};

struct CheckpointEncodeResult final {
    CheckpointEncodeStatus status{CheckpointEncodeStatus::NotReady};
    std::size_t required_size{};
    std::size_t encoded_size{};
    std::uint32_t crc32c{};
    recorder::HashChainDigest checkpoint_hash{};
};

enum class CheckpointRestoreStatus : std::uint8_t {
    Restored,
    NotReady,
    LedgerNotEmpty,
    InvalidEncoding,
    IntegrityFailure,
    ConfigurationMismatch,
    GenerationMismatch,
    CapacityExceeded,
    RouteKernelRejected,
};

struct CheckpointRestoreResult final {
    CheckpointRestoreStatus status{CheckpointRestoreStatus::NotReady};
    std::uint64_t replay_through_batch_id{};
    std::size_t restored_parents{};
    std::size_t restored_clients{};
    std::size_t restored_dedupe_entries{};
};

struct RecoveryMetadata final {
    bool ready{};
    bool checkpoint_restored{};
    bool recovery_only{};
    std::uint64_t replayed_through_batch_id{};
    std::uint64_t producer_sequence_high_water{};
    std::uint64_t recovery_source_sequence_high_water{};
    recorder::HashChainDigest latest_chain_hash{};
    std::size_t active_parents{};
    std::size_t active_children{};
    std::size_t unreconciled_children{};
    std::size_t clients{};
    std::size_t client_dedupe_entries{};
    dataplane::RecoveryIdentifierState identifiers{};
};

// Fixed-capacity recovery state machine. Construction is a cold operation and
// preallocates every table. replay_dispatch_batch(), apply_record(), and
// checkpoint encoding are noexcept and perform no heap allocation. The class
// has no transport/queue dependency and therefore cannot emit or authorize TX.
class RecoveryLedger final {
public:
    RecoveryLedger(
        dataplane::RouteKernel& kernel,
        RecoveryConfiguration configuration);
    ~RecoveryLedger();

    RecoveryLedger(const RecoveryLedger&) = delete;
    RecoveryLedger& operator=(const RecoveryLedger&) = delete;
    RecoveryLedger(RecoveryLedger&&) noexcept;
    RecoveryLedger& operator=(RecoveryLedger&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] DispatchReplayResult replay_dispatch_batch(
        const recorder::DurableToken& token,
        const recorder::DurableBatch& batch) noexcept;

    [[nodiscard]] RecoveryRecordResult apply_record(
        const RecoveryRecord& record) noexcept;

    [[nodiscard]] std::size_t checkpoint_size_required() const noexcept;
    [[nodiscard]] CheckpointEncodeResult encode_checkpoint(
        std::span<std::byte> destination) const noexcept;
    [[nodiscard]] CheckpointRestoreResult restore_checkpoint(
        std::span<const std::byte> encoded) noexcept;

    // Succeeds only when every still-active recovered child has an explicit
    // ReconcileWorking record. Terminal children are retired. Even on success,
    // RouteKernel remains killed and routing-disabled until the caller performs
    // its separate live-market/session readiness transition.
    [[nodiscard]] bool complete_reconciliation() noexcept;

    [[nodiscard]] RecoveryMetadata metadata() const noexcept;
    [[nodiscard]] std::uint64_t client_sequence_high_water(
        std::uint64_t client_id) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::recovery
