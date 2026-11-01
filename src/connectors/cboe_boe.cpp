#include "sorx/connectors/cboe_boe.hpp"

#include <algorithm>
#include <limits>

namespace sorx::connectors::cboe {
namespace {

[[nodiscard]] constexpr std::byte as_byte(const char value) noexcept {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
}

[[nodiscard]] constexpr char as_char(const std::byte value) noexcept {
    return static_cast<char>(std::to_integer<unsigned char>(value));
}

void write_le(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint64_t value,
    const std::size_t width) noexcept {
    for (std::size_t index{}; index < width; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        destination[offset + index] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint64_t read_le(
    const std::span<const std::byte> source,
    const std::size_t offset,
    const std::size_t width) noexcept {
    std::uint64_t value{};
    for (std::size_t index{}; index < width; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        value |= std::to_integer<std::uint64_t>(source[offset + index]) << shift;
    }
    return value;
}

template <std::size_t Size>
void write_chars(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::array<char, Size>& value) noexcept {
    for (std::size_t index{}; index < Size; ++index) {
        destination[offset + index] = as_byte(value[index]);
    }
}

template <std::size_t Size>
void read_chars(
    const std::span<const std::byte> source,
    const std::size_t offset,
    std::array<char, Size>& value) noexcept {
    for (std::size_t index{}; index < Size; ++index) {
        value[index] = as_char(source[offset + index]);
    }
}

[[nodiscard]] constexpr bool is_alpha(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z');
}

[[nodiscard]] constexpr bool is_alphanumeric(const char value) noexcept {
    return is_alpha(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] constexpr bool is_printable(const char value) noexcept {
    return value >= 33 && value <= 126;
}

[[nodiscard]] constexpr bool is_text_character(const char value) noexcept {
    return value >= 32 && value <= 126;
}

template <std::size_t Size, typename Predicate>
[[nodiscard]] bool valid_nul_padded(
    const std::array<char, Size>& value,
    const Predicate permitted,
    const bool require_value) noexcept {
    bool saw_value{};
    bool saw_padding{};
    for (const char character : value) {
        if (character == '\0') {
            saw_padding = true;
            continue;
        }
        if (saw_padding || !permitted(character)) {
            return false;
        }
        saw_value = true;
    }
    return saw_value || !require_value;
}

[[nodiscard]] bool valid_client_order_id(const Text20& value) noexcept {
    return valid_nul_padded(
        value,
        [](const char character) noexcept {
            return is_printable(character) && character != ',' &&
                character != ';' && character != '|' && character != '@' &&
                character != '"';
        },
        true);
}

[[nodiscard]] bool valid_symbol(const Symbol& value) noexcept {
    return valid_nul_padded(
        value,
        [](const char character) noexcept { return is_alphanumeric(character); },
        true);
}

[[nodiscard]] bool valid_account(const Text16& value) noexcept {
    return valid_nul_padded(
        value,
        [](const char character) noexcept {
            return is_alphanumeric(character) || character == ':';
        },
        true);
}

[[nodiscard]] bool valid_routing_instruction(
    const RoutingInstruction& value) noexcept {
    if (!valid_nul_padded(
            value,
            [](const char character) noexcept {
                return is_printable(character);
            },
            true)) {
        return false;
    }
    return value[0] == 'B' || value[0] == 'P' || value[0] == 'R' ||
        value[0] == 'S' || value[0] == 'X' || value[0] == 'K' ||
        value[0] == 'A' || value[0] == 'N';
}

[[nodiscard]] constexpr bool valid_side(const BoeSide side) noexcept {
    switch (side) {
        case BoeSide::Buy:
        case BoeSide::Sell:
        case BoeSide::SellShort:
        case BoeSide::SellShortExempt:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_time_in_force(const char value) noexcept {
    return value == '0' || value == '1' || value == '2' || value == '3' ||
        value == '4' || value == '5' || value == '6' || value == '7' ||
        value == 'E' || value == 'R' || value == 'T' || value == 'X';
}

[[nodiscard]] constexpr bool valid_capacity(const char value) noexcept {
    return value == 'A' || value == 'P' || value == 'R';
}

[[nodiscard]] constexpr bool valid_base_liquidity(const char value) noexcept {
    return value == 'A' || value == 'C' || value == 'R' || value == 'W' ||
        value == 'X';
}

[[nodiscard]] constexpr bool aggressive_routing_instruction(
    const RoutingInstruction& value) noexcept {
    return value[0] == 'R' || value[0] == 'S' || value[0] == 'X' ||
        value[0] == 'K';
}

[[nodiscard]] constexpr bool passive_routing_instruction(
    const RoutingInstruction& value) noexcept {
    return value[0] == 'P';
}

template <std::size_t Size>
[[nodiscard]] bool valid_text(const std::array<char, Size>& value) noexcept {
    return valid_nul_padded(
        value,
        [](const char character) noexcept { return is_text_character(character); },
        false);
}

void write_header(
    const std::span<std::byte> destination,
    const std::uint16_t total_size,
    const std::uint8_t type,
    const std::uint32_t sequence) noexcept {
    destination[0] = std::byte{0xba};
    destination[1] = std::byte{0xba};
    write_le(destination, 2U, total_size - 2U, 2U);
    destination[4] = static_cast<std::byte>(type);
    destination[5] = std::byte{0};
    write_le(destination, 6U, sequence, 4U);
}

[[nodiscard]] CodecStatus validate_frame(
    const std::span<const std::byte> source) noexcept {
    if (source.size() < 10U) {
        return CodecStatus::Truncated;
    }
    if (source[0] != std::byte{0xba} || source[1] != std::byte{0xba}) {
        return CodecStatus::InvalidField;
    }
    const auto length = static_cast<std::size_t>(read_le(source, 2U, 2U));
    return length >= 8U && source.size() == length + 2U
        ? CodecStatus::Ok
        : CodecStatus::InvalidLength;
}

[[nodiscard]] fastpath::FrameEncodeStatus map_status(
    const CodecStatus status) noexcept {
    switch (status) {
        case CodecStatus::Ok:
            return fastpath::FrameEncodeStatus::Encoded;
        case CodecStatus::BufferTooSmall:
            return fastpath::FrameEncodeStatus::BufferTooSmall;
        case CodecStatus::UnsupportedReturnBitfields:
            return fastpath::FrameEncodeStatus::Unsupported;
        default:
            return fastpath::FrameEncodeStatus::InvalidInput;
    }
}

[[nodiscard]] bool make_route_client_order_id(
    const dataplane::WireIdentityPlanRequest& request,
    Text20& output) noexcept {
    static constexpr std::string_view alphabet{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"};
    output.fill('0');
    std::uint64_t session_tag = request.venue_session_id ^
        (request.venue_session_epoch << 1U) ^ request.venue.value();
    output[0] = alphabet[session_tag % alphabet.size()];
    std::uint64_t child = request.child_order_id.value();
    for (std::size_t position = 14U; position-- > 1U;) {
        output[position] = alphabet[child % alphabet.size()];
        child /= alphabet.size();
    }
    for (std::size_t position = output.size(); position-- > 14U;) {
        output[position] = alphabet[session_tag % alphabet.size()];
        session_tag /= alphabet.size();
    }
    return child == 0U;
}

}  // namespace

bool make_planned_wire_identity(
    const std::uint32_t sequence_number,
    const std::string_view client_order_id,
    dataplane::InlineWireIdentity& destination) noexcept {
    if (sequence_number == 0U || client_order_id.empty() ||
        client_order_id.size() > Text20{}.size()) {
        return false;
    }
    for (const char character : client_order_id) {
        if (!is_printable(character) || character == ',' || character == ';' ||
            character == '|' || character == '@' || character == '"') {
            return false;
        }
    }

    destination = {};
    destination.size = static_cast<std::uint8_t>(kBoePlannedWireIdentityBytes);
    write_le(destination.bytes, 0U, sequence_number, 4U);
    for (std::size_t index{}; index < Text20{}.size(); ++index) {
        destination.bytes[4U + index] = index < client_order_id.size()
            ? as_byte(client_order_id[index])
            : std::byte{0};
    }
    return true;
}

bool parse_planned_wire_identity(
    const dataplane::InlineWireIdentity& source,
    PlannedWireIdentity& destination) noexcept {
    if (source.size != kBoePlannedWireIdentityBytes) {
        return false;
    }
    PlannedWireIdentity parsed;
    parsed.sequence_number = static_cast<std::uint32_t>(
        read_le(source.value(), 0U, 4U));
    read_chars(source.value(), 4U, parsed.client_order_id);
    if (parsed.sequence_number == 0U ||
        !valid_client_order_id(parsed.client_order_id)) {
        return false;
    }
    destination = parsed;
    return true;
}

dataplane::WireIdentityPlanStatus plan_route_wire_identity(
    const dataplane::WireIdentityPlanRequest& request,
    dataplane::InlineWireIdentity& destination) noexcept {
    destination = {};
    if (!request.venue || !request.child_order_id ||
        request.dispatch_attempt_id == 0U ||
        request.venue_session_id == 0U ||
        request.venue_session_epoch == 0U) {
        return dataplane::WireIdentityPlanStatus::InvalidInput;
    }
    if (request.dispatch_attempt_id >
        std::numeric_limits<std::uint32_t>::max()) {
        return dataplane::WireIdentityPlanStatus::SequenceExhausted;
    }
    Text20 client_order_id{};
    if (!make_route_client_order_id(request, client_order_id) ||
        !make_planned_wire_identity(
            static_cast<std::uint32_t>(request.dispatch_attempt_id),
            std::string_view{client_order_id.data(), client_order_id.size()},
            destination)) {
        destination = {};
        return dataplane::WireIdentityPlanStatus::InvalidOutput;
    }
    return dataplane::WireIdentityPlanStatus::Planned;
}

CodecResult encode_new_order(
    const NewOrder& message,
    const std::span<std::byte> destination) noexcept {
    const auto total_size = static_cast<std::uint16_t>(
        message.include_time_in_force ? 77U : 76U);
    if (destination.size() < total_size) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (message.identity.sequence_number == 0U ||
        !valid_client_order_id(message.identity.client_order_id) ||
        !valid_side(message.side) || message.quantity == 0U ||
        message.quantity > kMaximumOrderQuantity || message.price <= 0 ||
        !valid_symbol(message.symbol) || !valid_capacity(message.capacity) ||
        !valid_routing_instruction(message.routing_instruction) ||
        !valid_account(message.account) ||
        (message.include_time_in_force &&
         (!valid_time_in_force(message.time_in_force) ||
          (passive_routing_instruction(message.routing_instruction) &&
           (message.time_in_force == '3' || message.time_in_force == '4'))))) {
        return {CodecStatus::InvalidField, 0U};
    }

    write_header(destination, total_size, 0x38U, message.identity.sequence_number);
    write_chars(destination, 10U, message.identity.client_order_id);
    destination[30] = as_byte(static_cast<char>(message.side));
    write_le(destination, 31U, message.quantity, 4U);
    destination[35] = std::byte{3};
    destination[36] = message.include_time_in_force
        ? std::byte{0x24}
        : std::byte{0x04};
    destination[37] = std::byte{0xc1};
    destination[38] = std::byte{0x01};

    std::size_t offset = 39U;
    write_le(destination, offset, static_cast<std::uint64_t>(message.price), 8U);
    offset += 8U;
    if (message.include_time_in_force) {
        destination[offset] = as_byte(message.time_in_force);
        ++offset;
    }
    write_chars(destination, offset, message.symbol);
    offset += message.symbol.size();
    destination[offset] = as_byte(message.capacity);
    ++offset;
    write_chars(destination, offset, message.routing_instruction);
    offset += message.routing_instruction.size();
    write_chars(destination, offset, message.account);
    offset += message.account.size();
    if (offset != total_size) {
        return {CodecStatus::InvalidLength, 0U};
    }
    return {CodecStatus::Ok, total_size};
}

CodecResult encode_cancel_order(
    const CancelOrder& message,
    const std::span<std::byte> destination) noexcept {
    constexpr std::uint16_t total_size = 31U;
    if (destination.size() < total_size) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (message.sequence_number == 0U ||
        !valid_client_order_id(message.original_client_order_id)) {
        return {CodecStatus::InvalidField, 0U};
    }
    write_header(destination, total_size, 0x39U, message.sequence_number);
    write_chars(destination, 10U, message.original_client_order_id);
    destination[30] = std::byte{0};
    return {CodecStatus::Ok, total_size};
}

CodecResult encode_modify_order(
    const ModifyOrder& message,
    const std::span<std::byte> destination) noexcept {
    constexpr std::uint16_t total_size = 64U;
    if (destination.size() < total_size) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (message.sequence_number == 0U ||
        !valid_client_order_id(message.client_order_id) ||
        !valid_client_order_id(message.original_client_order_id) ||
        message.client_order_id == message.original_client_order_id ||
        message.quantity == 0U || message.quantity > kMaximumOrderQuantity ||
        message.price <= 0) {
        return {CodecStatus::InvalidField, 0U};
    }
    write_header(destination, total_size, 0x3aU, message.sequence_number);
    write_chars(destination, 10U, message.client_order_id);
    write_chars(destination, 30U, message.original_client_order_id);
    destination[50] = std::byte{1};
    destination[51] = std::byte{0x0c};
    write_le(destination, 52U, message.quantity, 4U);
    write_le(destination, 56U, static_cast<std::uint64_t>(message.price), 8U);
    return {CodecStatus::Ok, total_size};
}

CodecResult encode_client_heartbeat(
    const std::span<std::byte> destination) noexcept {
    constexpr std::uint16_t total_size = 10U;
    if (destination.size() < total_size) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    write_header(destination, total_size, 0x03U, 0U);
    return {CodecStatus::Ok, total_size};
}

CodecStatus decode_order_response(
    const std::span<const std::byte> source,
    OrderResponse& destination) noexcept {
    const auto frame_status = validate_frame(source);
    if (frame_status != CodecStatus::Ok) {
        return frame_status;
    }

    OrderResponse parsed;
    const auto type = std::to_integer<std::uint8_t>(source[4]);
    parsed.matching_unit = std::to_integer<std::uint8_t>(source[5]);
    parsed.sequence_number = static_cast<std::uint32_t>(read_le(source, 6U, 4U));

    if (type == 0x09U) {
        if (source.size() != 10U || parsed.matching_unit != 0U ||
            parsed.sequence_number != 0U) {
            return CodecStatus::InvalidField;
        }
        parsed.kind = ResponseKind::ServerHeartbeat;
        destination = parsed;
        return CodecStatus::Ok;
    }

    if (type == 0x25U) {
        if (source.size() < 48U) {
            return CodecStatus::Truncated;
        }
        if (source[47] != std::byte{0}) {
            return CodecStatus::UnsupportedReturnBitfields;
        }
        if (source.size() != 48U) {
            return CodecStatus::InvalidLength;
        }
        parsed.kind = ResponseKind::OrderAcknowledged;
        parsed.transaction_time_ns = read_le(source, 10U, 8U);
        read_chars(source, 18U, parsed.client_order_id);
        parsed.order_id = read_le(source, 38U, 8U);
        if (parsed.matching_unit == 0U || parsed.sequence_number == 0U ||
            parsed.transaction_time_ns == 0U || parsed.order_id == 0U ||
            !valid_client_order_id(parsed.client_order_id)) {
            return CodecStatus::InvalidField;
        }
    } else if (type == 0x26U || type == 0x2bU) {
        if (source.size() < 101U) {
            return CodecStatus::Truncated;
        }
        if (source[100] != std::byte{0}) {
            return CodecStatus::UnsupportedReturnBitfields;
        }
        if (source.size() != 101U) {
            return CodecStatus::InvalidLength;
        }
        parsed.kind = type == 0x26U
            ? ResponseKind::OrderRejected
            : ResponseKind::CancelRejected;
        parsed.transaction_time_ns = read_le(source, 10U, 8U);
        read_chars(source, 18U, parsed.client_order_id);
        parsed.reason = as_char(source[38]);
        read_chars(source, 39U, parsed.text);
        if (parsed.matching_unit != 0U || parsed.sequence_number != 0U ||
            parsed.transaction_time_ns == 0U ||
            !valid_client_order_id(parsed.client_order_id) ||
            !is_printable(parsed.reason) || !valid_text(parsed.text)) {
            return CodecStatus::InvalidField;
        }
    } else if (type == 0x2cU) {
        if (source.size() < 70U) {
            return CodecStatus::Truncated;
        }
        if (source[69] != std::byte{0}) {
            return CodecStatus::UnsupportedReturnBitfields;
        }
        if (source.size() != 70U) {
            return CodecStatus::InvalidLength;
        }
        parsed.kind = ResponseKind::OrderExecuted;
        parsed.transaction_time_ns = read_le(source, 10U, 8U);
        read_chars(source, 18U, parsed.client_order_id);
        parsed.execution_id = read_le(source, 38U, 8U);
        parsed.last_shares = static_cast<std::uint32_t>(read_le(source, 46U, 4U));
        parsed.last_price = static_cast<std::int64_t>(read_le(source, 50U, 8U));
        parsed.leaves_quantity = static_cast<std::uint32_t>(
            read_le(source, 58U, 4U));
        parsed.base_liquidity_indicator = as_char(source[62]);
        parsed.sub_liquidity_indicator = as_char(source[63]);
        std::array<char, 4U> contra_broker{};
        read_chars(source, 64U, contra_broker);
        if (parsed.matching_unit == 0U || parsed.sequence_number == 0U ||
            parsed.transaction_time_ns == 0U || parsed.execution_id == 0U ||
            parsed.last_shares == 0U ||
            parsed.last_shares > kMaximumOrderQuantity ||
            parsed.last_price <= 0 ||
            !valid_client_order_id(parsed.client_order_id) ||
            !valid_base_liquidity(parsed.base_liquidity_indicator) ||
            (parsed.sub_liquidity_indicator != '\0' &&
             !is_printable(parsed.sub_liquidity_indicator)) ||
            !valid_nul_padded(
                contra_broker,
                [](const char character) noexcept {
                    return is_alphanumeric(character);
                },
                true)) {
            return CodecStatus::InvalidField;
        }
    } else if (type == 0x2aU) {
        if (source.size() < 41U) {
            return CodecStatus::Truncated;
        }
        if (source[40] != std::byte{0}) {
            return CodecStatus::UnsupportedReturnBitfields;
        }
        if (source.size() != 41U) {
            return CodecStatus::InvalidLength;
        }
        parsed.kind = ResponseKind::OrderCanceled;
        parsed.transaction_time_ns = read_le(source, 10U, 8U);
        read_chars(source, 18U, parsed.client_order_id);
        parsed.reason = as_char(source[38]);
        if (parsed.matching_unit == 0U || parsed.sequence_number == 0U ||
            parsed.transaction_time_ns == 0U ||
            !valid_client_order_id(parsed.client_order_id) ||
            !is_printable(parsed.reason)) {
            return CodecStatus::InvalidField;
        }
    } else {
        return CodecStatus::InvalidType;
    }

    destination = parsed;
    return CodecStatus::Ok;
}

bool UnitSequenceTracker::set_next(
    const std::uint8_t matching_unit,
    const std::uint32_t next_sequence) noexcept {
    if (matching_unit == 0U || next_sequence == 0U) {
        return false;
    }
    next_[matching_unit] = next_sequence;
    return true;
}

CodecStatus UnitSequenceTracker::accept(
    const OrderResponse& response) noexcept {
    const bool sequenced = response.kind == ResponseKind::OrderAcknowledged ||
        response.kind == ResponseKind::OrderExecuted ||
        response.kind == ResponseKind::OrderCanceled;
    if (!sequenced) {
        return response.matching_unit == 0U && response.sequence_number == 0U
            ? CodecStatus::Ok
            : CodecStatus::InvalidField;
    }
    if (response.matching_unit == 0U || response.sequence_number == 0U) {
        return CodecStatus::InvalidField;
    }
    auto& expected = next_[response.matching_unit];
    if (expected == 0U) {
        return CodecStatus::SequenceNotConfigured;
    }
    if (response.sequence_number != expected ||
        expected == std::numeric_limits<std::uint32_t>::max()) {
        return CodecStatus::SequenceMismatch;
    }
    ++expected;
    return CodecStatus::Ok;
}

std::uint32_t UnitSequenceTracker::next(
    const std::uint8_t matching_unit) const noexcept {
    return next_[matching_unit];
}

BoeChildFrameEncoder::BoeChildFrameEncoder(
    const VenueId venue,
    const char capacity,
    const RoutingInstruction aggressive_routing_instruction_value,
    const RoutingInstruction passive_routing_instruction_value,
    const Text16 account) noexcept
    : venue_(venue),
      capacity_(capacity),
      aggressive_routing_instruction_(aggressive_routing_instruction_value),
      passive_routing_instruction_(passive_routing_instruction_value),
      account_(account) {}

bool BoeChildFrameEncoder::add_instrument(
    const InstrumentSymbol binding) noexcept {
    if (sealed_ || !binding.instrument || !valid_symbol(binding.symbol) ||
        symbol_count_ == symbols_.size()) {
        return false;
    }
    const auto slot = static_cast<std::size_t>(binding.instrument.value()) %
        symbols_.size();
    for (std::size_t probe{}; probe < symbols_.size(); ++probe) {
        auto& candidate = symbols_[(slot + probe) % symbols_.size()];
        if (candidate.occupied &&
            candidate.binding.instrument == binding.instrument) {
            return false;
        }
        if (!candidate.occupied) {
            candidate = Slot{binding, true};
            ++symbol_count_;
            return true;
        }
    }
    return false;
}

bool BoeChildFrameEncoder::seal() noexcept {
    if (sealed_ || !venue_ || symbol_count_ == 0U ||
        !valid_capacity(capacity_) ||
        !valid_routing_instruction(aggressive_routing_instruction_) ||
        !valid_routing_instruction(passive_routing_instruction_) ||
        !aggressive_routing_instruction(aggressive_routing_instruction_) ||
        !passive_routing_instruction(passive_routing_instruction_) ||
        !valid_account(account_)) {
        return false;
    }
    sealed_ = true;
    return true;
}

bool BoeChildFrameEncoder::sealed() const noexcept {
    return sealed_;
}

const Symbol* BoeChildFrameEncoder::find_symbol(
    const InstrumentId instrument) const noexcept {
    if (!instrument) {
        return nullptr;
    }
    const auto slot = static_cast<std::size_t>(instrument.value()) % symbols_.size();
    for (std::size_t probe{}; probe < symbols_.size(); ++probe) {
        const auto& candidate = symbols_[(slot + probe) % symbols_.size()];
        if (!candidate.occupied) {
            return nullptr;
        }
        if (candidate.binding.instrument == instrument) {
            return &candidate.binding.symbol;
        }
    }
    return nullptr;
}

fastpath::FrameEncodeResult BoeChildFrameEncoder::encode_new(
    const ParentOrder& parent,
    const ChildAllocation& allocation,
    const dataplane::PlannedChild& child,
    const std::span<std::byte> destination) noexcept {
    if (!sealed_ || !parent.id || !child.child_order_id ||
        child.dispatch_attempt_id == 0U || allocation.venue != venue_ ||
        allocation.venue_quantity.units >
            std::numeric_limits<std::uint32_t>::max()) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }
    PlannedWireIdentity identity;
    const Symbol* const symbol = find_symbol(parent.instrument);
    if (symbol == nullptr ||
        !parse_planned_wire_identity(child.wire_identity, identity)) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }

    NewOrder message;
    message.identity = identity;
    message.side = parent.side == Side::Buy ? BoeSide::Buy : BoeSide::Sell;
    message.quantity = static_cast<std::uint32_t>(
        allocation.venue_quantity.units);
    message.price = allocation.venue_limit_price.units;
    message.symbol = *symbol;
    message.capacity = capacity_;
    message.routing_instruction = parent.intent == RoutingIntent::AggressiveIOC
        ? aggressive_routing_instruction_
        : passive_routing_instruction_;
    message.account = account_;
    message.time_in_force = parent.intent == RoutingIntent::AggressiveIOC
        ? '3'
        : '1';
    message.include_time_in_force = true;
    const auto result = encode_new_order(message, destination);
    return result.ok()
        ? fastpath::FrameEncodeResult{
            fastpath::FrameEncodeStatus::Encoded,
            result.size}
        : fastpath::FrameEncodeResult{map_status(result.status), 0U};
}

fastpath::FrameEncodeResult BoeChildFrameEncoder::encode_cancel(
    const fastpath::CancelCommand& command,
    const fastpath::ActiveChildSnapshot& child,
    const std::uint64_t dispatch_attempt_id,
    const std::span<std::byte> destination) noexcept {
    if (!sealed_ || !command.valid() || !child.valid() ||
        dispatch_attempt_id == 0U ||
        dispatch_attempt_id > std::numeric_limits<std::uint32_t>::max() ||
        command.venue_id != venue_ || child.venue_id != venue_ ||
        command.child_id != child.child_id ||
        command.parent_order_id != child.parent_order_id ||
        command.expected_ownership_generation != child.ownership_generation ||
        command.expected_venue_session_id != child.venue_session_id ||
        command.expected_venue_session_epoch != child.venue_session_epoch) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }

    PlannedWireIdentity identity;
    if (!parse_planned_wire_identity(child.wire_identity, identity)) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }
    const auto encoded = encode_cancel_order(
        CancelOrder{
            static_cast<std::uint32_t>(dispatch_attempt_id),
            identity.client_order_id},
        destination);
    return encoded.ok()
        ? fastpath::FrameEncodeResult{
            fastpath::FrameEncodeStatus::Encoded,
            encoded.size}
        : fastpath::FrameEncodeResult{map_status(encoded.status), 0U};
}

}  // namespace sorx::connectors::cboe
