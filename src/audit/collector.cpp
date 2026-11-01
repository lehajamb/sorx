#include "sorx/audit/collector.hpp"

#include <algorithm>
#include <limits>

namespace sorx::audit {

std::optional<VerifiedRetentionQualification>
VerifiedRetentionQualification::validate(
    const RetentionQualificationEvidence evidence) noexcept {
    if (!evidence.authenticated ||
        evidence.qualification_generation == 0U ||
        evidence.usable_retained_bytes == 0U ||
        evidence.qualified_maximum_bytes_per_second == 0U ||
        evidence.guaranteed_retention_seconds == 0U ||
        evidence.guaranteed_retention_seconds >
            evidence.usable_retained_bytes /
                evidence.qualified_maximum_bytes_per_second) {
        return std::nullopt;
    }
    return VerifiedRetentionQualification{evidence};
}

VerifiedRetentionQualification::VerifiedRetentionQualification(
    const RetentionQualificationEvidence evidence) noexcept
    : evidence_(evidence) {}

std::uint64_t VerifiedRetentionQualification::qualification_generation()
    const noexcept {
    return evidence_.qualification_generation;
}

std::uint64_t VerifiedRetentionQualification::usable_retained_bytes()
    const noexcept {
    return evidence_.usable_retained_bytes;
}

std::uint64_t
VerifiedRetentionQualification::qualified_maximum_bytes_per_second()
    const noexcept {
    return evidence_.qualified_maximum_bytes_per_second;
}

std::uint64_t VerifiedRetentionQualification::guaranteed_retention_seconds()
    const noexcept {
    return evidence_.guaranteed_retention_seconds;
}

RecorderAuditStreamSource::RecorderAuditStreamSource(
    recorder::IFlightRecorder& flight_recorder) noexcept
    : recorder_(&flight_recorder) {}

RecorderAuditStreamSource::RecorderAuditStreamSource(
    recorder::IFlightRecorder& flight_recorder,
    VerifiedRetentionQualification qualification) noexcept
    : recorder_(&flight_recorder), qualification_(qualification) {}

StreamMetadata RecorderAuditStreamSource::metadata() const noexcept {
    StreamMetadata result;
    if (recorder_ == nullptr) {
        return result;
    }
    const auto source = recorder_->metadata();
    result.empty = source.retained.empty;
    result.earliest_sequence = source.retained.first_batch_id;
    result.latest_sequence = source.retained.last_batch_id;
    result.tier1_durable_through = source.tier1_drained_through;
    result.overwrite_eligible_through = source.overwrite_eligible_through;
    result.recorder_capacity_batches = source.tier0_capacity;
    result.recorder_retained_batches = source.retained_batches;
    result.durability_domain = source.domain;
    constexpr auto batch_accounting_bytes =
        static_cast<std::uint64_t>(recorder::kMaximumDurableBatchBytes);
    const auto accounted_bytes = [batch_accounting_bytes](
                                     const std::size_t slots) noexcept {
        return slots <= std::numeric_limits<std::uint64_t>::max() /
                            batch_accounting_bytes
            ? static_cast<std::uint64_t>(slots) * batch_accounting_bytes
            : 0U;
    };
    result.recorder_capacity_bytes =
        accounted_bytes(source.tier0_capacity);
    result.recorder_retained_bytes =
        accounted_bytes(source.retained_batches);
    if (qualification_.has_value()) {
        const auto usable = qualification_->usable_retained_bytes();
        const auto rate =
            qualification_->qualified_maximum_bytes_per_second();
        const auto retention =
            qualification_->guaranteed_retention_seconds();
        if (result.recorder_capacity_bytes != 0U && rate != 0U &&
            usable <= result.recorder_capacity_bytes &&
            retention <= usable / rate) {
            result.retention_seconds = retention;
            result.qualified_usable_retained_bytes = usable;
            result.qualified_maximum_bytes_per_second = rate;
            result.retention_qualification_generation =
                qualification_->qualification_generation();
            result.retention_qualification_verified = true;
        }
    }
    result.acknowledgements_affect_retention = false;
    result.retained_anchor_hash = source.retained_anchor_hash;
    result.latest_chain_hash = source.latest_chain_hash;
    return result;
}

StreamReadStatus RecorderAuditStreamSource::read(
    const std::uint64_t sequence,
    RecordEnvelope& envelope) const noexcept {
    if (recorder_ == nullptr || sequence == 0U) {
        return StreamReadStatus::InvalidSequence;
    }
    recorder::DurableBatch batch;
    recorder::DurableToken token;
    switch (recorder_->read(sequence, batch, token)) {
        case recorder::ReadStatus::Ok:
            envelope.batch = batch;
            envelope.token = token;
            return envelope.valid() ? StreamReadStatus::Ok
                                    : StreamReadStatus::Corrupt;
        case recorder::ReadStatus::InvalidBatchId:
            return StreamReadStatus::InvalidSequence;
        case recorder::ReadStatus::NotRetained:
            return StreamReadStatus::NotRetained;
        case recorder::ReadStatus::Corrupt:
            return StreamReadStatus::Corrupt;
    }
    return StreamReadStatus::Corrupt;
}

bool RecorderAuditStreamSource::acknowledge(
    const std::uint64_t highest_contiguous_sequence) noexcept {
    if (recorder_ == nullptr) {
        return false;
    }
    const auto result = recorder_->acknowledge(highest_contiguous_sequence);
    return result == recorder::AcknowledgeStatus::Advanced ||
        result == recorder::AcknowledgeStatus::Unchanged;
}

ReferenceCollector::ReferenceCollector(
    IAuditStreamSource& source,
    ICollectorSink& sink) noexcept
    : source_(&source), sink_(&sink) {}

CollectResult ReferenceCollector::collect(CollectRequest request) noexcept {
    CollectResult result;
    if (source_ == nullptr || sink_ == nullptr ||
        request.maximum_records_per_segment == 0U) {
        result.status = CollectStatus::InvalidRange;
        return result;
    }
    const StreamMetadata stream = source_->metadata();
    if (stream.empty) {
        result.status = CollectStatus::Empty;
        return result;
    }

    const std::uint64_t sink_highest =
        sink_->highest_contiguous_sequence();
    const recorder::HashChainDigest sink_hash =
        sink_->highest_contiguous_hash();
    const std::uint64_t sink_next =
        sink_highest == std::numeric_limits<std::uint64_t>::max()
            ? 0U
            : (sink_highest == 0U ? 0U : sink_highest + 1U);
    std::uint64_t first = request.first_sequence;
    if (first == 0U) {
        first = sink_next == 0U ? stream.earliest_sequence : sink_next;
    } else if (sink_next != 0U) {
        if (first > sink_next) {
            result.status = CollectStatus::InvalidRange;
            result.next_sequence = sink_next;
            return result;
        }
        // An overlapping resume request is idempotent, but only the first
        // sequence after the sink's verified contiguous prefix is fetched.
        first = sink_next;
    } else if (sink_highest == 0U && first != stream.earliest_sequence) {
        result.status = first < stream.earliest_sequence
            ? CollectStatus::NotRetained
            : CollectStatus::InvalidRange;
        result.next_sequence = stream.earliest_sequence;
        return result;
    }
    const std::uint64_t last = request.last_sequence == 0U
        ? stream.latest_sequence
        : request.last_sequence;
    result.next_sequence = first;
    if (first == 0U || last == 0U || last < first) {
        result.status = sink_highest >= last && last != 0U
            ? CollectStatus::AlreadyCurrent
            : CollectStatus::InvalidRange;
        return result;
    }
    if (first < stream.earliest_sequence) {
        result.status = CollectStatus::NotRetained;
        return result;
    }
    if (last > stream.latest_sequence) {
        result.status = CollectStatus::InvalidRange;
        return result;
    }

    result.first_collected = first;
    std::uint64_t cursor = first;
    recorder::HashChainDigest expected_previous =
        sink_highest == 0U ? stream.retained_anchor_hash : sink_hash;
    while (cursor <= last) {
        const std::uint64_t remaining = last - cursor + 1U;
        const std::uint64_t segment_count = std::min<std::uint64_t>(
            remaining,
            request.maximum_records_per_segment);
        CollectorSegment segment;
        segment.first_sequence = cursor;
        segment.last_sequence = cursor + segment_count - 1U;
        segment.record_count = static_cast<std::uint32_t>(segment_count);
        segment.previous_hash = expected_previous;
        if (!sink_->begin_segment(segment)) {
            result.status = CollectStatus::SinkRejected;
            return result;
        }

        for (std::uint64_t sequence = segment.first_sequence;; ++sequence) {
            RecordEnvelope envelope;
            switch (source_->read(sequence, envelope)) {
                case StreamReadStatus::Ok:
                    break;
                case StreamReadStatus::NotRetained:
                    sink_->abort_segment();
                    result.status = CollectStatus::NotRetained;
                    return result;
                case StreamReadStatus::InvalidSequence:
                    sink_->abort_segment();
                    result.status = CollectStatus::InvalidRange;
                    return result;
                case StreamReadStatus::Corrupt:
                    sink_->abort_segment();
                    result.status = CollectStatus::SourceCorrupt;
                    return result;
            }
            if (envelope.token.batch_id != sequence || !envelope.valid() ||
                envelope.token.previous_hash != expected_previous) {
                sink_->abort_segment();
                result.status = CollectStatus::SourceCorrupt;
                return result;
            }
            segment.root_hash = envelope.token.chain_hash;
            if (!sink_->append(envelope)) {
                sink_->abort_segment();
                result.status = CollectStatus::SinkRejected;
                return result;
            }
            expected_previous = envelope.token.chain_hash;
            if (sequence == segment.last_sequence) {
                break;
            }
        }
        if (!sink_->commit_segment(segment)) {
            sink_->abort_segment();
            result.status = CollectStatus::SinkRejected;
            return result;
        }
        if (sink_->highest_contiguous_sequence() != segment.last_sequence ||
            sink_->highest_contiguous_hash() != segment.root_hash) {
            result.status = CollectStatus::SinkRejected;
            return result;
        }
        if (!source_->acknowledge(segment.last_sequence)) {
            result.status = CollectStatus::AcknowledgeFailed;
            return result;
        }
        ++result.segments;
        result.records += segment.record_count;
        result.last_collected = segment.last_sequence;
        if (segment.last_sequence == std::numeric_limits<std::uint64_t>::max()) {
            result.next_sequence = 0U;
            break;
        }
        cursor = segment.last_sequence + 1U;
        result.next_sequence = cursor;
    }
    result.status = CollectStatus::Collected;
    return result;
}

}  // namespace sorx::audit
