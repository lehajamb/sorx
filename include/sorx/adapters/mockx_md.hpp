#pragma once

#include "sorx/md/feed_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sorx::adapters {

struct MockXInstrumentBinding final {
    std::uint16_t stock_locate{};
    std::string symbol;
    InstrumentId instrument{};
};

struct MockXMdConfig final {
    md::FeedEngineConfig feed;
    std::vector<MockXInstrumentBinding> instruments;
    std::size_t raw_queue_capacity{1'024};
    std::size_t max_raw_input_bytes{65'507};
    std::size_t max_messages_per_mold_packet{4'096};
    std::size_t max_glimpse_messages{100'000};
    std::size_t max_soup_buffer_bytes{131'072};
};

enum class MockXEnqueueResult : std::uint8_t {
    Accepted,
    QueueFull,
    InputTooLarge,
    InvalidTimestamp,
};

// Stable, non-zero FNV-1a identity for the trimmed ten-byte Mold session.
[[nodiscard]] md::FeedSessionKey mockx_session_key(std::string_view session);

// Deterministic MockX protocol profile plus a preallocated bounded SPSC raw
// input seam. Socket ownership, multicast membership, TCP login writes, retry
// policy, and thread lifecycle remain runtime concerns outside this class.
class MockXMdAdapter final {
public:
    explicit MockXMdAdapter(MockXMdConfig config);
    ~MockXMdAdapter();

    MockXMdAdapter(const MockXMdAdapter&) = delete;
    MockXMdAdapter& operator=(const MockXMdAdapter&) = delete;
    MockXMdAdapter(MockXMdAdapter&&) = delete;
    MockXMdAdapter& operator=(MockXMdAdapter&&) = delete;

    // live_high_water_sequence is end-exclusive and must describe the live
    // Mold session captured alongside this fresh GLIMPSE spin.
    void begin_glimpse(
        md::FeedSessionKey live_session_key,
        std::uint64_t live_high_water_sequence);

    [[nodiscard]] MockXEnqueueResult try_enqueue_mold_datagram(
        std::span<const std::byte> bytes,
        std::uint64_t local_receive_time_ns,
        std::uint64_t local_publication_time_ns) noexcept;

    // Bytes may contain a partial SoupBinTCP frame, one frame, or several
    // frames. Fragment state is bounded by max_soup_buffer_bytes.
    [[nodiscard]] MockXEnqueueResult try_enqueue_soup_bytes(
        std::span<const std::byte> bytes,
        std::uint64_t local_receive_time_ns,
        std::uint64_t local_publication_time_ns) noexcept;

    [[nodiscard]] std::vector<md::EngineUpdate> drain(
        std::size_t max_inputs = std::numeric_limits<std::size_t>::max());

    // Transport orchestration uses this seam when bytes are lost before they
    // can enter the adapter's own bounded raw queue. It synchronously
    // invalidates every previously published LIVE view.
    [[nodiscard]] md::EngineUpdate continuity_lost(
        md::FeedError error,
        std::uint64_t local_receive_time_ns,
        std::uint64_t local_publication_time_ns);

    [[nodiscard]] const md::FeedEngine& feed_engine() const noexcept;
    [[nodiscard]] std::size_t queued_input_count() const noexcept;
    [[nodiscard]] bool glimpse_in_progress() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::adapters


