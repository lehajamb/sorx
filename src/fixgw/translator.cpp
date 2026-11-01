#include "sorx/fixgw/translator.hpp"

#include <charconv>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace sorx::fixgw {
namespace {

struct ParsedField final {
    std::uint16_t tag{};
    std::string_view value{};
    std::size_t start{};
};

struct ParsedFrame final {
    std::array<ParsedField, kMaximumFixFields> fields{};
    std::uint8_t count{};
};

struct ParseResult final {
    TranslateError error{TranslateError::None};
    std::uint16_t offending_tag{};
    ParsedFrame frame{};
};

enum class IntegerParseStatus : std::uint8_t {
    Ok,
    Invalid,
    Overflow,
};

[[nodiscard]] IntegerParseStatus parse_uint64_status(
    const std::string_view text,
    std::uint64_t& output) noexcept {
    if (text.empty()) {
        return IntegerParseStatus::Invalid;
    }
    std::uint64_t value = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return IntegerParseStatus::Invalid;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return IntegerParseStatus::Overflow;
        }
        value = value * 10U + digit;
    }
    output = value;
    return IntegerParseStatus::Ok;
}

[[nodiscard]] bool parse_uint64(
    const std::string_view text,
    std::uint64_t& output) noexcept {
    return parse_uint64_status(text, output) == IntegerParseStatus::Ok;
}

[[nodiscard]] bool parse_tag(
    const std::string_view text,
    std::uint16_t& output) noexcept {
    std::uint64_t value = 0U;
    if (!parse_uint64(text, value) || value == 0U ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    output = static_cast<std::uint16_t>(value);
    return true;
}

[[nodiscard]] const ParsedField* find_field(
    const ParsedFrame& frame,
    const std::uint16_t tag) noexcept {
    for (std::size_t index = 0U; index < frame.count; ++index) {
        if (frame.fields[index].tag == tag) {
            return &frame.fields[index];
        }
    }
    return nullptr;
}

[[nodiscard]] bool valid_timestamp(const std::string_view value) noexcept {
    // FIX UTC timestamp: YYYYMMDD-HH:MM:SS with an optional fractional suffix.
    if (value.size() < 17U || value.size() > 27U || value[8U] != '-' ||
        value[11U] != ':' || value[14U] != ':') {
        return false;
    }
    for (std::size_t index = 0U; index < 17U; ++index) {
        if (index == 8U || index == 11U || index == 14U) {
            continue;
        }
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    const auto two_digits = [&value](const std::size_t offset) noexcept {
        return static_cast<unsigned>(value[offset] - '0') * 10U +
            static_cast<unsigned>(value[offset + 1U] - '0');
    };
    const unsigned month = two_digits(4U);
    const unsigned day = two_digits(6U);
    const unsigned hour = two_digits(9U);
    const unsigned minute = two_digits(12U);
    const unsigned second = two_digits(15U);
    if (month < 1U || month > 12U || day < 1U || day > 31U || hour > 23U ||
        minute > 59U || second > 60U) {
        return false;
    }
    if (value.size() == 17U) {
        return true;
    }
    if (value[17U] != '.' || value.size() == 18U) {
        return false;
    }
    for (std::size_t index = 18U; index < value.size(); ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ParseResult parse_frame(const std::span<const char> bytes) noexcept {
    ParseResult result;
    if (bytes.size() < 32U) {
        result.error = TranslateError::FrameTooShort;
        return result;
    }
    if (bytes.size() > kMaximumFixFrameBytes) {
        result.error = TranslateError::FrameTooLarge;
        return result;
    }

    std::size_t field_start = 0U;
    while (field_start < bytes.size()) {
        if (result.frame.count == kMaximumFixFields) {
            result.error = TranslateError::TooManyFields;
            return result;
        }
        std::size_t delimiter = field_start;
        while (delimiter < bytes.size() && bytes[delimiter] != kFixDelimiter) {
            ++delimiter;
        }
        if (delimiter == bytes.size()) {
            result.error = TranslateError::MissingDelimiter;
            return result;
        }
        std::size_t equals = field_start;
        while (equals < delimiter && bytes[equals] != '=') {
            ++equals;
        }
        if (equals == field_start || equals == delimiter) {
            result.error = TranslateError::InvalidTag;
            return result;
        }
        for (std::size_t index = equals + 1U; index < delimiter; ++index) {
            if (bytes[index] == '=') {
                result.error = TranslateError::InvalidTag;
                return result;
            }
        }
        ParsedField field;
        if (!parse_tag(
                {bytes.data() + field_start, equals - field_start}, field.tag)) {
            result.error = TranslateError::InvalidTag;
            return result;
        }
        field.value = {
            bytes.data() + equals + 1U, delimiter - equals - 1U};
        field.start = field_start;
        if (field.value.empty()) {
            result.error = TranslateError::EmptyValue;
            result.offending_tag = field.tag;
            return result;
        }
        for (std::size_t index = 0U; index < result.frame.count; ++index) {
            if (result.frame.fields[index].tag == field.tag) {
                result.error = TranslateError::DuplicateTag;
                result.offending_tag = field.tag;
                return result;
            }
        }
        result.frame.fields[result.frame.count++] = field;
        field_start = delimiter + 1U;
    }

    if (result.frame.count < 4U || result.frame.fields[0U].tag != 8U ||
        result.frame.fields[1U].tag != 9U || result.frame.fields[2U].tag != 35U ||
        result.frame.fields[result.frame.count - 1U].tag != 10U) {
        result.error = TranslateError::UnexpectedFieldOrder;
        return result;
    }
    if (result.frame.fields[0U].value != "FIX.4.4") {
        result.error = TranslateError::InvalidBeginString;
        result.offending_tag = 8U;
        return result;
    }

    std::uint64_t declared_body_length = 0U;
    if (!parse_uint64(result.frame.fields[1U].value, declared_body_length)) {
        result.error = TranslateError::InvalidBodyLength;
        result.offending_tag = 9U;
        return result;
    }
    const std::size_t body_start = result.frame.fields[2U].start;
    const std::size_t checksum_start =
        result.frame.fields[result.frame.count - 1U].start;
    if (checksum_start < body_start ||
        declared_body_length != checksum_start - body_start) {
        result.error = TranslateError::InvalidBodyLength;
        result.offending_tag = 9U;
        return result;
    }

    const auto checksum_text = result.frame.fields[result.frame.count - 1U].value;
    if (checksum_text.size() != 3U || checksum_text[0U] < '0' ||
        checksum_text[0U] > '9' || checksum_text[1U] < '0' ||
        checksum_text[1U] > '9' || checksum_text[2U] < '0' ||
        checksum_text[2U] > '9') {
        result.error = TranslateError::InvalidChecksum;
        result.offending_tag = 10U;
        return result;
    }
    const unsigned declared_checksum =
        static_cast<unsigned>(checksum_text[0U] - '0') * 100U +
        static_cast<unsigned>(checksum_text[1U] - '0') * 10U +
        static_cast<unsigned>(checksum_text[2U] - '0');
    unsigned calculated_checksum = 0U;
    for (std::size_t index = 0U; index < checksum_start; ++index) {
        calculated_checksum =
            (calculated_checksum + static_cast<unsigned char>(bytes[index])) & 0xffU;
    }
    if (declared_checksum != calculated_checksum) {
        result.error = TranslateError::InvalidChecksum;
        result.offending_tag = 10U;
    }
    return result;
}

[[nodiscard]] bool common_tag(const std::uint16_t tag) noexcept {
    return tag == 8U || tag == 9U || tag == 10U || tag == 35U || tag == 34U ||
        tag == 49U || tag == 56U || tag == 52U || tag == 43U || tag == 122U ||
        tag == 1U;
}

[[nodiscard]] bool allowed_tag(
    const CommandKind kind,
    const std::uint16_t tag) noexcept {
    if (common_tag(tag)) {
        return true;
    }
    switch (kind) {
        case CommandKind::NewOrder:
            return tag == 11U || tag == 18U || tag == 21U || tag == 38U || tag == 40U ||
                tag == 44U || tag == 54U || tag == 55U || tag == 59U ||
                tag == 60U;
        case CommandKind::CancelOrder:
            return tag == 11U || tag == 41U || tag == 54U || tag == 55U ||
                tag == 60U;
        case CommandKind::ReplaceOrder:
            return tag == 11U || tag == 18U || tag == 21U || tag == 38U || tag == 40U ||
                tag == 41U || tag == 44U || tag == 54U || tag == 55U ||
                tag == 59U || tag == 60U;
        case CommandKind::MassCancel:
            return tag == 11U || tag == 54U || tag == 55U || tag == 60U ||
                tag == 530U;
    }
    return false;
}

[[nodiscard]] const ParsedField* require_field(
    const ParsedFrame& frame,
    const std::uint16_t tag,
    TranslateResult& result) noexcept {
    const ParsedField* const field = find_field(frame, tag);
    if (field == nullptr) {
        result.error = TranslateError::MissingRequiredTag;
        result.offending_tag = tag;
    }
    return field;
}

[[nodiscard]] bool parse_price(
    const std::string_view text,
    const std::uint8_t fraction_digits,
    std::int64_t& output,
    TranslateError& error) noexcept {
    if (text.empty()) {
        error = TranslateError::InvalidNumeric;
        return false;
    }
    std::size_t index = 0U;
    std::uint64_t integral = 0U;
    bool saw_integral_digit = false;
    while (index < text.size() && text[index] != '.') {
        const char character = text[index++];
        if (character < '0' || character > '9') {
            error = TranslateError::InvalidNumeric;
            return false;
        }
        saw_integral_digit = true;
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (integral > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            error = TranslateError::NumericOverflow;
            return false;
        }
        integral = integral * 10U + digit;
    }
    if (!saw_integral_digit) {
        error = TranslateError::InvalidNumeric;
        return false;
    }

    std::uint64_t fraction = 0U;
    std::uint8_t accepted_fraction_digits = 0U;
    if (index < text.size()) {
        ++index;
        if (index == text.size()) {
            error = TranslateError::InvalidNumeric;
            return false;
        }
        for (; index < text.size(); ++index) {
            const char character = text[index];
            if (character < '0' || character > '9') {
                error = TranslateError::InvalidNumeric;
                return false;
            }
            if (accepted_fraction_digits < fraction_digits) {
                fraction = fraction * 10U +
                    static_cast<std::uint64_t>(character - '0');
                ++accepted_fraction_digits;
            } else if (character != '0') {
                error = TranslateError::InvalidNumeric;
                return false;
            }
        }
    }

    std::uint64_t scale = 1U;
    for (std::uint8_t digit = 0U; digit < fraction_digits; ++digit) {
        scale *= 10U;
    }
    while (accepted_fraction_digits < fraction_digits) {
        fraction *= 10U;
        ++accepted_fraction_digits;
    }
    const auto maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (integral > (maximum - fraction) / scale) {
        error = TranslateError::NumericOverflow;
        return false;
    }
    output = static_cast<std::int64_t>(integral * scale + fraction);
    if (output <= 0) {
        error = TranslateError::InvalidNumeric;
        return false;
    }
    return true;
}

[[nodiscard]] std::uint64_t fnv1a_append(
    std::uint64_t hash,
    const std::string_view value) noexcept {
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::uint64_t fnv1a_append_u64(
    std::uint64_t hash,
    const std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

template <std::size_t Size>
[[nodiscard]] bool valid_config_text(const FixedText<Size>& value) noexcept {
    const auto text = fixed_text_view(value);
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        if (character == kFixDelimiter || character == '=') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool command_kind(
    const std::string_view value,
    CommandKind& output) noexcept {
    if (value == "D") {
        output = CommandKind::NewOrder;
    } else if (value == "F") {
        output = CommandKind::CancelOrder;
    } else if (value == "G") {
        output = CommandKind::ReplaceOrder;
    } else if (value == "q") {
        output = CommandKind::MassCancel;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] std::string_view sobp_message_type(
    const protocol::sobp_v1::MessageType type) noexcept {
    switch (type) {
        case protocol::sobp_v1::MessageType::NewOrder: return "D";
        case protocol::sobp_v1::MessageType::CancelOrder: return "F";
        case protocol::sobp_v1::MessageType::ReplaceOrder: return "G";
        case protocol::sobp_v1::MessageType::MassCancel: return "q";
        default: return "U";
    }
}

[[nodiscard]] bool add_field(
    FixedFixMessage& message,
    const std::uint16_t tag,
    const std::string_view value) noexcept {
    if (message.field_count == kMaximumFixResponseFields || value.empty() ||
        value.size() > kMaximumFixFieldValueBytes) {
        return false;
    }
    FixedFixField& field = message.fields[message.field_count++];
    field.tag = tag;
    field.value_size = static_cast<std::uint8_t>(value.size());
    field.value.fill('\0');
    std::memcpy(field.value.data(), value.data(), value.size());
    return true;
}

template <typename Integer>
[[nodiscard]] bool add_integer(
    FixedFixMessage& message,
    const std::uint16_t tag,
    const Integer value) noexcept {
    std::array<char, 32U> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    return converted.ec == std::errc{} &&
        add_field(
            message,
            tag,
            {text.data(), static_cast<std::size_t>(converted.ptr - text.data())});
}

[[nodiscard]] bool add_price(
    FixedFixMessage& message,
    const std::uint16_t tag,
    const std::int64_t price,
    const std::uint8_t fraction_digits) noexcept {
    if (price < 0) {
        return false;
    }
    std::uint64_t scale = 1U;
    for (std::uint8_t digit = 0U; digit < fraction_digits; ++digit) {
        scale *= 10U;
    }
    const auto unsigned_price = static_cast<std::uint64_t>(price);
    const auto integral = unsigned_price / scale;
    const auto fraction = unsigned_price % scale;
    std::array<char, 48U> text{};
    auto converted = std::to_chars(text.data(), text.data() + text.size(), integral);
    if (converted.ec != std::errc{}) {
        return false;
    }
    char* cursor = converted.ptr;
    if (fraction_digits != 0U) {
        *cursor++ = '.';
        std::uint64_t divisor = scale / 10U;
        for (std::uint8_t digit = 0U; digit < fraction_digits; ++digit) {
            *cursor++ = static_cast<char>('0' + (fraction / divisor) % 10U);
            divisor = divisor == 1U ? 1U : divisor / 10U;
        }
    }
    return add_field(
        message,
        tag,
        {text.data(), static_cast<std::size_t>(cursor - text.data())});
}

[[nodiscard]] std::pair<std::string_view, std::string_view> execution_codes(
    const protocol::sobp_v1::ExecutionStatus status) noexcept {
    using Status = protocol::sobp_v1::ExecutionStatus;
    switch (status) {
        case Status::Pending: return {"A", "A"};
        case Status::Working: return {"0", "0"};
        case Status::PartiallyFilled: return {"F", "1"};
        case Status::Filled: return {"F", "2"};
        case Status::Canceled: return {"4", "4"};
        case Status::Rejected: return {"8", "8"};
        case Status::Ambiguous: return {"I", "9"};
    }
    return {"I", "9"};
}

}  // namespace

Translator::Translator(
    const TranslatorConfig& config,
    const std::span<const InstrumentMapping> instruments) noexcept
    : config_(config) {
    if (config.session_id == 0U || config.session_epoch == 0U ||
        config.price_fraction_digits > 9U ||
        !valid_config_text(config.expected_sender_comp_id) ||
        !valid_config_text(config.expected_target_comp_id) ||
        !valid_config_text(config.expected_account) ||
        !valid_config_text(config.response_sender_comp_id) ||
        !valid_config_text(config.response_target_comp_id) ||
        instruments.empty() || instruments.size() > kMaximumInstrumentMappings) {
        return;
    }
    for (std::size_t index = 0U; index < instruments.size(); ++index) {
        const auto symbol = fixed_text_view(instruments[index].symbol);
        if (symbol.empty() || instruments[index].instrument_id == 0U) {
            return;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (fixed_text_view(instruments[prior].symbol) == symbol ||
                instruments[prior].instrument_id == instruments[index].instrument_id) {
                return;
            }
        }
        instruments_[index] = instruments[index];
    }
    instrument_count_ = static_cast<std::uint16_t>(instruments.size());
    configured_ = true;
}

std::uint64_t Translator::request_id(
    const CommandKind kind,
    const std::string_view client_order_id) const noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    hash = fnv1a_append(hash, "SORX-FIX44-REQUEST-V1");
    hash = fnv1a_append_u64(hash, config_.session_id);
    hash = fnv1a_append_u64(hash, config_.session_epoch);
    hash = fnv1a_append_u64(hash, static_cast<std::uint8_t>(kind));
    hash = fnv1a_append(hash, client_order_id);
    return hash == 0U ? 1U : hash;
}

std::uint64_t Translator::parent_order_id(
    const std::string_view original_client_order_id) const noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    hash = fnv1a_append(hash, "SORX-FIX44-PARENT-V1");
    hash = fnv1a_append_u64(hash, config_.session_id);
    hash = fnv1a_append(hash, original_client_order_id);
    return hash == 0U ? 1U : hash;
}

TranslateResult Translator::translate(const std::span<const char> bytes) const noexcept {
    TranslateResult result;
    if (!configured_) {
        result.error = TranslateError::InvalidConfiguration;
        return result;
    }
    const ParseResult parsed = parse_frame(bytes);
    if (parsed.error != TranslateError::None) {
        result.error = parsed.error;
        result.offending_tag = parsed.offending_tag;
        return result;
    }
    const ParsedFrame& frame = parsed.frame;

    const ParsedField* message_type = require_field(frame, 35U, result);
    if (message_type == nullptr || !command_kind(message_type->value, result.command.kind)) {
        if (result.error == TranslateError::None) {
            result.error = TranslateError::UnsupportedMessageType;
            result.offending_tag = 35U;
        }
        return result;
    }
    for (std::size_t index = 0U; index < frame.count; ++index) {
        if (!allowed_tag(result.command.kind, frame.fields[index].tag)) {
            result.error = TranslateError::UnsupportedTag;
            result.offending_tag = frame.fields[index].tag;
            return result;
        }
    }

    const ParsedField* sequence = require_field(frame, 34U, result);
    const ParsedField* sender = require_field(frame, 49U, result);
    const ParsedField* target = require_field(frame, 56U, result);
    const ParsedField* sending_time = require_field(frame, 52U, result);
    const ParsedField* transaction_time = require_field(frame, 60U, result);
    if (sequence == nullptr || sender == nullptr || target == nullptr ||
        sending_time == nullptr || transaction_time == nullptr) {
        return result;
    }
    if (sender->value != fixed_text_view(config_.expected_sender_comp_id) ||
        target->value != fixed_text_view(config_.expected_target_comp_id)) {
        result.error = TranslateError::SessionIdentityMismatch;
        result.offending_tag = sender->value != fixed_text_view(config_.expected_sender_comp_id)
            ? 49U
            : 56U;
        return result;
    }
    if (!parse_uint64(sequence->value, result.command.client_sequence) ||
        result.command.client_sequence == 0U) {
        result.error = TranslateError::InvalidSessionField;
        result.offending_tag = 34U;
        return result;
    }
    if (!valid_timestamp(sending_time->value)) {
        result.error = TranslateError::InvalidTimestamp;
        result.offending_tag = 52U;
        return result;
    }
    if (!valid_timestamp(transaction_time->value)) {
        result.error = TranslateError::InvalidTimestamp;
        result.offending_tag = 60U;
        return result;
    }

    const ParsedField* account = find_field(frame, 1U);
    const auto configured_account = fixed_text_view(config_.expected_account);
    if ((!configured_account.empty() && account == nullptr) ||
        (account != nullptr && !configured_account.empty() &&
         account->value != configured_account)) {
        result.error = account == nullptr ? TranslateError::MissingRequiredTag
                                          : TranslateError::SessionIdentityMismatch;
        result.offending_tag = 1U;
        return result;
    }

    const ParsedField* possible_duplicate = find_field(frame, 43U);
    const ParsedField* original_sending_time = find_field(frame, 122U);
    if (possible_duplicate != nullptr && possible_duplicate->value != "Y" &&
        possible_duplicate->value != "N") {
        result.error = TranslateError::InvalidSessionField;
        result.offending_tag = 43U;
        return result;
    }
    result.command.possible_duplicate =
        possible_duplicate != nullptr && possible_duplicate->value == "Y";
    if ((result.command.possible_duplicate && original_sending_time == nullptr) ||
        (!result.command.possible_duplicate && original_sending_time != nullptr) ||
        (original_sending_time != nullptr &&
         !valid_timestamp(original_sending_time->value))) {
        result.error = TranslateError::InconsistentReplayFields;
        result.offending_tag = 122U;
        return result;
    }

    const ParsedField* client_id = require_field(frame, 11U, result);
    if (client_id == nullptr) {
        return result;
    }
    if (!assign_fixed_text(result.command.client_order_id, client_id->value)) {
        result.error = TranslateError::IdentifierTooLong;
        result.offending_tag = 11U;
        return result;
    }

    result.command.session_id = config_.session_id;
    result.command.session_epoch = config_.session_epoch;
    result.command.request_id = request_id(result.command.kind, client_id->value);

    const ParsedField* original_id = nullptr;
    if (result.command.kind == CommandKind::CancelOrder ||
        result.command.kind == CommandKind::ReplaceOrder) {
        original_id = require_field(frame, 41U, result);
        if (original_id == nullptr) {
            return result;
        }
        if (original_id->value == client_id->value ||
            !assign_fixed_text(
                result.command.original_client_order_id, original_id->value)) {
            result.error = original_id->value == client_id->value
                ? TranslateError::InvalidSessionField
                : TranslateError::IdentifierTooLong;
            result.offending_tag = 41U;
            return result;
        }
        result.command.parent_order_id = parent_order_id(original_id->value);
    } else if (result.command.kind == CommandKind::NewOrder) {
        result.command.parent_order_id = parent_order_id(client_id->value);
    }

    if (result.command.kind != CommandKind::MassCancel ||
        find_field(frame, 55U) != nullptr) {
        const ParsedField* symbol = require_field(frame, 55U, result);
        if (symbol == nullptr) {
            return result;
        }
        if (!assign_fixed_text(result.command.symbol, symbol->value)) {
            result.error = TranslateError::IdentifierTooLong;
            result.offending_tag = 55U;
            return result;
        }
        bool found = false;
        for (std::size_t index = 0U; index < instrument_count_; ++index) {
            if (fixed_text_view(instruments_[index].symbol) == symbol->value) {
                result.command.instrument_id = instruments_[index].instrument_id;
                found = true;
                break;
            }
        }
        if (!found) {
            result.error = TranslateError::UnknownInstrument;
            result.offending_tag = 55U;
            return result;
        }
    }

    const ParsedField* side = find_field(frame, 54U);
    if (result.command.kind != CommandKind::MassCancel && side == nullptr) {
        result.error = TranslateError::MissingRequiredTag;
        result.offending_tag = 54U;
        return result;
    }
    if (side != nullptr) {
        if (side->value == "1") {
            result.command.side = protocol::sobp_v1::Side::Buy;
            result.command.side_filter = 1U;
        } else if (side->value == "2") {
            result.command.side = protocol::sobp_v1::Side::Sell;
            result.command.side_filter = 2U;
        } else {
            result.error = TranslateError::UnsupportedSide;
            result.offending_tag = 54U;
            return result;
        }
    }

    if (result.command.kind == CommandKind::NewOrder ||
        result.command.kind == CommandKind::ReplaceOrder) {
        const ParsedField* handling_instruction = require_field(frame, 21U, result);
        const ParsedField* quantity = require_field(frame, 38U, result);
        const ParsedField* order_type = require_field(frame, 40U, result);
        const ParsedField* price = require_field(frame, 44U, result);
        const ParsedField* time_in_force = require_field(frame, 59U, result);
        if (handling_instruction == nullptr || quantity == nullptr ||
            order_type == nullptr || price == nullptr || time_in_force == nullptr) {
            return result;
        }
        if (handling_instruction->value != "1") {
            result.error = TranslateError::UnsupportedHandlingInstruction;
            result.offending_tag = 21U;
            return result;
        }
        const IntegerParseStatus quantity_status =
            parse_uint64_status(quantity->value, result.command.quantity);
        if (quantity_status != IntegerParseStatus::Ok ||
            result.command.quantity == 0U) {
            result.error = quantity_status == IntegerParseStatus::Overflow
                ? TranslateError::NumericOverflow
                : TranslateError::InvalidNumeric;
            result.offending_tag = 38U;
            return result;
        }
        if (order_type->value != "2") {
            result.error = TranslateError::UnsupportedOrderType;
            result.offending_tag = 40U;
            return result;
        }
        TranslateError price_error = TranslateError::None;
        if (!parse_price(
                price->value,
                config_.price_fraction_digits,
                result.command.limit_price,
                price_error)) {
            result.error = price_error;
            result.offending_tag = 44U;
            return result;
        }
        const ParsedField* execution_instruction = find_field(frame, 18U);
        if (time_in_force->value == "3") {
            result.command.time_in_force = TimeInForce::ImmediateOrCancel;
            if (execution_instruction != nullptr) {
                result.error = TranslateError::UnsupportedExecutionInstruction;
                result.offending_tag = 18U;
                return result;
            }
        } else if (time_in_force->value == "0" ||
                   time_in_force->value == "1") {
            result.command.time_in_force = time_in_force->value == "0"
                ? TimeInForce::Day
                : TimeInForce::GoodTillCancel;
            if (execution_instruction == nullptr ||
                execution_instruction->value != "6") {
                result.error = TranslateError::UnsupportedExecutionInstruction;
                result.offending_tag = 18U;
                return result;
            }
            result.command.post_only = true;
        } else {
            result.error = TranslateError::UnsupportedTimeInForce;
            result.offending_tag = 59U;
            return result;
        }
    } else if (find_field(frame, 18U) != nullptr || find_field(frame, 38U) != nullptr ||
               find_field(frame, 40U) != nullptr || find_field(frame, 44U) != nullptr ||
               find_field(frame, 59U) != nullptr) {
        result.error = TranslateError::UnsupportedTag;
        return result;
    }

    if (result.command.kind == CommandKind::MassCancel) {
        const ParsedField* scope = require_field(frame, 530U, result);
        if (scope == nullptr) {
            return result;
        }
        if (scope->value == "7") {
            if (find_field(frame, 55U) != nullptr) {
                result.error = TranslateError::UnsupportedMassCancelScope;
                result.offending_tag = 530U;
                return result;
            }
            result.command.mass_cancel_scope =
                protocol::sobp_v1::MassCancelScope::Session;
        } else if (scope->value == "1") {
            if (find_field(frame, 55U) == nullptr) {
                result.error = TranslateError::MissingRequiredTag;
                result.offending_tag = 55U;
                return result;
            }
            result.command.mass_cancel_scope =
                protocol::sobp_v1::MassCancelScope::Instrument;
        } else {
            result.error = TranslateError::UnsupportedMassCancelScope;
            result.offending_tag = 530U;
            return result;
        }
    }
    return result;
}

SobpConversionResult Translator::to_sobp(
    const CommandDescriptor& command,
    const std::uint64_t send_time_ns) const noexcept {
    SobpConversionResult result;
    if (!configured_ || command.session_id != config_.session_id ||
        command.session_epoch != config_.session_epoch ||
        command.client_sequence == 0U || command.request_id == 0U) {
        result.error = SobpConversionError::InvalidDescriptor;
        return result;
    }
    result.message.header.session_id = command.session_id;
    result.message.header.sequence = command.client_sequence;
    result.message.header.request_id = command.request_id;
    result.message.header.send_time_ns = send_time_ns;
    if (command.possible_duplicate) {
        result.message.header.flags =
            protocol::sobp_v1::flag(protocol::sobp_v1::HeaderFlag::Retransmission);
    }

    const auto client_id = fixed_text_view(command.client_order_id);
    switch (command.kind) {
        case CommandKind::NewOrder: {
            protocol::sobp_v1::NewOrderPayload payload;
            payload.parent_order_id = command.parent_order_id;
            payload.instrument_id = command.instrument_id;
            payload.side = command.side;
            payload.quantity = command.quantity;
            payload.limit_price = command.limit_price;
            if (command.time_in_force == TimeInForce::ImmediateOrCancel &&
                !command.post_only) {
                payload.intent = protocol::sobp_v1::RoutingIntent::AggressiveIoc;
            } else if ((command.time_in_force == TimeInForce::Day ||
                        command.time_in_force == TimeInForce::GoodTillCancel) &&
                       command.post_only) {
                payload.intent = protocol::sobp_v1::RoutingIntent::PassiveGtc;
            } else {
                result.error = SobpConversionError::UnsupportedSemantics;
                return result;
            }
            if (!protocol::sobp_v1::assign_text(payload.client_order_id, client_id)) {
                result.error = SobpConversionError::IdentifierTooLong;
                return result;
            }
            result.message.payload = payload;
            break;
        }
        case CommandKind::CancelOrder: {
            protocol::sobp_v1::CancelOrderPayload payload;
            payload.parent_order_id = command.parent_order_id;
            if (!protocol::sobp_v1::assign_text(payload.client_order_id, client_id)) {
                result.error = SobpConversionError::IdentifierTooLong;
                return result;
            }
            result.message.payload = payload;
            break;
        }
        case CommandKind::ReplaceOrder: {
            protocol::sobp_v1::ReplaceOrderPayload payload;
            payload.parent_order_id = command.parent_order_id;
            payload.new_total_quantity = command.quantity;
            payload.new_limit_price = command.limit_price;
            if (!protocol::sobp_v1::assign_text(payload.client_order_id, client_id)) {
                result.error = SobpConversionError::IdentifierTooLong;
                return result;
            }
            result.message.payload = payload;
            break;
        }
        case CommandKind::MassCancel: {
            protocol::sobp_v1::MassCancelPayload payload;
            payload.scope = command.mass_cancel_scope;
            payload.side_filter = command.side_filter;
            payload.instrument_id = command.instrument_id;
            if (!protocol::sobp_v1::assign_text(payload.client_order_id, client_id)) {
                result.error = SobpConversionError::IdentifierTooLong;
                return result;
            }
            result.message.payload = payload;
            break;
        }
    }
    result.message.header.message_type =
        protocol::sobp_v1::message_type_of(result.message.payload);
    return result;
}

ResponseResult Translator::translate_response(
    const protocol::sobp_v1::Message& response,
    const ResponseContext& context) const noexcept {
    ResponseResult result;
    if (!configured_) {
        result.error = ResponseError::InvalidConfiguration;
        return result;
    }
    const auto sending_time = fixed_text_view(context.sending_time);
    if (!valid_timestamp(sending_time)) {
        result.error = ResponseError::InvalidContext;
        return result;
    }
    const auto add_common = [&](const std::string_view type) noexcept {
        return add_field(result.message, 8U, "FIX.4.4") &&
            add_field(result.message, 35U, type) &&
            add_field(
                result.message,
                49U,
                fixed_text_view(config_.response_sender_comp_id)) &&
            add_field(
                result.message,
                56U,
                fixed_text_view(config_.response_target_comp_id)) &&
            add_integer(result.message, 34U, response.header.sequence) &&
            add_field(result.message, 52U, sending_time);
    };

    const bool encoded = std::visit(
        [&](const auto& payload) noexcept {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, protocol::sobp_v1::ExecutionPayload>) {
                const auto client_id = fixed_text_view(context.client_order_id);
                const auto execution_id = protocol::sobp_v1::text_view(payload.execution_id);
                const auto [execution_type, order_status] =
                    execution_codes(payload.status);
                return !client_id.empty() && !execution_id.empty() &&
                    add_common("8") && add_integer(result.message, 37U, payload.child_order_id) &&
                    add_field(result.message, 11U, client_id) &&
                    add_field(result.message, 17U, execution_id) &&
                    add_field(result.message, 150U, execution_type) &&
                    add_field(result.message, 39U, order_status) &&
                    add_field(
                        result.message,
                        54U,
                        payload.side == protocol::sobp_v1::Side::Buy ? "1" : "2") &&
                    add_integer(result.message, 32U, payload.last_quantity) &&
                    add_price(
                        result.message,
                        31U,
                        payload.last_price,
                        config_.price_fraction_digits) &&
                    add_integer(result.message, 14U, payload.cumulative_quantity) &&
                    add_integer(result.message, 151U, payload.leaves_quantity);
            } else if constexpr (
                std::is_same_v<Payload, protocol::sobp_v1::DurableAckPayload>) {
                const auto client_id = fixed_text_view(context.client_order_id);
                if (client_id.empty() || context.parent_order_id == 0U ||
                    context.order_quantity == 0U) {
                    return false;
                }
                std::string_view execution_type;
                std::string_view order_status;
                switch (payload.acknowledged_type) {
                    case protocol::sobp_v1::MessageType::NewOrder:
                        execution_type = "A";
                        order_status = "A";
                        break;
                    case protocol::sobp_v1::MessageType::CancelOrder:
                        execution_type = "6";
                        order_status = "6";
                        break;
                    case protocol::sobp_v1::MessageType::ReplaceOrder:
                        execution_type = "E";
                        order_status = "E";
                        break;
                    default: return false;
                }
                std::array<char, 32U> execution_id{};
                constexpr std::string_view prefix{"DURABLE-"};
                std::memcpy(execution_id.data(), prefix.data(), prefix.size());
                const auto converted = std::to_chars(
                    execution_id.data() + prefix.size(),
                    execution_id.data() + execution_id.size(),
                    payload.journal_record_id);
                if (converted.ec != std::errc{}) {
                    return false;
                }
                const std::string_view durable_execution_id{
                    execution_id.data(),
                    static_cast<std::size_t>(converted.ptr - execution_id.data())};
                return add_common("8") &&
                    add_integer(result.message, 37U, context.parent_order_id) &&
                    add_field(result.message, 11U, client_id) &&
                    add_field(result.message, 17U, durable_execution_id) &&
                    add_field(result.message, 150U, execution_type) &&
                    add_field(result.message, 39U, order_status) &&
                    add_field(
                        result.message,
                        54U,
                        context.side == protocol::sobp_v1::Side::Buy ? "1" : "2") &&
                    add_integer(result.message, 38U, context.order_quantity) &&
                    add_integer(result.message, 14U, 0U) &&
                    add_integer(result.message, 151U, context.order_quantity) &&
                    add_price(
                        result.message,
                        6U,
                        0,
                        config_.price_fraction_digits);
            } else if constexpr (std::is_same_v<Payload, protocol::sobp_v1::RejectPayload>) {
                const auto detail = protocol::sobp_v1::text_view(payload.detail);
                return add_common("3") &&
                    add_integer(result.message, 45U, response.header.sequence) &&
                    add_field(result.message, 372U, sobp_message_type(payload.rejected_type)) &&
                    add_field(result.message, 373U, "99") &&
                    add_field(result.message, 58U, detail.empty() ? "SORX reject" : detail);
            } else if constexpr (std::is_same_v<Payload, protocol::sobp_v1::StatusPayload>) {
                const auto detail = protocol::sobp_v1::text_view(payload.detail);
                std::string_view status = "1";
                if (payload.status == protocol::sobp_v1::ApplianceStatus::Ready) {
                    status = "2";
                } else if (payload.status ==
                           protocol::sobp_v1::ApplianceStatus::Bootstrapping ||
                           payload.status ==
                               protocol::sobp_v1::ApplianceStatus::RecoveryOnly) {
                    status = "4";
                } else if (payload.status ==
                           protocol::sobp_v1::ApplianceStatus::Maintenance) {
                    status = "3";
                }
                return add_common("h") && add_field(result.message, 336U, "SORX") &&
                    add_field(result.message, 340U, status) &&
                    add_field(result.message, 58U, detail.empty() ? "SORX status" : detail);
            } else {
                return false;
            }
        },
        response.payload);
    if (!encoded) {
        result.error = ResponseError::UnsupportedPayload;
        result.message = {};
    }
    return result;
}

EncodeFixResult Translator::encode_response(
    const FixedFixMessage& message,
    const std::span<char> destination) noexcept {
    EncodeFixResult result;
    if (message.field_count < 2U || message.fields[0U].tag != 8U ||
        message.fields[1U].tag != 35U ||
        message.fields[0U].value_view() != "FIX.4.4") {
        result.error = ResponseError::InvalidContext;
        return result;
    }
    std::size_t body_length = 0U;
    for (std::size_t index = 1U; index < message.field_count; ++index) {
        const FixedFixField& field = message.fields[index];
        if (field.tag == 8U || field.tag == 9U || field.tag == 10U ||
            field.value_size == 0U) {
            result.error = ResponseError::InvalidContext;
            return result;
        }
        for (const char character : field.value_view()) {
            if (character == kFixDelimiter || character == '\0') {
                result.error = ResponseError::InvalidContext;
                return result;
            }
        }
        std::array<char, 8U> tag_text{};
        const auto tag = std::to_chars(
            tag_text.data(), tag_text.data() + tag_text.size(), field.tag);
        if (tag.ec != std::errc{}) {
            result.error = ResponseError::InvalidContext;
            return result;
        }
        body_length += static_cast<std::size_t>(tag.ptr - tag_text.data()) + 1U +
            field.value_size + 1U;
    }
    std::array<char, 32U> body_length_text{};
    const auto body = std::to_chars(
        body_length_text.data(), body_length_text.data() + body_length_text.size(), body_length);
    if (body.ec != std::errc{}) {
        result.error = ResponseError::ValueCapacityExceeded;
        return result;
    }
    const auto begin = message.fields[0U].value_view();
    const std::size_t required = 2U + begin.size() + 1U + 2U +
        static_cast<std::size_t>(body.ptr - body_length_text.data()) + 1U +
        body_length + 7U;
    if (destination.size() < required) {
        result.error = ResponseError::BufferTooSmall;
        return result;
    }

    std::size_t cursor = 0U;
    const auto write_text = [&](const std::string_view text) noexcept {
        std::memcpy(destination.data() + cursor, text.data(), text.size());
        cursor += text.size();
    };
    write_text("8=");
    write_text(begin);
    destination[cursor++] = kFixDelimiter;
    write_text("9=");
    write_text(
        {body_length_text.data(),
         static_cast<std::size_t>(body.ptr - body_length_text.data())});
    destination[cursor++] = kFixDelimiter;
    for (std::size_t index = 1U; index < message.field_count; ++index) {
        std::array<char, 8U> tag_text{};
        const auto tag = std::to_chars(
            tag_text.data(), tag_text.data() + tag_text.size(), message.fields[index].tag);
        write_text(
            {tag_text.data(),
             static_cast<std::size_t>(tag.ptr - tag_text.data())});
        destination[cursor++] = '=';
        write_text(message.fields[index].value_view());
        destination[cursor++] = kFixDelimiter;
    }
    unsigned checksum = 0U;
    for (std::size_t index = 0U; index < cursor; ++index) {
        checksum = (checksum + static_cast<unsigned char>(destination[index])) & 0xffU;
    }
    write_text("10=");
    destination[cursor++] = static_cast<char>('0' + (checksum / 100U) % 10U);
    destination[cursor++] = static_cast<char>('0' + (checksum / 10U) % 10U);
    destination[cursor++] = static_cast<char>('0' + checksum % 10U);
    destination[cursor++] = kFixDelimiter;
    result.bytes_written = cursor;
    return result;
}

}  // namespace sorx::fixgw
