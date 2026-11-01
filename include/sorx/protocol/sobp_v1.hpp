#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace sorx::protocol::sobp_v1 {

inline constexpr std::uint32_t magic = 0x534F5258U;  // "SORX"
inline constexpr std::uint16_t version = 1U;
inline constexpr std::uint16_t header_size = 64U;
inline constexpr std::size_t crc_offset = 60U;
inline constexpr std::size_t maximum_payload_size = 88U;
inline constexpr std::size_t maximum_frame_size =
    header_size + maximum_payload_size;

enum class MessageType : std::uint16_t {
    Logon = 1U,
    Heartbeat = 2U,
    NewOrder = 10U,
    CancelOrder = 11U,
    ReplaceOrder = 12U,
    MassCancel = 13U,
    DurableAck = 20U,
    Reject = 21U,
    Execution = 22U,
    Status = 23U,
};

enum class HeaderFlag : std::uint16_t {
    Response = 1U << 0U,
    Retransmission = 1U << 1U,
    Administrative = 1U << 2U,
};

[[nodiscard]] constexpr std::uint16_t flag(const HeaderFlag value) noexcept {
    return static_cast<std::uint16_t>(value);
}

enum class Side : std::uint8_t {
    Buy = 1U,
    Sell = 2U,
};

enum class RoutingIntent : std::uint8_t {
    AggressiveIoc = 1U,
    PassiveGtc = 2U,
};

enum class MassCancelScope : std::uint8_t {
    Session = 1U,
    Instrument = 2U,
    Venue = 3U,
    InstrumentAndVenue = 4U,
};

enum class ExecutionStatus : std::uint8_t {
    Pending = 1U,
    Working = 2U,
    PartiallyFilled = 3U,
    Filled = 4U,
    Canceled = 5U,
    Rejected = 6U,
    Ambiguous = 7U,
};

enum class ApplianceStatus : std::uint16_t {
    Bootstrapping = 1U,
    RecoveryOnly = 2U,
    Ready = 3U,
    Degraded = 4U,
    Maintenance = 5U,
    Failed = 6U,
};

enum class RejectCode : std::uint16_t {
    Malformed = 1U,
    UnsupportedVersion = 2U,
    SessionMismatch = 3U,
    SequenceMismatch = 4U,
    DuplicateRequestConflict = 5U,
    CreditsExhausted = 6U,
    InvalidOrder = 7U,
    RiskRejected = 8U,
    RoutingUnavailable = 9U,
    InternalFailure = 10U,
};

template <std::size_t Size>
using FixedText = std::array<char, Size>;

template <std::size_t Size>
[[nodiscard]] bool assign_text(
    FixedText<Size>& destination,
    const std::string_view value) noexcept {
    if (value.empty() || value.size() > Size ||
        std::find(value.begin(), value.end(), '\0') != value.end()) {
        return false;
    }
    destination.fill('\0');
    std::copy(value.begin(), value.end(), destination.begin());
    return true;
}

template <std::size_t Size>
[[nodiscard]] std::string_view text_view(
    const FixedText<Size>& value) noexcept {
    const auto end = std::find(value.begin(), value.end(), '\0');
    return std::string_view{
        value.data(), static_cast<std::size_t>(end - value.begin())};
}

struct FrameHeader final {
    MessageType message_type{MessageType::Heartbeat};
    std::uint16_t flags{};
    std::uint32_t message_size{};
    std::uint64_t session_id{};
    std::uint64_t sequence{};
    std::uint64_t request_id{};
    std::uint64_t send_time_ns{};
    std::uint16_t normal_credit_grant{};
    std::uint16_t cancel_credit_grant{};
    std::uint16_t normal_credit_consumed{};
    std::uint16_t cancel_credit_consumed{};
    std::uint32_t crc32c{};

    constexpr auto operator<=>(const FrameHeader&) const noexcept = default;
};

struct LogonPayload final {
    std::uint16_t minimum_version{version};
    std::uint16_t maximum_version{version};
    std::uint32_t heartbeat_interval_ms{1'000U};
    std::uint64_t client_instance_id{};
    std::uint64_t resume_from_sequence{};
    FixedText<32U> account_context{};

    constexpr auto operator<=>(const LogonPayload&) const noexcept = default;
};

struct HeartbeatPayload final {
    std::uint64_t last_received_sequence{};
    std::uint64_t monotonic_time_ns{};

    constexpr auto operator<=>(const HeartbeatPayload&) const noexcept = default;
};

struct NewOrderPayload final {
    std::uint64_t parent_order_id{};
    std::uint32_t instrument_id{};
    Side side{Side::Buy};
    RoutingIntent intent{RoutingIntent::AggressiveIoc};
    std::uint64_t quantity{};
    std::int64_t limit_price{};
    FixedText<32U> client_order_id{};

    constexpr auto operator<=>(const NewOrderPayload&) const noexcept = default;
};

struct CancelOrderPayload final {
    std::uint64_t parent_order_id{};
    FixedText<32U> client_order_id{};

    constexpr auto operator<=>(const CancelOrderPayload&) const noexcept = default;
};

struct ReplaceOrderPayload final {
    std::uint64_t parent_order_id{};
    std::uint64_t new_total_quantity{};
    std::int64_t new_limit_price{};
    FixedText<32U> client_order_id{};

    constexpr auto operator<=>(const ReplaceOrderPayload&) const noexcept = default;
};

struct MassCancelPayload final {
    MassCancelScope scope{MassCancelScope::Session};
    std::uint8_t side_filter{};  // 0=both, otherwise a Side value.
    std::uint32_t instrument_id{};
    std::uint32_t venue_id{};
    FixedText<32U> client_order_id{};

    constexpr auto operator<=>(const MassCancelPayload&) const noexcept = default;
};

struct DurableAckPayload final {
    std::uint64_t acknowledged_request_id{};
    std::uint64_t journal_record_id{};
    std::uint64_t ingress_sequence{};
    MessageType acknowledged_type{MessageType::NewOrder};

    constexpr auto operator<=>(const DurableAckPayload&) const noexcept = default;
};

struct RejectPayload final {
    std::uint64_t rejected_request_id{};
    RejectCode code{RejectCode::Malformed};
    MessageType rejected_type{MessageType::NewOrder};
    FixedText<52U> detail{};

    constexpr auto operator<=>(const RejectPayload&) const noexcept = default;
};

struct ExecutionPayload final {
    std::uint64_t originating_request_id{};
    std::uint64_t parent_order_id{};
    std::uint64_t child_order_id{};
    std::uint32_t venue_id{};
    ExecutionStatus status{ExecutionStatus::Pending};
    Side side{Side::Buy};
    std::uint64_t last_quantity{};
    std::int64_t last_price{};
    std::uint64_t cumulative_quantity{};
    std::uint64_t leaves_quantity{};
    FixedText<24U> execution_id{};

    constexpr auto operator<=>(const ExecutionPayload&) const noexcept = default;
};

struct StatusPayload final {
    ApplianceStatus status{ApplianceStatus::Bootstrapping};
    bool routing_enabled{};
    bool kill_switch_active{};
    std::uint64_t last_ingress_sequence{};
    std::uint32_t active_parents{};
    std::uint32_t active_children{};
    std::uint32_t normal_credits_available{};
    std::uint32_t cancel_credits_available{};
    FixedText<48U> detail{};

    constexpr auto operator<=>(const StatusPayload&) const noexcept = default;
};

using Payload = std::variant<
    LogonPayload,
    HeartbeatPayload,
    NewOrderPayload,
    CancelOrderPayload,
    ReplaceOrderPayload,
    MassCancelPayload,
    DurableAckPayload,
    RejectPayload,
    ExecutionPayload,
    StatusPayload>;

struct Message final {
    FrameHeader header;
    Payload payload;

    constexpr auto operator<=>(const Message&) const noexcept = default;
};

enum class ProtocolError : std::uint8_t {
    None,
    FrameTooShort,
    BadMagic,
    UnsupportedVersion,
    InvalidHeaderSize,
    UnknownMessageType,
    InvalidFlags,
    InvalidFrameSize,
    PayloadSizeMismatch,
    ChecksumMismatch,
    ReservedFieldNonzero,
    InvalidValue,
    NonCanonicalText,
};

struct EncodeResult final {
    ProtocolError error{ProtocolError::None};
    std::vector<std::byte> bytes;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ProtocolError::None;
    }
};

struct EncodeToResult final {
    ProtocolError error{ProtocolError::None};
    std::size_t bytes_written{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeResult final {
    ProtocolError error{ProtocolError::None};
    std::optional<Message> message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ProtocolError::None && message.has_value();
    }
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;
// Calculates a frame CRC while logically treating the four CRC field bytes as
// zero. This permits validation directly from an immutable receive buffer.
[[nodiscard]] std::uint32_t frame_crc32c(
    std::span<const std::byte> frame) noexcept;
[[nodiscard]] std::size_t payload_size(MessageType type) noexcept;
[[nodiscard]] MessageType message_type_of(const Payload& payload) noexcept;
// Stable idempotency key over message type plus canonical payload. Both
// overloads are allocation-free; malformed type/payload pairs return zero.
[[nodiscard]] std::uint32_t request_fingerprint(
    MessageType type,
    std::span<const std::byte> canonical_payload) noexcept;
[[nodiscard]] std::uint32_t request_fingerprint(
    const Message& message) noexcept;
// Allocation-free hot-path API. The caller supplies at least
// header_size + payload_size(message.header.message_type) bytes.
[[nodiscard]] EncodeToResult encode_to(
    const Message& message,
    std::span<std::byte> destination) noexcept;
// Convenience wrapper for tools/tests. Gateways should use encode_to().
[[nodiscard]] EncodeResult encode(const Message& message);
[[nodiscard]] DecodeResult decode(
    std::span<const std::byte> frame) noexcept;

enum class EndpointRole : std::uint8_t {
    Client,
    Server,
};

enum class SessionState : std::uint8_t {
    AwaitingLogon,
    AwaitingLogonResponse,
    Established,
    Failed,
};

enum class SessionDisposition : std::uint8_t {
    Accept,
    IdempotentReplay,
    Reject,
};

enum class SessionError : std::uint8_t {
    None,
    FailedSession,
    SessionIdMismatch,
    SequenceMismatch,
    UnexpectedDirection,
    UnexpectedLogon,
    LogonRequired,
    InvalidSemantics,
    InvalidCreditDeclaration,
    NormalCreditsExhausted,
    CancelCreditsExhausted,
    CreditLimitExceeded,
    RequestIdRequired,
    DuplicateRequestConflict,
};

struct SessionValidationResult final {
    SessionDisposition disposition{SessionDisposition::Reject};
    SessionError error{SessionError::None};
    std::uint64_t expected_sequence{};

    [[nodiscard]] bool accepted() const noexcept {
        return disposition != SessionDisposition::Reject;
    }
};

struct CreditBalance final {
    std::uint32_t normal{};
    std::uint32_t cancel{};

    constexpr auto operator<=>(const CreditBalance&) const noexcept = default;
};

struct SessionPolicy final {
    EndpointRole local_role{EndpointRole::Server};
    std::uint64_t session_id{};
    std::uint32_t maximum_normal_credits{65'535U};
    std::uint32_t maximum_cancel_credits{65'535U};
    std::uint32_t minimum_heartbeat_interval_ms{100U};
    std::uint32_t maximum_heartbeat_interval_ms{60'000U};
    std::size_t request_history_capacity{4'096U};
};

// This cache identifies same-session retries. Durable idempotency still belongs
// in the order gateway/journal and must outlive this object.
class SessionValidator final {
public:
    explicit SessionValidator(SessionPolicy policy);

    [[nodiscard]] SessionValidationResult validate_inbound(
        const Message& message,
        std::uint64_t monotonic_receive_time_ns) noexcept;
    [[nodiscard]] SessionValidationResult validate_outbound(
        const Message& message) noexcept;

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t next_inbound_sequence() const noexcept {
        return next_inbound_sequence_;
    }
    [[nodiscard]] std::uint64_t next_outbound_sequence() const noexcept {
        return next_outbound_sequence_;
    }
    [[nodiscard]] CreditBalance local_credits() const noexcept {
        return local_credits_;
    }
    [[nodiscard]] CreditBalance peer_credits() const noexcept {
        return peer_credits_;
    }
    [[nodiscard]] std::uint32_t heartbeat_interval_ms() const noexcept {
        return heartbeat_interval_ms_;
    }
    [[nodiscard]] bool heartbeat_expired(
        std::uint64_t monotonic_now_ns) const noexcept;
    [[nodiscard]] std::size_t inbound_request_storage_capacity() const noexcept {
        return inbound_requests_.capacity();
    }
    [[nodiscard]] std::size_t outbound_request_storage_capacity() const noexcept {
        return outbound_requests_.capacity();
    }

private:
    struct RequestEntry final {
        std::uint64_t request_id{};
        MessageType type{MessageType::Heartbeat};
        std::uint32_t fingerprint{};
    };

    [[nodiscard]] SessionValidationResult validate(
        const Message& message,
        bool inbound,
        std::uint64_t monotonic_receive_time_ns) noexcept;

    SessionPolicy policy_;
    SessionState state_{SessionState::AwaitingLogon};
    std::uint64_t next_inbound_sequence_{1U};
    std::uint64_t next_outbound_sequence_{1U};
    CreditBalance local_credits_;
    CreditBalance peer_credits_;
    std::uint32_t heartbeat_interval_ms_{};
    std::uint64_t last_inbound_time_ns_{};
    std::vector<RequestEntry> inbound_requests_;
    std::vector<RequestEntry> outbound_requests_;
    std::size_t inbound_request_cursor_{};
    std::size_t outbound_request_cursor_{};
};

}  // namespace sorx::protocol::sobp_v1
