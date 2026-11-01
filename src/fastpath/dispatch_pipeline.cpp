#include "sorx/fastpath/dispatch_pipeline.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace sorx::fastpath {
namespace {

class CanonicalWriter final {
public:
    void clear() noexcept {
        size_ = 0U;
        valid_ = true;
    }

    [[nodiscard]] bool put_u8(const std::uint8_t value) noexcept {
        return put_byte(static_cast<std::byte>(value));
    }

    [[nodiscard]] bool put_bool(const bool value) noexcept {
        return put_u8(value ? 1U : 0U);
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
            const auto shift = static_cast<unsigned>((7U - index) * 8U);
            bytes[index] = static_cast<std::byte>((value >> shift) & 0xffU);
        }
        return put_bytes(bytes);
    }

    [[nodiscard]] bool put_i64(const std::int64_t value) noexcept {
        return put_u64(static_cast<std::uint64_t>(value));
    }

    [[nodiscard]] bool put_bytes(
        const std::span<const std::byte> bytes) noexcept {
        if (!valid_ || bytes.size() > storage_.size() - size_) {
            valid_ = false;
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), storage_.begin() + size_);
        size_ += bytes.size();
        return true;
    }

    [[nodiscard]] std::size_t reserve_u16() noexcept {
        const std::size_t offset = size_;
        if (!put_u16(0U)) {
            return storage_.size();
        }
        return offset;
    }

    [[nodiscard]] bool patch_u16(
        const std::size_t offset,
        const std::uint16_t value) noexcept {
        if (!valid_ || offset > size_ || size_ - offset < 2U) {
            valid_ = false;
            return false;
        }
        storage_[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
        storage_[offset + 1U] = static_cast<std::byte>(value & 0xffU);
        return true;
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

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

    std::array<std::byte, recorder::kMaximumDurableBatchBytes> storage_{};
    std::size_t size_{};
    bool valid_{true};
};

[[nodiscard]] bool write_book_version(
    CanonicalWriter& writer,
    const md::BookVersion& version) noexcept {
    return writer.put_u32(version.venue.value()) &&
           writer.put_u32(version.instrument.value()) &&
           writer.put_u64(version.feed_session_epoch) &&
           writer.put_u64(version.recovery_generation) &&
           writer.put_u64(version.view_generation) &&
           writer.put_u64(version.last_applied_sequence) &&
           writer.put_u64(version.next_sequence);
}

[[nodiscard]] bool write_policy_versions(
    CanonicalWriter& writer,
    const policy::PolicyVersionPin& pin) noexcept {
    return writer.put_u64(pin.policy_generation) &&
           writer.put_u64(pin.fee_generation) &&
           writer.put_u64(pin.metric_generation) &&
           writer.put_u64(pin.weight_generation) &&
           writer.put_u64(pin.compliance_generation) &&
           writer.put_u64(pin.compliance_rule_pack_id) &&
           writer.put_u64(pin.compliance_rule_pack_version) &&
           writer.put_u64(pin.compliance_effective_from_utc_ns) &&
           writer.put_u64(pin.compliance_effective_until_utc_ns);
}

[[nodiscard]] bool write_policy_order(
    CanonicalWriter& writer,
    const policy::PolicyOrder& order) noexcept {
    return writer.put_u64(order.parent_order_id.value()) &&
           writer.put_u32(order.instrument.value()) &&
           writer.put_u8(static_cast<std::uint8_t>(order.side)) &&
           writer.put_u8(static_cast<std::uint8_t>(order.intent)) &&
           writer.put_u64(order.quantity.units) &&
           writer.put_i64(order.limit_price.units) &&
           writer.put_u64(order.decision_monotonic_ns) &&
           writer.put_u64(order.decision_utc_ns);
}

[[nodiscard]] bool write_fact_versions(
    CanonicalWriter& writer,
    const policy::VenueFactPin& pin) noexcept {
    return writer.put_u32(pin.venue.value()) &&
           writer.put_u64(pin.versions.route_generation) &&
           writer.put_u64(pin.versions.market_generation) &&
           writer.put_u64(pin.versions.order_entry_generation) &&
           writer.put_u64(pin.versions.risk_generation) &&
           writer.put_u64(pin.versions.throttle_generation) &&
           writer.put_u64(pin.versions.tx_credit_generation) &&
           writer.put_u64(pin.versions.protected_quote_generation) &&
           writer.put_u64(pin.versions.fee_generation) &&
           writer.put_u64(pin.versions.metric_generation);
}

[[nodiscard]] bool write_score(
    CanonicalWriter& writer,
    const policy::ScoreBreakdown& score) noexcept {
    return writer.put_i64(score.depth_reward) &&
           writer.put_i64(score.fee_economics) &&
           writer.put_i64(score.fill_quality_reward) &&
           writer.put_i64(score.adverse_selection_penalty) &&
           writer.put_i64(score.latency_penalty) &&
           writer.put_i64(score.reject_penalty) &&
           writer.put_i64(score.total) && writer.put_bool(score.saturated);
}

[[nodiscard]] bool write_adaptive_policy_overlay(
    CanonicalWriter& writer,
    const dataplane::AdaptivePolicyOverlay& overlay) noexcept {
    if (!writer.put_bool(overlay.applied)) {
        return false;
    }
    if (!overlay.applied) {
        return true;
    }
    if (!writer.put_bool(overlay.eligible) || !writer.put_u16(overlay.rank) ||
        !writer.put_u8(static_cast<std::uint8_t>(overlay.exclusion_reason)) ||
        !write_policy_versions(writer, overlay.versions) ||
        !write_policy_order(writer, overlay.order) ||
        !write_fact_versions(writer, overlay.fact_pin)) {
        return false;
    }
    if (!overlay.eligible) {
        return true;
    }
    const policy::RankedCandidate& evidence = overlay.evidence;
    return writer.put_u32(evidence.venue.value()) &&
           writer.put_i64(evidence.executable_price.units) &&
           writer.put_u64(evidence.available_depth.units) &&
           writer.put_u64(evidence.quote_age_ns) &&
           writer.put_u64(evidence.maximum_quote_age_ns) &&
           writer.put_u32(evidence.throttle_credits) &&
           writer.put_u32(evidence.normal_tx_credits) &&
           writer.put_bool(evidence.protected_quote.view_healthy) &&
           writer.put_bool(evidence.protected_quote.best_opposite_present) &&
           writer.put_i64(evidence.protected_quote.best_opposite_price.units) &&
           writer.put_u32(static_cast<std::uint32_t>(evidence.fee_ppm_used)) &&
           writer.put_u32(evidence.metrics.fill_quality_ppm) &&
           writer.put_u32(evidence.metrics.adverse_selection_ppm) &&
           writer.put_u32(evidence.metrics.reject_rate_ppm) &&
           writer.put_u64(evidence.metrics.measured_latency_ns) &&
           write_score(writer, evidence.score) &&
           writer.put_u32(evidence.configured_priority);
}

[[nodiscard]] const dataplane::BookSlotView* find_view(
    const std::span<const dataplane::BookSlotView> views,
    const VenueId venue) noexcept {
    for (const dataplane::BookSlotView& view : views) {
        if (view.venue == venue) {
            return &view;
        }
    }
    return nullptr;
}

[[nodiscard]] bool write_view_metadata(
    CanonicalWriter& writer,
    const dataplane::BookSlotView* const view) noexcept {
    if (!writer.put_bool(view != nullptr)) {
        return false;
    }
    if (view == nullptr) {
        return true;
    }
    if (!writer.put_u32(view->venue.value()) ||
        !writer.put_u32(view->instrument.value()) ||
        !writer.put_u64(view->adjacency_generation) ||
        !writer.put_u8(static_cast<std::uint8_t>(view->market_data_state)) ||
        !writer.put_u8(static_cast<std::uint8_t>(view->order_entry_state)) ||
        !writer.put_u8(static_cast<std::uint8_t>(view->egress_state)) ||
        !writer.put_u8(static_cast<std::uint8_t>(view->instrument_state)) ||
        !writer.put_u64(view->feed_epoch) ||
        !writer.put_u64(view->source_health_generation) ||
        !writer.put_u64(view->monotonic_receive_time_ns) ||
        !writer.put_u64(view->monotonic_publication_time_ns) ||
        !writer.put_u64(view->maximum_quote_age_ns) ||
        !writer.put_u64(view->tx_credit_generation) ||
        !writer.put_u64(view->throttle_generation) ||
        !writer.put_u32(view->normal_tx_credits) ||
        !writer.put_u32(view->cancel_reserved_tx_credits) ||
        !writer.put_u32(view->throttle_credits) ||
        !writer.put_bool(view->has_book) ||
        !writer.put_bool(view->source_healthy) ||
        !writer.put_bool(view->conversion_compatible) ||
        !writer.put_bool(view->risk_allowed)) {
        return false;
    }
    if (!view->has_book) {
        return true;
    }
    return writer.put_u32(view->book.channel.value()) &&
           writer.put_u32(view->book.profile.value()) &&
           writer.put_i64(view->book.exchange_timestamp_ns) &&
           writer.put_bool(view->book.depth_truncated);
}

[[nodiscard]] bool write_depth_slice(
    CanonicalWriter& writer,
    const VenueId venue,
    const Side book_side,
    const std::uint16_t level_index,
    const md::Level& level) noexcept {
    return writer.put_u32(venue.value()) &&
           writer.put_u8(static_cast<std::uint8_t>(book_side)) &&
           writer.put_u16(level_index) &&
           writer.put_i64(level.price.units) &&
           writer.put_u64(level.quantity.units) &&
           writer.put_u32(level.order_count);
}

[[nodiscard]] bool append_record(
    recorder::DurableBatch& batch,
    const DispatchRecordType type,
    const CanonicalWriter& writer) noexcept {
    return writer.valid() && batch.try_append_record(
        static_cast<std::uint16_t>(type),
        kDispatchRecordVersion,
        writer.bytes());
}

[[nodiscard]] bool valid_policy(
    const DispatchPipelinePolicy policy) noexcept {
    const bool valid_domain =
        policy.required_domain ==
            recorder::DurabilityDomain::VolatileSoftwareReference ||
        policy.required_domain == recorder::DurabilityDomain::HardwarePersistent;
    return policy.leadership_epoch != 0U &&
           policy.configuration_generation != 0U && valid_domain;
}

[[nodiscard]] std::optional<ha::DispatchFenceError> preflight_fence(
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
    return std::nullopt;
}

}  // namespace

bool VenueTxQueues::try_enqueue_normal(
    const EncodedFrame& frame,
    const recorder::DurableEffectPermit& permit) noexcept {
    return frame.valid() && static_cast<bool>(permit) &&
           frame.dispatch_class == DispatchClass::NewOrReplace &&
           permit.leadership_epoch == frame.leadership_epoch &&
           permit.configuration_generation ==
               frame.configuration_generation &&
           permit.authorizes_external_effect(
               frame.venue_id,
               frame.child_id,
               frame.dispatch_attempt_id,
               frame.venue_session_id,
               frame.venue_session_epoch,
               frame.ownership_generation,
               static_cast<std::uint8_t>(frame.dispatch_class),
               frame.bytes()) &&
           normal_.try_push(AuthorizedFrame{frame, permit});
}

bool VenueTxQueues::try_enqueue_cancel(
    const EncodedFrame& frame,
    const recorder::DurableEffectPermit& permit) noexcept {
    return frame.valid() && static_cast<bool>(permit) &&
           frame.dispatch_class == DispatchClass::CancelOrReconcile &&
           permit.leadership_epoch == frame.leadership_epoch &&
           permit.configuration_generation ==
               frame.configuration_generation &&
           permit.authorizes_external_effect(
               frame.venue_id,
               frame.child_id,
               frame.dispatch_attempt_id,
               frame.venue_session_id,
               frame.venue_session_epoch,
               frame.ownership_generation,
               static_cast<std::uint8_t>(frame.dispatch_class),
               frame.bytes()) &&
           cancel_.try_push(AuthorizedFrame{frame, permit});
}

bool VenueTxQueues::try_pop_normal(AuthorizedFrame& output) noexcept {
    return normal_.try_pop(output);
}

bool VenueTxQueues::try_pop_cancel(AuthorizedFrame& output) noexcept {
    return cancel_.try_pop(output);
}

std::size_t VenueTxQueues::normal_size_approx() const noexcept {
    return normal_.size_approx();
}

std::size_t VenueTxQueues::cancel_size_approx() const noexcept {
    return cancel_.size_approx();
}

std::size_t VenueTxQueues::normal_available_approx() const noexcept {
    return kNormalVenueQueueCapacity - normal_.size_approx();
}

std::size_t VenueTxQueues::cancel_available_approx() const noexcept {
    return kCancelVenueQueueCapacity - cancel_.size_approx();
}

VenueRegistrationStatus FixedVenueRegistry::register_venue(
    const VenueId venue,
    VenueTxQueues& queues,
    IChildFrameEncoder& encoder) noexcept {
    if (sealed_) {
        return VenueRegistrationStatus::RegistrySealed;
    }
    if (!venue) {
        return VenueRegistrationStatus::InvalidBinding;
    }
    if (find(venue)) {
        return VenueRegistrationStatus::DuplicateVenue;
    }
    if (count_ == bindings_.size()) {
        return VenueRegistrationStatus::CapacityExceeded;
    }
    bindings_[count_++] = VenueDispatchBinding{venue, &queues, &encoder};
    return VenueRegistrationStatus::Registered;
}

bool FixedVenueRegistry::seal() noexcept {
    if (sealed_ || count_ == 0U) {
        return false;
    }
    sealed_ = true;
    return true;
}

VenueLookup FixedVenueRegistry::find(const VenueId venue) const noexcept {
    for (std::uint16_t index{}; index < count_; ++index) {
        if (bindings_[index].venue == venue) {
            return VenueLookup{&bindings_[index], index};
        }
    }
    return {};
}

bool FixedVenueRegistry::sealed() const noexcept {
    return sealed_;
}

std::size_t FixedVenueRegistry::size() const noexcept {
    return count_;
}

DispatchPipeline::DispatchPipeline(
    dataplane::RouteKernel& kernel,
    recorder::IFlightRecorder& flight_recorder,
    const FixedVenueRegistry& venues,
    const DispatchPipelinePolicy policy,
    ha::ReplicationGate* const replication_gate,
    IDualDurableReplicator* const dual_durable_replicator) noexcept
    : kernel_(&kernel),
      recorder_(&flight_recorder),
      venues_(&venues),
      policy_(policy),
      replication_gate_(replication_gate),
      dual_durable_replicator_(dual_durable_replicator) {}

bool DispatchPipeline::fail_stopped() const noexcept {
    return recovery_.required;
}

DispatchRecoveryRequirement DispatchPipeline::recovery_requirement()
    const noexcept {
    return recovery_;
}

void DispatchPipeline::require_recovery(
    const std::uint64_t producer_sequence,
    const recorder::DurableToken* const token,
    const DispatchRecoveryPhase applied_phase,
    const dataplane::CommitStatus commit_status) noexcept {
    recovery_.required = true;
    recovery_.producer_sequence = producer_sequence;
    recovery_.applied_phase = applied_phase;
    recovery_.commit_status = commit_status;
    if (token != nullptr) {
        recovery_.durable_batch_id = token->batch_id;
        recovery_.batch_hash = token->batch_hash;
    }
}

DispatchPipelineResult DispatchPipeline::try_dispatch(
    const ParentOrder& parent,
    const std::span<const dataplane::BookSlotView> pinned_views,
    const DispatchRequestMetadata metadata) noexcept {
    DispatchPipelineResult result;
    if (recovery_.required) {
        result.status = DispatchPipelineStatus::FailStopped;
        result.recovery = recovery_;
        return result;
    }
    if (kernel_ == nullptr || recorder_ == nullptr || venues_ == nullptr ||
        !valid_policy(policy_) || !parent.id || !parent.instrument ||
        metadata.producer_sequence == 0U || metadata.decision_time_ns == 0U ||
        metadata.commit_time_ns < metadata.decision_time_ns ||
        metadata.fence_time_ns == 0U) {
        return result;
    }
    if (!venues_->sealed()) {
        result.status = DispatchPipelineStatus::RegistryNotReady;
        return result;
    }
    if (replication_gate_ != nullptr) {
        if (const auto fence = preflight_fence(
                *replication_gate_,
                policy_.leadership_epoch,
                metadata.fence_time_ns)) {
            result.status = DispatchPipelineStatus::FenceRejected;
            result.fence_error = *fence;
            return result;
        }
    }

    const dataplane::PlanResult planned = kernel_->plan(
        parent, pinned_views, metadata.decision_time_ns);
    result.plan_status = planned.status;
    if (!planned.accepted()) {
        result.status = DispatchPipelineStatus::PlanRejected;
        return result;
    }

    const dataplane::RoutePlan& plan = planned.plan;
    if (plan.allocation_count == 0U ||
        plan.allocation_count > dataplane::kMaximumCandidatesPerInstrument) {
        result.status = DispatchPipelineStatus::PlanRejected;
        return result;
    }

    std::array<const VenueDispatchBinding*,
               dataplane::kMaximumCandidatesPerInstrument>
        bindings{};
    std::array<std::uint16_t, kMaximumDispatchVenues> required{};
    std::array<VenueTxQueues*, kMaximumDispatchVenues> queues_by_registry{};
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const VenueLookup lookup = venues_->find(plan.allocations[index].venue);
        if (!lookup || lookup.binding->queues == nullptr ||
            lookup.binding->encoder == nullptr) {
            result.status = DispatchPipelineStatus::VenueUnavailable;
            return result;
        }
        bindings[index] = lookup.binding;
        queues_by_registry[lookup.index] = lookup.binding->queues;
        if (required[lookup.index] ==
            std::numeric_limits<std::uint16_t>::max()) {
            result.status =
                DispatchPipelineStatus::NormalQueueCapacityUnavailable;
            return result;
        }
        ++required[lookup.index];
    }
    for (std::size_t index{}; index < venues_->size(); ++index) {
        if (required[index] == 0U) {
            continue;
        }
        if (queues_by_registry[index] == nullptr ||
            queues_by_registry[index]->normal_available_approx() <
                required[index]) {
            result.status =
                DispatchPipelineStatus::NormalQueueCapacityUnavailable;
            return result;
        }
    }

    std::array<EncodedFrame, dataplane::kMaximumCandidatesPerInstrument> frames{};
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        EncodedFrame& frame = frames[index];
        const FrameEncodeResult encoded = bindings[index]->encoder->encode_new(
            plan.parent,
            plan.allocations[index],
            plan.planned_children[index],
            frame.storage);
        result.encode_status = encoded.status;
        if (!encoded.encoded() || encoded.size > frame.storage.size()) {
            result.status = DispatchPipelineStatus::FrameEncodingFailed;
            return result;
        }
        frame.venue_id = plan.allocations[index].venue.value();
        frame.child_id = plan.planned_children[index].child_order_id.value();
        frame.dispatch_attempt_id =
            plan.planned_children[index].dispatch_attempt_id;
        frame.leadership_epoch = policy_.leadership_epoch;
        frame.configuration_generation = policy_.configuration_generation;
        frame.venue_session_id =
            plan.planned_children[index].venue_session_id;
        frame.venue_session_epoch =
            plan.planned_children[index].venue_session_epoch;
        frame.ownership_generation =
            plan.planned_children[index].ownership_generation;
        frame.dispatch_class = DispatchClass::NewOrReplace;
        frame.size = encoded.size;
        if (!frame.valid()) {
            result.status = DispatchPipelineStatus::FrameEncodingFailed;
            return result;
        }
    }

    const std::uint64_t first_child =
        plan.planned_children[0].child_order_id.value();
    const std::uint64_t first_attempt =
        plan.planned_children[0].dispatch_attempt_id;
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        if (index > std::numeric_limits<std::uint64_t>::max() - first_child ||
            index > std::numeric_limits<std::uint64_t>::max() - first_attempt ||
            plan.planned_children[index].child_order_id.value() !=
                first_child + index ||
            plan.planned_children[index].dispatch_attempt_id !=
                first_attempt + index) {
            result.status = DispatchPipelineStatus::AuthorizationNotContiguous;
            return result;
        }
    }

    recorder::DurableBatch batch(
        recorder::DurabilityLane::Normal, metadata.producer_sequence);
    batch.set_leadership_epoch(policy_.leadership_epoch);
    batch.set_configuration_generation(policy_.configuration_generation);
    if (!batch.set_dispatch_authorization(recorder::DispatchAuthorization{
            first_child,
            first_attempt,
            plan.allocation_count})) {
        result.status = DispatchPipelineStatus::DurableBatchCapacityExceeded;
        return result;
    }
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const EncodedFrame& frame = frames[index];
        if (!batch.try_add_external_effect_authorization(
                frame.venue_id,
                frame.child_id,
                frame.dispatch_attempt_id,
                frame.venue_session_id,
                frame.venue_session_epoch,
                frame.ownership_generation,
                static_cast<std::uint8_t>(frame.dispatch_class),
                frame.bytes())) {
            result.status =
                DispatchPipelineStatus::DurableBatchCapacityExceeded;
            return result;
        }
    }
    if (!build_durable_batch(
            plan,
            pinned_views,
            std::span<const EncodedFrame>{frames.data(), plan.allocation_count},
            metadata,
            batch)) {
        result.status = DispatchPipelineStatus::DurableBatchCapacityExceeded;
        return result;
    }

    if (!completion_.reset()) {
        result.status = DispatchPipelineStatus::DurableTokenUnavailable;
        return result;
    }
    result.append_status = recorder_->try_append(batch, completion_);
    if (result.append_status != recorder::AppendStatus::Committed) {
        result.status = DispatchPipelineStatus::RecorderAppendRejected;
        return result;
    }
    const std::optional<recorder::DurableToken> token = completion_.token();
    if (!token) {
        result.status = DispatchPipelineStatus::DurableTokenUnavailable;
        require_recovery(
            metadata.producer_sequence,
            nullptr,
            DispatchRecoveryPhase::DurableOnly,
            result.commit_status);
        result.recovery = recovery_;
        return result;
    }
    result.token = *token;
    const recorder::DispatchAuthorization expected{
        first_child, first_attempt, plan.allocation_count};
    if (!static_cast<bool>(result.token) ||
        result.token.producer_sequence != metadata.producer_sequence ||
        result.token.leadership_epoch != policy_.leadership_epoch ||
        result.token.configuration_generation !=
            policy_.configuration_generation ||
        result.token.dispatch_authorization != expected ||
        !recorder_->authorizes_external_effect(
            result.token, policy_.required_domain)) {
        result.status = DispatchPipelineStatus::DurableTokenRejected;
        require_recovery(
            metadata.producer_sequence,
            &result.token,
            DispatchRecoveryPhase::DurableOnly,
            result.commit_status);
        result.recovery = recovery_;
        return result;
    }
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const EncodedFrame& frame = frames[index];
        if (!result.token.authorizes_external_effect(
                frame.venue_id,
                frame.child_id,
                frame.dispatch_attempt_id,
                frame.venue_session_id,
                frame.venue_session_epoch,
                frame.ownership_generation,
                static_cast<std::uint8_t>(frame.dispatch_class),
                frame.bytes())) {
            result.status = DispatchPipelineStatus::DurableTokenRejected;
            require_recovery(
                metadata.producer_sequence,
                &result.token,
                DispatchRecoveryPhase::DurableOnly,
                result.commit_status);
            result.recovery = recovery_;
            return result;
        }
    }

    if (replication_gate_ != nullptr) {
        if (!replication_gate_->note_local_durable(
                result.token.batch_id, result.token.leadership_epoch)) {
            result.status = DispatchPipelineStatus::FenceRejected;
            result.fence_error = ha::DispatchFenceError::LocalRecordNotDurable;
            require_recovery(
                metadata.producer_sequence,
                &result.token,
                DispatchRecoveryPhase::DurableOnly,
                result.commit_status);
            result.recovery = recovery_;
            return result;
        }
        if (replication_gate_->policy() == ha::ReplicationPolicy::DualDurable) {
            if (dual_durable_replicator_ == nullptr ||
                !dual_durable_replicator_->replicate_and_wait_durable(
                    batch, result.token) ||
                !replication_gate_->note_remote_durable(
                    result.token.batch_id, result.token.leadership_epoch)) {
                result.status = DispatchPipelineStatus::DualDurabilityRejected;
                result.fence_error =
                    ha::DispatchFenceError::RemoteRecordNotDurable;
                require_recovery(
                    metadata.producer_sequence,
                    &result.token,
                    DispatchRecoveryPhase::DurableOnly,
                    result.commit_status);
                result.recovery = recovery_;
                return result;
            }
        }
        result.fence_error = replication_gate_->authorize(
            result.token.batch_id,
            result.token.leadership_epoch,
            metadata.fence_time_ns);
        if (result.fence_error != ha::DispatchFenceError::None) {
            result.status = DispatchPipelineStatus::FenceRejected;
            require_recovery(
                metadata.producer_sequence,
                &result.token,
                DispatchRecoveryPhase::DurableOnly,
                result.commit_status);
            result.recovery = recovery_;
            return result;
        }
    }

    const dataplane::CommitResult committed = kernel_->commit(
        plan, pinned_views, metadata.commit_time_ns);
    result.commit_status = committed.status;
    if (!committed.committed()) {
        result.status = DispatchPipelineStatus::KernelCommitRejected;
        require_recovery(
            metadata.producer_sequence,
            &result.token,
            DispatchRecoveryPhase::DurableOnly,
            result.commit_status);
        result.recovery = recovery_;
        return result;
    }

    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const auto permit = result.token.effect_permit(index);
        if (!bindings[index]->queues->try_enqueue_normal(
                frames[index], permit ? *permit : recorder::DurableEffectPermit{})) {
            result.status =
                DispatchPipelineStatus::QueueOwnershipInvariantViolated;
            require_recovery(
                metadata.producer_sequence,
                &result.token,
                DispatchRecoveryPhase::KernelCommitted,
                result.commit_status);
            result.recovery = recovery_;
            return result;
        }
    }
    result.child_count = plan.allocation_count;
    result.status = DispatchPipelineStatus::Dispatched;
    return result;
}

bool DispatchPipeline::build_durable_batch(
    const dataplane::RoutePlan& plan,
    const std::span<const dataplane::BookSlotView> pinned_views,
    const std::span<const EncodedFrame> frames,
    const DispatchRequestMetadata metadata,
    recorder::DurableBatch& batch) const noexcept {
    if (kernel_ == nullptr || frames.size() != plan.allocation_count) {
        return false;
    }

    CanonicalWriter writer;
    if (!writer.put_u32(0x534f5258U) ||
        !writer.put_u64(metadata.producer_sequence) ||
        !writer.put_u64(policy_.leadership_epoch) ||
        !writer.put_u64(policy_.configuration_generation) ||
        !writer.put_u64(metadata.decision_time_ns) ||
        !writer.put_u64(metadata.commit_time_ns) ||
        !writer.put_u64(metadata.fence_time_ns) ||
        !writer.put_u8(static_cast<std::uint8_t>(policy_.required_domain)) ||
        !writer.put_u64(plan.parent.id.value()) ||
        !writer.put_u32(plan.parent.instrument.value()) ||
        !writer.put_u8(static_cast<std::uint8_t>(plan.parent.side)) ||
        !writer.put_u64(plan.parent.quantity.units) ||
        !writer.put_i64(plan.parent.limit_price.units) ||
        !writer.put_u8(static_cast<std::uint8_t>(plan.parent.intent)) ||
        !append_record(batch, DispatchRecordType::ParentCommand, writer)) {
        return false;
    }

    writer.clear();
    if (!writer.put_u16(plan.candidate_pin_count)) {
        return false;
    }
    for (std::size_t index{}; index < plan.candidate_pin_count; ++index) {
        const dataplane::DataplaneCandidatePin& pin = plan.candidate_pins[index];
        if (!writer.put_u32(pin.market.venue.value()) ||
            !writer.put_u64(pin.market.adjacency_generation) ||
            !writer.put_bool(pin.market.book_version.has_value())) {
            return false;
        }
        if (pin.market.book_version &&
            !write_book_version(writer, *pin.market.book_version)) {
            return false;
        }
        if (!writer.put_u64(pin.feed_epoch) ||
            !writer.put_u64(pin.source_health_generation) ||
            !writer.put_u64(pin.tx_credit_generation) ||
            !writer.put_u64(pin.throttle_generation) ||
            !writer.put_u32(pin.normal_tx_credits) ||
            !writer.put_u32(pin.cancel_reserved_tx_credits) ||
            !writer.put_u32(pin.throttle_credits) ||
            !write_adaptive_policy_overlay(writer, pin.adaptive_policy) ||
            !write_view_metadata(
                writer, find_view(pinned_views, pin.market.venue))) {
            return false;
        }
    }

    const std::size_t slice_count_offset = writer.reserve_u16();
    std::uint16_t slice_count{};
    for (std::size_t pin_index{};
         pin_index < plan.candidate_pin_count;
         ++pin_index) {
        const dataplane::BookSlotView* const view = find_view(
            pinned_views, plan.candidate_pins[pin_index].market.venue);
        if (view == nullptr || !view->has_book) {
            continue;
        }
        const bool consume_asks = plan.parent.side == Side::Buy;
        const std::uint16_t available = consume_asks ?
            view->book.ask_count : view->book.bid_count;
        const std::uint16_t recorded =
            plan.parent.intent == RoutingIntent::PassiveGTC ?
                std::min<std::uint16_t>(available, 1U) : available;
        for (std::uint16_t level{}; level < recorded; ++level) {
            if (slice_count == kMaximumRecordedDepthSlices) {
                return false;
            }
            const md::Level& depth = consume_asks ?
                view->book.asks[level] : view->book.bids[level];
            if (!write_depth_slice(
                    writer,
                    view->venue,
                    consume_asks ? Side::Sell : Side::Buy,
                    level,
                    depth)) {
                return false;
            }
            ++slice_count;
        }
    }
    if (!writer.patch_u16(slice_count_offset, slice_count) ||
        !append_record(
            batch, DispatchRecordType::PinnedRoutingInputs, writer)) {
        return false;
    }

    writer.clear();
    const dataplane::InstrumentRiskCounters instrument_before =
        kernel_->instrument_counters(plan.parent.instrument);
    const auto projected_gross = checked_add(
        instrument_before.gross_outstanding, plan.allocated);
    if (!instrument_before.found || !projected_gross ||
        instrument_before.open_parents ==
            std::numeric_limits<std::uint64_t>::max() ||
        plan.allocation_count >
            std::numeric_limits<std::uint64_t>::max() -
                instrument_before.open_children ||
        !writer.put_u64(plan.kernel_state_generation) ||
        !writer.put_u64(plan.candidate_table_generation) ||
        !writer.put_u64(plan.risk_generation) ||
        !writer.put_u64(plan.monotonic_decision_time_ns) ||
        !writer.put_u64(plan.next_child_order_id) ||
        !writer.put_u64(plan.next_dispatch_attempt_id) ||
        !writer.put_u64(instrument_before.gross_outstanding.units) ||
        !writer.put_u64(projected_gross->units) ||
        !writer.put_u64(instrument_before.open_parents) ||
        !writer.put_u64(instrument_before.open_parents + 1U) ||
        !writer.put_u64(instrument_before.open_children) ||
        !writer.put_u64(
            instrument_before.open_children + plan.allocation_count) ||
        !writer.put_u64(plan.allocated.units) ||
        !writer.put_u64(plan.remainder.units) ||
        !writer.put_u8(static_cast<std::uint8_t>(plan.remainder_reason)) ||
        !writer.put_u8(static_cast<std::uint8_t>(plan.reject_reason)) ||
        !writer.put_bool(plan.observed_truncated_depth) ||
        !writer.put_u16(plan.exclusion_count)) {
        return false;
    }
    for (std::size_t index{}; index < plan.exclusion_count; ++index) {
        if (!writer.put_u32(plan.exclusions[index].venue.value()) ||
            !writer.put_u8(
                static_cast<std::uint8_t>(plan.exclusions[index].reason))) {
            return false;
        }
    }
    if (!writer.put_u16(plan.allocation_count)) {
        return false;
    }
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const ChildAllocation& allocation = plan.allocations[index];
        const dataplane::PlannedChild& child = plan.planned_children[index];
        const dataplane::VenueRiskCounters venue_before =
            kernel_->venue_counters(allocation.venue);
        const auto venue_projected = checked_add(
            venue_before.outstanding, allocation.quantity);
        if (!venue_before.found || !venue_projected ||
            child.wire_identity.size > child.wire_identity.bytes.size() ||
            !writer.put_u32(allocation.venue.value()) ||
            !writer.put_u64(allocation.quantity.units) ||
            !writer.put_i64(allocation.limit_price.units) ||
            !writer.put_u64(allocation.venue_quantity.units) ||
            !writer.put_i64(allocation.venue_limit_price.units) ||
            !writer.put_u32(allocation.allocation_order) ||
            !writer.put_u64(allocation.adjacency_generation) ||
            !write_book_version(writer, allocation.book_version) ||
            !writer.put_u64(venue_before.outstanding.units) ||
            !writer.put_u64(venue_projected->units) ||
            !writer.put_u64(child.child_order_id.value()) ||
            !writer.put_u64(child.dispatch_attempt_id) ||
            !writer.put_u8(child.wire_identity.size) ||
            !writer.put_bytes({
                child.wire_identity.bytes.data(), child.wire_identity.size}) ||
            !writer.put_u64(child.tx_credit_generation) ||
            !writer.put_u64(child.throttle_generation) ||
            !writer.put_u16(child.normal_tx_credit_delta) ||
            !writer.put_u16(child.cancel_tx_credit_delta) ||
            !writer.put_u16(child.throttle_credit_delta)) {
            return false;
        }
    }
    if (!append_record(
            batch, DispatchRecordType::RouteAndRiskDecision, writer)) {
        return false;
    }

    writer.clear();
    if (!writer.put_u16(plan.allocation_count)) {
        return false;
    }
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const dataplane::PlannedChild& child = plan.planned_children[index];
        const EncodedFrame& frame = frames[index];
        if (child.wire_identity.size > child.wire_identity.bytes.size() ||
            !frame.valid() ||
            !writer.put_u32(frame.venue_id) ||
            !writer.put_u64(frame.child_id) ||
            !writer.put_u64(frame.dispatch_attempt_id) ||
            !writer.put_u64(frame.leadership_epoch) ||
            !writer.put_u64(frame.configuration_generation) ||
            !writer.put_u8(static_cast<std::uint8_t>(frame.dispatch_class)) ||
            !writer.put_u8(child.wire_identity.size) ||
            !writer.put_bytes({
                child.wire_identity.bytes.data(), child.wire_identity.size}) ||
            !writer.put_u16(frame.size) ||
            !writer.put_bytes(frame.bytes())) {
            return false;
        }
    }
    if (!writer.put_u16(plan.allocation_count)) {
        return false;
    }
    for (std::size_t index{}; index < plan.allocation_count; ++index) {
        const dataplane::PlannedChild& child = plan.planned_children[index];
        const EncodedFrame& frame = frames[index];
        if (child.venue_session_id == 0U ||
            child.venue_session_epoch == 0U ||
            child.ownership_generation == 0U ||
            child.wire_identity.size == 0U ||
            child.wire_identity.size > child.wire_identity.bytes.size() ||
            frame.venue_id != plan.allocations[index].venue.value() ||
            frame.child_id != child.child_order_id.value() ||
            frame.dispatch_attempt_id != child.dispatch_attempt_id ||
            !writer.put_u32(frame.venue_id) ||
            !writer.put_u64(frame.child_id) ||
            !writer.put_u64(child.venue_session_id) ||
            !writer.put_u64(child.venue_session_epoch) ||
            !writer.put_u64(child.ownership_generation) ||
            !writer.put_u64(child.dispatch_attempt_id) ||
            !writer.put_u8(child.wire_identity.size) ||
            !writer.put_bytes(child.wire_identity.value())) {
            return false;
        }
    }
    return append_record(batch, DispatchRecordType::ChildDispatches, writer) &&
           batch.valid();
}

TxOwner::TxOwner(
    VenueTxQueues& queues,
    TokenGatedOrderEntrySession& session) noexcept
    : queues_(&queues), session_(&session) {}

TxPollResult TxOwner::try_pending(
    AuthorizedFrame& pending,
    bool& has_pending,
    const bool cancel) noexcept {
    if (!has_pending || session_ == nullptr) {
        return TxPollResult::Idle;
    }
    const SendResult sent = session_->try_send(pending.frame, pending.permit);
    if (sent == SendResult::Sent) {
        has_pending = false;
        if (cancel) {
            ++statistics_.sent_cancel;
            return TxPollResult::SentCancel;
        }
        ++statistics_.sent_normal;
        return TxPollResult::SentNormal;
    }
    if (sent == SendResult::WouldBlock) {
        ++statistics_.backpressured;
        return TxPollResult::Backpressured;
    }
    has_pending = false;
    ++statistics_.rejected;
    statistics_.last_rejection = sent;
    require_recovery(pending, sent, cancel);
    return TxPollResult::RecoveryRequired;
}

TxPollResult TxOwner::poll_once() noexcept {
    ++statistics_.poll_iterations;
    if (queues_ == nullptr || session_ == nullptr) {
        ++statistics_.rejected;
        statistics_.last_rejection = SendResult::TransportFailure;
        return TxPollResult::Rejected;
    }

    // A failed cancel/reconciliation effect, or any transport result for which
    // descriptor ownership is unknown, invalidates the venue session itself.
    // Do not dequeue even reserved traffic until reconciliation establishes a
    // new owner/session. Only a non-ambiguous invariant rejection of a normal
    // frame may continue to service already-authorized reserved cancels.
    if (recovery_.required &&
        (recovery_.cancel_or_reconcile ||
         recovery_.delivery_ambiguous || recovery_.session_fatal)) {
        return TxPollResult::RecoveryRequired;
    }

    if (!has_pending_cancel_) {
        has_pending_cancel_ = queues_->try_pop_cancel(pending_cancel_);
    }
    if (has_pending_cancel_) {
        return try_pending(pending_cancel_, has_pending_cancel_, true);
    }

    // Once a durable effect has failed final transmission, normal order flow
    // is recovery-only. The failed attempt is deliberately discarded and is
    // never retried. Reserved traffic above remains serviceable only for the
    // non-ambiguous normal-frame rejection case described at function entry.
    if (recovery_.required) {
        return TxPollResult::RecoveryRequired;
    }

    if (!has_pending_normal_) {
        has_pending_normal_ = queues_->try_pop_normal(pending_normal_);
    }
    if (has_pending_normal_) {
        return try_pending(pending_normal_, has_pending_normal_, false);
    }
    return TxPollResult::Idle;
}

void TxOwner::run_until_stopped(
    const std::atomic<bool>& stop_requested) noexcept {
    while (!stop_requested.load(std::memory_order_acquire)) {
        static_cast<void>(poll_once());
    }
}

TxOwnerStatistics TxOwner::statistics() const noexcept {
    return statistics_;
}

bool TxOwner::recovery_required() const noexcept {
    return recovery_.required;
}

TxRecoveryRequirement TxOwner::recovery_requirement() const noexcept {
    return recovery_;
}

void TxOwner::require_recovery(
    const AuthorizedFrame& frame,
    const SendResult rejection,
    const bool cancel) noexcept {
    if (recovery_.required) {
        return;
    }
    recovery_ = TxRecoveryRequirement{
        true,
        rejection == SendResult::TransportClosed ||
            rejection == SendResult::TransportFailure,
        rejection == SendResult::WrongVenue,
        cancel,
        frame.permit.batch_id,
        frame.permit.producer_sequence,
        frame.permit.leadership_epoch,
        frame.permit.configuration_generation,
        frame.frame.venue_id,
        frame.frame.child_id,
        frame.frame.dispatch_attempt_id,
        frame.frame.venue_session_id,
        frame.frame.venue_session_epoch,
        frame.frame.ownership_generation,
        frame.permit.effect_ordinal,
        frame.permit.lane,
        frame.permit.domain,
        rejection,
        frame.permit.authorization.frame_hash,
        frame.permit.batch_hash,
    };
}

}  // namespace sorx::fastpath
