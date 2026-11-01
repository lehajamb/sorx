#include "sorx/fastpath/contracts.hpp"

namespace sorx::fastpath {

TokenGatedOrderEntrySession::TokenGatedOrderEntrySession(
    const TokenGatePolicy policy,
    const recorder::IFlightRecorder& flight_recorder,
    IFrameTransport& transport) noexcept
    : policy_(policy), recorder_(&flight_recorder), transport_(&transport) {}

SendResult TokenGatedOrderEntrySession::try_send(
    const EncodedFrame& frame,
    const recorder::DurableEffectPermit& permit) noexcept {
    if (!frame.valid()) {
        return SendResult::InvalidFrame;
    }
    if (policy_.venue_id == 0U || policy_.venue_session_id == 0U ||
        policy_.venue_session_epoch == 0U ||
        frame.venue_id != policy_.venue_id ||
        frame.venue_session_id != policy_.venue_session_id ||
        frame.venue_session_epoch != policy_.venue_session_epoch) {
        return SendResult::WrongVenue;
    }
    if (attempt_replayed_or_too_old(frame.dispatch_attempt_id)) {
        return SendResult::DuplicateOrExpiredAttempt;
    }
    if (recorder_ == nullptr || transport_ == nullptr ||
        permit.leadership_epoch != frame.leadership_epoch ||
        permit.configuration_generation != frame.configuration_generation ||
        !permit.authorizes_external_effect(
            frame.venue_id,
            frame.child_id,
            frame.dispatch_attempt_id,
            frame.venue_session_id,
            frame.venue_session_epoch,
            frame.ownership_generation,
            static_cast<std::uint8_t>(frame.dispatch_class),
            frame.bytes()) ||
        !recorder_->authorizes_external_effect(
            permit,
            policy_.required_domain)) {
        return SendResult::MissingOrInvalidDurableToken;
    }

    switch (transport_->try_transmit(frame.bytes())) {
        case TransportWriteResult::Sent:
            mark_attempt_transmitted(frame.dispatch_attempt_id);
            last_transmitted_attempt_id_ = frame.dispatch_attempt_id;
            return SendResult::Sent;
        case TransportWriteResult::WouldBlock:
            return SendResult::WouldBlock;
        case TransportWriteResult::Closed:
            return SendResult::TransportClosed;
        case TransportWriteResult::Failed:
            return SendResult::TransportFailure;
    }
    return SendResult::TransportFailure;
}

std::uint64_t
TokenGatedOrderEntrySession::last_transmitted_attempt_id() const noexcept {
    return last_transmitted_attempt_id_;
}

std::uint64_t
TokenGatedOrderEntrySession::highest_transmitted_attempt_id() const noexcept {
    return highest_transmitted_attempt_id_;
}

bool TokenGatedOrderEntrySession::attempt_replayed_or_too_old(
    const std::uint64_t attempt_id) const noexcept {
    static_assert(
        (kAttemptReplayWindow & (kAttemptReplayWindow - 1U)) == 0U);
    if (attempt_id == 0U) {
        return true;
    }
    if (attempt_id <= highest_transmitted_attempt_id_ &&
        highest_transmitted_attempt_id_ - attempt_id >=
            kAttemptReplayWindow) {
        return true;
    }
    const std::size_t slot = static_cast<std::size_t>(attempt_id) &
        (kAttemptReplayWindow - 1U);
    return transmitted_attempts_[slot] == attempt_id;
}

void TokenGatedOrderEntrySession::mark_attempt_transmitted(
    const std::uint64_t attempt_id) noexcept {
    const std::size_t slot = static_cast<std::size_t>(attempt_id) &
        (kAttemptReplayWindow - 1U);
    transmitted_attempts_[slot] = attempt_id;
    if (attempt_id > highest_transmitted_attempt_id_) {
        highest_transmitted_attempt_id_ = attempt_id;
    }
}

}  // namespace sorx::fastpath
