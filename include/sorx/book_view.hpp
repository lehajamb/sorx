#pragma once

#include "sorx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sorx::md {

inline constexpr std::size_t kMaxDepthLevels = 32;

struct Level final {
    Price price{};
    Quantity quantity{};
    std::uint32_t order_count{};

    constexpr auto operator<=>(const Level&) const noexcept = default;
};

struct BookVersion final {
    VenueId venue{};
    InstrumentId instrument{};
    std::uint64_t feed_session_epoch{};
    std::uint64_t recovery_generation{};
    std::uint64_t view_generation{};
    std::uint64_t last_applied_sequence{};
    std::uint64_t next_sequence{};

    constexpr auto operator<=>(const BookVersion&) const noexcept = default;
};

struct BookView final {
    VenueId venue{};
    ChannelId channel{};
    ProfileId profile{};
    InstrumentId instrument{};

    std::array<Level, kMaxDepthLevels> bids{};
    std::array<Level, kMaxDepthLevels> asks{};
    std::uint16_t bid_count{};
    std::uint16_t ask_count{};

    std::uint64_t feed_session_epoch{};
    std::uint64_t recovery_generation{};
    std::uint64_t view_generation{};
    std::uint64_t last_applied_sequence{};
    std::uint64_t next_sequence{};

    std::int64_t exchange_timestamp_ns{};
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};

    MarketDataState feed_state{MarketDataState::Snapshotting};
    InstrumentState trading_state{InstrumentState::Unknown};
    bool depth_truncated{};

    [[nodiscard]] constexpr bool push_bid(const Level level) noexcept {
        if (bid_count >= kMaxDepthLevels) {
            depth_truncated = true;
            return false;
        }
        bids[bid_count++] = level;
        return true;
    }

    [[nodiscard]] constexpr bool push_ask(const Level level) noexcept {
        if (ask_count >= kMaxDepthLevels) {
            depth_truncated = true;
            return false;
        }
        asks[ask_count++] = level;
        return true;
    }

    constexpr void clear_depth() noexcept {
        bid_count = 0;
        ask_count = 0;
        depth_truncated = false;
    }

    [[nodiscard]] constexpr BookVersion version() const noexcept {
        return BookVersion{
            venue,
            instrument,
            feed_session_epoch,
            recovery_generation,
            view_generation,
            last_applied_sequence,
            next_sequence,
        };
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        const bool valid_feed_state =
            feed_state == MarketDataState::Snapshotting ||
            feed_state == MarketDataState::Stitching ||
            feed_state == MarketDataState::Live ||
            feed_state == MarketDataState::Recovering ||
            feed_state == MarketDataState::Failed;
        const bool valid_trading_state =
            trading_state == InstrumentState::Unknown ||
            trading_state == InstrumentState::Trading ||
            trading_state == InstrumentState::Halted ||
            trading_state == InstrumentState::Closed;
        if (!venue || !channel || !profile || !instrument ||
            feed_session_epoch == 0 || recovery_generation == 0 ||
            view_generation == 0 || bid_count > kMaxDepthLevels ||
            ask_count > kMaxDepthLevels || exchange_timestamp_ns < 0 ||
            local_publication_time_ns < local_receive_time_ns ||
            !valid_feed_state || !valid_trading_state) {
            return false;
        }
        if (last_applied_sequence == UINT64_MAX ||
            next_sequence != last_applied_sequence + 1) {
            return false;
        }

        for (std::size_t index = 0; index < bid_count; ++index) {
            if (bids[index].price.units <= 0 || !bids[index].quantity ||
                bids[index].order_count == 0) {
                return false;
            }
            if (index != 0 && bids[index - 1].price <= bids[index].price) {
                return false;
            }
        }
        for (std::size_t index = 0; index < ask_count; ++index) {
            if (asks[index].price.units <= 0 || !asks[index].quantity ||
                asks[index].order_count == 0) {
                return false;
            }
            if (index != 0 && asks[index - 1].price >= asks[index].price) {
                return false;
            }
        }
        if (bid_count != 0U && ask_count != 0U &&
            bids[0].price >= asks[0].price) {
            return false;
        }
        return true;
    }
};

static_assert(std::is_trivially_copyable_v<BookView>);
static_assert(std::is_standard_layout_v<BookView>);

}  // namespace sorx::md


