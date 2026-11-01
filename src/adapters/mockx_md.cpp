#include "sorx/adapters/mockx_md.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sorx::adapters {
namespace {

constexpr std::size_t kMoldHeaderSize = 20;
constexpr std::size_t kSystemEventSize = 12;
constexpr std::size_t kStockDirectorySize = 39;
constexpr std::size_t kTradingActionSize = 25;
constexpr std::size_t kAddOrderSize = 36;
constexpr std::size_t kOrderExecutedSize = 31;
constexpr std::size_t kOrderExecutedWithPriceSize = 36;
constexpr std::size_t kOrderDeleteSize = 19;
constexpr std::size_t kEndSnapshotSize = 21;
constexpr std::uint64_t kNanosecondsPerDay = 86'400'000'000'000ULL;

enum class DecodeContext : std::uint8_t {
    Glimpse,
    Live,
};

enum class RawInputKind : std::uint8_t {
    Mold,
    Soup,
};

struct RawInputView final {
    RawInputKind kind{RawInputKind::Mold};
    std::span<const std::byte> bytes;
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};
};

struct PendingLoss final {
    md::FeedError error{md::FeedError::None};
    std::uint64_t local_receive_time_ns{};
    std::uint64_t local_publication_time_ns{};
};

[[nodiscard]] std::uint64_t load_be(const std::span<const std::byte> bytes) noexcept {
    std::uint64_t result{};
    for (const auto byte : bytes) {
        result = (result << 8U) | std::to_integer<std::uint8_t>(byte);
    }
    return result;
}

[[nodiscard]] std::string trim_fixed_ascii(const std::span<const std::byte> bytes) {
    auto first = std::size_t{};
    auto last = bytes.size();
    while (first < last && bytes[first] == std::byte{' '}) {
        ++first;
    }
    while (last > first && bytes[last - 1] == std::byte{' '}) {
        --last;
    }
    if (first == last) {
        throw std::invalid_argument("fixed-width ASCII field is empty");
    }

    std::string result;
    result.reserve(last - first);
    for (auto index = first; index < last; ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[index]);
        if (value < 0x21U || value > 0x7eU) {
            throw std::invalid_argument("fixed-width field contains invalid ASCII");
        }
        result.push_back(static_cast<char>(value));
    }
    return result;
}

[[nodiscard]] std::uint64_t parse_padded_decimal(
    const std::span<const std::byte> bytes) {
    auto first = std::size_t{};
    while (first < bytes.size() && bytes[first] == std::byte{' '}) {
        ++first;
    }
    if (first == bytes.size()) {
        throw std::invalid_argument("padded decimal is empty");
    }

    std::uint64_t result{};
    for (auto index = first; index < bytes.size(); ++index) {
        const auto character =
            static_cast<char>(std::to_integer<unsigned char>(bytes[index]));
        if (character < '0' || character > '9') {
            throw std::invalid_argument("padded decimal contains a non-digit");
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw std::invalid_argument("padded decimal overflows uint64");
        }
        result = result * 10U + digit;
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t> expected_itch_size(
    const char type) noexcept {
    switch (type) {
    case 'S':
        return kSystemEventSize;
    case 'R':
        return kStockDirectorySize;
    case 'H':
        return kTradingActionSize;
    case 'A':
        return kAddOrderSize;
    case 'E':
        return kOrderExecutedSize;
    case 'C':
        return kOrderExecutedWithPriceSize;
    case 'D':
        return kOrderDeleteSize;
    case 'G':
        return kEndSnapshotSize;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::int64_t decode_timestamp(
    const std::span<const std::byte> message) {
    const auto timestamp = load_be(message.subspan(5, 6));
    if (timestamp >= kNanosecondsPerDay ||
        timestamp > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("ITCH timestamp is outside the trading day");
    }
    return static_cast<std::int64_t>(timestamp);
}

class SpscRawQueue final {
public:
    SpscRawQueue(const std::size_t capacity, const std::size_t max_input_bytes)
        : capacity_(capacity),
          max_input_bytes_(max_input_bytes),
          storage_(checked_storage_size(capacity, max_input_bytes)),
          slots_(capacity) {}

    [[nodiscard]] MockXEnqueueResult try_push(
        const RawInputKind kind,
        const std::span<const std::byte> bytes,
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns) noexcept {
        if (publication_time_ns < receive_time_ns) {
            note_loss(md::FeedError::ClockRegression, receive_time_ns, publication_time_ns);
            return MockXEnqueueResult::InvalidTimestamp;
        }
        if (bytes.size() > max_input_bytes_) {
            note_loss(md::FeedError::TruncatedDatagram, receive_time_ns, publication_time_ns);
            return MockXEnqueueResult::InputTooLarge;
        }

        const auto producer = producer_.load(std::memory_order_relaxed);
        const auto consumer = consumer_.load(std::memory_order_acquire);
        if (producer - consumer >= capacity_) {
            note_loss(md::FeedError::RingContinuityLoss, receive_time_ns, publication_time_ns);
            return MockXEnqueueResult::QueueFull;
        }

        const auto slot_index = producer % capacity_;
        auto& slot = slots_[slot_index];
        auto* destination = storage_.data() + slot_index * max_input_bytes_;
        if (!bytes.empty()) {
            std::memcpy(destination, bytes.data(), bytes.size());
        }
        slot.kind = kind;
        slot.size = bytes.size();
        slot.local_receive_time_ns = receive_time_ns;
        slot.local_publication_time_ns = publication_time_ns;
        producer_.store(producer + 1, std::memory_order_release);
        return MockXEnqueueResult::Accepted;
    }

    [[nodiscard]] std::optional<RawInputView> try_peek() const noexcept {
        const auto consumer = consumer_.load(std::memory_order_relaxed);
        const auto producer = producer_.load(std::memory_order_acquire);
        if (consumer == producer) {
            return std::nullopt;
        }
        const auto slot_index = consumer % capacity_;
        const auto& slot = slots_[slot_index];
        return RawInputView{
            slot.kind,
            std::span<const std::byte>{
                storage_.data() + slot_index * max_input_bytes_,
                slot.size,
            },
            slot.local_receive_time_ns,
            slot.local_publication_time_ns,
        };
    }

    void release() noexcept {
        const auto consumer = consumer_.load(std::memory_order_relaxed);
        consumer_.store(consumer + 1, std::memory_order_release);
    }

    void discard_all() noexcept {
        const auto producer = producer_.load(std::memory_order_acquire);
        consumer_.store(producer, std::memory_order_release);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto producer = producer_.load(std::memory_order_acquire);
        const auto consumer = consumer_.load(std::memory_order_acquire);
        return producer - consumer;
    }

    [[nodiscard]] std::optional<PendingLoss> pending_loss() noexcept {
        if (!loss_pending_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const auto result = loss_;
        loss_pending_.store(false, std::memory_order_release);
        return result;
    }

private:
    struct Slot final {
        RawInputKind kind{RawInputKind::Mold};
        std::size_t size{};
        std::uint64_t local_receive_time_ns{};
        std::uint64_t local_publication_time_ns{};
    };

    [[nodiscard]] static std::size_t checked_storage_size(
        const std::size_t capacity,
        const std::size_t max_input_bytes) {
        if (capacity == 0 || max_input_bytes == 0 ||
            capacity > std::numeric_limits<std::size_t>::max() / max_input_bytes) {
            throw std::invalid_argument("invalid MockX raw queue dimensions");
        }
        return capacity * max_input_bytes;
    }

    void note_loss(
        const md::FeedError error,
        const std::uint64_t receive_time_ns,
        const std::uint64_t publication_time_ns) noexcept {
        // The seam is SPSC: while pending is true the sole producer never
        // rewrites this record, so the sole consumer can copy then release it.
        if (!loss_pending_.load(std::memory_order_acquire)) {
            loss_ = PendingLoss{error, receive_time_ns, publication_time_ns};
            loss_pending_.store(true, std::memory_order_release);
        }
    }

    std::size_t capacity_{};
    std::size_t max_input_bytes_{};
    std::vector<std::byte> storage_;
    std::vector<Slot> slots_;
    std::atomic<std::size_t> producer_{};
    std::atomic<std::size_t> consumer_{};
    std::atomic<bool> loss_pending_{};
    PendingLoss loss_{};
};

struct BindingValue final {
    std::string symbol;
    InstrumentId instrument{};
};

struct ItchDecode final {
    md::FeedError error{md::FeedError::None};
    std::optional<md::FeedEvent> event;
    std::uint64_t snapshot_cut{};
};

}  // namespace

md::FeedSessionKey mockx_session_key(const std::string_view session) {
    if (session.empty() || session.size() > 10) {
        throw std::invalid_argument("MockX Mold session must contain 1-10 ASCII bytes");
    }
    auto first = std::size_t{};
    auto last = session.size();
    while (first < last && session[first] == ' ') {
        ++first;
    }
    while (last > first && session[last - 1] == ' ') {
        --last;
    }
    if (first == last) {
        throw std::invalid_argument("MockX Mold session must not be blank");
    }

    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    auto result = offset;
    for (auto index = first; index < last; ++index) {
        const auto value = static_cast<unsigned char>(session[index]);
        if (value < 0x21U || value > 0x7eU) {
            throw std::invalid_argument("MockX Mold session contains invalid ASCII");
        }
        result ^= value;
        result *= prime;
    }
    return result == 0 ? 1 : result;
}

struct MockXMdAdapter::Impl final {
    explicit Impl(MockXMdConfig value)
        : config(std::move(value)),
          engine(config.feed),
          queue(config.raw_queue_capacity, config.max_raw_input_bytes) {
        validate_config();
        soup_buffer.reserve(config.max_soup_buffer_bytes);
        glimpse_events.reserve(std::min<std::size_t>(
            config.max_glimpse_messages,
            4'096));
    }

    void validate_config() {
        if (config.max_raw_input_bytes < kMoldHeaderSize ||
            config.max_messages_per_mold_packet == 0 ||
            config.max_messages_per_mold_packet >=
                std::numeric_limits<std::uint16_t>::max() ||
            config.max_glimpse_messages == 0 ||
            config.max_soup_buffer_bytes < 3) {
            throw std::invalid_argument("invalid MockX MD profile configuration");
        }

        std::unordered_set<std::uint32_t> instrument_ids;
        std::unordered_set<std::string> symbols;
        for (const auto& binding : config.instruments) {
            if (binding.stock_locate == 0 || !binding.instrument ||
                binding.symbol.empty() || binding.symbol.size() > 8) {
                throw std::invalid_argument("invalid MockX instrument binding");
            }
            for (const auto character : binding.symbol) {
                const auto value = static_cast<unsigned char>(character);
                if (value < 0x21U || value > 0x7eU) {
                    throw std::invalid_argument("MockX symbol contains invalid ASCII");
                }
            }
            if (!instrument_ids.insert(binding.instrument.value()).second ||
                !symbols.insert(binding.symbol).second ||
                bindings.contains(binding.stock_locate)) {
                throw std::invalid_argument("duplicate MockX instrument binding");
            }
            bindings.emplace(
                binding.stock_locate,
                BindingValue{binding.symbol, binding.instrument});
        }
        if (bindings.empty()) {
            throw std::invalid_argument("MockX instrument bindings must not be empty");
        }
    }

    [[nodiscard]] ItchDecode decode_itch(
        const std::span<const std::byte> message,
        const DecodeContext context) const {
        if (message.empty()) {
            return ItchDecode{md::FeedError::MalformedFrame};
        }
        const auto type =
            static_cast<char>(std::to_integer<unsigned char>(message.front()));
        const auto expected = expected_itch_size(type);
        if (!expected) {
            return ItchDecode{md::FeedError::UnsupportedMessage};
        }
        if (message.size() != *expected) {
            return ItchDecode{md::FeedError::MalformedFrame};
        }

        try {
            const auto timestamp = type == 'G' ? 0 : decode_timestamp(message);
            const auto locate = type == 'G' || type == 'S'
                ? std::uint16_t{}
                : static_cast<std::uint16_t>(load_be(message.subspan(1, 2)));
            const auto binding = locate == 0 ? bindings.end() : bindings.find(locate);
            if (locate != 0 && binding == bindings.end()) {
                return ItchDecode{md::FeedError::InvalidEvent};
            }

            switch (type) {
            case 'S': {
                if (context != DecodeContext::Live) {
                    return ItchDecode{md::FeedError::UnsupportedMessage};
                }
                const auto code =
                    static_cast<char>(std::to_integer<unsigned char>(message[11]));
                constexpr std::string_view valid_codes{"OSQMEC"};
                if (valid_codes.find(code) == std::string_view::npos) {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                if (code == 'C') {
                    return ItchDecode{
                        md::FeedError::None,
                        md::EndOfSessionEvent{timestamp},
                    };
                }
                return ItchDecode{
                    md::FeedError::None,
                    md::KnownBookNeutralEvent{timestamp},
                };
            }
            case 'R': {
                const auto symbol = trim_fixed_ascii(message.subspan(11, 8));
                if (symbol != binding->second.symbol) {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                return ItchDecode{
                    md::FeedError::None,
                    md::InstrumentDirectoryEvent{
                        locate,
                        binding->second.instrument,
                        timestamp,
                    },
                };
            }
            case 'H': {
                const auto symbol = trim_fixed_ascii(message.subspan(11, 8));
                if (symbol != binding->second.symbol) {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                const auto state_code =
                    static_cast<char>(std::to_integer<unsigned char>(message[19]));
                InstrumentState state{InstrumentState::Unknown};
                if (state_code == 'Q' || state_code == 'T') {
                    state = InstrumentState::Trading;
                } else if (state_code == 'H' || state_code == 'P') {
                    state = InstrumentState::Halted;
                } else {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                return ItchDecode{
                    md::FeedError::None,
                    md::TradingStateEvent{
                        locate,
                        binding->second.instrument,
                        state,
                        timestamp,
                    },
                };
            }
            case 'A': {
                const auto symbol = trim_fixed_ascii(message.subspan(24, 8));
                if (symbol != binding->second.symbol) {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                const auto side_code =
                    static_cast<char>(std::to_integer<unsigned char>(message[19]));
                Side side{Side::Buy};
                if (side_code == 'B') {
                    side = Side::Buy;
                } else if (side_code == 'S') {
                    side = Side::Sell;
                } else {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                const auto price = load_be(message.subspan(32, 4));
                if (price > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                    return ItchDecode{md::FeedError::InvalidEvent};
                }
                return ItchDecode{
                    md::FeedError::None,
                    md::AddOrderEvent{
                        load_be(message.subspan(11, 8)),
                        locate,
                        binding->second.instrument,
                        side,
                        Quantity{load_be(message.subspan(20, 4))},
                        Price{static_cast<std::int64_t>(price)},
                        timestamp,
                    },
                };
            }
            case 'E':
            case 'C': {
                if (context != DecodeContext::Live) {
                    return ItchDecode{md::FeedError::UnsupportedMessage};
                }
                if (type == 'C') {
                    const auto printable = static_cast<char>(
                        std::to_integer<unsigned char>(message[31]));
                    if (printable != 'Y' && printable != 'N') {
                        return ItchDecode{md::FeedError::InvalidEvent};
                    }
                }
                return ItchDecode{
                    md::FeedError::None,
                    md::ExecuteOrderEvent{
                        load_be(message.subspan(11, 8)),
                        locate,
                        Quantity{load_be(message.subspan(19, 4))},
                        timestamp,
                    },
                };
            }
            case 'D': {
                if (context != DecodeContext::Live) {
                    return ItchDecode{md::FeedError::UnsupportedMessage};
                }
                return ItchDecode{
                    md::FeedError::None,
                    md::DeleteOrderEvent{
                        load_be(message.subspan(11, 8)),
                        locate,
                        timestamp,
                    },
                };
            }
            case 'G': {
                if (context != DecodeContext::Glimpse) {
                    return ItchDecode{md::FeedError::UnsupportedMessage};
                }
                const auto cut = parse_padded_decimal(message.subspan(1, 20));
                if (cut == 0) {
                    return ItchDecode{md::FeedError::InvalidSnapshot};
                }
                return ItchDecode{md::FeedError::None, std::nullopt, cut};
            }
            default:
                return ItchDecode{md::FeedError::UnsupportedMessage};
            }
        } catch (const std::invalid_argument&) {
            return ItchDecode{md::FeedError::InvalidEvent};
        }
    }

    void maybe_complete_stitch(
        const std::uint64_t publication_time_ns,
        std::vector<md::EngineUpdate>& updates) {
        if (engine.state() == MarketDataState::Stitching &&
            engine.stitch_high_water_sequence() != 0 &&
            engine.next_sequence() >= engine.stitch_high_water_sequence()) {
            updates.push_back(engine.complete_stitch(publication_time_ns));
        }
    }

    void process_mold(
        const RawInputView& input,
        std::vector<md::EngineUpdate>& updates) {
        if (input.bytes.size() < kMoldHeaderSize) {
            updates.push_back(engine.continuity_lost(
                md::FeedError::TruncatedDatagram,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return;
        }

        std::string session;
        md::FeedSessionKey session_key{};
        try {
            session = trim_fixed_ascii(input.bytes.first(10));
            session_key = mockx_session_key(session);
        } catch (const std::invalid_argument&) {
            updates.push_back(engine.continuity_lost(
                md::FeedError::MalformedFrame,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return;
        }

        const auto first_sequence = load_be(input.bytes.subspan(10, 8));
        const auto message_count =
            static_cast<std::uint16_t>(load_be(input.bytes.subspan(18, 2)));
        if (first_sequence == 0) {
            updates.push_back(engine.continuity_lost(
                md::FeedError::MalformedFrame,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return;
        }

        const bool control = message_count == 0 ||
            message_count == std::numeric_limits<std::uint16_t>::max();
        if (control) {
            if (input.bytes.size() != kMoldHeaderSize) {
                updates.push_back(engine.continuity_lost(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return;
            }
            updates.push_back(engine.apply_packet(md::PacketFrame{
                session_key,
                first_sequence,
                message_count,
                {},
                input.local_receive_time_ns,
                input.local_publication_time_ns,
            }));
            maybe_complete_stitch(input.local_publication_time_ns, updates);
            return;
        }
        if (message_count > config.max_messages_per_mold_packet ||
            message_count >
                std::numeric_limits<std::uint64_t>::max() - first_sequence) {
            updates.push_back(engine.continuity_lost(
                md::FeedError::MalformedFrame,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return;
        }

        std::vector<md::FeedEvent> events;
        events.reserve(message_count);
        auto offset = kMoldHeaderSize;
        for (auto index = std::uint16_t{}; index < message_count; ++index) {
            if (input.bytes.size() - offset < 2) {
                updates.push_back(engine.continuity_lost(
                    md::FeedError::TruncatedDatagram,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return;
            }
            const auto length = static_cast<std::size_t>(
                load_be(input.bytes.subspan(offset, 2)));
            offset += 2;
            if (length == 0) {
                updates.push_back(engine.continuity_lost(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return;
            }
            if (length > input.bytes.size() - offset) {
                updates.push_back(engine.continuity_lost(
                    md::FeedError::TruncatedDatagram,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return;
            }

            const auto decoded = decode_itch(
                input.bytes.subspan(offset, length),
                DecodeContext::Live);
            if (decoded.error != md::FeedError::None || !decoded.event) {
                updates.push_back(engine.fail(
                    decoded.error == md::FeedError::None
                        ? md::FeedError::InvalidEvent
                        : decoded.error,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return;
            }
            events.push_back(*decoded.event);
            offset += length;
        }
        if (offset != input.bytes.size()) {
            updates.push_back(engine.continuity_lost(
                md::FeedError::MalformedFrame,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return;
        }

        updates.push_back(engine.apply_packet(md::PacketFrame{
            session_key,
            first_sequence,
            message_count,
            events,
            input.local_receive_time_ns,
            input.local_publication_time_ns,
        }));
        maybe_complete_stitch(input.local_publication_time_ns, updates);
    }

    [[nodiscard]] bool process_soup_packet(
        const char type,
        const std::span<const std::byte> payload,
        const RawInputView& input,
        std::vector<md::EngineUpdate>& updates) {
        if (!glimpse_active) {
            updates.push_back(engine.fail(
                md::FeedError::InvalidState,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return false;
        }

        switch (type) {
        case 'A': {
            if (glimpse_login_accepted || payload.size() != 30) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            try {
                (void)trim_fixed_ascii(payload.first(10));
                if (parse_padded_decimal(payload.subspan(10, 20)) == 0) {
                    throw std::invalid_argument("Soup sequence is zero");
                }
            } catch (const std::invalid_argument&) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            glimpse_login_accepted = true;
            return true;
        }
        case 'J':
            if (payload.size() != 1 ||
                (payload.front() != std::byte{'A'} &&
                 payload.front() != std::byte{'S'})) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
            } else {
                updates.push_back(engine.fail(
                    md::FeedError::TransportFailure,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
            }
            return false;
        case 'H':
            if (!payload.empty()) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            return true;
        case '+':
        case 'U':
            return true;
        case 'S': {
            if (!glimpse_login_accepted || glimpse_cut != 0 || payload.empty() ||
                glimpse_message_count >= config.max_glimpse_messages) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            const auto decoded = decode_itch(payload, DecodeContext::Glimpse);
            if (decoded.error != md::FeedError::None) {
                updates.push_back(engine.fail(
                    decoded.error,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            ++glimpse_message_count;
            if (decoded.snapshot_cut != 0) {
                glimpse_cut = decoded.snapshot_cut;
            } else if (decoded.event) {
                glimpse_events.push_back(*decoded.event);
            } else {
                updates.push_back(engine.fail(
                    md::FeedError::InvalidSnapshot,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            return true;
        }
        case 'Z': {
            if (!glimpse_login_accepted || !payload.empty() || glimpse_cut == 0) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                return false;
            }
            updates.push_back(engine.apply_snapshot(md::SnapshotImage{
                glimpse_events,
                glimpse_cut,
                input.local_receive_time_ns,
                input.local_publication_time_ns,
                glimpse_session_key,
                glimpse_high_water_sequence,
            }));
            glimpse_active = false;
            glimpse_login_accepted = false;
            glimpse_events.clear();
            glimpse_message_count = 0;
            glimpse_cut = 0;
            maybe_complete_stitch(input.local_publication_time_ns, updates);
            return true;
        }
        default:
            updates.push_back(engine.fail(
                md::FeedError::UnsupportedMessage,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            return false;
        }
    }

    void process_soup(
        const RawInputView& input,
        std::vector<md::EngineUpdate>& updates) {
        if (input.bytes.size() > config.max_soup_buffer_bytes - soup_buffer.size()) {
            updates.push_back(engine.fail(
                md::FeedError::BufferCapacityExceeded,
                input.local_receive_time_ns,
                input.local_publication_time_ns));
            glimpse_active = false;
            soup_buffer.clear();
            return;
        }
        soup_buffer.insert(
            soup_buffer.end(),
            input.bytes.begin(),
            input.bytes.end());

        auto consumed = std::size_t{};
        while (soup_buffer.size() - consumed >= 2) {
            const auto length = static_cast<std::size_t>(load_be(
                std::span<const std::byte>{soup_buffer}.subspan(consumed, 2)));
            if (length == 0) {
                updates.push_back(engine.fail(
                    md::FeedError::MalformedFrame,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                glimpse_active = false;
                soup_buffer.clear();
                return;
            }
            if (length > config.max_soup_buffer_bytes - 2) {
                updates.push_back(engine.fail(
                    md::FeedError::BufferCapacityExceeded,
                    input.local_receive_time_ns,
                    input.local_publication_time_ns));
                glimpse_active = false;
                soup_buffer.clear();
                return;
            }
            if (soup_buffer.size() - consumed < length + 2) {
                break;
            }

            const auto type = static_cast<char>(
                std::to_integer<unsigned char>(soup_buffer[consumed + 2]));
            const auto payload = std::span<const std::byte>{soup_buffer}.subspan(
                consumed + 3,
                length - 1);
            consumed += length + 2;
            if (!process_soup_packet(type, payload, input, updates)) {
                soup_buffer.clear();
                glimpse_active = false;
                return;
            }
        }

        if (consumed != 0) {
            soup_buffer.erase(
                soup_buffer.begin(),
                soup_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
    }

    MockXMdConfig config;
    md::FeedEngine engine;
    SpscRawQueue queue;
    std::unordered_map<std::uint16_t, BindingValue> bindings;
    std::vector<std::byte> soup_buffer;
    bool glimpse_active{};
    bool glimpse_login_accepted{};
    md::FeedSessionKey glimpse_session_key{};
    std::uint64_t glimpse_high_water_sequence{};
    std::uint64_t glimpse_cut{};
    std::size_t glimpse_message_count{};
    std::vector<md::FeedEvent> glimpse_events;
};

MockXMdAdapter::MockXMdAdapter(MockXMdConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

MockXMdAdapter::~MockXMdAdapter() = default;

void MockXMdAdapter::begin_glimpse(
    const md::FeedSessionKey live_session_key,
    const std::uint64_t live_high_water_sequence) {
    if (live_session_key == 0 || live_high_water_sequence == 0) {
        throw std::invalid_argument("GLIMPSE live identity must be positive");
    }
    if (impl_->glimpse_active || !impl_->soup_buffer.empty()) {
        throw std::logic_error("a GLIMPSE stream is already active");
    }
    if (impl_->engine.state() != MarketDataState::Snapshotting &&
        impl_->engine.state() != MarketDataState::Recovering) {
        throw std::logic_error("GLIMPSE requires SNAPSHOTTING or RECOVERING");
    }

    impl_->glimpse_active = true;
    impl_->glimpse_login_accepted = false;
    impl_->glimpse_session_key = live_session_key;
    impl_->glimpse_high_water_sequence = live_high_water_sequence;
    impl_->glimpse_cut = 0;
    impl_->glimpse_message_count = 0;
    impl_->glimpse_events.clear();
}

MockXEnqueueResult MockXMdAdapter::try_enqueue_mold_datagram(
    const std::span<const std::byte> bytes,
    const std::uint64_t local_receive_time_ns,
    const std::uint64_t local_publication_time_ns) noexcept {
    return impl_->queue.try_push(
        RawInputKind::Mold,
        bytes,
        local_receive_time_ns,
        local_publication_time_ns);
}

MockXEnqueueResult MockXMdAdapter::try_enqueue_soup_bytes(
    const std::span<const std::byte> bytes,
    const std::uint64_t local_receive_time_ns,
    const std::uint64_t local_publication_time_ns) noexcept {
    return impl_->queue.try_push(
        RawInputKind::Soup,
        bytes,
        local_receive_time_ns,
        local_publication_time_ns);
}

std::vector<md::EngineUpdate> MockXMdAdapter::drain(const std::size_t max_inputs) {
    std::vector<md::EngineUpdate> updates;
    if (const auto loss = impl_->queue.pending_loss()) {
        // The reported loss occurred after every currently committed slot.
        // Discard those older bytes before advancing the engine's clock to the
        // loss timestamp; their continuity can no longer be advertised.
        impl_->queue.discard_all();
        updates.push_back(impl_->engine.continuity_lost(
            loss->error,
            loss->local_receive_time_ns,
            loss->local_publication_time_ns));
    }

    auto consumed = std::size_t{};
    while (consumed < max_inputs) {
        const auto input = impl_->queue.try_peek();
        if (!input) {
            break;
        }
        if (impl_->engine.state() != MarketDataState::Failed) {
            if (input->kind == RawInputKind::Mold) {
                impl_->process_mold(*input, updates);
            } else {
                impl_->process_soup(*input, updates);
            }
        }
        impl_->queue.release();
        ++consumed;
    }
    return updates;
}

md::EngineUpdate MockXMdAdapter::continuity_lost(
    const md::FeedError error,
    const std::uint64_t local_receive_time_ns,
    const std::uint64_t local_publication_time_ns) {
    impl_->queue.discard_all();
    impl_->soup_buffer.clear();
    impl_->glimpse_active = false;
    impl_->glimpse_login_accepted = false;
    impl_->glimpse_session_key = 0;
    impl_->glimpse_high_water_sequence = 0;
    impl_->glimpse_cut = 0;
    impl_->glimpse_message_count = 0;
    impl_->glimpse_events.clear();
    return impl_->engine.continuity_lost(
        error,
        local_receive_time_ns,
        local_publication_time_ns);
}

const md::FeedEngine& MockXMdAdapter::feed_engine() const noexcept {
    return impl_->engine;
}

std::size_t MockXMdAdapter::queued_input_count() const noexcept {
    return impl_->queue.size();
}

bool MockXMdAdapter::glimpse_in_progress() const noexcept {
    return impl_->glimpse_active;
}

}  // namespace sorx::adapters






