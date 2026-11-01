#pragma once

#include "sorx/recorder/flight_recorder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sorx::audit {

struct StreamMetadata final {
    bool empty{true};
    std::uint64_t earliest_sequence{};
    std::uint64_t latest_sequence{};
    std::uint64_t tier1_durable_through{};
    std::uint64_t overwrite_eligible_through{};
    std::uint64_t retention_seconds{};
    std::uint64_t recorder_capacity_bytes{};
    std::uint64_t recorder_retained_bytes{};
    std::uint64_t qualified_usable_retained_bytes{};
    std::uint64_t qualified_maximum_bytes_per_second{};
    std::uint64_t retention_qualification_generation{};
    std::size_t recorder_capacity_batches{};
    std::size_t recorder_retained_batches{};
    bool retention_qualification_verified{};
    bool acknowledgements_affect_retention{};
    recorder::DurabilityDomain durability_domain{
        recorder::DurabilityDomain::VolatileSoftwareReference};
    recorder::HashChainDigest retained_anchor_hash{};
    recorder::HashChainDigest latest_chain_hash{};
};

struct RecordEnvelope final {
    recorder::DurableToken token{};
    recorder::DurableBatch batch{};

    [[nodiscard]] bool valid() const noexcept {
        return recorder::verify_durable_token(token, batch);
    }
};

enum class StreamReadStatus : std::uint8_t {
    Ok,
    InvalidSequence,
    NotRetained,
    Corrupt,
};

class IAuditStreamSource {
public:
    virtual ~IAuditStreamSource() = default;
    [[nodiscard]] virtual StreamMetadata metadata() const noexcept = 0;
    [[nodiscard]] virtual StreamReadStatus read(
        std::uint64_t sequence,
        RecordEnvelope& envelope) const noexcept = 0;
    [[nodiscard]] virtual bool acknowledge(
        std::uint64_t highest_contiguous_sequence) noexcept = 0;
};

// The signature/profile verifier lives outside this allocation-free adapter.
// `authenticated` is its fail-closed handoff: the factory rejects an absent
// attestation and binds the exact capacity, accounting rate, retention and
// qualification generation into an immutable capability. The rate is in
// worst-case retained-storage accounting bytes: every occupied recorder slot
// is charged kMaximumDurableBatchBytes, independent of its encoded payload.
// Capacity and time arithmetic are checked here and rechecked against the
// recorder at use time.
struct RetentionQualificationEvidence final {
    bool authenticated{};
    std::uint64_t qualification_generation{};
    std::uint64_t usable_retained_bytes{};
    std::uint64_t qualified_maximum_bytes_per_second{};
    std::uint64_t guaranteed_retention_seconds{};
};

class VerifiedRetentionQualification final {
public:
    [[nodiscard]] static std::optional<VerifiedRetentionQualification>
    validate(RetentionQualificationEvidence evidence) noexcept;

    [[nodiscard]] std::uint64_t qualification_generation() const noexcept;
    [[nodiscard]] std::uint64_t usable_retained_bytes() const noexcept;
    [[nodiscard]] std::uint64_t qualified_maximum_bytes_per_second()
        const noexcept;
    [[nodiscard]] std::uint64_t guaranteed_retention_seconds() const noexcept;

private:
    explicit VerifiedRetentionQualification(
        RetentionQualificationEvidence evidence) noexcept;

    RetentionQualificationEvidence evidence_{};
};

// Adapts the recorder to the semantic core of Audit Stream Protocol v1.
// TLS, framing, access control and the dedicated Ethernet interface are
// transport responsibilities outside this allocation-free source adapter.
class RecorderAuditStreamSource final : public IAuditStreamSource {
public:
    explicit RecorderAuditStreamSource(
        recorder::IFlightRecorder& recorder) noexcept;
    RecorderAuditStreamSource(
        recorder::IFlightRecorder& recorder,
        VerifiedRetentionQualification qualification) noexcept;

    [[nodiscard]] StreamMetadata metadata() const noexcept override;
    [[nodiscard]] StreamReadStatus read(
        std::uint64_t sequence,
        RecordEnvelope& envelope) const noexcept override;
    [[nodiscard]] bool acknowledge(
        std::uint64_t highest_contiguous_sequence) noexcept override;

private:
    recorder::IFlightRecorder* recorder_{};
    std::optional<VerifiedRetentionQualification> qualification_;
};

struct CollectorSegment final {
    std::uint64_t first_sequence{};
    std::uint64_t last_sequence{};
    std::uint32_t record_count{};
    recorder::HashChainDigest previous_hash{};
    recorder::HashChainDigest root_hash{};
};

class ICollectorSink {
public:
    virtual ~ICollectorSink() = default;
    [[nodiscard]] virtual std::uint64_t highest_contiguous_sequence()
        const noexcept = 0;
    [[nodiscard]] virtual recorder::HashChainDigest highest_contiguous_hash()
        const noexcept = 0;
    [[nodiscard]] virtual bool begin_segment(
        const CollectorSegment& expected) noexcept = 0;
    [[nodiscard]] virtual bool append(
        const RecordEnvelope& envelope) noexcept = 0;
    [[nodiscard]] virtual bool commit_segment(
        const CollectorSegment& completed) noexcept = 0;
    virtual void abort_segment() noexcept = 0;
};

struct CollectRequest final {
    // Zero starts after the sink's highest contiguous sequence, or at the
    // source's earliest retained sequence for an empty sink.
    std::uint64_t first_sequence{};
    // Zero captures everything currently available (a live-tail caller polls
    // again); a nonzero value is an inclusive range endpoint.
    std::uint64_t last_sequence{};
    std::uint32_t maximum_records_per_segment{1'024U};
};

enum class CollectStatus : std::uint8_t {
    Collected,
    Empty,
    AlreadyCurrent,
    InvalidRange,
    NotRetained,
    SourceCorrupt,
    SinkRejected,
    AcknowledgeFailed,
};

struct CollectResult final {
    CollectStatus status{CollectStatus::InvalidRange};
    std::uint64_t first_collected{};
    std::uint64_t last_collected{};
    std::uint64_t next_sequence{};
    std::uint32_t segments{};
    std::uint64_t records{};

    [[nodiscard]] bool completed() const noexcept {
        return status == CollectStatus::Collected ||
            status == CollectStatus::AlreadyCurrent ||
            status == CollectStatus::Empty;
    }
};

class ReferenceCollector final {
public:
    ReferenceCollector(
        IAuditStreamSource& source,
        ICollectorSink& sink) noexcept;

    [[nodiscard]] CollectResult collect(CollectRequest request) noexcept;

private:
    IAuditStreamSource* source_{};
    ICollectorSink* sink_{};
};

}  // namespace sorx::audit
