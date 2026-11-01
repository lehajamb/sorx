#include "sorx/connectors/nasdaq_ouch5.hpp"

#include <algorithm>
#include <limits>

namespace sorx::connectors::nasdaq {
namespace {

[[nodiscard]] constexpr std::byte as_byte(const char value) noexcept {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
}

[[nodiscard]] constexpr char as_char(const std::byte value) noexcept {
    return static_cast<char>(std::to_integer<unsigned char>(value));
}

void write_be(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint64_t value,
    const std::size_t width) noexcept {
    for (std::size_t index{}; index < width; ++index) {
        const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
        destination[offset + index] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint64_t read_be(
    const std::span<const std::byte> source,
    const std::size_t offset,
    const std::size_t width) noexcept {
    std::uint64_t value{};
    for (std::size_t index{}; index < width; ++index) {
        value = (value << 8U) |
            std::to_integer<std::uint64_t>(source[offset + index]);
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

template <std::size_t Size>
[[nodiscard]] bool valid_space_padded_alphanumeric(
    const std::array<char, Size>& value) noexcept {
    bool saw_value{};
    bool saw_padding{};
    for (const char character : value) {
        if (character == ' ') {
            saw_padding = true;
            continue;
        }
        if (saw_padding || !is_alphanumeric(character)) {
            return false;
        }
        saw_value = true;
    }
    return saw_value;
}

[[nodiscard]] constexpr bool valid_side(const OuchSide side) noexcept {
    switch (side) {
        case OuchSide::Buy:
        case OuchSide::Sell:
        case OuchSide::SellShort:
        case OuchSide::SellShortExempt:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_time_in_force(const char value) noexcept {
    return value == '0' || value == '3' || value == '5' ||
        value == '6' || value == 'E';
}

[[nodiscard]] constexpr bool valid_display(const char value) noexcept {
    return value == 'Y' || value == 'N' || value == 'A';
}

[[nodiscard]] constexpr bool valid_capacity(const char value) noexcept {
    return value == 'A' || value == 'P' || value == 'R' || value == 'O';
}

[[nodiscard]] constexpr bool valid_cross_type(const char value) noexcept {
    return value == 'N' || value == 'O' || value == 'C' || value == 'H' ||
        value == 'S' || value == 'R' || value == 'E' || value == 'A';
}

[[nodiscard]] constexpr bool valid_price(const std::uint64_t price) noexcept {
    return price <= kOuchMaximumLimitPrice || price == 0x7fffffffULL;
}

[[nodiscard]] constexpr bool valid_liquidity_flag(const char value) noexcept {
    switch (value) {
        case 'A':
        case 'C':
        case 'e':
        case 'H':
        case 'i':
        case 'J':
        case 'j':
        case 'k':
        case 'K':
        case 'L':
        case 'M':
        case 'm':
        case 'N':
        case 'n':
        case 'O':
        case 'p':
        case 'q':
        case 'R':
        case 'r':
        case 't':
        case 'u':
        case '0':
        case '1':
        case '2':
        case '3':
        case '5':
        case '7':
        case '8':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool valid_identity(const PlannedWireIdentity& identity) noexcept {
    return identity.user_reference_number != 0U &&
        valid_space_padded_alphanumeric(identity.client_order_id);
}

[[nodiscard]] CodecStatus validate_appendage(
    const std::span<const std::byte> source,
    const std::size_t length_offset,
    const std::size_t payload_offset,
    const bool length_field_optional) noexcept {
    if (length_field_optional && source.size() == length_offset) {
        return CodecStatus::Ok;
    }
    if (source.size() < payload_offset) {
        return CodecStatus::Truncated;
    }
    const auto appendage_length = static_cast<std::size_t>(
        read_be(source, length_offset, 2U));
    return source.size() == payload_offset + appendage_length
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
        case CodecStatus::Unsupported:
            return fastpath::FrameEncodeStatus::Unsupported;
        default:
            return fastpath::FrameEncodeStatus::InvalidInput;
    }
}

[[nodiscard]] bool make_route_client_order_id(
    const dataplane::WireIdentityPlanRequest& request,
    ClientOrderId& output) noexcept {
    static constexpr std::string_view alphabet{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"};
    output.fill('0');
    const std::uint64_t session_tag = request.venue_session_id ^
        (request.venue_session_epoch << 1U) ^ request.venue.value();
    output[0] = alphabet[session_tag % alphabet.size()];
    std::uint64_t value = request.child_order_id.value();
    for (std::size_t position = output.size(); position-- > 1U;) {
        output[position] = alphabet[value % alphabet.size()];
        value /= alphabet.size();
    }
    return value == 0U;
}

}  // namespace

bool make_planned_wire_identity(
    const std::uint32_t user_reference_number,
    const std::string_view client_order_id,
    dataplane::InlineWireIdentity& destination) noexcept {
    if (user_reference_number == 0U || client_order_id.empty() ||
        client_order_id.size() > ClientOrderId{}.size()) {
        return false;
    }
    for (const char character : client_order_id) {
        if (!is_alphanumeric(character)) {
            return false;
        }
    }

    destination = {};
    destination.size = static_cast<std::uint8_t>(
        kOuchPlannedWireIdentityBytes);
    write_be(destination.bytes, 0U, user_reference_number, 4U);
    for (std::size_t index{}; index < ClientOrderId{}.size(); ++index) {
        const char value = index < client_order_id.size()
            ? client_order_id[index]
            : ' ';
        destination.bytes[4U + index] = as_byte(value);
    }
    return true;
}

bool parse_planned_wire_identity(
    const dataplane::InlineWireIdentity& source,
    PlannedWireIdentity& destination) noexcept {
    if (source.size != kOuchPlannedWireIdentityBytes) {
        return false;
    }
    PlannedWireIdentity parsed;
    parsed.user_reference_number = static_cast<std::uint32_t>(
        read_be(source.value(), 0U, 4U));
    read_chars(source.value(), 4U, parsed.client_order_id);
    if (!valid_identity(parsed)) {
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
    ClientOrderId client_order_id{};
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

CodecResult encode_enter_order(
    const EnterOrder& message,
    const std::span<std::byte> destination) noexcept {
    const auto required_size = message.post_only
        ? kOuchEnterOrderPostOnlyBytes
        : kOuchEnterOrderBytes;
    if (destination.size() < required_size) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (!valid_identity(message.identity) || !valid_side(message.side) ||
        message.quantity == 0U || message.quantity > 999'999U ||
        !valid_space_padded_alphanumeric(message.symbol) ||
        !valid_price(message.price) ||
        !valid_time_in_force(message.time_in_force) ||
        !valid_display(message.display) || !valid_capacity(message.capacity) ||
        (message.intermarket_sweep_eligibility != 'Y' &&
         message.intermarket_sweep_eligibility != 'N') ||
        !valid_cross_type(message.cross_type)) {
        return {CodecStatus::InvalidField, 0U};
    }

    destination[0] = as_byte('O');
    write_be(destination, 1U, message.identity.user_reference_number, 4U);
    destination[5] = as_byte(static_cast<char>(message.side));
    write_be(destination, 6U, message.quantity, 4U);
    write_chars(destination, 10U, message.symbol);
    write_be(destination, 18U, message.price, 8U);
    destination[26] = as_byte(message.time_in_force);
    destination[27] = as_byte(message.display);
    destination[28] = as_byte(message.capacity);
    destination[29] = as_byte(message.intermarket_sweep_eligibility);
    destination[30] = as_byte(message.cross_type);
    write_chars(destination, 31U, message.identity.client_order_id);
    if (message.post_only) {
        write_be(destination, 45U, 3U, 2U);
        destination[47] = std::byte{2};
        destination[48] = std::byte{12};
        destination[49] = as_byte('P');
    } else {
        write_be(destination, 45U, 0U, 2U);
    }
    return {CodecStatus::Ok, static_cast<std::uint16_t>(required_size)};
}

CodecResult encode_replace_order(
    const ReplaceOrder& message,
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < kOuchReplaceOrderBytes) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (message.original_user_reference_number == 0U ||
        !valid_identity(message.replacement_identity) ||
        message.original_user_reference_number ==
            message.replacement_identity.user_reference_number ||
        message.total_quantity == 0U || message.total_quantity > 999'999U ||
        !valid_price(message.price) ||
        !valid_time_in_force(message.time_in_force) ||
        !valid_display(message.display) ||
        (message.intermarket_sweep_eligibility != 'Y' &&
         message.intermarket_sweep_eligibility != 'N')) {
        return {CodecStatus::InvalidField, 0U};
    }

    destination[0] = as_byte('U');
    write_be(destination, 1U, message.original_user_reference_number, 4U);
    write_be(
        destination,
        5U,
        message.replacement_identity.user_reference_number,
        4U);
    write_be(destination, 9U, message.total_quantity, 4U);
    write_be(destination, 13U, message.price, 8U);
    destination[21] = as_byte(message.time_in_force);
    destination[22] = as_byte(message.display);
    destination[23] = as_byte(message.intermarket_sweep_eligibility);
    write_chars(destination, 24U, message.replacement_identity.client_order_id);
    write_be(destination, 38U, 0U, 2U);
    return {CodecStatus::Ok, static_cast<std::uint16_t>(kOuchReplaceOrderBytes)};
}

CodecResult encode_cancel_order(
    const CancelOrder& message,
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < kOuchCancelOrderBytes) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (message.user_reference_number == 0U ||
        message.intended_total_quantity > 999'999U) {
        return {CodecStatus::InvalidField, 0U};
    }
    destination[0] = as_byte('X');
    write_be(destination, 1U, message.user_reference_number, 4U);
    write_be(destination, 5U, message.intended_total_quantity, 4U);
    write_be(destination, 9U, 0U, 2U);
    return {CodecStatus::Ok, static_cast<std::uint16_t>(kOuchCancelOrderBytes)};
}

CodecResult encode_modify_order(
    const ModifyOrder& message,
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < kOuchModifyOrderBytes) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    if (message.user_reference_number == 0U || !valid_side(message.side) ||
        message.intended_total_quantity > 999'999U) {
        return {CodecStatus::InvalidField, 0U};
    }
    destination[0] = as_byte('M');
    write_be(destination, 1U, message.user_reference_number, 4U);
    destination[5] = as_byte(static_cast<char>(message.side));
    write_be(destination, 6U, message.intended_total_quantity, 4U);
    write_be(destination, 10U, 0U, 2U);
    return {CodecStatus::Ok, static_cast<std::uint16_t>(kOuchModifyOrderBytes)};
}

CodecResult encode_soup_unsequenced(
    const std::span<const std::byte> payload,
    const std::span<std::byte> destination) noexcept {
    if (payload.size() > std::numeric_limits<std::uint16_t>::max() - 1U) {
        return {CodecStatus::InvalidLength, 0U};
    }
    const auto total = payload.size() + 3U;
    if (destination.size() < total) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    write_be(destination, 0U, payload.size() + 1U, 2U);
    destination[2] = as_byte('U');
    std::copy(payload.begin(), payload.end(), destination.begin() + 3);
    return {CodecStatus::Ok, static_cast<std::uint16_t>(total)};
}

CodecResult encode_soup_client_heartbeat(
    const std::span<std::byte> destination) noexcept {
    if (destination.size() < 3U) {
        return {CodecStatus::BufferTooSmall, 0U};
    }
    destination[0] = std::byte{0};
    destination[1] = std::byte{1};
    destination[2] = as_byte('R');
    return {CodecStatus::Ok, 3U};
}

CodecStatus decode_soup_packet(
    const std::span<const std::byte> source,
    SoupPacketView& destination) noexcept {
    if (source.size() < 3U) {
        return CodecStatus::Truncated;
    }
    const auto packet_length = static_cast<std::size_t>(read_be(source, 0U, 2U));
    if (packet_length == 0U || source.size() != packet_length + 2U) {
        return CodecStatus::InvalidLength;
    }

    SoupPacketType type{};
    switch (as_char(source[2])) {
        case 'S': type = SoupPacketType::SequencedData; break;
        case 'U': type = SoupPacketType::UnsequencedData; break;
        case 'H': type = SoupPacketType::ServerHeartbeat; break;
        case 'R': type = SoupPacketType::ClientHeartbeat; break;
        case 'Z': type = SoupPacketType::EndOfSession; break;
        default: return CodecStatus::Unsupported;
    }
    if ((type == SoupPacketType::ServerHeartbeat ||
         type == SoupPacketType::ClientHeartbeat ||
         type == SoupPacketType::EndOfSession) && packet_length != 1U) {
        return CodecStatus::InvalidLength;
    }
    destination = SoupPacketView{type, source.subspan(3U)};
    return CodecStatus::Ok;
}

CodecStatus SoupSequenceTracker::accept(
    const SoupPacketView& packet,
    std::uint64_t& packet_sequence) noexcept {
    packet_sequence = 0U;
    if (packet.type != SoupPacketType::SequencedData) {
        return CodecStatus::Ok;
    }
    if (next_sequence_ == 0U ||
        next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return CodecStatus::SequenceMismatch;
    }
    packet_sequence = next_sequence_;
    ++next_sequence_;
    return CodecStatus::Ok;
}

CodecStatus decode_order_response(
    const std::span<const std::byte> source,
    OrderResponse& destination) noexcept {
    if (source.empty()) {
        return CodecStatus::Truncated;
    }
    OrderResponse parsed;
    const char type = as_char(source[0]);

    if (type == 'A') {
        if (source.size() < 64U) {
            return CodecStatus::Truncated;
        }
        const auto appendage = validate_appendage(source, 62U, 64U, false);
        if (appendage != CodecStatus::Ok) {
            return appendage;
        }
        parsed.kind = ResponseKind::Accepted;
        parsed.timestamp_ns = read_be(source, 1U, 8U);
        parsed.user_reference_number = static_cast<std::uint32_t>(
            read_be(source, 9U, 4U));
        parsed.side = static_cast<OuchSide>(as_char(source[13]));
        parsed.quantity = static_cast<std::uint32_t>(read_be(source, 14U, 4U));
        read_chars(source, 18U, parsed.symbol);
        parsed.price = read_be(source, 26U, 8U);
        parsed.order_reference_number = read_be(source, 36U, 8U);
        parsed.order_state = as_char(source[47]);
        read_chars(source, 48U, parsed.client_order_id);
        if (!valid_side(parsed.side) || parsed.quantity == 0U ||
            parsed.quantity > 999'999U ||
            !valid_space_padded_alphanumeric(parsed.symbol) ||
            !valid_price(parsed.price) ||
            !valid_time_in_force(as_char(source[34])) ||
            !(valid_display(as_char(source[35])) || as_char(source[35]) == 'Z') ||
            !valid_capacity(as_char(source[44])) ||
            (as_char(source[45]) != 'Y' && as_char(source[45]) != 'N') ||
            !valid_cross_type(as_char(source[46])) ||
            (parsed.order_state != 'L' && parsed.order_state != 'D') ||
            !valid_space_padded_alphanumeric(parsed.client_order_id)) {
            return CodecStatus::InvalidField;
        }
    } else if (type == 'J') {
        if (source.size() < 29U) {
            return CodecStatus::Truncated;
        }
        const auto appendage = validate_appendage(source, 29U, 31U, true);
        if (appendage != CodecStatus::Ok) {
            return appendage;
        }
        parsed.kind = ResponseKind::Rejected;
        parsed.timestamp_ns = read_be(source, 1U, 8U);
        parsed.user_reference_number = static_cast<std::uint32_t>(
            read_be(source, 9U, 4U));
        parsed.reject_reason = static_cast<std::uint16_t>(read_be(source, 13U, 2U));
        read_chars(source, 15U, parsed.client_order_id);
        if (parsed.reject_reason == 0U ||
            !valid_space_padded_alphanumeric(parsed.client_order_id)) {
            return CodecStatus::InvalidField;
        }
    } else if (type == 'E') {
        if (source.size() < 36U) {
            return CodecStatus::Truncated;
        }
        const auto appendage = validate_appendage(source, 34U, 36U, false);
        if (appendage != CodecStatus::Ok) {
            return appendage;
        }
        parsed.kind = ResponseKind::Executed;
        parsed.timestamp_ns = read_be(source, 1U, 8U);
        parsed.user_reference_number = static_cast<std::uint32_t>(
            read_be(source, 9U, 4U));
        parsed.quantity = static_cast<std::uint32_t>(read_be(source, 13U, 4U));
        parsed.price = read_be(source, 17U, 8U);
        parsed.liquidity_flag = as_char(source[25]);
        parsed.match_number = read_be(source, 26U, 8U);
        if (parsed.quantity == 0U || parsed.quantity > 999'999U ||
            !valid_price(parsed.price) ||
            !valid_liquidity_flag(parsed.liquidity_flag) ||
            parsed.match_number == 0U) {
            return CodecStatus::InvalidField;
        }
    } else if (type == 'C') {
        if (source.size() < 18U) {
            return CodecStatus::Truncated;
        }
        const auto appendage = validate_appendage(source, 18U, 20U, true);
        if (appendage != CodecStatus::Ok) {
            return appendage;
        }
        parsed.kind = ResponseKind::Canceled;
        parsed.timestamp_ns = read_be(source, 1U, 8U);
        parsed.user_reference_number = static_cast<std::uint32_t>(
            read_be(source, 9U, 4U));
        parsed.quantity = static_cast<std::uint32_t>(read_be(source, 13U, 4U));
        parsed.cancel_reason = as_char(source[17]);
        if (parsed.quantity == 0U || parsed.quantity > 999'999U ||
            !is_alpha(parsed.cancel_reason)) {
            return CodecStatus::InvalidField;
        }
    } else {
        return CodecStatus::InvalidType;
    }

    if (parsed.timestamp_ns >= kNanosecondsPerDay ||
        parsed.user_reference_number == 0U) {
        return CodecStatus::InvalidField;
    }
    destination = parsed;
    return CodecStatus::Ok;
}

Ouch5ChildFrameEncoder::Ouch5ChildFrameEncoder(
    const VenueId venue,
    const char display,
    const char capacity,
    const char intermarket_sweep_eligibility) noexcept
    : venue_(venue),
      display_(display),
      capacity_(capacity),
      intermarket_sweep_eligibility_(intermarket_sweep_eligibility) {}

bool Ouch5ChildFrameEncoder::add_instrument(
    const InstrumentSymbol binding) noexcept {
    if (sealed_ || !binding.instrument ||
        !valid_space_padded_alphanumeric(binding.symbol) ||
        symbol_count_ == symbols_.size()) {
        return false;
    }
    auto slot = static_cast<std::size_t>(binding.instrument.value()) % symbols_.size();
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

bool Ouch5ChildFrameEncoder::seal() noexcept {
    if (sealed_ || !venue_ || symbol_count_ == 0U ||
        !valid_display(display_) || !valid_capacity(capacity_) ||
        (intermarket_sweep_eligibility_ != 'Y' &&
         intermarket_sweep_eligibility_ != 'N')) {
        return false;
    }
    sealed_ = true;
    return true;
}

bool Ouch5ChildFrameEncoder::sealed() const noexcept {
    return sealed_;
}

const Symbol* Ouch5ChildFrameEncoder::find_symbol(
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

fastpath::FrameEncodeResult Ouch5ChildFrameEncoder::encode_new(
    const ParentOrder& parent,
    const ChildAllocation& allocation,
    const dataplane::PlannedChild& child,
    const std::span<std::byte> destination) noexcept {
    constexpr std::size_t soup_header_size = 3U;
    if (!sealed_ || !parent.id || !child.child_order_id ||
        child.dispatch_attempt_id == 0U || allocation.venue != venue_ ||
        allocation.venue_quantity.units >
            std::numeric_limits<std::uint32_t>::max()) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }
    if (destination.size() < soup_header_size + kOuchEnterOrderBytes) {
        return {fastpath::FrameEncodeStatus::BufferTooSmall, 0U};
    }
    PlannedWireIdentity identity;
    const Symbol* const symbol = find_symbol(parent.instrument);
    if (symbol == nullptr || !parse_planned_wire_identity(
            child.wire_identity, identity)) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }

    EnterOrder message;
    message.identity = identity;
    message.side = parent.side == Side::Buy ? OuchSide::Buy : OuchSide::Sell;
    message.quantity = static_cast<std::uint32_t>(
        allocation.venue_quantity.units);
    message.symbol = *symbol;
    if (allocation.venue_limit_price.units < 0) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }
    message.price = static_cast<std::uint64_t>(
        allocation.venue_limit_price.units);
    message.time_in_force = parent.intent == RoutingIntent::AggressiveIOC
        ? '3'
        : '0';
    message.display = display_;
    message.capacity = capacity_;
    message.intermarket_sweep_eligibility =
        intermarket_sweep_eligibility_;
    message.cross_type = 'N';
    message.post_only = parent.intent == RoutingIntent::PassiveGTC;

    const auto encoded = encode_enter_order(
        message, destination.subspan(soup_header_size));
    if (!encoded.ok()) {
        return {map_status(encoded.status), 0U};
    }
    write_be(destination, 0U, encoded.size + 1U, 2U);
    destination[2] = as_byte('U');
    const auto size = static_cast<std::uint16_t>(encoded.size + soup_header_size);
    return {fastpath::FrameEncodeStatus::Encoded, size};
}

fastpath::FrameEncodeResult Ouch5ChildFrameEncoder::encode_cancel(
    const fastpath::CancelCommand& command,
    const fastpath::ActiveChildSnapshot& child,
    const std::uint64_t dispatch_attempt_id,
    const std::span<std::byte> destination) noexcept {
    constexpr std::size_t soup_header_size = 3U;
    if (!sealed_ || !command.valid() || !child.valid() ||
        dispatch_attempt_id == 0U || command.venue_id != venue_ ||
        child.venue_id != venue_ || command.child_id != child.child_id ||
        command.parent_order_id != child.parent_order_id ||
        command.expected_ownership_generation != child.ownership_generation ||
        command.expected_venue_session_id != child.venue_session_id ||
        command.expected_venue_session_epoch != child.venue_session_epoch) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }
    if (destination.size() < soup_header_size + kOuchCancelOrderBytes) {
        return {fastpath::FrameEncodeStatus::BufferTooSmall, 0U};
    }

    PlannedWireIdentity identity;
    if (!parse_planned_wire_identity(child.wire_identity, identity)) {
        return {fastpath::FrameEncodeStatus::InvalidInput, 0U};
    }
    const auto encoded = encode_cancel_order(
        CancelOrder{identity.user_reference_number, 0U},
        destination.subspan(soup_header_size));
    if (!encoded.ok()) {
        return {map_status(encoded.status), 0U};
    }
    write_be(destination, 0U, encoded.size + 1U, 2U);
    destination[2] = as_byte('U');
    return {
        fastpath::FrameEncodeStatus::Encoded,
        static_cast<std::uint16_t>(encoded.size + soup_header_size)};
}

}  // namespace sorx::connectors::nasdaq
