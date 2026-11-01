#include "sorx/adapters/mockx_md_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sorx::adapters {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
[[nodiscard]] NativeSocket native_socket(const std::intptr_t value) noexcept {
    return static_cast<NativeSocket>(value);
}
[[nodiscard]] std::intptr_t stored_socket(const NativeSocket value) noexcept {
    return static_cast<std::intptr_t>(value);
}
[[nodiscard]] int last_socket_error() noexcept { return WSAGetLastError(); }
void close_socket(const NativeSocket value) noexcept { (void)::closesocket(value); }
void shutdown_socket(const NativeSocket value) noexcept {
    (void)::shutdown(value, SD_BOTH);
}
[[nodiscard]] bool interrupted(const int error) noexcept {
    return error == WSAEINTR;
}
[[nodiscard]] bool timed_out(const int error) noexcept {
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}
[[nodiscard]] bool connect_pending(const int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
        error == WSAEINVAL;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
[[nodiscard]] NativeSocket native_socket(const std::intptr_t value) noexcept {
    return static_cast<NativeSocket>(value);
}
[[nodiscard]] std::intptr_t stored_socket(const NativeSocket value) noexcept {
    return static_cast<std::intptr_t>(value);
}
[[nodiscard]] int last_socket_error() noexcept { return errno; }
void close_socket(const NativeSocket value) noexcept { (void)::close(value); }
void shutdown_socket(const NativeSocket value) noexcept {
    (void)::shutdown(value, SHUT_RDWR);
}
[[nodiscard]] bool interrupted(const int error) noexcept {
    return error == EINTR;
}
[[nodiscard]] bool timed_out(const int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}
[[nodiscard]] bool connect_pending(const int error) noexcept {
    return error == EINPROGRESS;
}
#endif

class SocketFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throw_socket_failure(const char* operation) {
    throw SocketFailure{
        std::string{operation} + " failed with socket error " +
        std::to_string(last_socket_error())};
}

class NetworkRuntime final {
public:
    NetworkRuntime() {
#ifdef _WIN32
        WSADATA data{};
        const auto result = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw SocketFailure{
                "WSAStartup failed with socket error " + std::to_string(result)};
        }
#endif
    }

    ~NetworkRuntime() {
#ifdef _WIN32
        (void)::WSACleanup();
#endif
    }

    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class SocketOwner final {
public:
    SocketOwner() = default;
    explicit SocketOwner(const NativeSocket value) noexcept : value_(value) {}
    ~SocketOwner() { reset(); }

    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;

    SocketOwner(SocketOwner&& other) noexcept
        : value_(std::exchange(other.value_, kInvalidSocket)) {}

    SocketOwner& operator=(SocketOwner&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, kInvalidSocket);
        }
        return *this;
    }

    [[nodiscard]] NativeSocket get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != kInvalidSocket; }

    void reset() noexcept {
        if (valid()) {
            close_socket(value_);
            value_ = kInvalidSocket;
        }
    }

private:
    NativeSocket value_{kInvalidSocket};
};

void set_reuse_address(const NativeSocket socket) {
    const int enabled = 1;
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled))) != 0) {
        throw_socket_failure("setsockopt(SO_REUSEADDR)");
    }
}

void set_receive_timeout(
    const NativeSocket socket,
    const std::chrono::milliseconds timeout) {
#ifdef _WIN32
    const auto bounded = std::clamp<std::int64_t>(timeout.count(), 1, 0xffff'ffffLL);
    const auto value = static_cast<DWORD>(bounded);
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&value),
            static_cast<int>(sizeof(value))) != 0) {
        throw_socket_failure("setsockopt(SO_RCVTIMEO)");
    }
#else
    const auto count = std::max<std::int64_t>(timeout.count(), 1);
    timeval value{
        static_cast<long>(count / 1'000),
        static_cast<long>((count % 1'000) * 1'000)};
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &value,
            static_cast<socklen_t>(sizeof(value))) != 0) {
        throw_socket_failure("setsockopt(SO_RCVTIMEO)");
    }
#endif
}

void set_blocking(const NativeSocket socket, const bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 0UL : 1UL;
    if (::ioctlsocket(socket, FIONBIO, &mode) != 0) {
        throw_socket_failure("ioctlsocket(FIONBIO)");
    }
#else
    const auto flags = ::fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        throw_socket_failure("fcntl(F_GETFL)");
    }
    const auto next = enabled ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (::fcntl(socket, F_SETFL, next) == -1) {
        throw_socket_failure("fcntl(F_SETFL)");
    }
#endif
}

[[nodiscard]] SocketOwner connect_tcp(
    const std::string& host,
    const std::uint16_t port,
    const std::chrono::milliseconds timeout,
    const std::chrono::milliseconds receive_poll) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto lookup = ::getaddrinfo(
        host.c_str(),
        service.c_str(),
        &hints,
        &addresses);
    if (lookup != 0) {
        throw SocketFailure{"getaddrinfo failed with code " + std::to_string(lookup)};
    }

    SocketOwner connected;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        SocketOwner candidate{::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol)};
        if (!candidate.valid()) {
            continue;
        }
        try {
            set_blocking(candidate.get(), false);
#ifdef _WIN32
            const auto result = ::connect(
                candidate.get(),
                address->ai_addr,
                static_cast<int>(address->ai_addrlen));
#else
            const auto result = ::connect(
                candidate.get(),
                address->ai_addr,
                address->ai_addrlen);
#endif
            if (result != 0) {
                const auto error = last_socket_error();
                if (!connect_pending(error)) {
                    continue;
                }

                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(candidate.get(), &writable);
                const auto count = std::max<std::int64_t>(timeout.count(), 1);
                timeval wait{
                    static_cast<long>(count / 1'000),
                    static_cast<long>((count % 1'000) * 1'000)};
#ifdef _WIN32
                const auto selected = ::select(0, nullptr, &writable, nullptr, &wait);
#else
                const auto selected = ::select(
                    candidate.get() + 1,
                    nullptr,
                    &writable,
                    nullptr,
                    &wait);
#endif
                if (selected <= 0) {
                    continue;
                }
                int socket_error{};
#ifdef _WIN32
                int length = static_cast<int>(sizeof(socket_error));
#else
                socklen_t length = static_cast<socklen_t>(sizeof(socket_error));
#endif
                if (::getsockopt(
                        candidate.get(),
                        SOL_SOCKET,
                        SO_ERROR,
                        reinterpret_cast<char*>(&socket_error),
                        &length) != 0 ||
                    socket_error != 0) {
                    continue;
                }
            }
            set_blocking(candidate.get(), true);
            set_receive_timeout(candidate.get(), receive_poll);
            connected = std::move(candidate);
            break;
        } catch (const SocketFailure&) {
            continue;
        }
    }
    ::freeaddrinfo(addresses);
    if (!connected.valid()) {
        throw SocketFailure{"connect(TCP) exhausted resolved addresses"};
    }
    return connected;
}

void send_all(const NativeSocket socket, const std::span<const std::byte> bytes) {
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
        if (sent != SOCKET_ERROR) {
#else
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto sent = ::send(
            socket,
            bytes.data() + offset,
            static_cast<std::size_t>(count),
            send_flags);
        if (sent >= 0) {
#endif
            if (sent == 0) {
                throw SocketFailure{"TCP peer closed during send"};
            }
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        const auto error = last_socket_error();
        if (!interrupted(error)) {
            throw SocketFailure{"send(TCP) failed with socket error " +
                std::to_string(error)};
        }
    }
}

enum class ReceiveKind : std::uint8_t { Data, Timeout, Closed, Truncated };

struct ReceiveResult final {
    ReceiveKind kind{ReceiveKind::Timeout};
    std::size_t size{};
};

[[nodiscard]] ReceiveResult receive_tcp(
    const NativeSocket socket,
    const std::span<std::byte> destination) {
    const auto count = static_cast<int>(std::min<std::size_t>(
        destination.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    for (;;) {
#ifdef _WIN32
        const auto received = ::recv(
            socket,
            reinterpret_cast<char*>(destination.data()),
            count,
            0);
        if (received != SOCKET_ERROR) {
#else
        const auto received = ::recv(
            socket,
            destination.data(),
            static_cast<std::size_t>(count),
            0);
        if (received >= 0) {
#endif
            if (received == 0) {
                return {ReceiveKind::Closed, 0};
            }
            return {ReceiveKind::Data, static_cast<std::size_t>(received)};
        }
        const auto error = last_socket_error();
        if (interrupted(error)) {
            continue;
        }
        if (timed_out(error)) {
            return {ReceiveKind::Timeout, 0};
        }
        throw SocketFailure{
            "recv(TCP) failed with socket error " + std::to_string(error)};
    }
}

[[nodiscard]] bool wait_readable(
    const NativeSocket socket,
    const std::chrono::milliseconds timeout) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    const auto count = std::max<std::int64_t>(timeout.count(), 1);
    timeval wait{
        static_cast<long>(count / 1'000),
        static_cast<long>((count % 1'000) * 1'000)};
#ifdef _WIN32
    const auto selected = ::select(0, &readable, nullptr, nullptr, &wait);
#else
    const auto selected = ::select(
        socket + 1,
        &readable,
        nullptr,
        nullptr,
        &wait);
#endif
    if (selected > 0) {
        return true;
    }
    if (selected == 0 || interrupted(last_socket_error())) {
        return false;
    }
    throw_socket_failure("select(read)");
}

[[nodiscard]] ReceiveResult receive_udp(
    const NativeSocket socket,
    const std::span<std::byte> destination) {
    for (;;) {
#ifdef _WIN32
        const auto count = static_cast<int>(std::min<std::size_t>(
            destination.size(),
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const auto received = ::recvfrom(
            socket,
            reinterpret_cast<char*>(destination.data()),
            count,
            0,
            nullptr,
            nullptr);
        if (received != SOCKET_ERROR) {
            return {ReceiveKind::Data, static_cast<std::size_t>(received)};
        }
        const auto error = last_socket_error();
        if (error == WSAEMSGSIZE) {
            return {ReceiveKind::Truncated, 0};
        }
#else
        iovec vector{destination.data(), destination.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const auto received = ::recvmsg(socket, &message, MSG_TRUNC);
        if (received >= 0) {
            if (static_cast<std::size_t>(received) > destination.size() ||
                (message.msg_flags & MSG_TRUNC) != 0) {
                return {ReceiveKind::Truncated, 0};
            }
            return {ReceiveKind::Data, static_cast<std::size_t>(received)};
        }
        const auto error = last_socket_error();
#endif
        if (interrupted(error)) {
            continue;
        }
        if (timed_out(error)) {
            return {ReceiveKind::Timeout, 0};
        }
        throw SocketFailure{
            "recvfrom(UDP) failed with socket error " + std::to_string(error)};
    }
}

[[nodiscard]] SocketOwner open_udp(
    const MockXMdUdpEndpoint& endpoint,
    const std::chrono::milliseconds receive_poll,
    std::uint16_t& actual_port) {
    SocketOwner socket{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (!socket.valid()) {
        throw_socket_failure("socket(UDP)");
    }
    set_reuse_address(socket.get());
    set_receive_timeout(socket.get(), receive_poll);

    const auto buffer_size = static_cast<int>(std::min<std::size_t>(
        endpoint.receive_buffer_bytes,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    if (buffer_size > 0) {
        (void)::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_RCVBUF,
            reinterpret_cast<const char*>(&buffer_size),
            static_cast<int>(sizeof(buffer_size)));
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(endpoint.port);
    if (::inet_pton(AF_INET, endpoint.bind_address.c_str(), &local.sin_addr) != 1) {
        throw std::invalid_argument{"invalid UDP IPv4 bind address"};
    }
    if (::bind(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&local),
            static_cast<int>(sizeof(local))) != 0) {
        throw_socket_failure("bind(UDP)");
    }

    if (!endpoint.multicast_group.empty()) {
        ip_mreq membership{};
        if (::inet_pton(
                AF_INET,
                endpoint.multicast_group.c_str(),
                &membership.imr_multiaddr) != 1 ||
            ::inet_pton(
                AF_INET,
                endpoint.multicast_interface.c_str(),
                &membership.imr_interface) != 1) {
            throw std::invalid_argument{"invalid IPv4 multicast endpoint"};
        }
        if (::setsockopt(
                socket.get(),
                IPPROTO_IP,
                IP_ADD_MEMBERSHIP,
                reinterpret_cast<const char*>(&membership),
                static_cast<int>(sizeof(membership))) != 0) {
            throw_socket_failure("setsockopt(IP_ADD_MEMBERSHIP)");
        }
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int bound_size = static_cast<int>(sizeof(bound));
#else
    socklen_t bound_size = static_cast<socklen_t>(sizeof(bound));
#endif
    if (::getsockname(
            socket.get(),
            reinterpret_cast<sockaddr*>(&bound),
            &bound_size) != 0) {
        throw_socket_failure("getsockname(UDP)");
    }
    actual_port = ntohs(bound.sin_port);
    return socket;
}

[[nodiscard]] bool printable_ascii(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto code = static_cast<unsigned char>(character);
        return code >= 0x21U && code <= 0x7eU;
    });
}

void append_fixed(
    std::vector<std::byte>& output,
    const std::string_view value,
    const std::size_t width,
    const bool left_pad) {
    const auto padding = width - value.size();
    if (left_pad) {
        output.insert(output.end(), padding, std::byte{' '});
    }
    for (const auto character : value) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
    if (!left_pad) {
        output.insert(output.end(), padding, std::byte{' '});
    }
}

[[nodiscard]] std::vector<std::byte> soup_login(
    const MockXGlimpseEndpoint& config,
    const std::string_view session,
    const std::uint64_t sequence) {
    std::vector<std::byte> result;
    result.reserve(54);
    result.push_back(std::byte{0});
    result.push_back(std::byte{52});
    result.push_back(std::byte{'L'});
    append_fixed(result, config.username, 6, false);
    append_fixed(result, config.password, 10, false);
    append_fixed(result, session, 10, true);
    append_fixed(result, std::to_string(sequence), 20, true);
    append_fixed(
        result,
        std::to_string(config.heartbeat_timeout_ms),
        5,
        true);
    return result;
}

[[nodiscard]] constexpr std::array<std::byte, 3> soup_heartbeat() noexcept {
    return {std::byte{0}, std::byte{1}, std::byte{'R'}};
}

[[nodiscard]] std::uint64_t load_be(
    const std::span<const std::byte> bytes) noexcept {
    auto result = std::uint64_t{};
    for (const auto value : bytes) {
        result = (result << 8U) | std::to_integer<std::uint8_t>(value);
    }
    return result;
}

[[nodiscard]] std::string fixed_ascii(
    const std::span<const std::byte> bytes) {
    auto first = std::size_t{};
    auto last = bytes.size();
    while (first < last && bytes[first] == std::byte{' '}) {
        ++first;
    }
    while (last > first && bytes[last - 1] == std::byte{' '}) {
        --last;
    }
    if (first == last) {
        throw std::invalid_argument{"blank fixed ASCII field"};
    }
    std::string result;
    result.reserve(last - first);
    for (auto index = first; index < last; ++index) {
        const auto code = std::to_integer<unsigned char>(bytes[index]);
        if (code < 0x21U || code > 0x7eU) {
            throw std::invalid_argument{"invalid fixed ASCII field"};
        }
        result.push_back(static_cast<char>(code));
    }
    return result;
}

[[nodiscard]] std::uint64_t padded_decimal(
    const std::span<const std::byte> bytes) {
    auto first = std::size_t{};
    while (first < bytes.size() && bytes[first] == std::byte{' '}) {
        ++first;
    }
    if (first == bytes.size()) {
        throw std::invalid_argument{"blank decimal field"};
    }
    auto result = std::uint64_t{};
    for (auto index = first; index < bytes.size(); ++index) {
        const auto character = static_cast<char>(
            std::to_integer<unsigned char>(bytes[index]));
        if (character < '0' || character > '9') {
            throw std::invalid_argument{"invalid decimal field"};
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw std::invalid_argument{"decimal field overflow"};
        }
        result = result * 10U + digit;
    }
    return result;
}

struct RawView final {
    std::span<const std::byte> bytes;
    std::uint64_t transport_sequence{};
    std::uint64_t receive_time_ns{};
    std::uint64_t snapshot_generation{};
};

class DirectSpscRing final {
public:
    DirectSpscRing(const std::size_t capacity, const std::size_t max_bytes)
        : capacity_(capacity),
          max_bytes_(max_bytes),
          storage_(checked_storage_size(capacity, max_bytes)),
          slots_(capacity) {}

    [[nodiscard]] std::optional<std::span<std::byte>> try_acquire() noexcept {
        const auto producer = producer_.load(std::memory_order_relaxed);
        const auto consumer = consumer_.load(std::memory_order_acquire);
        if (producer - consumer >= capacity_) {
            return std::nullopt;
        }
        return std::span<std::byte>{
            storage_.data() + (producer % capacity_) * max_bytes_,
            max_bytes_};
    }

    void commit(
        const std::size_t size,
        const std::uint64_t transport_sequence,
        const std::uint64_t receive_time_ns,
        const std::uint64_t snapshot_generation) noexcept {
        const auto producer = producer_.load(std::memory_order_relaxed);
        auto& slot = slots_[producer % capacity_];
        slot.size = size;
        slot.transport_sequence = transport_sequence;
        slot.receive_time_ns = receive_time_ns;
        slot.snapshot_generation = snapshot_generation;
        producer_.store(producer + 1, std::memory_order_release);
    }

    [[nodiscard]] std::optional<RawView> try_peek() const noexcept {
        const auto consumer = consumer_.load(std::memory_order_relaxed);
        const auto producer = producer_.load(std::memory_order_acquire);
        if (consumer == producer) {
            return std::nullopt;
        }
        const auto index = consumer % capacity_;
        const auto& slot = slots_[index];
        return RawView{
            std::span<const std::byte>{
                storage_.data() + index * max_bytes_,
                slot.size},
            slot.transport_sequence,
            slot.receive_time_ns,
            slot.snapshot_generation};
    }

    void release() noexcept {
        const auto consumer = consumer_.load(std::memory_order_relaxed);
        consumer_.store(consumer + 1, std::memory_order_release);
    }

    void discard_all() noexcept {
        consumer_.store(
            producer_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

private:
    struct Slot final {
        std::size_t size{};
        std::uint64_t transport_sequence{};
        std::uint64_t receive_time_ns{};
        std::uint64_t snapshot_generation{};
    };

    [[nodiscard]] static std::size_t checked_storage_size(
        const std::size_t capacity,
        const std::size_t max_bytes) {
        if (capacity == 0 || max_bytes == 0 ||
            capacity > std::numeric_limits<std::size_t>::max() / max_bytes) {
            throw std::invalid_argument{"invalid session raw-ring dimensions"};
        }
        return capacity * max_bytes;
    }

    std::size_t capacity_{};
    std::size_t max_bytes_{};
    std::vector<std::byte> storage_;
    std::vector<Slot> slots_;
    std::atomic<std::size_t> producer_{};
    std::atomic<std::size_t> consumer_{};
};

class SteadyClock final : public MockXMdMonotonicClock {
public:
    [[nodiscard]] std::uint64_t now_ns() noexcept override {
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return value <= 0 ? 1 : static_cast<std::uint64_t>(value);
    }
};

enum class FaultKind : std::uint8_t {
    None,
    UdpRawLoss,
    UdpTransport,
    GlimpseTransport,
    GlimpseAuthentication,
    GlimpseProtocol,
};

struct Fault final {
    FaultKind kind{FaultKind::None};
    std::uint64_t snapshot_generation{};
    std::uint64_t time_ns{};
};

[[nodiscard]] MockXMdSessionState session_state(
    const MarketDataState state) noexcept {
    switch (state) {
    case MarketDataState::Snapshotting:
        return MockXMdSessionState::Snapshotting;
    case MarketDataState::Stitching:
        return MockXMdSessionState::Stitching;
    case MarketDataState::Live:
        return MockXMdSessionState::Live;
    case MarketDataState::Recovering:
        return MockXMdSessionState::Recovering;
    case MarketDataState::Failed:
        return MockXMdSessionState::Failed;
    }
    return MockXMdSessionState::Failed;
}

[[nodiscard]] MockXMdSessionError session_error(
    const FaultKind kind) noexcept {
    switch (kind) {
    case FaultKind::UdpRawLoss:
        return MockXMdSessionError::RawQueueContinuityLoss;
    case FaultKind::UdpTransport:
        return MockXMdSessionError::UdpTransportFailure;
    case FaultKind::GlimpseTransport:
        return MockXMdSessionError::GlimpseTransportFailure;
    case FaultKind::GlimpseAuthentication:
        return MockXMdSessionError::GlimpseAuthenticationRejected;
    case FaultKind::GlimpseProtocol:
        return MockXMdSessionError::GlimpseProtocolFailure;
    case FaultKind::None:
        return MockXMdSessionError::None;
    }
    return MockXMdSessionError::GlimpseProtocolFailure;
}

[[nodiscard]] bool fault_is_terminal(const FaultKind kind) noexcept {
    return kind == FaultKind::UdpTransport ||
        kind == FaultKind::GlimpseTransport ||
        kind == FaultKind::GlimpseAuthentication ||
        kind == FaultKind::GlimpseProtocol;
}

}  // namespace

struct BoundedMockXMdPublicationSink::Impl final {
    explicit Impl(const std::size_t value)
        : capacity(value), slots(value) {
        if (capacity == 0) {
            throw std::invalid_argument{"publication sink capacity must be positive"};
        }
    }

    std::size_t capacity{};
    std::vector<std::optional<MockXMdPublication>> slots;
    std::atomic<std::size_t> producer{};
    std::atomic<std::size_t> consumer{};
};

BoundedMockXMdPublicationSink::BoundedMockXMdPublicationSink(
    const std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

BoundedMockXMdPublicationSink::~BoundedMockXMdPublicationSink() = default;

bool BoundedMockXMdPublicationSink::try_publish(
    MockXMdPublication publication) noexcept {
    try {
        const auto producer = impl_->producer.load(std::memory_order_relaxed);
        const auto consumer = impl_->consumer.load(std::memory_order_acquire);
        if (producer - consumer >= impl_->capacity) {
            return false;
        }
        impl_->slots[producer % impl_->capacity].emplace(std::move(publication));
        impl_->producer.store(producer + 1, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<MockXMdPublication>
BoundedMockXMdPublicationSink::try_pop() noexcept {
    static_assert(std::is_nothrow_move_constructible_v<MockXMdPublication>);
    const auto consumer = impl_->consumer.load(std::memory_order_relaxed);
    const auto producer = impl_->producer.load(std::memory_order_acquire);
    if (consumer == producer) {
        return std::nullopt;
    }
    auto& slot = impl_->slots[consumer % impl_->capacity];
    std::optional<MockXMdPublication> result{std::move(*slot)};
    slot.reset();
    impl_->consumer.store(consumer + 1, std::memory_order_release);
    return result;
}

std::size_t BoundedMockXMdPublicationSink::size() const noexcept {
    const auto producer = impl_->producer.load(std::memory_order_acquire);
    const auto consumer = impl_->consumer.load(std::memory_order_acquire);
    return producer - consumer;
}

std::size_t BoundedMockXMdPublicationSink::capacity() const noexcept {
    return impl_->capacity;
}

std::shared_ptr<MockXMdMonotonicClock> make_steady_mockx_md_clock() {
    return std::make_shared<SteadyClock>();
}

struct MockXMdSession::Impl final {
    Impl(
        MockXMdSessionConfig value,
        std::shared_ptr<MockXMdPublicationSink> output,
        std::shared_ptr<MockXMdMonotonicClock> time_source)
        : config(std::move(value)),
          sink(std::move(output)),
          clock(time_source ? std::move(time_source) : make_steady_mockx_md_clock()),
          adapter(config.adapter),
          udp_ring(
              config.transport_queue_capacity,
              config.max_udp_datagram_bytes),
          soup_ring(
              config.transport_queue_capacity,
              config.max_soup_frame_bytes) {
        validate_config();
    }

    ~Impl() { stop(); }

    void validate_config() const {
        if (!sink || !clock || config.udp.bind_address.empty() ||
            config.udp.receive_buffer_bytes == 0 ||
            config.glimpse.host.empty() || config.glimpse.port == 0 ||
            config.glimpse.username.empty() || config.glimpse.password.empty() ||
            config.glimpse.username.size() > 6 ||
            config.glimpse.password.size() > 10 ||
            !printable_ascii(config.glimpse.username) ||
            !printable_ascii(config.glimpse.password) ||
            config.glimpse.heartbeat_timeout_ms == 0 ||
            config.glimpse.heartbeat_timeout_ms > 99'999 ||
            config.glimpse.transport_retries > 100 ||
            config.glimpse.connect_timeout.count() <= 0 ||
            config.glimpse.connect_timeout > std::chrono::seconds{60} ||
            config.glimpse.socket_poll_interval.count() <= 0 ||
            config.glimpse.socket_poll_interval > std::chrono::seconds{5} ||
            config.glimpse.initial_retry_backoff.count() <= 0 ||
            config.glimpse.maximum_retry_backoff <
                config.glimpse.initial_retry_backoff ||
            config.glimpse.maximum_retry_backoff > std::chrono::seconds{60} ||
            config.max_udp_datagram_bytes < 20 ||
            config.max_udp_datagram_bytes > 65'507 ||
            config.max_soup_frame_bytes < 33 ||
            config.max_soup_frame_bytes > 65'537 ||
            config.max_udp_datagram_bytes > config.adapter.max_raw_input_bytes ||
            config.max_soup_frame_bytes > config.adapter.max_raw_input_bytes ||
            config.coordinator_drain_batch == 0) {
            throw std::invalid_argument{"invalid MockX market-data session configuration"};
        }
    }

    [[nodiscard]] MockXMdSessionCounters counters_snapshot() const noexcept {
        return MockXMdSessionCounters{
            udp_datagrams_received.load(std::memory_order_relaxed),
            udp_bytes_received.load(std::memory_order_relaxed),
            soup_frames_received.load(std::memory_order_relaxed),
            soup_bytes_received.load(std::memory_order_relaxed),
            raw_queue_losses.load(std::memory_order_relaxed),
            adapter_queue_losses.load(std::memory_order_relaxed),
            engine_updates_published.load(std::memory_order_relaxed),
            publications_rejected.load(std::memory_order_relaxed),
            glimpse_attempts.load(std::memory_order_relaxed),
            glimpse_reconnects.load(std::memory_order_relaxed),
            recovery_requests.load(std::memory_order_relaxed)};
    }

    [[nodiscard]] std::uint64_t now() noexcept {
        const auto value = clock->now_ns();
        return value == 0 ? 1 : value;
    }

    void request_shutdown() noexcept {
        stop_requested.store(true, std::memory_order_release);
        const auto udp = udp_handle.load(std::memory_order_acquire);
        if (udp != -1) {
            shutdown_socket(native_socket(udp));
        }
        const auto tcp = tcp_handle.load(std::memory_order_acquire);
        if (tcp != -1) {
            shutdown_socket(native_socket(tcp));
        }
        wake.notify_all();
        glimpse_wake.notify_all();
    }

    [[nodiscard]] bool publish_payload(MockXMdPublicationPayload payload) noexcept {
        const auto sequence = publication_sequence.fetch_add(
            1,
            std::memory_order_relaxed) + 1;
        if (sink->try_publish(MockXMdPublication{sequence, std::move(payload)})) {
            return true;
        }
        publications_rejected.fetch_add(1, std::memory_order_relaxed);
        last_session_error.store(
            MockXMdSessionError::PublicationSinkFull,
            std::memory_order_release);
        current_state.store(MockXMdSessionState::Failed, std::memory_order_release);
        request_shutdown();
        return false;
    }

    [[nodiscard]] bool transition(
        const MockXMdSessionState next,
        const MockXMdSessionError error = MockXMdSessionError::None) noexcept {
        const auto previous = current_state.exchange(next, std::memory_order_acq_rel);
        if (error != MockXMdSessionError::None) {
            last_session_error.store(error, std::memory_order_release);
        }
        if (previous == next && error == MockXMdSessionError::None) {
            return true;
        }
        const auto generation = health_generation.fetch_add(
            1,
            std::memory_order_relaxed) + 1;
        return publish_payload(MockXMdPublicationPayload{
            std::in_place_type<MockXMdHealthTransition>,
            MockXMdHealthTransition{
                previous,
                next,
                error,
                generation,
                now(),
                counters_snapshot()}});
    }

    [[nodiscard]] bool publish_engine(md::EngineUpdate update) noexcept {
        if (!publish_payload(MockXMdPublicationPayload{
                std::in_place_type<md::EngineUpdate>,
                std::move(update)})) {
            return false;
        }
        engine_updates_published.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void report_fault(
        const FaultKind kind,
        const std::uint64_t generation = 0) noexcept {
        int expected = 0;
        if (!fault_state.compare_exchange_strong(
                expected,
                1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return;
        }
        pending_fault = Fault{kind, generation, now()};
        fault_state.store(2, std::memory_order_release);
        wake.notify_one();
    }

    [[nodiscard]] std::optional<Fault> take_fault() noexcept {
        if (fault_state.load(std::memory_order_acquire) != 2) {
            return std::nullopt;
        }
        const auto value = pending_fault;
        fault_state.store(0, std::memory_order_release);
        return value;
    }

    void commit_raw(
        DirectSpscRing& ring,
        const std::size_t size,
        const std::uint64_t snapshot_generation) noexcept {
        std::lock_guard lock{raw_commit_mutex};
        const auto time = now();
        const auto sequence = next_transport_sequence.fetch_add(
            1,
            std::memory_order_relaxed);
        ring.commit(size, sequence, time, snapshot_generation);
        wake.notify_one();
    }

    [[nodiscard]] bool is_current_snapshot(
        const std::uint64_t generation) const noexcept {
        return !stop_requested.load(std::memory_order_acquire) &&
            requested_snapshot_generation.load(std::memory_order_acquire) == generation;
    }

    void request_snapshot(const bool recovery) {
        if (active_session_key == 0 || active_high_water == 0 ||
            adapter.glimpse_in_progress()) {
            return;
        }
        const auto generation = requested_snapshot_generation.fetch_add(
            1,
            std::memory_order_acq_rel) + 1;
        adapter.begin_glimpse(active_session_key, active_high_water);
        if (recovery) {
            recovery_requests.fetch_add(1, std::memory_order_relaxed);
            (void)transition(MockXMdSessionState::Recovering);
        } else {
            (void)transition(MockXMdSessionState::Snapshotting);
        }
        glimpse_wake.notify_one();
        (void)generation;
    }

    void process_engine_updates(
        std::vector<md::EngineUpdate> updates,
        const bool allow_recovery = true) {
        for (auto& update : updates) {
            const auto target = session_state(update.state);
            if (target != current_state.load(std::memory_order_acquire)) {
                (void)transition(target);
            }
            const auto recovery = update.recovery;
            const auto failed = update.state == MarketDataState::Failed;
            if (!publish_engine(std::move(update))) {
                return;
            }
            if (failed) {
                (void)transition(
                    MockXMdSessionState::Failed,
                    MockXMdSessionError::GlimpseProtocolFailure);
                request_shutdown();
                return;
            }
            if (recovery == md::RecoveryAction::RequestFreshSnapshot &&
                allow_recovery) {
                request_snapshot(true);
            } else if (recovery == md::RecoveryAction::Stop) {
                (void)transition(
                    MockXMdSessionState::Failed,
                    MockXMdSessionError::GlimpseProtocolFailure);
                request_shutdown();
                return;
            }
        }
    }

    [[nodiscard]] bool inspect_mold_identity(
        const std::span<const std::byte> bytes) {
        if (bytes.size() < 20) {
            return false;
        }
        try {
            const auto session = fixed_ascii(bytes.first(10));
            const auto key = mockx_session_key(session);
            const auto first_sequence = load_be(bytes.subspan(10, 8));
            const auto count = static_cast<std::uint16_t>(
                load_be(bytes.subspan(18, 2)));
            if (first_sequence == 0) {
                return false;
            }
            auto high_water = first_sequence;
            if (count != 0 && count != std::numeric_limits<std::uint16_t>::max()) {
                if (first_sequence >
                    std::numeric_limits<std::uint64_t>::max() - count) {
                    return false;
                }
                high_water += count;
            }
            if (active_session_key != key) {
                active_session_key = key;
                active_high_water = high_water;
            } else {
                active_high_water = std::max(active_high_water, high_water);
            }
            return true;
        } catch (const std::invalid_argument&) {
            return false;
        }
    }

    void adapter_enqueue_failed(
        const MockXEnqueueResult result) {
        adapter_queue_losses.fetch_add(1, std::memory_order_relaxed);
        const auto time = now();
        const auto feed_error = result == MockXEnqueueResult::InvalidTimestamp
            ? md::FeedError::ClockRegression
            : (result == MockXEnqueueResult::InputTooLarge
                ? md::FeedError::TruncatedDatagram
                : md::FeedError::RingContinuityLoss);
        std::vector<md::EngineUpdate> updates;
        updates.push_back(adapter.continuity_lost(feed_error, time, time));
        process_engine_updates(std::move(updates));
        const auto transport_error = result == MockXEnqueueResult::InvalidTimestamp
            ? MockXMdSessionError::ClockFailure
            : MockXMdSessionError::AdapterQueueContinuityLoss;
        (void)transition(
            current_state.load(std::memory_order_acquire),
            transport_error);
    }

    void process_udp(const RawView& raw) {
        const auto has_identity = inspect_mold_identity(raw.bytes);
        const auto publication_time = now();
        const auto result = adapter.try_enqueue_mold_datagram(
            raw.bytes,
            raw.receive_time_ns,
            publication_time);
        if (result != MockXEnqueueResult::Accepted) {
            adapter_enqueue_failed(result);
            return;
        }
        process_engine_updates(adapter.drain(1));
        if (has_identity && !adapter.glimpse_in_progress() &&
            (adapter.feed_engine().state() == MarketDataState::Snapshotting ||
             adapter.feed_engine().state() == MarketDataState::Recovering)) {
            request_snapshot(
                adapter.feed_engine().state() == MarketDataState::Recovering);
        }
    }

    void process_soup(const RawView& raw) {
        if (raw.snapshot_generation !=
            requested_snapshot_generation.load(std::memory_order_acquire)) {
            return;
        }
        const auto result = adapter.try_enqueue_soup_bytes(
            raw.bytes,
            raw.receive_time_ns,
            now());
        if (result != MockXEnqueueResult::Accepted) {
            adapter_enqueue_failed(result);
            return;
        }
        process_engine_updates(adapter.drain(1));
    }

    void handle_fault(const Fault& fault) {
        if ((fault.kind == FaultKind::GlimpseTransport ||
             fault.kind == FaultKind::GlimpseAuthentication ||
             fault.kind == FaultKind::GlimpseProtocol) &&
            fault.snapshot_generation != 0 &&
            fault.snapshot_generation !=
                requested_snapshot_generation.load(std::memory_order_acquire)) {
            return;
        }

        udp_ring.discard_all();
        soup_ring.discard_all();
        expected_transport_sequence = next_transport_sequence.load(
            std::memory_order_acquire);
        if (fault.kind == FaultKind::UdpRawLoss) {
            raw_queue_losses.fetch_add(1, std::memory_order_relaxed);
        }

        const auto time = std::max(fault.time_ns, now());
        std::vector<md::EngineUpdate> updates;
        updates.push_back(adapter.continuity_lost(
            fault.kind == FaultKind::UdpTransport ||
                    fault.kind == FaultKind::GlimpseTransport
                ? md::FeedError::TransportFailure
                : md::FeedError::RingContinuityLoss,
            time,
            time));
        const auto terminal = fault_is_terminal(fault.kind);
        process_engine_updates(std::move(updates), !terminal);

        if (terminal) {
            (void)transition(
                MockXMdSessionState::Failed,
                session_error(fault.kind));
            request_shutdown();
        } else {
            (void)transition(
                current_state.load(std::memory_order_acquire),
                session_error(fault.kind));
        }
    }

    void coordinator_loop() noexcept {
        try {
            if (!transition(MockXMdSessionState::Starting) ||
                !transition(MockXMdSessionState::WaitingForLiveIdentity)) {
                running_flag.store(false, std::memory_order_release);
                return;
            }
            while (!stop_requested.load(std::memory_order_acquire)) {
                if (const auto fault = take_fault()) {
                    handle_fault(*fault);
                    continue;
                }

                auto processed = std::size_t{};
                while (processed < config.coordinator_drain_batch &&
                    !stop_requested.load(std::memory_order_acquire)) {
                    const auto udp = udp_ring.try_peek();
                    const auto soup = soup_ring.try_peek();
                    if (!udp && !soup) {
                        break;
                    }
                    const bool choose_udp = udp &&
                        (!soup || udp->transport_sequence < soup->transport_sequence);
                    const auto& selected = choose_udp ? *udp : *soup;
                    if (selected.transport_sequence > expected_transport_sequence) {
                        break;
                    }
                    if (selected.transport_sequence == expected_transport_sequence) {
                        ++expected_transport_sequence;
                        if (choose_udp) {
                            process_udp(selected);
                        } else {
                            process_soup(selected);
                        }
                    }
                    if (choose_udp) {
                        udp_ring.release();
                    } else {
                        soup_ring.release();
                    }
                    ++processed;
                }
                if (processed == 0) {
                    std::unique_lock lock{wake_mutex};
                    wake.wait_for(lock, std::chrono::milliseconds{2});
                }
            }
        } catch (...) {
            (void)transition(
                MockXMdSessionState::Failed,
                MockXMdSessionError::GlimpseProtocolFailure);
            request_shutdown();
        }

        const auto state = current_state.load(std::memory_order_acquire);
        if (state != MockXMdSessionState::Failed) {
            (void)transition(MockXMdSessionState::Stopping);
            (void)transition(MockXMdSessionState::Stopped);
        }
        running_flag.store(false, std::memory_order_release);
    }

    void udp_loop() noexcept {
        try {
            const auto socket = native_socket(udp_handle.load(std::memory_order_acquire));
            while (!stop_requested.load(std::memory_order_acquire)) {
                auto destination = udp_ring.try_acquire();
                if (!destination) {
                    if (!wait_readable(
                            socket,
                            config.glimpse.socket_poll_interval)) {
                        continue;
                    }
                    destination = udp_ring.try_acquire();
                    if (!destination) {
                        report_fault(FaultKind::UdpRawLoss);
                        std::this_thread::yield();
                        continue;
                    }
                }
                const auto received = receive_udp(socket, *destination);
                if (received.kind == ReceiveKind::Timeout) {
                    continue;
                }
                if (received.kind == ReceiveKind::Truncated) {
                    report_fault(FaultKind::UdpRawLoss);
                    continue;
                }
                if (received.kind != ReceiveKind::Data) {
                    report_fault(FaultKind::UdpTransport);
                    return;
                }
                udp_datagrams_received.fetch_add(1, std::memory_order_relaxed);
                udp_bytes_received.fetch_add(received.size, std::memory_order_relaxed);
                commit_raw(udp_ring, received.size, 0);
            }
        } catch (...) {
            if (!stop_requested.load(std::memory_order_acquire)) {
                report_fault(FaultKind::UdpTransport);
            }
        }
    }

    [[nodiscard]] bool heartbeat_due(
        const std::chrono::steady_clock::time_point last) const noexcept {
        const auto half = std::max<std::uint32_t>(
            1,
            config.glimpse.heartbeat_timeout_ms / 2);
        const auto period = std::chrono::milliseconds{
            std::min<std::uint32_t>(1'000, half)};
        return std::chrono::steady_clock::now() - last >= period;
    }

    void read_exact(
        const NativeSocket socket,
        const std::span<std::byte> destination,
        const std::uint64_t generation,
        std::chrono::steady_clock::time_point& last_heartbeat) {
        auto offset = std::size_t{};
        while (offset < destination.size()) {
            if (!is_current_snapshot(generation)) {
                throw SocketFailure{"snapshot request superseded"};
            }
            const auto received = receive_tcp(socket, destination.subspan(offset));
            if (received.kind == ReceiveKind::Data) {
                offset += received.size;
                continue;
            }
            if (received.kind == ReceiveKind::Closed) {
                throw SocketFailure{"TCP peer closed during Soup frame"};
            }
            if (received.kind == ReceiveKind::Timeout) {
                if (heartbeat_due(last_heartbeat)) {
                    const auto heartbeat = soup_heartbeat();
                    send_all(socket, heartbeat);
                    last_heartbeat = std::chrono::steady_clock::now();
                }
                continue;
            }
            throw SocketFailure{"unexpected TCP receive status"};
        }
    }

    [[nodiscard]] bool wait_backoff(
        const std::uint32_t failure,
        const std::uint64_t generation) {
        auto delay = config.glimpse.initial_retry_backoff;
        const auto maximum = config.glimpse.maximum_retry_backoff;
        for (auto count = std::uint32_t{1}; count < failure; ++count) {
            if (delay >= maximum || delay.count() > maximum.count() / 2) {
                delay = maximum;
                break;
            }
            delay *= 2;
        }
        std::unique_lock lock{glimpse_mutex};
        glimpse_wake.wait_for(lock, delay, [&] {
            return !is_current_snapshot(generation);
        });
        return is_current_snapshot(generation);
    }

    void run_glimpse(const std::uint64_t generation) {
        auto requested_session = std::string{};
        auto next_sequence = std::uint64_t{1};
        auto accepted_once = false;
        auto failure = std::uint32_t{};

        while (is_current_snapshot(generation)) {
            try {
                glimpse_attempts.fetch_add(1, std::memory_order_relaxed);
                auto socket = connect_tcp(
                    config.glimpse.host,
                    config.glimpse.port,
                    config.glimpse.connect_timeout,
                    config.glimpse.socket_poll_interval);
                tcp_handle.store(stored_socket(socket.get()), std::memory_order_release);
                const auto login = soup_login(
                    config.glimpse,
                    requested_session,
                    next_sequence);
                send_all(socket.get(), login);
                auto accepted_this_connection = false;
                auto last_heartbeat = std::chrono::steady_clock::now();

                while (is_current_snapshot(generation)) {
                    auto destination = soup_ring.try_acquire();
                    while (!destination && is_current_snapshot(generation)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        destination = soup_ring.try_acquire();
                    }
                    if (!destination) {
                        tcp_handle.store(-1, std::memory_order_release);
                        return;
                    }
                    read_exact(
                        socket.get(),
                        destination->first(2),
                        generation,
                        last_heartbeat);
                    const auto body_size = static_cast<std::size_t>(
                        load_be(std::span<const std::byte>{*destination}.first(2)));
                    if (body_size == 0 || body_size > destination->size() - 2) {
                        tcp_handle.store(-1, std::memory_order_release);
                        report_fault(FaultKind::GlimpseProtocol, generation);
                        return;
                    }
                    read_exact(
                        socket.get(),
                        destination->subspan(2, body_size),
                        generation,
                        last_heartbeat);
                    const auto frame_size = body_size + 2;
                    const auto type = static_cast<char>(
                        std::to_integer<unsigned char>((*destination)[2]));
                    const auto payload = std::span<const std::byte>{*destination}.subspan(
                        3,
                        body_size - 1);
                    auto publish_frame = true;

                    if (type == 'A') {
                        if (accepted_this_connection || payload.size() != 30) {
                            tcp_handle.store(-1, std::memory_order_release);
                            report_fault(FaultKind::GlimpseProtocol, generation);
                            return;
                        }
                        try {
                            const auto session = fixed_ascii(payload.first(10));
                            const auto sequence = padded_decimal(payload.subspan(10, 20));
                            if (sequence != next_sequence ||
                                (!requested_session.empty() &&
                                 requested_session != session)) {
                                throw std::invalid_argument{"resume identity mismatch"};
                            }
                            requested_session = session;
                        } catch (const std::invalid_argument&) {
                            tcp_handle.store(-1, std::memory_order_release);
                            report_fault(FaultKind::GlimpseProtocol, generation);
                            return;
                        }
                        accepted_this_connection = true;
                        publish_frame = !accepted_once;
                        accepted_once = true;
                    } else if (type == 'J') {
                        tcp_handle.store(-1, std::memory_order_release);
                        const auto authentication = payload.size() == 1 &&
                            payload.front() == std::byte{'A'};
                        report_fault(
                            authentication
                                ? FaultKind::GlimpseAuthentication
                                : FaultKind::GlimpseProtocol,
                            generation);
                        return;
                    } else if (!accepted_this_connection) {
                        tcp_handle.store(-1, std::memory_order_release);
                        report_fault(FaultKind::GlimpseProtocol, generation);
                        return;
                    } else if (type == 'S') {
                        if (payload.empty() ||
                            next_sequence == std::numeric_limits<std::uint64_t>::max()) {
                            tcp_handle.store(-1, std::memory_order_release);
                            report_fault(FaultKind::GlimpseProtocol, generation);
                            return;
                        }
                    } else if (type == 'H') {
                        if (!payload.empty()) {
                            tcp_handle.store(-1, std::memory_order_release);
                            report_fault(FaultKind::GlimpseProtocol, generation);
                            return;
                        }
                    } else if (type == 'Z') {
                        if (!payload.empty()) {
                            tcp_handle.store(-1, std::memory_order_release);
                            report_fault(FaultKind::GlimpseProtocol, generation);
                            return;
                        }
                    } else if (type != '+' && type != 'U') {
                        tcp_handle.store(-1, std::memory_order_release);
                        report_fault(FaultKind::GlimpseProtocol, generation);
                        return;
                    }

                    if (publish_frame) {
                        soup_frames_received.fetch_add(1, std::memory_order_relaxed);
                        soup_bytes_received.fetch_add(frame_size, std::memory_order_relaxed);
                        commit_raw(soup_ring, frame_size, generation);
                    }
                    if (type == 'S') {
                        ++next_sequence;
                    }
                    if (type == 'Z') {
                        tcp_handle.store(-1, std::memory_order_release);
                        return;
                    }
                }
                tcp_handle.store(-1, std::memory_order_release);
                return;
            } catch (const SocketFailure&) {
                tcp_handle.store(-1, std::memory_order_release);
                if (!is_current_snapshot(generation)) {
                    return;
                }
                ++failure;
                if (failure > config.glimpse.transport_retries) {
                    report_fault(FaultKind::GlimpseTransport, generation);
                    return;
                }
                glimpse_reconnects.fetch_add(1, std::memory_order_relaxed);
                if (!wait_backoff(failure, generation)) {
                    return;
                }
            }
        }
    }

    void glimpse_loop() noexcept {
        auto handled_generation = std::uint64_t{};
        try {
            while (!stop_requested.load(std::memory_order_acquire)) {
                const auto requested = requested_snapshot_generation.load(
                    std::memory_order_acquire);
                if (requested == 0 || requested == handled_generation) {
                    std::unique_lock lock{glimpse_mutex};
                    glimpse_wake.wait_for(lock, std::chrono::milliseconds{10});
                    continue;
                }
                handled_generation = requested;
                run_glimpse(requested);
            }
        } catch (...) {
            if (!stop_requested.load(std::memory_order_acquire)) {
                report_fault(FaultKind::GlimpseTransport, handled_generation);
            }
        }
        tcp_handle.store(-1, std::memory_order_release);
    }

    void start() {
        std::lock_guard lock{lifecycle_mutex};
        if (started.exchange(true, std::memory_order_acq_rel)) {
            throw std::logic_error{"MockX market-data session is one-shot"};
        }
        auto socket = open_udp(
            config.udp,
            config.glimpse.socket_poll_interval,
            bound_udp_port);
        udp_handle.store(stored_socket(socket.get()), std::memory_order_release);
        udp_socket = std::move(socket);
        stop_requested.store(false, std::memory_order_release);
        running_flag.store(true, std::memory_order_release);
        try {
            coordinator_thread = std::thread{[this] { coordinator_loop(); }};
            glimpse_thread = std::thread{[this] { glimpse_loop(); }};
            udp_thread = std::thread{[this] { udp_loop(); }};
        } catch (...) {
            request_shutdown();
            if (udp_thread.joinable()) {
                udp_thread.join();
            }
            if (glimpse_thread.joinable()) {
                glimpse_thread.join();
            }
            if (coordinator_thread.joinable()) {
                coordinator_thread.join();
            }
            udp_socket.reset();
            udp_handle.store(-1, std::memory_order_release);
            running_flag.store(false, std::memory_order_release);
            throw;
        }
    }

    void stop() noexcept {
        std::lock_guard lock{lifecycle_mutex};
        if (!started.load(std::memory_order_acquire)) {
            return;
        }
        request_shutdown();
        if (udp_thread.joinable()) {
            udp_thread.join();
        }
        if (glimpse_thread.joinable()) {
            glimpse_thread.join();
        }
        if (coordinator_thread.joinable()) {
            coordinator_thread.join();
        }
        udp_socket.reset();
        udp_handle.store(-1, std::memory_order_release);
        running_flag.store(false, std::memory_order_release);
    }

    MockXMdSessionConfig config;
    std::shared_ptr<MockXMdPublicationSink> sink;
    std::shared_ptr<MockXMdMonotonicClock> clock;
    NetworkRuntime network;
    MockXMdAdapter adapter;
    DirectSpscRing udp_ring;
    DirectSpscRing soup_ring;
    SocketOwner udp_socket;

    std::mutex lifecycle_mutex;
    std::mutex wake_mutex;
    std::condition_variable wake;
    std::mutex glimpse_mutex;
    std::condition_variable glimpse_wake;
    std::mutex raw_commit_mutex;
    std::thread coordinator_thread;
    std::thread udp_thread;
    std::thread glimpse_thread;

    std::atomic<bool> started{};
    std::atomic<bool> stop_requested{};
    std::atomic<bool> running_flag{};
    std::atomic<std::intptr_t> udp_handle{-1};
    std::atomic<std::intptr_t> tcp_handle{-1};
    std::uint16_t bound_udp_port{};

    std::atomic<MockXMdSessionState> current_state{MockXMdSessionState::Stopped};
    std::atomic<MockXMdSessionError> last_session_error{MockXMdSessionError::None};
    std::atomic<std::uint64_t> health_generation{};
    std::atomic<std::uint64_t> publication_sequence{};
    std::atomic<std::uint64_t> next_transport_sequence{1};
    std::uint64_t expected_transport_sequence{1};
    std::atomic<std::uint64_t> requested_snapshot_generation{};
    md::FeedSessionKey active_session_key{};
    std::uint64_t active_high_water{};

    std::atomic<int> fault_state{};
    Fault pending_fault{};

    std::atomic<std::uint64_t> udp_datagrams_received{};
    std::atomic<std::uint64_t> udp_bytes_received{};
    std::atomic<std::uint64_t> soup_frames_received{};
    std::atomic<std::uint64_t> soup_bytes_received{};
    std::atomic<std::uint64_t> raw_queue_losses{};
    std::atomic<std::uint64_t> adapter_queue_losses{};
    std::atomic<std::uint64_t> engine_updates_published{};
    std::atomic<std::uint64_t> publications_rejected{};
    std::atomic<std::uint64_t> glimpse_attempts{};
    std::atomic<std::uint64_t> glimpse_reconnects{};
    std::atomic<std::uint64_t> recovery_requests{};
};

MockXMdSession::MockXMdSession(
    MockXMdSessionConfig config,
    std::shared_ptr<MockXMdPublicationSink> sink,
    std::shared_ptr<MockXMdMonotonicClock> clock)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          std::move(sink),
          std::move(clock))) {}

MockXMdSession::~MockXMdSession() = default;

void MockXMdSession::start() { impl_->start(); }

void MockXMdSession::stop() noexcept { impl_->stop(); }

bool MockXMdSession::running() const noexcept {
    return impl_->running_flag.load(std::memory_order_acquire);
}

MockXMdSessionState MockXMdSession::state() const noexcept {
    return impl_->current_state.load(std::memory_order_acquire);
}

MockXMdSessionError MockXMdSession::last_error() const noexcept {
    return impl_->last_session_error.load(std::memory_order_acquire);
}

MockXMdSessionCounters MockXMdSession::counters() const noexcept {
    return impl_->counters_snapshot();
}

std::uint16_t MockXMdSession::local_udp_port() const noexcept {
    return impl_->bound_udp_port;
}

}  // namespace sorx::adapters








