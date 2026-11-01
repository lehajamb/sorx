#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/fastpath/contracts.hpp"

#include <array>
#include <cassert>
#include <span>

namespace {

class CountingTransport final : public sorx::fastpath::IFrameTransport {
public:
    [[nodiscard]] sorx::fastpath::TransportWriteResult try_transmit(
        const std::span<const std::byte> frame) noexcept override {
        ++calls;
        last_size = frame.size();
        return result;
    }

    sorx::fastpath::TransportWriteResult result{
        sorx::fastpath::TransportWriteResult::Sent};
    std::size_t calls{};
    std::size_t last_size{};
};

}  // namespace

int main() {
    using namespace sorx;

    recorder::InMemoryMirroredTier0 flight_recorder({8U, 1U, 1U, false});
    fastpath::EncodedFrame frame;
    frame.venue_id = 9U;
    frame.child_id = 41U;
    frame.dispatch_attempt_id = 101U;
    frame.leadership_epoch = 7U;
    frame.configuration_generation = 13U;
    frame.venue_session_id = 55U;
    frame.venue_session_epoch = 66U;
    frame.ownership_generation = 1U;
    frame.size = 2U;
    frame.storage[0] = std::byte{0x01};
    frame.storage[1] = std::byte{0x02};

    fastpath::EncodedFrame later = frame;
    later.child_id = 42U;
    later.dispatch_attempt_id = 102U;

    recorder::DurableBatch batch(recorder::DurabilityLane::Normal, 91U);
    batch.set_leadership_epoch(7U);
    batch.set_configuration_generation(13U);
    assert(batch.set_dispatch_authorization({41U, 101U, 2U}));
    assert(batch.try_add_external_effect_authorization(
        frame.venue_id, frame.child_id, frame.dispatch_attempt_id,
        frame.venue_session_id, frame.venue_session_epoch,
        frame.ownership_generation,
        static_cast<std::uint8_t>(frame.dispatch_class), frame.bytes()));
    assert(batch.try_add_external_effect_authorization(
        later.venue_id, later.child_id, later.dispatch_attempt_id,
        later.venue_session_id, later.venue_session_epoch,
        later.ownership_generation,
        static_cast<std::uint8_t>(later.dispatch_class), later.bytes()));
    const std::array payload{std::byte{0x42}, std::byte{0x43}};
    assert(batch.try_append_record(10U, 1U, payload));

    recorder::CompletionSlot completion;
    assert(flight_recorder.try_append(batch, completion) ==
           recorder::AppendStatus::Committed);
    const auto token = completion.token();
    assert(token.has_value());
    const auto first_permit = token->effect_permit(0U);
    const auto second_permit = token->effect_permit(1U);
    assert(first_permit.has_value());
    assert(second_permit.has_value());

    CountingTransport transport;
    fastpath::TokenGatedOrderEntrySession session(
        {9U, recorder::DurabilityDomain::VolatileSoftwareReference, 55U, 66U},
        flight_recorder,
        transport);

    auto wrong_identity = frame;
    wrong_identity.child_id = 42U;
    assert(session.try_send(wrong_identity, *first_permit) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    assert(transport.calls == 0U);

    auto wrong_epoch = frame;
    wrong_epoch.leadership_epoch = 8U;
    assert(session.try_send(wrong_epoch, *first_permit) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    assert(transport.calls == 0U);

    auto changed_bytes = frame;
    changed_bytes.storage[0] ^= std::byte{0x01U};
    assert(session.try_send(changed_bytes, *first_permit) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    auto changed_ownership = frame;
    ++changed_ownership.ownership_generation;
    assert(session.try_send(changed_ownership, *first_permit) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    auto changed_session = frame;
    ++changed_session.venue_session_epoch;
    assert(session.try_send(changed_session, *first_permit) ==
           fastpath::SendResult::WrongVenue);
    assert(session.try_send(frame, *second_permit) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    assert(transport.calls == 0U);

    assert(session.try_send(frame, *first_permit) == fastpath::SendResult::Sent);
    assert(transport.calls == 1U);
    assert(transport.last_size == 2U);
    assert(session.last_transmitted_attempt_id() == 101U);
    assert(session.highest_transmitted_attempt_id() == 101U);

    assert(session.try_send(frame, *first_permit) ==
           fastpath::SendResult::DuplicateOrExpiredAttempt);
    assert(transport.calls == 1U);

    auto tampered = *second_permit;
    tampered.batch_hash[0] ^= std::byte{0x01U};
    assert(token->authorizes_dispatch(42U, 102U, 7U, 13U));
    assert(session.try_send(later, tampered) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    assert(transport.calls == 1U);

    CountingTransport strict_transport;
    fastpath::TokenGatedOrderEntrySession production_gate(
        {9U, recorder::DurabilityDomain::HardwarePersistent, 55U, 66U},
        flight_recorder,
        strict_transport);
    assert(production_gate.try_send(frame, *first_permit) ==
           fastpath::SendResult::MissingOrInvalidDurableToken);
    assert(strict_transport.calls == 0U);

    CountingTransport unconfigured_transport;
    fastpath::TokenGatedOrderEntrySession unconfigured_gate(
        {9U, recorder::DurabilityDomain::VolatileSoftwareReference},
        flight_recorder,
        unconfigured_transport);
    assert(unconfigured_gate.try_send(later, *second_permit) ==
           fastpath::SendResult::WrongVenue);
    assert(unconfigured_transport.calls == 0U);

    // Priority scheduling may legitimately send a newer cancellation attempt
    // before an older queued new order. Both remain unique and authorized.
    recorder::DurableBatch reordered_batch(
        recorder::DurabilityLane::Cancel, 92U);
    reordered_batch.set_leadership_epoch(7U);
    reordered_batch.set_configuration_generation(13U);
    assert(reordered_batch.set_dispatch_authorization({100U, 200U, 2U}));
    fastpath::EncodedFrame older = frame;
    older.child_id = 100U;
    older.dispatch_attempt_id = 200U;
    fastpath::EncodedFrame newer = frame;
    newer.child_id = 101U;
    newer.dispatch_attempt_id = 201U;
    assert(reordered_batch.try_add_external_effect_authorization(
        older.venue_id, older.child_id, older.dispatch_attempt_id,
        older.venue_session_id, older.venue_session_epoch,
        older.ownership_generation,
        static_cast<std::uint8_t>(older.dispatch_class), older.bytes()));
    assert(reordered_batch.try_add_external_effect_authorization(
        newer.venue_id, newer.child_id, newer.dispatch_attempt_id,
        newer.venue_session_id, newer.venue_session_epoch,
        newer.ownership_generation,
        static_cast<std::uint8_t>(newer.dispatch_class), newer.bytes()));
    assert(reordered_batch.try_append_record(11U, 1U, payload));
    recorder::CompletionSlot reordered_completion;
    assert(flight_recorder.try_append(
               reordered_batch, reordered_completion) ==
           recorder::AppendStatus::Committed);
    const auto reordered_token = reordered_completion.token();
    assert(reordered_token.has_value());
    const auto older_permit = reordered_token->effect_permit(0U);
    const auto newer_permit = reordered_token->effect_permit(1U);
    assert(older_permit.has_value());
    assert(newer_permit.has_value());
    assert(session.try_send(newer, *newer_permit) ==
           fastpath::SendResult::Sent);
    assert(session.try_send(older, *older_permit) ==
           fastpath::SendResult::Sent);
    assert(session.last_transmitted_attempt_id() == 200U);
    assert(session.highest_transmitted_attempt_id() == 201U);
    assert(session.try_send(newer, *newer_permit) ==
           fastpath::SendResult::DuplicateOrExpiredAttempt);

    return 0;
}
