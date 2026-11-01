#include "sorx/fastpath/risk_dispatch_pipeline.hpp"

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
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
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
        return put_u16(0U) ? offset : storage_.size();
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

    [[nodiscard]] bool valid() const noexcept { return valid_; }

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

[[nodiscard]] bool append_record(
    recorder::DurableBatch& batch,
    const RiskDispatchRecordType type,
    const CanonicalWriter& writer) noexcept {
    return writer.valid() && batch.try_append_record(
        static_cast<std::uint16_t>(type),
        kRiskDispatchRecordVersion,
        writer.bytes());
}

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

[[nodiscard]] constexpr RemainderAccountingStatus remainder_status(
    const risk::RiskAccountingStatus status) noexcept {
    switch (status) {
        case risk::RiskAccountingStatus::Applied:
            return RemainderAccountingStatus::Applied;
        case risk::RiskAccountingStatus::ParentNotFound:
            return RemainderAccountingStatus::ParentNotFound;
        case risk::RiskAccountingStatus::InvalidQuantity:
            return RemainderAccountingStatus::InvalidQuantity;
        case risk::RiskAccountingStatus::QuantityExceedsReservation:
            return RemainderAccountingStatus::QuantityExceedsReservation;
        case risk::RiskAccountingStatus::InvariantViolation:
            return RemainderAccountingStatus::InvariantViolation;
        case risk::RiskAccountingStatus::GenerationExhausted:
            return RemainderAccountingStatus::GenerationExhausted;
        case risk::RiskAccountingStatus::RecoveryOnly:
            return RemainderAccountingStatus::RecoveryOnly;
    }
    return RemainderAccountingStatus::InvariantViolation;
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
    return write_book_version(writer, view->book.version()) &&
        writer.put_u32(view->book.channel.value()) &&
        writer.put_u32(view->book.profile.value()) &&
        writer.put_i64(view->book.exchange_timestamp_ns) &&
        writer.put_u64(view->book.local_receive_time_ns) &&
        writer.put_u64(view->book.local_publication_time_ns) &&
        writer.put_u8(static_cast<std::uint8_t>(view->book.feed_state)) &&
        writer.put_u8(static_cast<std::uint8_t>(view->book.trading_state)) &&
        writer.put_bool(view->book.depth_truncated);
}

[[nodiscard]] bool write_depth_slice(
    CanonicalWriter& writer,
    const VenueId venue,
    const Side side,
    const std::uint16_t index,
    const md::Level& level) noexcept {
    return writer.put_u32(venue.value()) &&
        writer.put_u8(static_cast<std::uint8_t>(side)) &&
        writer.put_u16(index) && writer.put_i64(level.price.units) &&
        writer.put_u64(level.quantity.units) &&
        writer.put_u32(level.order_count);
}

[[nodiscard]] bool write_risk_command(
    CanonicalWriter& writer,
    const risk::RiskCommand& command) noexcept {
    return writer.put_u8(static_cast<std::uint8_t>(command.kind)) &&
        writer.put_u64(command.request_id) &&
        writer.put_bytes(command.payload_digest) &&
        writer.put_u64(command.client_id) &&
        writer.put_u64(command.account_id) &&
        writer.put_u64(command.parent_order_id.value()) &&
        writer.put_u32(command.instrument.value()) &&
        writer.put_u8(static_cast<std::uint8_t>(command.side)) &&
        writer.put_u64(command.quantity.units) &&
        writer.put_i64(command.limit_price.units);
}

[[nodiscard]] bool write_scope_counters(
    CanonicalWriter& writer,
    const risk::ScopeRiskCounters& counters) noexcept {
    return writer.put_bool(counters.found) &&
        writer.put_u64(counters.open_quantity.units) &&
        writer.put_u64(counters.open_notional.units) &&
        writer.put_u32(counters.open_orders) &&
        writer.put_u32(counters.normal_tokens) &&
        writer.put_u32(counters.cancel_tokens);
}

[[nodiscard]] bool valid_policy(const DispatchPipelinePolicy policy) noexcept {
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

[[nodiscard]] bool command_matches_parent(
    const risk::RiskCommand& command,
    const ParentOrder& parent) noexcept {
    return command.kind == risk::RiskCommandKind::NewOrder &&
        command.parent_order_id == parent.id &&
        command.instrument == parent.instrument && command.side == parent.side &&
        command.quantity == parent.quantity &&
        command.limit_price == parent.limit_price;
}

}  // namespace

RiskDispatchPipeline::RiskDispatchPipeline(
    risk::RiskKernel& account_risk,
    dataplane::RouteKernel& route_kernel,
    recorder::IFlightRecorder& flight_recorder,
    const FixedVenueRegistry& venues,
    const DispatchPipelinePolicy policy,
    ha::ReplicationGate* const replication_gate,
    IDualDurableReplicator* const dual_durable_replicator) noexcept
    : account_risk_(&account_risk),
      route_kernel_(&route_kernel),
      recorder_(&flight_recorder),
      venues_(&venues),
      policy_(policy),
      replication_gate_(replication_gate),
      dual_durable_replicator_(dual_durable_replicator) {}

bool RiskDispatchPipeline::fail_stopped() const noexcept {
    return recovery_.required;
}

RiskDispatchRecoveryRequirement
RiskDispatchPipeline::recovery_requirement() const noexcept {
    return recovery_;
}

void RiskDispatchPipeline::require_recovery(
    const recorder::DurableToken& token,
    const std::uint64_t fallback_producer_sequence,
    const RecoveryCommitPhase applied_phase,
    const risk::RiskCommitStatus risk_status,
    const dataplane::CommitStatus route_status,
    const Quantity unrouted_remainder,
    const RemainderAccountingStatus remainder_accounting_status) noexcept {
    recovery_.required = true;
    recovery_.durable_locator_complete =
        token.batch_id != 0U && std::any_of(
            token.batch_hash.begin(),
            token.batch_hash.end(),
            [](const std::byte value) noexcept { return value != std::byte{}; });
    recovery_.durable_batch_id = token.batch_id;
    recovery_.producer_sequence = token.producer_sequence != 0U
        ? token.producer_sequence
        : fallback_producer_sequence;
    recovery_.applied_phase = applied_phase;
    recovery_.risk_commit_status = risk_status;
    recovery_.route_commit_status = route_status;
    recovery_.unrouted_remainder = unrouted_remainder;
    recovery_.remainder_accounting_status = remainder_accounting_status;
    recovery_.batch_hash = token.batch_hash;
}

RiskDispatchResult RiskDispatchPipeline::try_dispatch(
    const risk::RiskCommand& command,
    const ParentOrder& parent,
    const std::span<const dataplane::BookSlotView> pinned_views,
    const DispatchRequestMetadata metadata) noexcept {
    RiskDispatchResult result;
    if (recovery_.required) {
        result.status = RiskDispatchStatus::FailStopped;
        result.recovery = recovery_;
        return result;
    }
    if (account_risk_ == nullptr || route_kernel_ == nullptr ||
        recorder_ == nullptr || venues_ == nullptr || !valid_policy(policy_) ||
        !command_matches_parent(command, parent) ||
        metadata.producer_sequence == 0U || metadata.decision_time_ns == 0U ||
        metadata.commit_time_ns < metadata.decision_time_ns ||
        metadata.fence_time_ns == 0U) {
        return result;
    }
    if (!venues_->sealed()) {
        result.status = RiskDispatchStatus::RegistryNotReady;
        return result;
    }
    if (replication_gate_ != nullptr) {
        if (const auto fence = preflight_fence(
                *replication_gate_,
                policy_.leadership_epoch,
                metadata.fence_time_ns)) {
            result.status = RiskDispatchStatus::FenceRejected;
            result.fence_error = *fence;
            return result;
        }
    }

    const risk::RiskPlanResult risk_plan =
        account_risk_->plan(command, metadata.decision_time_ns);
    result.risk_disposition = risk_plan.disposition;
    result.risk_verdict = risk_plan.verdict;
    if (risk_plan.disposition == risk::RiskPlanDisposition::DuplicateReplay) {
        result.status = RiskDispatchStatus::RiskDuplicateReplay;
        return result;
    }
    if (risk_plan.disposition == risk::RiskPlanDisposition::DuplicateConflict) {
        result.status = RiskDispatchStatus::RiskDuplicateConflict;
        return result;
    }
    if (risk_plan.disposition != risk::RiskPlanDisposition::Planned ||
        risk_plan.verdict != risk::RiskVerdict::Allowed ||
        !risk_plan.delta.committable()) {
        result.status = RiskDispatchStatus::RiskRejected;
        return result;
    }
    if (risk_plan.delta.configuration_generation !=
        policy_.configuration_generation) {
        result.status = RiskDispatchStatus::GenerationMismatch;
        return result;
    }

    const dataplane::PlanResult route_plan = route_kernel_->plan(
        parent, pinned_views, metadata.decision_time_ns);
    result.route_plan_status = route_plan.status;
    if (!route_plan.accepted()) {
        result.status = RiskDispatchStatus::RouteRejected;
        return result;
    }
    if (risk_plan.delta.risk_generation != route_plan.plan.risk_generation) {
        result.status = RiskDispatchStatus::GenerationMismatch;
        return result;
    }
    const dataplane::RoutePlan& plan = route_plan.plan;
    if (plan.allocation_count == 0U ||
        plan.allocation_count > dataplane::kMaximumCandidatesPerInstrument) {
        result.status = RiskDispatchStatus::RouteRejected;
        return result;
    }
    const auto planned_total = checked_add(plan.allocated, plan.remainder);
    if (!planned_total || *planned_total != risk_plan.delta.reservation_quantity ||
        *planned_total != parent.quantity) {
        result.status = RiskDispatchStatus::RouteRejected;
        return result;
    }
    result.unrouted_remainder = plan.remainder;
    if (plan.remainder) {
        result.remainder_accounting_status =
            RemainderAccountingStatus::Pending;
        constexpr std::uint64_t kMaximumStateGeneration =
            std::numeric_limits<std::uint64_t>::max();
        if (risk_plan.delta.kernel_state_generation >
            kMaximumStateGeneration - 2U) {
            result.status =
                RiskDispatchStatus::RemainderAccountingPreflightRejected;
            result.remainder_accounting_status =
                RemainderAccountingStatus::GenerationExhausted;
            return result;
        }
    }

    std::array<const VenueDispatchBinding*,
               dataplane::kMaximumCandidatesPerInstrument>
        bindings{};
    std::array<std::uint16_t, kMaximumDispatchVenues> required{};
    std::array<VenueTxQueues*, kMaximumDispatchVenues> queues_by_registry{};
    for (std::size_t index = 0U; index < plan.allocation_count; ++index) {
        const VenueLookup lookup = venues_->find(plan.allocations[index].venue);
        if (!lookup || lookup.binding->queues == nullptr ||
            lookup.binding->encoder == nullptr) {
            result.status = RiskDispatchStatus::VenueUnavailable;
            return result;
        }
        bindings[index] = lookup.binding;
        queues_by_registry[lookup.index] = lookup.binding->queues;
        if (required[lookup.index] ==
            std::numeric_limits<std::uint16_t>::max()) {
            result.status =
                RiskDispatchStatus::NormalQueueCapacityUnavailable;
            return result;
        }
        ++required[lookup.index];
    }
    for (std::size_t index = 0U; index < venues_->size(); ++index) {
        if (required[index] != 0U &&
            (queues_by_registry[index] == nullptr ||
             queues_by_registry[index]->normal_available_approx() <
                 required[index])) {
            result.status =
                RiskDispatchStatus::NormalQueueCapacityUnavailable;
            return result;
        }
    }

    std::array<EncodedFrame, dataplane::kMaximumCandidatesPerInstrument> frames{};
    for (std::size_t index = 0U; index < plan.allocation_count; ++index) {
        EncodedFrame& frame = frames[index];
        const FrameEncodeResult encoded = bindings[index]->encoder->encode_new(
            plan.parent,
            plan.allocations[index],
            plan.planned_children[index],
            frame.storage);
        result.encode_status = encoded.status;
        if (!encoded.encoded() || encoded.size > frame.storage.size()) {
            result.status = RiskDispatchStatus::FrameEncodingFailed;
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
            result.status = RiskDispatchStatus::FrameEncodingFailed;
            return result;
        }
    }

    const std::uint64_t first_child =
        plan.planned_children[0U].child_order_id.value();
    const std::uint64_t first_attempt =
        plan.planned_children[0U].dispatch_attempt_id;
    for (std::size_t index = 0U; index < plan.allocation_count; ++index) {
        if (index > std::numeric_limits<std::uint64_t>::max() - first_child ||
            index > std::numeric_limits<std::uint64_t>::max() - first_attempt ||
            plan.planned_children[index].child_order_id.value() !=
                first_child + index ||
            plan.planned_children[index].dispatch_attempt_id !=
                first_attempt + index) {
            result.status = RiskDispatchStatus::AuthorizationNotContiguous;
            return result;
        }
    }

    recorder::DurableBatch batch(
        recorder::DurabilityLane::Normal, metadata.producer_sequence);
    batch.set_leadership_epoch(policy_.leadership_epoch);
    batch.set_configuration_generation(policy_.configuration_generation);
    if (!batch.set_dispatch_authorization(recorder::DispatchAuthorization{
            first_child, first_attempt, plan.allocation_count})) {
        result.status = RiskDispatchStatus::DurableBatchCapacityExceeded;
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
            result.status = RiskDispatchStatus::DurableBatchCapacityExceeded;
            return result;
        }
    }
    if (!build_durable_batch(
            risk_plan.delta,
            plan,
            pinned_views,
            std::span<const EncodedFrame>{
                frames.data(), plan.allocation_count},
            metadata,
            batch)) {
        result.status = RiskDispatchStatus::DurableBatchCapacityExceeded;
        return result;
    }
    if (!completion_.reset()) {
        result.status = RiskDispatchStatus::DurableTokenUnavailable;
        return result;
    }
    result.append_status = recorder_->try_append(batch, completion_);
    if (result.append_status != recorder::AppendStatus::Committed) {
        result.status = RiskDispatchStatus::RecorderAppendRejected;
        return result;
    }
    const std::optional<recorder::DurableToken> token = completion_.token();
    if (!token) {
        result.status = RiskDispatchStatus::DurableTokenUnavailable;
        require_recovery(
            {},
            metadata.producer_sequence,
            RecoveryCommitPhase::DurableOnly,
            result.risk_commit_status,
            result.route_commit_status,
            result.unrouted_remainder,
            result.remainder_accounting_status);
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
        result.status = RiskDispatchStatus::DurableTokenRejected;
        require_recovery(
            result.token,
            metadata.producer_sequence,
            RecoveryCommitPhase::DurableOnly,
            result.risk_commit_status,
            result.route_commit_status,
            result.unrouted_remainder,
            result.remainder_accounting_status);
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
            result.status = RiskDispatchStatus::DurableTokenRejected;
            require_recovery(
                result.token,
                metadata.producer_sequence,
                RecoveryCommitPhase::DurableOnly,
                result.risk_commit_status,
                result.route_commit_status,
                result.unrouted_remainder,
                result.remainder_accounting_status);
            result.recovery = recovery_;
            return result;
        }
    }

    if (replication_gate_ != nullptr) {
        if (!replication_gate_->note_local_durable(
                result.token.batch_id, result.token.leadership_epoch)) {
            result.status = RiskDispatchStatus::FenceRejected;
            result.fence_error = ha::DispatchFenceError::LocalRecordNotDurable;
            require_recovery(
                result.token,
                metadata.producer_sequence,
                RecoveryCommitPhase::DurableOnly,
                result.risk_commit_status,
                result.route_commit_status,
                result.unrouted_remainder,
                result.remainder_accounting_status);
            result.recovery = recovery_;
            return result;
        }
        if (replication_gate_->policy() == ha::ReplicationPolicy::DualDurable) {
            if (dual_durable_replicator_ == nullptr ||
                !dual_durable_replicator_->replicate_and_wait_durable(
                    batch, result.token) ||
                !replication_gate_->note_remote_durable(
                    result.token.batch_id, result.token.leadership_epoch)) {
                result.status = RiskDispatchStatus::DualDurabilityRejected;
                result.fence_error =
                    ha::DispatchFenceError::RemoteRecordNotDurable;
                require_recovery(
                    result.token,
                    metadata.producer_sequence,
                    RecoveryCommitPhase::DurableOnly,
                    result.risk_commit_status,
                    result.route_commit_status,
                    result.unrouted_remainder,
                    result.remainder_accounting_status);
                result.recovery = recovery_;
                return result;
            }
        }
        result.fence_error = replication_gate_->authorize(
            result.token.batch_id,
            result.token.leadership_epoch,
            metadata.fence_time_ns);
        if (result.fence_error != ha::DispatchFenceError::None) {
            result.status = RiskDispatchStatus::FenceRejected;
            require_recovery(
                result.token,
                metadata.producer_sequence,
                RecoveryCommitPhase::DurableOnly,
                result.risk_commit_status,
                result.route_commit_status,
                result.unrouted_remainder,
                result.remainder_accounting_status);
            result.recovery = recovery_;
            return result;
        }
    }

    result.risk_commit_status = account_risk_->commit(risk_plan.delta);
    if (result.risk_commit_status != risk::RiskCommitStatus::Committed) {
        result.status = RiskDispatchStatus::RiskCommitRejectedFailStop;
        require_recovery(
            result.token,
            metadata.producer_sequence,
            RecoveryCommitPhase::DurableOnly,
            result.risk_commit_status,
            result.route_commit_status,
            result.unrouted_remainder,
            result.remainder_accounting_status);
        result.recovery = recovery_;
        return result;
    }

    RecoveryCommitPhase route_failure_phase =
        RecoveryCommitPhase::RiskCommitted;
    if (plan.remainder) {
        const risk::RiskAccountingStatus release_status =
            account_risk_->release(parent.id, plan.remainder);
        result.remainder_accounting_status = remainder_status(release_status);
        if (release_status != risk::RiskAccountingStatus::Applied) {
            result.status =
                RiskDispatchStatus::RemainderReleaseRejectedFailStop;
            require_recovery(
                result.token,
                metadata.producer_sequence,
                RecoveryCommitPhase::RiskCommitted,
                result.risk_commit_status,
                result.route_commit_status,
                result.unrouted_remainder,
                result.remainder_accounting_status);
            result.recovery = recovery_;
            return result;
        }
        route_failure_phase = RecoveryCommitPhase::RiskAndRemainderReleased;
    }

    const dataplane::CommitResult route_commit = route_kernel_->commit(
        plan, pinned_views, metadata.commit_time_ns);
    result.route_commit_status = route_commit.status;
    if (!route_commit.committed()) {
        result.status = RiskDispatchStatus::RouteCommitRejectedFailStop;
        require_recovery(
            result.token,
            metadata.producer_sequence,
            route_failure_phase,
            result.risk_commit_status,
            result.route_commit_status,
            result.unrouted_remainder,
            result.remainder_accounting_status);
        result.recovery = recovery_;
        return result;
    }

    for (std::size_t index = 0U; index < plan.allocation_count; ++index) {
        const auto permit = result.token.effect_permit(index);
        if (!bindings[index]->queues->try_enqueue_normal(
                frames[index], permit ? *permit : recorder::DurableEffectPermit{})) {
            result.status =
                RiskDispatchStatus::QueueOwnershipInvariantFailStop;
            require_recovery(
                result.token,
                metadata.producer_sequence,
                RecoveryCommitPhase::RiskAndRouteCommitted,
                result.risk_commit_status,
                result.route_commit_status,
                result.unrouted_remainder,
                result.remainder_accounting_status);
            result.recovery = recovery_;
            return result;
        }
    }
    result.child_count = plan.allocation_count;
    result.status = RiskDispatchStatus::Dispatched;
    return result;
}

bool RiskDispatchPipeline::build_durable_batch(
    const risk::RiskDelta& risk_delta,
    const dataplane::RoutePlan& route_plan,
    const std::span<const dataplane::BookSlotView> pinned_views,
    const std::span<const EncodedFrame> frames,
    const DispatchRequestMetadata metadata,
    recorder::DurableBatch& batch) const noexcept {
    if (account_risk_ == nullptr || route_kernel_ == nullptr ||
        frames.size() != route_plan.allocation_count) {
        return false;
    }

    CanonicalWriter writer;
    if (!writer.put_u32(kRiskDispatchSchemaMagic) ||
        !writer.put_u16(kRiskDispatchRecordVersion) ||
        !writer.put_u64(metadata.producer_sequence) ||
        !writer.put_u64(policy_.leadership_epoch) ||
        !writer.put_u64(policy_.configuration_generation) ||
        !writer.put_u64(metadata.decision_time_ns) ||
        !writer.put_u64(metadata.commit_time_ns) ||
        !writer.put_u64(metadata.fence_time_ns) ||
        !writer.put_u8(static_cast<std::uint8_t>(policy_.required_domain)) ||
        !write_risk_command(writer, risk_delta.command) ||
        !writer.put_u8(static_cast<std::uint8_t>(route_plan.parent.intent)) ||
        !append_record(
            batch, RiskDispatchRecordType::ClientAccountCommand, writer)) {
        return false;
    }

    const risk::RiskKernelMetadata risk_metadata = account_risk_->metadata();
    const risk::ScopeRiskCounters client_before =
        account_risk_->client_counters(risk_delta.command.client_id);
    const risk::ScopeRiskCounters account_before =
        account_risk_->account_counters(risk_delta.command.account_id);
    const auto planned_total = checked_add(
        route_plan.allocated, route_plan.remainder);
    const auto reservation_notional = checked_notional(
        risk_delta.command.limit_price, risk_delta.reservation_quantity);
    const auto released_notional = checked_notional(
        risk_delta.command.limit_price, route_plan.remainder);
    const auto final_notional = checked_notional(
        risk_delta.command.limit_price, route_plan.allocated);
    const auto client_quantity_after = checked_add(
        client_before.open_quantity, risk_delta.reservation_quantity);
    const auto account_quantity_after = checked_add(
        account_before.open_quantity, risk_delta.reservation_quantity);
    if (!client_before.found || !account_before.found || !planned_total ||
        *planned_total != risk_delta.reservation_quantity ||
        risk_delta.reservation_quantity != route_plan.parent.quantity ||
        !reservation_notional ||
        *reservation_notional != risk_delta.reservation_notional ||
        !released_notional || !final_notional ||
        released_notional->units > risk_delta.reservation_notional.units ||
        final_notional->units !=
            risk_delta.reservation_notional.units - released_notional->units ||
        risk_metadata.risk_generation != risk_delta.risk_generation ||
        risk_metadata.configuration_generation !=
            risk_delta.configuration_generation ||
        risk_metadata.kernel_state_generation !=
            risk_delta.kernel_state_generation ||
        !client_quantity_after || !account_quantity_after ||
        risk_delta.reservation_notional.units >
            std::numeric_limits<std::uint64_t>::max() -
                client_before.open_notional.units ||
        risk_delta.reservation_notional.units >
            std::numeric_limits<std::uint64_t>::max() -
                account_before.open_notional.units ||
        client_before.open_orders == std::numeric_limits<std::uint32_t>::max() ||
        account_before.open_orders == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const risk::ScopeRiskCounters client_full{
        true,
        *client_quantity_after,
        Notional{client_before.open_notional.units +
                 risk_delta.reservation_notional.units},
        client_before.open_orders + 1U,
        risk_delta.normal_tokens_after,
        risk_delta.cancel_tokens_after,
    };
    const risk::ScopeRiskCounters account_full{
        true,
        *account_quantity_after,
        Notional{account_before.open_notional.units +
                 risk_delta.reservation_notional.units},
        account_before.open_orders + 1U,
        0U,
        0U,
    };
    if (client_full.open_quantity < route_plan.remainder ||
        account_full.open_quantity < route_plan.remainder ||
        client_full.open_notional < *released_notional ||
        account_full.open_notional < *released_notional) {
        return false;
    }
    const risk::ScopeRiskCounters client_final{
        true,
        Quantity{client_full.open_quantity.units - route_plan.remainder.units},
        Notional{client_full.open_notional.units - released_notional->units},
        client_full.open_orders,
        client_full.normal_tokens,
        client_full.cancel_tokens,
    };
    const risk::ScopeRiskCounters account_final{
        true,
        Quantity{account_full.open_quantity.units - route_plan.remainder.units},
        Notional{account_full.open_notional.units - released_notional->units},
        account_full.open_orders,
        account_full.normal_tokens,
        account_full.cancel_tokens,
    };
    const std::uint64_t full_state_generation =
        risk_delta.kernel_state_generation + 1U;
    const std::uint64_t final_state_generation = route_plan.remainder
        ? full_state_generation + 1U
        : full_state_generation;
    const RemainderAccountingStatus expected_remainder_status =
        route_plan.remainder ? RemainderAccountingStatus::Applied
                             : RemainderAccountingStatus::NotRequired;
    writer.clear();
    if (!writer.put_u8(static_cast<std::uint8_t>(risk_delta.disposition)) ||
        !writer.put_u8(static_cast<std::uint8_t>(risk_delta.verdict)) ||
        !write_risk_command(writer, risk_delta.command) ||
        !writer.put_u64(risk_delta.command_fingerprint) ||
        !writer.put_u64(risk_delta.risk_generation) ||
        !writer.put_u64(risk_delta.configuration_generation) ||
        !writer.put_u64(risk_delta.kernel_state_generation) ||
        !writer.put_u64(risk_delta.monotonic_time_ns) ||
        !writer.put_u64(risk_delta.reservation_quantity.units) ||
        !writer.put_u64(risk_delta.reservation_notional.units) ||
        !writer.put_u32(risk_delta.normal_tokens_after) ||
        !writer.put_u32(risk_delta.cancel_tokens_after) ||
        !writer.put_u64(risk_delta.normal_refill_time_after) ||
        !writer.put_u64(risk_delta.cancel_refill_time_after) ||
        !writer.put_u64(risk_delta.authentication_tag) ||
        !writer.put_u64(risk_metadata.risk_generation) ||
        !writer.put_u64(risk_metadata.configuration_generation) ||
        !writer.put_u64(risk_metadata.kernel_state_generation) ||
        !writer.put_u64(risk_metadata.last_committed_monotonic_time_ns) ||
        !write_scope_counters(writer, client_before) ||
        !write_scope_counters(writer, client_full) ||
        !write_scope_counters(writer, account_before) ||
        !write_scope_counters(writer, account_full) ||
        !writer.put_bool(static_cast<bool>(route_plan.remainder)) ||
        !writer.put_u64(risk_delta.command.parent_order_id.value()) ||
        !writer.put_u64(route_plan.remainder.units) ||
        !writer.put_u64(released_notional->units) ||
        !writer.put_u8(static_cast<std::uint8_t>(expected_remainder_status)) ||
        !writer.put_u64(full_state_generation) ||
        !writer.put_u64(final_state_generation) ||
        !write_scope_counters(writer, client_final) ||
        !write_scope_counters(writer, account_final) ||
        !append_record(
            batch, RiskDispatchRecordType::AccountClientRiskDelta, writer)) {
        return false;
    }

    writer.clear();
    if (!writer.put_u16(route_plan.candidate_pin_count)) {
        return false;
    }
    for (std::size_t index = 0U; index < route_plan.candidate_pin_count; ++index) {
        const dataplane::DataplaneCandidatePin& pin =
            route_plan.candidate_pins[index];
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
    for (std::size_t pin_index = 0U;
         pin_index < route_plan.candidate_pin_count;
         ++pin_index) {
        const dataplane::BookSlotView* const view = find_view(
            pinned_views,
            route_plan.candidate_pins[pin_index].market.venue);
        if (view == nullptr || !view->has_book) {
            continue;
        }
        const bool consume_asks = route_plan.parent.side == Side::Buy;
        const std::uint16_t available =
            consume_asks ? view->book.ask_count : view->book.bid_count;
        const std::uint16_t recorded =
            route_plan.parent.intent == RoutingIntent::PassiveGTC
            ? std::min<std::uint16_t>(available, 1U)
            : available;
        for (std::uint16_t level = 0U; level < recorded; ++level) {
            if (slice_count == kMaximumRecordedDepthSlices) {
                return false;
            }
            const md::Level& depth = consume_asks
                ? view->book.asks[level]
                : view->book.bids[level];
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
            batch, RiskDispatchRecordType::PinnedRouteInputs, writer)) {
        return false;
    }

    const dataplane::InstrumentRiskCounters instrument_before =
        route_kernel_->instrument_counters(route_plan.parent.instrument);
    const auto projected_gross = checked_add(
        instrument_before.gross_outstanding, route_plan.allocated);
    writer.clear();
    if (!instrument_before.found || !projected_gross ||
        instrument_before.open_parents ==
            std::numeric_limits<std::uint64_t>::max() ||
        route_plan.allocation_count >
            std::numeric_limits<std::uint64_t>::max() -
                instrument_before.open_children ||
        !writer.put_u64(route_plan.kernel_state_generation) ||
        !writer.put_u64(route_plan.candidate_table_generation) ||
        !writer.put_u64(route_plan.risk_generation) ||
        !writer.put_u64(route_plan.monotonic_decision_time_ns) ||
        !writer.put_u64(route_plan.next_child_order_id) ||
        !writer.put_u64(route_plan.next_dispatch_attempt_id) ||
        !writer.put_u64(instrument_before.gross_outstanding.units) ||
        !writer.put_u64(projected_gross->units) ||
        !writer.put_u64(instrument_before.open_parents) ||
        !writer.put_u64(instrument_before.open_parents + 1U) ||
        !writer.put_u64(instrument_before.open_children) ||
        !writer.put_u64(
            instrument_before.open_children + route_plan.allocation_count) ||
        !writer.put_u64(route_plan.allocated.units) ||
        !writer.put_u64(route_plan.remainder.units) ||
        !writer.put_u8(static_cast<std::uint8_t>(route_plan.remainder_reason)) ||
        !writer.put_u8(static_cast<std::uint8_t>(route_plan.reject_reason)) ||
        !writer.put_bool(route_plan.observed_truncated_depth) ||
        !writer.put_u16(route_plan.exclusion_count)) {
        return false;
    }
    for (std::size_t index = 0U; index < route_plan.exclusion_count; ++index) {
        if (!writer.put_u32(route_plan.exclusions[index].venue.value()) ||
            !writer.put_u8(static_cast<std::uint8_t>(
                route_plan.exclusions[index].reason))) {
            return false;
        }
    }
    if (!writer.put_u16(route_plan.allocation_count)) {
        return false;
    }
    for (std::size_t index = 0U; index < route_plan.allocation_count; ++index) {
        const ChildAllocation& allocation = route_plan.allocations[index];
        const dataplane::PlannedChild& child =
            route_plan.planned_children[index];
        const dataplane::VenueRiskCounters venue_before =
            route_kernel_->venue_counters(allocation.venue);
        const auto venue_after = checked_add(
            venue_before.outstanding, allocation.quantity);
        if (!venue_before.found || !venue_after ||
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
            !writer.put_u64(venue_after->units) ||
            !writer.put_u32(venue_before.reserved_normal_tx_credits) ||
            !writer.put_u32(venue_before.reserved_cancel_tx_credits) ||
            !writer.put_u32(venue_before.reserved_throttle_credits) ||
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
            batch, RiskDispatchRecordType::RouteDecision, writer)) {
        return false;
    }

    writer.clear();
    if (!writer.put_u16(route_plan.allocation_count)) {
        return false;
    }
    for (std::size_t index = 0U; index < route_plan.allocation_count; ++index) {
        const dataplane::PlannedChild& child =
            route_plan.planned_children[index];
        const EncodedFrame& frame = frames[index];
        if (!frame.valid() ||
            child.wire_identity.size > child.wire_identity.bytes.size() ||
            !writer.put_u32(frame.venue_id) ||
            !writer.put_u64(frame.child_id) ||
            !writer.put_u64(frame.dispatch_attempt_id) ||
            !writer.put_u64(frame.leadership_epoch) ||
            !writer.put_u64(frame.configuration_generation) ||
            !writer.put_u8(static_cast<std::uint8_t>(frame.dispatch_class)) ||
            !writer.put_u8(child.wire_identity.size) ||
            !writer.put_bytes({
                child.wire_identity.bytes.data(), child.wire_identity.size}) ||
            !writer.put_u16(frame.size) || !writer.put_bytes(frame.bytes())) {
            return false;
        }
    }
    if (!writer.put_u16(route_plan.allocation_count)) {
        return false;
    }
    for (std::size_t index{}; index < route_plan.allocation_count; ++index) {
        const dataplane::PlannedChild& child =
            route_plan.planned_children[index];
        const EncodedFrame& frame = frames[index];
        if (child.venue_session_id == 0U ||
            child.venue_session_epoch == 0U ||
            child.ownership_generation == 0U ||
            child.wire_identity.size == 0U ||
            child.wire_identity.size > child.wire_identity.bytes.size() ||
            frame.venue_id != route_plan.allocations[index].venue.value() ||
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
    return append_record(
               batch, RiskDispatchRecordType::ExactChildFrames, writer) &&
        batch.valid();
}

}  // namespace sorx::fastpath
