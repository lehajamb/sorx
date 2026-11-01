#pragma once

#include "sorx/book_view.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace sorx::marketdata {

using OrderReference = std::uint64_t;

enum class L3Operation : std::uint8_t {
    Add = 1U,
    Modify = 2U,
    Delete = 3U,
};

struct L3Event final {
    L3Operation operation{L3Operation::Add};
    OrderReference order_reference{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    std::int64_t exchange_timestamp_ns{};
};

struct FixedL3BookConfig final {
    VenueId venue{};
    ChannelId channel{};
    ProfileId profile{};
    InstrumentId instrument{};
    std::size_t order_capacity{};
    std::size_t order_index_capacity{};
    std::size_t price_level_capacity{};
    std::size_t maximum_events_per_packet{};
    InstrumentState trading_state{InstrumentState::Trading};
};

struct BookPacketContext final {
    std::uint64_t feed_session_epoch{};
    std::uint64_t recovery_generation{};
    std::uint64_t last_applied_sequence{};
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};
    MarketDataState feed_state{MarketDataState::Live};
};

enum class BookApplyStatus : std::uint8_t {
    Applied,
    NoChange,
    MalformedEvent,
    DuplicateOrderReference,
    UnknownOrderReference,
    OrderCapacityExceeded,
    OrderIndexCapacityExceeded,
    PriceLevelCapacityExceeded,
    UndoCapacityExceeded,
    AggregateOverflow,
    CrossedBook,
    DecodeFailure,
    InvalidContext,
};

struct BookApplyResult final {
    BookApplyStatus status{BookApplyStatus::Applied};
    std::size_t applied_event_count{};
    // Empty when the packet changed no book state. Otherwise this contains
    // exactly the changed instrument view and remains valid until the next
    // non-const operation on the book.
    std::span<const md::BookView> publications;

    [[nodiscard]] bool accepted() const noexcept {
        return status == BookApplyStatus::Applied ||
               status == BookApplyStatus::NoChange;
    }
};

enum class DecodeStep : std::uint8_t {
    Event,
    End,
    Malformed,
};

using DecodeNextEvent = DecodeStep (*)(
    std::span<const std::byte> registered_buffer,
    std::size_t& cursor,
    L3Event& event) noexcept;

struct OrderSnapshot final {
    OrderReference order_reference{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
};

struct PriceLevelSnapshot final {
    Side side{Side::Buy};
    Price price{};
    Quantity aggregate_quantity{};
    std::uint32_t order_count{};
};

struct FixedL3BookMetadata final {
    std::size_t order_capacity{};
    std::size_t order_index_capacity{};
    std::size_t price_level_capacity{};
    std::size_t maximum_events_per_packet{};
    std::size_t undo_capacity{};
    std::size_t active_orders{};
    std::size_t active_price_levels{};
    std::size_t occupied_index_entries{};
    std::size_t index_tombstones{};
    std::uint64_t view_generation{};
};

class FixedL3Book final {
public:
    explicit FixedL3Book(FixedL3BookConfig config);
    ~FixedL3Book();

    FixedL3Book(const FixedL3Book&) = delete;
    FixedL3Book& operator=(const FixedL3Book&) = delete;
    FixedL3Book(FixedL3Book&&) noexcept;
    FixedL3Book& operator=(FixedL3Book&&) noexcept;

    [[nodiscard]] BookApplyResult apply_packet(
        const BookPacketContext& context,
        std::span<const L3Event> events) noexcept;

    // The decoder reads directly from the caller-owned span (for example a
    // registered NIC RX buffer). Only one fixed-size L3Event is materialized
    // at a time; the payload is never copied or retained.
    [[nodiscard]] BookApplyResult decode_and_apply(
        const BookPacketContext& context,
        std::span<const std::byte> registered_buffer,
        std::size_t declared_event_count,
        DecodeNextEvent decode_next) noexcept;

    [[nodiscard]] std::optional<OrderSnapshot> order(
        OrderReference reference) const noexcept;
    [[nodiscard]] std::optional<PriceLevelSnapshot> level(
        Side side,
        Price price) const noexcept;
    [[nodiscard]] std::size_t copy_fifo(
        Side side,
        Price price,
        std::span<OrderReference> destination) const noexcept;
    [[nodiscard]] FixedL3BookMetadata metadata() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Canonical test/reference decoder. Each big-endian record is 36 bytes:
// operation:u8, side:u8, reserved:u16, reference:u64, price:i64,
// quantity:u64, exchange_timestamp_ns:i64.
[[nodiscard]] DecodeStep decode_canonical_l3_event(
    std::span<const std::byte> registered_buffer,
    std::size_t& cursor,
    L3Event& event) noexcept;

}  // namespace sorx::marketdata
