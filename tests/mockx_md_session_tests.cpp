#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/adapters/mockx_md_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using sorx::ChannelId;
using sorx::InstrumentId;
using sorx::MarketDataState;
using sorx::Price;
using sorx::ProfileId;
using sorx::Quantity;
using sorx::VenueId;
using sorx::adapters::BoundedMockXMdPublicationSink;
using sorx::adapters::MockXInstrumentBinding;
using sorx::adapters::MockXMdConfig;
using sorx::adapters::MockXMdHealthTransition;
using sorx::adapters::MockXMdPublication;
using sorx::adapters::MockXMdSession;
using sorx::adapters::MockXMdSessionConfig;
using sorx::adapters::MockXMdSessionError;
using sorx::adapters::MockXMdSessionState;
using sorx::md::EngineUpdate;
using sorx::md::FeedEngineConfig;
using sorx::md::FeedError;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void close_socket(const NativeSocket socket) { (void)::closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void close_socket(const NativeSocket socket) { (void)::close(socket); }
#endif

class TestNetwork final {
public:
    TestNetwork() {
#ifdef _WIN32
        WSADATA data{};
        assert(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    }
    ~TestNetwork() {
#ifdef _WIN32
        (void)::WSACleanup();
#endif
    }
};

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
    std::fill_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        width,
        std::byte{' '});
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

[[nodiscard]] std::vector<std::byte> directory() {
    auto result = itch_message('R', 39, 17, 1);
    set_ascii(result, 11, 8, "ALPHA");
    return result;
}

[[nodiscard]] std::vector<std::byte> trading_action() {
    auto result = itch_message('H', 25, 17, 2);
    set_ascii(result, 11, 8, "ALPHA");
    result[19] = std::byte{'T'};
    return result;
}

[[nodiscard]] std::vector<std::byte> add_order(
    const std::uint64_t reference,
    const char side,
    const std::uint32_t quantity,
    const std::uint32_t price,
    const std::uint64_t timestamp) {
    auto result = itch_message('A', 36, 17, timestamp);
    set_be(result, 11, 8, reference);
    result[19] = static_cast<std::byte>(static_cast<unsigned char>(side));
    set_be(result, 20, 4, quantity);
    set_ascii(result, 24, 8, "ALPHA");
    set_be(result, 32, 4, price);
    return result;
}

[[nodiscard]] std::vector<std::byte> execution(
    const std::uint64_t reference,
    const std::uint32_t quantity,
    const std::uint64_t timestamp) {
    auto result = itch_message('E', 31, 17, timestamp);
    set_be(result, 11, 8, reference);
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
    const std::vector<std::vector<std::byte>>& messages) {
    auto size = std::size_t{20};
    for (const auto& message : messages) {
        size += 2 + message.size();
    }
    std::vector<std::byte> result(size);
    set_ascii(result, 0, 10, "MX00000001");
    set_be(result, 10, 8, first_sequence);
    set_be(result, 18, 2, messages.size());
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

[[nodiscard]] std::vector<std::byte> login_accepted() {
    std::vector<std::byte> payload(30, std::byte{' '});
    set_ascii(payload, 0, 10, "GS00000001");
    set_ascii(payload, 10, 20, "1", true);
    return soup_packet('A', payload);
}

[[nodiscard]] std::vector<std::byte> snapshot_stream(const std::uint64_t cut) {
    std::vector<std::byte> result;
    append(result, login_accepted());
    append(result, soup_packet('S', directory()));
    append(result, soup_packet('S', trading_action()));
    append(result, soup_packet('S', add_order(100, 'B', 5, 1'000'000, 3)));
    append(result, soup_packet('S', add_order(102, 'S', 9, 1'050'000, 4)));
    append(result, soup_packet('S', end_snapshot(cut)));
    append(result, soup_packet('Z'));
    return result;
}

void send_all(
    const NativeSocket socket,
    const std::span<const std::byte> bytes) {
    auto offset = std::size_t{};
    while (offset < bytes.size()) {
        const auto count = static_cast<int>(std::min<std::size_t>(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
        const auto sent = ::send(
            socket,
            reinterpret_cast<const char*>(bytes.data() + offset),
            count,
            0);
        assert(sent != SOCKET_ERROR && sent > 0);
#else
        const auto sent = ::send(
            socket,
            bytes.data() + offset,
            static_cast<std::size_t>(count),
            0);
        assert(sent > 0);
#endif
        offset += static_cast<std::size_t>(sent);
    }
}

void receive_exact(
    const NativeSocket socket,
    const std::span<std::byte> destination) {
    auto offset = std::size_t{};
    while (offset < destination.size()) {
        const auto count = static_cast<int>(destination.size() - offset);
#ifdef _WIN32
        const auto received = ::recv(
            socket,
            reinterpret_cast<char*>(destination.data() + offset),
            count,
            0);
        assert(received != SOCKET_ERROR && received > 0);
#else
        const auto received = ::recv(
            socket,
            destination.data() + offset,
            static_cast<std::size_t>(count),
            0);
        assert(received > 0);
#endif
        offset += static_cast<std::size_t>(received);
    }
}

class FakeGlimpse final {
public:
    FakeGlimpse() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener_ != kInvalidSocket);
        const int enabled = 1;
        assert(::setsockopt(
            listener_,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled))) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        assert(::bind(
            listener_,
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == 0);
        assert(::listen(listener_, 4) == 0);
#ifdef _WIN32
        int size = static_cast<int>(sizeof(address));
#else
        socklen_t size = static_cast<socklen_t>(sizeof(address));
#endif
        assert(::getsockname(
            listener_,
            reinterpret_cast<sockaddr*>(&address),
            &size) == 0);
        port_ = ntohs(address.sin_port);
    }

    ~FakeGlimpse() {
        if (thread_.joinable()) {
            thread_.join();
        }
        close_socket(listener_);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void start() {
        thread_ = std::thread{[this] {
            try {
                serve();
            } catch (...) {
                failure_ = std::current_exception();
            }
        }};
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] NativeSocket accept_login() {
        const auto client = ::accept(listener_, nullptr, nullptr);
        assert(client != kInvalidSocket);
        std::array<std::byte, 54> login{};
        receive_exact(client, login);
        assert(login[0] == std::byte{0});
        assert(login[1] == std::byte{52});
        assert(login[2] == std::byte{'L'});
        assert(std::all_of(login.begin() + 19, login.begin() + 29, [](const auto byte) {
            return byte == std::byte{' '};
        }));
        assert(login[48] == std::byte{'1'});
        return client;
    }

    void send_fragmented_snapshot(
        const NativeSocket client,
        const std::uint64_t cut) {
        const auto stream = snapshot_stream(cut);
        send_all(client, std::span<const std::byte>{stream}.first(1));
        std::this_thread::sleep_for(10ms);
        // One write now contains the rest of the partial first frame and many
        // complete frames, exercising both TCP fragmentation and coalescing.
        send_all(client, std::span<const std::byte>{stream}.subspan(1));
    }

    void serve() {
        auto client = accept_login();
        close_socket(client);  // force one bounded pre-login reconnect

        client = accept_login();
        send_fragmented_snapshot(client, 10);
        close_socket(client);

        client = accept_login();
        send_fragmented_snapshot(client, 12);
        close_socket(client);
    }

    NativeSocket listener_{kInvalidSocket};
    std::uint16_t port_{};
    std::thread thread_;
    std::exception_ptr failure_;
};

void send_udp(
    const std::uint16_t port,
    const std::span<const std::byte> bytes) {
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(socket != kInvalidSocket);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) == 1);
#ifdef _WIN32
    const auto sent = ::sendto(
        socket,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        static_cast<int>(sizeof(destination)));
    assert(sent != SOCKET_ERROR && static_cast<std::size_t>(sent) == bytes.size());
#else
    const auto sent = ::sendto(
        socket,
        bytes.data(),
        bytes.size(),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        static_cast<socklen_t>(sizeof(destination)));
    assert(sent >= 0 && static_cast<std::size_t>(sent) == bytes.size());
#endif
    close_socket(socket);
}

[[nodiscard]] MockXMdSessionConfig config(const std::uint16_t glimpse_port) {
    MockXMdSessionConfig result;
    result.adapter.feed = FeedEngineConfig{
        VenueId{7},
        ChannelId{9},
        ProfileId{11},
        8,
        sorx::md::RecoveryStrategy::FreshSnapshot,
        64,
        1'024};
    result.adapter.instruments = {
        MockXInstrumentBinding{17, "ALPHA", InstrumentId{101}}};
    result.adapter.raw_queue_capacity = 8;
    result.adapter.max_raw_input_bytes = 4'096;
    result.adapter.max_messages_per_mold_packet = 32;
    result.adapter.max_glimpse_messages = 64;
    result.adapter.max_soup_buffer_bytes = 4'096;
    result.udp.bind_address = "127.0.0.1";
    result.udp.port = 0;
    result.udp.multicast_group.clear();
    result.glimpse.host = "127.0.0.1";
    result.glimpse.port = glimpse_port;
    result.glimpse.username = "GLIMP";
    result.glimpse.password = "PASS";
    result.glimpse.heartbeat_timeout_ms = 200;
    result.glimpse.transport_retries = 2;
    result.glimpse.connect_timeout = 500ms;
    result.glimpse.socket_poll_interval = 20ms;
    result.glimpse.initial_retry_backoff = 10ms;
    result.glimpse.maximum_retry_backoff = 20ms;
    result.transport_queue_capacity = 32;
    result.max_udp_datagram_bytes = 4'096;
    result.max_soup_frame_bytes = 4'096;
    result.coordinator_drain_batch = 64;
    return result;
}

void drain(
    const std::shared_ptr<BoundedMockXMdPublicationSink>& sink,
    std::vector<MockXMdPublication>& publications) {
    while (auto publication = sink->try_pop()) {
        publications.push_back(std::move(*publication));
    }
}

template <class Predicate>
[[nodiscard]] bool wait_until(
    Predicate predicate,
    const std::shared_ptr<BoundedMockXMdPublicationSink>& sink,
    std::vector<MockXMdPublication>& publications,
    const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        drain(sink, publications);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    drain(sink, publications);
    return predicate();
}

[[nodiscard]] std::size_t live_update_count(
    const std::vector<MockXMdPublication>& publications) {
    return static_cast<std::size_t>(std::count_if(
        publications.begin(),
        publications.end(),
        [](const auto& publication) {
            const auto* update = std::get_if<EngineUpdate>(&publication.payload);
            return update != nullptr && update->healthy();
        }));
}

void bounded_sink_is_nonblocking_and_loss_explicit() {
    BoundedMockXMdPublicationSink sink{1};
    MockXMdPublication first{
        1,
        sorx::md::EngineUpdate{}};
    MockXMdPublication second{
        2,
        MockXMdHealthTransition{}};
    assert(sink.try_publish(std::move(first)));
    assert(!sink.try_publish(std::move(second)));
    assert(sink.size() == 1);
    const auto popped = sink.try_pop();
    assert(popped && popped->sequence == 1);
    assert(!sink.try_pop());
}

void loopback_session_stitches_recovers_and_stops() {
    FakeGlimpse server;
    server.start();
    auto sink = std::make_shared<BoundedMockXMdPublicationSink>(512);
    MockXMdSession session{config(server.port()), sink};
    session.start();
    assert(session.local_udp_port() != 0);

    std::vector<MockXMdPublication> publications;
    const auto live = mold_packet(10, {execution(100, 2, 5)});
    send_udp(session.local_udp_port(), live);
    assert(wait_until(
        [&] { return session.state() == MockXMdSessionState::Live; },
        sink,
        publications));
    assert(live_update_count(publications) >= 1);

    auto saw_expected_book = false;
    for (const auto& publication : publications) {
        const auto* update = std::get_if<EngineUpdate>(&publication.payload);
        if (update == nullptr || !update->healthy() || update->publications.empty()) {
            continue;
        }
        const auto& book = update->publications.front();
        if (book.instrument == InstrumentId{101} &&
            book.bids[0].price == Price{1'000'000} &&
            book.bids[0].quantity == Quantity{3} &&
            book.asks[0].quantity == Quantity{9}) {
            saw_expected_book = true;
        }
    }
    assert(saw_expected_book);

    const auto gap = mold_packet(12, {system_event('M', 6)});
    send_udp(session.local_udp_port(), gap);
    assert(wait_until(
        [&] { return live_update_count(publications) >= 2; },
        sink,
        publications));

    auto saw_gap = false;
    auto saw_non_live_health = false;
    for (const auto& publication : publications) {
        if (const auto* update = std::get_if<EngineUpdate>(&publication.payload)) {
            saw_gap = saw_gap || update->error == FeedError::SequenceGap;
        }
        if (const auto* health =
                std::get_if<MockXMdHealthTransition>(&publication.payload)) {
            saw_non_live_health = saw_non_live_health ||
                health->current == MockXMdSessionState::Recovering ||
                health->current == MockXMdSessionState::Snapshotting;
        }
    }
    assert(saw_gap);
    assert(saw_non_live_health);
    assert(session.state() == MockXMdSessionState::Live);
    assert(session.last_error() == MockXMdSessionError::None);
    const auto counters = session.counters();
    assert(counters.udp_datagrams_received == 2);
    assert(counters.soup_frames_received == 14);
    assert(counters.glimpse_attempts == 3);
    assert(counters.glimpse_reconnects == 1);
    assert(counters.recovery_requests >= 1);
    assert(counters.raw_queue_losses == 0);

    server.join();
    const auto before = std::chrono::steady_clock::now();
    session.stop();
    assert(std::chrono::steady_clock::now() - before < 1s);
    drain(sink, publications);
    assert(!session.running());
    assert(session.state() == MockXMdSessionState::Stopped);
}

void native_raw_ring_loss_is_immediately_visible() {
    auto sink = std::make_shared<BoundedMockXMdPublicationSink>(512);
    auto value = config(1);
    value.transport_queue_capacity = 1;
    value.adapter.feed.stitch_buffer_packet_capacity = 4'096;
    value.adapter.feed.stitch_buffer_event_capacity = 65'536;
    MockXMdSession session{std::move(value), sink};
    session.start();

    std::vector<std::vector<std::byte>> messages;
    messages.reserve(32);
    for (auto index = std::uint64_t{}; index < 32; ++index) {
        messages.push_back(system_event('O', index + 1));
    }
    std::vector<std::vector<std::byte>> packets;
    packets.reserve(512);
    packets.push_back(mold_packet(10, messages));
    for (auto index = std::uint64_t{}; index < 511; ++index) {
        packets.push_back(mold_packet(
            42 + index,
            {system_event('O', 100 + index)}));
    }

    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(socket != kInvalidSocket);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(session.local_udp_port());
    assert(::inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) == 1);
    for (const auto& packet : packets) {
#ifdef _WIN32
        const auto sent = ::sendto(
            socket,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            static_cast<int>(sizeof(destination)));
        assert(sent != SOCKET_ERROR &&
            static_cast<std::size_t>(sent) == packet.size());
#else
        const auto sent = ::sendto(
            socket,
            packet.data(),
            packet.size(),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            static_cast<socklen_t>(sizeof(destination)));
        assert(sent >= 0 && static_cast<std::size_t>(sent) == packet.size());
#endif
    }
    close_socket(socket);

    std::vector<MockXMdPublication> publications;
    assert(wait_until(
        [&] { return session.counters().raw_queue_losses != 0; },
        sink,
        publications));
    auto saw_feed_invalidation = false;
    auto saw_transport_invalidation = false;
    for (const auto& publication : publications) {
        if (const auto* update = std::get_if<EngineUpdate>(&publication.payload)) {
            saw_feed_invalidation = saw_feed_invalidation ||
                (update->error == FeedError::RingContinuityLoss &&
                 update->state != MarketDataState::Live);
        }
        if (const auto* health =
                std::get_if<MockXMdHealthTransition>(&publication.payload)) {
            saw_transport_invalidation = saw_transport_invalidation ||
                health->error == MockXMdSessionError::RawQueueContinuityLoss;
        }
    }
    assert(saw_feed_invalidation);
    assert(saw_transport_invalidation);
    session.stop();
}

void publication_overload_fails_closed_without_network_input() {
    auto sink = std::make_shared<BoundedMockXMdPublicationSink>(1);
    auto value = config(1);
    MockXMdSession session{std::move(value), sink};
    session.start();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (session.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    assert(session.state() == MockXMdSessionState::Failed);
    assert(session.last_error() == MockXMdSessionError::PublicationSinkFull);
    assert(session.counters().publications_rejected == 1);
    session.stop();
}

}  // namespace

int main() {
    TestNetwork network;
    bounded_sink_is_nonblocking_and_loss_explicit();
    loopback_session_stitches_recovers_and_stops();
    native_raw_ring_loss_is_immediately_visible();
    publication_overload_fails_closed_without_network_input();
    return 0;
}








