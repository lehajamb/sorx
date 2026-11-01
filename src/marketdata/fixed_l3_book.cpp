#include "sorx/marketdata/fixed_l3_book.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sorx::marketdata {
namespace {

constexpr std::uint32_t kInvalidSlot =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kCanonicalEventBytes = 36U;
constexpr std::size_t kUndoEntriesPerEvent = 12U;

[[nodiscard]] constexpr bool valid_side(const Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] constexpr bool valid_feed_state(
    const MarketDataState state) noexcept {
    return state == MarketDataState::Snapshotting ||
           state == MarketDataState::Stitching ||
           state == MarketDataState::Live ||
           state == MarketDataState::Recovering ||
           state == MarketDataState::Failed;
}

[[nodiscard]] constexpr bool valid_trading_state(
    const InstrumentState state) noexcept {
    return state == InstrumentState::Unknown ||
           state == InstrumentState::Trading ||
           state == InstrumentState::Halted ||
           state == InstrumentState::Closed;
}

[[nodiscard]] std::uint64_t read_u64(
    const std::byte* const source) noexcept {
    std::uint64_t result = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        result = (result << 8U) |
                 std::to_integer<std::uint8_t>(source[index]);
    }
    return result;
}

}  // namespace

struct FixedL3Book::Impl final {
    struct OrderNode final {
        OrderReference reference{};
        Price price{};
        Quantity quantity{};
        Side side{Side::Buy};
        std::uint32_t level_slot{kInvalidSlot};
        std::uint32_t previous{kInvalidSlot};
        std::uint32_t next{kInvalidSlot};
        std::uint32_t free_next{kInvalidSlot};
        bool active{};
    };

    struct LevelNode final {
        Price price{};
        Quantity aggregate{};
        Side side{Side::Buy};
        std::uint32_t head{kInvalidSlot};
        std::uint32_t tail{kInvalidSlot};
        std::uint32_t order_count{};
        std::uint32_t free_next{kInvalidSlot};
        bool active{};
    };

    enum class IndexState : std::uint8_t {
        Empty,
        Occupied,
        Tombstone,
    };

    struct IndexEntry final {
        OrderReference reference{};
        std::uint32_t order_slot{kInvalidSlot};
        IndexState state{IndexState::Empty};
    };

    struct ScalarSnapshot final {
        std::uint32_t order_free_head{kInvalidSlot};
        std::uint32_t level_free_head{kInvalidSlot};
        std::size_t active_orders{};
        std::size_t active_levels{};
        std::size_t occupied_index{};
        std::size_t index_tombstones{};
    };

    enum class UndoKind : std::uint8_t {
        Scalars,
        Order,
        Level,
        Hash,
        SortedInserted,
        SortedErased,
    };

    struct UndoEntry final {
        UndoKind kind{UndoKind::Scalars};
        std::uint32_t slot{};
        std::uint32_t position{};
        std::uint32_t sorted_value{};
        Side side{Side::Buy};
        ScalarSnapshot scalars{};
        OrderNode order{};
        LevelNode level{};
        IndexEntry hash{};
    };

    explicit Impl(const FixedL3BookConfig supplied)
        : config(supplied),
          orders(std::make_unique<OrderNode[]>(supplied.order_capacity)),
          levels(std::make_unique<LevelNode[]>(
              supplied.price_level_capacity)),
          order_index(std::make_unique<IndexEntry[]>(
              supplied.order_index_capacity)),
          bid_levels(std::make_unique<std::uint32_t[]>(
              supplied.price_level_capacity)),
          ask_levels(std::make_unique<std::uint32_t[]>(
              supplied.price_level_capacity)),
          undo_capacity(1U + supplied.maximum_events_per_packet *
                                 kUndoEntriesPerEvent),
          undo(std::make_unique<UndoEntry[]>(undo_capacity)) {
        for (std::size_t index = 0U; index < config.order_capacity; ++index) {
            orders[index].free_next =
                index + 1U < config.order_capacity
                    ? static_cast<std::uint32_t>(index + 1U)
                    : kInvalidSlot;
        }
        for (std::size_t index = 0U; index < config.price_level_capacity;
             ++index) {
            levels[index].free_next =
                index + 1U < config.price_level_capacity
                    ? static_cast<std::uint32_t>(index + 1U)
                    : kInvalidSlot;
        }
        order_free_head = 0U;
        level_free_head = 0U;
    }

    [[nodiscard]] bool push_undo(const UndoEntry& entry) noexcept {
        if (undo_count >= undo_capacity) {
            return false;
        }
        undo[undo_count++] = entry;
        return true;
    }

    [[nodiscard]] bool begin_transaction() noexcept {
        undo_count = 0U;
        UndoEntry entry{};
        entry.kind = UndoKind::Scalars;
        entry.scalars = ScalarSnapshot{order_free_head,
                                       level_free_head,
                                       active_order_count,
                                       active_level_count,
                                       occupied_index_count,
                                       tombstone_count};
        return push_undo(entry);
    }

    [[nodiscard]] bool log_order(const std::uint32_t slot) noexcept {
        UndoEntry entry{};
        entry.kind = UndoKind::Order;
        entry.slot = slot;
        entry.order = orders[slot];
        return push_undo(entry);
    }

    [[nodiscard]] bool log_level(const std::uint32_t slot) noexcept {
        UndoEntry entry{};
        entry.kind = UndoKind::Level;
        entry.slot = slot;
        entry.level = levels[slot];
        return push_undo(entry);
    }

    [[nodiscard]] bool log_hash(const std::uint32_t slot) noexcept {
        UndoEntry entry{};
        entry.kind = UndoKind::Hash;
        entry.slot = slot;
        entry.hash = order_index[slot];
        return push_undo(entry);
    }

    void rollback() noexcept {
        while (undo_count != 0U) {
            const UndoEntry& entry = undo[--undo_count];
            switch (entry.kind) {
                case UndoKind::Scalars:
                    order_free_head = entry.scalars.order_free_head;
                    level_free_head = entry.scalars.level_free_head;
                    active_order_count = entry.scalars.active_orders;
                    active_level_count = entry.scalars.active_levels;
                    occupied_index_count = entry.scalars.occupied_index;
                    tombstone_count = entry.scalars.index_tombstones;
                    break;
                case UndoKind::Order:
                    orders[entry.slot] = entry.order;
                    break;
                case UndoKind::Level:
                    levels[entry.slot] = entry.level;
                    break;
                case UndoKind::Hash:
                    order_index[entry.slot] = entry.hash;
                    break;
                case UndoKind::SortedInserted:
                    rollback_sorted_insert(entry.side, entry.position);
                    break;
                case UndoKind::SortedErased:
                    rollback_sorted_erase(entry.side, entry.position,
                                          entry.sorted_value);
                    break;
            }
        }
    }

    void commit_transaction() noexcept { undo_count = 0U; }

    [[nodiscard]] std::uint32_t* sorted(const Side side) noexcept {
        return side == Side::Buy ? bid_levels.get() : ask_levels.get();
    }

    [[nodiscard]] const std::uint32_t* sorted(
        const Side side) const noexcept {
        return side == Side::Buy ? bid_levels.get() : ask_levels.get();
    }

    [[nodiscard]] std::size_t& sorted_count(const Side side) noexcept {
        return side == Side::Buy ? bid_level_count : ask_level_count;
    }

    [[nodiscard]] std::size_t sorted_count(const Side side) const noexcept {
        return side == Side::Buy ? bid_level_count : ask_level_count;
    }

    [[nodiscard]] std::size_t sorted_position(
        const Side side,
        const Price price) const noexcept {
        const std::uint32_t* const entries = sorted(side);
        const std::size_t count = sorted_count(side);
        std::size_t first = 0U;
        std::size_t last = count;
        while (first < last) {
            const std::size_t middle = first + (last - first) / 2U;
            const Price candidate = levels[entries[middle]].price;
            const bool before = side == Side::Buy ? candidate > price
                                                   : candidate < price;
            if (before) {
                first = middle + 1U;
            } else {
                last = middle;
            }
        }
        return first;
    }

    [[nodiscard]] std::uint32_t find_level(
        const Side side,
        const Price price) const noexcept {
        const std::size_t position = sorted_position(side, price);
        if (position >= sorted_count(side)) {
            return kInvalidSlot;
        }
        const std::uint32_t slot = sorted(side)[position];
        return levels[slot].price == price ? slot : kInvalidSlot;
    }

    [[nodiscard]] bool insert_sorted(
        const Side side,
        const std::uint32_t level_slot) noexcept {
        std::size_t& count = sorted_count(side);
        if (count >= config.price_level_capacity) {
            return false;
        }
        const std::size_t position =
            sorted_position(side, levels[level_slot].price);
        UndoEntry entry{};
        entry.kind = UndoKind::SortedInserted;
        entry.side = side;
        entry.position = static_cast<std::uint32_t>(position);
        if (!push_undo(entry)) {
            return false;
        }
        std::uint32_t* const entries = sorted(side);
        for (std::size_t index = count; index > position; --index) {
            entries[index] = entries[index - 1U];
        }
        entries[position] = level_slot;
        ++count;
        return true;
    }

    [[nodiscard]] bool erase_sorted(
        const Side side,
        const std::uint32_t level_slot) noexcept {
        std::size_t& count = sorted_count(side);
        std::uint32_t* const entries = sorted(side);
        std::size_t position = count;
        for (std::size_t index = 0U; index < count; ++index) {
            if (entries[index] == level_slot) {
                position = index;
                break;
            }
        }
        if (position == count) {
            return false;
        }
        UndoEntry entry{};
        entry.kind = UndoKind::SortedErased;
        entry.side = side;
        entry.position = static_cast<std::uint32_t>(position);
        entry.sorted_value = level_slot;
        if (!push_undo(entry)) {
            return false;
        }
        for (std::size_t index = position; index + 1U < count; ++index) {
            entries[index] = entries[index + 1U];
        }
        --count;
        return true;
    }

    void rollback_sorted_insert(
        const Side side,
        const std::uint32_t position) noexcept {
        std::size_t& count = sorted_count(side);
        std::uint32_t* const entries = sorted(side);
        for (std::size_t index = position; index + 1U < count; ++index) {
            entries[index] = entries[index + 1U];
        }
        --count;
    }

    void rollback_sorted_erase(
        const Side side,
        const std::uint32_t position,
        const std::uint32_t value) noexcept {
        std::size_t& count = sorted_count(side);
        std::uint32_t* const entries = sorted(side);
        for (std::size_t index = count; index > position; --index) {
            entries[index] = entries[index - 1U];
        }
        entries[position] = value;
        ++count;
    }

    [[nodiscard]] std::size_t hash_start(
        const OrderReference reference) const noexcept {
        constexpr std::uint64_t multiplier = 0x9e3779b97f4a7c15ULL;
        return static_cast<std::size_t>((reference * multiplier) %
                                        config.order_index_capacity);
    }

    [[nodiscard]] std::uint32_t find_order(
        const OrderReference reference) const noexcept {
        const std::size_t start = hash_start(reference);
        for (std::size_t probe = 0U; probe < config.order_index_capacity;
             ++probe) {
            const IndexEntry& entry =
                order_index[(start + probe) % config.order_index_capacity];
            if (entry.state == IndexState::Empty) {
                return kInvalidSlot;
            }
            if (entry.state == IndexState::Occupied &&
                entry.reference == reference) {
                return entry.order_slot;
            }
        }
        return kInvalidSlot;
    }

    [[nodiscard]] BookApplyStatus insert_hash(
        const OrderReference reference,
        const std::uint32_t order_slot) noexcept {
        const std::size_t start = hash_start(reference);
        std::uint32_t first_tombstone = kInvalidSlot;
        for (std::size_t probe = 0U; probe < config.order_index_capacity;
             ++probe) {
            const auto index = static_cast<std::uint32_t>(
                (start + probe) % config.order_index_capacity);
            IndexEntry& entry = order_index[index];
            if (entry.state == IndexState::Occupied) {
                if (entry.reference == reference) {
                    return BookApplyStatus::DuplicateOrderReference;
                }
                continue;
            }
            if (entry.state == IndexState::Tombstone) {
                if (first_tombstone == kInvalidSlot) {
                    first_tombstone = index;
                }
                continue;
            }
            const std::uint32_t target =
                first_tombstone == kInvalidSlot ? index : first_tombstone;
            if (!log_hash(target)) {
                return BookApplyStatus::UndoCapacityExceeded;
            }
            if (order_index[target].state == IndexState::Tombstone) {
                --tombstone_count;
            }
            order_index[target] =
                IndexEntry{reference, order_slot, IndexState::Occupied};
            ++occupied_index_count;
            return BookApplyStatus::Applied;
        }
        if (first_tombstone != kInvalidSlot) {
            if (!log_hash(first_tombstone)) {
                return BookApplyStatus::UndoCapacityExceeded;
            }
            order_index[first_tombstone] =
                IndexEntry{reference, order_slot, IndexState::Occupied};
            --tombstone_count;
            ++occupied_index_count;
            return BookApplyStatus::Applied;
        }
        return BookApplyStatus::OrderIndexCapacityExceeded;
    }

    [[nodiscard]] BookApplyStatus erase_hash(
        const OrderReference reference) noexcept {
        const std::size_t start = hash_start(reference);
        for (std::size_t probe = 0U; probe < config.order_index_capacity;
             ++probe) {
            const auto index = static_cast<std::uint32_t>(
                (start + probe) % config.order_index_capacity);
            IndexEntry& entry = order_index[index];
            if (entry.state == IndexState::Empty) {
                return BookApplyStatus::UnknownOrderReference;
            }
            if (entry.state == IndexState::Occupied &&
                entry.reference == reference) {
                if (!log_hash(index)) {
                    return BookApplyStatus::UndoCapacityExceeded;
                }
                entry.state = IndexState::Tombstone;
                entry.order_slot = kInvalidSlot;
                --occupied_index_count;
                ++tombstone_count;
                return BookApplyStatus::Applied;
            }
        }
        return BookApplyStatus::UnknownOrderReference;
    }

    [[nodiscard]] BookApplyStatus create_level(
        const Side side,
        const Price price,
        std::uint32_t& result) noexcept {
        if (level_free_head == kInvalidSlot) {
            return BookApplyStatus::PriceLevelCapacityExceeded;
        }
        result = level_free_head;
        if (!log_level(result)) {
            return BookApplyStatus::UndoCapacityExceeded;
        }
        level_free_head = levels[result].free_next;
        levels[result] = LevelNode{price,
                                   Quantity{},
                                   side,
                                   kInvalidSlot,
                                   kInvalidSlot,
                                   0U,
                                   kInvalidSlot,
                                   true};
        ++active_level_count;
        if (!insert_sorted(side, result)) {
            return undo_count >= undo_capacity
                       ? BookApplyStatus::UndoCapacityExceeded
                       : BookApplyStatus::PriceLevelCapacityExceeded;
        }
        return BookApplyStatus::Applied;
    }

    [[nodiscard]] BookApplyStatus get_or_create_level(
        const Side side,
        const Price price,
        std::uint32_t& result) noexcept {
        result = find_level(side, price);
        return result == kInvalidSlot
                   ? create_level(side, price, result)
                   : BookApplyStatus::Applied;
    }

    [[nodiscard]] BookApplyStatus release_level(
        const std::uint32_t slot) noexcept {
        if (!log_level(slot)) {
            return BookApplyStatus::UndoCapacityExceeded;
        }
        const Side side = levels[slot].side;
        if (!erase_sorted(side, slot)) {
            return undo_count >= undo_capacity
                       ? BookApplyStatus::UndoCapacityExceeded
                       : BookApplyStatus::MalformedEvent;
        }
        levels[slot].active = false;
        levels[slot].free_next = level_free_head;
        level_free_head = slot;
        --active_level_count;
        return BookApplyStatus::Applied;
    }

    [[nodiscard]] BookApplyStatus attach_order(
        const std::uint32_t order_slot,
        const std::uint32_t level_slot) noexcept {
        OrderNode& node = orders[order_slot];
        LevelNode& level = levels[level_slot];
        if (node.quantity.units >
            std::numeric_limits<std::uint64_t>::max() -
                level.aggregate.units) {
            return BookApplyStatus::AggregateOverflow;
        }
        if (!log_order(order_slot) || !log_level(level_slot)) {
            return BookApplyStatus::UndoCapacityExceeded;
        }
        if (level.tail != kInvalidSlot) {
            if (!log_order(level.tail)) {
                return BookApplyStatus::UndoCapacityExceeded;
            }
            orders[level.tail].next = order_slot;
        } else {
            level.head = order_slot;
        }
        node.previous = level.tail;
        node.next = kInvalidSlot;
        node.level_slot = level_slot;
        level.tail = order_slot;
        level.aggregate.units += node.quantity.units;
        ++level.order_count;
        return BookApplyStatus::Applied;
    }

    [[nodiscard]] BookApplyStatus detach_order(
        const std::uint32_t order_slot) noexcept {
        OrderNode& node = orders[order_slot];
        if (node.level_slot == kInvalidSlot) {
            return BookApplyStatus::MalformedEvent;
        }
        const std::uint32_t level_slot = node.level_slot;
        LevelNode& level = levels[level_slot];
        if (level.aggregate.units < node.quantity.units ||
            level.order_count == 0U || !log_order(order_slot) ||
            !log_level(level_slot)) {
            return undo_count >= undo_capacity
                       ? BookApplyStatus::UndoCapacityExceeded
                       : BookApplyStatus::MalformedEvent;
        }
        if (node.previous != kInvalidSlot) {
            if (!log_order(node.previous)) {
                return BookApplyStatus::UndoCapacityExceeded;
            }
            orders[node.previous].next = node.next;
        } else {
            level.head = node.next;
        }
        if (node.next != kInvalidSlot) {
            if (!log_order(node.next)) {
                return BookApplyStatus::UndoCapacityExceeded;
            }
            orders[node.next].previous = node.previous;
        } else {
            level.tail = node.previous;
        }
        level.aggregate.units -= node.quantity.units;
        --level.order_count;
        node.previous = kInvalidSlot;
        node.next = kInvalidSlot;
        node.level_slot = kInvalidSlot;
        if (level.order_count == 0U) {
            return release_level(level_slot);
        }
        return BookApplyStatus::Applied;
    }

    [[nodiscard]] BookApplyStatus apply_add(const L3Event& event) noexcept {
        if (find_order(event.order_reference) != kInvalidSlot) {
            return BookApplyStatus::DuplicateOrderReference;
        }
        if (order_free_head == kInvalidSlot) {
            return BookApplyStatus::OrderCapacityExceeded;
        }
        const std::uint32_t order_slot = order_free_head;
        if (!log_order(order_slot)) {
            return BookApplyStatus::UndoCapacityExceeded;
        }
        order_free_head = orders[order_slot].free_next;
        orders[order_slot] = OrderNode{event.order_reference,
                                       event.price,
                                       event.quantity,
                                       event.side,
                                       kInvalidSlot,
                                       kInvalidSlot,
                                       kInvalidSlot,
                                       kInvalidSlot,
                                       true};
        ++active_order_count;

        std::uint32_t level_slot = kInvalidSlot;
        BookApplyStatus status =
            get_or_create_level(event.side, event.price, level_slot);
        if (status != BookApplyStatus::Applied) {
            return status;
        }
        status = attach_order(order_slot, level_slot);
        if (status != BookApplyStatus::Applied) {
            return status;
        }
        return insert_hash(event.order_reference, order_slot);
    }

    [[nodiscard]] BookApplyStatus apply_delete(
        const L3Event& event) noexcept {
        const std::uint32_t order_slot = find_order(event.order_reference);
        if (order_slot == kInvalidSlot) {
            return BookApplyStatus::UnknownOrderReference;
        }
        BookApplyStatus status = detach_order(order_slot);
        if (status != BookApplyStatus::Applied) {
            return status;
        }
        status = erase_hash(event.order_reference);
        if (status != BookApplyStatus::Applied) {
            return status;
        }
        OrderNode& node = orders[order_slot];
        node.active = false;
        node.free_next = order_free_head;
        order_free_head = order_slot;
        --active_order_count;
        return BookApplyStatus::Applied;
    }

    [[nodiscard]] BookApplyStatus apply_modify(
        const L3Event& event,
        bool& changed) noexcept {
        const std::uint32_t order_slot = find_order(event.order_reference);
        if (order_slot == kInvalidSlot) {
            return BookApplyStatus::UnknownOrderReference;
        }
        OrderNode& node = orders[order_slot];
        if (node.side == event.side && node.price == event.price &&
            node.quantity == event.quantity) {
            changed = false;
            return BookApplyStatus::Applied;
        }
        if (node.side == event.side && node.price == event.price) {
            LevelNode& level = levels[node.level_slot];
            const std::uint64_t without_old =
                level.aggregate.units - node.quantity.units;
            if (event.quantity.units >
                std::numeric_limits<std::uint64_t>::max() - without_old) {
                return BookApplyStatus::AggregateOverflow;
            }
            if (!log_order(order_slot) || !log_level(node.level_slot)) {
                return BookApplyStatus::UndoCapacityExceeded;
            }
            level.aggregate.units = without_old + event.quantity.units;
            node.quantity = event.quantity;
            return BookApplyStatus::Applied;
        }

        BookApplyStatus status = detach_order(order_slot);
        if (status != BookApplyStatus::Applied) {
            return status;
        }
        node.side = event.side;
        node.price = event.price;
        node.quantity = event.quantity;
        std::uint32_t level_slot = kInvalidSlot;
        status = get_or_create_level(event.side, event.price, level_slot);
        if (status != BookApplyStatus::Applied) {
            return status;
        }
        return attach_order(order_slot, level_slot);
    }

    [[nodiscard]] BookApplyStatus apply_event(
        const L3Event& event,
        bool& changed) noexcept {
        changed = true;
        if (event.order_reference == 0U || event.exchange_timestamp_ns < 0) {
            return BookApplyStatus::MalformedEvent;
        }
        switch (event.operation) {
            case L3Operation::Add:
                if (!valid_side(event.side) || event.price.units <= 0 ||
                    !event.quantity) {
                    return BookApplyStatus::MalformedEvent;
                }
                return apply_add(event);
            case L3Operation::Modify:
                if (!valid_side(event.side) || event.price.units <= 0 ||
                    !event.quantity) {
                    return BookApplyStatus::MalformedEvent;
                }
                return apply_modify(event, changed);
            case L3Operation::Delete:
                return apply_delete(event);
        }
        return BookApplyStatus::MalformedEvent;
    }

    [[nodiscard]] bool context_valid(
        const BookPacketContext& context) const noexcept {
        return context.feed_session_epoch != 0U &&
               context.recovery_generation != 0U &&
               context.last_applied_sequence !=
                   std::numeric_limits<std::uint64_t>::max() &&
               context.local_publication_time_ns >=
                   context.local_receive_time_ns &&
               valid_feed_state(context.feed_state);
    }

    [[nodiscard]] bool crossed() const noexcept {
        return bid_level_count != 0U && ask_level_count != 0U &&
               levels[bid_levels[0]].price >= levels[ask_levels[0]].price;
    }

    void publish(
        const BookPacketContext& context,
        const std::int64_t exchange_timestamp_ns) noexcept {
        md::BookView& view = publication[0];
        view = {};
        view.venue = config.venue;
        view.channel = config.channel;
        view.profile = config.profile;
        view.instrument = config.instrument;
        view.feed_session_epoch = context.feed_session_epoch;
        view.recovery_generation = context.recovery_generation;
        view.view_generation = ++view_generation;
        view.last_applied_sequence = context.last_applied_sequence;
        view.next_sequence = context.last_applied_sequence + 1U;
        view.exchange_timestamp_ns = exchange_timestamp_ns;
        view.local_receive_time_ns = context.local_receive_time_ns;
        view.local_publication_time_ns = context.local_publication_time_ns;
        view.feed_state = context.feed_state;
        view.trading_state = config.trading_state;
        const std::size_t bids =
            std::min<std::size_t>(bid_level_count, md::kMaxDepthLevels);
        const std::size_t asks =
            std::min<std::size_t>(ask_level_count, md::kMaxDepthLevels);
        for (std::size_t index = 0U; index < bids; ++index) {
            const LevelNode& level = levels[bid_levels[index]];
            static_cast<void>(view.push_bid(md::Level{
                level.price, level.aggregate, level.order_count}));
        }
        for (std::size_t index = 0U; index < asks; ++index) {
            const LevelNode& level = levels[ask_levels[index]];
            static_cast<void>(view.push_ask(md::Level{
                level.price, level.aggregate, level.order_count}));
        }
        view.depth_truncated = bid_level_count > md::kMaxDepthLevels ||
                               ask_level_count > md::kMaxDepthLevels;
    }

    template <typename Next>
    [[nodiscard]] BookApplyResult apply_with(
        const BookPacketContext& context,
        const std::size_t event_count,
        Next&& next,
        const bool require_end) noexcept {
        publication[0] = {};
        if (!context_valid(context)) {
            return {BookApplyStatus::InvalidContext, 0U, {}};
        }
        if (event_count > config.maximum_events_per_packet) {
            return {BookApplyStatus::UndoCapacityExceeded, 0U, {}};
        }
        if (!begin_transaction()) {
            return {BookApplyStatus::UndoCapacityExceeded, 0U, {}};
        }
        bool packet_changed = false;
        std::int64_t greatest_exchange_timestamp = 0;
        for (std::size_t index = 0U; index < event_count; ++index) {
            L3Event event{};
            if (next(index, event) != DecodeStep::Event) {
                rollback();
                return {BookApplyStatus::DecodeFailure, 0U, {}};
            }
            bool event_changed = false;
            const BookApplyStatus status = apply_event(event, event_changed);
            if (status != BookApplyStatus::Applied) {
                rollback();
                return {status, 0U, {}};
            }
            packet_changed = packet_changed || event_changed;
            greatest_exchange_timestamp =
                std::max(greatest_exchange_timestamp,
                         event.exchange_timestamp_ns);
        }
        if (require_end) {
            L3Event ignored{};
            if (next(event_count, ignored) != DecodeStep::End) {
                rollback();
                return {BookApplyStatus::DecodeFailure, 0U, {}};
            }
        }
        if (crossed()) {
            rollback();
            return {BookApplyStatus::CrossedBook, 0U, {}};
        }
        commit_transaction();
        if (!packet_changed) {
            return {BookApplyStatus::NoChange, event_count, {}};
        }
        publish(context, greatest_exchange_timestamp);
        return {BookApplyStatus::Applied, event_count,
                {publication.data(), 1U}};
    }

    FixedL3BookConfig config;
    std::unique_ptr<OrderNode[]> orders;
    std::unique_ptr<LevelNode[]> levels;
    std::unique_ptr<IndexEntry[]> order_index;
    std::unique_ptr<std::uint32_t[]> bid_levels;
    std::unique_ptr<std::uint32_t[]> ask_levels;
    std::size_t undo_capacity{};
    std::unique_ptr<UndoEntry[]> undo;
    std::size_t undo_count{};
    std::uint32_t order_free_head{kInvalidSlot};
    std::uint32_t level_free_head{kInvalidSlot};
    std::size_t active_order_count{};
    std::size_t active_level_count{};
    std::size_t occupied_index_count{};
    std::size_t tombstone_count{};
    std::size_t bid_level_count{};
    std::size_t ask_level_count{};
    std::uint64_t view_generation{};
    std::array<md::BookView, 1U> publication{};
};

FixedL3Book::FixedL3Book(const FixedL3BookConfig config) {
    if (!config.venue || !config.channel || !config.profile ||
        !config.instrument || config.order_capacity == 0U ||
        config.order_index_capacity < config.order_capacity ||
        config.price_level_capacity == 0U ||
        config.maximum_events_per_packet == 0U ||
        !valid_trading_state(config.trading_state) ||
        config.order_capacity > kInvalidSlot ||
        config.order_index_capacity > kInvalidSlot ||
        config.price_level_capacity > kInvalidSlot ||
        config.maximum_events_per_packet >
            (std::numeric_limits<std::size_t>::max() - 1U) /
                kUndoEntriesPerEvent) {
        throw std::invalid_argument("invalid fixed L3 book capacity or identity");
    }
    impl_ = std::make_unique<Impl>(config);
}

FixedL3Book::~FixedL3Book() = default;
FixedL3Book::FixedL3Book(FixedL3Book&&) noexcept = default;
FixedL3Book& FixedL3Book::operator=(FixedL3Book&&) noexcept = default;

BookApplyResult FixedL3Book::apply_packet(
    const BookPacketContext& context,
    const std::span<const L3Event> events) noexcept {
    const auto next = [&](const std::size_t index,
                          L3Event& event) noexcept {
        if (index >= events.size()) {
            return DecodeStep::End;
        }
        event = events[index];
        return DecodeStep::Event;
    };
    return impl_->apply_with(context, events.size(), next, false);
}

BookApplyResult FixedL3Book::decode_and_apply(
    const BookPacketContext& context,
    const std::span<const std::byte> registered_buffer,
    const std::size_t declared_event_count,
    const DecodeNextEvent decode_next) noexcept {
    if (decode_next == nullptr) {
        return {BookApplyStatus::DecodeFailure, 0U, {}};
    }
    std::size_t cursor = 0U;
    const auto next = [&](const std::size_t,
                          L3Event& event) noexcept {
        return decode_next(registered_buffer, cursor, event);
    };
    return impl_->apply_with(context, declared_event_count, next, true);
}

std::optional<OrderSnapshot> FixedL3Book::order(
    const OrderReference reference) const noexcept {
    if (reference == 0U) {
        return std::nullopt;
    }
    const std::uint32_t slot = impl_->find_order(reference);
    if (slot == kInvalidSlot) {
        return std::nullopt;
    }
    const Impl::OrderNode& node = impl_->orders[slot];
    return OrderSnapshot{node.reference, node.side, node.price, node.quantity};
}

std::optional<PriceLevelSnapshot> FixedL3Book::level(
    const Side side,
    const Price price) const noexcept {
    if (!valid_side(side) || price.units <= 0) {
        return std::nullopt;
    }
    const std::uint32_t slot = impl_->find_level(side, price);
    if (slot == kInvalidSlot) {
        return std::nullopt;
    }
    const Impl::LevelNode& value = impl_->levels[slot];
    return PriceLevelSnapshot{value.side, value.price, value.aggregate,
                              value.order_count};
}

std::size_t FixedL3Book::copy_fifo(
    const Side side,
    const Price price,
    const std::span<OrderReference> destination) const noexcept {
    if (!valid_side(side) || price.units <= 0) {
        return 0U;
    }
    const std::uint32_t level_slot = impl_->find_level(side, price);
    if (level_slot == kInvalidSlot) {
        return 0U;
    }
    std::size_t copied = 0U;
    std::uint32_t order_slot = impl_->levels[level_slot].head;
    while (order_slot != kInvalidSlot && copied < destination.size()) {
        destination[copied++] = impl_->orders[order_slot].reference;
        order_slot = impl_->orders[order_slot].next;
    }
    return copied;
}

FixedL3BookMetadata FixedL3Book::metadata() const noexcept {
    return FixedL3BookMetadata{impl_->config.order_capacity,
                               impl_->config.order_index_capacity,
                               impl_->config.price_level_capacity,
                               impl_->config.maximum_events_per_packet,
                               impl_->undo_capacity,
                               impl_->active_order_count,
                               impl_->active_level_count,
                               impl_->occupied_index_count,
                               impl_->tombstone_count,
                               impl_->view_generation};
}

DecodeStep decode_canonical_l3_event(
    const std::span<const std::byte> registered_buffer,
    std::size_t& cursor,
    L3Event& event) noexcept {
    if (cursor == registered_buffer.size()) {
        return DecodeStep::End;
    }
    if (cursor > registered_buffer.size() ||
        kCanonicalEventBytes > registered_buffer.size() - cursor) {
        return DecodeStep::Malformed;
    }
    const std::byte* const record = registered_buffer.data() + cursor;
    const std::uint8_t operation = std::to_integer<std::uint8_t>(record[0]);
    const std::uint8_t side = std::to_integer<std::uint8_t>(record[1]);
    if ((operation < static_cast<std::uint8_t>(L3Operation::Add) ||
         operation > static_cast<std::uint8_t>(L3Operation::Delete)) ||
        side > 1U || record[2] != std::byte{} ||
        record[3] != std::byte{}) {
        return DecodeStep::Malformed;
    }
    event.operation = static_cast<L3Operation>(operation);
    event.side = side == 0U ? Side::Buy : Side::Sell;
    event.order_reference = read_u64(record + 4U);
    event.price = Price{std::bit_cast<std::int64_t>(read_u64(record + 12U))};
    event.quantity = Quantity{read_u64(record + 20U)};
    event.exchange_timestamp_ns =
        std::bit_cast<std::int64_t>(read_u64(record + 28U));
    cursor += kCanonicalEventBytes;
    return DecodeStep::Event;
}

}  // namespace sorx::marketdata
