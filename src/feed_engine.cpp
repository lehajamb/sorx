#include "sorx/md/feed_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sorx::md {
namespace {

template <class... Functions>
struct Overloaded final : Functions... {
    using Functions::operator()...;
};

template <class... Functions>
Overloaded(Functions...) -> Overloaded<Functions...>;

[[nodiscard]] constexpr bool valid_state(const InstrumentState state) noexcept {
    switch (state) {
    case InstrumentState::Unknown:
    case InstrumentState::Trading:
    case InstrumentState::Halted:
    case InstrumentState::Closed:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_side(const Side side) noexcept {
    switch (side) {
    case Side::Buy:
    case Side::Sell:
        return true;
    }
    return false;
}

struct LevelState final {
    std::uint64_t quantity{};
    std::list<OrderReference> fifo;
};

struct InstrumentBook final {
    FeedInstrumentKey key{};
    InstrumentState trading_state{InstrumentState::Unknown};
    std::int64_t exchange_timestamp_ns{};
    std::map<std::int64_t, LevelState, std::greater<>> bids;
    std::map<std::int64_t, LevelState> asks;
};

struct Order final {
    FeedInstrumentKey key{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price price{};
};

class BookState final {
public:
    [[nodiscard]] bool apply(const InstrumentDirectoryEvent& event) {
        if (event.key == 0 || !event.instrument || event.exchange_timestamp_ns < 0) {
            return false;
        }

        const auto key_it = directory_.find(event.key);
        if (key_it != directory_.end() && key_it->second != event.instrument.value()) {
            return false;
        }

        const auto instrument_it = instruments_.find(event.instrument.value());
        if (instrument_it != instruments_.end()) {
            if (instrument_it->second.key != event.key ||
                event.exchange_timestamp_ns < instrument_it->second.exchange_timestamp_ns) {
                return false;
            }
            directory_[event.key] = event.instrument.value();
            instrument_it->second.exchange_timestamp_ns = event.exchange_timestamp_ns;
            return true;
        }

        directory_.emplace(event.key, event.instrument.value());
        instruments_.emplace(
            event.instrument.value(),
            InstrumentBook{
                event.key,
                InstrumentState::Unknown,
                event.exchange_timestamp_ns,
                {},
                {},
            });
        return true;
    }

    [[nodiscard]] bool apply(const TradingStateEvent& event) {
        auto* book = find_book(event.key, event.instrument);
        if (book == nullptr || !valid_state(event.state) ||
            event.exchange_timestamp_ns < book->exchange_timestamp_ns) {
            return false;
        }
        book->trading_state = event.state;
        book->exchange_timestamp_ns = event.exchange_timestamp_ns;
        return true;
    }

    [[nodiscard]] bool apply(const AddOrderEvent& event) {
        auto* book = find_book(event.key, event.instrument);
        if (book == nullptr || event.order_reference == 0 || !event.quantity ||
            event.price.units <= 0 || !valid_side(event.side) ||
            event.exchange_timestamp_ns < book->exchange_timestamp_ns ||
            orders_.contains(event.order_reference)) {
            return false;
        }

        if (event.side == Side::Buy) {
            if (!book->asks.empty() && event.price.units >= book->asks.begin()->first) {
                return false;
            }
            return add_to_side(book->bids, *book, event);
        }

        if (!book->bids.empty() && event.price.units <= book->bids.begin()->first) {
            return false;
        }
        return add_to_side(book->asks, *book, event);
    }

    [[nodiscard]] bool apply(const ExecuteOrderEvent& event) {
        if (event.order_reference == 0 || event.key == 0 || !event.quantity ||
            event.exchange_timestamp_ns < 0) {
            return false;
        }
        const auto order_it = orders_.find(event.order_reference);
        if (order_it == orders_.end() || order_it->second.key != event.key ||
            event.quantity.units > order_it->second.quantity.units) {
            return false;
        }

        auto instrument_it = instruments_.find(order_it->second.instrument.value());
        if (instrument_it == instruments_.end() ||
            event.exchange_timestamp_ns < instrument_it->second.exchange_timestamp_ns) {
            return false;
        }

        auto& book = instrument_it->second;
        const auto price = order_it->second.price.units;
        const auto side = order_it->second.side;
        const auto new_quantity = order_it->second.quantity.units - event.quantity.units;
        const bool removed = side == Side::Buy
            ? reduce_level(book.bids, price, event.order_reference, event.quantity.units, new_quantity == 0)
            : reduce_level(book.asks, price, event.order_reference, event.quantity.units, new_quantity == 0);
        if (!removed) {
            return false;
        }

        book.exchange_timestamp_ns = event.exchange_timestamp_ns;
        if (new_quantity == 0) {
            orders_.erase(order_it);
        } else {
            order_it->second.quantity = Quantity{new_quantity};
        }
        return true;
    }

    [[nodiscard]] bool apply(const DeleteOrderEvent& event) {
        if (event.order_reference == 0 || event.key == 0 || event.exchange_timestamp_ns < 0) {
            return false;
        }
        const auto order_it = orders_.find(event.order_reference);
        if (order_it == orders_.end() || order_it->second.key != event.key) {
            return false;
        }

        auto instrument_it = instruments_.find(order_it->second.instrument.value());
        if (instrument_it == instruments_.end() ||
            event.exchange_timestamp_ns < instrument_it->second.exchange_timestamp_ns) {
            return false;
        }

        auto& book = instrument_it->second;
        const auto& order = order_it->second;
        const bool removed = order.side == Side::Buy
            ? reduce_level(
                  book.bids,
                  order.price.units,
                  event.order_reference,
                  order.quantity.units,
                  true)
            : reduce_level(
                  book.asks,
                  order.price.units,
                  event.order_reference,
                  order.quantity.units,
                  true);
        if (!removed) {
            return false;
        }

        book.exchange_timestamp_ns = event.exchange_timestamp_ns;
        orders_.erase(order_it);
        return true;
    }

    [[nodiscard]] bool valid() const {
        std::unordered_set<OrderReference> indexed_orders;
        indexed_orders.reserve(orders_.size());

        for (const auto& [instrument_value, book] : instruments_) {
            const auto directory_it = directory_.find(book.key);
            if (directory_it == directory_.end() || directory_it->second != instrument_value ||
                !validate_side(book.bids, instrument_value, book, Side::Buy, indexed_orders) ||
                !validate_side(book.asks, instrument_value, book, Side::Sell, indexed_orders)) {
                return false;
            }
            if (!book.bids.empty() && !book.asks.empty() &&
                book.bids.begin()->first >= book.asks.begin()->first) {
                return false;
            }
        }
        return indexed_orders.size() == orders_.size();
    }

    [[nodiscard]] std::vector<BookView> make_views(
        const FeedEngineConfig& config,
        const MarketDataState state,
        const std::uint64_t feed_session_epoch,
        const std::uint64_t recovery_generation,
        const std::uint64_t view_generation,
        const std::uint64_t next_sequence,
        const std::uint64_t local_receive_time_ns,
        const std::uint64_t local_publication_time_ns) const {
        std::vector<BookView> result;
        result.reserve(instruments_.size());

        for (const auto& [instrument_value, book] : instruments_) {
            BookView view;
            view.venue = config.venue;
            view.channel = config.channel;
            view.profile = config.profile;
            view.instrument = InstrumentId{instrument_value};
            view.feed_session_epoch = feed_session_epoch;
            view.recovery_generation = recovery_generation;
            view.view_generation = view_generation;
            view.last_applied_sequence = next_sequence - 1;
            view.next_sequence = next_sequence;
            view.exchange_timestamp_ns = book.exchange_timestamp_ns;
            view.local_receive_time_ns = local_receive_time_ns;
            view.local_publication_time_ns = local_publication_time_ns;
            view.feed_state = state;
            view.trading_state = book.trading_state;
            view.depth_truncated =
                book.bids.size() > config.publication_depth ||
                book.asks.size() > config.publication_depth;

            append_levels(book.bids, config.publication_depth, view.bids, view.bid_count);
            append_levels(book.asks, config.publication_depth, view.asks, view.ask_count);
            result.push_back(view);
        }
        return result;
    }

private:
    template <typename Levels>
    [[nodiscard]] bool add_to_side(
        Levels& levels,
        InstrumentBook& book,
        const AddOrderEvent& event) {
        auto level_it = levels.find(event.price.units);
        if (level_it != levels.end()) {
            if (event.quantity.units >
                    std::numeric_limits<std::uint64_t>::max() - level_it->second.quantity ||
                level_it->second.fifo.size() >=
                    std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
        }

        auto [actual_it, inserted] = levels.try_emplace(event.price.units);
        (void)inserted;
        actual_it->second.quantity += event.quantity.units;
        actual_it->second.fifo.push_back(event.order_reference);
        orders_.emplace(
            event.order_reference,
            Order{event.key, event.instrument, event.side, event.quantity, event.price});
        book.exchange_timestamp_ns = event.exchange_timestamp_ns;
        return true;
    }

    template <typename Levels>
    [[nodiscard]] static bool reduce_level(
        Levels& levels,
        const std::int64_t price,
        const OrderReference order_reference,
        const std::uint64_t reduction,
        const bool remove_order) {
        const auto level_it = levels.find(price);
        if (level_it == levels.end() || reduction > level_it->second.quantity) {
            return false;
        }

        if (remove_order) {
            const auto fifo_it = std::find(
                level_it->second.fifo.begin(),
                level_it->second.fifo.end(),
                order_reference);
            if (fifo_it == level_it->second.fifo.end()) {
                return false;
            }
            level_it->second.fifo.erase(fifo_it);
        }

        level_it->second.quantity -= reduction;
        if (level_it->second.quantity == 0) {
            if (!level_it->second.fifo.empty()) {
                return false;
            }
            levels.erase(level_it);
        } else if (level_it->second.fifo.empty()) {
            return false;
        }
        return true;
    }

    [[nodiscard]] InstrumentBook* find_book(
        const FeedInstrumentKey key,
        const InstrumentId instrument) {
        if (key == 0 || !instrument) {
            return nullptr;
        }
        const auto directory_it = directory_.find(key);
        if (directory_it == directory_.end() || directory_it->second != instrument.value()) {
            return nullptr;
        }
        const auto instrument_it = instruments_.find(instrument.value());
        return instrument_it == instruments_.end() ? nullptr : &instrument_it->second;
    }

    template <typename Levels>
    [[nodiscard]] bool validate_side(
        const Levels& levels,
        const std::uint32_t instrument_value,
        const InstrumentBook& book,
        const Side side,
        std::unordered_set<OrderReference>& indexed_orders) const {
        for (const auto& [price, level] : levels) {
            if (level.quantity == 0 || level.fifo.empty() ||
                level.fifo.size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            std::uint64_t aggregate{};
            for (const auto order_reference : level.fifo) {
                if (!indexed_orders.insert(order_reference).second) {
                    return false;
                }
                const auto order_it = orders_.find(order_reference);
                if (order_it == orders_.end() ||
                    order_it->second.instrument.value() != instrument_value ||
                    order_it->second.key != book.key || order_it->second.side != side ||
                    order_it->second.price.units != price || !order_it->second.quantity ||
                    order_it->second.quantity.units >
                        std::numeric_limits<std::uint64_t>::max() - aggregate) {
                    return false;
                }
                aggregate += order_it->second.quantity.units;
            }
            if (aggregate != level.quantity) {
                return false;
            }
        }
        return true;
    }

    template <typename Levels>
    static void append_levels(
        const Levels& levels,
        const std::size_t depth,
        std::array<Level, kMaxDepthLevels>& destination,
        std::uint16_t& count) {
        count = 0;
        for (auto it = levels.begin(); it != levels.end() && count < depth; ++it) {
            destination[count++] = Level{
                Price{it->first},
                Quantity{it->second.quantity},
                static_cast<std::uint32_t>(it->second.fifo.size()),
            };
        }
    }

    std::map<FeedInstrumentKey, std::uint32_t> directory_;
    std::map<std::uint32_t, InstrumentBook> instruments_;
    std::unordered_map<OrderReference, Order> orders_;
};

enum class EventResult : std::uint8_t {
    Applied,
    EndOfSession,
    Invalid,
};

[[nodiscard]] EventResult apply_event(BookState& book, const FeedEvent& event) {
    return std::visit(
        Overloaded{
            [&book](const InstrumentDirectoryEvent& value) {
                return book.apply(value) ? EventResult::Applied : EventResult::Invalid;
            },
            [&book](const TradingStateEvent& value) {
                return book.apply(value) ? EventResult::Applied : EventResult::Invalid;
            },
            [&book](const AddOrderEvent& value) {
                return book.apply(value) ? EventResult::Applied : EventResult::Invalid;
            },
            [&book](const ExecuteOrderEvent& value) {
                return book.apply(value) ? EventResult::Applied : EventResult::Invalid;
            },
            [&book](const DeleteOrderEvent& value) {
                return book.apply(value) ? EventResult::Applied : EventResult::Invalid;
            },
            [](const KnownBookNeutralEvent& value) {
                return value.exchange_timestamp_ns >= 0
                    ? EventResult::Applied
                    : EventResult::Invalid;
            },
            [](const EndOfSessionEvent& value) {
                return value.exchange_timestamp_ns >= 0
                    ? EventResult::EndOfSession
                    : EventResult::Invalid;
            },
        },
        event);
}

[[nodiscard]] bool snapshot_event_allowed(const FeedEvent& event) {
    return !std::holds_alternative<ExecuteOrderEvent>(event) &&
        !std::holds_alternative<DeleteOrderEvent>(event) &&
        !std::holds_alternative<EndOfSessionEvent>(event);
}

}  // namespace

namespace {

struct FrameApplication final {
    FeedError error{FeedError::None};
    std::size_t applied{};
    std::size_t skipped{};
    bool end_of_session{};
};

[[nodiscard]] FeedError validate_packet_shape(const PacketFrame& packet) noexcept {
    if (packet.session_key == 0 || packet.first_sequence == 0) {
        return FeedError::MalformedFrame;
    }
    const bool control = packet.declared_message_count == 0 ||
        packet.declared_message_count == std::numeric_limits<std::uint16_t>::max();
    if ((control && !packet.events.empty()) ||
        (!control && packet.events.size() != packet.declared_message_count) ||
        (!control &&
         packet.declared_message_count >
             std::numeric_limits<std::uint64_t>::max() - packet.first_sequence)) {
        return FeedError::MalformedFrame;
    }
    return FeedError::None;
}

[[nodiscard]] FrameApplication apply_validated_frame(
    BookState& book,
    std::uint64_t& next_sequence,
    const PacketFrame& packet) {
    FrameApplication result;
    if (packet.first_sequence > next_sequence) {
        result.error = FeedError::SequenceGap;
        return result;
    }

    if (packet.declared_message_count == 0) {
        return result;
    }
    if (packet.declared_message_count ==
        std::numeric_limits<std::uint16_t>::max()) {
        result.end_of_session = packet.first_sequence == next_sequence;
        return result;
    }

    const auto frame_end =
        packet.first_sequence + packet.declared_message_count;
    result.skipped = frame_end <= next_sequence
        ? static_cast<std::size_t>(packet.declared_message_count)
        : static_cast<std::size_t>(next_sequence - packet.first_sequence);

    for (auto index = result.skipped; index < packet.events.size(); ++index) {
        const auto event_result = apply_event(book, packet.events[index]);
        if (event_result == EventResult::Invalid ||
            (event_result == EventResult::EndOfSession &&
             index + 1 != packet.events.size())) {
            result.error = FeedError::InvalidEvent;
            return result;
        }

        ++next_sequence;
        ++result.applied;
        if (event_result == EventResult::EndOfSession) {
            result.end_of_session = true;
            break;
        }
    }

    if (!book.valid()) {
        result.error = FeedError::InvalidEvent;
    }
    return result;
}

struct OwnedPacket final {
    FeedSessionKey session_key{};
    std::uint64_t first_sequence{};
    std::uint16_t declared_message_count{};
    std::vector<FeedEvent> events;
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};

    [[nodiscard]] PacketFrame view() const noexcept {
        return PacketFrame{
            session_key,
            first_sequence,
            declared_message_count,
            std::span<const FeedEvent>{events},
            local_receive_time_ns,
            local_publication_time_ns,
        };
    }
};

}  // namespace

struct FeedEngine::Impl final {
    explicit Impl(FeedEngineConfig value) : config(std::move(value)) {
        if (!config.venue || !config.channel || !config.profile ||
            config.publication_depth == 0 ||
            config.publication_depth > kMaxDepthLevels ||
            config.recovery_strategy != RecoveryStrategy::FreshSnapshot ||
            config.stitch_buffer_packet_capacity == 0 ||
            config.stitch_buffer_event_capacity == 0) {
            throw std::invalid_argument("invalid feed-engine configuration");
        }
    }

    [[nodiscard]] EngineUpdate update(
        const FeedError error = FeedError::None,
        const RecoveryAction recovery = RecoveryAction::None) const {
        EngineUpdate result;
        result.state = state;
        result.error = error;
        result.recovery = recovery;
        result.publication_generation = publication_generation;
        return result;
    }

    void publish(EngineUpdate& result) {
        if (publication_generation != std::numeric_limits<std::uint64_t>::max()) {
            ++publication_generation;
        }
        result.publication_generation = publication_generation;
        result.publications = book.make_views(
            config,
            state,
            feed_session_epoch,
            recovery_generation,
            publication_generation,
            next_sequence,
            last_receive_time_ns,
            last_publication_time_ns);
    }

    [[nodiscard]] bool clocks_valid(
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns) const noexcept {
        return publication_time_ns >= receive_time_ns &&
            receive_time_ns >= last_receive_time_ns &&
            publication_time_ns >= last_publication_time_ns;
    }

    void set_times(
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns) noexcept {
        last_receive_time_ns = receive_time_ns;
        last_publication_time_ns = publication_time_ns;
    }

    void normalize_failure_times(
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns) noexcept {
        last_receive_time_ns = std::max(last_receive_time_ns, receive_time_ns);
        last_publication_time_ns = std::max(
            {last_publication_time_ns, publication_time_ns, last_receive_time_ns});
    }

    void clear_buffer() noexcept {
        buffered_packets.clear();
        buffered_event_count = 0;
    }

    void clear_stitch_proof() noexcept {
        stitch_cut_sequence = 0;
        stitch_high_water_sequence = 0;
    }

    [[nodiscard]] bool buffer_can_accept(const PacketFrame& packet) const noexcept {
        return buffered_packets.size() < config.stitch_buffer_packet_capacity &&
            packet.events.size() <=
                config.stitch_buffer_event_capacity - buffered_event_count;
    }

    void store_buffered(const PacketFrame& packet) {
        buffered_packets.push_back(OwnedPacket{
            packet.session_key,
            packet.first_sequence,
            packet.declared_message_count,
            std::vector<FeedEvent>{packet.events.begin(), packet.events.end()},
            packet.local_receive_time_ns,
            packet.local_publication_time_ns,
        });
        buffered_event_count += packet.events.size();
        set_times(packet.local_receive_time_ns, packet.local_publication_time_ns);
    }

    [[nodiscard]] EngineUpdate terminal(
        FeedError error,
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns,
        const bool end_of_session = false) {
        if (error == FeedError::None) {
            error = FeedError::InvalidState;
        }
        const auto old_state = state;
        normalize_failure_times(receive_time_ns, publication_time_ns);
        clear_buffer();
        clear_stitch_proof();
        state = MarketDataState::Failed;
        auto result = update(error, RecoveryAction::Stop);
        result.state_changed = old_state != state;
        result.end_of_session = end_of_session;
        publish(result);
        return result;
    }

    [[nodiscard]] EngineUpdate recover(
        FeedError error,
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns) {
        if (error == FeedError::None) {
            error = FeedError::InvalidEvent;
        }
        if (state == MarketDataState::Failed) {
            return update(FeedError::InvalidState, RecoveryAction::Stop);
        }
        if (!clocks_valid(receive_time_ns, publication_time_ns)) {
            return terminal(
                FeedError::ClockRegression,
                receive_time_ns,
                publication_time_ns);
        }
        if (recovery_generation == std::numeric_limits<std::uint64_t>::max()) {
            return terminal(error, receive_time_ns, publication_time_ns);
        }

        const auto old_state = state;
        ++recovery_generation;
        set_times(receive_time_ns, publication_time_ns);
        clear_buffer();
        clear_stitch_proof();
        state = MarketDataState::Recovering;
        auto result = update(error, RecoveryAction::RequestFreshSnapshot);
        result.state_changed = old_state != state;
        publish(result);
        return result;
    }

    [[nodiscard]] EngineUpdate buffer_packet(const PacketFrame& packet) {
        if (packet.events.size() > config.stitch_buffer_event_capacity) {
            return recover(
                FeedError::BufferCapacityExceeded,
                packet.local_receive_time_ns,
                packet.local_publication_time_ns);
        }
        if (!buffer_can_accept(packet)) {
            auto result = recover(
                FeedError::BufferCapacityExceeded,
                packet.local_receive_time_ns,
                packet.local_publication_time_ns);
            if (result.state == MarketDataState::Recovering) {
                store_buffered(packet);
            }
            return result;
        }

        store_buffered(packet);
        return update();
    }

    FeedEngineConfig config;
    BookState book;
    MarketDataState state{MarketDataState::Snapshotting};
    std::uint64_t next_sequence{1};
    std::uint64_t feed_session_epoch{1};
    std::uint64_t recovery_generation{1};
    std::uint64_t publication_generation{};
    FeedSessionKey active_session_key{};
    std::uint64_t stitch_cut_sequence{};
    std::uint64_t stitch_high_water_sequence{};
    std::uint64_t last_receive_time_ns{};
    std::uint64_t last_publication_time_ns{};
    std::deque<OwnedPacket> buffered_packets;
    std::size_t buffered_event_count{};
};

FeedEngine::FeedEngine(FeedEngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

FeedEngine::~FeedEngine() = default;
FeedEngine::FeedEngine(FeedEngine&&) noexcept = default;
FeedEngine& FeedEngine::operator=(FeedEngine&&) noexcept = default;

EngineUpdate FeedEngine::apply_snapshot(const SnapshotImage& snapshot) {
    if (impl_->state != MarketDataState::Snapshotting &&
        impl_->state != MarketDataState::Recovering) {
        return impl_->update(FeedError::InvalidState);
    }
    if (!impl_->clocks_valid(
            snapshot.local_receive_time_ns,
            snapshot.local_publication_time_ns)) {
        return impl_->terminal(
            FeedError::ClockRegression,
            snapshot.local_receive_time_ns,
            snapshot.local_publication_time_ns);
    }
    if (snapshot.session_key == 0 || snapshot.next_sequence == 0 ||
        snapshot.live_high_water_sequence < snapshot.next_sequence) {
        return impl_->recover(
            FeedError::InvalidSnapshot,
            snapshot.local_receive_time_ns,
            snapshot.local_publication_time_ns);
    }

    if (impl_->active_session_key == 0) {
        impl_->active_session_key = snapshot.session_key;
    } else if (impl_->active_session_key != snapshot.session_key) {
        if (impl_->feed_session_epoch == std::numeric_limits<std::uint64_t>::max()) {
            return impl_->terminal(
                FeedError::NewSession,
                snapshot.local_receive_time_ns,
                snapshot.local_publication_time_ns);
        }
        impl_->active_session_key = snapshot.session_key;
        ++impl_->feed_session_epoch;
        return impl_->recover(
            FeedError::NewSession,
            snapshot.local_receive_time_ns,
            snapshot.local_publication_time_ns);
    }

    BookState candidate;
    for (const auto& event : snapshot.events) {
        if (!snapshot_event_allowed(event) ||
            apply_event(candidate, event) != EventResult::Applied) {
            return impl_->recover(
                FeedError::InvalidSnapshot,
                snapshot.local_receive_time_ns,
                snapshot.local_publication_time_ns);
        }
    }
    if (!candidate.valid()) {
        return impl_->recover(
            FeedError::InvalidSnapshot,
            snapshot.local_receive_time_ns,
            snapshot.local_publication_time_ns);
    }

    auto candidate_next_sequence = snapshot.next_sequence;
    auto live_applied = std::size_t{};
    auto live_skipped = std::size_t{};
    for (const auto& buffered : impl_->buffered_packets) {
        if (buffered.session_key != snapshot.session_key) {
            return impl_->recover(
                FeedError::NewSession,
                snapshot.local_receive_time_ns,
                snapshot.local_publication_time_ns);
        }

        const auto application =
            apply_validated_frame(candidate, candidate_next_sequence, buffered.view());
        live_applied += application.applied;
        live_skipped += application.skipped;
        if (application.error != FeedError::None) {
            auto result = impl_->recover(
                application.error,
                snapshot.local_receive_time_ns,
                snapshot.local_publication_time_ns);
            result.applied_message_count = live_applied;
            result.skipped_message_count = live_skipped;
            return result;
        }
        if (application.end_of_session) {
            auto result = impl_->terminal(
                FeedError::EndOfSession,
                snapshot.local_receive_time_ns,
                snapshot.local_publication_time_ns,
                true);
            result.applied_message_count = live_applied;
            result.skipped_message_count = live_skipped;
            return result;
        }
    }

    const auto old_state = impl_->state;
    impl_->book = std::move(candidate);
    impl_->next_sequence = candidate_next_sequence;
    impl_->stitch_cut_sequence = snapshot.next_sequence;
    impl_->stitch_high_water_sequence = snapshot.live_high_water_sequence;
    impl_->clear_buffer();
    impl_->set_times(
        snapshot.local_receive_time_ns,
        snapshot.local_publication_time_ns);
    impl_->state = MarketDataState::Stitching;
    auto result = impl_->update();
    result.state_changed = old_state != impl_->state;
    result.applied_message_count = snapshot.events.size() + live_applied;
    result.skipped_message_count = live_skipped;
    impl_->publish(result);
    return result;
}

EngineUpdate FeedEngine::apply_packet(const PacketFrame& packet) {
    if (impl_->state == MarketDataState::Failed) {
        return impl_->update(FeedError::InvalidState, RecoveryAction::Stop);
    }
    if (!impl_->clocks_valid(
            packet.local_receive_time_ns,
            packet.local_publication_time_ns)) {
        return impl_->terminal(
            FeedError::ClockRegression,
            packet.local_receive_time_ns,
            packet.local_publication_time_ns);
    }

    const auto shape_error = validate_packet_shape(packet);
    if (shape_error != FeedError::None) {
        return impl_->recover(
            shape_error,
            packet.local_receive_time_ns,
            packet.local_publication_time_ns);
    }

    if (impl_->active_session_key == 0) {
        impl_->active_session_key = packet.session_key;
    } else if (impl_->active_session_key != packet.session_key) {
        if (impl_->feed_session_epoch == std::numeric_limits<std::uint64_t>::max()) {
            return impl_->terminal(
                FeedError::NewSession,
                packet.local_receive_time_ns,
                packet.local_publication_time_ns);
        }
        impl_->active_session_key = packet.session_key;
        ++impl_->feed_session_epoch;
        auto result = impl_->recover(
            FeedError::NewSession,
            packet.local_receive_time_ns,
            packet.local_publication_time_ns);
        if (result.state == MarketDataState::Recovering &&
            packet.events.size() <= impl_->config.stitch_buffer_event_capacity) {
            impl_->store_buffered(packet);
        }
        return result;
    }

    if (impl_->state == MarketDataState::Snapshotting ||
        impl_->state == MarketDataState::Recovering) {
        if (packet.declared_message_count ==
            std::numeric_limits<std::uint16_t>::max()) {
            return impl_->terminal(
                FeedError::EndOfSession,
                packet.local_receive_time_ns,
                packet.local_publication_time_ns,
                true);
        }
        return impl_->buffer_packet(packet);
    }
    if (impl_->state != MarketDataState::Stitching &&
        impl_->state != MarketDataState::Live) {
        return impl_->update(FeedError::InvalidState);
    }

    BookState candidate = impl_->book;
    auto candidate_next_sequence = impl_->next_sequence;
    const auto application =
        apply_validated_frame(candidate, candidate_next_sequence, packet);
    if (application.error == FeedError::SequenceGap) {
        auto result = impl_->recover(
            FeedError::SequenceGap,
            packet.local_receive_time_ns,
            packet.local_publication_time_ns);
        if (result.state == MarketDataState::Recovering &&
            packet.events.size() <= impl_->config.stitch_buffer_event_capacity) {
            impl_->store_buffered(packet);
        }
        result.applied_message_count = application.applied;
        result.skipped_message_count = application.skipped;
        return result;
    }
    if (application.error != FeedError::None) {
        auto result = impl_->recover(
            application.error,
            packet.local_receive_time_ns,
            packet.local_publication_time_ns);
        result.applied_message_count = application.applied;
        result.skipped_message_count = application.skipped;
        return result;
    }

    impl_->book = std::move(candidate);
    impl_->next_sequence = candidate_next_sequence;
    impl_->set_times(
        packet.local_receive_time_ns,
        packet.local_publication_time_ns);
    if (application.end_of_session) {
        auto result = impl_->terminal(
            FeedError::EndOfSession,
            packet.local_receive_time_ns,
            packet.local_publication_time_ns,
            true);
        result.applied_message_count = application.applied;
        result.skipped_message_count = application.skipped;
        return result;
    }

    auto result = impl_->update();
    result.applied_message_count = application.applied;
    result.skipped_message_count = application.skipped;
    impl_->publish(result);
    return result;
}

EngineUpdate FeedEngine::complete_stitch(const std::uint64_t local_publication_time_ns) {
    if (impl_->state != MarketDataState::Stitching ||
        impl_->active_session_key == 0 ||
        impl_->stitch_cut_sequence == 0 ||
        impl_->stitch_high_water_sequence < impl_->stitch_cut_sequence ||
        impl_->next_sequence < impl_->stitch_high_water_sequence) {
        return impl_->update(FeedError::InvalidState);
    }
    if (local_publication_time_ns < impl_->last_receive_time_ns ||
        local_publication_time_ns < impl_->last_publication_time_ns) {
        return impl_->terminal(
            FeedError::ClockRegression,
            impl_->last_receive_time_ns,
            local_publication_time_ns);
    }

    impl_->last_publication_time_ns = local_publication_time_ns;
    impl_->state = MarketDataState::Live;
    auto result = impl_->update();
    result.state_changed = true;
    impl_->publish(result);
    return result;
}

EngineUpdate FeedEngine::continuity_lost(
    const FeedError error,
    const std::uint64_t local_receive_time_ns,
    const std::uint64_t local_publication_time_ns) {
    return impl_->recover(error, local_receive_time_ns, local_publication_time_ns);
}

EngineUpdate FeedEngine::fail(
    const FeedError error,
    const std::uint64_t local_receive_time_ns,
    const std::uint64_t local_publication_time_ns) {
    return impl_->terminal(error, local_receive_time_ns, local_publication_time_ns);
}

MarketDataState FeedEngine::state() const noexcept {
    return impl_->state;
}

std::uint64_t FeedEngine::next_sequence() const noexcept {
    return impl_->next_sequence;
}

std::uint64_t FeedEngine::last_applied_sequence() const noexcept {
    return impl_->next_sequence - 1;
}

std::uint64_t FeedEngine::feed_session_epoch() const noexcept {
    return impl_->feed_session_epoch;
}

std::uint64_t FeedEngine::recovery_generation() const noexcept {
    return impl_->recovery_generation;
}

std::uint64_t FeedEngine::publication_generation() const noexcept {
    return impl_->publication_generation;
}

FeedSessionKey FeedEngine::active_session_key() const noexcept {
    return impl_->active_session_key;
}

std::uint64_t FeedEngine::stitch_cut_sequence() const noexcept {
    return impl_->stitch_cut_sequence;
}

std::uint64_t FeedEngine::stitch_high_water_sequence() const noexcept {
    return impl_->stitch_high_water_sequence;
}

std::size_t FeedEngine::buffered_packet_count() const noexcept {
    return impl_->buffered_packets.size();
}

}  // namespace sorx::md



