#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/fixgw/translator.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> allocation_count{0U};

void record_allocation() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t requested_size) {
    record_allocation();
    if (void* const memory = std::malloc(requested_size == 0U ? 1U : requested_size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t requested_size,
    const std::size_t alignment) {
    record_allocation();
#if defined(_MSC_VER)
    if (void* const memory =
            _aligned_malloc(requested_size == 0U ? 1U : requested_size, alignment)) {
        return memory;
    }
#else
    const auto nonzero_size = requested_size == 0U ? 1U : requested_size;
    const auto rounded_size =
        ((nonzero_size + alignment - 1U) / alignment) * alignment;
    if (void* const memory = std::aligned_alloc(alignment, rounded_size)) {
        return memory;
    }
#endif
    throw std::bad_alloc{};
}

void deallocate_aligned(void* const memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

class Scope final {
public:
    Scope() noexcept {
        allocation_count.store(0U, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { armed.store(false, std::memory_order_relaxed); }

    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocation_count.load(std::memory_order_relaxed);
    }
};

}  // namespace allocation_probe

void* operator new(const std::size_t size) {
    return allocation_probe::allocate(size);
}
void* operator new[](const std::size_t size) {
    return allocation_probe::allocate(size);
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void* operator new(const std::size_t size, const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(void* const memory, const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](void* const memory, const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete(
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}

namespace {

namespace fix = sorx::fixgw;
namespace sobp = sorx::protocol::sobp_v1;

[[nodiscard]] std::string field(
    const std::string_view tag,
    const std::string_view value) {
    std::string output;
    output.reserve(tag.size() + value.size() + 2U);
    output.append(tag);
    output.push_back('=');
    output.append(value);
    output.push_back(fix::kFixDelimiter);
    return output;
}

[[nodiscard]] std::string frame_from_body(const std::string_view body) {
    std::string output = field("8", "FIX.4.4");
    output += field("9", std::to_string(body.size()));
    output.append(body);
    unsigned checksum = 0U;
    for (const char character : output) {
        checksum = (checksum + static_cast<unsigned char>(character)) & 0xffU;
    }
    output += "10=";
    output.push_back(static_cast<char>('0' + (checksum / 100U) % 10U));
    output.push_back(static_cast<char>('0' + (checksum / 10U) % 10U));
    output.push_back(static_cast<char>('0' + checksum % 10U));
    output.push_back(fix::kFixDelimiter);
    return output;
}

[[nodiscard]] std::string common_body(
    const std::string_view message_type,
    const std::string_view sequence) {
    return field("35", message_type) + field("34", sequence) +
        field("49", "CLIENT") + field("56", "SORX") +
        field("52", "20260815-12:34:56.123") + field("1", "ACCOUNT-A");
}

[[nodiscard]] std::string new_order_body(
    const std::string_view client_id = "NEW-1",
    const std::string_view quantity = "100",
    const std::string_view price = "123.4567") {
    return common_body("D", "12") + field("11", client_id) +
        field("21", "1") + field("55", "AAPL") + field("54", "1") + field("38", quantity) +
        field("40", "2") + field("44", price) + field("59", "3") +
        field("60", "20260815-12:34:56.123");
}

[[nodiscard]] fix::TranslatorConfig config() {
    fix::TranslatorConfig result;
    result.session_id = 77U;
    result.session_epoch = 9U;
    result.price_fraction_digits = 4U;
    assert(fix::assign_fixed_text(result.expected_sender_comp_id, "CLIENT"));
    assert(fix::assign_fixed_text(result.expected_target_comp_id, "SORX"));
    assert(fix::assign_fixed_text(result.expected_account, "ACCOUNT-A"));
    assert(fix::assign_fixed_text(result.response_sender_comp_id, "SORX"));
    assert(fix::assign_fixed_text(result.response_target_comp_id, "CLIENT"));
    return result;
}

[[nodiscard]] std::array<fix::InstrumentMapping, 2U> instruments() {
    std::array<fix::InstrumentMapping, 2U> result{};
    assert(fix::assign_fixed_text(result[0U].symbol, "AAPL"));
    result[0U].instrument_id = 101U;
    assert(fix::assign_fixed_text(result[1U].symbol, "MSFT"));
    result[1U].instrument_id = 102U;
    return result;
}

[[nodiscard]] const fix::FixedFixField* response_field(
    const fix::FixedFixMessage& message,
    const std::uint16_t tag) noexcept {
    for (const fix::FixedFixField& candidate : message.view()) {
        if (candidate.tag == tag) {
            return &candidate;
        }
    }
    return nullptr;
}

void new_order_and_replay_tests() {
    const auto mapping = instruments();
    const fix::Translator translator{config(), mapping};
    assert(translator.configured());
    const std::string frame = frame_from_body(new_order_body());
    const auto translated = translator.translate(frame);
    assert(translated);
    assert(translated.command.kind == fix::CommandKind::NewOrder);
    assert(translated.command.session_id == 77U);
    assert(translated.command.session_epoch == 9U);
    assert(translated.command.client_sequence == 12U);
    assert(translated.command.instrument_id == 101U);
    assert(translated.command.side == sobp::Side::Buy);
    assert(translated.command.quantity == 100U);
    assert(translated.command.limit_price == 1'234'567);
    assert(translated.command.time_in_force == fix::TimeInForce::ImmediateOrCancel);
    assert(!translated.command.post_only);
    assert(fix::fixed_text_view(translated.command.client_order_id) == "NEW-1");
    assert(translated.command.parent_order_id == translator.parent_order_id("NEW-1"));
    assert(translated.command.request_id ==
           translator.request_id(fix::CommandKind::NewOrder, "NEW-1"));

    const auto binary = translator.to_sobp(translated.command, 900U);
    assert(binary);
    assert(binary.message.header.message_type == sobp::MessageType::NewOrder);
    assert(binary.message.header.request_id == translated.command.request_id);
    const auto& payload = std::get<sobp::NewOrderPayload>(binary.message.payload);
    assert(payload.instrument_id == 101U);
    assert(payload.intent == sobp::RoutingIntent::AggressiveIoc);
    assert(payload.limit_price == 1'234'567);

    const std::string replay_body = common_body("D", "12") +
        field("43", "Y") + field("122", "20260815-12:34:55.999") +
        field("11", "NEW-1") + field("21", "1") + field("55", "AAPL") + field("54", "1") +
        field("38", "100") + field("40", "2") + field("44", "123.4567") +
        field("59", "3") + field("60", "20260815-12:34:56.123");
    const auto replay = translator.translate(frame_from_body(replay_body));
    assert(replay);
    assert(replay.command.possible_duplicate);
    assert(replay.command.request_id == translated.command.request_id);
    assert(replay.command.parent_order_id == translated.command.parent_order_id);

    auto reconnected_config = config();
    reconnected_config.session_epoch = 10U;
    const fix::Translator reconnected{reconnected_config, mapping};
    assert(reconnected.parent_order_id("NEW-1") == translated.command.parent_order_id);
    assert(reconnected.request_id(fix::CommandKind::NewOrder, "NEW-1") !=
           translated.command.request_id);
}

void cancel_replace_and_mass_cancel_tests() {
    const auto mapping = instruments();
    const fix::Translator translator{config(), mapping};

    const std::string cancel_body = common_body("F", "13") +
        field("11", "CANCEL-1") + field("41", "NEW-1") +
        field("55", "AAPL") + field("54", "1") +
        field("60", "20260815-12:34:57");
    const auto cancel = translator.translate(frame_from_body(cancel_body));
    assert(cancel);
    assert(cancel.command.kind == fix::CommandKind::CancelOrder);
    assert(cancel.command.parent_order_id == translator.parent_order_id("NEW-1"));
    assert(fix::fixed_text_view(cancel.command.original_client_order_id) == "NEW-1");
    const auto binary_cancel = translator.to_sobp(cancel.command, 901U);
    assert(binary_cancel);
    assert(binary_cancel.message.header.message_type == sobp::MessageType::CancelOrder);

    const std::string replace_body = common_body("G", "14") +
        field("11", "REPLACE-1") + field("41", "NEW-1") +
        field("21", "1") + field("55", "AAPL") + field("54", "1") + field("38", "125") +
        field("40", "2") + field("44", "123.4000") + field("59", "0") +
        field("18", "6") + field("60", "20260815-12:34:58");
    const auto replace = translator.translate(frame_from_body(replace_body));
    assert(replace);
    assert(replace.command.kind == fix::CommandKind::ReplaceOrder);
    assert(replace.command.parent_order_id == cancel.command.parent_order_id);
    assert(replace.command.time_in_force == fix::TimeInForce::Day);
    assert(replace.command.post_only);
    assert(replace.command.quantity == 125U);

    const std::string instrument_mass_body = common_body("q", "15") +
        field("11", "MASS-1") + field("530", "1") + field("55", "AAPL") +
        field("54", "2") + field("60", "20260815-12:34:59");
    const auto instrument_mass =
        translator.translate(frame_from_body(instrument_mass_body));
    assert(instrument_mass);
    assert(instrument_mass.command.mass_cancel_scope ==
           sobp::MassCancelScope::Instrument);
    assert(instrument_mass.command.instrument_id == 101U);
    assert(instrument_mass.command.side_filter == 2U);

    const std::string session_mass_body = common_body("q", "16") +
        field("11", "MASS-2") + field("530", "7") +
        field("60", "20260815-12:35:00");
    const auto session_mass = translator.translate(frame_from_body(session_mass_body));
    assert(session_mass);
    assert(session_mass.command.mass_cancel_scope == sobp::MassCancelScope::Session);
    assert(session_mass.command.instrument_id == 0U);
    assert(session_mass.command.side_filter == 0U);
}

void malformed_and_unsupported_tests() {
    const auto mapping = instruments();
    const fix::Translator translator{config(), mapping};

    std::string bad_length = frame_from_body(new_order_body());
    const std::size_t length_digit = bad_length.find("9=") + 2U;
    bad_length[length_digit] = bad_length[length_digit] == '9'
        ? '8'
        : static_cast<char>(bad_length[length_digit] + 1);
    assert(translator.translate(bad_length).error == fix::TranslateError::InvalidBodyLength);

    std::string bad_checksum = frame_from_body(new_order_body());
    bad_checksum[bad_checksum.size() - 2U] =
        bad_checksum[bad_checksum.size() - 2U] == '9' ? '0' : '9';
    assert(translator.translate(bad_checksum).error == fix::TranslateError::InvalidChecksum);

    const std::string duplicate_body = new_order_body() + field("11", "DUPLICATE");
    const auto duplicate = translator.translate(frame_from_body(duplicate_body));
    assert(duplicate.error == fix::TranslateError::DuplicateTag);
    assert(duplicate.offending_tag == 11U);

    const std::string unsupported_tag_body =
        new_order_body() + field("100", "EDGX");
    const auto unsupported_tag =
        translator.translate(frame_from_body(unsupported_tag_body));
    assert(unsupported_tag.error == fix::TranslateError::UnsupportedTag);
    assert(unsupported_tag.offending_tag == 100U);

    const auto quantity_overflow = translator.translate(frame_from_body(
        new_order_body("NEW-2", "18446744073709551616", "1.0000")));
    assert(quantity_overflow.error == fix::TranslateError::NumericOverflow);
    assert(quantity_overflow.offending_tag == 38U);

    const auto price_precision = translator.translate(
        frame_from_body(new_order_body("NEW-3", "1", "123.45671")));
    assert(price_precision.error == fix::TranslateError::InvalidNumeric);
    assert(price_precision.offending_tag == 44U);

    const std::string passive_without_post_only = common_body("D", "17") +
        field("11", "NEW-4") + field("55", "AAPL") + field("54", "1") +
        field("21", "1") + field("38", "1") + field("40", "2") + field("44", "1.0000") +
        field("59", "0") + field("60", "20260815-12:35:01");
    const auto not_post_only =
        translator.translate(frame_from_body(passive_without_post_only));
    assert(not_post_only.error ==
           fix::TranslateError::UnsupportedExecutionInstruction);

    const std::string unknown_symbol_body = common_body("D", "18") +
        field("11", "NEW-5") + field("55", "UNKNOWN") + field("54", "1") +
        field("21", "1") + field("38", "1") + field("40", "2") + field("44", "1.0000") +
        field("59", "3") + field("60", "20260815-12:35:02");
    assert(translator.translate(frame_from_body(unknown_symbol_body)).error ==
           fix::TranslateError::UnknownInstrument);

    const std::string replay_without_original_time = common_body("D", "19") +
        field("43", "Y") + field("11", "NEW-6") + field("55", "AAPL") +
        field("54", "1") + field("21", "1") + field("38", "1") + field("40", "2") +
        field("44", "1.0000") + field("59", "3") +
        field("60", "20260815-12:35:03");
    assert(translator.translate(frame_from_body(replay_without_original_time)).error ==
           fix::TranslateError::InconsistentReplayFields);

    const std::string long_id(33U, 'X');
    assert(translator.translate(frame_from_body(new_order_body(long_id))).error ==
           fix::TranslateError::IdentifierTooLong);
}

void response_and_allocation_tests() {
    const auto mapping = instruments();
    const fix::Translator translator{config(), mapping};
    const std::string inbound = frame_from_body(new_order_body());
    const auto warmup = translator.translate(inbound);
    assert(warmup);

    sobp::ExecutionPayload execution;
    execution.originating_request_id = warmup.command.request_id;
    execution.parent_order_id = warmup.command.parent_order_id;
    execution.child_order_id = 9001U;
    execution.venue_id = 2U;
    execution.status = sobp::ExecutionStatus::PartiallyFilled;
    execution.side = sobp::Side::Buy;
    execution.last_quantity = 25U;
    execution.last_price = 1'234'500;
    execution.cumulative_quantity = 25U;
    execution.leaves_quantity = 75U;
    assert(sobp::assign_text(execution.execution_id, "EXEC-1"));
    sobp::Message response;
    response.header.message_type = sobp::MessageType::Execution;
    response.header.session_id = 77U;
    response.header.sequence = 44U;
    response.payload = execution;
    fix::ResponseContext context;
    assert(fix::assign_fixed_text(context.client_order_id, "NEW-1"));
    assert(fix::assign_fixed_text(context.sending_time, "20260815-12:36:00.001"));
    context.parent_order_id = warmup.command.parent_order_id;
    context.order_quantity = warmup.command.quantity;
    context.side = warmup.command.side;

    const auto fields = translator.translate_response(response, context);
    assert(fields);
    assert(response_field(fields.message, 35U)->value_view() == "8");
    assert(response_field(fields.message, 11U)->value_view() == "NEW-1");
    assert(response_field(fields.message, 150U)->value_view() == "F");
    assert(response_field(fields.message, 39U)->value_view() == "1");
    assert(response_field(fields.message, 31U)->value_view() == "123.4500");

    std::array<char, fix::kMaximumFixFrameBytes> encoded{};
    const auto encoded_result =
        fix::Translator::encode_response(fields.message, encoded);
    assert(encoded_result);
    const std::string_view encoded_view{encoded.data(), encoded_result.bytes_written};
    assert(encoded_view.starts_with("8=FIX.4.4\x01" "9="));
    assert(encoded_view.find("35=8\x01") != std::string_view::npos);
    assert(encoded_view.find("10=") != std::string_view::npos);

    sobp::DurableAckPayload durable_ack;
    durable_ack.acknowledged_request_id = warmup.command.request_id;
    durable_ack.journal_record_id = 7001U;
    durable_ack.ingress_sequence = 12U;
    durable_ack.acknowledged_type = sobp::MessageType::NewOrder;
    sobp::Message durable_response;
    durable_response.header.message_type = sobp::MessageType::DurableAck;
    durable_response.header.session_id = 77U;
    durable_response.header.sequence = 45U;
    durable_response.payload = durable_ack;
    const auto durable_fields =
        translator.translate_response(durable_response, context);
    assert(durable_fields);
    assert(response_field(durable_fields.message, 150U)->value_view() == "A");
    assert(response_field(durable_fields.message, 17U)->value_view() ==
           "DURABLE-7001");

    {
        allocation_probe::Scope probe;
        const auto translated = translator.translate(inbound);
        const auto binary = translator.to_sobp(translated.command, 1'000U);
        const auto fixed_response = translator.translate_response(response, context);
        const auto fixed_durable_response =
            translator.translate_response(durable_response, context);
        const auto encoded_again =
            fix::Translator::encode_response(fixed_response.message, encoded);
        assert(translated && binary && fixed_response && fixed_durable_response &&
               encoded_again);
        assert(probe.allocations() == 0U);
    }
}

void invalid_configuration_tests() {
    auto mapping = instruments();
    mapping[1U].instrument_id = mapping[0U].instrument_id;
    const fix::Translator duplicate_instrument{config(), mapping};
    assert(!duplicate_instrument.configured());
    assert(duplicate_instrument.translate({}).error ==
           fix::TranslateError::InvalidConfiguration);
}

}  // namespace

int main() {
    new_order_and_replay_tests();
    cancel_replace_and_mass_cancel_tests();
    malformed_and_unsupported_tests();
    response_and_allocation_tests();
    invalid_configuration_tests();
    return 0;
}
