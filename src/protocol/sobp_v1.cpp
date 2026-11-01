#include "sorx/protocol/sobp_v1.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace sorx::protocol::sobp_v1 {
namespace {

constexpr std::uint16_t known_flags =
    flag(HeaderFlag::Response) |
    flag(HeaderFlag::Retransmission) |
    flag(HeaderFlag::Administrative);

class Writer final {
public:
    explicit Writer(const std::span<std::byte> output) : output_(output) {}

    void u8(const std::uint8_t value) {
        if (offset_ >= output_.size()) {
            ok_ = false;
            return;
        }
        output_[offset_++] = static_cast<std::byte>(value);
    }

    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value >> 8U));
        u8(static_cast<std::uint8_t>(value));
    }

    void u32(const std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value >> 16U));
        u16(static_cast<std::uint16_t>(value));
    }

    void u64(const std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value >> 32U));
        u32(static_cast<std::uint32_t>(value));
    }

    void i64(const std::int64_t value) {
        u64(static_cast<std::uint64_t>(value));
    }

    template <std::size_t Size>
    void text(const FixedText<Size>& value) {
        for (const char item : value) {
            u8(static_cast<std::uint8_t>(item));
        }
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t written() const noexcept { return offset_; }

private:
    std::span<std::byte> output_;
    std::size_t offset_{};
    bool ok_{true};
};

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> input) : input_(input) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return ok_ ? input_.size() - offset_ : 0U;
    }

    std::uint8_t u8() {
        if (offset_ >= input_.size()) {
            ok_ = false;
            return 0U;
        }
        return std::to_integer<std::uint8_t>(input_[offset_++]);
    }

    std::uint16_t u16() {
        const auto high = u8();
        const auto low = u8();
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(high) << 8U) | low);
    }

    std::uint32_t u32() {
        const auto high = u16();
        const auto low = u16();
        return (static_cast<std::uint32_t>(high) << 16U) | low;
    }

    std::uint64_t u64() {
        const auto high = u32();
        const auto low = u32();
        return (static_cast<std::uint64_t>(high) << 32U) | low;
    }

    std::int64_t i64() {
        return static_cast<std::int64_t>(u64());
    }

    template <std::size_t Size>
    FixedText<Size> text(bool& canonical) {
        FixedText<Size> output{};
        bool saw_terminator = false;
        for (char& item : output) {
            const auto value = u8();
            item = static_cast<char>(value);
            if (value == 0U) {
                saw_terminator = true;
            } else if (saw_terminator || value < 0x20U || value > 0x7EU) {
                canonical = false;
            }
        }
        return output;
    }

private:
    std::span<const std::byte> input_;
    std::size_t offset_{};
    bool ok_{true};
};

[[nodiscard]] bool valid_message_type(const MessageType type) noexcept {
    switch (type) {
        case MessageType::Logon:
        case MessageType::Heartbeat:
        case MessageType::NewOrder:
        case MessageType::CancelOrder:
        case MessageType::ReplaceOrder:
        case MessageType::MassCancel:
        case MessageType::DurableAck:
        case MessageType::Reject:
        case MessageType::Execution:
        case MessageType::Status:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_side(const Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] bool valid_intent(const RoutingIntent intent) noexcept {
    return intent == RoutingIntent::AggressiveIoc ||
        intent == RoutingIntent::PassiveGtc;
}

[[nodiscard]] bool valid_scope(const MassCancelScope scope) noexcept {
    return scope == MassCancelScope::Session ||
        scope == MassCancelScope::Instrument ||
        scope == MassCancelScope::Venue ||
        scope == MassCancelScope::InstrumentAndVenue;
}

[[nodiscard]] bool valid_execution_status(
    const ExecutionStatus status) noexcept {
    return status >= ExecutionStatus::Pending &&
        status <= ExecutionStatus::Ambiguous;
}

[[nodiscard]] bool valid_appliance_status(
    const ApplianceStatus status) noexcept {
    return status >= ApplianceStatus::Bootstrapping &&
        status <= ApplianceStatus::Failed;
}

[[nodiscard]] bool valid_reject_code(const RejectCode code) noexcept {
    return code >= RejectCode::Malformed && code <= RejectCode::InternalFailure;
}

void encode_payload(Writer& writer, const Payload& payload) {
    std::visit(
        [&writer](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, LogonPayload>) {
                writer.u16(value.minimum_version);
                writer.u16(value.maximum_version);
                writer.u32(value.heartbeat_interval_ms);
                writer.u64(value.client_instance_id);
                writer.u64(value.resume_from_sequence);
                writer.text(value.account_context);
            } else if constexpr (std::is_same_v<T, HeartbeatPayload>) {
                writer.u64(value.last_received_sequence);
                writer.u64(value.monotonic_time_ns);
            } else if constexpr (std::is_same_v<T, NewOrderPayload>) {
                writer.u64(value.parent_order_id);
                writer.u32(value.instrument_id);
                writer.u8(static_cast<std::uint8_t>(value.side));
                writer.u8(static_cast<std::uint8_t>(value.intent));
                writer.u16(0U);
                writer.u64(value.quantity);
                writer.i64(value.limit_price);
                writer.text(value.client_order_id);
            } else if constexpr (std::is_same_v<T, CancelOrderPayload>) {
                writer.u64(value.parent_order_id);
                writer.text(value.client_order_id);
            } else if constexpr (std::is_same_v<T, ReplaceOrderPayload>) {
                writer.u64(value.parent_order_id);
                writer.u64(value.new_total_quantity);
                writer.i64(value.new_limit_price);
                writer.text(value.client_order_id);
            } else if constexpr (std::is_same_v<T, MassCancelPayload>) {
                writer.u8(static_cast<std::uint8_t>(value.scope));
                writer.u8(value.side_filter);
                writer.u16(0U);
                writer.u32(value.instrument_id);
                writer.u32(value.venue_id);
                writer.text(value.client_order_id);
            } else if constexpr (std::is_same_v<T, DurableAckPayload>) {
                writer.u64(value.acknowledged_request_id);
                writer.u64(value.journal_record_id);
                writer.u64(value.ingress_sequence);
                writer.u16(static_cast<std::uint16_t>(value.acknowledged_type));
                writer.u16(0U);
            } else if constexpr (std::is_same_v<T, RejectPayload>) {
                writer.u64(value.rejected_request_id);
                writer.u16(static_cast<std::uint16_t>(value.code));
                writer.u16(static_cast<std::uint16_t>(value.rejected_type));
                writer.text(value.detail);
            } else if constexpr (std::is_same_v<T, ExecutionPayload>) {
                writer.u64(value.originating_request_id);
                writer.u64(value.parent_order_id);
                writer.u64(value.child_order_id);
                writer.u32(value.venue_id);
                writer.u8(static_cast<std::uint8_t>(value.status));
                writer.u8(static_cast<std::uint8_t>(value.side));
                writer.u16(0U);
                writer.u64(value.last_quantity);
                writer.i64(value.last_price);
                writer.u64(value.cumulative_quantity);
                writer.u64(value.leaves_quantity);
                writer.text(value.execution_id);
            } else if constexpr (std::is_same_v<T, StatusPayload>) {
                writer.u16(static_cast<std::uint16_t>(value.status));
                writer.u8(value.routing_enabled ? 1U : 0U);
                writer.u8(value.kill_switch_active ? 1U : 0U);
                writer.u64(value.last_ingress_sequence);
                writer.u32(value.active_parents);
                writer.u32(value.active_children);
                writer.u32(value.normal_credits_available);
                writer.u32(value.cancel_credits_available);
                writer.text(value.detail);
            }
        },
        payload);
}

[[nodiscard]] ProtocolError decode_payload(
    const MessageType type,
    Reader& reader,
    Payload& payload) {
    bool canonical = true;
    switch (type) {
        case MessageType::Logon: {
            LogonPayload value;
            value.minimum_version = reader.u16();
            value.maximum_version = reader.u16();
            value.heartbeat_interval_ms = reader.u32();
            value.client_instance_id = reader.u64();
            value.resume_from_sequence = reader.u64();
            value.account_context = reader.text<32U>(canonical);
            payload = value;
            break;
        }
        case MessageType::Heartbeat: {
            HeartbeatPayload value;
            value.last_received_sequence = reader.u64();
            value.monotonic_time_ns = reader.u64();
            payload = value;
            break;
        }
        case MessageType::NewOrder: {
            NewOrderPayload value;
            value.parent_order_id = reader.u64();
            value.instrument_id = reader.u32();
            value.side = static_cast<Side>(reader.u8());
            value.intent = static_cast<RoutingIntent>(reader.u8());
            if (reader.u16() != 0U) {
                return ProtocolError::ReservedFieldNonzero;
            }
            value.quantity = reader.u64();
            value.limit_price = reader.i64();
            value.client_order_id = reader.text<32U>(canonical);
            if (!valid_side(value.side) || !valid_intent(value.intent)) {
                return ProtocolError::InvalidValue;
            }
            payload = value;
            break;
        }
        case MessageType::CancelOrder: {
            CancelOrderPayload value;
            value.parent_order_id = reader.u64();
            value.client_order_id = reader.text<32U>(canonical);
            payload = value;
            break;
        }
        case MessageType::ReplaceOrder: {
            ReplaceOrderPayload value;
            value.parent_order_id = reader.u64();
            value.new_total_quantity = reader.u64();
            value.new_limit_price = reader.i64();
            value.client_order_id = reader.text<32U>(canonical);
            payload = value;
            break;
        }
        case MessageType::MassCancel: {
            MassCancelPayload value;
            value.scope = static_cast<MassCancelScope>(reader.u8());
            value.side_filter = reader.u8();
            if (reader.u16() != 0U) {
                return ProtocolError::ReservedFieldNonzero;
            }
            value.instrument_id = reader.u32();
            value.venue_id = reader.u32();
            value.client_order_id = reader.text<32U>(canonical);
            if (!valid_scope(value.scope) ||
                (value.side_filter != 0U &&
                 !valid_side(static_cast<Side>(value.side_filter)))) {
                return ProtocolError::InvalidValue;
            }
            payload = value;
            break;
        }
        case MessageType::DurableAck: {
            DurableAckPayload value;
            value.acknowledged_request_id = reader.u64();
            value.journal_record_id = reader.u64();
            value.ingress_sequence = reader.u64();
            value.acknowledged_type = static_cast<MessageType>(reader.u16());
            if (reader.u16() != 0U) {
                return ProtocolError::ReservedFieldNonzero;
            }
            if (!valid_message_type(value.acknowledged_type)) {
                return ProtocolError::InvalidValue;
            }
            payload = value;
            break;
        }
        case MessageType::Reject: {
            RejectPayload value;
            value.rejected_request_id = reader.u64();
            value.code = static_cast<RejectCode>(reader.u16());
            value.rejected_type = static_cast<MessageType>(reader.u16());
            value.detail = reader.text<52U>(canonical);
            if (!valid_reject_code(value.code) ||
                !valid_message_type(value.rejected_type)) {
                return ProtocolError::InvalidValue;
            }
            payload = value;
            break;
        }
        case MessageType::Execution: {
            ExecutionPayload value;
            value.originating_request_id = reader.u64();
            value.parent_order_id = reader.u64();
            value.child_order_id = reader.u64();
            value.venue_id = reader.u32();
            value.status = static_cast<ExecutionStatus>(reader.u8());
            value.side = static_cast<Side>(reader.u8());
            if (reader.u16() != 0U) {
                return ProtocolError::ReservedFieldNonzero;
            }
            value.last_quantity = reader.u64();
            value.last_price = reader.i64();
            value.cumulative_quantity = reader.u64();
            value.leaves_quantity = reader.u64();
            value.execution_id = reader.text<24U>(canonical);
            if (!valid_execution_status(value.status) || !valid_side(value.side)) {
                return ProtocolError::InvalidValue;
            }
            payload = value;
            break;
        }
        case MessageType::Status: {
            StatusPayload value;
            value.status = static_cast<ApplianceStatus>(reader.u16());
            const auto routing = reader.u8();
            const auto kill = reader.u8();
            value.routing_enabled = routing != 0U;
            value.kill_switch_active = kill != 0U;
            value.last_ingress_sequence = reader.u64();
            value.active_parents = reader.u32();
            value.active_children = reader.u32();
            value.normal_credits_available = reader.u32();
            value.cancel_credits_available = reader.u32();
            value.detail = reader.text<48U>(canonical);
            if (!valid_appliance_status(value.status) || routing > 1U || kill > 1U) {
                return ProtocolError::InvalidValue;
            }
            payload = value;
            break;
        }
    }
    if (!reader.ok() || reader.remaining() != 0U) {
        return ProtocolError::PayloadSizeMismatch;
    }
    return canonical ? ProtocolError::None : ProtocolError::NonCanonicalText;
}

[[nodiscard]] bool has_response_flag(const Message& message) noexcept {
    return (message.header.flags & flag(HeaderFlag::Response)) != 0U;
}

[[nodiscard]] bool client_message(const Message& message) noexcept {
    switch (message.header.message_type) {
        case MessageType::Logon:
            return !has_response_flag(message);
        case MessageType::Heartbeat:
        case MessageType::NewOrder:
        case MessageType::CancelOrder:
        case MessageType::ReplaceOrder:
        case MessageType::MassCancel:
            return true;
        case MessageType::DurableAck:
        case MessageType::Reject:
        case MessageType::Execution:
        case MessageType::Status:
            return false;
    }
    return false;
}

enum class CreditClass : std::uint8_t {
    None,
    Normal,
    Cancel,
};

[[nodiscard]] CreditClass credit_class(const MessageType type) noexcept {
    switch (type) {
        case MessageType::NewOrder:
        case MessageType::ReplaceOrder:
            return CreditClass::Normal;
        case MessageType::CancelOrder:
        case MessageType::MassCancel:
            return CreditClass::Cancel;
        default:
            return CreditClass::None;
    }
}

[[nodiscard]] bool requires_request_id(const MessageType type) noexcept {
    return type != MessageType::Logon && type != MessageType::Heartbeat;
}

[[nodiscard]] bool command_message(const MessageType type) noexcept {
    return type == MessageType::NewOrder || type == MessageType::CancelOrder ||
        type == MessageType::ReplaceOrder || type == MessageType::MassCancel;
}

[[nodiscard]] bool valid_payload_semantics(
    const Message& message,
    const SessionPolicy& policy) noexcept {
    return std::visit(
        [&policy](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, LogonPayload>) {
                return value.minimum_version <= version &&
                    value.maximum_version >= version &&
                    value.minimum_version <= value.maximum_version &&
                    value.client_instance_id != 0U &&
                    value.heartbeat_interval_ms >=
                        policy.minimum_heartbeat_interval_ms &&
                    value.heartbeat_interval_ms <=
                        policy.maximum_heartbeat_interval_ms &&
                    !text_view(value.account_context).empty();
            } else if constexpr (std::is_same_v<T, NewOrderPayload>) {
                return value.parent_order_id != 0U && value.instrument_id != 0U &&
                    value.quantity != 0U && value.limit_price > 0 &&
                    valid_side(value.side) && valid_intent(value.intent) &&
                    !text_view(value.client_order_id).empty();
            } else if constexpr (std::is_same_v<T, CancelOrderPayload>) {
                return value.parent_order_id != 0U &&
                    !text_view(value.client_order_id).empty();
            } else if constexpr (std::is_same_v<T, ReplaceOrderPayload>) {
                return value.parent_order_id != 0U &&
                    value.new_total_quantity != 0U && value.new_limit_price > 0 &&
                    !text_view(value.client_order_id).empty();
            } else if constexpr (std::is_same_v<T, MassCancelPayload>) {
                const bool instrument_required =
                    value.scope == MassCancelScope::Instrument ||
                    value.scope == MassCancelScope::InstrumentAndVenue;
                const bool venue_required = value.scope == MassCancelScope::Venue ||
                    value.scope == MassCancelScope::InstrumentAndVenue;
                return valid_scope(value.scope) &&
                    (!instrument_required || value.instrument_id != 0U) &&
                    (!venue_required || value.venue_id != 0U) &&
                    !text_view(value.client_order_id).empty();
            } else if constexpr (std::is_same_v<T, DurableAckPayload>) {
                return value.acknowledged_request_id != 0U &&
                    value.journal_record_id != 0U && value.ingress_sequence != 0U &&
                    command_message(value.acknowledged_type);
            } else if constexpr (std::is_same_v<T, RejectPayload>) {
                return value.rejected_request_id != 0U &&
                    valid_reject_code(value.code) &&
                    valid_message_type(value.rejected_type) &&
                    !text_view(value.detail).empty();
            } else if constexpr (std::is_same_v<T, ExecutionPayload>) {
                return value.originating_request_id != 0U &&
                    value.parent_order_id != 0U && value.child_order_id != 0U &&
                    value.venue_id != 0U && valid_execution_status(value.status) &&
                    valid_side(value.side) &&
                    value.cumulative_quantity + value.leaves_quantity >=
                        value.cumulative_quantity &&
                    !text_view(value.execution_id).empty();
            } else if constexpr (std::is_same_v<T, StatusPayload>) {
                return valid_appliance_status(value.status);
            } else {
                return true;
            }
        },
        message.payload);
}

[[nodiscard]] SessionValidationResult rejected(
    const SessionError error,
    const std::uint64_t expected) noexcept {
    return SessionValidationResult{
        SessionDisposition::Reject, error, expected};
}

}  // namespace

std::uint32_t crc32c(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

std::uint32_t frame_crc32c(
    const std::span<const std::byte> frame) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0U; index < frame.size(); ++index) {
        const auto value = index >= crc_offset && index < crc_offset + 4U ?
            std::uint8_t{0U} : std::to_integer<std::uint8_t>(frame[index]);
        crc ^= value;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

std::size_t payload_size(const MessageType type) noexcept {
    switch (type) {
        case MessageType::Logon: return 56U;
        case MessageType::Heartbeat: return 16U;
        case MessageType::NewOrder: return 64U;
        case MessageType::CancelOrder: return 40U;
        case MessageType::ReplaceOrder: return 56U;
        case MessageType::MassCancel: return 44U;
        case MessageType::DurableAck: return 28U;
        case MessageType::Reject: return 64U;
        case MessageType::Execution: return 88U;
        case MessageType::Status: return 76U;
    }
    return 0U;
}

MessageType message_type_of(const Payload& payload) noexcept {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, LogonPayload>) {
                return MessageType::Logon;
            } else if constexpr (std::is_same_v<T, HeartbeatPayload>) {
                return MessageType::Heartbeat;
            } else if constexpr (std::is_same_v<T, NewOrderPayload>) {
                return MessageType::NewOrder;
            } else if constexpr (std::is_same_v<T, CancelOrderPayload>) {
                return MessageType::CancelOrder;
            } else if constexpr (std::is_same_v<T, ReplaceOrderPayload>) {
                return MessageType::ReplaceOrder;
            } else if constexpr (std::is_same_v<T, MassCancelPayload>) {
                return MessageType::MassCancel;
            } else if constexpr (std::is_same_v<T, DurableAckPayload>) {
                return MessageType::DurableAck;
            } else if constexpr (std::is_same_v<T, RejectPayload>) {
                return MessageType::Reject;
            } else if constexpr (std::is_same_v<T, ExecutionPayload>) {
                return MessageType::Execution;
            } else {
                return MessageType::Status;
            }
        },
        payload);
}

std::uint32_t request_fingerprint(
    const MessageType type,
    const std::span<const std::byte> canonical_payload) noexcept {
    if (canonical_payload.size() != payload_size(type) ||
        canonical_payload.size() > maximum_payload_size) {
        return 0U;
    }
    std::array<std::byte, 2U + maximum_payload_size> bytes{};
    bytes[0] = static_cast<std::byte>(static_cast<std::uint16_t>(type) >> 8U);
    bytes[1] = static_cast<std::byte>(static_cast<std::uint16_t>(type));
    std::copy(
        canonical_payload.begin(),
        canonical_payload.end(),
        bytes.begin() + 2U);
    return crc32c(
        std::span<const std::byte>{bytes}.first(2U + canonical_payload.size()));
}

std::uint32_t request_fingerprint(const Message& message) noexcept {
    if (message.header.message_type != message_type_of(message.payload)) {
        return 0U;
    }
    std::array<std::byte, maximum_payload_size> payload{};
    Writer writer{payload};
    encode_payload(writer, message.payload);
    if (!writer.ok() ||
        writer.written() != payload_size(message.header.message_type)) {
        return 0U;
    }
    return request_fingerprint(
        message.header.message_type,
        std::span<const std::byte>{payload}.first(writer.written()));
}

EncodeToResult encode_to(
    const Message& message,
    const std::span<std::byte> destination) noexcept {
    if (!valid_message_type(message.header.message_type) ||
        message.header.message_type != message_type_of(message.payload)) {
        return EncodeToResult{ProtocolError::UnknownMessageType, 0U};
    }
    if ((message.header.flags & ~known_flags) != 0U) {
        return EncodeToResult{ProtocolError::InvalidFlags, 0U};
    }
    const auto body_size = payload_size(message.header.message_type);
    const auto total_size = static_cast<std::uint32_t>(header_size + body_size);
    if (destination.size() < total_size) {
        return EncodeToResult{ProtocolError::FrameTooShort, 0U};
    }
    const auto output = destination.first(total_size);
    Writer writer{output};
    writer.u32(magic);
    writer.u16(version);
    writer.u16(header_size);
    writer.u16(static_cast<std::uint16_t>(message.header.message_type));
    writer.u16(message.header.flags);
    writer.u32(total_size);
    writer.u64(message.header.session_id);
    writer.u64(message.header.sequence);
    writer.u64(message.header.request_id);
    writer.u64(message.header.send_time_ns);
    writer.u16(message.header.normal_credit_grant);
    writer.u16(message.header.cancel_credit_grant);
    writer.u16(message.header.normal_credit_consumed);
    writer.u16(message.header.cancel_credit_consumed);
    writer.u32(0U);
    writer.u32(0U);
    encode_payload(writer, message.payload);
    if (!writer.ok() || writer.written() != total_size) {
        return EncodeToResult{ProtocolError::PayloadSizeMismatch, 0U};
    }
    const auto checksum = frame_crc32c(output);
    output[crc_offset] = static_cast<std::byte>(checksum >> 24U);
    output[crc_offset + 1U] = static_cast<std::byte>(checksum >> 16U);
    output[crc_offset + 2U] = static_cast<std::byte>(checksum >> 8U);
    output[crc_offset + 3U] = static_cast<std::byte>(checksum);
    return EncodeToResult{ProtocolError::None, total_size};
}

EncodeResult encode(const Message& message) {
    if (!valid_message_type(message.header.message_type)) {
        return EncodeResult{ProtocolError::UnknownMessageType, {}};
    }
    std::vector<std::byte> output(
        header_size + payload_size(message.header.message_type));
    const auto encoded = encode_to(message, output);
    if (!encoded) {
        return EncodeResult{encoded.error, {}};
    }
    output.resize(encoded.bytes_written);
    return EncodeResult{ProtocolError::None, std::move(output)};
}

DecodeResult decode(const std::span<const std::byte> frame) noexcept {
    if (frame.size() < header_size) {
        return DecodeResult{ProtocolError::FrameTooShort, std::nullopt};
    }
    const std::uint32_t stored_checksum =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[60U])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[61U])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[62U])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[63U]));
    if (stored_checksum != frame_crc32c(frame)) {
        return DecodeResult{ProtocolError::ChecksumMismatch, std::nullopt};
    }

    Reader reader{frame.first(header_size)};
    if (reader.u32() != magic) {
        return DecodeResult{ProtocolError::BadMagic, std::nullopt};
    }
    if (reader.u16() != version) {
        return DecodeResult{ProtocolError::UnsupportedVersion, std::nullopt};
    }
    if (reader.u16() != header_size) {
        return DecodeResult{ProtocolError::InvalidHeaderSize, std::nullopt};
    }
    Message message;
    message.header.message_type = static_cast<MessageType>(reader.u16());
    if (!valid_message_type(message.header.message_type)) {
        return DecodeResult{ProtocolError::UnknownMessageType, std::nullopt};
    }
    message.header.flags = reader.u16();
    if ((message.header.flags & ~known_flags) != 0U) {
        return DecodeResult{ProtocolError::InvalidFlags, std::nullopt};
    }
    message.header.message_size = reader.u32();
    if (message.header.message_size != frame.size()) {
        return DecodeResult{ProtocolError::InvalidFrameSize, std::nullopt};
    }
    if (frame.size() != header_size + payload_size(message.header.message_type)) {
        return DecodeResult{ProtocolError::PayloadSizeMismatch, std::nullopt};
    }
    message.header.session_id = reader.u64();
    message.header.sequence = reader.u64();
    message.header.request_id = reader.u64();
    message.header.send_time_ns = reader.u64();
    message.header.normal_credit_grant = reader.u16();
    message.header.cancel_credit_grant = reader.u16();
    message.header.normal_credit_consumed = reader.u16();
    message.header.cancel_credit_consumed = reader.u16();
    if (reader.u32() != 0U) {
        return DecodeResult{ProtocolError::ReservedFieldNonzero, std::nullopt};
    }
    message.header.crc32c = reader.u32();
    if (!reader.ok() || reader.remaining() != 0U) {
        return DecodeResult{ProtocolError::InvalidHeaderSize, std::nullopt};
    }
    Reader payload_reader{frame.subspan(header_size)};
    const auto payload_error = decode_payload(
        message.header.message_type, payload_reader, message.payload);
    if (payload_error != ProtocolError::None) {
        return DecodeResult{payload_error, std::nullopt};
    }
    return DecodeResult{ProtocolError::None, std::move(message)};
}

SessionValidator::SessionValidator(SessionPolicy policy)
    : policy_(std::move(policy)) {
    if (policy_.session_id == 0U || policy_.maximum_normal_credits == 0U ||
        policy_.maximum_cancel_credits == 0U ||
        policy_.maximum_normal_credits > std::numeric_limits<std::uint16_t>::max() ||
        policy_.maximum_cancel_credits > std::numeric_limits<std::uint16_t>::max() ||
        policy_.minimum_heartbeat_interval_ms == 0U ||
        policy_.minimum_heartbeat_interval_ms >
            policy_.maximum_heartbeat_interval_ms ||
        policy_.request_history_capacity == 0U) {
        state_ = SessionState::Failed;
        return;
    }
    inbound_requests_.reserve(policy_.request_history_capacity);
    outbound_requests_.reserve(policy_.request_history_capacity);
}

SessionValidationResult SessionValidator::validate_inbound(
    const Message& message,
    const std::uint64_t monotonic_receive_time_ns) noexcept {
    return validate(message, true, monotonic_receive_time_ns);
}

SessionValidationResult SessionValidator::validate_outbound(
    const Message& message) noexcept {
    return validate(message, false, 0U);
}

SessionValidationResult SessionValidator::validate(
    const Message& message,
    const bool inbound,
    const std::uint64_t monotonic_receive_time_ns) noexcept {
    const auto expected_sequence = inbound ?
        next_inbound_sequence_ : next_outbound_sequence_;
    const auto consume_sequence = [this, inbound,
                                   monotonic_receive_time_ns]() noexcept {
        if (inbound) {
            ++next_inbound_sequence_;
            last_inbound_time_ns_ = monotonic_receive_time_ns;
        } else {
            ++next_outbound_sequence_;
        }
    };
    if (state_ == SessionState::Failed) {
        return rejected(SessionError::FailedSession, expected_sequence);
    }
    if (message.header.message_type != message_type_of(message.payload) ||
        !valid_payload_semantics(message, policy_)) {
        state_ = SessionState::Failed;
        return rejected(SessionError::InvalidSemantics, expected_sequence);
    }
    if (message.header.session_id != policy_.session_id) {
        state_ = SessionState::Failed;
        return rejected(SessionError::SessionIdMismatch, expected_sequence);
    }
    if (message.header.sequence != expected_sequence) {
        state_ = SessionState::Failed;
        return rejected(SessionError::SequenceMismatch, expected_sequence);
    }
    if (requires_request_id(message.header.message_type) &&
        message.header.request_id == 0U) {
        state_ = SessionState::Failed;
        return rejected(SessionError::RequestIdRequired, expected_sequence);
    }

    const bool remote_is_client =
        (policy_.local_role == EndpointRole::Server && inbound) ||
        (policy_.local_role == EndpointRole::Client && !inbound);
    if (client_message(message) != remote_is_client) {
        state_ = SessionState::Failed;
        return rejected(SessionError::UnexpectedDirection, expected_sequence);
    }

    const bool is_logon = message.header.message_type == MessageType::Logon;
    if (state_ == SessionState::AwaitingLogon) {
        const bool permitted = is_logon &&
            ((policy_.local_role == EndpointRole::Client && !inbound) ||
             (policy_.local_role == EndpointRole::Server && inbound));
        if (!permitted) {
            state_ = SessionState::Failed;
            return rejected(SessionError::LogonRequired, expected_sequence);
        }
    } else if (state_ == SessionState::AwaitingLogonResponse) {
        const bool permitted = is_logon &&
            ((policy_.local_role == EndpointRole::Client && inbound) ||
             (policy_.local_role == EndpointRole::Server && !inbound));
        if (!permitted) {
            state_ = SessionState::Failed;
            return rejected(SessionError::UnexpectedLogon, expected_sequence);
        }
    } else if (is_logon) {
        state_ = SessionState::Failed;
        return rejected(SessionError::UnexpectedLogon, expected_sequence);
    }

    const auto credit = credit_class(message.header.message_type);
    const std::uint16_t expected_normal_consumed =
        credit == CreditClass::Normal ? 1U : 0U;
    const std::uint16_t expected_cancel_consumed =
        credit == CreditClass::Cancel ? 1U : 0U;
    if (message.header.normal_credit_consumed != expected_normal_consumed ||
        message.header.cancel_credit_consumed != expected_cancel_consumed ||
        (remote_is_client &&
         (message.header.normal_credit_grant != 0U ||
          message.header.cancel_credit_grant != 0U))) {
        state_ = SessionState::Failed;
        return rejected(SessionError::InvalidCreditDeclaration, expected_sequence);
    }

    CreditBalance& consumed_balance = inbound ? peer_credits_ : local_credits_;
    CreditBalance& granted_balance = inbound ? local_credits_ : peer_credits_;
    if (expected_normal_consumed != 0U && consumed_balance.normal == 0U) {
        // A fully authenticated, correctly sequenced command is consumed even
        // when backpressure rejects it. This keeps the validator's sequence in
        // lockstep with SessionAuthenticator and prevents retrying an already
        // authenticated envelope under the same sequence number.
        consume_sequence();
        return rejected(SessionError::NormalCreditsExhausted, expected_sequence);
    }
    if (expected_cancel_consumed != 0U && consumed_balance.cancel == 0U) {
        consume_sequence();
        return rejected(SessionError::CancelCreditsExhausted, expected_sequence);
    }
    if (message.header.normal_credit_grant >
            policy_.maximum_normal_credits - granted_balance.normal ||
        message.header.cancel_credit_grant >
            policy_.maximum_cancel_credits - granted_balance.cancel) {
        state_ = SessionState::Failed;
        return rejected(SessionError::CreditLimitExceeded, expected_sequence);
    }

    SessionDisposition disposition = SessionDisposition::Accept;
    auto* history = inbound ? &inbound_requests_ : &outbound_requests_;
    auto* cursor = inbound ? &inbound_request_cursor_ : &outbound_request_cursor_;
    RequestEntry new_entry;
    bool append_request = false;
    if (command_message(message.header.message_type)) {
        new_entry = RequestEntry{
            message.header.request_id,
            message.header.message_type,
            request_fingerprint(message)};
        const auto found = std::find_if(
            history->begin(),
            history->end(),
            [&new_entry](const RequestEntry& item) {
                return item.request_id == new_entry.request_id;
            });
        if (found != history->end()) {
            if (found->type != new_entry.type ||
                found->fingerprint != new_entry.fingerprint) {
                state_ = SessionState::Failed;
                return rejected(
                    SessionError::DuplicateRequestConflict, expected_sequence);
            }
            disposition = SessionDisposition::IdempotentReplay;
        } else {
            append_request = true;
        }
    }

    consumed_balance.normal -= expected_normal_consumed;
    consumed_balance.cancel -= expected_cancel_consumed;
    granted_balance.normal += message.header.normal_credit_grant;
    granted_balance.cancel += message.header.cancel_credit_grant;
    if (append_request) {
        if (history->size() < policy_.request_history_capacity) {
            history->push_back(new_entry);
        } else {
            (*history)[*cursor] = new_entry;
            *cursor = (*cursor + 1U) % policy_.request_history_capacity;
        }
    }
    if (is_logon) {
        const auto& logon = std::get<LogonPayload>(message.payload);
        heartbeat_interval_ms_ = logon.heartbeat_interval_ms;
        state_ = state_ == SessionState::AwaitingLogon ?
            SessionState::AwaitingLogonResponse : SessionState::Established;
    }
    consume_sequence();
    return SessionValidationResult{
        disposition, SessionError::None, expected_sequence};
}

bool SessionValidator::heartbeat_expired(
    const std::uint64_t monotonic_now_ns) const noexcept {
    if (state_ != SessionState::Established || heartbeat_interval_ms_ == 0U ||
        last_inbound_time_ns_ == 0U || monotonic_now_ns < last_inbound_time_ns_) {
        return false;
    }
    constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000U;
    const auto interval = static_cast<std::uint64_t>(heartbeat_interval_ms_);
    if (interval > std::numeric_limits<std::uint64_t>::max() /
            (3U * nanoseconds_per_millisecond)) {
        return false;
    }
    const auto timeout = interval * 3U * nanoseconds_per_millisecond;
    return monotonic_now_ns - last_inbound_time_ns_ > timeout;
}

}  // namespace sorx::protocol::sobp_v1
