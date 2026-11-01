#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace sorx::recorder {

inline constexpr std::size_t kMaximumDurableBatchBytes = 4U * 1'024U;
inline constexpr std::size_t kMaximumDurableRecordsPerBatch = 32U;
inline constexpr std::size_t kMaximumExternalEffectAuthorizations = 32U;
inline constexpr std::size_t kMaximumAuthorizedFrameBytes = 512U;

using HashChainDigest = std::array<std::byte, 32U>;

enum class DurabilityLane : std::uint8_t {
    Normal,
    Cancel,
    Reconciliation,
};

// The reference recorder deliberately returns VolatileSoftwareReference.
// HardwarePersistent may only be returned by a backend that has completed a
// platform-specific power-fail persistence barrier (for example, real MRAM).
enum class DurabilityDomain : std::uint8_t {
    VolatileSoftwareReference,
    HardwarePersistent,
};

enum class AuthorizationEnvelopeVersion : std::uint8_t {
    // Historical range-only envelope. It remains replayable but can never
    // authorize transmission or issue a per-frame permit.
    LegacyRangeV0 = 0U,
    ExactEffectsV1 = 1U,
};

struct DurableRecordView final {
    std::uint16_t type{};
    std::uint16_t version{};
    std::span<const std::byte> payload;
};

// A token authorizes one aligned, contiguous range of (child, attempt) pairs.
// Pair N is (first_child_id + N, first_attempt_id + N). This lets one atomic
// parent/risk/children batch authorize all initially planned child sends while
// ensuring a retry, which has a new attempt id for an existing child, requires
// a new durable batch and token.
struct DispatchAuthorization final {
    std::uint64_t first_child_id{};
    std::uint64_t first_attempt_id{};
    std::uint32_t dispatch_count{};

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool authorizes(
        std::uint64_t candidate_child_id,
        std::uint64_t candidate_attempt_id) const noexcept;

    auto operator<=>(const DispatchAuthorization&) const = default;
};

// Exact pre-wire authorization generated before recorder append. The frame
// digest covers only the native bytes; all envelope/session fields below are
// independently bound into the recorder CRC and chained batch hash.
struct ExternalEffectAuthorization final {
    std::uint32_t venue_id{};
    std::uint64_t child_id{};
    std::uint64_t dispatch_attempt_id{};
    std::uint64_t venue_session_id{};
    std::uint64_t venue_session_epoch{};
    std::uint64_t ownership_generation{};
    std::uint8_t dispatch_class{};
    std::uint16_t frame_size{};
    HashChainDigest frame_hash{};

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const ExternalEffectAuthorization&) const = default;
};

struct ExternalEffectAuthorizationList final {
    std::array<ExternalEffectAuthorization,
               kMaximumExternalEffectAuthorizations>
        entries{};
    std::uint16_t count{};

    [[nodiscard]] std::span<const ExternalEffectAuthorization> values()
        const noexcept {
        return {entries.data(), count};
    }
    [[nodiscard]] bool valid(
        DispatchAuthorization dispatch_authorization) const noexcept;
    auto operator<=>(const ExternalEffectAuthorizationList&) const = default;
};

// Compact, per-frame capability narrowed from a committed master token. Queue
// entries carry one permit rather than copying the complete authorization list.
struct DurableEffectPermit final {
    std::uint64_t batch_id{};
    std::uint64_t producer_sequence{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint16_t effect_ordinal{};
    DurabilityLane lane{DurabilityLane::Normal};
    DurabilityDomain domain{DurabilityDomain::VolatileSoftwareReference};
    ExternalEffectAuthorization authorization{};
    HashChainDigest batch_hash{};

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] bool authorizes_external_effect(
        std::uint32_t candidate_venue_id,
        std::uint64_t candidate_child_id,
        std::uint64_t candidate_attempt_id,
        std::uint64_t candidate_venue_session_id,
        std::uint64_t candidate_venue_session_epoch,
        std::uint64_t candidate_ownership_generation,
        std::uint8_t candidate_dispatch_class,
        std::span<const std::byte> candidate_frame) const noexcept;

    auto operator<=>(const DurableEffectPermit&) const = default;
};

static_assert(std::is_trivially_copyable_v<ExternalEffectAuthorization>);
static_assert(std::is_trivially_copyable_v<ExternalEffectAuthorizationList>);
static_assert(std::is_trivially_copyable_v<DurableEffectPermit>);
// The portable permit is bounded to 2.5 conventional 64-byte cache lines.
// The much larger master list never belongs in a per-frame TX ring entry.
static_assert(sizeof(DurableEffectPermit) <= 160U);

class DurableBatch final {
public:
    explicit DurableBatch(
        DurabilityLane lane = DurabilityLane::Normal,
        std::uint64_t producer_sequence = 0U) noexcept;

    [[nodiscard]] bool try_append_record(
        std::uint16_t type,
        std::uint16_t version,
        std::span<const std::byte> payload) noexcept;
    void clear() noexcept;

    [[nodiscard]] DurabilityLane lane() const noexcept;
    [[nodiscard]] std::uint64_t producer_sequence() const noexcept;
    void set_producer_sequence(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t leadership_epoch() const noexcept;
    void set_leadership_epoch(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t configuration_generation() const noexcept;
    void set_configuration_generation(std::uint64_t value) noexcept;
    [[nodiscard]] bool set_authorization_envelope_version(
        AuthorizationEnvelopeVersion version) noexcept;
    [[nodiscard]] AuthorizationEnvelopeVersion
    authorization_envelope_version() const noexcept;
    [[nodiscard]] DispatchAuthorization dispatch_authorization() const noexcept;
    [[nodiscard]] bool set_dispatch_authorization(
        DispatchAuthorization authorization) noexcept;
    [[nodiscard]] bool try_add_external_effect_authorization(
        std::uint32_t venue_id,
        std::uint64_t child_id,
        std::uint64_t dispatch_attempt_id,
        std::uint64_t venue_session_id,
        std::uint64_t venue_session_epoch,
        std::uint64_t ownership_generation,
        std::uint8_t dispatch_class,
        std::span<const std::byte> frame) noexcept;
    // Adds a caller-prehashed authorization while preserving canonical range,
    // ordering, and duplicate checks. This is used by qualified hardware
    // encoders and recovery-integrity fixtures that already own the digest.
    [[nodiscard]] bool try_add_external_effect_authorization(
        const ExternalEffectAuthorization& authorization) noexcept;
    [[nodiscard]] const ExternalEffectAuthorizationList&
    external_effect_authorizations() const noexcept;
    [[nodiscard]] std::size_t record_count() const noexcept;
    [[nodiscard]] std::size_t encoded_size() const noexcept;
    [[nodiscard]] std::span<const std::byte> encoded_bytes() const noexcept;
    [[nodiscard]] std::optional<DurableRecordView> record(
        std::size_t index) const noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    friend class InMemoryMirroredTier0;

    [[nodiscard]] bool assign_encoded(
        DurabilityLane lane,
        std::uint64_t producer_sequence,
        std::uint64_t leadership_epoch,
        std::uint64_t configuration_generation,
        DispatchAuthorization dispatch_authorization,
        AuthorizationEnvelopeVersion authorization_envelope_version,
        ExternalEffectAuthorizationList external_effect_authorizations,
        std::uint16_t record_count,
        std::span<const std::byte> bytes) noexcept;

    DurabilityLane lane_{DurabilityLane::Normal};
    std::uint64_t producer_sequence_{};
    std::uint64_t leadership_epoch_{};
    std::uint64_t configuration_generation_{};
    DispatchAuthorization dispatch_authorization_{};
    AuthorizationEnvelopeVersion authorization_envelope_version_{
        AuthorizationEnvelopeVersion::ExactEffectsV1};
    ExternalEffectAuthorizationList external_effect_authorizations_{};
    std::uint16_t record_count_{};
    std::uint32_t encoded_size_{};
    std::array<std::uint32_t, kMaximumDurableRecordsPerBatch> record_offsets_{};
    std::array<std::byte, kMaximumDurableBatchBytes> encoded_{};
};

struct DurableToken final {
    std::uint64_t batch_id{};
    std::uint64_t producer_sequence{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    DispatchAuthorization dispatch_authorization{};
    AuthorizationEnvelopeVersion authorization_envelope_version{
        AuthorizationEnvelopeVersion::ExactEffectsV1};
    ExternalEffectAuthorizationList external_effect_authorizations{};
    std::uint32_t payload_crc32c{};
    std::uint32_t payload_size{};
    DurabilityLane lane{DurabilityLane::Normal};
    DurabilityDomain domain{DurabilityDomain::VolatileSoftwareReference};
    HashChainDigest batch_hash{};
    HashChainDigest previous_hash{};
    HashChainDigest chain_hash{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return batch_id != 0U && payload_size != 0U &&
               leadership_epoch != 0U && configuration_generation != 0U &&
               dispatch_authorization.valid() &&
               ((authorization_envelope_version ==
                     AuthorizationEnvelopeVersion::LegacyRangeV0 &&
                 external_effect_authorizations.count == 0U) ||
                (authorization_envelope_version ==
                     AuthorizationEnvelopeVersion::ExactEffectsV1 &&
                 external_effect_authorizations.valid(
                     dispatch_authorization)));
    }

    [[nodiscard]] bool authorizes_dispatch(
        std::uint64_t candidate_child_id,
        std::uint64_t candidate_attempt_id,
        std::uint64_t candidate_leadership_epoch,
        std::uint64_t candidate_configuration_generation) const noexcept;

    [[nodiscard]] bool authorizes_external_effect(
        std::uint32_t candidate_venue_id,
        std::uint64_t candidate_child_id,
        std::uint64_t candidate_attempt_id,
        std::uint64_t candidate_venue_session_id,
        std::uint64_t candidate_venue_session_epoch,
        std::uint64_t candidate_ownership_generation,
        std::uint8_t candidate_dispatch_class,
        std::span<const std::byte> candidate_frame) const noexcept;
    [[nodiscard]] std::optional<DurableEffectPermit> effect_permit(
        std::size_t ordinal) const noexcept;

    auto operator<=>(const DurableToken&) const = default;
};

static_assert(std::is_trivially_copyable_v<DurableToken>);
static_assert(sizeof(DurableToken) <= 3U * 1'024U);

enum class CompletionState : std::uint8_t {
    Empty,
    Pending,
    Committed,
    Rejected,
};

enum class CompletionError : std::uint8_t {
    None,
    InvalidBatch,
    CapacityReserved,
    CapacityExhausted,
    TornCommit,
    MirrorMismatch,
    BackendFailure,
};

// A caller-owned completion cell. Consumers must obtain their DurableToken
// through token(); it remains unavailable until the backend publishes
// Committed with release semantics.
class CompletionSlot final {
public:
    CompletionSlot() noexcept = default;
    CompletionSlot(const CompletionSlot&) = delete;
    CompletionSlot& operator=(const CompletionSlot&) = delete;

    [[nodiscard]] CompletionState state() const noexcept;
    [[nodiscard]] CompletionError error() const noexcept;
    [[nodiscard]] std::optional<DurableToken> token() const noexcept;
    [[nodiscard]] bool reset() noexcept;

private:
    friend class InMemoryMirroredTier0;

    [[nodiscard]] bool try_begin() noexcept;
    void publish_commit(const DurableToken& token) noexcept;
    void publish_rejection(CompletionError error) noexcept;

    std::atomic<CompletionState> state_{CompletionState::Empty};
    std::atomic<CompletionError> error_{CompletionError::None};
    DurableToken token_{};
};

enum class AppendStatus : std::uint8_t {
    Committed,
    CompletionSlotBusy,
    InvalidBatch,
    CapacityReserved,
    CapacityExhausted,
    TornCommitRejected,
    MirrorMismatch,
    BackendFailure,
};

enum class ReadStatus : std::uint8_t {
    Ok,
    InvalidBatchId,
    NotRetained,
    Corrupt,
};

struct ReplayRange final {
    std::uint64_t first_batch_id{};
    std::uint64_t last_batch_id{};
};

enum class ReplayStatus : std::uint8_t {
    Ok,
    Empty,
    InvalidRange,
    NotRetained,
    Corrupt,
    SinkStopped,
};

class IReplaySink {
public:
    virtual ~IReplaySink() = default;
    [[nodiscard]] virtual bool on_batch(
        const DurableToken& token,
        const DurableBatch& batch) noexcept = 0;
};

struct RetainedRange final {
    bool empty{true};
    std::uint64_t first_batch_id{};
    std::uint64_t last_batch_id{};
};

struct FlightRecorderMetadata final {
    std::size_t tier0_capacity{};
    std::size_t normal_capacity{};
    std::size_t cancel_reserved_capacity{};
    std::size_t reconciliation_reserved_capacity{};
    std::size_t retained_batches{};
    std::size_t protected_normal_batches{};
    std::size_t protected_cancel_batches{};
    std::size_t protected_reconciliation_batches{};
    std::uint64_t newest_batch_id{};
    std::uint64_t acknowledged_through{};
    std::uint64_t tier1_drained_through{};
    std::uint64_t overwrite_eligible_through{};
    std::uint64_t overwritten_batches{};
    std::uint64_t rejected_appends{};
    RetainedRange retained;
    HashChainDigest retained_anchor_hash{};
    HashChainDigest latest_chain_hash{};
    bool tier1_required_before_overwrite{};
    DurabilityDomain domain{DurabilityDomain::VolatileSoftwareReference};
};

enum class AcknowledgeStatus : std::uint8_t {
    Advanced,
    Unchanged,
    InvalidBatchId,
};

struct Tier1SegmentMetadata final {
    std::uint64_t segment_id{};
    std::uint64_t first_batch_id{};
    std::uint64_t last_batch_id{};
    std::uint32_t batch_count{};
    HashChainDigest previous_hash{};
    HashChainDigest final_hash{};
    DurabilityDomain source_domain{DurabilityDomain::VolatileSoftwareReference};
};

// A transactional segment sink. begin/append/commit are allocation-free for
// the in-memory reference below. A real implementation may map these calls to
// files, remote storage, or another durable tier.
class ITier1SegmentSink {
public:
    virtual ~ITier1SegmentSink() = default;
    [[nodiscard]] virtual bool begin_segment(
        const Tier1SegmentMetadata& metadata) noexcept = 0;
    [[nodiscard]] virtual bool append_batch(
        const DurableToken& token,
        const DurableBatch& batch) noexcept = 0;
    [[nodiscard]] virtual bool commit_segment(
        const Tier1SegmentMetadata& metadata) noexcept = 0;
    virtual void abort_segment() noexcept = 0;
};

enum class DrainStatus : std::uint8_t {
    Ok,
    Empty,
    InvalidRange,
    NotRetained,
    Corrupt,
    SinkRejected,
};

class IFlightRecorder {
public:
    virtual ~IFlightRecorder() = default;

    [[nodiscard]] virtual AppendStatus try_append(
        const DurableBatch& batch,
        CompletionSlot& completion) noexcept = 0;
    [[nodiscard]] virtual bool authorizes_external_effect(
        const DurableToken& token,
        DurabilityDomain required_domain) const noexcept = 0;
    [[nodiscard]] virtual bool authorizes_external_effect(
        const DurableEffectPermit& permit,
        DurabilityDomain required_domain) const noexcept = 0;
    [[nodiscard]] virtual ReadStatus read(
        std::uint64_t batch_id,
        DurableBatch& batch,
        DurableToken& token) const noexcept = 0;
    [[nodiscard]] virtual ReplayStatus replay(
        ReplayRange range,
        IReplaySink& sink) const noexcept = 0;
    [[nodiscard]] virtual AcknowledgeStatus acknowledge(
        std::uint64_t through_batch_id) noexcept = 0;
    [[nodiscard]] virtual DrainStatus drain_to_tier1(
        ReplayRange range,
        ITier1SegmentSink& sink) noexcept = 0;
    [[nodiscard]] virtual FlightRecorderMetadata metadata() const noexcept = 0;
    [[nodiscard]] virtual DurabilityDomain durability_domain() const noexcept = 0;
};

struct InMemoryTier0Configuration final {
    std::size_t capacity{};
    std::size_t cancel_reserved_capacity{};
    std::size_t reconciliation_reserved_capacity{};
    // Production semantics: only Tier1 durability advances overwrite
    // eligibility. Audit acknowledgements remain metadata and never gate it.
    // false exists only for isolated reference tests without a Tier1 sink.
    bool require_tier1_before_overwrite{};
};

enum class ReferenceFaultPoint : std::uint8_t {
    None,
    AfterPrimaryWrite,
    AfterPrimaryCommit,
};

enum class MirrorCopy : std::uint8_t {
    Primary,
    Secondary,
};

// This backend models mirrored Tier0 ordering and validation in ordinary RAM.
// It is useful as a deterministic reference and fault-injection target, but it
// provides no survival guarantee across process, OS, or power failure.
class InMemoryMirroredTier0 final : public IFlightRecorder {
public:
    explicit InMemoryMirroredTier0(InMemoryTier0Configuration configuration);
    ~InMemoryMirroredTier0() override;

    InMemoryMirroredTier0(const InMemoryMirroredTier0&) = delete;
    InMemoryMirroredTier0& operator=(const InMemoryMirroredTier0&) = delete;
    InMemoryMirroredTier0(InMemoryMirroredTier0&&) noexcept;
    InMemoryMirroredTier0& operator=(InMemoryMirroredTier0&&) noexcept;

    [[nodiscard]] AppendStatus try_append(
        const DurableBatch& batch,
        CompletionSlot& completion) noexcept override;
    [[nodiscard]] bool authorizes_external_effect(
        const DurableToken& token,
        DurabilityDomain required_domain) const noexcept override;
    [[nodiscard]] bool authorizes_external_effect(
        const DurableEffectPermit& permit,
        DurabilityDomain required_domain) const noexcept override;
    [[nodiscard]] ReadStatus read(
        std::uint64_t batch_id,
        DurableBatch& batch,
        DurableToken& token) const noexcept override;
    [[nodiscard]] ReplayStatus replay(
        ReplayRange range,
        IReplaySink& sink) const noexcept override;
    [[nodiscard]] AcknowledgeStatus acknowledge(
        std::uint64_t through_batch_id) noexcept override;
    [[nodiscard]] DrainStatus drain_to_tier1(
        ReplayRange range,
        ITier1SegmentSink& sink) noexcept override;
    [[nodiscard]] FlightRecorderMetadata metadata() const noexcept override;
    [[nodiscard]] DurabilityDomain durability_domain() const noexcept override;

    void set_next_append_fault_for_testing(ReferenceFaultPoint point) noexcept;
    [[nodiscard]] bool corrupt_payload_for_testing(
        std::uint64_t batch_id,
        MirrorCopy copy,
        std::size_t byte_offset) noexcept;
    [[nodiscard]] bool corrupt_external_effect_for_testing(
        std::uint64_t batch_id,
        MirrorCopy copy,
        std::size_t effect_ordinal) noexcept;
    [[nodiscard]] bool tear_commit_for_testing(
        std::uint64_t batch_id,
        MirrorCopy copy) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// File-free Tier1 transaction target for unit tests. Like the Tier0 reference,
// this class is volatile software storage and must never be described as MRAM,
// NVRAM, or power-fail durable.
class InMemoryTier1Archive final : public ITier1SegmentSink {
public:
    InMemoryTier1Archive(
        std::size_t batch_capacity,
        std::size_t segment_capacity);
    ~InMemoryTier1Archive() override;

    InMemoryTier1Archive(const InMemoryTier1Archive&) = delete;
    InMemoryTier1Archive& operator=(const InMemoryTier1Archive&) = delete;
    InMemoryTier1Archive(InMemoryTier1Archive&&) noexcept;
    InMemoryTier1Archive& operator=(InMemoryTier1Archive&&) noexcept;

    [[nodiscard]] bool begin_segment(
        const Tier1SegmentMetadata& metadata) noexcept override;
    [[nodiscard]] bool append_batch(
        const DurableToken& token,
        const DurableBatch& batch) noexcept override;
    [[nodiscard]] bool commit_segment(
        const Tier1SegmentMetadata& metadata) noexcept override;
    void abort_segment() noexcept override;

    [[nodiscard]] std::size_t batch_count() const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept;
    [[nodiscard]] std::optional<Tier1SegmentMetadata> segment(
        std::size_t index) const noexcept;
    [[nodiscard]] ReadStatus read(
        std::uint64_t batch_id,
        DurableBatch& batch,
        DurableToken& token) const noexcept;
    [[nodiscard]] bool corrupt_external_effect_for_testing(
        std::uint64_t batch_id,
        std::size_t effect_ordinal) noexcept;
    [[nodiscard]] DurabilityDomain durability_domain() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::uint32_t crc32c(
    std::span<const std::byte> bytes) noexcept;
// Allocation-free structural and cryptographic consistency check for an
// exported token/batch pair. It does not prove that a backend still retains
// the token; dispatch authorization must continue to use
// IFlightRecorder::authorizes_external_effect().
[[nodiscard]] bool verify_durable_token(
    const DurableToken& token,
    const DurableBatch& batch) noexcept;

}  // namespace sorx::recorder
