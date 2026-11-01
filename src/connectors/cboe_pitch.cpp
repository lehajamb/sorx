#include "sorx/connectors/cboe_pitch.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace sorx::connectors::cboe::pitch {
namespace {

using native_md::ControlEvent;
using native_md::ControlKind;
using native_md::DecodeBuffers;
using native_md::DecodeStatus;
using native_md::NativeOrderState;
using native_md::RoutedL3Event;
using native_md::SequenceState;

[[nodiscard]] constexpr std::uint16_t read_le16(
    const std::byte* const source) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[0])) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[1]))
         << 8U));
}

[[nodiscard]] constexpr std::uint32_t read_le32(
    const std::byte* const source) noexcept {
    return static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(source[0])) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[1]))
            << 8U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[2]))
            << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[3]))
            << 24U);
}

[[nodiscard]] constexpr std::uint64_t read_le64(
    const std::byte* const source) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(source[index]))
                 << static_cast<unsigned>(index * 8U);
    }
    return value;
}

constexpr void write_le16(
    std::byte* const destination,
    const std::uint16_t value) noexcept {
    destination[0] = static_cast<std::byte>(value);
    destination[1] = static_cast<std::byte>(value >> 8U);
}

constexpr void write_le32(
    std::byte* const destination,
    const std::uint32_t value) noexcept {
    destination[0] = static_cast<std::byte>(value);
    destination[1] = static_cast<std::byte>(value >> 8U);
    destination[2] = static_cast<std::byte>(value >> 16U);
    destination[3] = static_cast<std::byte>(value >> 24U);
}

[[nodiscard]] constexpr bool printable(const std::uint8_t value) noexcept {
    return value >= 0x20U && value <= 0x7EU;
}

[[nodiscard]] bool valid_symbol_bytes(
    const std::byte* const source,
    const std::size_t length) noexcept {
    bool saw_character{};
    bool saw_padding{};
    for (std::size_t index = 0U; index < length; ++index) {
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
    for (std::size_t index = 0U; index < length; ++index) {
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

[[nodiscard]] std::size_t exact_length(
    const std::uint8_t type,
    const bool allow_epoch_time) noexcept {
    switch (type) {
        case 0x03U: return 9U;
        case 0x04U: return 10U;
        case 0x20U: return 6U;
        case 0x21U: return 34U;
        case 0x22U: return 26U;
        case 0x23U: return 26U;
        case 0x24U: return 38U;
        case 0x25U: return 18U;
        case 0x26U: return 16U;
        case 0x27U: return 27U;
        case 0x28U: return 19U;
        case 0x29U: return 14U;
        case 0x2AU: return 41U;
        case 0x2BU: return 33U;
        case 0x2CU: return 14U;
        case 0x2DU: return 6U;
        case 0x2FU: return 41U;
        case 0x30U: return 43U;
        case 0x31U: return 18U;
        case 0x80U: return 6U;
        case 0x81U: return 6U;
        case 0x82U: return 11U;
        case 0x83U: return 6U;
        case 0x95U: return 47U;
        case 0x96U: return 27U;
        case 0x97U: return 6U;
        case 0x98U: return 15U;
        case 0xB1U: return allow_epoch_time ? 18U : 0U;
        default: return 0U;
    }
}

[[nodiscard]] bool valid_message_length(
    const std::uint8_t type,
    const std::size_t length,
    const bool allow_epoch_time) noexcept {
    if (type == 0x20U) {
        return length == 6U || (allow_epoch_time && length == 10U);
    }
    const auto minimum = exact_length(type, allow_epoch_time);
    if (minimum == 0U) {
        return false;
    }
    // GRP/Spin control messages and Time Reference are exact framing
    // contracts. Sequenced PITCH messages may grow by appended fields, as the
    // Cboe specification explicitly requires receivers to tolerate.
    if (type == 0x03U || type == 0x04U ||
        (type >= 0x80U && type <= 0x83U) || type == 0xB1U) {
        return length == minimum;
    }
    return length >= minimum;
}

[[nodiscard]] std::size_t l3_slots(const std::uint8_t type) noexcept {
    switch (type) {
        case 0x21U:
        case 0x22U:
        case 0x23U:
        case 0x24U:
        case 0x25U:
        case 0x26U:
        case 0x29U:
        case 0x2FU: return 1U;
        case 0x27U:
        case 0x28U: return 2U;
        default: return 0U;
    }
}

[[nodiscard]] std::size_t control_slots(const std::uint8_t type) noexcept {
    switch (type) {
        case 0x21U:
        case 0x22U:
        case 0x25U:
        case 0x26U:
        case 0x27U:
        case 0x28U:
        case 0x29U:
        case 0x2FU: return 0U;
        default: return 1U;
    }
}

[[nodiscard]] std::uint64_t normalized_sequence(
    const std::uint64_t epoch,
    const std::uint32_t sequence,
    bool& valid) noexcept {
    if (epoch == 0U || sequence == 0U ||
        epoch - 1U >
            (std::numeric_limits<std::uint64_t>::max() - sequence) /
                kSequencesPerEpoch) {
        valid = false;
        return 0U;
    }
    valid = true;
    return (epoch - 1U) * kSequencesPerEpoch + sequence;
}

[[nodiscard]] std::uint64_t infer_packet_epoch(
    const std::uint64_t expected_epoch,
    const std::uint32_t expected,
    const std::uint32_t first) noexcept {
    if (expected == 0U || first == 0U || expected == first) {
        return expected_epoch;
    }
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (first < expected) {
        const auto forward = static_cast<std::uint64_t>(maximum - expected) +
                             first;
        const auto backward = static_cast<std::uint64_t>(expected - first);
        if (forward < backward) {
            return expected_epoch == std::numeric_limits<std::uint64_t>::max()
                       ? 0U
                       : expected_epoch + 1U;
        }
    } else {
        const auto forward = static_cast<std::uint64_t>(first - expected);
        const auto backward = static_cast<std::uint64_t>(maximum - first) +
                              expected;
        if (backward < forward) {
            return expected_epoch <= 1U ? 0U : expected_epoch - 1U;
        }
    }
    return expected_epoch;
}

[[nodiscard]] SequenceState classify_sequence(
    const std::uint32_t first,
    const std::uint32_t next,
    const std::uint32_t expected,
    const std::uint32_t rollovers) noexcept {
    if (first == 0U) {
        return SequenceState::Unsequenced;
    }
    if (expected == 0U) {
        return rollovers == 0U ? SequenceState::NotChecked
                               : SequenceState::Rollover;
    }
    if (first == expected) {
        return rollovers == 0U ? SequenceState::InOrder
                               : SequenceState::Rollover;
    }
    const auto forward = first >= expected
                             ? static_cast<std::uint64_t>(first - expected)
                             : static_cast<std::uint64_t>(
                                   std::numeric_limits<std::uint32_t>::max() -
                                   expected) +
                                   first;
    if (forward < (kSequencesPerEpoch / 2U)) {
        return SequenceState::Gap;
    }
    const auto backward = expected >= first
                              ? static_cast<std::uint64_t>(expected - first)
                              : static_cast<std::uint64_t>(
                                    std::numeric_limits<std::uint32_t>::max() -
                                    first) +
                                    expected;
    const auto span = first <= next
                          ? static_cast<std::uint64_t>(next - first)
                          : static_cast<std::uint64_t>(
                                std::numeric_limits<std::uint32_t>::max() -
                                first) +
                                next;
    return backward >= span ? SequenceState::Duplicate
                            : SequenceState::Overlap;
}

[[nodiscard]] DecodeResult failed(
    const DecodeStatus status,
    const DecodeContext& context,
    const SequenceState state = SequenceState::NotChecked) noexcept {
    DecodeResult result{};
    result.packet.status = status;
    result.packet.sequence_state = state;
    result.state.last_time_seconds = context.last_time_seconds;
    result.state.has_time_base = context.has_time_base;
    result.state.next_sequence_epoch = context.sequence_epoch;
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

[[nodiscard]] bool timestamp_for(
    const std::span<const std::byte> message,
    const bool has_time,
    const std::uint32_t time_seconds,
    std::int64_t& timestamp) noexcept {
    if (!has_time || message.size() < 6U) {
        return false;
    }
    const auto offset = read_le32(message.data() + 2U);
    if (offset >= 1'000'000'000U || time_seconds > 86'399U) {
        return false;
    }
    timestamp = static_cast<std::int64_t>(time_seconds) * 1'000'000'000LL +
                offset;
    return true;
}

}  // namespace

struct Decoder::Impl final {
    std::uint8_t unit{};
    std::vector<SymbolBinding> symbols;
    UnmappedInstrumentPolicy unmapped_policy{UnmappedInstrumentPolicy::Reject};
    bool allow_epoch_time{};
    bool valid{};

    explicit Impl(const DecoderConfig& config)
        : unit(config.expected_unit),
          symbols(config.symbols.begin(), config.symbols.end()),
          unmapped_policy(config.unmapped_policy),
          allow_epoch_time(config.allow_epoch_time_extension) {
        valid = unit != 0U && !symbols.empty() &&
                unmapped_policy == UnmappedInstrumentPolicy::Reject;
        std::sort(
            symbols.begin(), symbols.end(),
            [](const SymbolBinding& left, const SymbolBinding& right) noexcept {
                return left.symbol < right.symbol;
            });
        for (std::size_t index = 0U; index < symbols.size(); ++index) {
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
    }

    [[nodiscard]] InstrumentId resolve(
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
};

DecodeStatus encode_gap_request_message(
    const GapRequest& request,
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < kGapRequestMessageBytes) {
        return DecodeStatus::Truncated;
    }
    if (request.unit == 0U || request.first_sequence == 0U ||
        request.message_count == 0U) {
        return DecodeStatus::InvalidField;
    }
    destination[0] = static_cast<std::byte>(kGapRequestMessageBytes);
    destination[1] = std::byte{0x03U};
    destination[2] = static_cast<std::byte>(request.unit);
    write_le32(destination.data() + 3U, request.first_sequence);
    write_le16(destination.data() + 7U, request.message_count);
    return DecodeStatus::Ok;
}

DecodeStatus decode_gap_response_message(
    const std::span<const std::byte> source,
    GapResponse& response) noexcept {
    if (source.size() < kGapResponseMessageBytes) {
        return DecodeStatus::Truncated;
    }
    if (source.size() != kGapResponseMessageBytes ||
        std::to_integer<std::uint8_t>(source[0]) !=
            kGapResponseMessageBytes) {
        return DecodeStatus::InvalidLength;
    }
    if (std::to_integer<std::uint8_t>(source[1]) != 0x04U) {
        return DecodeStatus::InvalidType;
    }
    const auto status = static_cast<char>(
        std::to_integer<std::uint8_t>(source[9]));
    const auto unit = std::to_integer<std::uint8_t>(source[2]);
    const auto sequence = read_le32(source.data() + 3U);
    const auto count = read_le16(source.data() + 7U);
    if (unit == 0U || sequence == 0U || count == 0U ||
        !printable(static_cast<std::uint8_t>(status))) {
        return DecodeStatus::InvalidField;
    }
    response = GapResponse{unit, sequence, count, status};
    return DecodeStatus::Ok;
}

DecodeStatus encode_spin_request_message(
    const SpinRequest& request,
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < kSpinRequestMessageBytes) {
        return DecodeStatus::Truncated;
    }
    if (request.sequence == 0U) {
        return DecodeStatus::InvalidField;
    }
    destination[0] = static_cast<std::byte>(kSpinRequestMessageBytes);
    destination[1] = std::byte{0x81U};
    write_le32(destination.data() + 2U, request.sequence);
    return DecodeStatus::Ok;
}

std::uint32_t advance_sequence(
    const std::uint32_t first,
    const std::uint32_t count,
    std::uint32_t& rollovers) noexcept {
    rollovers = 0U;
    if (first == 0U) {
        return 0U;
    }
    std::uint64_t zero_based = static_cast<std::uint64_t>(first - 1U) + count;
    rollovers = static_cast<std::uint32_t>(
        zero_based / kSequencesPerEpoch);
    zero_based %= kSequencesPerEpoch;
    return static_cast<std::uint32_t>(zero_based + 1U);
}

Decoder::Decoder(const DecoderConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

bool Decoder::configured() const noexcept {
    return impl_ != nullptr && impl_->valid;
}

DecodeResult Decoder::decode_packet(
    const std::span<const std::byte> packet,
    const DecodeContext& context,
    const native_md::OrderLookup& order_lookup,
    DecodeBuffers destination) noexcept {
    if (!configured() || context.leg == marketdata::FeedLeg::None ||
        context.sequence_epoch == 0U) {
        return failed(DecodeStatus::InvalidConfiguration, context);
    }
    if (packet.size() < kSequencedUnitHeaderBytes) {
        return failed(DecodeStatus::Truncated, context);
    }
    const auto header_length = read_le16(packet.data());
    const auto message_count = std::to_integer<std::uint8_t>(packet[2]);
    const auto unit = std::to_integer<std::uint8_t>(packet[3]);
    const auto first_native = read_le32(packet.data() + 4U);
    const bool control_or_spin =
        context.mode == DecodeMode::ControlSession ||
        context.mode == DecodeMode::SpinServer;
    const bool valid_unit = unit == impl_->unit ||
                            (unit == 0U && (control_or_spin || message_count == 0U));
    if (header_length != packet.size() || !valid_unit) {
        return failed(DecodeStatus::InvalidEnvelope, context);
    }
    const bool unsequenced = first_native == 0U;
    const bool multicast_mode =
        context.mode == DecodeMode::RealtimeMulticast ||
        context.mode == DecodeMode::GapMulticast;
    if (message_count != 0U &&
        ((multicast_mode && unsequenced) ||
         (!multicast_mode && !unsequenced))) {
        return failed(DecodeStatus::InvalidEnvelope, context);
    }
    const auto packet_epoch = unsequenced
                                  ? context.sequence_epoch
                                  : infer_packet_epoch(
                                        context.sequence_epoch,
                                        context.expected_sequence,
                                        first_native);
    if (packet_epoch == 0U) {
        return failed(DecodeStatus::SequenceOverflow, context);
    }
    std::uint32_t packet_rollovers{};
    const auto next_native = unsequenced
                                 ? 0U
                                 : advance_sequence(
                                       first_native,
                                       message_count,
                                       packet_rollovers);
    const auto sequence_state = classify_sequence(
        first_native,
        next_native,
        context.expected_sequence,
        packet_rollovers);

    if (message_count == 0U) {
        if (packet.size() != kSequencedUnitHeaderBytes) {
            return failed(
                DecodeStatus::InvalidEnvelope,
                context,
                sequence_state);
        }
        bool normalized_valid{};
        const auto normalized = unsequenced
                                    ? 0U
                                    : normalized_sequence(
                                          packet_epoch,
                                          first_native,
                                          normalized_valid);
        if (!unsequenced && !normalized_valid) {
            return failed(
                DecodeStatus::SequenceOverflow, context, sequence_state);
        }
        if (context.decode_payload) {
            if (destination.controls.empty() || destination.actions.empty()) {
                return failed(
                    DecodeStatus::OutputCapacityExceeded,
                    context,
                    sequence_state);
            }
            destination.controls[0] = ControlEvent{
                ControlKind::Heartbeat,
                InstrumentId{},
                context.leg,
                normalized,
            };
            destination.actions[0] = native_md::DecodedAction{
                native_md::DecodedActionKind::Control,
                normalized,
                0U,
            };
        }
        DecodeResult result{};
        result.packet.status = DecodeStatus::Heartbeat;
        result.packet.sequence_state = sequence_state;
        result.packet.control_count = context.decode_payload ? 1U : 0U;
        result.packet.action_count = context.decode_payload ? 1U : 0U;
        result.packet.first_sequence = normalized;
        result.packet.next_sequence = normalized;
        result.packet.wire_unit = unit;
        result.packet.payload_decoded = context.decode_payload;
        result.state.last_time_seconds = context.last_time_seconds;
        result.state.has_time_base = context.has_time_base;
        result.state.next_native_sequence = first_native;
        result.state.next_sequence_epoch = packet_epoch;
        return result;
    }

    std::size_t cursor = kSequencedUnitHeaderBytes;
    std::size_t required_l3{};
    std::size_t required_controls{};
    bool has_extension_bytes{};
    for (std::uint8_t index = 0U; index < message_count; ++index) {
        if (cursor >= packet.size()) {
            return failed(DecodeStatus::Truncated, context, sequence_state);
        }
        const auto length = std::to_integer<std::uint8_t>(packet[cursor]);
        if (length < 2U || packet.size() - cursor < length) {
            return failed(DecodeStatus::Truncated, context, sequence_state);
        }
        const auto type = std::to_integer<std::uint8_t>(packet[cursor + 1U]);
        if (exact_length(type, impl_->allow_epoch_time) == 0U &&
            !(type == 0x20U && impl_->allow_epoch_time)) {
            return failed(
                DecodeStatus::UnsupportedMessage, context, sequence_state);
        }
        if (!valid_message_length(type, length, impl_->allow_epoch_time)) {
            return failed(DecodeStatus::InvalidLength, context, sequence_state);
        }
        const bool gap_control = type == 0x03U || type == 0x04U;
        const bool spin_control = type >= 0x80U && type <= 0x83U;
        if ((context.mode == DecodeMode::ControlSession && !gap_control) ||
            (context.mode != DecodeMode::ControlSession && gap_control) ||
            (context.mode == DecodeMode::SpinServer && gap_control) ||
            (context.mode != DecodeMode::SpinServer && spin_control)) {
            return failed(DecodeStatus::InvalidEnvelope, context, sequence_state);
        }
        const auto minimum = exact_length(type, impl_->allow_epoch_time);
        if (type != 0x20U && length > minimum) {
            has_extension_bytes = true;
        }
        required_l3 += l3_slots(type);
        required_controls += control_slots(type);
        cursor += length;
    }
    if (cursor != packet.size()) {
        return failed(DecodeStatus::InvalidEnvelope, context, sequence_state);
    }
    bool first_normalized_valid{unsequenced};
    const auto first_normalized = unsequenced
                                      ? 0U
                                      : normalized_sequence(
                                            packet_epoch,
                                            first_native,
                                            first_normalized_valid);
    if (!first_normalized_valid ||
        (!unsequenced &&
         first_normalized >
             std::numeric_limits<std::uint64_t>::max() - message_count)) {
        return failed(DecodeStatus::SequenceOverflow, context, sequence_state);
    }
    if (!context.decode_payload) {
        DecodeResult result{};
        result.packet.status = DecodeStatus::Ok;
        result.packet.sequence_state = sequence_state;
        result.packet.first_sequence = first_normalized;
        result.packet.next_sequence = unsequenced
                                          ? 0U
                                          : first_normalized + message_count;
        result.packet.wire_message_count = message_count;
        result.packet.sequence_count = message_count;
        result.packet.wire_unit = unit;
        result.packet.contains_book_update = required_l3 != 0U;
        result.packet.has_extension_bytes = has_extension_bytes;
        result.state.last_time_seconds = context.last_time_seconds;
        result.state.has_time_base = context.has_time_base;
        result.state.next_native_sequence = next_native;
        result.state.next_sequence_epoch =
            packet_epoch + packet_rollovers;
        return result;
    }
    if (required_l3 > destination.l3_events.size() ||
        required_controls > destination.controls.size() ||
        required_l3 + required_controls > destination.actions.size() ||
        required_l3 > std::numeric_limits<std::uint32_t>::max() ||
        required_controls > std::numeric_limits<std::uint32_t>::max()) {
        return failed(
            DecodeStatus::OutputCapacityExceeded, context, sequence_state);
    }

    cursor = kSequencedUnitHeaderBytes;
    std::size_t l3_count{};
    std::size_t control_count{};
    auto time_seconds = context.last_time_seconds;
    auto has_time = context.has_time_base;
    auto native_sequence = first_native;
    auto sequence_epoch = packet_epoch;
    for (std::uint8_t index = 0U; index < message_count; ++index) {
        const auto length = std::to_integer<std::uint8_t>(packet[cursor]);
        const auto message = packet.subspan(cursor, length);
        cursor += length;
        const auto* const bytes = message.data();
        const auto type = std::to_integer<std::uint8_t>(bytes[1U]);
        bool sequence_valid{unsequenced};
        const auto sequence = unsequenced
                                  ? 0U
                                  : normalized_sequence(
                                        sequence_epoch,
                                        native_sequence,
                                        sequence_valid);
        if (!sequence_valid) {
            return failed(
                DecodeStatus::SequenceOverflow, context, sequence_state);
        }

        if (type == 0x20U) {
            const auto seconds = read_le32(bytes + 2U);
            if (seconds > 86'399U ||
                (message.size() == 10U && !impl_->allow_epoch_time)) {
                return failed(
                    DecodeStatus::InvalidField, context, sequence_state);
            }
            time_seconds = seconds;
            has_time = true;
            ControlEvent control{};
            control.kind = ControlKind::Time;
            control.leg = context.leg;
            control.source_sequence = sequence;
            control.exchange_timestamp_ns =
                static_cast<std::int64_t>(seconds) * 1'000'000'000LL;
            control.protocol_sequence =
                message.size() == 10U ? read_le32(bytes + 6U) : 0U;
            control.unit = unit;
            control.wire_type = type;
            destination.controls[control_count++] = control;
        } else if (type == 0xB1U) {
            const auto seconds = read_le32(bytes + 6U);
            const auto offset = read_le32(bytes + 10U);
            const auto date = read_le32(bytes + 14U);
            if (!impl_->allow_epoch_time || seconds > 86'399U ||
                offset >= 1'000'000'000U || date < 20'000'101U ||
                date > 99'991'231U) {
                return failed(
                    DecodeStatus::InvalidField, context, sequence_state);
            }
            time_seconds = seconds;
            has_time = true;
            ControlEvent control{};
            control.kind = ControlKind::TimeReference;
            control.leg = context.leg;
            control.source_sequence = sequence;
            control.exchange_timestamp_ns =
                static_cast<std::int64_t>(seconds) * 1'000'000'000LL +
                offset;
            control.protocol_sequence = read_le32(bytes + 2U);
            control.count = date;
            control.unit = unit;
            control.wire_type = type;
            destination.controls[control_count++] = control;
        } else if (type == 0x03U || type == 0x04U) {
            if (!unsequenced) {
                return failed(
                    DecodeStatus::InvalidEnvelope, context, sequence_state);
            }
            const auto request_unit = std::to_integer<std::uint8_t>(bytes[2U]);
            const auto request_sequence = read_le32(bytes + 3U);
            const auto request_count = read_le16(bytes + 7U);
            if (request_unit == 0U || request_sequence == 0U ||
                request_count == 0U) {
                return failed(
                    DecodeStatus::InvalidField, context, sequence_state);
            }
            ControlEvent control{};
            control.kind = type == 0x04U ? ControlKind::GapResponse
                                        : ControlKind::Administrative;
            control.leg = context.leg;
            control.protocol_sequence = request_sequence;
            control.count = request_count;
            control.unit = request_unit;
            control.wire_type = type;
            if (type == 0x04U) {
                control.code[0] = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[9U]));
                if (!printable(
                        static_cast<std::uint8_t>(control.code[0]))) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
            }
            destination.controls[control_count++] = control;
        } else if (type >= 0x80U && type <= 0x83U) {
            if (!unsequenced) {
                return failed(
                    DecodeStatus::InvalidEnvelope, context, sequence_state);
            }
            ControlEvent control{};
            control.leg = context.leg;
            control.protocol_sequence = read_le32(bytes + 2U);
            control.unit = unit;
            control.wire_type = type;
            if (type == 0x80U) {
                control.kind = ControlKind::SpinImageAvailable;
            } else if (type == 0x82U) {
                control.kind = ControlKind::SpinResponse;
                control.count = read_le32(bytes + 6U);
                control.code[0] = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[10U]));
                if (!printable(
                        static_cast<std::uint8_t>(control.code[0]))) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
            } else if (type == 0x83U) {
                control.kind = ControlKind::SpinFinished;
            } else {
                control.kind = ControlKind::Administrative;
            }
            if (control.protocol_sequence == 0U) {
                return failed(
                    DecodeStatus::InvalidField, context, sequence_state);
            }
            destination.controls[control_count++] = control;
        } else {
            std::int64_t timestamp{};
            if (!timestamp_for(message, has_time, time_seconds, timestamp)) {
                return failed(
                    has_time ? DecodeStatus::InvalidField
                             : DecodeStatus::MissingTimeBase,
                    context,
                    sequence_state);
            }

            if (type == 0x95U || type == 0x96U || type == 0x98U) {
                if (!valid_symbol_bytes(bytes + 6U, 8U)) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
                const auto symbol = read_symbol(bytes + 6U, 8U);
                const auto instrument = impl_->resolve(symbol);
                if (!instrument) {
                    return failed(
                        DecodeStatus::UnknownInstrument,
                        context,
                        sequence_state);
                }
                ControlEvent control{};
                control.kind = ControlKind::Administrative;
                control.instrument = instrument;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.unit = unit;
                control.wire_type = type;
                control.code[0] = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[14U]));
                control.symbol = symbol;
                destination.controls[control_count++] = control;
            } else if (type == 0x97U || type == 0x2DU) {
                ControlEvent control{};
                control.kind = type == 0x97U ? ControlKind::UnitClear
                                            : ControlKind::EndOfSession;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.unit = unit;
                control.wire_type = type;
                destination.controls[control_count++] = control;
            } else if (type == 0x31U) {
                if (!valid_symbol_bytes(bytes + 6U, 8U)) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
                const auto symbol = read_symbol(bytes + 6U, 8U);
                const auto instrument = impl_->resolve(symbol);
                const auto status = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[14U]));
                const auto reg_sho = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[15U]));
                if (!valid_code(status, "AHQST") ||
                    !valid_code(reg_sho, "01") ||
                    (!instrument && impl_->unmapped_policy ==
                                        UnmappedInstrumentPolicy::Reject)) {
                    return failed(
                        !instrument ? DecodeStatus::UnknownInstrument
                                    : DecodeStatus::InvalidField,
                        context,
                        sequence_state);
                }
                ControlEvent control{};
                control.kind = ControlKind::TradingStatus;
                control.instrument = instrument;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.unit = unit;
                control.wire_type = type;
                control.code[0] = status;
                control.code[1] = reg_sho;
                control.symbol = symbol;
                destination.controls[control_count++] = control;
            } else if (type == 0x21U || type == 0x22U || type == 0x2FU) {
                const auto long_form = type != 0x22U;
                const auto expanded = type == 0x2FU;
                const auto reference = read_le64(bytes + 6U);
                const auto side_value = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[14U]));
                const auto quantity = long_form ? read_le32(bytes + 15U)
                                                : read_le16(bytes + 15U);
                const auto symbol_offset = long_form ? 19U : 17U;
                const auto symbol_length = expanded ? 8U : 6U;
                const auto price_offset = expanded ? 27U : (long_form ? 25U : 23U);
                const auto price = long_form
                                       ? read_le64(bytes + price_offset)
                                       : static_cast<std::uint64_t>(
                                             read_le16(bytes + price_offset)) *
                                             100U;
                const auto flags_offset = expanded ? 35U : (long_form ? 33U : 25U);
                const auto flags = std::to_integer<std::uint8_t>(
                    bytes[flags_offset]);
                if (reference == 0U || !valid_code(side_value, "BS") ||
                    quantity == 0U || price == 0U ||
                    price > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) ||
                    flags != 0x01U ||
                    !valid_symbol_bytes(bytes + symbol_offset, symbol_length)) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
                const auto symbol = read_symbol(
                    bytes + symbol_offset, symbol_length);
                const auto instrument = impl_->resolve(symbol);
                if (!instrument && impl_->unmapped_policy ==
                                       UnmappedInstrumentPolicy::Reject) {
                    return failed(
                        DecodeStatus::UnknownInstrument,
                        context,
                        sequence_state);
                }
                if (instrument) {
                    emit_book_event(
                        destination,
                        l3_count,
                        instrument,
                        context.leg,
                        sequence,
                        marketdata::L3Operation::Add,
                        reference,
                        side_value == 'B' ? Side::Buy : Side::Sell,
                        Price{static_cast<std::int64_t>(price)},
                        Quantity{quantity},
                        timestamp);
                }
            } else if (type == 0x23U || type == 0x25U || type == 0x26U) {
                const auto reference = read_le64(bytes + 6U);
                const auto reduction = type == 0x26U
                                           ? read_le16(bytes + 14U)
                                           : read_le32(bytes + 14U);
                if (reference == 0U || reduction == 0U) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
                DecodeStatus reduce_status{DecodeStatus::Ok};
                if (!reduce_order(
                        destination,
                        l3_count,
                        order_lookup,
                        context.leg,
                        sequence,
                        reference,
                        reduction,
                        timestamp,
                        reduce_status)) {
                    return failed(reduce_status, context, sequence_state);
                }
                if (type == 0x23U) {
                    const auto& routed = destination.l3_events[l3_count - 1U];
                    ControlEvent control{};
                    control.kind = ControlKind::Execution;
                    control.instrument = routed.instrument;
                    control.leg = context.leg;
                    control.source_sequence = sequence;
                    control.exchange_timestamp_ns = timestamp;
                    control.order_reference = reference;
                    control.match_or_execution_id = read_le64(bytes + 18U);
                    control.quantity = reduction;
                    control.unit = unit;
                    control.wire_type = type;
                    control.printable = true;
                    if (control.match_or_execution_id == 0U) {
                        return failed(
                            DecodeStatus::InvalidField,
                            context,
                            sequence_state);
                    }
                    destination.controls[control_count++] = control;
                }
            } else if (type == 0x24U) {
                const auto reference = read_le64(bytes + 6U);
                const auto executed = read_le32(bytes + 14U);
                const auto remaining = read_le32(bytes + 18U);
                const auto execution_id = read_le64(bytes + 22U);
                const auto execution_price = read_le64(bytes + 30U);
                NativeOrderState state{};
                if (reference == 0U || executed == 0U || execution_id == 0U ||
                    execution_price == 0U ||
                    execution_price > static_cast<std::uint64_t>(
                                          std::numeric_limits<std::int64_t>::max()) ||
                    !staged_order(
                        destination.l3_events,
                        l3_count,
                        order_lookup,
                        reference,
                        state)) {
                    return failed(
                        reference == 0U || executed == 0U ||
                                execution_id == 0U || execution_price == 0U ||
                                execution_price > static_cast<std::uint64_t>(
                                                      std::numeric_limits<
                                                          std::int64_t>::max())
                            ? DecodeStatus::InvalidField
                            : DecodeStatus::OrderStateUnavailable,
                        context,
                        sequence_state);
                }
                emit_book_event(
                    destination,
                    l3_count,
                    state.instrument,
                    context.leg,
                    sequence,
                    remaining == 0U ? marketdata::L3Operation::Delete
                                    : marketdata::L3Operation::Modify,
                    reference,
                    state.side,
                    state.price,
                    Quantity{remaining},
                    timestamp);
                ControlEvent control{};
                control.kind = ControlKind::Execution;
                control.instrument = state.instrument;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.order_reference = reference;
                control.match_or_execution_id = execution_id;
                control.quantity = executed;
                control.price_units = static_cast<std::int64_t>(execution_price);
                control.unit = unit;
                control.wire_type = type;
                control.printable = true;
                destination.controls[control_count++] = control;
            } else if (type == 0x27U || type == 0x28U) {
                const auto long_form = type == 0x27U;
                const auto reference = read_le64(bytes + 6U);
                const auto quantity = long_form ? read_le32(bytes + 14U)
                                                : read_le16(bytes + 14U);
                const auto price = long_form
                                       ? read_le64(bytes + 18U)
                                       : static_cast<std::uint64_t>(
                                             read_le16(bytes + 16U)) *
                                             100U;
                const auto flags = std::to_integer<std::uint8_t>(
                    bytes[long_form ? 26U : 18U]);
                NativeOrderState state{};
                if (reference == 0U || price == 0U ||
                    price > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) ||
                    (flags & 0xFCU) != 0U ||
                    !staged_order(
                        destination.l3_events,
                        l3_count,
                        order_lookup,
                        reference,
                        state)) {
                    return failed(
                        reference == 0U || price == 0U ||
                                price > static_cast<std::uint64_t>(
                                            std::numeric_limits<
                                                std::int64_t>::max()) ||
                                (flags & 0xFCU) != 0U
                            ? DecodeStatus::InvalidField
                            : DecodeStatus::OrderStateUnavailable,
                        context,
                        sequence_state);
                }
                const auto new_price = Price{static_cast<std::int64_t>(price)};
                if (quantity == 0U) {
                    emit_book_event(
                        destination,
                        l3_count,
                        state.instrument,
                        context.leg,
                        sequence,
                        marketdata::L3Operation::Delete,
                        reference,
                        state.side,
                        state.price,
                        Quantity{0U},
                        timestamp);
                } else if ((flags & 0x02U) == 0U) {
                    // Reset-priority modifies, including apparent no-ops, must
                    // move the order to the tail of its price-level FIFO.
                    emit_book_event(
                        destination,
                        l3_count,
                        state.instrument,
                        context.leg,
                        sequence,
                        marketdata::L3Operation::Delete,
                        reference,
                        state.side,
                        state.price,
                        Quantity{0U},
                        timestamp);
                    emit_book_event(
                        destination,
                        l3_count,
                        state.instrument,
                        context.leg,
                        sequence,
                        marketdata::L3Operation::Add,
                        reference,
                        state.side,
                        new_price,
                        Quantity{quantity},
                        timestamp);
                } else {
                    emit_book_event(
                        destination,
                        l3_count,
                        state.instrument,
                        context.leg,
                        sequence,
                        marketdata::L3Operation::Modify,
                        reference,
                        state.side,
                        new_price,
                        Quantity{quantity},
                        timestamp);
                }
            } else if (type == 0x29U) {
                const auto reference = read_le64(bytes + 6U);
                NativeOrderState state{};
                if (reference == 0U ||
                    !staged_order(
                        destination.l3_events,
                        l3_count,
                        order_lookup,
                        reference,
                        state)) {
                    return failed(
                        reference == 0U ? DecodeStatus::InvalidField
                                        : DecodeStatus::OrderStateUnavailable,
                        context,
                        sequence_state);
                }
                emit_book_event(
                    destination,
                    l3_count,
                    state.instrument,
                    context.leg,
                    sequence,
                    marketdata::L3Operation::Delete,
                    reference,
                    state.side,
                    state.price,
                    Quantity{0U},
                    timestamp);
            } else if (type == 0x2AU || type == 0x2BU || type == 0x30U) {
                const auto long_form = type != 0x2BU;
                const auto expanded = type == 0x30U;
                const auto quantity = long_form ? read_le32(bytes + 15U)
                                                : read_le16(bytes + 15U);
                const auto symbol_offset = long_form ? 19U : 17U;
                const auto symbol_length = expanded ? 8U : 6U;
                const auto price_offset = expanded ? 27U : (long_form ? 25U : 23U);
                const auto execution_offset = expanded ? 35U
                                                       : (long_form ? 33U : 25U);
                const auto price = long_form
                                       ? read_le64(bytes + price_offset)
                                       : static_cast<std::uint64_t>(
                                             read_le16(bytes + price_offset)) *
                                             100U;
                const auto side_value = static_cast<char>(
                    std::to_integer<std::uint8_t>(bytes[14U]));
                const auto execution_id = read_le64(bytes + execution_offset);
                if (side_value != 'B' || quantity == 0U || price == 0U ||
                    execution_id == 0U ||
                    price > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) ||
                    !valid_symbol_bytes(bytes + symbol_offset, symbol_length)) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
                const auto symbol = read_symbol(
                    bytes + symbol_offset, symbol_length);
                const auto instrument = impl_->resolve(symbol);
                if (!instrument && impl_->unmapped_policy ==
                                       UnmappedInstrumentPolicy::Reject) {
                    return failed(
                        DecodeStatus::UnknownInstrument,
                        context,
                        sequence_state);
                }
                ControlEvent control{};
                control.kind = ControlKind::Trade;
                control.instrument = instrument;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.order_reference = read_le64(bytes + 6U);
                control.match_or_execution_id = execution_id;
                control.quantity = quantity;
                control.price_units = static_cast<std::int64_t>(price);
                control.unit = unit;
                control.wire_type = type;
                control.symbol = symbol;
                control.printable = true;
                destination.controls[control_count++] = control;
            } else if (type == 0x2CU) {
                const auto execution_id = read_le64(bytes + 6U);
                if (execution_id == 0U) {
                    return failed(
                        DecodeStatus::InvalidField, context, sequence_state);
                }
                ControlEvent control{};
                control.kind = ControlKind::BrokenTrade;
                control.leg = context.leg;
                control.source_sequence = sequence;
                control.exchange_timestamp_ns = timestamp;
                control.match_or_execution_id = execution_id;
                control.unit = unit;
                control.wire_type = type;
                destination.controls[control_count++] = control;
            } else {
                return failed(
                    DecodeStatus::UnsupportedMessage, context, sequence_state);
            }
        }

        if (!unsequenced) {
            if (native_sequence ==
                std::numeric_limits<std::uint32_t>::max()) {
                native_sequence = 1U;
                ++sequence_epoch;
            } else {
                ++native_sequence;
            }
        }
    }

    build_action_stream(destination, l3_count, control_count);
    DecodeResult result{};
    result.packet.status = DecodeStatus::Ok;
    result.packet.sequence_state = sequence_state;
    result.packet.l3_event_count = l3_count;
    result.packet.control_count = control_count;
    result.packet.action_count = l3_count + control_count;
    result.packet.first_sequence = first_normalized;
    result.packet.next_sequence = unsequenced
                                      ? 0U
                                      : first_normalized + message_count;
    result.packet.wire_message_count = message_count;
    result.packet.sequence_count = message_count;
    result.packet.wire_unit = unit;
    result.packet.contains_book_update = l3_count != 0U;
    result.packet.payload_decoded = true;
    result.packet.has_extension_bytes = has_extension_bytes;
    result.state.last_time_seconds = time_seconds;
    result.state.has_time_base = has_time;
    result.state.next_native_sequence = native_sequence;
    result.state.next_sequence_epoch = sequence_epoch;
    return result;
}

}  // namespace sorx::connectors::cboe::pitch
