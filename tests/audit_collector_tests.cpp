#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/audit/collector.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using namespace sorx;

recorder::DurableToken append(
    recorder::InMemoryMirroredTier0& target,
    const std::uint64_t sequence) {
    recorder::DurableBatch batch(recorder::DurabilityLane::Normal, sequence);
    batch.set_leadership_epoch(3U);
    batch.set_configuration_generation(5U);
    assert(batch.set_dispatch_authorization({sequence, sequence + 100U, 1U}));
    const std::array payload{static_cast<std::byte>(sequence)};
    assert(batch.try_add_external_effect_authorization(
        1U,
        sequence,
        sequence + 100U,
        1U,
        1U,
        1U,
        0U,
        payload));
    assert(batch.try_append_record(1U, 1U, payload));
    recorder::CompletionSlot completion;
    assert(target.try_append(batch, completion) ==
           recorder::AppendStatus::Committed);
    const auto token = completion.token();
    assert(token.has_value());
    return *token;
}

class FixedSink final : public audit::ICollectorSink {
public:
    [[nodiscard]] std::uint64_t highest_contiguous_sequence()
        const noexcept override {
        return highest;
    }

    [[nodiscard]] recorder::HashChainDigest highest_contiguous_hash()
        const noexcept override {
        return highest_hash;
    }

    [[nodiscard]] bool begin_segment(
        const audit::CollectorSegment& expected) noexcept override {
        if (active || segment_count >= roots.size()) {
            return false;
        }
        active = true;
        pending = expected;
        appended = 0U;
        return true;
    }

    [[nodiscard]] bool append(
        const audit::RecordEnvelope& envelope) noexcept override {
        if (!active || !envelope.valid() || fail_next_append) {
            fail_next_append = false;
            return false;
        }
        const std::uint64_t expected = pending.first_sequence + appended;
        if (envelope.token.batch_id != expected) {
            return false;
        }
        const auto expected_previous = appended == 0U
            ? pending.previous_hash
            : pending_root;
        if (envelope.token.previous_hash != expected_previous) {
            return false;
        }
        pending_root = envelope.token.chain_hash;
        ++appended;
        return true;
    }

    [[nodiscard]] bool commit_segment(
        const audit::CollectorSegment& completed) noexcept override {
        if (!active || appended != pending.record_count ||
            completed.first_sequence != pending.first_sequence ||
            completed.last_sequence != pending.last_sequence ||
            completed.previous_hash != pending.previous_hash ||
            completed.root_hash != pending_root) {
            return false;
        }
        roots[segment_count++] = completed.root_hash;
        highest = completed.last_sequence;
        highest_hash = completed.root_hash;
        active = false;
        return true;
    }

    void abort_segment() noexcept override {
        active = false;
        appended = 0U;
        ++aborts;
    }

    std::array<recorder::HashChainDigest, 8U> roots{};
    recorder::HashChainDigest highest_hash{};
    recorder::HashChainDigest pending_root{};
    audit::CollectorSegment pending{};
    std::uint64_t highest{};
    std::uint32_t appended{};
    std::uint32_t segment_count{};
    std::uint32_t aborts{};
    bool active{};
    bool fail_next_append{};
};

class FaultInjectingSource final : public audit::IAuditStreamSource {
public:
    enum class Fault : std::uint8_t {
        SwapFirstTwo,
        BreakHash,
    };

    FaultInjectingSource(audit::IAuditStreamSource& delegate, const Fault fault)
        : delegate_(&delegate), fault_(fault) {}

    [[nodiscard]] audit::StreamMetadata metadata() const noexcept override {
        return delegate_->metadata();
    }

    [[nodiscard]] audit::StreamReadStatus read(
        const std::uint64_t sequence,
        audit::RecordEnvelope& envelope) const noexcept override {
        std::uint64_t selected = sequence;
        if (fault_ == Fault::SwapFirstTwo) {
            if (sequence == 1U) selected = 2U;
            else if (sequence == 2U) selected = 1U;
        }
        const auto status = delegate_->read(selected, envelope);
        if (status == audit::StreamReadStatus::Ok &&
            fault_ == Fault::BreakHash) {
            envelope.token.previous_hash[0] ^= std::byte{0x01U};
        }
        return status;
    }

    [[nodiscard]] bool acknowledge(const std::uint64_t) noexcept override {
        return true;
    }

private:
    audit::IAuditStreamSource* delegate_{};
    Fault fault_{Fault::SwapFirstTwo};
};

}  // namespace

int main() {
    using namespace sorx;

    recorder::InMemoryMirroredTier0 recorder({8U, 1U, 1U, true});
    static_cast<void>(append(recorder, 1U));
    static_cast<void>(append(recorder, 2U));
    static_cast<void>(append(recorder, 3U));

    audit::RecorderAuditStreamSource source(recorder);
    const auto before = source.metadata();
    assert(!before.empty);
    assert(before.earliest_sequence == 1U);
    assert(before.latest_sequence == 3U);
    assert(before.retention_seconds == 0U);
    assert(before.recorder_capacity_bytes ==
           8U * recorder::kMaximumDurableBatchBytes);
    assert(before.recorder_retained_bytes ==
           3U * recorder::kMaximumDurableBatchBytes);
    assert(before.qualified_usable_retained_bytes == 0U);
    assert(before.qualified_maximum_bytes_per_second == 0U);
    assert(before.retention_qualification_generation == 0U);
    assert(!before.retention_qualification_verified);
    assert(before.durability_domain ==
           recorder::DurabilityDomain::VolatileSoftwareReference);
    assert(!before.acknowledgements_affect_retention);

    assert(!audit::VerifiedRetentionQualification::validate({
        false, 1U, 32'768U, 4'096U, 8U}));
    assert(!audit::VerifiedRetentionQualification::validate({
        true, 1U, 32'767U, 4'096U, 8U}));
    assert(!audit::VerifiedRetentionQualification::validate({
        true,
        1U,
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        2U}));

    const auto production_sized_claim =
        audit::VerifiedRetentionQualification::validate({
            true, 4U, 450'000'000'000ULL, 500'000'000U, 900U});
    assert(production_sized_claim.has_value());
    audit::RecorderAuditStreamSource tiny_source(
        recorder, *production_sized_claim);
    const auto tiny_metadata = tiny_source.metadata();
    assert(tiny_metadata.retention_seconds == 0U);
    assert(!tiny_metadata.retention_qualification_verified);
    assert(tiny_metadata.qualified_usable_retained_bytes == 0U);
    assert(tiny_metadata.qualified_maximum_bytes_per_second == 0U);

    const auto reference_qualification =
        audit::VerifiedRetentionQualification::validate({
            true,
            9U,
            8U * recorder::kMaximumDurableBatchBytes,
            recorder::kMaximumDurableBatchBytes,
            8U});
    assert(reference_qualification.has_value());
    audit::RecorderAuditStreamSource qualified_source(
        recorder, *reference_qualification);
    const auto qualified = qualified_source.metadata();
    assert(qualified.retention_qualification_verified);
    assert(qualified.retention_seconds == 8U);
    assert(qualified.qualified_usable_retained_bytes ==
           8U * recorder::kMaximumDurableBatchBytes);
    assert(qualified.qualified_maximum_bytes_per_second ==
           recorder::kMaximumDurableBatchBytes);
    assert(qualified.retention_qualification_generation == 9U);

    FixedSink skipped_sink;
    audit::ReferenceCollector skipped(source, skipped_sink);
    const auto skipped_result = skipped.collect({2U, 3U, 2U});
    assert(skipped_result.status == audit::CollectStatus::InvalidRange);
    assert(skipped_sink.highest == 0U);
    assert(recorder.metadata().acknowledged_through == 0U);

    FaultInjectingSource swapped_source{
        source, FaultInjectingSource::Fault::SwapFirstTwo};
    FixedSink swapped_sink;
    audit::ReferenceCollector swapped(swapped_source, swapped_sink);
    assert(swapped.collect({1U, 2U, 2U}).status ==
           audit::CollectStatus::SourceCorrupt);
    assert(swapped_sink.highest == 0U);
    assert(swapped_sink.aborts == 1U);

    FaultInjectingSource broken_source{
        source, FaultInjectingSource::Fault::BreakHash};
    FixedSink broken_sink;
    audit::ReferenceCollector broken(broken_source, broken_sink);
    assert(broken.collect({1U, 1U, 1U}).status ==
           audit::CollectStatus::SourceCorrupt);
    assert(broken_sink.highest == 0U);
    assert(broken_sink.aborts == 1U);

    FixedSink sink;
    audit::ReferenceCollector collector(source, sink);
    const auto result = collector.collect({1U, 3U, 2U});
    assert(result.status == audit::CollectStatus::Collected);
    assert(result.first_collected == 1U);
    assert(result.last_collected == 3U);
    assert(result.records == 3U);
    assert(result.segments == 2U);
    assert(sink.highest == 3U);

    const auto after = recorder.metadata();
    assert(after.acknowledged_through == 3U);
    assert(after.overwrite_eligible_through == 0U);

    const auto current = collector.collect({0U, 3U, 2U});
    assert(current.status == audit::CollectStatus::AlreadyCurrent);

    static_cast<void>(append(recorder, 4U));
    sink.fail_next_append = true;
    const auto failed = collector.collect({0U, 0U, 2U});
    assert(failed.status == audit::CollectStatus::SinkRejected);
    assert(sink.aborts == 1U);
    assert(sink.highest == 3U);
    assert(recorder.metadata().acknowledged_through == 3U);

    const auto resumed = collector.collect({0U, 0U, 2U});
    assert(resumed.status == audit::CollectStatus::Collected);
    assert(resumed.first_collected == 4U);
    assert(resumed.last_collected == 4U);
    assert(sink.highest == 4U);

    return 0;
}
