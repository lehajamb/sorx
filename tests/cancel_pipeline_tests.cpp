#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/fastpath/cancel_pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>

namespace {

std::atomic<std::uint64_t> allocation_calls{};

}  // namespace

void* operator new(const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void operator delete(void* const value) noexcept {
    std::free(value);
}

void operator delete[](void* const value) noexcept {
    std::free(value);
}

void operator delete(void* const value, const std::size_t) noexcept {
    std::free(value);
}

void operator delete[](void* const value, const std::size_t) noexcept {
    std::free(value);
}

namespace {

using namespace sorx;
using namespace sorx::fastpath;

constexpr VenueId kVenue{7U};
constexpr ParentOrderId kParent{200U};
constexpr ChildOrderId kChild{300U};
constexpr std::uint64_t kAccountContext = 17U;
constexpr std::uint64_t kOwnershipGeneration = 9U;
constexpr std::uint64_t kVenueSessionId = 33U;
constexpr std::uint64_t kVenueSessionEpoch = 5U;
constexpr std::uint64_t kLeadershipEpoch = 11U;
constexpr std::uint64_t kConfigurationGeneration = 13U;

void put_u32(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    destination[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    destination[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    destination[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    destination[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

void put_u64(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index{}; index < 8U; ++index) {
        const unsigned shift = static_cast<unsigned>((7U - index) * 8U);
        destination[offset + index] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] ActiveChildSnapshot working_child() noexcept {
    ActiveChildSnapshot child;
    child.account_context_id = kAccountContext;
    child.parent_order_id = kParent;
    child.child_id = kChild;
    child.venue_id = kVenue;
    child.venue_session_id = kVenueSessionId;
    child.venue_session_epoch = kVenueSessionEpoch;
    child.ownership_generation = kOwnershipGeneration;
    child.last_dispatch_attempt_id = 100U;
    child.wire_identity.size = 4U;
    child.wire_identity.bytes[0] = std::byte{'O'};
    child.wire_identity.bytes[1] = std::byte{'I'};
    child.wire_identity.bytes[2] = std::byte{'D'};
    child.wire_identity.bytes[3] = std::byte{'1'};
    child.state = ActiveChildState::Working;
    assert(child.valid());
    return child;
}

[[nodiscard]] CancelCommand cancel_command(
    const std::uint64_t request_id = 400U,
    const std::uint64_t sequence = 1U,
    const CancelRequestKind kind = CancelRequestKind::Cancel) noexcept {
    return CancelCommand{
        request_id,
        sequence,
        2U,
        kAccountContext,
        kParent,
        kChild,
        kVenue,
        kOwnershipGeneration,
        kVenueSessionId,
        kVenueSessionEpoch,
        19U,
        kind,
    };
}

[[nodiscard]] CancelRequestMetadata metadata(
    const std::uint64_t sequence = 1'000U) noexcept {
    return {sequence, 50U};
}

class DeterministicCancelEncoder final : public INativeCancelFrameEncoder {
public:
    [[nodiscard]] FrameEncodeResult encode_cancel(
        const CancelCommand& command,
        const ActiveChildSnapshot& child,
        const std::uint64_t dispatch_attempt_id,
        const std::span<std::byte> destination) noexcept override {
        constexpr std::size_t kFixedSize = 38U;
        ++calls;
        if (fail) {
            return {FrameEncodeStatus::Failed, 0U};
        }
        const std::size_t required = kFixedSize + child.wire_identity.size;
        if (!command.valid() || !child.valid() || dispatch_attempt_id == 0U) {
            return {FrameEncodeStatus::InvalidInput, 0U};
        }
        if (required > destination.size()) {
            return {FrameEncodeStatus::BufferTooSmall, 0U};
        }
        destination[0] = std::byte{'C'};
        put_u32(destination, 1U, command.venue_id.value());
        put_u64(destination, 5U, command.child_id.value());
        put_u64(destination, 13U, dispatch_attempt_id);
        put_u64(destination, 21U, child.venue_session_id);
        put_u64(destination, 29U, child.venue_session_epoch);
        destination[37U] = static_cast<std::byte>(child.wire_identity.size);
        std::copy_n(
            child.wire_identity.bytes.begin(),
            child.wire_identity.size,
            destination.begin() + kFixedSize);
        return {
            FrameEncodeStatus::Encoded,
            static_cast<std::uint16_t>(required),
        };
    }

    bool fail{false};
    std::uint64_t calls{0U};
};

class FixedCancelQueue final : public ICancelTxQueue {
public:
    [[nodiscard]] std::size_t cancel_available_approx() const noexcept override {
        return force_cancel_full ? 0U : entries.size() - size;
    }

    [[nodiscard]] bool try_enqueue_cancel(
        const EncodedFrame& frame,
        const recorder::DurableEffectPermit& permit) noexcept override {
        ++enqueue_calls;
        if (force_enqueue_failure || size == entries.size() || !frame.valid() ||
            !static_cast<bool>(permit) ||
            frame.dispatch_class != DispatchClass::CancelOrReconcile ||
            !permit.authorizes_external_effect(
                frame.venue_id,
                frame.child_id,
                frame.dispatch_attempt_id,
                frame.venue_session_id,
                frame.venue_session_epoch,
                frame.ownership_generation,
                static_cast<std::uint8_t>(frame.dispatch_class),
                frame.bytes())) {
            return false;
        }
        entries[size++] = AuthorizedFrame{frame, permit};
        return true;
    }

    std::array<AuthorizedFrame, 8U> entries{};
    std::size_t size{0U};
    std::uint64_t enqueue_calls{0U};
    bool normal_ring_full{true};
    bool force_cancel_full{false};
    bool force_enqueue_failure{false};
};

class FixedOwnership final : public IActiveChildOwnership {
public:
    explicit FixedOwnership(const ActiveChildSnapshot& initial) noexcept
        : child(initial) {}

    [[nodiscard]] OwnershipLookupResult lookup(
        const ChildOrderId child_id) const noexcept override {
        ++lookup_calls;
        if (unavailable) {
            return {OwnershipLookupStatus::Unavailable, {}};
        }
        if (!present || child_id != child.child_id) {
            return {OwnershipLookupStatus::NotFound, {}};
        }
        return {OwnershipLookupStatus::Found, child};
    }

    [[nodiscard]] OwnershipTransitionStatus mark_cancel_pending(
        const CancelPendingTransition& transition) noexcept override {
        ++transition_calls;
        last_transition = transition;
        if (forced_transition != OwnershipTransitionStatus::Marked) {
            return forced_transition;
        }
        if (!present || transition.expected.child_id != child.child_id ||
            transition.expected.parent_order_id != child.parent_order_id ||
            transition.expected.venue_id != child.venue_id ||
            transition.expected.ownership_generation != child.ownership_generation ||
            transition.expected.venue_session_id != child.venue_session_id ||
            transition.expected.venue_session_epoch != child.venue_session_epoch ||
            transition.expected.last_dispatch_attempt_id !=
                child.last_dispatch_attempt_id ||
            transition.expected.state != child.state ||
            transition.dispatch_attempt_id <= child.last_dispatch_attempt_id) {
            return OwnershipTransitionStatus::StaleSnapshot;
        }
        if (child.state == ActiveChildState::Terminal ||
            child.state == ActiveChildState::Reconciled) {
            return OwnershipTransitionStatus::Terminal;
        }
        if (child.state == ActiveChildState::CancelPending &&
            child.pending_client_request_id == transition.client_request_id &&
            child.pending_client_sequence == transition.client_sequence &&
            child.pending_client_session_epoch == transition.client_session_epoch) {
            return OwnershipTransitionStatus::AlreadyPendingSame;
        }
        child.state = ActiveChildState::CancelPending;
        child.pending_client_request_id = transition.client_request_id;
        child.pending_client_sequence = transition.client_sequence;
        child.pending_client_session_epoch = transition.client_session_epoch;
        child.pending_cancel_attempt_id = transition.dispatch_attempt_id;
        child.last_dispatch_attempt_id = transition.dispatch_attempt_id;
        assert(child.valid());
        return OwnershipTransitionStatus::Marked;
    }

    mutable std::uint64_t lookup_calls{0U};
    std::uint64_t transition_calls{0U};
    ActiveChildSnapshot child{};
    CancelPendingTransition last_transition{};
    OwnershipTransitionStatus forced_transition{OwnershipTransitionStatus::Marked};
    bool present{true};
    bool unavailable{false};
};

class CountingRecorder final : public recorder::IFlightRecorder {
public:
    explicit CountingRecorder(recorder::IFlightRecorder& inner) noexcept
        : inner_(&inner) {}

    [[nodiscard]] recorder::AppendStatus try_append(
        const recorder::DurableBatch& batch,
        recorder::CompletionSlot& completion) noexcept override {
        ++append_calls;
        return inner_->try_append(batch, completion);
    }

    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableToken& token,
        const recorder::DurabilityDomain required) const noexcept override {
        return inner_->authorizes_external_effect(token, required);
    }
    [[nodiscard]] bool authorizes_external_effect(
        const recorder::DurableEffectPermit& permit,
        const recorder::DurabilityDomain required) const noexcept override {
        return inner_->authorizes_external_effect(permit, required);
    }

    [[nodiscard]] recorder::ReadStatus read(
        const std::uint64_t batch_id,
        recorder::DurableBatch& batch,
        recorder::DurableToken& token) const noexcept override {
        return inner_->read(batch_id, batch, token);
    }

    [[nodiscard]] recorder::ReplayStatus replay(
        const recorder::ReplayRange range,
        recorder::IReplaySink& sink) const noexcept override {
        return inner_->replay(range, sink);
    }

    [[nodiscard]] recorder::AcknowledgeStatus acknowledge(
        const std::uint64_t through) noexcept override {
        return inner_->acknowledge(through);
    }

    [[nodiscard]] recorder::DrainStatus drain_to_tier1(
        const recorder::ReplayRange range,
        recorder::ITier1SegmentSink& sink) noexcept override {
        return inner_->drain_to_tier1(range, sink);
    }

    [[nodiscard]] recorder::FlightRecorderMetadata metadata() const noexcept override {
        return inner_->metadata();
    }

    [[nodiscard]] recorder::DurabilityDomain durability_domain() const noexcept override {
        return inner_->durability_domain();
    }

    std::uint64_t append_calls{0U};

private:
    recorder::IFlightRecorder* inner_{nullptr};
};

class FixedDualReplicator final : public IDualDurableReplicator {
public:
    [[nodiscard]] bool replicate_and_wait_durable(
        const recorder::DurableBatch& batch,
        const recorder::DurableToken& local_token) noexcept override {
        ++calls;
        last_producer_sequence = batch.producer_sequence();
        last_batch_id = local_token.batch_id;
        return succeed;
    }

    bool succeed{false};
    std::uint64_t calls{0U};
    std::uint64_t last_producer_sequence{0U};
    std::uint64_t last_batch_id{0U};
};

struct Fixture final {
    explicit Fixture(
        const recorder::DurabilityDomain required =
            recorder::DurabilityDomain::VolatileSoftwareReference,
        const std::uint64_t first_attempt = 101U,
        ha::ReplicationGate* const replication = nullptr,
        IDualDurableReplicator* const dual = nullptr)
        : tier0({64U, 16U, 4U, false}),
          recorder(tier0),
          ownership(working_child()),
          attempts(first_attempt, 1'000U),
          policy{kLeadershipEpoch, kConfigurationGeneration, required},
          pipeline(
              recorder,
              ownership,
              attempts,
              registry,
              policy,
              replication,
              dual) {
        assert(registry.bind({kVenue, &queue, &encoder}));
        registry.seal();
    }

    recorder::InMemoryMirroredTier0 tier0;
    CountingRecorder recorder;
    FixedOwnership ownership;
    BoundedDispatchAttemptSource attempts;
    FixedCancelQueue queue;
    DeterministicCancelEncoder encoder;
    FixedCancelVenueRegistry registry;
    DispatchPipelinePolicy policy;
    CancelPipeline pipeline;
};

void assert_record_ends_with_frame(
    const recorder::DurableBatch& batch,
    const EncodedFrame& frame) {
    assert(batch.record_count() == 1U);
    const auto record = batch.record(0U);
    assert(record.has_value());
    assert(record->type == kCancelDispatchRecordType);
    assert(record->version == kCancelDispatchRecordVersion);
    assert(record->payload.size() >= frame.size);
    const auto suffix = record->payload.last(frame.size);
    assert(std::equal(suffix.begin(), suffix.end(), frame.bytes().begin()));
}

void test_success_is_atomic_allocation_free_and_uses_reserved_lane() {
    Fixture fixture;
    assert(fixture.queue.normal_ring_full);
    const std::uint64_t allocations_before =
        allocation_calls.load(std::memory_order_relaxed);
    const CancelPipelineResult result =
        fixture.pipeline.try_cancel(cancel_command(), metadata());
    const std::uint64_t allocations_after =
        allocation_calls.load(std::memory_order_relaxed);

    assert(allocations_after == allocations_before);
    assert(result.enqueued());
    assert(result.dispatch_attempt_id == 101U);
    assert(fixture.recorder.append_calls == 1U);
    assert(fixture.ownership.transition_calls == 1U);
    assert(fixture.ownership.child.state == ActiveChildState::CancelPending);
    assert(fixture.ownership.child.pending_cancel_attempt_id == 101U);
    assert(fixture.queue.enqueue_calls == 1U);
    assert(fixture.queue.size == 1U);

    const AuthorizedFrame& authorized = fixture.queue.entries[0U];
    assert(authorized.frame.dispatch_class == DispatchClass::CancelOrReconcile);
    assert(authorized.frame.storage[0] == std::byte{'C'});
    assert(authorized.frame.dispatch_attempt_id == 101U);
    const auto expected_permit = result.token.effect_permit(0U);
    assert(expected_permit.has_value());
    assert(authorized.permit == *expected_permit);
    assert(result.token.lane == recorder::DurabilityLane::Cancel);
    const recorder::DispatchAuthorization first_expected{
        kChild.value(), 101U, 1U};
    assert(result.token.dispatch_authorization == first_expected);
    assert(fixture.tier0.authorizes_external_effect(
        result.token,
        recorder::DurabilityDomain::VolatileSoftwareReference));

    recorder::DurableBatch recorded;
    recorder::DurableToken recorded_token;
    assert(fixture.tier0.read(result.token.batch_id, recorded, recorded_token) ==
           recorder::ReadStatus::Ok);
    assert(recorded.lane() == recorder::DurabilityLane::Cancel);
    assert(recorded.producer_sequence() == metadata().producer_sequence);
    assert_record_ends_with_frame(recorded, authorized.frame);

    Fixture repeated;
    const CancelPipelineResult repeated_result =
        repeated.pipeline.try_cancel(cancel_command(), metadata());
    assert(repeated_result.enqueued());
    recorder::DurableBatch repeated_batch;
    recorder::DurableToken repeated_token;
    assert(repeated.tier0.read(
               repeated_result.token.batch_id, repeated_batch, repeated_token) ==
           recorder::ReadStatus::Ok);
    assert(recorded.encoded_size() == repeated_batch.encoded_size());
    assert(std::equal(
        recorded.encoded_bytes().begin(),
        recorded.encoded_bytes().end(),
        repeated_batch.encoded_bytes().begin()));
    assert(recorded_token == repeated_token);
}

void test_duplicate_is_idempotent_and_retry_gets_new_recorded_attempt() {
    Fixture fixture;
    const CancelCommand initial = cancel_command();
    const CancelPipelineResult first =
        fixture.pipeline.try_cancel(initial, metadata());
    assert(first.enqueued());

    const std::uint64_t next_before_duplicate = fixture.attempts.next_candidate();
    const CancelPipelineResult duplicate =
        fixture.pipeline.try_cancel(initial, metadata());
    assert(duplicate.status == CancelPipelineStatus::DuplicateAlreadyPending);
    assert(duplicate.dispatch_attempt_id == first.dispatch_attempt_id);
    assert(fixture.attempts.next_candidate() == next_before_duplicate);
    assert(fixture.recorder.append_calls == 1U);
    assert(fixture.queue.size == 1U);

    const CancelCommand retry = cancel_command(401U, 2U, CancelRequestKind::Retry);
    const CancelPipelineResult retried =
        fixture.pipeline.try_cancel(retry, metadata(1'001U));
    assert(retried.enqueued());
    assert(retried.dispatch_attempt_id == 102U);
    assert(retried.dispatch_attempt_id != first.dispatch_attempt_id);
    assert(retried.token.batch_id != first.token.batch_id);
    const recorder::DispatchAuthorization retry_expected{
        kChild.value(), 102U, 1U};
    assert(retried.token.dispatch_authorization == retry_expected);
    assert(fixture.recorder.append_calls == 2U);
    assert(fixture.queue.size == 2U);

    const CancelPipelineResult retry_duplicate =
        fixture.pipeline.try_cancel(retry, metadata(1'001U));
    assert(retry_duplicate.status ==
           CancelPipelineStatus::DuplicateAlreadyPending);
    assert(retry_duplicate.dispatch_attempt_id == 102U);
    assert(fixture.recorder.append_calls == 2U);
    assert(fixture.queue.size == 2U);
}

void test_forged_ownership_and_stale_attempts_fail_closed() {
    {
        Fixture fixture;
        CancelCommand forged = cancel_command();
        forged.parent_order_id = ParentOrderId{999U};
        const CancelPipelineResult result =
            fixture.pipeline.try_cancel(forged, metadata());
        assert(result.status == CancelPipelineStatus::OwnershipMismatch);
        assert(fixture.recorder.append_calls == 0U);
        assert(fixture.ownership.transition_calls == 0U);
        assert(fixture.queue.enqueue_calls == 0U);
        assert(fixture.attempts.next_candidate() == 101U);
    }
    {
        Fixture fixture(
            recorder::DurabilityDomain::VolatileSoftwareReference, 100U);
        const CancelPipelineResult result =
            fixture.pipeline.try_cancel(cancel_command(), metadata());
        assert(result.status == CancelPipelineStatus::AttemptUnavailable);
        assert(fixture.recorder.append_calls == 0U);
        assert(fixture.ownership.transition_calls == 0U);
        assert(fixture.queue.enqueue_calls == 0U);
    }
    {
        Fixture fixture;
        fixture.queue.force_cancel_full = true;
        const CancelPipelineResult result =
            fixture.pipeline.try_cancel(cancel_command(), metadata());
        assert(result.status == CancelPipelineStatus::CancelQueueFull);
        assert(fixture.attempts.next_candidate() == 101U);
        assert(fixture.recorder.append_calls == 0U);
    }
}

void test_no_queue_handoff_without_qualified_token_or_state_transition() {
    Fixture fixture(recorder::DurabilityDomain::HardwarePersistent);
    const CancelPipelineResult result =
        fixture.pipeline.try_cancel(cancel_command(), metadata());
    assert(result.status == CancelPipelineStatus::DurableTokenRejected);
    assert(result.recovery.required);
    assert(result.recovery.applied_phase ==
           CancelRecoveryPhase::DurableOnly);
    assert(fixture.pipeline.fail_stopped());
    assert(fixture.recorder.append_calls == 1U);
    assert(fixture.ownership.transition_calls == 0U);
    assert(fixture.ownership.child.state == ActiveChildState::Working);
    assert(fixture.queue.enqueue_calls == 0U);
    assert(fixture.queue.size == 0U);
    const CancelPipelineResult stopped =
        fixture.pipeline.try_cancel(cancel_command(), metadata(1'001U));
    assert(stopped.status == CancelPipelineStatus::FailStopped);
    assert(stopped.recovery == fixture.pipeline.recovery_requirement());
    assert(fixture.recorder.append_calls == 1U);
}

void test_dual_durable_failure_requires_recovery_before_fresh_attempt() {
    ha::ReplicationGate gate{1U, ha::ReplicationPolicy::DualDurable};
    assert(gate.install_authenticated_lease(
        ha::FenceLease{1U, kLeadershipEpoch, 1U, 100U}));
    FixedDualReplicator dual;
    Fixture fixture(
        recorder::DurabilityDomain::VolatileSoftwareReference,
        101U,
        &gate,
        &dual);

    const CancelPipelineResult blocked =
        fixture.pipeline.try_cancel(cancel_command(), metadata());
    assert(blocked.status == CancelPipelineStatus::DualDurabilityRejected);
    assert(blocked.dispatch_attempt_id == 101U);
    assert(blocked.recovery.required);
    assert(blocked.recovery.applied_phase ==
           CancelRecoveryPhase::DurableOnly);
    assert(fixture.pipeline.fail_stopped());
    assert(fixture.recorder.append_calls == 1U);
    assert(fixture.ownership.transition_calls == 0U);
    assert(fixture.queue.enqueue_calls == 0U);

    dual.succeed = true;
    const CancelPipelineResult still_stopped =
        fixture.pipeline.try_cancel(cancel_command(), metadata(1'001U));
    assert(still_stopped.status == CancelPipelineStatus::FailStopped);
    assert(fixture.recorder.append_calls == 1U);
    assert(dual.calls == 1U);

    // Recovery/reconciliation reconstructs both the fence and coordinator.
    ha::ReplicationGate recovered_gate{1U, ha::ReplicationPolicy::DualDurable};
    assert(recovered_gate.install_authenticated_lease(
        ha::FenceLease{1U, kLeadershipEpoch, 1U, 100U}));
    CancelPipeline recovered_pipeline(
        fixture.recorder,
        fixture.ownership,
        fixture.attempts,
        fixture.registry,
        fixture.policy,
        &recovered_gate,
        &dual);
    const CancelPipelineResult recovered = recovered_pipeline.try_cancel(
        cancel_command(), metadata(1'001U));
    assert(recovered.enqueued());
    assert(recovered.dispatch_attempt_id == 102U);
    assert(recovered.token.batch_id != blocked.token.batch_id);
    assert(fixture.recorder.append_calls == 2U);
    assert(dual.calls == 2U);
    assert(fixture.queue.size == 1U);
}

void test_transition_or_enqueue_race_never_creates_unauthorized_tx() {
    {
        Fixture fixture;
        fixture.ownership.forced_transition =
            OwnershipTransitionStatus::StaleSnapshot;
        const CancelPipelineResult result =
            fixture.pipeline.try_cancel(cancel_command(), metadata());
        assert(result.status ==
               CancelPipelineStatus::OwnershipTransitionRejected);
        assert(result.recovery.required);
        assert(result.recovery.applied_phase ==
               CancelRecoveryPhase::DurableOnly);
        assert(fixture.pipeline.fail_stopped());
        assert(fixture.recorder.append_calls == 1U);
        assert(fixture.queue.enqueue_calls == 0U);
    }
    {
        Fixture fixture;
        fixture.queue.force_enqueue_failure = true;
        const CancelPipelineResult result =
            fixture.pipeline.try_cancel(cancel_command(), metadata());
        assert(result.status == CancelPipelineStatus::QueueInvariantViolation);
        assert(result.recovery.required);
        assert(result.recovery.applied_phase ==
               CancelRecoveryPhase::OwnershipMarked);
        assert(fixture.pipeline.fail_stopped());
        assert(fixture.recorder.append_calls == 1U);
        assert(fixture.ownership.child.state == ActiveChildState::CancelPending);
        assert(fixture.queue.size == 0U);
    }
}

class TwoChildMassCancelSource final : public IBoundedMassCancelSource {
public:
    [[nodiscard]] MassCancelNextStatus next(
        MassCancelCursor& cursor,
        ActiveChildSnapshot& child) const noexcept override {
        if (cursor.visited >= cursor.maximum_visits) {
            return MassCancelNextStatus::VisitLimit;
        }
        if (cursor.emitted >= cursor.maximum_emits) {
            return MassCancelNextStatus::EmitLimit;
        }
        if (cursor.next_slot >= children.size()) {
            return MassCancelNextStatus::Done;
        }
        child = children[cursor.next_slot++];
        ++cursor.visited;
        ++cursor.emitted;
        return MassCancelNextStatus::Item;
    }

    std::array<ActiveChildSnapshot, 2U> children{
        working_child(), working_child()};
};

void test_mass_cancel_cursor_is_caller_bounded() {
    TwoChildMassCancelSource source;
    source.children[1U].child_id = ChildOrderId{301U};
    MassCancelCursor cursor{0U, 0U, 0U, 8U, 1U};
    ActiveChildSnapshot child;
    assert(source.next(cursor, child) == MassCancelNextStatus::Item);
    assert(child.child_id == kChild);
    assert(source.next(cursor, child) == MassCancelNextStatus::EmitLimit);
    assert(cursor.visited == 1U);
    assert(cursor.emitted == 1U);
}

void test_cancel_then_replace_never_releases_early() {
    const ActiveChildSnapshot child = working_child();
    CancelThenReplaceGate gate;
    assert(gate.begin(child, 900U));
    assert(gate.state() == CancelThenReplaceState::AwaitingCancelDispatch);
    assert(!gate.try_release_replacement());
    assert(!gate.observe_terminal_or_reconciled(
        kChild, kOwnershipGeneration, ActiveChildState::Terminal));
    assert(!gate.note_cancel_dispatched(kChild, kOwnershipGeneration + 1U, 101U));
    assert(gate.note_cancel_dispatched(kChild, kOwnershipGeneration, 101U));
    assert(!gate.try_release_replacement());
    assert(!gate.observe_terminal_or_reconciled(
        kChild, kOwnershipGeneration, ActiveChildState::Working));
    assert(gate.observe_terminal_or_reconciled(
        kChild, kOwnershipGeneration, ActiveChildState::Terminal));
    assert(gate.state() == CancelThenReplaceState::ReplacementEligible);
    assert(gate.try_release_replacement());
    assert(!gate.try_release_replacement());

    ActiveChildSnapshot pending = working_child();
    pending.state = ActiveChildState::CancelPending;
    pending.pending_client_request_id = 400U;
    pending.pending_client_sequence = 1U;
    pending.pending_client_session_epoch = 2U;
    pending.pending_cancel_attempt_id = 101U;
    pending.last_dispatch_attempt_id = 101U;
    assert(pending.valid());
    CancelThenReplaceGate reconciled;
    assert(reconciled.begin(pending, 901U));
    assert(reconciled.state() ==
           CancelThenReplaceState::AwaitingTerminalOrReconciliation);
    assert(!reconciled.try_release_replacement());
    assert(reconciled.observe_terminal_or_reconciled(
        kChild, kOwnershipGeneration, ActiveChildState::Reconciled));
    assert(reconciled.try_release_replacement());
}

}  // namespace

int main() {
    test_success_is_atomic_allocation_free_and_uses_reserved_lane();
    test_duplicate_is_idempotent_and_retry_gets_new_recorded_attempt();
    test_forged_ownership_and_stale_attempts_fail_closed();
    test_no_queue_handoff_without_qualified_token_or_state_transition();
    test_dual_durable_failure_requires_recovery_before_fresh_attempt();
    test_transition_or_enqueue_race_never_creates_unauthorized_tx();
    test_mass_cancel_cursor_is_caller_bounded();
    test_cancel_then_replace_never_releases_early();
    return 0;
}
