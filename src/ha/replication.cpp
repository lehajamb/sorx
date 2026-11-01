#include "sorx/ha/replication.hpp"

namespace sorx::ha {

ReplicationGate::ReplicationGate(
    const std::uint64_t local_node_id,
    const ReplicationPolicy policy) noexcept
    : local_node_id_(local_node_id), policy_(policy) {}

bool ReplicationGate::install_authenticated_lease(
    const FenceLease& lease) noexcept {
    if (local_node_id_ == 0U || !lease.well_formed() ||
        lease.leader_node_id != local_node_id_) {
        return false;
    }
    const bool active = lease_.well_formed();
    const bool same_authenticated_tuple =
        lease.leadership_epoch == maximum_authenticated_epoch_ &&
        lease.witness_term == maximum_authenticated_witness_term_;
    if (same_authenticated_tuple) {
        // An exact-tuple replay is only a renewal while that lease remains
        // active, and it must strictly extend the authenticated expiry.
        if (!active || lease.expires_at_monotonic_ns <=
                           lease_.expires_at_monotonic_ns) {
            return false;
        }
    } else {
        // Both components are monotonic; at least one must advance. This is
        // stricter than lexicographic comparison and rejects a new epoch that
        // attempts to roll the witness term backwards.
        if (lease.leadership_epoch < maximum_authenticated_epoch_ ||
            lease.witness_term < maximum_authenticated_witness_term_ ||
            (lease.leadership_epoch == maximum_authenticated_epoch_ &&
             lease.witness_term == maximum_authenticated_witness_term_) ||
            (active && lease.expires_at_monotonic_ns <=
                           lease_.expires_at_monotonic_ns)) {
            return false;
        }
        local_durable_sequence_ = 0U;
        remote_durable_sequence_ = 0U;
        maximum_authenticated_epoch_ = lease.leadership_epoch;
        maximum_authenticated_witness_term_ = lease.witness_term;
    }
    lease_ = lease;
    return true;
}

void ReplicationGate::revoke() noexcept {
    lease_ = {};
    local_durable_sequence_ = 0U;
    remote_durable_sequence_ = 0U;
}

bool ReplicationGate::note_local_durable(
    const std::uint64_t record_sequence,
    const std::uint64_t leadership_epoch) noexcept {
    if (record_sequence == 0U || !lease_.well_formed() ||
        leadership_epoch != lease_.leadership_epoch ||
        record_sequence < local_durable_sequence_) {
        return false;
    }
    local_durable_sequence_ = record_sequence;
    return true;
}

bool ReplicationGate::note_remote_durable(
    const std::uint64_t record_sequence,
    const std::uint64_t leadership_epoch) noexcept {
    if (record_sequence == 0U || !lease_.well_formed() ||
        leadership_epoch != lease_.leadership_epoch ||
        record_sequence < remote_durable_sequence_) {
        return false;
    }
    remote_durable_sequence_ = record_sequence;
    return true;
}

DispatchFenceError ReplicationGate::authorize(
    const std::uint64_t record_sequence,
    const std::uint64_t leadership_epoch,
    const std::uint64_t now_monotonic_ns) const noexcept {
    if (!lease_.well_formed()) {
        return DispatchFenceError::NoLease;
    }
    if (lease_.leader_node_id != local_node_id_) {
        return DispatchFenceError::WrongLeader;
    }
    if (now_monotonic_ns == 0U || now_monotonic_ns >= lease_.expires_at_monotonic_ns) {
        return DispatchFenceError::LeaseExpired;
    }
    if (leadership_epoch != lease_.leadership_epoch) {
        return DispatchFenceError::StaleLeadershipEpoch;
    }
    if (record_sequence == 0U || record_sequence > local_durable_sequence_) {
        return DispatchFenceError::LocalRecordNotDurable;
    }
    if (policy_ == ReplicationPolicy::DualDurable &&
        record_sequence > remote_durable_sequence_) {
        return DispatchFenceError::RemoteRecordNotDurable;
    }
    return DispatchFenceError::None;
}

ReplicationPolicy ReplicationGate::policy() const noexcept {
    return policy_;
}

FenceLease ReplicationGate::lease() const noexcept {
    return lease_;
}

std::uint64_t ReplicationGate::local_durable_sequence() const noexcept {
    return local_durable_sequence_;
}

std::uint64_t ReplicationGate::remote_durable_sequence() const noexcept {
    return remote_durable_sequence_;
}

TakeoverGate::TakeoverGate(ReplicationGate& replication) noexcept
    : replication_(&replication) {}

bool TakeoverGate::begin_recovery(
    const FenceLease& authenticated_lease) noexcept {
    if ((state_ != TakeoverState::Standby && state_ != TakeoverState::Revoked) ||
        replication_ == nullptr ||
        !replication_->install_authenticated_lease(authenticated_lease)) {
        return false;
    }
    state_ = TakeoverState::RecoveryOnly;
    return true;
}

bool TakeoverGate::begin_reconciliation() noexcept {
    if (state_ != TakeoverState::RecoveryOnly) {
        return false;
    }
    state_ = TakeoverState::Reconciling;
    return true;
}

bool TakeoverGate::complete_reconciliation(
    const bool accounting_consistent,
    const bool venue_sessions_recovered) noexcept {
    if (state_ != TakeoverState::Reconciling || !accounting_consistent ||
        !venue_sessions_recovered) {
        return false;
    }
    state_ = TakeoverState::Ready;
    return true;
}

void TakeoverGate::revoke() noexcept {
    if (replication_ != nullptr) {
        replication_->revoke();
    }
    state_ = TakeoverState::Revoked;
}

TakeoverState TakeoverGate::state() const noexcept {
    return state_;
}

bool TakeoverGate::may_route_new() const noexcept {
    return state_ == TakeoverState::Ready;
}

}  // namespace sorx::ha
