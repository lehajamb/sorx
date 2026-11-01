#pragma once

#include "sorx/book_view.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace sorx::md {

using FeedInstrumentKey = std::uint32_t;
using FeedSessionKey = std::uint64_t;
using OrderReference = std::uint64_t;

enum class RecoveryStrategy : std::uint8_t {
    FreshSnapshot,
};

enum class RecoveryAction : std::uint8_t {
    None,
    RequestFreshSnapshot,
    Stop,
};

enum class FeedError : std::uint8_t {
    None,
    InvalidState,
    ClockRegression,
    MalformedFrame,
    SequenceGap,
    NewSession,
    RingContinuityLoss,
    TruncatedDatagram,
    InvalidSnapshot,
    InvalidEvent,
    BufferCapacityExceeded,
    UnsupportedMessage,
    TransportFailure,
    EndOfSession,
};

struct FeedEngineConfig final {
    VenueId venue{};
    ChannelId channel{};
    ProfileId profile{};
    std::size_t publication_depth{kMaxDepthLevels};
    RecoveryStrategy recovery_strategy{RecoveryStrategy::FreshSnapshot};
    std::size_t stitch_buffer_packet_capacity{1'024};
    std::size_t stitch_buffer_event_capacity{65'536};
};

struct InstrumentDirectoryEvent final {
    FeedInstrumentKey key{};
    InstrumentId instrument{};
    std::int64_t exchange_timestamp_ns{};
};

struct TradingStateEvent final {
    FeedInstrumentKey key{};
    InstrumentId instrument{};
    InstrumentState state{InstrumentState::Unknown};
    std::int64_t exchange_timestamp_ns{};
};

struct AddOrderEvent final {
    OrderReference order_reference{};
    FeedInstrumentKey key{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price price{};
    std::int64_t exchange_timestamp_ns{};
};

struct ExecuteOrderEvent final {
    OrderReference order_reference{};
    FeedInstrumentKey key{};
    Quantity quantity{};
    std::int64_t exchange_timestamp_ns{};
};

struct DeleteOrderEvent final {
    OrderReference order_reference{};
    FeedInstrumentKey key{};
    std::int64_t exchange_timestamp_ns{};
};

// A profile emits this only after validating the exact wire type and length.
// The deterministic book intentionally ignores its payload.
struct KnownBookNeutralEvent final {
    std::int64_t exchange_timestamp_ns{};
};

// Some profiles carry a sequenced end marker inside the data stream rather
// than in the framing layer.
struct EndOfSessionEvent final {
    std::int64_t exchange_timestamp_ns{};
};

using FeedEvent = std::variant<
    InstrumentDirectoryEvent,
    TradingStateEvent,
    AddOrderEvent,
    ExecuteOrderEvent,
    DeleteOrderEvent,
    KnownBookNeutralEvent,
    EndOfSessionEvent>;

struct SnapshotImage final {
    std::span<const FeedEvent> events;
    std::uint64_t next_sequence{1};
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};

    // The snapshot is a cut of this exact sequenced live session. The high
    // water is end-exclusive: every sequence in [next_sequence,
    // live_high_water_sequence) must be applied before LIVE is permitted.
    FeedSessionKey session_key{};
    std::uint64_t live_high_water_sequence{};
};

// This is a protocol-neutral representation of one already length-decoded
// sequenced frame. declared_message_count retains the framing contract:
// 0 is a heartbeat and 0xffff is end-of-session; both require no messages.
struct PacketFrame final {
    FeedSessionKey session_key{};
    std::uint64_t first_sequence{1};
    std::uint16_t declared_message_count{};
    std::span<const FeedEvent> events;
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};
};

struct EngineUpdate final {
    MarketDataState state{MarketDataState::Snapshotting};
    FeedError error{FeedError::None};
    RecoveryAction recovery{RecoveryAction::None};
    bool state_changed{};
    bool end_of_session{};
    std::size_t applied_message_count{};
    std::size_t skipped_message_count{};
    std::uint64_t publication_generation{};
    std::vector<BookView> publications;

    [[nodiscard]] bool healthy() const noexcept {
        return state == MarketDataState::Live && error == FeedError::None;
    }
};

class FeedEngine final {
public:
    explicit FeedEngine(FeedEngineConfig config);
    ~FeedEngine();

    FeedEngine(const FeedEngine&) = delete;
    FeedEngine& operator=(const FeedEngine&) = delete;
    FeedEngine(FeedEngine&&) noexcept;
    FeedEngine& operator=(FeedEngine&&) noexcept;

    [[nodiscard]] EngineUpdate apply_snapshot(const SnapshotImage& snapshot);
    [[nodiscard]] EngineUpdate apply_packet(const PacketFrame& packet);
    [[nodiscard]] EngineUpdate complete_stitch(
        std::uint64_t local_publication_time_ns);

    // Recoverable continuity failures synchronously publish a non-LIVE
    // generation before asking the configured recovery strategy for a cut.
    [[nodiscard]] EngineUpdate continuity_lost(
        FeedError error,
        std::uint64_t local_receive_time_ns,
        std::uint64_t local_publication_time_ns);

    // Permanent profile/transport failures synchronously withdraw the feed.
    [[nodiscard]] EngineUpdate fail(
        FeedError error,
        std::uint64_t local_receive_time_ns,
        std::uint64_t local_publication_time_ns);

    [[nodiscard]] MarketDataState state() const noexcept;
    [[nodiscard]] std::uint64_t next_sequence() const noexcept;
    [[nodiscard]] std::uint64_t last_applied_sequence() const noexcept;
    [[nodiscard]] std::uint64_t feed_session_epoch() const noexcept;
    [[nodiscard]] std::uint64_t recovery_generation() const noexcept;
    [[nodiscard]] std::uint64_t publication_generation() const noexcept;
    [[nodiscard]] FeedSessionKey active_session_key() const noexcept;
    [[nodiscard]] std::uint64_t stitch_cut_sequence() const noexcept;
    [[nodiscard]] std::uint64_t stitch_high_water_sequence() const noexcept;
    [[nodiscard]] std::size_t buffered_packet_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::md
