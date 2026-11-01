#include "sorx/connectors/nasdaq_itch5.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace sorx::connectors::nasdaq::itch5 {
namespace {

using native_md::ControlEvent;
using native_md::ControlKind;
using native_md::DecodeBuffers;
using native_md::DecodeResult;
using native_md::DecodeStatus;
using native_md::NativeOrderState;
using native_md::RoutedL3Event;
using native_md::SequenceState;

[[nodiscard]] constexpr std::uint16_t read_be16(
    const std::byte* const source) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[0]))
         << 8U) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[1])));
}

[[nodiscard]] constexpr std::uint32_t read_be32(
    const std::byte* const source) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[0]))
            << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[1]))
            << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[2]))
            << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[3]));
}

[[nodiscard]] constexpr std::uint64_t read_be48(
    const std::byte* const source) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 6U; ++index) {
        value = (value << 8U) |
                std::to_integer<std::uint8_t>(source[index]);
    }
    return value;
}

[[nodiscard]] constexpr std::uint64_t read_be64(
    const std::byte* const source) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) |
                std::to_integer<std::uint8_t>(source[index]);
    }
    return value;
}

constexpr void write_be16(
    std::byte* const destination,
    const std::uint16_t value) noexcept {
    destination[0] = static_cast<std::byte>(value >> 8U);
    destination[1] = static_cast<std::byte>(value);
}

constexpr void write_be64(
    std::byte* const destination,
    const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        destination[index] = static_cast<std::byte>(value >> shift);
    }
}

[[nodiscard]] constexpr bool printable(const std::uint8_t value) noexcept {
    return value >= 0x20U && value <= 0x7EU;
}

[[nodiscard]] bool valid_session(const MoldSession& session) noexcept {
    return std::all_of(
        session.begin(), session.end(), [](const char value) noexcept {
            return printable(static_cast<std::uint8_t>(value));
        });
}

[[nodiscard]] bool valid_symbol_bytes(
    const std::byte* const source,
    const std::size_t length) noexcept {
    bool saw_character{};
    bool saw_padding{};
    for (std::size_t index = 0; index < length; ++index) {
        const auto value = std::to_integer<std::uint8_t>(source[index]);
        if (!printable(value)) {
            return false;
        }
        if (value == static_cast<std::uint8_t>(' ')) {
            saw_padding = true;
        } else {
            if (saw_padding) {
                return false;
            }
            saw_character = true;
        }
    }
    return saw_character;
}

[[nodiscard]] native_md::Symbol8 read_symbol(
    const std::byte* const source,
    const std::size_t length) noexcept {
    native_md::Symbol8 symbol{};
    symbol.fill(' ');
    for (std::size_t index = 0; index < length; ++index) {
        symbol[index] = static_cast<char>(
            std::to_integer<std::uint8_t>(source[index]));
    }
    return symbol;
}

[[nodiscard]] bool valid_code(
    const char value,
    const std::string_view allowed) noexcept {
    return allowed.find(value) != std::string_view::npos;
}

[[nodiscard]] std::size_t exact_itch_length(const std::uint8_t type) noexcept {
    switch (type) {
        case 'S': return 12U;
        case 'R': return 39U;
        case 'H': return 25U;
        case 'Y': return 20U;
        case 'L': return 26U;
        case 'V': return 35U;
        case 'W': return 12U;
        case 'K': return 28U;
        case 'J': return 35U;
        case 'h': return 21U;
        case 'A': return 36U;
        case 'F': return 40U;
        case 'E': return 31U;
        case 'C': return 36U;
        case 'X': return 23U;
        case 'D': return 19U;
        case 'U': return 35U;
        case 'P': return 44U;
        case 'Q': return 40U;
        case 'B': return 19U;
        case 'I': return 50U;
        case 'N': return 20U;
        case 'O': return 48U;
        default: return 0U;
    }
}

[[nodiscard]] std::size_t l3_slots(const std::uint8_t type) noexcept {
    switch (type) {
        case 'A':
        case 'F':
        case 'E':
        case 'C':
        case 'X':
        case 'D': return 1U;
        case 'U': return 2U;
        default: return 0U;
    }
}

[[nodiscard]] std::size_t control_slots(const std::uint8_t type) noexcept {
    switch (type) {
        case 'A':
        case 'F':
        case 'X':
        case 'D':
        case 'U': return 0U;
        default: return 1U;
    }
}

[[nodiscard]] bool valid_common_header(
    const std::span<const std::byte> message) noexcept {
    if (message.size() < 11U) {
        return false;
    }
    const auto timestamp = read_be48(message.data() + 5U);
    return timestamp < kNanosecondsPerDay;
}

[[nodiscard]] SequenceState classify_sequence(
    const std::uint64_t first,
    const std::uint64_t next,
    const std::uint64_t expected) noexcept {
    if (expected == 0U) {
        return SequenceState::NotChecked;
    }
    if (first == expected) {
        return SequenceState::InOrder;
    }
    if (first > expected) {
        return SequenceState::Gap;
    }
    return next <= expected ? SequenceState::Duplicate : SequenceState::Overlap;
}

[[nodiscard]] DecodeResult failed(
    const DecodeStatus status,
    const SequenceState sequence_state = SequenceState::NotChecked) noexcept {
    DecodeResult result{};
    result.status = status;
    result.sequence_state = sequence_state;
    return result;
}

[[nodiscard]] bool staged_order(
    const std::span<RoutedL3Event> events,
    const std::size_t event_count,
    const native_md::OrderLookup& lookup,
    const std::uint64_t reference,
    NativeOrderState& state) noexcept {
    for (std::size_t index = event_count; index != 0U; --index) {
        const auto& routed = events[index - 1U];
        if (routed.event.order_reference != reference) {
            continue;
        }
        if (routed.event.operation == marketdata::L3Operation::Delete) {
            return false;
        }
        state.instrument = routed.instrument;
        state.side = routed.event.side;
        state.price = routed.event.price;
        state.quantity = routed.event.quantity;
        return true;
    }
    return lookup.find(reference, state);
}

void emit_book_event(
    DecodeBuffers& destination,
    std::size_t& count,
    const InstrumentId instrument,
    const marketdata::FeedLeg leg,
    const std::uint64_t sequence,
    const marketdata::L3Operation operation,
    const std::uint64_t reference,
    const Side side,
    const Price price,
    const Quantity quantity,
    const std::int64_t timestamp) noexcept {
    destination.l3_events[count++] = RoutedL3Event{
        instrument,
        leg,
        sequence,
        marketdata::L3Event{
            operation,
            reference,
            side,
            price,
            quantity,
            timestamp,
        },
    };
}

void build_action_stream(
    DecodeBuffers& destination,
    const std::size_t l3_count,
    const std::size_t control_count) noexcept {
    std::size_t l3_index{};
    std::size_t control_index{};
    std::size_t action_index{};
    while (l3_index < l3_count || control_index < control_count) {
        const bool take_l3 =
            l3_index < l3_count &&
            (control_index == control_count ||
             destination.l3_events[l3_index].source_sequence <=
                 destination.controls[control_index].source_sequence);
        if (take_l3) {
            destination.actions[action_index++] = native_md::DecodedAction{
                native_md::DecodedActionKind::L3Event,
                destination.l3_events[l3_index].source_sequence,
                static_cast<std::uint32_t>(l3_index),
            };
            ++l3_index;
        } else {
            destination.actions[action_index++] = native_md::DecodedAction{
                native_md::DecodedActionKind::Control,
                destination.controls[control_index].source_sequence,
                static_cast<std::uint32_t>(control_index),
            };
            ++control_index;
        }
    }
}

[[nodiscard]] bool reduce_order(
    DecodeBuffers& destination,
    std::size_t& count,
    const native_md::OrderLookup& lookup,
    const InstrumentId expected_instrument,
    const marketdata::FeedLeg leg,
    const std::uint64_t sequence,
    const std::uint64_t reference,
    const std::uint64_t reduction,
    const std::int64_t timestamp,
    DecodeStatus& status) noexcept {
    NativeOrderState state{};
    if (!staged_order(destination.l3_events, count, lookup, reference, state)) {
        status = DecodeStatus::OrderStateUnavailable;
        return false;
    }
    if (expected_instrument && state.instrument != expected_instrument) {
        status = DecodeStatus::InvalidField;
        return false;
    }
    if (reduction == 0U || reduction > state.quantity.units) {
        status = DecodeStatus::QuantityUnderflow;
        return false;
    }
    if (reduction == state.quantity.units) {
        emit_book_event(
            destination,
            count,
            state.instrument,
            leg,
            sequence,
            marketdata::L3Operation::Delete,
            reference,
            state.side,
            state.price,
            Quantity{0U},
            timestamp);
    } else {
        emit_book_event(
            destination,
            count,
            state.instrument,
            leg,
            sequence,
            marketdata::L3Operation::Modify,
            reference,
            state.side,
            state.price,
            Quantity{state.quantity.units - reduction},
            timestamp);
    }
    return true;
}

}  // namespace

struct Decoder::Impl final {
    MoldSession session{};
    std::vector<SymbolBinding> symbols;
    std::array<InstrumentId, 65'536U> locates{};
    std::vector<LocateBinding> pending_updates;
    UnmappedInstrumentPolicy unmapped_policy{UnmappedInstrumentPolicy::Reject};
    std::size_t pending_update_count{};
    std::uint64_t pending_token{};
    std::uint64_t next_token{1U};
    bool valid{};

    explicit Impl(const DecoderConfig& config)
        : session(config.expected_session),
          symbols(config.symbols.begin(), config.symbols.end()),
          pending_updates(config.maximum_directory_updates_per_packet),
          unmapped_policy(config.unmapped_policy) {
        valid = valid_session(session) && !symbols.empty() &&
                !pending_updates.empty();
        std::sort(
            symbols.begin(), symbols.end(),
            [](const SymbolBinding& left, const SymbolBinding& right) noexcept {
                return left.symbol < right.symbol;
            });
        for (std::size_t index = 0; index < symbols.size(); ++index) {
            if (!symbols[index].instrument ||
                !valid_symbol_bytes(
                    reinterpret_cast<const std::byte*>(
                        symbols[index].symbol.data()),
                    symbols[index].symbol.size()) ||
                (index != 0U &&
                 symbols[index - 1U].symbol == symbols[index].symbol)) {
                valid = false;
            }
        }
        for (const auto& binding : config.initial_locates) {
            if (binding.stock_locate == 0U || !binding.instrument ||
                locates[binding.stock_locate]) {
                valid = false;
                continue;
            }
            locates[binding.stock_locate] = binding.instrument;
        }
    }

    [[nodiscard]] InstrumentId symbol_instrument(
        const native_md::Symbol8& symbol) const noexcept {
        const auto found = std::lower_bound(
            symbols.begin(), symbols.end(), symbol,
            [](const SymbolBinding& binding,
               const native_md::Symbol8& key) noexcept {
                return binding.symbol < key;
            });
        return found != symbols.end() && found->symbol == symbol
                   ? found->instrument
                   : InstrumentId{};
    }

    [[nodiscard]] InstrumentId resolve_locate(
        const std::uint16_t locate,
        const std::span<ControlEvent> controls,
        const std::size_t control_count) const noexcept {
        for (std::size_t index = control_count; index != 0U; --index) {
            const auto& control = controls[index - 1U];
            if (control.kind == ControlKind::Directory &&
                control.instrument_key == locate) {
                return control.instrument;
            }
        }
        return locates[locate];
    }
};

DecodeStatus encode_mold_rerequest(
    const MoldRerequest& request,
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < kMoldRequestBytes) {
        return DecodeStatus::Truncated;
    }
    if (!valid_session(request.session) || request.first_sequence == 0U ||
        request.message_count == 0U || request.message_count == 0xFFFFU) {
        return DecodeStatus::InvalidField;
    }
    std::memcpy(destination.data(), request.session.data(), request.session.size());
    write_be64(destination.data() + 10U, request.first_sequence);
    write_be16(destination.data() + 18U, request.message_count);
    return DecodeStatus::Ok;
}

DecodeStatus decode_glimpse_snapshot(
    const std::span<const std::byte> message,
    GlimpseSnapshotCut& snapshot) noexcept {
    if (message.size() < kGlimpseSnapshotBytes) {
        return DecodeStatus::Truncated;
    }
    if (message.size() != kGlimpseSnapshotBytes ||
        std::to_integer<std::uint8_t>(message[0]) !=
            static_cast<std::uint8_t>('G')) {
        return message.size() != kGlimpseSnapshotBytes
                   ? DecodeStatus::InvalidLength
                   : DecodeStatus::InvalidType;
    }
    std::uint64_t value{};
    bool saw_digit{};
    for (std::size_t index = 1U; index < message.size(); ++index) {
        const auto character = static_cast<char>(
            std::to_integer<std::uint8_t>(message[index]));
        if (character == ' ' && !saw_digit) {
            continue;
        }
        if (character < '0' || character > '9') {
            return DecodeStatus::InvalidField;
        }
        saw_digit = true;
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return DecodeStatus::SequenceOverflow;
        }
        value = value * 10U + digit;
    }
    if (!saw_digit || value == 0U) {
        return DecodeStatus::InvalidField;
    }
    snapshot.realtime_sequence = value;
    return DecodeStatus::Ok;
}

Decoder::Decoder(const DecoderConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

bool Decoder::configured() const noexcept {
    return impl_ != nullptr && impl_->valid;
}

void Decoder::clear_locates() noexcept {
    if (impl_ != nullptr) {
        impl_->locates.fill(InstrumentId{});
        impl_->pending_update_count = 0U;
        impl_->pending_token = 0U;
    }
}

DirectoryCommitStatus Decoder::finish_directory_commit(
    const std::uint64_t commit_token,
    const bool application_succeeded) noexcept {
    if (impl_ == nullptr || impl_->pending_token == 0U) {
        return DirectoryCommitStatus::NoPendingCommit;
    }
    if (commit_token == 0U || commit_token != impl_->pending_token) {
        return DirectoryCommitStatus::InvalidToken;
    }
    if (application_succeeded) {
        for (std::size_t index = 0U;
             index < impl_->pending_update_count;
             ++index) {
            const auto& update = impl_->pending_updates[index];
            impl_->locates[update.stock_locate] = update.instrument;
        }
    }
    impl_->pending_update_count = 0U;
    impl_->pending_token = 0U;
    return application_succeeded ? DirectoryCommitStatus::Committed
                                 : DirectoryCommitStatus::Aborted;
}

DecodeResult Decoder::decode_mold_packet(
    const std::span<const std::byte> packet,
    const DecodeContext& context,
    const native_md::OrderLookup& order_lookup,
    DecodeBuffers destination) noexcept {
    if (!configured() || context.leg == marketdata::FeedLeg::None) {
        return failed(DecodeStatus::InvalidConfiguration);
    }
    if (packet.size() < kMoldHeaderBytes) {
        return failed(DecodeStatus::Truncated);
    }
    if (std::memcmp(packet.data(), impl_->session.data(), impl_->session.size()) !=
        0) {
        return failed(DecodeStatus::InvalidSession);
    }

    const auto first_sequence = read_be64(packet.data() + 10U);
    const auto message_count = read_be16(packet.data() + 18U);
    if (first_sequence == 0U) {
        return failed(DecodeStatus::InvalidEnvelope);
    }
    const auto ordinary_count =
        message_count == 0xFFFFU ? 0U : static_cast<std::uint64_t>(message_count);
    if (first_sequence >
        std::numeric_limits<std::uint64_t>::max() - ordinary_count) {
        return failed(DecodeStatus::SequenceOverflow);
    }
    const auto next_sequence = first_sequence + ordinary_count;
    const auto sequence_state = classify_sequence(
        first_sequence, next_sequence, context.expected_sequence);

    if (message_count == 0U || message_count == 0xFFFFU) {
        if (packet.size() != kMoldHeaderBytes) {
            return failed(DecodeStatus::InvalidEnvelope, sequence_state);
        }
        if (!context.decode_payload) {
            DecodeResult result{};
            result.status = message_count == 0xFFFFU
                                ? DecodeStatus::EndOfSession
                                : DecodeStatus::Heartbeat;
            result.sequence_state = sequence_state;
            result.first_sequence = first_sequence;
            result.next_sequence = first_sequence;
            result.wire_message_count = message_count;
            result.sequence_count = 0U;
            return result;
        }
        if (destination.controls.empty() || destination.actions.empty()) {
            return failed(DecodeStatus::OutputCapacityExceeded, sequence_state);
        }
        const bool end = message_count == 0xFFFFU;
        destination.controls[0] = ControlEvent{
            end ? ControlKind::EndOfSession : ControlKind::Heartbeat,
            InstrumentId{},
            context.leg,
            first_sequence,
        };
        DecodeResult result{};
        result.status = end ? DecodeStatus::EndOfSession : DecodeStatus::Heartbeat;
        result.sequence_state = sequence_state;
        result.control_count = 1U;
        result.action_count = 1U;
        result.first_sequence = first_sequence;
        result.next_sequence = first_sequence;
        result.wire_message_count = message_count;
        result.sequence_count = 0U;
        result.payload_decoded = true;
        destination.actions[0] = native_md::DecodedAction{
            native_md::DecodedActionKind::Control,
            first_sequence,
            0U,
        };
        return result;
    }

    std::size_t cursor = kMoldHeaderBytes;
    std::size_t required_l3{};
    std::size_t required_controls{};
    std::size_t directory_count{};
    for (std::uint16_t index = 0U; index < message_count; ++index) {
        if (cursor > packet.size() || packet.size() - cursor < 2U) {
            return failed(DecodeStatus::Truncated, sequence_state);
        }
        const auto length = read_be16(packet.data() + cursor);
        cursor += 2U;
        if (length == 0U || cursor > packet.size() ||
            packet.size() - cursor < length) {
            return failed(DecodeStatus::Truncated, sequence_state);
        }
        const auto type = std::to_integer<std::uint8_t>(packet[cursor]);
        const auto exact_length = exact_itch_length(type);
        if (exact_length == 0U) {
            return failed(DecodeStatus::UnsupportedMessage, sequence_state);
        }
        if (length != exact_length) {
            return failed(DecodeStatus::InvalidLength, sequence_state);
        }
        const auto message = packet.subspan(cursor, length);
        if (!valid_common_header(message)) {
            return failed(DecodeStatus::InvalidField, sequence_state);
        }
        required_l3 += l3_slots(type);
        required_controls += control_slots(type);
        directory_count += type == 'R' ? 1U : 0U;
        cursor += length;
    }
    if (cursor != packet.size()) {
        return failed(DecodeStatus::InvalidEnvelope, sequence_state);
    }
    if (!context.decode_payload) {
        DecodeResult result{};
        result.status = DecodeStatus::Ok;
        result.sequence_state = sequence_state;
        result.first_sequence = first_sequence;
        result.next_sequence = next_sequence;
        result.wire_message_count = message_count;
        result.sequence_count = message_count;
        result.contains_book_update = required_l3 != 0U;
        return result;
    }
    if (directory_count > impl_->pending_updates.size()) {
        return failed(DecodeStatus::OutputCapacityExceeded, sequence_state);
    }
    if (directory_count != 0U && impl_->pending_token != 0U) {
        return failed(DecodeStatus::PendingCommit, sequence_state);
    }
    if (required_l3 > destination.l3_events.size() ||
        required_controls > destination.controls.size() ||
        required_l3 + required_controls > destination.actions.size() ||
        required_l3 > std::numeric_limits<std::uint32_t>::max() ||
        required_controls > std::numeric_limits<std::uint32_t>::max()) {
        return failed(DecodeStatus::OutputCapacityExceeded, sequence_state);
    }

    cursor = kMoldHeaderBytes;
    std::size_t l3_count{};
    std::size_t control_count{};
    for (std::uint16_t index = 0U; index < message_count; ++index) {
        const auto length = read_be16(packet.data() + cursor);
        cursor += 2U;
        const auto message = packet.subspan(cursor, length);
        cursor += length;
        const auto* const bytes = message.data();
        const auto type = std::to_integer<std::uint8_t>(bytes[0]);
        const auto locate = read_be16(bytes + 1U);
        const auto timestamp = static_cast<std::int64_t>(read_be48(bytes + 5U));
        const auto sequence = first_sequence + index;
        auto instrument = impl_->resolve_locate(
            locate, destination.controls, control_count);

        if (type == 'R') {
            if (locate == 0U || !valid_symbol_bytes(bytes + 11U, 8U)) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            const auto symbol = read_symbol(bytes + 11U, 8U);
            const auto mapped = impl_->symbol_instrument(symbol);
            const auto staged_or_committed = impl_->resolve_locate(
                locate, destination.controls, control_count);
            const auto market_category = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[19U]));
            const auto ipo_flag = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[31U]));
            if (!valid_code(market_category, "QGSNAPZVMF ") ||
                !valid_code(ipo_flag, "YNZ ") ||
                (staged_or_committed && mapped &&
                 staged_or_committed != mapped)) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            destination.controls[control_count++] = ControlEvent{
                ControlKind::Directory,
                mapped,
                context.leg,
                sequence,
                timestamp,
                0U,
                0U,
                0U,
                0,
                0U,
                0U,
                locate,
                0U,
                type,
                {market_category, ipo_flag, '\0', '\0'},
                symbol,
                false,
            };
            continue;
        }

        if (type == 'S') {
            const auto event_code = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[11U]));
            if (locate != 0U || !valid_code(event_code, "OSQMEC")) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            destination.controls[control_count++] = ControlEvent{
                ControlKind::SystemEvent,
                InstrumentId{},
                context.leg,
                sequence,
                timestamp,
                0U,
                0U,
                0U,
                0,
                0U,
                0U,
                0U,
                0U,
                type,
                {event_code, '\0', '\0', '\0'},
            };
            continue;
        }

        const bool mapped = static_cast<bool>(instrument);

        if (type == 'H') {
            if (!valid_symbol_bytes(bytes + 11U, 8U)) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            const auto symbol = read_symbol(bytes + 11U, 8U);
            const auto state = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[19U]));
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                !valid_code(state, "HPQT") ||
                (mapped && impl_->symbol_instrument(symbol) != instrument)) {
                return failed(
                    !mapped ? DecodeStatus::UnknownInstrument
                            : DecodeStatus::InvalidField,
                    sequence_state);
            }
            ControlEvent control{};
            control.kind = ControlKind::TradingStatus;
            control.instrument = instrument;
            control.leg = context.leg;
            control.source_sequence = sequence;
            control.exchange_timestamp_ns = timestamp;
            control.instrument_key = locate;
            control.wire_type = type;
            control.code[0] = state;
            control.symbol = symbol;
            destination.controls[control_count++] = control;
            continue;
        }

        if (type == 'A' || type == 'F') {
            const auto reference = read_be64(bytes + 11U);
            const auto side_value = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[19U]));
            const auto shares = read_be32(bytes + 20U);
            const auto price = read_be32(bytes + 32U);
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                reference == 0U || !valid_code(side_value, "BS") ||
                shares == 0U || price == 0U || price > kMaximumPriceUnits ||
                !valid_symbol_bytes(bytes + 24U, 8U)) {
                return failed(
                    !mapped ? DecodeStatus::UnknownInstrument
                            : DecodeStatus::InvalidField,
                    sequence_state);
            }
            const auto symbol = read_symbol(bytes + 24U, 8U);
            if (mapped && impl_->symbol_instrument(symbol) != instrument) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            if (mapped) {
                emit_book_event(
                    destination,
                    l3_count,
                    instrument,
                    context.leg,
                    sequence,
                    marketdata::L3Operation::Add,
                    reference,
                    side_value == 'B' ? Side::Buy : Side::Sell,
                    Price{price},
                    Quantity{shares},
                    timestamp);
            }
            continue;
        }

        if (type == 'E' || type == 'C' || type == 'X') {
            const auto reference = read_be64(bytes + 11U);
            const auto reduction = read_be32(bytes + 19U);
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                reference == 0U || reduction == 0U) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            DecodeStatus reduce_status{DecodeStatus::Ok};
            if (mapped && !reduce_order(
                              destination,
                              l3_count,
                              order_lookup,
                              instrument,
                              context.leg,
                              sequence,
                              reference,
                              reduction,
                              timestamp,
                              reduce_status)) {
                return failed(reduce_status, sequence_state);
            }
            if (type == 'E' || type == 'C') {
                ControlEvent control{};
                control.kind = ControlKind::Execution;
                control.instrument = instrument;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.order_reference = reference;
                control.match_or_execution_id = read_be64(bytes + 23U);
                control.quantity = reduction;
                control.price_units = type == 'C'
                                          ? static_cast<std::int64_t>(
                                                read_be32(bytes + 32U))
                                          : 0;
                control.instrument_key = locate;
                control.wire_type = type;
                control.printable =
                    type == 'E' ||
                    std::to_integer<std::uint8_t>(bytes[31U]) ==
                        static_cast<std::uint8_t>('Y');
                if (type == 'C' &&
                    (!valid_code(static_cast<char>(
                                     std::to_integer<std::uint8_t>(bytes[31U])),
                                 "YN") ||
                     control.price_units <= 0 ||
                     control.price_units > kMaximumPriceUnits)) {
                    return failed(DecodeStatus::InvalidField, sequence_state);
                }
                if (control.match_or_execution_id == 0U) {
                    return failed(DecodeStatus::InvalidField, sequence_state);
                }
                destination.controls[control_count++] = control;
            }
            continue;
        }

        if (type == 'D') {
            const auto reference = read_be64(bytes + 11U);
            NativeOrderState state{};
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                reference == 0U) {
                return failed(
                    !mapped ? DecodeStatus::UnknownInstrument
                            : DecodeStatus::InvalidField,
                    sequence_state);
            }
            if (mapped) {
                if (!staged_order(
                        destination.l3_events,
                        l3_count,
                        order_lookup,
                        reference,
                        state)) {
                    return failed(
                        DecodeStatus::OrderStateUnavailable, sequence_state);
                }
                if (state.instrument != instrument) {
                    return failed(DecodeStatus::InvalidField, sequence_state);
                }
                emit_book_event(
                    destination,
                    l3_count,
                    instrument,
                    context.leg,
                    sequence,
                    marketdata::L3Operation::Delete,
                    reference,
                    state.side,
                    state.price,
                    Quantity{0U},
                    timestamp);
            }
            continue;
        }

        if (type == 'U') {
            const auto old_reference = read_be64(bytes + 11U);
            const auto new_reference = read_be64(bytes + 19U);
            const auto shares = read_be32(bytes + 27U);
            const auto price = read_be32(bytes + 31U);
            NativeOrderState state{};
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                old_reference == 0U || new_reference == 0U ||
                old_reference == new_reference || shares == 0U || price == 0U ||
                price > kMaximumPriceUnits) {
                return failed(
                    !mapped ? DecodeStatus::UnknownInstrument
                            : DecodeStatus::InvalidField,
                    sequence_state);
            }
            if (mapped) {
                if (!staged_order(
                        destination.l3_events,
                        l3_count,
                        order_lookup,
                        old_reference,
                        state)) {
                    return failed(
                        DecodeStatus::OrderStateUnavailable, sequence_state);
                }
                if (state.instrument != instrument) {
                    return failed(DecodeStatus::InvalidField, sequence_state);
                }
                emit_book_event(
                    destination,
                    l3_count,
                    instrument,
                    context.leg,
                    sequence,
                    marketdata::L3Operation::Delete,
                    old_reference,
                    state.side,
                    state.price,
                    Quantity{0U},
                    timestamp);
                emit_book_event(
                    destination,
                    l3_count,
                    instrument,
                    context.leg,
                    sequence,
                    marketdata::L3Operation::Add,
                    new_reference,
                    state.side,
                    Price{price},
                    Quantity{shares},
                    timestamp);
            }
            continue;
        }

        if (type == 'P') {
            const auto shares = read_be32(bytes + 20U);
            const auto price = read_be32(bytes + 32U);
            const auto side_value = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[19U]));
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                read_be64(bytes + 11U) != 0U || side_value != 'B' ||
                shares == 0U || price == 0U ||
                price > kMaximumPriceUnits ||
                !valid_symbol_bytes(bytes + 24U, 8U)) {
                return failed(
                    !mapped ? DecodeStatus::UnknownInstrument
                            : DecodeStatus::InvalidField,
                    sequence_state);
            }
            ControlEvent control{};
            control.kind = ControlKind::Trade;
            control.instrument = instrument;
            control.leg = context.leg;
            control.source_sequence = sequence;
            control.exchange_timestamp_ns = timestamp;
            control.order_reference = read_be64(bytes + 11U);
            control.match_or_execution_id = read_be64(bytes + 36U);
            control.quantity = shares;
            control.price_units = price;
            control.instrument_key = locate;
            control.wire_type = type;
            control.symbol = read_symbol(bytes + 24U, 8U);
            control.printable = true;
            if (control.match_or_execution_id == 0U ||
                (mapped && impl_->symbol_instrument(control.symbol) != instrument)) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            destination.controls[control_count++] = control;
            continue;
        }

        if (type == 'Q') {
            const auto shares = read_be64(bytes + 11U);
            const auto price = read_be32(bytes + 27U);
            const auto cross_type = static_cast<char>(
                std::to_integer<std::uint8_t>(bytes[39U]));
            if ((!mapped && impl_->unmapped_policy ==
                                UnmappedInstrumentPolicy::Reject) ||
                price == 0U || price > kMaximumPriceUnits ||
                !valid_code(cross_type, "OCHI") ||
                !valid_symbol_bytes(bytes + 19U, 8U)) {
                return failed(
                    !mapped ? DecodeStatus::UnknownInstrument
                            : DecodeStatus::InvalidField,
                    sequence_state);
            }
            ControlEvent control{};
            control.kind = ControlKind::Trade;
            control.instrument = instrument;
            control.leg = context.leg;
            control.source_sequence = sequence;
            control.exchange_timestamp_ns = timestamp;
            control.match_or_execution_id = read_be64(bytes + 31U);
            control.quantity = shares;
            control.price_units = price;
            control.instrument_key = locate;
            control.wire_type = type;
            control.code[0] = cross_type;
            control.symbol = read_symbol(bytes + 19U, 8U);
            control.printable = true;
            if (control.match_or_execution_id == 0U ||
                (mapped && impl_->symbol_instrument(control.symbol) != instrument)) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            destination.controls[control_count++] = control;
            continue;
        }

        if (type == 'B') {
            ControlEvent control{};
            control.kind = ControlKind::BrokenTrade;
            control.instrument = instrument;
            control.leg = context.leg;
            control.source_sequence = sequence;
            control.exchange_timestamp_ns = timestamp;
            control.match_or_execution_id = read_be64(bytes + 11U);
            control.instrument_key = locate;
            control.wire_type = type;
            if (control.match_or_execution_id == 0U) {
                return failed(DecodeStatus::InvalidField, sequence_state);
            }
            destination.controls[control_count++] = control;
            continue;
        }

        ControlEvent control{};
        control.kind = ControlKind::Administrative;
        control.instrument = instrument;
        control.leg = context.leg;
        control.source_sequence = sequence;
        control.exchange_timestamp_ns = timestamp;
        control.instrument_key = locate;
        control.wire_type = type;
        destination.controls[control_count++] = control;
    }

    build_action_stream(destination, l3_count, control_count);

    DecodeResult result{};
    result.status = DecodeStatus::Ok;
    result.sequence_state = sequence_state;
    result.l3_event_count = l3_count;
    result.control_count = control_count;
    result.action_count = l3_count + control_count;
    result.first_sequence = first_sequence;
    result.next_sequence = next_sequence;
    result.wire_message_count = message_count;
    result.sequence_count = message_count;
    result.contains_book_update = l3_count != 0U;
    result.payload_decoded = true;
    if (directory_count != 0U) {
        impl_->pending_update_count = 0U;
        for (std::size_t index = 0U; index < control_count; ++index) {
            const auto& control = destination.controls[index];
            if (control.kind == ControlKind::Directory && control.instrument) {
                impl_->pending_updates[impl_->pending_update_count++] =
                    LocateBinding{control.instrument_key, control.instrument};
            }
        }
        if (impl_->pending_update_count != 0U) {
            impl_->pending_token = impl_->next_token++;
            if (impl_->pending_token == 0U) {
                impl_->pending_token = impl_->next_token++;
            }
            result.commit_token = impl_->pending_token;
        }
    }
    return result;
}

}  // namespace sorx::connectors::nasdaq::itch5
