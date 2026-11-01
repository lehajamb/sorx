#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/recorder/flight_recorder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

using namespace sorx::recorder;

[[nodiscard]] std::array<std::byte, 4U> effect_frame(
    const std::uint64_t sequence,
    const std::size_t ordinal) noexcept {
    return {
        static_cast<std::byte>(sequence & 0xffU),
        static_cast<std::byte>(ordinal & 0xffU),
        std::byte{0xa5U},
        std::byte{0x5aU},
    };
}

[[nodiscard]] DurableBatch batch(
    const std::uint64_t sequence,
    const DurabilityLane lane = DurabilityLane::Normal,
    const std::string_view payload = "order") {
    DurableBatch result{lane, sequence};
    result.set_leadership_epoch(7U);
    result.set_configuration_generation(11U);
    assert(result.set_dispatch_authorization(
        DispatchAuthorization{1'000U + sequence, 10'000U + sequence, 2U}));
    for (std::size_t ordinal{}; ordinal < 2U; ++ordinal) {
        const auto frame = effect_frame(sequence, ordinal);
        assert(result.try_add_external_effect_authorization(
            static_cast<std::uint32_t>(100U + ordinal),
            1'000U + sequence + ordinal,
            10'000U + sequence + ordinal,
            20'000U + ordinal,
            30'000U + ordinal,
            1U,
            lane == DurabilityLane::Normal ? 0U : 1U,
            frame));
    }
    const std::span<const char> characters{payload.data(), payload.size()};
    assert(result.try_append_record(1U, 1U, std::as_bytes(characters)));
    assert(result.valid());
    return result;
}

[[nodiscard]] DurableToken append(
    InMemoryMirroredTier0& recorder,
    const DurableBatch& value) {
    CompletionSlot completion;
    assert(recorder.try_append(value, completion) == AppendStatus::Committed);
    assert(completion.state() == CompletionState::Committed);
    assert(completion.error() == CompletionError::None);
    const auto token = completion.token();
    assert(token.has_value());
    return *token;
}

[[nodiscard]] DurableBatch frozen_legacy_batch() {
    DurableBatch result{DurabilityLane::Cancel, 77U};
    result.set_leadership_epoch(7U);
    result.set_configuration_generation(11U);
    assert(result.set_authorization_envelope_version(
        AuthorizationEnvelopeVersion::LegacyRangeV0));
    assert(result.set_dispatch_authorization({123U, 456U, 2U}));
    const std::array<std::byte, 4U> payload{
        std::byte{0xdeU}, std::byte{0xadU},
        std::byte{0xbeU}, std::byte{0xefU}};
    assert(result.try_append_record(0x7777U, 9U, payload));
    assert(result.valid());
    return result;
}

class CollectingSink final : public IReplaySink {
public:
    bool on_batch(
        const DurableToken& token,
        const DurableBatch& value) noexcept override {
        if (count >= ids.size()) {
            return false;
        }
        ids[count] = token.batch_id;
        sequences[count] = value.producer_sequence();
        ++count;
        return true;
    }

    std::array<std::uint64_t, 16U> ids{};
    std::array<std::uint64_t, 16U> sequences{};
    std::size_t count{};
};

void test_fixed_batch_and_crc() {
    constexpr std::string_view check = "123456789";
    assert(crc32c(std::as_bytes(
               std::span<const char>{check.data(), check.size()})) ==
           0xe3069283U);

    DurableBatch maximum{DurabilityLane::Normal, 1U};
    maximum.set_leadership_epoch(7U);
    maximum.set_configuration_generation(11U);
    assert(maximum.set_dispatch_authorization({1U, 1U, 1U}));
    const std::array<std::byte, 1U> frame{std::byte{0x42U}};
    assert(maximum.try_add_external_effect_authorization(
        1U, 1U, 1U, 1U, 1U, 1U, 0U, frame));
    std::array<std::byte,
               kMaximumDurableBatchBytes - 8U>
        payload{};
    assert(maximum.try_append_record(9U, 2U, payload));
    assert(maximum.encoded_size() == kMaximumDurableBatchBytes);
    assert(!maximum.try_append_record(10U, 1U, {}));
    assert(maximum.valid());
    const auto stored = maximum.record(0U);
    assert(stored.has_value());
    assert(stored->type == 9U);
    assert(stored->version == 2U);
    assert(stored->payload.size() == payload.size());

    DurableBatch range_mismatch{DurabilityLane::Normal, 2U};
    range_mismatch.set_leadership_epoch(7U);
    range_mismatch.set_configuration_generation(11U);
    assert(range_mismatch.set_dispatch_authorization({10U, 20U, 2U}));
    assert(!range_mismatch.try_add_external_effect_authorization(
        1U, 11U, 21U, 1U, 1U, 1U, 0U, frame));
    assert(range_mismatch.try_add_external_effect_authorization(
        1U, 10U, 20U, 1U, 1U, 1U, 0U, frame));
    assert(!range_mismatch.try_add_external_effect_authorization(
        range_mismatch.external_effect_authorizations().entries[0U]));
    assert(range_mismatch.try_append_record(1U, 1U, frame));
    assert(!range_mismatch.valid());
}

void test_completion_and_token_gate() {
    InMemoryMirroredTier0 recorder{{4U, 1U, 1U, false}};
    CompletionSlot completion;
    assert(completion.state() == CompletionState::Empty);
    assert(!completion.token().has_value());

    const DurableBatch first = batch(1U);
    assert(recorder.try_append(first, completion) == AppendStatus::Committed);
    const auto token = completion.token();
    assert(token.has_value());
    assert(token->batch_id == 1U);
    assert(token->leadership_epoch == 7U);
    assert(token->configuration_generation == 11U);
    assert(token->dispatch_authorization ==
           (DispatchAuthorization{1'001U, 10'001U, 2U}));
    assert(token->authorizes_dispatch(1'001U, 10'001U, 7U, 11U));
    assert(token->authorizes_dispatch(1'002U, 10'002U, 7U, 11U));
    assert(!token->authorizes_dispatch(1'001U, 10'002U, 7U, 11U));
    assert(!token->authorizes_dispatch(1'002U, 10'001U, 7U, 11U));
    assert(!token->authorizes_dispatch(1'001U, 10'003U, 7U, 11U));
    assert(!token->authorizes_dispatch(1'001U, 10'001U, 8U, 11U));
    assert(token->domain == DurabilityDomain::VolatileSoftwareReference);
    assert(token->authorization_envelope_version ==
           AuthorizationEnvelopeVersion::ExactEffectsV1);
    assert(token->external_effect_authorizations.count == 2U);
    assert(recorder.authorizes_external_effect(
        *token, DurabilityDomain::VolatileSoftwareReference));
    assert(!recorder.authorizes_external_effect(
        *token, DurabilityDomain::HardwarePersistent));

    DurableToken tampered = *token;
    ++tampered.leadership_epoch;
    assert(!recorder.authorizes_external_effect(
        tampered, DurabilityDomain::VolatileSoftwareReference));
    tampered = *token;
    ++tampered.configuration_generation;
    assert(!recorder.authorizes_external_effect(
        tampered, DurabilityDomain::VolatileSoftwareReference));
    tampered = *token;
    ++tampered.dispatch_authorization.first_attempt_id;
    assert(!recorder.authorizes_external_effect(
        tampered, DurabilityDomain::VolatileSoftwareReference));
    tampered = *token;
    tampered.batch_hash[0] ^= std::byte{0x01U};
    assert(!recorder.authorizes_external_effect(
        tampered, DurabilityDomain::VolatileSoftwareReference));

    const auto first_permit = token->effect_permit(0U);
    const auto second_permit = token->effect_permit(1U);
    assert(first_permit.has_value());
    assert(second_permit.has_value());
    assert(!token->effect_permit(2U).has_value());
    const auto first_frame = effect_frame(1U, 0U);
    assert(first_permit->authorizes_external_effect(
        100U, 1'001U, 10'001U, 20'000U, 30'000U, 1U, 0U,
        first_frame));
    assert(recorder.authorizes_external_effect(
        *first_permit, DurabilityDomain::VolatileSoftwareReference));
    auto altered_frame = first_frame;
    altered_frame[0] ^= std::byte{0x01U};
    assert(!first_permit->authorizes_external_effect(
        100U, 1'001U, 10'001U, 20'000U, 30'000U, 1U, 0U,
        altered_frame));
    DurableEffectPermit altered_permit = *first_permit;
    ++altered_permit.authorization.venue_session_epoch;
    assert(!recorder.authorizes_external_effect(
        altered_permit, DurabilityDomain::VolatileSoftwareReference));
    altered_permit = *first_permit;
    altered_permit.authorization = second_permit->authorization;
    assert(!recorder.authorizes_external_effect(
        altered_permit, DurabilityDomain::VolatileSoftwareReference));
    assert(recorder.try_append(batch(2U), completion) ==
           AppendStatus::CompletionSlotBusy);

    assert(completion.reset());
    recorder.set_next_append_fault_for_testing(
        ReferenceFaultPoint::AfterPrimaryCommit);
    assert(recorder.try_append(batch(2U), completion) ==
           AppendStatus::TornCommitRejected);
    assert(completion.state() == CompletionState::Rejected);
    assert(completion.error() == CompletionError::TornCommit);
    assert(!completion.token().has_value());

    assert(completion.reset());
    assert(recorder.try_append(batch(2U), completion) ==
           AppendStatus::Committed);
    assert(completion.token()->batch_id == 2U);
}

void test_frozen_legacy_envelope_is_replay_only() {
    InMemoryMirroredTier0 recorder{{4U, 1U, 1U, false}};
    const DurableBatch legacy = frozen_legacy_batch();
    const DurableToken token = append(recorder, legacy);
    assert(token.authorization_envelope_version ==
           AuthorizationEnvelopeVersion::LegacyRangeV0);
    assert(token.external_effect_authorizations.count == 0U);
    assert(token.payload_crc32c == 0x89f95d25U);
    const HashChainDigest expected_batch_hash{
        std::byte{0x12U}, std::byte{0x0cU}, std::byte{0x08U},
        std::byte{0x32U}, std::byte{0xa8U}, std::byte{0x62U},
        std::byte{0x02U}, std::byte{0xe3U}, std::byte{0x87U},
        std::byte{0x5cU}, std::byte{0x6dU}, std::byte{0x3bU},
        std::byte{0xf5U}, std::byte{0xa6U}, std::byte{0x89U},
        std::byte{0x12U}, std::byte{0xc4U}, std::byte{0x96U},
        std::byte{0x73U}, std::byte{0xdaU}, std::byte{0xf5U},
        std::byte{0xb5U}, std::byte{0xa0U}, std::byte{0xe4U},
        std::byte{0xf6U}, std::byte{0x2aU}, std::byte{0x33U},
        std::byte{0xf9U}, std::byte{0xd1U}, std::byte{0x6bU},
        std::byte{0x05U}, std::byte{0x1bU},
    };
    const HashChainDigest expected_chain_hash{
        std::byte{0xfaU}, std::byte{0xa3U}, std::byte{0x2cU},
        std::byte{0xadU}, std::byte{0x79U}, std::byte{0x1eU},
        std::byte{0x0cU}, std::byte{0x44U}, std::byte{0xd2U},
        std::byte{0x65U}, std::byte{0x45U}, std::byte{0xb6U},
        std::byte{0xb6U}, std::byte{0x49U}, std::byte{0x66U},
        std::byte{0x82U}, std::byte{0x4fU}, std::byte{0x54U},
        std::byte{0x60U}, std::byte{0x6eU}, std::byte{0x8cU},
        std::byte{0x2bU}, std::byte{0xffU}, std::byte{0x32U},
        std::byte{0xe0U}, std::byte{0x3cU}, std::byte{0x54U},
        std::byte{0xeaU}, std::byte{0x12U}, std::byte{0x92U},
        std::byte{0xc8U}, std::byte{0xfbU},
    };
    assert(token.batch_hash == expected_batch_hash);
    assert(token.chain_hash == expected_chain_hash);
    assert(verify_durable_token(token, legacy));
    assert(!token.effect_permit(0U).has_value());
    assert(!recorder.authorizes_external_effect(
        token, DurabilityDomain::VolatileSoftwareReference));

    DurableBatch recovered;
    DurableToken recovered_token;
    assert(recorder.read(1U, recovered, recovered_token) == ReadStatus::Ok);
    assert(recovered_token == token);
    assert(recovered.authorization_envelope_version() ==
           AuthorizationEnvelopeVersion::LegacyRangeV0);
    assert(verify_durable_token(recovered_token, recovered));
    CollectingSink sink;
    assert(recorder.replay({1U, 1U}, sink) == ReplayStatus::Ok);
    assert(sink.count == 1U);
}

void test_corruption_and_torn_commit_rejection() {
    InMemoryMirroredTier0 corrupt{{4U, 1U, 1U, false}};
    const DurableToken corrupt_token = append(corrupt, batch(1U));
    assert(corrupt.corrupt_payload_for_testing(
        corrupt_token.batch_id, MirrorCopy::Secondary, 0U));
    assert(!corrupt.authorizes_external_effect(
        corrupt_token, DurabilityDomain::VolatileSoftwareReference));
    DurableBatch read_batch;
    DurableToken read_token;
    assert(corrupt.read(1U, read_batch, read_token) == ReadStatus::Corrupt);
    CollectingSink corrupt_sink;
    assert(corrupt.replay({1U, 1U}, corrupt_sink) == ReplayStatus::Corrupt);
    assert(corrupt_sink.count == 0U);

    InMemoryMirroredTier0 effect_mismatch{{4U, 1U, 1U, false}};
    const DurableToken effect_mismatch_token =
        append(effect_mismatch, batch(1U));
    assert(effect_mismatch.corrupt_external_effect_for_testing(
        effect_mismatch_token.batch_id, MirrorCopy::Secondary, 0U));
    assert(!effect_mismatch.authorizes_external_effect(
        effect_mismatch_token, DurabilityDomain::VolatileSoftwareReference));
    assert(effect_mismatch.read(1U, read_batch, read_token) ==
           ReadStatus::Corrupt);

    InMemoryMirroredTier0 effect_crc{{4U, 1U, 1U, false}};
    const DurableToken effect_crc_token = append(effect_crc, batch(1U));
    assert(effect_crc.corrupt_external_effect_for_testing(
        effect_crc_token.batch_id, MirrorCopy::Primary, 0U));
    assert(effect_crc.corrupt_external_effect_for_testing(
        effect_crc_token.batch_id, MirrorCopy::Secondary, 0U));
    assert(effect_crc.read(1U, read_batch, read_token) == ReadStatus::Corrupt);

    InMemoryMirroredTier0 torn{{4U, 1U, 1U, false}};
    const DurableToken torn_token = append(torn, batch(1U));
    assert(torn.tear_commit_for_testing(
        torn_token.batch_id, MirrorCopy::Secondary));
    assert(!torn.authorizes_external_effect(
        torn_token, DurabilityDomain::VolatileSoftwareReference));
    assert(torn.read(1U, read_batch, read_token) == ReadStatus::Corrupt);
}

void test_reserved_lanes_and_capacity() {
    InMemoryMirroredTier0 recorder{{5U, 1U, 1U, false}};
    static_cast<void>(append(recorder, batch(1U)));
    static_cast<void>(append(recorder, batch(2U)));
    static_cast<void>(append(recorder, batch(3U)));

    CompletionSlot normal_rejection;
    assert(recorder.try_append(batch(4U), normal_rejection) ==
           AppendStatus::CapacityReserved);
    assert(normal_rejection.error() == CompletionError::CapacityReserved);

    static_cast<void>(append(
        recorder, batch(4U, DurabilityLane::Cancel, "cancel")));
    static_cast<void>(append(
        recorder,
        batch(5U, DurabilityLane::Reconciliation, "reconcile")));
    CompletionSlot full_rejection;
    assert(recorder.try_append(
               batch(6U, DurabilityLane::Cancel), full_rejection) ==
           AppendStatus::CapacityExhausted);
    assert(full_rejection.error() == CompletionError::CapacityExhausted);

    const FlightRecorderMetadata metadata = recorder.metadata();
    assert(metadata.retained_batches == 5U);
    assert(metadata.normal_capacity == 3U);
    assert(metadata.protected_normal_batches == 3U);
    assert(metadata.protected_cancel_batches == 1U);
    assert(metadata.protected_reconciliation_batches == 1U);

    InMemoryMirroredTier0 borrowing{{5U, 1U, 1U, false}};
    static_cast<void>(append(
        borrowing, batch(1U, DurabilityLane::Cancel, "c1")));
    static_cast<void>(append(
        borrowing, batch(2U, DurabilityLane::Cancel, "c2")));
    static_cast<void>(append(
        borrowing, batch(3U, DurabilityLane::Cancel, "c3")));
    static_cast<void>(append(
        borrowing, batch(4U, DurabilityLane::Cancel, "c4")));
    CompletionSlot reserved_for_reconciliation;
    assert(borrowing.try_append(
               batch(5U, DurabilityLane::Cancel),
               reserved_for_reconciliation) ==
           AppendStatus::CapacityReserved);
    static_cast<void>(append(
        borrowing,
        batch(5U, DurabilityLane::Reconciliation, "last-reserve")));
}

void test_replay_ranges() {
    InMemoryMirroredTier0 recorder{{6U, 1U, 1U, false}};
    for (std::uint64_t sequence = 1U; sequence <= 4U; ++sequence) {
        static_cast<void>(append(recorder, batch(sequence)));
    }
    CollectingSink sink;
    assert(recorder.replay({2U, 3U}, sink) == ReplayStatus::Ok);
    assert(sink.count == 2U);
    assert(sink.ids[0] == 2U && sink.sequences[0] == 2U);
    assert(sink.ids[1] == 3U && sink.sequences[1] == 3U);

    CollectingSink unused;
    assert(recorder.replay({3U, 2U}, unused) ==
           ReplayStatus::InvalidRange);
    assert(recorder.replay({1U, 5U}, unused) ==
           ReplayStatus::NotRetained);
    InMemoryMirroredTier0 empty{{3U, 1U, 1U, false}};
    assert(empty.replay({1U, 1U}, unused) == ReplayStatus::Empty);
}

void test_retention_and_overwrite() {
    InMemoryMirroredTier0 recorder{{4U, 1U, 1U, false}};
    const DurableToken oldest = append(recorder, batch(1U));
    static_cast<void>(append(recorder, batch(2U)));
    static_cast<void>(append(
        recorder, batch(3U, DurabilityLane::Cancel, "cancel")));
    static_cast<void>(append(
        recorder,
        batch(4U, DurabilityLane::Reconciliation, "reconcile")));

    CompletionSlot blocked;
    assert(recorder.try_append(batch(5U), blocked) ==
           AppendStatus::CapacityExhausted);
    assert(recorder.acknowledge(1U) == AcknowledgeStatus::Advanced);
    const DurableToken replacement = append(recorder, batch(5U));
    assert(replacement.batch_id == 5U);

    const FlightRecorderMetadata metadata = recorder.metadata();
    assert(metadata.retained_batches == 4U);
    assert(metadata.retained.first_batch_id == 2U);
    assert(metadata.retained.last_batch_id == 5U);
    assert(metadata.overwritten_batches == 1U);
    assert(metadata.overwrite_eligible_through == 1U);
    assert(!recorder.authorizes_external_effect(
        oldest, DurabilityDomain::VolatileSoftwareReference));
    DurableBatch value;
    DurableToken token;
    assert(recorder.read(1U, value, token) == ReadStatus::NotRetained);
    CollectingSink sink;
    assert(recorder.replay({2U, 5U}, sink) == ReplayStatus::Ok);
    assert(sink.count == 4U);
}

void test_tier1_gate_and_archive() {
    InMemoryMirroredTier0 recorder{{3U, 1U, 1U, true}};
    const DurableToken oldest = append(recorder, batch(1U));
    static_cast<void>(append(
        recorder, batch(2U, DurabilityLane::Cancel, "cancel")));
    static_cast<void>(append(
        recorder,
        batch(3U, DurabilityLane::Reconciliation, "reconcile")));
    CompletionSlot not_drained;
    assert(recorder.try_append(batch(4U), not_drained) ==
           AppendStatus::CapacityExhausted);
    InMemoryTier1Archive archive{3U, 2U};
    assert(archive.durability_domain() ==
           DurabilityDomain::VolatileSoftwareReference);
    assert(recorder.drain_to_tier1({1U, 1U}, archive) == DrainStatus::Ok);
    assert(archive.batch_count() == 1U);
    assert(archive.segment_count() == 1U);
    const auto segment = archive.segment(0U);
    assert(segment.has_value());
    assert(segment->first_batch_id == 1U);
    assert(segment->last_batch_id == 1U);
    assert(segment->source_domain ==
           DurabilityDomain::VolatileSoftwareReference);

    DurableBatch archived_batch;
    DurableToken archived_token;
    assert(archive.read(1U, archived_batch, archived_token) == ReadStatus::Ok);
    assert(archived_token == oldest);
    assert(archived_batch.producer_sequence() == 1U);
    assert(archived_batch.external_effect_authorizations() ==
           oldest.external_effect_authorizations);
    const auto archived_permit = archived_token.effect_permit(0U);
    assert(archived_permit.has_value());
    const auto archived_frame = effect_frame(1U, 0U);
    assert(archived_permit->authorizes_external_effect(
        100U, 1'001U, 10'001U, 20'000U, 30'000U, 1U, 0U,
        archived_frame));
    assert(append(recorder, batch(4U)).batch_id == 4U);

    const FlightRecorderMetadata metadata = recorder.metadata();
    assert(metadata.tier1_required_before_overwrite);
    // Audit-client acknowledgement is observability only and never blocks a
    // Tier1-protected recycle decision.
    assert(metadata.acknowledged_through == 0U);
    assert(metadata.tier1_drained_through == 1U);
    assert(metadata.overwrite_eligible_through == 1U);
    assert(metadata.retained.first_batch_id == 2U);

    assert(archive.corrupt_external_effect_for_testing(1U, 0U));
    assert(archive.read(1U, archived_batch, archived_token) ==
           ReadStatus::Corrupt);
}

void test_tier1_transaction_and_chain_invariants() {
    InMemoryMirroredTier0 recorder{{6U, 1U, 1U, false}};
    const DurableBatch first_batch = batch(1U);
    const DurableBatch second_batch = batch(2U);
    const DurableBatch third_batch = batch(3U);
    const DurableToken first_token = append(recorder, first_batch);
    const DurableToken second_token = append(recorder, second_batch);
    const DurableToken third_token = append(recorder, third_batch);

    const Tier1SegmentMetadata first_segment{
        10U,
        first_token.batch_id,
        first_token.batch_id,
        1U,
        first_token.previous_hash,
        first_token.chain_hash,
        DurabilityDomain::VolatileSoftwareReference};
    const Tier1SegmentMetadata second_segment{
        11U,
        second_token.batch_id,
        second_token.batch_id,
        1U,
        second_token.previous_hash,
        second_token.chain_hash,
        DurabilityDomain::VolatileSoftwareReference};
    const Tier1SegmentMetadata third_segment{
        12U,
        third_token.batch_id,
        third_token.batch_id,
        1U,
        third_token.previous_hash,
        third_token.chain_hash,
        DurabilityDomain::VolatileSoftwareReference};

    InMemoryTier1Archive archive{3U, 3U};
    assert(archive.begin_segment(first_segment));
    assert(!archive.begin_segment(first_segment));
    assert(!archive.append_batch(second_token, second_batch));
    assert(archive.append_batch(first_token, first_batch));
    auto mismatched_commit = first_segment;
    mismatched_commit.final_hash[0] ^= std::byte{0x01U};
    assert(!archive.commit_segment(mismatched_commit));
    assert(archive.batch_count() == 0U);
    assert(archive.segment_count() == 0U);
    assert(!archive.begin_segment(first_segment));
    archive.abort_segment();
    assert(archive.batch_count() == 0U);
    assert(archive.segment_count() == 0U);

    assert(archive.begin_segment(first_segment));
    assert(archive.append_batch(first_token, first_batch));
    assert(archive.commit_segment(first_segment));
    assert(archive.batch_count() == 1U);
    assert(archive.segment_count() == 1U);

    assert(!archive.begin_segment(first_segment));
    auto older_segment_id = second_segment;
    older_segment_id.segment_id = 9U;
    assert(!archive.begin_segment(older_segment_id));
    auto skipped_segment_id = second_segment;
    skipped_segment_id.segment_id = 12U;
    assert(!archive.begin_segment(skipped_segment_id));
    auto overlapping_range = second_segment;
    overlapping_range.first_batch_id = first_token.batch_id;
    overlapping_range.last_batch_id = first_token.batch_id;
    overlapping_range.previous_hash = first_token.previous_hash;
    overlapping_range.final_hash = first_token.chain_hash;
    assert(!archive.begin_segment(overlapping_range));
    auto gapped_range = second_segment;
    gapped_range.first_batch_id = third_token.batch_id;
    gapped_range.last_batch_id = third_token.batch_id;
    gapped_range.previous_hash = third_token.previous_hash;
    gapped_range.final_hash = third_token.chain_hash;
    assert(!archive.begin_segment(gapped_range));
    auto broken_root = second_segment;
    broken_root.previous_hash[0] ^= std::byte{0x01U};
    assert(!archive.begin_segment(broken_root));

    assert(archive.begin_segment(second_segment));
    assert(!archive.append_batch(third_token, third_batch));
    assert(archive.append_batch(second_token, second_batch));
    auto wrong_descriptor = second_segment;
    ++wrong_descriptor.segment_id;
    assert(!archive.commit_segment(wrong_descriptor));
    assert(archive.batch_count() == 1U);
    assert(archive.segment_count() == 1U);
    assert(archive.commit_segment(second_segment));
    assert(archive.batch_count() == 2U);
    assert(archive.segment_count() == 2U);

    assert(archive.begin_segment(third_segment));
    assert(archive.append_batch(third_token, third_batch));
    archive.abort_segment();
    assert(archive.batch_count() == 2U);
    assert(archive.segment_count() == 2U);
    DurableBatch absent_batch;
    DurableToken absent_token;
    assert(archive.read(third_token.batch_id, absent_batch, absent_token) ==
           ReadStatus::NotRetained);
    assert(archive.begin_segment(third_segment));
    assert(archive.append_batch(third_token, third_batch));
    assert(archive.commit_segment(third_segment));
    assert(archive.batch_count() == 3U);
    assert(archive.segment_count() == 3U);
}

}  // namespace

int main() {
    test_fixed_batch_and_crc();
    test_completion_and_token_gate();
    test_frozen_legacy_envelope_is_replay_only();
    test_corruption_and_torn_commit_rejection();
    test_reserved_lanes_and_capacity();
    test_replay_ranges();
    test_retention_and_overwrite();
    test_tier1_gate_and_archive();
    test_tier1_transaction_and_chain_invariants();
    return 0;
}
