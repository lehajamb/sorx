#include "sorx/fastpath/cancel_pipeline.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace sorx::fastpath {
namespace {

class CanonicalCancelWriter final {
public:
    [[nodiscard]] bool put_u8(const std::uint8_t value) noexcept {
        return put_byte(static_cast<std::byte>(value));
    }

    [[nodiscard]] bool put_u16(const std::uint16_t value) noexcept {
        const std::array bytes{
            static_cast<std::byte>((value >> 8U) & 0xffU),
            static_cast<std::byte>(value & 0xffU),
        };
        return put_bytes(bytes);
    }

    [[nodiscard]] bool put_u32(const std::uint32_t value) noexcept {
        const std::array bytes{
            static_cast<std::byte>((value >> 24U) & 0xffU),
            static_cast<std::byte>((value >> 16U) & 0xffU),
            static_cast<std::byte>((value >> 8U) & 0xffU),
            static_cast<std::byte>(value & 0xffU),
        };
        return put_bytes(bytes);
    }

    [[nodiscard]] bool put_u64(const std::uint64_t value) noexcept {
        std::array<std::byte, 8U> bytes{};
        for (std::size_t index{}; index < bytes.size(); ++index) {
            const unsigned shift = static_cast<unsigned>((7U - index) * 8U);
            bytes[index] = static_cast<std::byte>((value >> shift) & 0xffU);
        }
        return put_bytes(bytes);
    }

    [[nodiscard]] bool put_bytes(const std::span<const std::byte> bytes) noexcept {
        if (!valid_ || bytes.size() > storage_.size() - size_) {
            valid_ = false;
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), storage_.begin() + size_);
        size_ += bytes.size();
        return true;
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {storage_.data(), size_};
    }

private:
    [[nodiscard]] bool put_byte(const std::byte value) noexcept {
        if (!valid_ || size_ == storage_.size()) {
            valid_ = false;
            return false;
        }
        storage_[size_++] = value;
        return true;
    }

    std::array<std::byte, 1'024U> storage_{};
    std::size_t size_{0U};
    bool valid_{true};
};

[[nodiscard]] bool valid_policy(const DispatchPipelinePolicy policy) noexcept {
    const bool valid_domain =
        policy.required_domain == recorder::DurabilityDomain::VolatileSoftwareReference ||
        policy.required_domain == recorder::DurabilityDomain::HardwarePersistent;
    return policy.leadership_epoch != 0U &&
           policy.configuration_generation != 0U && valid_domain;
}

[[nodiscard]] ha::DispatchFenceError preflight_fence(
    const ha::ReplicationGate& gate,
    const std::uint64_t leadership_epoch,
    const std::uint64_t now_ns) noexcept {
    const ha::FenceLease lease = gate.lease();
    if (!lease.well_formed()) {
        return ha::DispatchFenceError::NoLease;
    }
    if (lease.leadership_epoch != leadership_epoch) {
        return ha::DispatchFenceError::StaleLeadershipEpoch;
    }
    if (now_ns == 0U || now_ns >= lease.expires_at_monotonic_ns) {
        return ha::DispatchFenceError::LeaseExpired;
    }
    return ha::DispatchFenceError::None;
}

[[nodiscard]] bool ownership_matches(
    const CancelCommand& command,
    const ActiveChildSnapshot& child) noexcept {
    return child.valid() && child.account_context_id == command.account_context_id &&
           child.parent_order_id == command.parent_order_id &&
           child.child_id == command.child_id && child.venue_id == command.venue_id &&
           child.ownership_generation == command.expected_ownership_generation &&
           child.venue_session_id == command.expected_venue_session_id &&
           child.venue_session_epoch == command.expected_venue_session_epoch;
}

[[nodiscard]] bool build_cancel_batch(
    const CancelCommand& command,
    const ActiveChildSnapshot& child,
    const EncodedFrame& frame,
    const CancelRequestMetadata metadata,
    const DispatchPipelinePolicy policy,
    recorder::DurableBatch& batch) noexcept {
    CanonicalCancelWriter writer;
    if (!writer.put_u32(0x534f5243U) ||  // "SORC"
        !writer.put_u16(kCancelDispatchRecordVersion) ||
        !writer.put_u64(metadata.producer_sequence) ||
        !writer.put_u64(policy.leadership_epoch) ||
        !writer.put_u64(policy.configuration_generation) ||
        !writer.put_u8(static_cast<std::uint8_t>(policy.required_domain)) ||
        !writer.put_u64(metadata.fence_time_ns) ||
        !writer.put_u64(command.client_request_id) ||
        !writer.put_u64(command.client_sequence) ||
        !writer.put_u64(command.client_session_epoch) ||
        !writer.put_u64(command.account_context_id) ||
        !writer.put_u64(command.parent_order_id.value()) ||
        !writer.put_u64(command.child_id.value()) ||
        !writer.put_u32(command.venue_id.value()) ||
        !writer.put_u64(command.expected_ownership_generation) ||
        !writer.put_u64(command.expected_venue_session_id) ||
        !writer.put_u64(command.expected_venue_session_epoch) ||
        !writer.put_u16(command.reason_code) ||
        !writer.put_u8(static_cast<std::uint8_t>(command.kind)) ||
        !writer.put_u64(child.account_context_id) ||
        !writer.put_u64(child.parent_order_id.value()) ||
        !writer.put_u64(child.child_id.value()) ||
        !writer.put_u32(child.venue_id.value()) ||
        !writer.put_u64(child.venue_session_id) ||
        !writer.put_u64(child.venue_session_epoch) ||
        !writer.put_u64(child.ownership_generation) ||
        !writer.put_u64(child.last_dispatch_attempt_id) ||
        !writer.put_u8(static_cast<std::uint8_t>(child.state)) ||
        !writer.put_u64(child.pending_client_request_id) ||
        !writer.put_u64(child.pending_client_sequence) ||
        !writer.put_u64(child.pending_client_session_epoch) ||
        !writer.put_u64(child.pending_cancel_attempt_id) ||
        !writer.put_u8(child.wire_identity.size) ||
        !writer.put_bytes(child.wire_identity.value()) ||
        !writer.put_u32(frame.venue_id) ||
        !writer.put_u64(frame.child_id) ||
        !writer.put_u64(frame.dispatch_attempt_id) ||
        !writer.put_u64(frame.leadership_epoch) ||
        !writer.put_u64(frame.configuration_generation) ||
        !writer.put_u8(static_cast<std::uint8_t>(frame.dispatch_class)) ||
        !writer.put_u16(frame.size) ||
        !writer.put_bytes(frame.bytes()) || !writer.valid()) {
        return false;
    }
    return batch.try_append_record(
               kCancelDispatchRecordType,
               kCancelDispatchRecordVersion,
               writer.bytes()) &&
           batch.valid();
}

}  // namespace

RouteKernelActiveChildOwnership::RouteKernelActiveChildOwnership(
    dataplane::RouteKernel& kernel,
    const std::uint64_t account_context_id) noexcept
    : kernel_(&kernel), account_context_id_(account_context_id) {}

OwnershipLookupResult RouteKernelActiveChildOwnership::lookup(
    const ChildOrderId child_id) const noexcept {
    OwnershipLookupResult result;
    if (kernel_ == nullptr || account_context_id_ == 0U) {
        result.status = OwnershipLookupStatus::Unavailable;
        return result;
    }
    const dataplane::ActiveChildLookupResult found =
        kernel_->lookup_active_child(child_id);
    if (found.status == dataplane::ActiveChildLookupStatus::NotFound) {
        return result;
    }
    if (found.status != dataplane::ActiveChildLookupStatus::Found) {
        result.status = OwnershipLookupStatus::Unavailable;
        return result;
    }
    const dataplane::ActiveChildOwnershipSnapshot& source = found.snapshot;
    ActiveChildSnapshot& target = result.snapshot;
    target.account_context_id = account_context_id_;
    target.parent_order_id = source.parent_order_id;
    target.child_id = source.child_id;
    target.venue_id = source.venue_id;
    target.venue_session_id = source.venue_session_id;
    target.venue_session_epoch = source.venue_session_epoch;
    target.ownership_generation = source.ownership_generation;
    target.last_dispatch_attempt_id = source.last_dispatch_attempt_id;
    target.wire_identity = source.wire_identity;
    target.state = source.state ==
            dataplane::ActiveChildOwnershipState::CancelPending
        ? ActiveChildState::CancelPending
        : ActiveChildState::Working;
    target.pending_client_request_id = source.pending_client_request_id;
    target.pending_client_sequence = source.pending_client_sequence;
    target.pending_client_session_epoch =
        source.pending_client_session_epoch;
    target.pending_cancel_attempt_id = source.pending_cancel_attempt_id;
    if (!target.valid()) {
        result.status = OwnershipLookupStatus::Unavailable;
        return result;
    }
    result.status = OwnershipLookupStatus::Found;
    return result;
}

OwnershipTransitionStatus RouteKernelActiveChildOwnership::mark_cancel_pending(
    const CancelPendingTransition& transition) noexcept {
    if (kernel_ == nullptr || account_context_id_ == 0U ||
        transition.expected.account_context_id != account_context_id_) {
        return OwnershipTransitionStatus::Rejected;
    }
    dataplane::ActiveChildOwnershipSnapshot expected;
    expected.parent_order_id = transition.expected.parent_order_id;
    expected.child_id = transition.expected.child_id;
    expected.venue_id = transition.expected.venue_id;
    expected.venue_session_id = transition.expected.venue_session_id;
    expected.venue_session_epoch = transition.expected.venue_session_epoch;
    expected.ownership_generation = transition.expected.ownership_generation;
    expected.last_dispatch_attempt_id =
        transition.expected.last_dispatch_attempt_id;
    expected.wire_identity = transition.expected.wire_identity;
    expected.state = transition.expected.state == ActiveChildState::CancelPending
        ? dataplane::ActiveChildOwnershipState::CancelPending
        : dataplane::ActiveChildOwnershipState::Working;
    expected.pending_client_request_id =
        transition.expected.pending_client_request_id;
    expected.pending_client_sequence =
        transition.expected.pending_client_sequence;
    expected.pending_client_session_epoch =
        transition.expected.pending_client_session_epoch;
    expected.pending_cancel_attempt_id =
        transition.expected.pending_cancel_attempt_id;
    const dataplane::ActiveChildTransitionStatus changed =
        kernel_->mark_cancel_pending({
            expected,
            transition.client_request_id,
            transition.client_sequence,
            transition.client_session_epoch,
            transition.dispatch_attempt_id,
        });
    switch (changed) {
        case dataplane::ActiveChildTransitionStatus::Marked:
            return OwnershipTransitionStatus::Marked;
        case dataplane::ActiveChildTransitionStatus::StaleSnapshot:
            return OwnershipTransitionStatus::StaleSnapshot;
        case dataplane::ActiveChildTransitionStatus::AlreadyPendingSame:
            return OwnershipTransitionStatus::AlreadyPendingSame;
        case dataplane::ActiveChildTransitionStatus::AlreadyPendingOther:
            return OwnershipTransitionStatus::AlreadyPendingOther;
        case dataplane::ActiveChildTransitionStatus::Terminal:
            return OwnershipTransitionStatus::Terminal;
        case dataplane::ActiveChildTransitionStatus::Rejected:
        case dataplane::ActiveChildTransitionStatus::GenerationExhausted:
            return OwnershipTransitionStatus::Rejected;
    }
    return OwnershipTransitionStatus::Rejected;
}

AttemptAssignment BoundedDispatchAttemptSource::assign_after(
    const std::uint64_t exclusive_floor) noexcept {
    if (exclusive_floor == std::numeric_limits<std::uint64_t>::max()) {
        return {AttemptAssignmentStatus::InvalidFloor, 0U};
    }
    if (exhausted_ || next_attempt_id_ == 0U ||
        next_attempt_id_ > maximum_attempt_id_) {
        exhausted_ = true;
        return {AttemptAssignmentStatus::Exhausted, 0U};
    }
    if (next_attempt_id_ <= exclusive_floor) {
        return {AttemptAssignmentStatus::InvalidFloor, 0U};
    }

    const std::uint64_t assigned = next_attempt_id_;
    if (assigned == maximum_attempt_id_) {
        exhausted_ = true;
    } else {
        ++next_attempt_id_;
    }
    return {AttemptAssignmentStatus::Assigned, assigned};
}

bool FixedCancelVenueRegistry::bind(const CancelVenueBinding& binding) noexcept {
    if (sealed_ || !static_cast<bool>(binding.venue_id) ||
        binding.cancel_queue == nullptr || binding.encoder == nullptr ||
        size_ == bindings_.size() || find(binding.venue_id) != nullptr) {
        return false;
    }
    bindings_[size_++] = binding;
    return true;
}

const CancelVenueBinding* FixedCancelVenueRegistry::find(
    const VenueId venue_id) const noexcept {
    for (std::size_t index{}; index < size_; ++index) {
        if (bindings_[index].venue_id == venue_id) {
            return &bindings_[index];
        }
    }
    return nullptr;
}

CancelPipeline::CancelPipeline(
    recorder::IFlightRecorder& flight_recorder,
    IActiveChildOwnership& ownership,
    BoundedDispatchAttemptSource& attempts,
    const FixedCancelVenueRegistry& venues,
    const DispatchPipelinePolicy policy,
    ha::ReplicationGate* const replication_gate,
    IDualDurableReplicator* const dual_replicator) noexcept
    : recorder_(&flight_recorder),
      ownership_(&ownership),
      attempts_(&attempts),
      venues_(&venues),
      policy_(policy),
      replication_gate_(replication_gate),
      dual_replicator_(dual_replicator) {}

bool CancelPipeline::fail_stopped() const noexcept {
    return recovery_.required;
}

CancelRecoveryRequirement CancelPipeline::recovery_requirement()
    const noexcept {
    return recovery_;
}

void CancelPipeline::require_recovery(
    const CancelCommand& command,
    const std::uint64_t producer_sequence,
    const recorder::DurableToken* const token,
    const std::uint64_t dispatch_attempt_id,
    const CancelRecoveryPhase applied_phase) noexcept {
    recovery_.required = true;
    recovery_.producer_sequence = producer_sequence;
    recovery_.child_id = command.child_id.value();
    recovery_.dispatch_attempt_id = dispatch_attempt_id;
    recovery_.applied_phase = applied_phase;
    if (token != nullptr) {
        recovery_.durable_batch_id = token->batch_id;
        recovery_.batch_hash = token->batch_hash;
    }
}

CancelPipelineResult CancelPipeline::try_cancel(
    const CancelCommand& command,
    const CancelRequestMetadata& metadata) noexcept {
    CancelPipelineResult result;
    if (recovery_.required) {
        result.status = CancelPipelineStatus::FailStopped;
        result.recovery = recovery_;
        return result;
    }
    if (!command.valid() || !metadata.valid() || recorder_ == nullptr ||
        ownership_ == nullptr || attempts_ == nullptr || venues_ == nullptr) {
        result.status = CancelPipelineStatus::InvalidCommand;
        return result;
    }
    if (!valid_policy(policy_)) {
        result.status = CancelPipelineStatus::PolicyInvalid;
        return result;
    }
    if (!venues_->sealed()) {
        result.status = CancelPipelineStatus::VenueUnavailable;
        return result;
    }
    if (replication_gate_ != nullptr) {
        const ha::DispatchFenceError fence = preflight_fence(
            *replication_gate_, policy_.leadership_epoch, metadata.fence_time_ns);
        if (fence != ha::DispatchFenceError::None) {
            result.status = CancelPipelineStatus::LeadershipFenceRejected;
            return result;
        }
    }

    const CancelVenueBinding* const venue = venues_->find(command.venue_id);
    if (venue == nullptr || venue->cancel_queue == nullptr || venue->encoder == nullptr) {
        result.status = CancelPipelineStatus::VenueUnavailable;
        return result;
    }

    const OwnershipLookupResult lookup = ownership_->lookup(command.child_id);
    if (lookup.status == OwnershipLookupStatus::NotFound) {
        result.status = CancelPipelineStatus::OwnershipNotFound;
        return result;
    }
    if (lookup.status != OwnershipLookupStatus::Found) {
        result.status = CancelPipelineStatus::OwnershipUnavailable;
        return result;
    }
    const ActiveChildSnapshot& child = lookup.snapshot;
    if (!ownership_matches(command, child)) {
        result.status = CancelPipelineStatus::OwnershipMismatch;
        return result;
    }
    if (child.state == ActiveChildState::Terminal ||
        child.state == ActiveChildState::Reconciled) {
        result.status = CancelPipelineStatus::AlreadyTerminal;
        return result;
    }
    if (child.same_cancel_identity(command)) {
        result.status = CancelPipelineStatus::DuplicateAlreadyPending;
        result.dispatch_attempt_id = child.pending_cancel_attempt_id;
        return result;
    }
    if (command.kind == CancelRequestKind::Cancel &&
        child.state == ActiveChildState::CancelPending) {
        result.status = CancelPipelineStatus::CancelAlreadyPending;
        return result;
    }
    if (command.kind == CancelRequestKind::Retry &&
        child.state != ActiveChildState::CancelPending) {
        result.status = CancelPipelineStatus::RetryNotPending;
        return result;
    }
    if (command.kind == CancelRequestKind::Retry &&
        child.pending_client_session_epoch != command.client_session_epoch) {
        result.status = CancelPipelineStatus::OwnershipMismatch;
        return result;
    }
    if (venue->cancel_queue->cancel_available_approx() == 0U) {
        result.status = CancelPipelineStatus::CancelQueueFull;
        return result;
    }

    const AttemptAssignment attempt = attempts_->assign_after(
        child.last_dispatch_attempt_id);
    if (attempt.status != AttemptAssignmentStatus::Assigned) {
        result.status = CancelPipelineStatus::AttemptUnavailable;
        return result;
    }
    result.dispatch_attempt_id = attempt.attempt_id;

    EncodedFrame frame;
    const FrameEncodeResult encoded = venue->encoder->encode_cancel(
        command, child, attempt.attempt_id, frame.storage);
    if (!encoded.encoded() || encoded.size > frame.storage.size()) {
        result.status = CancelPipelineStatus::EncodeRejected;
        return result;
    }
    frame.venue_id = command.venue_id.value();
    frame.child_id = command.child_id.value();
    frame.dispatch_attempt_id = attempt.attempt_id;
    frame.leadership_epoch = policy_.leadership_epoch;
    frame.configuration_generation = policy_.configuration_generation;
    frame.venue_session_id = child.venue_session_id;
    frame.venue_session_epoch = child.venue_session_epoch;
    frame.ownership_generation = child.ownership_generation;
    frame.dispatch_class = DispatchClass::CancelOrReconcile;
    frame.size = encoded.size;
    if (!frame.valid()) {
        result.status = CancelPipelineStatus::EncodeRejected;
        return result;
    }

    recorder::DurableBatch batch{
        recorder::DurabilityLane::Cancel, metadata.producer_sequence};
    batch.set_leadership_epoch(policy_.leadership_epoch);
    batch.set_configuration_generation(policy_.configuration_generation);
    const recorder::DispatchAuthorization expected_authorization{
        command.child_id.value(), attempt.attempt_id, 1U};
    if (!batch.set_dispatch_authorization(expected_authorization) ||
        !batch.try_add_external_effect_authorization(
            frame.venue_id,
            frame.child_id,
            frame.dispatch_attempt_id,
            frame.venue_session_id,
            frame.venue_session_epoch,
            frame.ownership_generation,
            static_cast<std::uint8_t>(frame.dispatch_class),
            frame.bytes()) ||
        !build_cancel_batch(command, child, frame, metadata, policy_, batch)) {
        result.status = CancelPipelineStatus::RecorderRejected;
        return result;
    }

    if (!completion_.reset()) {
        result.status = CancelPipelineStatus::CompletionMissing;
        return result;
    }
    const recorder::AppendStatus appended = recorder_->try_append(batch, completion_);
    if (appended != recorder::AppendStatus::Committed) {
        result.status = appended == recorder::AppendStatus::CompletionSlotBusy
            ? CancelPipelineStatus::RecorderBusy
            : CancelPipelineStatus::RecorderRejected;
        return result;
    }
    const auto token = completion_.token();
    if (!token) {
        result.status = CancelPipelineStatus::CompletionMissing;
        require_recovery(
            command,
            metadata.producer_sequence,
            nullptr,
            attempt.attempt_id,
            CancelRecoveryPhase::DurableOnly);
        result.recovery = recovery_;
        return result;
    }
    result.token = *token;
    if (!static_cast<bool>(result.token) ||
        result.token.producer_sequence != metadata.producer_sequence ||
        result.token.leadership_epoch != policy_.leadership_epoch ||
        result.token.configuration_generation != policy_.configuration_generation ||
        result.token.lane != recorder::DurabilityLane::Cancel ||
        result.token.dispatch_authorization != expected_authorization ||
        !result.token.authorizes_dispatch(
            command.child_id.value(),
            attempt.attempt_id,
            policy_.leadership_epoch,
            policy_.configuration_generation) ||
        !result.token.authorizes_external_effect(
            frame.venue_id,
            frame.child_id,
            frame.dispatch_attempt_id,
            frame.venue_session_id,
            frame.venue_session_epoch,
            frame.ownership_generation,
            static_cast<std::uint8_t>(frame.dispatch_class),
            frame.bytes()) ||
        !recorder_->authorizes_external_effect(
            result.token, policy_.required_domain)) {
        result.status = CancelPipelineStatus::DurableTokenRejected;
        require_recovery(
            command,
            metadata.producer_sequence,
            &result.token,
            attempt.attempt_id,
            CancelRecoveryPhase::DurableOnly);
        result.recovery = recovery_;
        return result;
    }

    if (replication_gate_ != nullptr) {
        if (!replication_gate_->note_local_durable(
                result.token.batch_id, result.token.leadership_epoch)) {
            result.status = CancelPipelineStatus::LeadershipFenceRejected;
            require_recovery(
                command,
                metadata.producer_sequence,
                &result.token,
                attempt.attempt_id,
                CancelRecoveryPhase::DurableOnly);
            result.recovery = recovery_;
            return result;
        }
        if (replication_gate_->policy() == ha::ReplicationPolicy::DualDurable) {
            if (dual_replicator_ == nullptr ||
                !dual_replicator_->replicate_and_wait_durable(batch, result.token) ||
                !replication_gate_->note_remote_durable(
                    result.token.batch_id, result.token.leadership_epoch)) {
                result.status = CancelPipelineStatus::DualDurabilityRejected;
                require_recovery(
                    command,
                    metadata.producer_sequence,
                    &result.token,
                    attempt.attempt_id,
                    CancelRecoveryPhase::DurableOnly);
                result.recovery = recovery_;
                return result;
            }
        }
        if (replication_gate_->authorize(
                result.token.batch_id,
                result.token.leadership_epoch,
                metadata.fence_time_ns) != ha::DispatchFenceError::None) {
            result.status = CancelPipelineStatus::LeadershipFenceRejected;
            require_recovery(
                command,
                metadata.producer_sequence,
                &result.token,
                attempt.attempt_id,
                CancelRecoveryPhase::DurableOnly);
            result.recovery = recovery_;
            return result;
        }
    }

    const CancelPendingTransition transition{
        child,
        command.client_request_id,
        command.client_sequence,
        command.client_session_epoch,
        attempt.attempt_id,
        result.token.batch_id,
        policy_.leadership_epoch,
        policy_.configuration_generation,
    };
    const OwnershipTransitionStatus changed =
        ownership_->mark_cancel_pending(transition);
    if (changed == OwnershipTransitionStatus::AlreadyPendingSame) {
        result.status = CancelPipelineStatus::DuplicateAlreadyPending;
        require_recovery(
            command,
            metadata.producer_sequence,
            &result.token,
            attempt.attempt_id,
            CancelRecoveryPhase::DurableOnly);
        result.recovery = recovery_;
        return result;
    }
    if (changed != OwnershipTransitionStatus::Marked) {
        result.status = CancelPipelineStatus::OwnershipTransitionRejected;
        require_recovery(
            command,
            metadata.producer_sequence,
            &result.token,
            attempt.attempt_id,
            CancelRecoveryPhase::DurableOnly);
        result.recovery = recovery_;
        return result;
    }
    const auto permit = result.token.effect_permit(0U);
    if (!permit ||
        !venue->cancel_queue->try_enqueue_cancel(frame, *permit)) {
        result.status = CancelPipelineStatus::QueueInvariantViolation;
        require_recovery(
            command,
            metadata.producer_sequence,
            &result.token,
            attempt.attempt_id,
            CancelRecoveryPhase::OwnershipMarked);
        result.recovery = recovery_;
        return result;
    }

    result.status = CancelPipelineStatus::Enqueued;
    return result;
}

bool CancelThenReplaceGate::begin(
    const ActiveChildSnapshot& child,
    const std::uint64_t replacement_request_id) noexcept {
    if (state_ != CancelThenReplaceState::Idle || !child.valid() ||
        replacement_request_id == 0U ||
        (child.state != ActiveChildState::Working &&
         child.state != ActiveChildState::CancelPending)) {
        return false;
    }
    child_id_ = child.child_id;
    ownership_generation_ = child.ownership_generation;
    replacement_request_id_ = replacement_request_id;
    if (child.state == ActiveChildState::CancelPending) {
        cancel_attempt_id_ = child.pending_cancel_attempt_id;
        state_ = CancelThenReplaceState::AwaitingTerminalOrReconciliation;
    } else {
        state_ = CancelThenReplaceState::AwaitingCancelDispatch;
    }
    return true;
}

bool CancelThenReplaceGate::note_cancel_dispatched(
    const ChildOrderId child_id,
    const std::uint64_t ownership_generation,
    const std::uint64_t cancel_attempt_id) noexcept {
    if (state_ != CancelThenReplaceState::AwaitingCancelDispatch ||
        child_id != child_id_ || ownership_generation != ownership_generation_ ||
        cancel_attempt_id == 0U) {
        return false;
    }
    cancel_attempt_id_ = cancel_attempt_id;
    state_ = CancelThenReplaceState::AwaitingTerminalOrReconciliation;
    return true;
}

bool CancelThenReplaceGate::observe_terminal_or_reconciled(
    const ChildOrderId child_id,
    const std::uint64_t ownership_generation,
    const ActiveChildState observed_state) noexcept {
    if (state_ != CancelThenReplaceState::AwaitingTerminalOrReconciliation ||
        child_id != child_id_ || ownership_generation != ownership_generation_ ||
        (observed_state != ActiveChildState::Terminal &&
         observed_state != ActiveChildState::Reconciled)) {
        return false;
    }
    state_ = CancelThenReplaceState::ReplacementEligible;
    return true;
}

bool CancelThenReplaceGate::try_release_replacement() noexcept {
    if (state_ != CancelThenReplaceState::ReplacementEligible) {
        return false;
    }
    state_ = CancelThenReplaceState::ReplacementReleased;
    return true;
}

void CancelThenReplaceGate::abort() noexcept {
    if (state_ != CancelThenReplaceState::ReplacementReleased) {
        state_ = CancelThenReplaceState::Aborted;
    }
}

}  // namespace sorx::fastpath
