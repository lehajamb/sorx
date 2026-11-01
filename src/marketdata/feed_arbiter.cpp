#include "sorx/marketdata/feed_arbiter.hpp"

#include <limits>
#include <stdexcept>

namespace sorx::marketdata {
namespace {

[[nodiscard]] constexpr bool valid_leg(const FeedLeg leg) noexcept {
    return leg == FeedLeg::A || leg == FeedLeg::B;
}

[[nodiscard]] constexpr FeedLeg other_leg(const FeedLeg leg) noexcept {
    return leg == FeedLeg::A ? FeedLeg::B : FeedLeg::A;
}

[[nodiscard]] bool tickets_equal(
    const ArbitrationTicket& left,
    const ArbitrationTicket& right) noexcept {
    return left.nonce == right.nonce &&
           left.feed_epoch == right.feed_epoch &&
           left.first_sequence == right.first_sequence &&
           left.next_sequence == right.next_sequence &&
           left.local_receive_time_ns == right.local_receive_time_ns &&
           left.leg == right.leg &&
           left.contains_quote == right.contains_quote;
}

}  // namespace

FeedArbiter::FeedArbiter(const FeedArbiterConfig config) : config_(config) {
    if (config_.heartbeat_timeout_ns == 0U ||
        config_.maximum_quote_age_ns == 0U) {
        throw std::invalid_argument("feed health timeouts must be nonzero");
    }
}

bool FeedArbiter::install_snapshot(
    const FeedSnapshotCut& snapshot) noexcept {
    if (!valid_leg(snapshot.source) || snapshot.feed_epoch == 0U ||
        snapshot.next_sequence == 0U ||
        snapshot.local_receive_time_ns == 0U ||
        snapshot.last_quote_receive_time_ns == 0U ||
        snapshot.last_quote_receive_time_ns > snapshot.local_receive_time_ns ||
        snapshot.feed_epoch < feed_epoch_) {
        return false;
    }
    if (snapshot.feed_epoch != feed_epoch_) {
        leg_a_ = {};
        leg_b_ = {};
    }
    feed_epoch_ = snapshot.feed_epoch;
    next_sequence_ = snapshot.next_sequence;
    selected_leg_ = snapshot.source;
    state_ = FeedArbiterState::Live;
    pending_ticket_ = {};
    leg_a_.gap_first_sequence = 0U;
    leg_b_.gap_first_sequence = 0U;
    LegState& source = leg(snapshot.source);
    source.seen = true;
    source.last_observation_time_ns = snapshot.local_receive_time_ns;
    last_quote_receive_time_ns_ = snapshot.last_quote_receive_time_ns;
    last_polled_time_ns_ = snapshot.local_receive_time_ns;
    return true;
}

ArbitrationResult FeedArbiter::inspect(
    const FeedPacketObservation& packet) noexcept {
    if (state_ == FeedArbiterState::Failed) {
        return {ArbitrationAction::Failed, {}, FeedHealth::Failed};
    }
    if (!valid_leg(packet.leg) || packet.feed_epoch == 0U ||
        packet.first_sequence == 0U ||
        packet.local_receive_time_ns == 0U) {
        enter_recovery();
        return {ArbitrationAction::Malformed, {}, FeedHealth::Recovering};
    }

    LegState& observed_leg = leg(packet.leg);
    if (observed_leg.seen &&
        packet.local_receive_time_ns <
            observed_leg.last_observation_time_ns) {
        enter_recovery();
        return {ArbitrationAction::Malformed, {},
                FeedHealth::ClockRegression};
    }
    observed_leg.seen = true;
    observed_leg.last_observation_time_ns = packet.local_receive_time_ns;

    if (feed_epoch_ == 0U || packet.feed_epoch > feed_epoch_) {
        feed_epoch_ = packet.feed_epoch;
        next_sequence_ = 0U;
        selected_leg_ = FeedLeg::None;
        leg_a_ = {};
        leg_b_ = {};
        enter_recovery();
        return {ArbitrationAction::EpochRollover, {},
                FeedHealth::Recovering};
    }
    if (packet.feed_epoch < feed_epoch_) {
        return {ArbitrationAction::StaleEpochIgnored, {},
                health(packet.local_receive_time_ns)};
    }
    if (state_ != FeedArbiterState::Live) {
        return {ArbitrationAction::AwaitingSnapshot, {},
                health(packet.local_receive_time_ns)};
    }
    if (packet.message_count == 0U) {
        if (packet.first_sequence != next_sequence_) {
            enter_recovery();
            return {ArbitrationAction::Malformed, {},
                    FeedHealth::Recovering};
        }
        return {ArbitrationAction::HeartbeatAccepted, {},
                health(packet.local_receive_time_ns)};
    }
    if (pending_ticket_) {
        return {ArbitrationAction::PendingCommit, {},
                health(packet.local_receive_time_ns)};
    }
    if (packet.message_count >
        std::numeric_limits<std::uint64_t>::max() - packet.first_sequence) {
        enter_recovery();
        return {ArbitrationAction::Malformed, {},
                FeedHealth::Recovering};
    }
    const std::uint64_t packet_next =
        packet.first_sequence + packet.message_count;
    if (packet.first_sequence < next_sequence_) {
        if (packet_next <= next_sequence_) {
            return {ArbitrationAction::DuplicateIgnored, {},
                    health(packet.local_receive_time_ns)};
        }
        enter_recovery();
        return {ArbitrationAction::RecoveryRequired, {},
                FeedHealth::Recovering};
    }
    if (packet.first_sequence > next_sequence_) {
        observed_leg.gap_first_sequence = packet.first_sequence;
        const FeedLeg alternate = other_leg(packet.leg);
        const LegState& alternate_state = leg(alternate);
        if (leg_healthy(alternate, packet.local_receive_time_ns) &&
            (alternate_state.gap_first_sequence == 0U ||
             alternate_state.gap_first_sequence <= next_sequence_)) {
            return {ArbitrationAction::AwaitPeer, {}, FeedHealth::GapPending};
        }
        enter_recovery();
        return {ArbitrationAction::RecoveryRequired, {},
                FeedHealth::Recovering};
    }

    const bool failover =
        selected_leg_ != FeedLeg::None && selected_leg_ != packet.leg &&
        (!leg_healthy(selected_leg_, packet.local_receive_time_ns) ||
         leg(selected_leg_).gap_first_sequence > next_sequence_);
    leg_a_.gap_first_sequence = 0U;
    leg_b_.gap_first_sequence = 0U;
    if (next_nonce_ == 0U) {
        next_nonce_ = 1U;
    }
    pending_ticket_ = ArbitrationTicket{next_nonce_++,
                                        packet.feed_epoch,
                                        packet.first_sequence,
                                        packet_next,
                                        packet.local_receive_time_ns,
                                        packet.leg,
                                        packet.contains_quote};
    return {failover ? ArbitrationAction::FailoverApply
                     : ArbitrationAction::Apply,
            pending_ticket_, health(packet.local_receive_time_ns)};
}

CommitStatus FeedArbiter::commit(
    const ArbitrationTicket& ticket,
    const bool application_succeeded) noexcept {
    if (!pending_ticket_ || !tickets_equal(ticket, pending_ticket_) ||
        state_ != FeedArbiterState::Live ||
        ticket.feed_epoch != feed_epoch_ ||
        ticket.first_sequence != next_sequence_) {
        return CommitStatus::InvalidTicket;
    }
    if (!application_succeeded) {
        enter_recovery();
        return CommitStatus::ApplicationRejectedRecoveryRequired;
    }
    next_sequence_ = ticket.next_sequence;
    selected_leg_ = ticket.leg;
    if (ticket.contains_quote) {
        last_quote_receive_time_ns_ = ticket.local_receive_time_ns;
    }
    pending_ticket_ = {};
    return CommitStatus::Committed;
}

FeedHealth FeedArbiter::poll(const std::uint64_t now_ns) noexcept {
    if (last_polled_time_ns_ != 0U && now_ns < last_polled_time_ns_) {
        enter_recovery();
        return FeedHealth::ClockRegression;
    }
    last_polled_time_ns_ = now_ns;
    if (state_ == FeedArbiterState::Live &&
        (leg_a_.gap_first_sequence != 0U ||
         leg_b_.gap_first_sequence != 0U)) {
        const bool both_gapped = leg_a_.gap_first_sequence > next_sequence_ &&
                                 leg_b_.gap_first_sequence > next_sequence_;
        const bool neither_healthy = !leg_healthy(FeedLeg::A, now_ns) &&
                                     !leg_healthy(FeedLeg::B, now_ns);
        const bool gap_timed_out =
            last_quote_receive_time_ns_ == 0U ||
            (now_ns >= last_quote_receive_time_ns_ &&
             now_ns - last_quote_receive_time_ns_ >
                 config_.maximum_quote_age_ns);
        if (both_gapped || neither_healthy || gap_timed_out) {
            enter_recovery();
        }
    }
    return health(now_ns);
}

void FeedArbiter::force_recovery() noexcept { enter_recovery(); }

void FeedArbiter::fail() noexcept {
    state_ = FeedArbiterState::Failed;
    pending_ticket_ = {};
}

FeedHealth FeedArbiter::health(const std::uint64_t now_ns) const noexcept {
    switch (state_) {
        case FeedArbiterState::AwaitingSnapshot:
            return FeedHealth::AwaitingSnapshot;
        case FeedArbiterState::Recovering:
            return FeedHealth::Recovering;
        case FeedArbiterState::Failed:
            return FeedHealth::Failed;
        case FeedArbiterState::Live:
            break;
    }
    if ((leg_a_.seen && now_ns < leg_a_.last_observation_time_ns) ||
        (leg_b_.seen && now_ns < leg_b_.last_observation_time_ns) ||
        (last_quote_receive_time_ns_ != 0U &&
         now_ns < last_quote_receive_time_ns_)) {
        return FeedHealth::ClockRegression;
    }
    if (leg_a_.gap_first_sequence != 0U ||
        leg_b_.gap_first_sequence != 0U) {
        return FeedHealth::GapPending;
    }
    if (!leg_healthy(FeedLeg::A, now_ns) &&
        !leg_healthy(FeedLeg::B, now_ns)) {
        return FeedHealth::HeartbeatsStalled;
    }
    if (last_quote_receive_time_ns_ == 0U ||
        now_ns - last_quote_receive_time_ns_ >
            config_.maximum_quote_age_ns) {
        return FeedHealth::QuoteStale;
    }
    return FeedHealth::Healthy;
}

bool FeedArbiter::may_publish(const std::uint64_t now_ns) const noexcept {
    return !pending_ticket_ && health(now_ns) == FeedHealth::Healthy;
}

FeedArbiterMetadata FeedArbiter::metadata() const noexcept {
    return FeedArbiterMetadata{state_,
                               selected_leg_,
                               feed_epoch_,
                               recovery_generation_,
                               next_sequence_,
                               last_quote_receive_time_ns_,
                               static_cast<bool>(pending_ticket_)};
}

bool FeedArbiter::leg_healthy(
    const FeedLeg value,
    const std::uint64_t now_ns) const noexcept {
    if (!valid_leg(value)) {
        return false;
    }
    const LegState& state = leg(value);
    return state.seen && now_ns >= state.last_observation_time_ns &&
           now_ns - state.last_observation_time_ns <=
               config_.heartbeat_timeout_ns;
}

FeedArbiter::LegState& FeedArbiter::leg(const FeedLeg value) noexcept {
    return value == FeedLeg::A ? leg_a_ : leg_b_;
}

const FeedArbiter::LegState& FeedArbiter::leg(
    const FeedLeg value) const noexcept {
    return value == FeedLeg::A ? leg_a_ : leg_b_;
}

void FeedArbiter::enter_recovery() noexcept {
    if (state_ != FeedArbiterState::Recovering) {
        if (recovery_generation_ !=
            std::numeric_limits<std::uint64_t>::max()) {
            ++recovery_generation_;
        }
    }
    state_ = FeedArbiterState::Recovering;
    pending_ticket_ = {};
}

}  // namespace sorx::marketdata
