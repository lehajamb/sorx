#pragma once

#include "sorx/adapters/mockx_md.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace sorx::adapters {

enum class MockXMdSessionState : std::uint8_t {
    Stopped,
    Starting,
    WaitingForLiveIdentity,
    Snapshotting,
    Stitching,
    Live,
    Recovering,
    Stopping,
    Failed,
};

enum class MockXMdSessionError : std::uint8_t {
    None,
    UdpTransportFailure,
    GlimpseTransportFailure,
    GlimpseAuthenticationRejected,
    GlimpseProtocolFailure,
    RawQueueContinuityLoss,
    AdapterQueueContinuityLoss,
    PublicationSinkFull,
    ClockFailure,
};

struct MockXMdSessionCounters final {
    std::uint64_t udp_datagrams_received{};
    std::uint64_t udp_bytes_received{};
    std::uint64_t soup_frames_received{};
    std::uint64_t soup_bytes_received{};
    std::uint64_t raw_queue_losses{};
    std::uint64_t adapter_queue_losses{};
    std::uint64_t engine_updates_published{};
    std::uint64_t publications_rejected{};
    std::uint64_t glimpse_attempts{};
    std::uint64_t glimpse_reconnects{};
    std::uint64_t recovery_requests{};
};

struct MockXMdHealthTransition final {
    MockXMdSessionState previous{MockXMdSessionState::Stopped};
    MockXMdSessionState current{MockXMdSessionState::Stopped};
    MockXMdSessionError error{MockXMdSessionError::None};
    std::uint64_t generation{};
    std::uint64_t monotonic_time_ns{};
    MockXMdSessionCounters counters;
};

using MockXMdPublicationPayload =
    std::variant<md::EngineUpdate, MockXMdHealthTransition>;

struct MockXMdPublication final {
    std::uint64_t sequence{};
    MockXMdPublicationPayload payload;
};

// Implementations must return immediately. A false return means the event was
// not retained and causes the session to fail closed.
class MockXMdPublicationSink {
public:
    virtual ~MockXMdPublicationSink() = default;
    [[nodiscard]] virtual bool try_publish(
        MockXMdPublication publication) noexcept = 0;
};

// A bounded SPSC sink. The session is the sole producer and the application
// must use a single consumer for try_pop().
class BoundedMockXMdPublicationSink final : public MockXMdPublicationSink {
public:
    explicit BoundedMockXMdPublicationSink(std::size_t capacity);
    ~BoundedMockXMdPublicationSink() override;

    BoundedMockXMdPublicationSink(const BoundedMockXMdPublicationSink&) = delete;
    BoundedMockXMdPublicationSink& operator=(
        const BoundedMockXMdPublicationSink&) = delete;
    BoundedMockXMdPublicationSink(BoundedMockXMdPublicationSink&&) = delete;
    BoundedMockXMdPublicationSink& operator=(
        BoundedMockXMdPublicationSink&&) = delete;

    [[nodiscard]] bool try_publish(
        MockXMdPublication publication) noexcept override;
    [[nodiscard]] std::optional<MockXMdPublication> try_pop() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MockXMdMonotonicClock {
public:
    virtual ~MockXMdMonotonicClock() = default;
    [[nodiscard]] virtual std::uint64_t now_ns() noexcept = 0;
};

[[nodiscard]] std::shared_ptr<MockXMdMonotonicClock>
make_steady_mockx_md_clock();

struct MockXMdUdpEndpoint final {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{18'000};
    // Empty selects unicast. Otherwise this must be an IPv4 multicast group.
    std::string multicast_group;
    std::string multicast_interface{"0.0.0.0"};
    std::size_t receive_buffer_bytes{4U * 1'024U * 1'024U};
};

struct MockXGlimpseEndpoint final {
    std::string host{"127.0.0.1"};
    std::uint16_t port{18'001};
    std::string username{"GLIMP"};
    std::string password;
    std::uint32_t heartbeat_timeout_ms{30'000};
    std::uint32_t transport_retries{3};
    std::chrono::milliseconds connect_timeout{2'000};
    std::chrono::milliseconds socket_poll_interval{100};
    std::chrono::milliseconds initial_retry_backoff{50};
    std::chrono::milliseconds maximum_retry_backoff{1'000};
};

struct MockXMdSessionConfig final {
    MockXMdConfig adapter;
    MockXMdUdpEndpoint udp;
    MockXGlimpseEndpoint glimpse;
    std::size_t transport_queue_capacity{1'024};
    std::size_t max_udp_datagram_bytes{65'507};
    std::size_t max_soup_frame_bytes{131'072};
    std::size_t coordinator_drain_batch{256};
};

// Owns the MockX wire transports and their threads. All feed-engine mutation
// remains confined to the coordinator thread.
class MockXMdSession final {
public:
    MockXMdSession(
        MockXMdSessionConfig config,
        std::shared_ptr<MockXMdPublicationSink> sink,
        std::shared_ptr<MockXMdMonotonicClock> clock = {});
    ~MockXMdSession();

    MockXMdSession(const MockXMdSession&) = delete;
    MockXMdSession& operator=(const MockXMdSession&) = delete;
    MockXMdSession(MockXMdSession&&) = delete;
    MockXMdSession& operator=(MockXMdSession&&) = delete;

    // A session instance is one-shot: start() may be called exactly once.
    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] MockXMdSessionState state() const noexcept;
    [[nodiscard]] MockXMdSessionError last_error() const noexcept;
    [[nodiscard]] MockXMdSessionCounters counters() const noexcept;
    // Useful for port-zero loopback tests and ephemeral deployments.
    [[nodiscard]] std::uint16_t local_udp_port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::adapters
