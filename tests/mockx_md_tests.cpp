#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/adapters/mockx_md.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using sorx::ChannelId;
using sorx::InstrumentId;
using sorx::MarketDataState;
using sorx::Price;
using sorx::ProfileId;
using sorx::Quantity;
using sorx::VenueId;
using sorx::adapters::MockXEnqueueResult;
using sorx::adapters::MockXInstrumentBinding;
using sorx::adapters::MockXMdAdapter;
using sorx::adapters::MockXMdConfig;
using sorx::adapters::mockx_session_key;
using sorx::md::EngineUpdate;
using sorx::md::FeedEngineConfig;
using sorx::md::FeedError;

void set_be(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::size_t width,
    std::uint64_t value) {
    assert(width != 0 && width <= 8 && offset + width <= bytes.size());
    for (auto index = offset + width; index-- > offset;) {
        bytes[index] = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
    assert(value == 0);
}

void set_ascii(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::size_t width,
    const std::string_view value,
    const bool left_pad = false) {
    assert(value.size() <= width && offset + width <= bytes.size());
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), width, std::byte{' '});
    const auto destination = left_pad ? offset + width - value.size() : offset;
    for (auto index = std::size_t{}; index < value.size(); ++index) {
        bytes[destination + index] = static_cast<std::byte>(
            static_cast<unsigned char>(value[index]));
    }
}

[[nodiscard]] std::vector<std::byte> itch_message(
    const char type,
    const std::size_t size,
    const std::uint16_t locate,
    const std::uint64_t timestamp) {
    std::vector<std::byte> result(size);
    result[0] = static_cast<std::byte>(static_cast<unsigned char>(type));
    if (size >= 11) {
        set_be(result, 1, 2, locate);
        set_be(result, 5, 6, timestamp);
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> directory(
    const std::uint64_t timestamp = 1) {
    auto result = itch_message('R', 39, 17, timestamp);
    set_ascii(result, 11, 8, "ALPHA");
    return result;
}

[[nodiscard]] std::vector<std::byte> trading_action(
    const std::uint64_t timestamp = 2) {
    auto result = itch_message('H', 25, 17, timestamp);
    set_ascii(result, 11, 8, "ALPHA");
    result[19] = std::byte{'T'};
    return result;
}

[[nodiscard]] std::vector<std::byte> add_order(
    const std::uint64_t order_reference,
    const char side,
    const std::uint32_t quantity,
    const std::uint32_t price,
    const std::uint64_t timestamp) {
    auto result = itch_message('A', 36, 17, timestamp);
    set_be(result, 11, 8, order_reference);
    result[19] = static_cast<std::byte>(static_cast<unsigned char>(side));
    set_be(result, 20, 4, quantity);
    set_ascii(result, 24, 8, "ALPHA");
    set_be(result, 32, 4, price);
    return result;
}

[[nodiscard]] std::vector<std::byte> execution(
    const std::uint64_t order_reference,
    const std::uint32_t quantity,
    const std::uint64_t timestamp) {
    auto result = itch_message('E', 31, 17, timestamp);
    set_be(result, 11, 8, order_reference);
    set_be(result, 19, 4, quantity);
    set_be(result, 23, 8, 77);
    return result;
}

[[nodiscard]] std::vector<std::byte> system_event(
    const char code,
    const std::uint64_t timestamp) {
    auto result = itch_message('S', 12, 0, timestamp);
    result[11] = static_cast<std::byte>(static_cast<unsigned char>(code));
    return result;
}

[[nodiscard]] std::vector<std::byte> end_snapshot(const std::uint64_t cut) {
    std::vector<std::byte> result(21, std::byte{' '});
    result[0] = std::byte{'G'};
    set_ascii(result, 1, 20, std::to_string(cut), true);
    return result;
}

[[nodiscard]] std::vector<std::byte> mold_packet(
    const std::uint64_t first_sequence,
    const std::vector<std::vector<std::byte>>& messages,
    const std::uint16_t control_count = 0) {
    auto size = std::size_t{20};
    for (const auto& message : messages) {
        size += 2 + message.size();
    }
    std::vector<std::byte> result(size);
    set_ascii(result, 0, 10, "MX00000001");
    set_be(result, 10, 8, first_sequence);
    const auto count = messages.empty()
        ? control_count
        : static_cast<std::uint16_t>(messages.size());
    set_be(result, 18, 2, count);
    auto offset = std::size_t{20};
    for (const auto& message : messages) {
        set_be(result, offset, 2, message.size());
        offset += 2;
        std::memcpy(result.data() + offset, message.data(), message.size());
        offset += message.size();
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> soup_packet(
    const char type,
    const std::span<const std::byte> payload = {}) {
    assert(payload.size() < std::numeric_limits<std::uint16_t>::max());
    std::vector<std::byte> result(3 + payload.size());
    set_be(result, 0, 2, payload.size() + 1);
    result[2] = static_cast<std::byte>(static_cast<unsigned char>(type));
    if (!payload.empty()) {
        std::memcpy(result.data() + 3, payload.data(), payload.size());
    }
    return result;
}

void append(
    std::vector<std::byte>& destination,
    const std::vector<std::byte>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

[[nodiscard]] std::vector<std::byte> accepted_login() {
    std::vector<std::byte> payload(30, std::byte{' '});
    set_ascii(payload, 0, 10, "GS00000001");
    set_ascii(payload, 10, 20, "1", true);
    return soup_packet('A', payload);
}

[[nodiscard]] std::vector<std::byte> snapshot_stream(const std::uint64_t cut) {
    std::vector<std::byte> result;
    append(result, accepted_login());
    const auto r = directory();
    const auto h = trading_action();
    const auto bid = add_order(100, 'B', 5, 1'000'000, 3);
    const auto ask = add_order(102, 'S', 9, 1'050'000, 4);
    const auto marker = end_snapshot(cut);
    append(result, soup_packet('S', r));
    append(result, soup_packet('S', h));
    append(result, soup_packet('S', bid));
    append(result, soup_packet('S', ask));
    append(result, soup_packet('S', marker));
    append(result, soup_packet('Z'));
    return result;
}

[[nodiscard]] MockXMdConfig config(
    const std::size_t queue_capacity = 16,
    const std::size_t max_raw_bytes = 2'048) {
    MockXMdConfig result;
    result.feed = FeedEngineConfig{
        VenueId{7},
        ChannelId{9},
        ProfileId{11},
        8,
        sorx::md::RecoveryStrategy::FreshSnapshot,
        16,
        256,
    };
    result.instruments = {
        MockXInstrumentBinding{17, "ALPHA", InstrumentId{101}},
    };
    result.raw_queue_capacity = queue_capacity;
    result.max_raw_input_bytes = max_raw_bytes;
    result.max_messages_per_mold_packet = 32;
    result.max_glimpse_messages = 64;
    result.max_soup_buffer_bytes = 4'096;
    return result;
}

[[nodiscard]] const sorx::md::BookView& only_view(const EngineUpdate& update) {
    assert(update.publications.size() == 1);
    assert(update.publications.front().valid());
    return update.publications.front();
}

void fresh_glimpse_stitches_buffered_live_bytes() {
    MockXMdAdapter adapter{config()};
    const auto live_session = mockx_session_key("MX00000001");
    adapter.begin_glimpse(live_session, 11);

    const auto live = mold_packet(10, {execution(100, 2, 5)});
    assert(adapter.try_enqueue_mold_datagram(live, 10, 11) ==
        MockXEnqueueResult::Accepted);
    const auto buffered = adapter.drain();
    assert(buffered.size() == 1);
    assert(buffered.front().state == MarketDataState::Snapshotting);
    assert(adapter.feed_engine().buffered_packet_count() == 1);

    const auto stream = snapshot_stream(10);
    const auto split = stream.size() / 2;
    assert(adapter.try_enqueue_soup_bytes(
               std::span<const std::byte>{stream}.first(split),
               100,
               101) == MockXEnqueueResult::Accepted);
    assert(adapter.drain().empty());
    assert(adapter.glimpse_in_progress());
    assert(adapter.try_enqueue_soup_bytes(
               std::span<const std::byte>{stream}.subspan(split),
               102,
               103) == MockXEnqueueResult::Accepted);
    const auto completed = adapter.drain();
    assert(completed.size() == 2);
    assert(completed[0].state == MarketDataState::Stitching);
    assert(completed[1].healthy());
    assert(adapter.feed_engine().state() == MarketDataState::Live);
    assert(adapter.feed_engine().next_sequence() == 11);
    assert(adapter.feed_engine().active_session_key() == live_session);
    assert(!adapter.glimpse_in_progress());

    const auto& book = only_view(completed.back());
    assert(book.instrument == InstrumentId{101});
    assert(book.bids[0].price == Price{1'000'000});
    assert(book.bids[0].quantity == Quantity{3});
    assert(book.asks[0].price == Price{1'050'000});
    assert(book.asks[0].quantity == Quantity{9});
}

void known_neutral_live_and_gap_are_deterministic() {
    MockXMdAdapter adapter{config()};
    adapter.begin_glimpse(mockx_session_key("MX00000001"), 10);
    const auto snapshot = snapshot_stream(10);
    assert(adapter.try_enqueue_soup_bytes(snapshot, 100, 101) ==
        MockXEnqueueResult::Accepted);
    const auto ready = adapter.drain();
    assert(ready.size() == 2);
    assert(ready.back().healthy());

    const auto neutral = mold_packet(10, {system_event('O', 6)});
    assert(adapter.try_enqueue_mold_datagram(neutral, 110, 111) ==
        MockXEnqueueResult::Accepted);
    const auto applied = adapter.drain();
    assert(applied.size() == 1);
    assert(applied.front().healthy());
    assert(adapter.feed_engine().next_sequence() == 11);

    const auto future = mold_packet(12, {system_event('M', 7)});
    assert(adapter.try_enqueue_mold_datagram(future, 120, 121) ==
        MockXEnqueueResult::Accepted);
    const auto gap = adapter.drain();
    assert(gap.size() == 1);
    assert(gap.front().error == FeedError::SequenceGap);
    assert(gap.front().state == MarketDataState::Recovering);
    assert(adapter.feed_engine().buffered_packet_count() == 1);
    assert(only_view(gap.front()).feed_state == MarketDataState::Recovering);
}

void malformed_and_unknown_wire_inputs_withdraw() {
    MockXMdAdapter truncated{config()};
    const std::vector<std::byte> short_packet(19);
    assert(truncated.try_enqueue_mold_datagram(short_packet, 10, 11) ==
        MockXEnqueueResult::Accepted);
    const auto short_result = truncated.drain();
    assert(short_result.size() == 1);
    assert(short_result.front().error == FeedError::TruncatedDatagram);
    assert(short_result.front().state == MarketDataState::Recovering);

    MockXMdAdapter unknown{config()};
    const std::vector<std::byte> unknown_message{std::byte{'X'}};
    const auto unknown_packet = mold_packet(1, {unknown_message});
    assert(unknown.try_enqueue_mold_datagram(unknown_packet, 10, 11) ==
        MockXEnqueueResult::Accepted);
    const auto unknown_result = unknown.drain();
    assert(unknown_result.size() == 1);
    assert(unknown_result.front().error == FeedError::UnsupportedMessage);
    assert(unknown_result.front().state == MarketDataState::Failed);

    MockXMdAdapter wrong_length{config()};
    auto bad_add = add_order(1, 'B', 1, 1'000'000, 1);
    bad_add.pop_back();
    const auto bad_packet = mold_packet(1, {bad_add});
    assert(wrong_length.try_enqueue_mold_datagram(bad_packet, 10, 11) ==
        MockXEnqueueResult::Accepted);
    const auto bad_result = wrong_length.drain();
    assert(bad_result.size() == 1);
    assert(bad_result.front().error == FeedError::MalformedFrame);
    assert(bad_result.front().state == MarketDataState::Failed);

    MockXMdAdapter bad_soup{config()};
    bad_soup.begin_glimpse(mockx_session_key("MX00000001"), 1);
    const auto unsupported_soup = soup_packet('X');
    assert(bad_soup.try_enqueue_soup_bytes(unsupported_soup, 10, 11) ==
        MockXEnqueueResult::Accepted);
    const auto soup_result = bad_soup.drain();
    assert(soup_result.size() == 1);
    assert(soup_result.front().error == FeedError::UnsupportedMessage);
    assert(soup_result.front().state == MarketDataState::Failed);
}

void bounded_raw_queue_reports_continuity_loss() {
    MockXMdAdapter adapter{config(1, 512)};
    const auto heartbeat = mold_packet(1, {}, 0);
    assert(adapter.try_enqueue_mold_datagram(heartbeat, 10, 11) ==
        MockXEnqueueResult::Accepted);
    assert(adapter.try_enqueue_mold_datagram(heartbeat, 12, 13) ==
        MockXEnqueueResult::QueueFull);
    const auto updates = adapter.drain();
    assert(!updates.empty());
    assert(updates.front().error == FeedError::RingContinuityLoss);
    assert(updates.front().state == MarketDataState::Recovering);
    assert(adapter.feed_engine().state() == MarketDataState::Recovering);
}

void external_transport_loss_withdraws_live_publications() {
    MockXMdAdapter adapter{config()};
    adapter.begin_glimpse(mockx_session_key("MX00000001"), 10);
    const auto snapshot = snapshot_stream(10);
    assert(adapter.try_enqueue_soup_bytes(snapshot, 100, 101) ==
        MockXEnqueueResult::Accepted);
    const auto ready = adapter.drain();
    assert(ready.size() == 2);
    assert(ready.back().healthy());

    const auto loss = adapter.continuity_lost(
        FeedError::RingContinuityLoss,
        110,
        111);
    assert(loss.error == FeedError::RingContinuityLoss);
    assert(loss.state == MarketDataState::Recovering);
    assert(loss.recovery == sorx::md::RecoveryAction::RequestFreshSnapshot);
    assert(!loss.publications.empty());
    assert(loss.publications.front().feed_state == MarketDataState::Recovering);
}

void external_transport_loss_aborts_partial_glimpse() {
    MockXMdAdapter adapter{config()};
    const auto session = mockx_session_key("MX00000001");
    adapter.begin_glimpse(session, 10);
    const auto stream = snapshot_stream(10);
    assert(adapter.try_enqueue_soup_bytes(
               std::span<const std::byte>{stream}.first(1),
               100,
               101) == MockXEnqueueResult::Accepted);
    assert(adapter.drain().empty());
    assert(adapter.glimpse_in_progress());

    const auto loss = adapter.continuity_lost(
        FeedError::RingContinuityLoss,
        102,
        103);
    assert(loss.state == MarketDataState::Recovering);
    assert(!adapter.glimpse_in_progress());

    adapter.begin_glimpse(session, 10);
    assert(adapter.glimpse_in_progress());
}

}  // namespace

int main() {
    fresh_glimpse_stitches_buffered_live_bytes();
    known_neutral_live_and_gap_are_deterministic();
    malformed_and_unknown_wire_inputs_withdraw();
    bounded_raw_queue_reports_continuity_loss();
    external_transport_loss_withdraws_live_publications();
    external_transport_loss_aborts_partial_glimpse();
    return 0;
}




