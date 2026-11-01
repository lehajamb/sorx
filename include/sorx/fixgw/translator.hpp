#pragma once

#include "sorx/protocol/sobp_v1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sorx::fixgw {

inline constexpr char kFixDelimiter = '\x01';
inline constexpr std::size_t kMaximumFixFrameBytes = 4'096U;
inline constexpr std::size_t kMaximumFixFields = 64U;
inline constexpr std::size_t kMaximumInstrumentMappings = 256U;
inline constexpr std::size_t kMaximumFixResponseFields = 32U;
inline constexpr std::size_t kMaximumFixFieldValueBytes = 64U;

template <std::size_t Size>
using FixedText = std::array<char, Size>;

template <std::size_t Size>
[[nodiscard]] bool assign_fixed_text(
    FixedText<Size>& destination,
    std::string_view value) noexcept;

template <std::size_t Size>
[[nodiscard]] std::string_view fixed_text_view(
    const FixedText<Size>& value) noexcept;

enum class CommandKind : std::uint8_t {
    NewOrder,
    CancelOrder,
    ReplaceOrder,
    MassCancel,
};

enum class TimeInForce : std::uint8_t {
    NotApplicable,
    Day,
    GoodTillCancel,
    ImmediateOrCancel,
};

struct InstrumentMapping final {
    FixedText<16U> symbol{};
    std::uint32_t instrument_id{};

    constexpr auto operator<=>(const InstrumentMapping&) const noexcept = default;
};

struct TranslatorConfig final {
    std::uint64_t session_id{};
    std::uint64_t session_epoch{};
    std::uint8_t price_fraction_digits{4U};
    FixedText<16U> expected_sender_comp_id{};
    FixedText<16U> expected_target_comp_id{};
    FixedText<24U> expected_account{};
    FixedText<16U> response_sender_comp_id{};
    FixedText<16U> response_target_comp_id{};
};

// The descriptor is the fixed ingress seam between sorx-fixgw and sorxd. It
// deliberately preserves identifiers and FIX-only TIF detail even where the
// v1 binary protocol has a coarser routing-intent representation.
struct CommandDescriptor final {
    CommandKind kind{CommandKind::NewOrder};
    std::uint64_t session_id{};
    std::uint64_t session_epoch{};
    std::uint64_t client_sequence{};
    std::uint64_t request_id{};
    std::uint64_t parent_order_id{};
    std::uint32_t instrument_id{};
    protocol::sobp_v1::Side side{protocol::sobp_v1::Side::Buy};
    std::uint8_t side_filter{};
    protocol::sobp_v1::MassCancelScope mass_cancel_scope{
        protocol::sobp_v1::MassCancelScope::Session};
    TimeInForce time_in_force{TimeInForce::NotApplicable};
    bool post_only{};
    bool possible_duplicate{};
    std::uint64_t quantity{};
    std::int64_t limit_price{};
    FixedText<32U> client_order_id{};
    FixedText<32U> original_client_order_id{};
    FixedText<16U> symbol{};

    constexpr auto operator<=>(const CommandDescriptor&) const noexcept = default;
};

enum class TranslateError : std::uint8_t {
    None,
    InvalidConfiguration,
    FrameTooShort,
    FrameTooLarge,
    TooManyFields,
    MissingDelimiter,
    InvalidTag,
    EmptyValue,
    DuplicateTag,
    UnexpectedFieldOrder,
    InvalidBeginString,
    InvalidBodyLength,
    InvalidChecksum,
    UnsupportedMessageType,
    MissingRequiredTag,
    UnsupportedTag,
    SessionIdentityMismatch,
    InvalidSessionField,
    InvalidTimestamp,
    InvalidNumeric,
    NumericOverflow,
    IdentifierTooLong,
    UnknownInstrument,
    UnsupportedHandlingInstruction,
    UnsupportedSide,
    UnsupportedOrderType,
    UnsupportedTimeInForce,
    UnsupportedExecutionInstruction,
    UnsupportedMassCancelScope,
    InconsistentReplayFields,
};

struct TranslateResult final {
    TranslateError error{TranslateError::None};
    std::uint16_t offending_tag{};
    CommandDescriptor command{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == TranslateError::None;
    }
};

enum class SobpConversionError : std::uint8_t {
    None,
    InvalidDescriptor,
    IdentifierTooLong,
    UnsupportedSemantics,
};

struct SobpConversionResult final {
    SobpConversionError error{SobpConversionError::None};
    protocol::sobp_v1::Message message{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SobpConversionError::None;
    }
};

struct FixedFixField final {
    std::uint16_t tag{};
    std::uint8_t value_size{};
    FixedText<kMaximumFixFieldValueBytes> value{};

    [[nodiscard]] std::string_view value_view() const noexcept {
        return {value.data(), value_size};
    }
};

// Canonical ordered representation. The raw encoder always recalculates tags
// 9 and 10; callers cannot inject either field into this structure.
struct FixedFixMessage final {
    std::array<FixedFixField, kMaximumFixResponseFields> fields{};
    std::uint8_t field_count{};

    [[nodiscard]] std::span<const FixedFixField> view() const noexcept {
        return {fields.data(), field_count};
    }
};

struct ResponseContext final {
    FixedText<32U> client_order_id{};
    FixedText<21U> sending_time{};
    std::uint64_t parent_order_id{};
    std::uint64_t order_quantity{};
    protocol::sobp_v1::Side side{protocol::sobp_v1::Side::Buy};
};

enum class ResponseError : std::uint8_t {
    None,
    InvalidConfiguration,
    UnsupportedPayload,
    InvalidContext,
    FieldCapacityExceeded,
    ValueCapacityExceeded,
    BufferTooSmall,
};

struct ResponseResult final {
    ResponseError error{ResponseError::None};
    FixedFixMessage message{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ResponseError::None;
    }
};

struct EncodeFixResult final {
    ResponseError error{ResponseError::None};
    std::size_t bytes_written{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ResponseError::None;
    }
};

class Translator final {
public:
    Translator(
        const TranslatorConfig& config,
        std::span<const InstrumentMapping> instruments) noexcept;

    [[nodiscard]] bool configured() const noexcept { return configured_; }

    // Parses one complete FIX 4.4 frame including the checksum field. The
    // caller retains buffer ownership; no references escape this call.
    [[nodiscard]] TranslateResult translate(
        std::span<const char> frame) const noexcept;

    [[nodiscard]] SobpConversionResult to_sobp(
        const CommandDescriptor& command,
        std::uint64_t send_time_ns) const noexcept;

    // Builds deterministic northbound representations for existing SOBP
    // DurableAck, Execution, Reject and Status payloads. Network/session
    // sequencing stays outside this protocol core.
    [[nodiscard]] ResponseResult translate_response(
        const protocol::sobp_v1::Message& response,
        const ResponseContext& context) const noexcept;

    [[nodiscard]] static EncodeFixResult encode_response(
        const FixedFixMessage& message,
        std::span<char> destination) noexcept;

    [[nodiscard]] std::uint64_t request_id(
        CommandKind kind,
        std::string_view client_order_id) const noexcept;
    // Parent identity is stable across FIX session reconnect epochs. FIX
    // ClOrdID uniqueness is therefore enforced in the durable sorxd identity
    // table for the lifetime of the configured session_id.
    [[nodiscard]] std::uint64_t parent_order_id(
        std::string_view original_client_order_id) const noexcept;

private:
    TranslatorConfig config_{};
    std::array<InstrumentMapping, kMaximumInstrumentMappings> instruments_{};
    std::uint16_t instrument_count_{};
    bool configured_{};
};

template <std::size_t Size>
bool assign_fixed_text(
    FixedText<Size>& destination,
    const std::string_view value) noexcept {
    if (value.empty() || value.size() > Size) {
        return false;
    }
    destination.fill('\0');
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '\0' || character == kFixDelimiter) {
            destination.fill('\0');
            return false;
        }
        destination[index] = character;
    }
    return true;
}

template <std::size_t Size>
std::string_view fixed_text_view(const FixedText<Size>& value) noexcept {
    std::size_t size = 0U;
    while (size < Size && value[size] != '\0') {
        ++size;
    }
    return {value.data(), size};
}

}  // namespace sorx::fixgw
