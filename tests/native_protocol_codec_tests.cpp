#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/connectors/cboe_boe.hpp"
#include "sorx/connectors/nasdaq_ouch5.hpp"
#include "sorx/recovery/fast_recovery.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::atomic<std::uint64_t> allocation_calls{};

}  // namespace

void* operator new(const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) {
    allocation_calls.fetch_add(1U, std::memory_order_relaxed);
    if (void* const value = std::malloc(size == 0U ? 1U : size)) {
        return value;
    }
    throw std::bad_alloc{};
}

void operator delete(void* const value) noexcept {
    std::free(value);
}

void operator delete[](void* const value) noexcept {
    std::free(value);
}

void operator delete(void* const value, const std::size_t) noexcept {
    std::free(value);
}

void operator delete[](void* const value, const std::size_t) noexcept {
    std::free(value);
}

namespace {

namespace nq = sorx::connectors::nasdaq;
namespace cb = sorx::connectors::cboe;

[[nodiscard]] constexpr std::byte b(const char value) noexcept {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
}

template <std::size_t Size>
[[nodiscard]] std::array<char, Size> nul_padded(
    const std::string_view value) noexcept {
    std::array<char, Size> output{};
    assert(value.size() <= Size);
    std::copy(value.begin(), value.end(), output.begin());
    return output;
}

template <std::size_t Size>
[[nodiscard]] std::array<char, Size> space_padded(
    const std::string_view value) noexcept {
    std::array<char, Size> output{};
    output.fill(' ');
    assert(value.size() <= Size);
    std::copy(value.begin(), value.end(), output.begin());
    return output;
}

void put_le(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint64_t value,
    const std::size_t width) noexcept {
    for (std::size_t index{}; index < width; ++index) {
        destination[offset + index] = static_cast<std::byte>(
            (value >> static_cast<unsigned>(index * 8U)) & 0xffU);
    }
}

void put_be(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::uint64_t value,
    const std::size_t width) noexcept {
    for (std::size_t index{}; index < width; ++index) {
        const auto shift = static_cast<unsigned>((width - index - 1U) * 8U);
        destination[offset + index] = static_cast<std::byte>(
            (value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint32_t get_le_u32(
    const std::span<const std::byte> source,
    const std::size_t offset) noexcept {
    assert(offset + 4U <= source.size());
    std::uint32_t value{};
    for (std::size_t index{}; index < 4U; ++index) {
        value |= std::to_integer<std::uint32_t>(source[offset + index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

constexpr sorx::VenueId kNativeNasdaqVenue{31U};
constexpr sorx::VenueId kNativeCboeVenue{32U};
constexpr sorx::InstrumentId kNativeInstrument{701U};
constexpr std::uint64_t kNativeLeadershipEpoch = 17U;
constexpr std::uint64_t kNativeConfigurationGeneration = 29U;
constexpr std::uint64_t kNativeAccountContext = 41U;

[[nodiscard]] sorx::RouteConfig native_route(
    const sorx::VenueId venue,
    const std::uint32_t priority) {
    sorx::RouteConfig output;
    output.venue = venue;
    output.instrument = kNativeInstrument;
    output.venue_name = venue == kNativeNasdaqVenue ? "NASDAQ" : "EDGX";
    output.canonical_symbol = "MSFT";
    output.wire_symbol = "MSFT";
    output.capabilities = sorx::VenueCapabilities{true, true};
    output.allocation_cap = sorx::Quantity{1'000U};
    output.aggressive_priority = priority;
    output.passive_priority = priority;
    output.configured_latency_ns = 100U + priority;
    return output;
}

[[nodiscard]] std::shared_ptr<const sorx::CandidateTable>
native_candidate_table() {
    sorx::RouteRib rib;
    std::vector<sorx::RouteValidationError> errors;
    assert(rib.replace_all(
        std::vector<sorx::RouteConfig>{
            native_route(kNativeNasdaqVenue, 1U),
            native_route(kNativeCboeVenue, 2U)},
        errors));
    assert(errors.empty());
    sorx::FibCompiler compiler;
    return compiler.compile(rib);
}

[[nodiscard]] sorx::CoreConfiguration native_configuration() {
    sorx::CoreConfiguration output;
    output.maximum_aggressive_venues = 2U;
    output.risk.generation = 7U;
    sorx::InstrumentRiskLimit instrument;
    instrument.instrument = kNativeInstrument;
    instrument.maximum_parent_quantity = sorx::Quantity{1'000U};
    instrument.maximum_child_quantity = sorx::Quantity{1'000U};
    instrument.maximum_parent_notional = sorx::Notional{10'000'000'000U};
    instrument.maximum_child_notional = sorx::Notional{10'000'000'000U};
    instrument.minimum_price = sorx::Price{1};
    instrument.maximum_price = sorx::Price{1'000'000};
    instrument.maximum_open_parents = 100U;
    instrument.maximum_open_children = 100U;
    instrument.maximum_gross_outstanding = sorx::Quantity{10'000U};
    output.risk.instruments.push_back(instrument);
    output.risk.venues.push_back(sorx::VenueRiskLimit{
        kNativeNasdaqVenue, true, sorx::Quantity{10'000U}});
    output.risk.venues.push_back(sorx::VenueRiskLimit{
        kNativeCboeVenue, true, sorx::Quantity{10'000U}});
    return output;
}

[[nodiscard]] sorx::dataplane::BookSlotView native_view(
    const sorx::VenueId venue,
    const sorx::Price ask,
    const sorx::Quantity ask_quantity) {
    sorx::md::BookView book;
    book.venue = venue;
    book.channel = sorx::ChannelId{1U};
    book.profile = sorx::ProfileId{1U};
    book.instrument = kNativeInstrument;
    assert(book.push_bid(sorx::md::Level{
        sorx::Price{ask.units - 1}, sorx::Quantity{20U}, 1U}));
    assert(book.push_ask(sorx::md::Level{ask, ask_quantity, 1U}));
    book.feed_session_epoch = 3U;
    book.recovery_generation = 1U;
    book.view_generation = 1U;
    book.last_applied_sequence = 1U;
    book.next_sequence = 2U;
    book.exchange_timestamp_ns = 900U;
    book.local_receive_time_ns = 1'000U;
    book.local_publication_time_ns = 1'010U;
    book.feed_state = sorx::MarketDataState::Live;
    book.trading_state = sorx::InstrumentState::Trading;
    assert(book.valid());

    sorx::dataplane::BookSlotView output;
    output.venue = venue;
    output.instrument = kNativeInstrument;
    output.adjacency_generation = 9U;
    output.book = book;
    output.market_data_state = sorx::MarketDataState::Live;
    output.order_entry_state = sorx::OrderEntryState::Ready;
    output.egress_state = sorx::EgressState::Ready;
    output.instrument_state = sorx::InstrumentState::Trading;
    output.feed_epoch = book.feed_session_epoch;
    output.source_health_generation = 5U;
    output.monotonic_receive_time_ns = 1'000U;
    output.monotonic_publication_time_ns = 1'010U;
    output.maximum_quote_age_ns = 100U;
    output.tx_credit_generation = 4U;
    output.throttle_generation = 8U;
    output.normal_tx_credits = 64U;
    output.cancel_reserved_tx_credits = 16U;
    output.throttle_credits = 64U;
    output.has_book = true;
    output.source_healthy = true;
    output.conversion_compatible = true;
    output.risk_allowed = true;
    assert(output.valid());
    return output;
}

[[nodiscard]] sorx::ParentOrder native_parent(
    const std::uint64_t parent_id,
    const sorx::Quantity quantity = sorx::Quantity{10U}) noexcept {
    return {
        sorx::ParentOrderId{parent_id},
        kNativeInstrument,
        sorx::Side::Buy,
        quantity,
        sorx::Price{101},
        sorx::RoutingIntent::AggressiveIOC};
}

[[nodiscard]] sorx::recovery::RecoveryConfiguration
native_recovery_configuration(
    const sorx::CandidateTable& candidates) noexcept {
    sorx::recovery::RecoveryConfiguration output;
    output.active_parent_capacity = 16U;
    output.client_capacity = 8U;
    output.client_dedupe_capacity = 32U;
    output.replay_dedupe_capacity = 16U;
    output.leadership_epoch = kNativeLeadershipEpoch;
    output.configuration_generation = kNativeConfigurationGeneration;
    output.candidate_table_generation = candidates.generation();
    output.risk_generation = 7U;
    return output;
}

[[nodiscard]] sorx::recovery::RecoveryRequestDigest native_digest(
    const std::uint8_t seed) noexcept {
    sorx::recovery::RecoveryRequestDigest output{};
    for (std::size_t index{}; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>(seed + index);
    }
    return output;
}

[[nodiscard]] sorx::dataplane::WireIdentityPlanStatus
colliding_ouch_identity(
    const sorx::dataplane::WireIdentityPlanRequest& request,
    sorx::dataplane::InlineWireIdentity& destination) noexcept {
    if (!request.venue || !request.child_order_id ||
        request.dispatch_attempt_id == 0U ||
        request.venue_session_id == 0U ||
        request.venue_session_epoch == 0U) {
        return sorx::dataplane::WireIdentityPlanStatus::InvalidInput;
    }
    return nq::make_planned_wire_identity(77U, "COLLIDE", destination)
        ? sorx::dataplane::WireIdentityPlanStatus::Planned
        : sorx::dataplane::WireIdentityPlanStatus::InvalidOutput;
}

[[nodiscard]] sorx::dataplane::WireIdentityPlanStatus
colliding_boe_identity(
    const sorx::dataplane::WireIdentityPlanRequest& request,
    sorx::dataplane::InlineWireIdentity& destination) noexcept {
    if (!request.venue || !request.child_order_id ||
        request.dispatch_attempt_id == 0U ||
        request.venue_session_id == 0U ||
        request.venue_session_epoch == 0U) {
        return sorx::dataplane::WireIdentityPlanStatus::InvalidInput;
    }
    return cb::make_planned_wire_identity(88U, "COLLIDE", destination)
        ? sorx::dataplane::WireIdentityPlanStatus::Planned
        : sorx::dataplane::WireIdentityPlanStatus::InvalidOutput;
}

template <std::size_t Size>
void put_chars(
    const std::span<std::byte> destination,
    const std::size_t offset,
    const std::array<char, Size>& value) noexcept {
    for (std::size_t index{}; index < Size; ++index) {
        destination[offset + index] = b(value[index]);
    }
}

void test_ouch_golden_and_malformed() {
    sorx::dataplane::InlineWireIdentity inline_identity;
    assert(nq::make_planned_wire_identity(1U, "ABC123", inline_identity));
    nq::PlannedWireIdentity identity;
    assert(nq::parse_planned_wire_identity(inline_identity, identity));

    nq::EnterOrder message;
    message.identity = identity;
    message.side = nq::OuchSide::Buy;
    message.quantity = 1'000U;
    message.symbol = space_padded<8U>("MSFT");
    message.price = 1'234'500U;
    message.time_in_force = '3';
    message.display = 'Y';
    message.capacity = 'A';
    message.intermarket_sweep_eligibility = 'Y';
    message.cross_type = 'N';

    std::array<std::byte, nq::kOuchEnterOrderBytes> encoded{};
    const auto result = nq::encode_enter_order(message, encoded);
    assert(result.ok());
    const std::array<std::byte, nq::kOuchEnterOrderBytes> expected{
        b('O'), std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, b('B'),
        std::byte{0}, std::byte{0}, std::byte{0x03}, std::byte{0xe8},
        b('M'), b('S'), b('F'), b('T'), b(' '), b(' '), b(' '), b(' '),
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x12}, std::byte{0xd6}, std::byte{0x44},
        b('3'), b('Y'), b('A'), b('Y'), b('N'),
        b('A'), b('B'), b('C'), b('1'), b('2'), b('3'),
        b(' '), b(' '), b(' '), b(' '), b(' '), b(' '), b(' '), b(' '),
        std::byte{0}, std::byte{0},
    };
    assert(encoded == expected);

    std::array<std::byte, 64U> soup{};
    const auto wrapped = nq::encode_soup_unsequenced(encoded, soup);
    assert(wrapped.ok());
    assert(wrapped.size == 50U);
    assert(soup[0] == std::byte{0});
    assert(soup[1] == std::byte{0x30});
    assert(soup[2] == b('U'));
    assert(std::equal(encoded.begin(), encoded.end(), soup.begin() + 3));

    nq::SoupPacketView packet;
    assert(nq::decode_soup_packet(
        std::span<const std::byte>{soup.data(), wrapped.size}, packet) ==
        nq::CodecStatus::Ok);
    assert(packet.type == nq::SoupPacketType::UnsequencedData);
    assert(std::equal(packet.payload.begin(), packet.payload.end(), encoded.begin()));

    soup[1] = std::byte{0x31};
    assert(nq::decode_soup_packet(
        std::span<const std::byte>{soup.data(), wrapped.size}, packet) ==
        nq::CodecStatus::InvalidLength);
    soup[1] = std::byte{0x30};

    message.quantity = 0U;
    assert(nq::encode_enter_order(message, encoded).status ==
           nq::CodecStatus::InvalidField);
    assert(nq::encode_enter_order(
        nq::EnterOrder{}, std::span<std::byte>{encoded}.first(5U)).status ==
        nq::CodecStatus::BufferTooSmall);

    std::array<std::byte, nq::kOuchCancelOrderBytes> cancel{};
    assert(nq::encode_cancel_order({1U, 0U}, cancel).ok());
    const std::array<std::byte, nq::kOuchCancelOrderBytes> expected_cancel{
        b('X'), std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0},
    };
    assert(cancel == expected_cancel);

    std::array<std::byte, nq::kOuchModifyOrderBytes> modify{};
    assert(nq::encode_modify_order(
        {1U, nq::OuchSide::SellShort, 500U}, modify).ok());
    assert(modify[0] == b('M'));
    assert(modify[5] == b('T'));
    assert(modify[8] == std::byte{0x01});
    assert(modify[9] == std::byte{0xf4});

    nq::ReplaceOrder replace;
    replace.original_user_reference_number = 1U;
    assert(nq::make_planned_wire_identity(
        2U, "ABC124", inline_identity));
    assert(nq::parse_planned_wire_identity(
        inline_identity, replace.replacement_identity));
    replace.total_quantity = 1'000U;
    replace.price = 1'234'500U;
    std::array<std::byte, nq::kOuchReplaceOrderBytes> replaced{};
    assert(nq::encode_replace_order(replace, replaced).ok());
    assert(replaced[0] == b('U'));
    assert(replaced[4] == std::byte{1});
    assert(replaced[8] == std::byte{2});
    assert(replaced[38] == std::byte{0});
    assert(replaced[39] == std::byte{0});
}

void test_ouch_response_and_sequence() {
    std::array<std::byte, 64U> accepted{};
    accepted[0] = b('A');
    put_be(accepted, 1U, 123U, 8U);
    put_be(accepted, 9U, 7U, 4U);
    accepted[13] = b('B');
    put_be(accepted, 14U, 100U, 4U);
    put_chars(accepted, 18U, space_padded<8U>("MSFT"));
    put_be(accepted, 26U, 1'234'500U, 8U);
    accepted[34] = b('3');
    accepted[35] = b('Y');
    put_be(accepted, 36U, 99U, 8U);
    accepted[44] = b('A');
    accepted[45] = b('Y');
    accepted[46] = b('N');
    accepted[47] = b('L');
    put_chars(accepted, 48U, space_padded<14U>("ABC123"));

    nq::OrderResponse response;
    assert(nq::decode_order_response(accepted, response) == nq::CodecStatus::Ok);
    assert(response.kind == nq::ResponseKind::Accepted);
    assert(response.user_reference_number == 7U);
    assert(response.order_reference_number == 99U);

    std::array<std::byte, 67U> soup{};
    soup[0] = std::byte{0};
    soup[1] = std::byte{65};
    soup[2] = b('S');
    std::copy(accepted.begin(), accepted.end(), soup.begin() + 3);
    nq::SoupPacketView packet;
    assert(nq::decode_soup_packet(soup, packet) == nq::CodecStatus::Ok);
    nq::SoupSequenceTracker tracker{42U};
    std::uint64_t sequence{};
    assert(tracker.accept(packet, sequence) == nq::CodecStatus::Ok);
    assert(sequence == 42U);
    assert(tracker.next_sequence() == 43U);
    assert(nq::decode_order_response(packet.payload, response) ==
           nq::CodecStatus::Ok);

    std::array<std::byte, 31U> rejected{};
    rejected[0] = b('J');
    put_be(rejected, 1U, 124U, 8U);
    put_be(rejected, 9U, 8U, 4U);
    put_be(rejected, 13U, 1U, 2U);
    put_chars(rejected, 15U, space_padded<14U>("ABC124"));
    assert(nq::decode_order_response(rejected, response) == nq::CodecStatus::Ok);
    assert(response.kind == nq::ResponseKind::Rejected);
    assert(response.reject_reason == 1U);

    std::array<std::byte, 36U> executed{};
    executed[0] = b('E');
    put_be(executed, 1U, 125U, 8U);
    put_be(executed, 9U, 7U, 4U);
    put_be(executed, 13U, 10U, 4U);
    put_be(executed, 17U, 123'400U, 8U);
    executed[25] = b('A');
    put_be(executed, 26U, 77U, 8U);
    assert(nq::decode_order_response(executed, response) == nq::CodecStatus::Ok);
    assert(response.kind == nq::ResponseKind::Executed);
    assert(response.quantity == 10U);
    assert(response.match_number == 77U);
    executed[25] = b('7');
    assert(nq::decode_order_response(executed, response) == nq::CodecStatus::Ok);

    accepted[47] = b('Q');
    assert(nq::decode_order_response(accepted, response) ==
           nq::CodecStatus::InvalidField);
}

void test_boe_official_golden_vectors() {
    sorx::dataplane::InlineWireIdentity inline_identity;
    assert(cb::make_planned_wire_identity(100U, "ABC123", inline_identity));
    cb::PlannedWireIdentity identity;
    assert(cb::parse_planned_wire_identity(inline_identity, identity));

    cb::NewOrder message;
    message.identity = identity;
    message.side = cb::BoeSide::Buy;
    message.quantity = 1'000U;
    message.price = 1'234'500;
    message.symbol = nul_padded<8U>("MSFT");
    message.capacity = 'P';
    message.routing_instruction = nul_padded<4U>("R");
    message.account = nul_padded<16U>("DEFG");
    message.include_time_in_force = false;

    std::array<std::byte, 76U> encoded{};
    const auto result = cb::encode_new_order(message, encoded);
    assert(result.ok());
    const std::array<std::byte, 76U> expected{
        std::byte{0xba}, std::byte{0xba}, std::byte{0x4a}, std::byte{0},
        std::byte{0x38}, std::byte{0}, std::byte{0x64}, std::byte{0},
        std::byte{0}, std::byte{0},
        b('A'), b('B'), b('C'), b('1'), b('2'), b('3'),
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0},
        b('1'), std::byte{0xe8}, std::byte{0x03}, std::byte{0}, std::byte{0},
        std::byte{0x03}, std::byte{0x04}, std::byte{0xc1}, std::byte{0x01},
        std::byte{0x44}, std::byte{0xd6}, std::byte{0x12}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        b('M'), b('S'), b('F'), b('T'), std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, b('P'), b('R'), std::byte{0},
        std::byte{0}, std::byte{0}, b('D'), b('E'), b('F'), b('G'),
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
    };
    assert(encoded == expected);

    cb::ModifyOrder modify;
    modify.sequence_number = 100U;
    modify.client_order_id = nul_padded<20U>("ABC124");
    modify.original_client_order_id = nul_padded<20U>("ABC123");
    modify.quantity = 12'000U;
    modify.price = 123'400;
    std::array<std::byte, 64U> modified{};
    assert(cb::encode_modify_order(modify, modified).ok());
    assert(modified[0] == std::byte{0xba});
    assert(modified[2] == std::byte{0x3e});
    assert(modified[4] == std::byte{0x3a});
    assert(modified[50] == std::byte{1});
    assert(modified[51] == std::byte{0x0c});
    assert(modified[52] == std::byte{0xe0});
    assert(modified[53] == std::byte{0x2e});
    assert(modified[56] == std::byte{0x08});
    assert(modified[57] == std::byte{0xe2});
    assert(modified[58] == std::byte{0x01});

    cb::CancelOrder cancel;
    cancel.sequence_number = 101U;
    cancel.original_client_order_id = nul_padded<20U>("ABC123");
    std::array<std::byte, 31U> canceled{};
    assert(cb::encode_cancel_order(cancel, canceled).ok());
    assert(canceled[0] == std::byte{0xba});
    assert(canceled[2] == std::byte{0x1d});
    assert(canceled[4] == std::byte{0x39});
    assert(canceled[6] == std::byte{0x65});
    assert(canceled[30] == std::byte{0});

    std::array<std::byte, 10U> heartbeat{};
    assert(cb::encode_client_heartbeat(heartbeat).ok());
    const std::array<std::byte, 10U> expected_heartbeat{
        std::byte{0xba}, std::byte{0xba}, std::byte{0x08}, std::byte{0},
        std::byte{0x03}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0},
    };
    assert(heartbeat == expected_heartbeat);

    cb::OrderResponse heartbeat_response;
    auto server_heartbeat = heartbeat;
    server_heartbeat[4] = std::byte{0x09};
    assert(cb::decode_order_response(server_heartbeat, heartbeat_response) ==
           cb::CodecStatus::Ok);
    assert(heartbeat_response.kind == cb::ResponseKind::ServerHeartbeat);
}

void test_boe_response_sequence_and_malformed() {
    const auto clord = nul_padded<20U>("ABC123");
    std::array<std::byte, 48U> accepted{};
    accepted[0] = std::byte{0xba};
    accepted[1] = std::byte{0xba};
    put_le(accepted, 2U, 46U, 2U);
    accepted[4] = std::byte{0x25};
    accepted[5] = std::byte{3};
    put_le(accepted, 6U, 100U, 4U);
    put_le(accepted, 10U, 1'294'909'373'757'324'000ULL, 8U);
    put_chars(accepted, 18U, clord);
    put_le(accepted, 38U, 0x022f395eb71e1005ULL, 8U);

    cb::OrderResponse response;
    assert(cb::decode_order_response(accepted, response) == cb::CodecStatus::Ok);
    assert(response.kind == cb::ResponseKind::OrderAcknowledged);
    assert(response.matching_unit == 3U);
    assert(response.sequence_number == 100U);

    cb::UnitSequenceTracker tracker;
    assert(tracker.set_next(3U, 100U));
    assert(tracker.accept(response) == cb::CodecStatus::Ok);
    assert(tracker.next(3U) == 101U);
    assert(tracker.accept(response) == cb::CodecStatus::SequenceMismatch);

    accepted[47] = std::byte{1};
    assert(cb::decode_order_response(accepted, response) ==
           cb::CodecStatus::UnsupportedReturnBitfields);
    accepted[47] = std::byte{0};
    accepted[2] = std::byte{0x2f};
    assert(cb::decode_order_response(accepted, response) ==
           cb::CodecStatus::InvalidLength);

    std::array<std::byte, 101U> rejected{};
    rejected[0] = std::byte{0xba};
    rejected[1] = std::byte{0xba};
    put_le(rejected, 2U, 99U, 2U);
    rejected[4] = std::byte{0x26};
    put_le(rejected, 10U, 1'294'909'373'757'324'000ULL, 8U);
    put_chars(rejected, 18U, clord);
    rejected[38] = b('D');
    put_chars(rejected, 39U, nul_padded<60U>("Duplicate ClOrdID"));
    assert(cb::decode_order_response(rejected, response) == cb::CodecStatus::Ok);
    assert(response.kind == cb::ResponseKind::OrderRejected);
    assert(response.reason == 'D');
    assert(tracker.accept(response) == cb::CodecStatus::Ok);

    std::array<std::byte, 70U> execution{};
    execution[0] = std::byte{0xba};
    execution[1] = std::byte{0xba};
    put_le(execution, 2U, 68U, 2U);
    execution[4] = std::byte{0x2c};
    execution[5] = std::byte{3};
    put_le(execution, 6U, 101U, 4U);
    put_le(execution, 10U, 1'294'909'373'757'324'001ULL, 8U);
    put_chars(execution, 18U, clord);
    put_le(execution, 38U, 7U, 8U);
    put_le(execution, 46U, 100U, 4U);
    put_le(execution, 50U, 123'400U, 8U);
    put_le(execution, 58U, 20U, 4U);
    execution[62] = b('A');
    execution[63] = std::byte{0};
    put_chars(execution, 64U, nul_padded<4U>("BATS"));
    assert(cb::decode_order_response(execution, response) == cb::CodecStatus::Ok);
    assert(response.kind == cb::ResponseKind::OrderExecuted);
    assert(response.last_shares == 100U);
    assert(response.last_price == 123'400);
    assert(tracker.accept(response) == cb::CodecStatus::Ok);
    assert(tracker.next(3U) == 102U);
}

void test_child_adapters_are_bounded_and_allocation_free() {
    using namespace sorx;
    constexpr VenueId nasdaq_venue{1U};
    constexpr VenueId cboe_venue{2U};
    constexpr InstrumentId instrument{17U};

    nq::Ouch5ChildFrameEncoder ouch{nasdaq_venue, 'Y', 'A', 'Y'};
    assert(ouch.add_instrument({instrument, space_padded<8U>("MSFT")}));
    assert(ouch.seal());

    cb::BoeChildFrameEncoder boe{
        cboe_venue,
        'P',
        nul_padded<4U>("R"),
        nul_padded<4U>("P"),
        nul_padded<16U>("DEFG")};
    assert(boe.add_instrument({instrument, nul_padded<8U>("MSFT")}));
    assert(boe.seal());

    ParentOrder parent{
        ParentOrderId{9U},
        instrument,
        Side::Buy,
        Quantity{1'000U},
        Price{1'234'500},
        RoutingIntent::AggressiveIOC};
    ChildAllocation allocation;
    allocation.venue = nasdaq_venue;
    allocation.quantity = Quantity{1'000U};
    allocation.limit_price = Price{1'234'500};
    allocation.venue_quantity = Quantity{1'000U};
    allocation.venue_limit_price = Price{1'234'500};
    dataplane::PlannedChild child;
    child.child_order_id = ChildOrderId{51U};
    child.dispatch_attempt_id = 71U;
    child.venue_session_id = 1U;
    child.venue_session_epoch = 1U;
    child.ownership_generation = 1U;
    assert(nq::make_planned_wire_identity(100U, "N100", child.wire_identity));

    std::array<std::byte, fastpath::kMaximumEncodedFrameBytes> storage{};
    allocation_calls.store(0U, std::memory_order_relaxed);
    for (std::size_t index{}; index < 100U; ++index) {
        const auto encoded = ouch.encode_new(parent, allocation, child, storage);
        assert(encoded.encoded());
        assert(encoded.size == 50U);
        assert(storage[2] == b('U'));
        assert(storage[3] == b('O'));
    }
    assert(allocation_calls.load(std::memory_order_relaxed) == 0U);

    parent.intent = RoutingIntent::PassiveGTC;
    const auto passive_ouch = ouch.encode_new(parent, allocation, child, storage);
    assert(passive_ouch.encoded());
    assert(passive_ouch.size == 53U);
    assert(storage[48] == std::byte{0});
    assert(storage[49] == std::byte{3});
    assert(storage[50] == std::byte{2});
    assert(storage[51] == std::byte{12});
    assert(storage[52] == b('P'));

    allocation.venue = cboe_venue;
    parent.intent = RoutingIntent::AggressiveIOC;
    assert(cb::make_planned_wire_identity(200U, "C200", child.wire_identity));
    allocation_calls.store(0U, std::memory_order_relaxed);
    for (std::size_t index{}; index < 100U; ++index) {
        const auto encoded = boe.encode_new(parent, allocation, child, storage);
        assert(encoded.encoded());
        assert(encoded.size == 77U);
        assert(storage[0] == std::byte{0xba});
        assert(storage[4] == std::byte{0x38});
        assert(storage[6] == std::byte{0xc8});
    }
    assert(allocation_calls.load(std::memory_order_relaxed) == 0U);

    parent.intent = RoutingIntent::PassiveGTC;
    const auto passive_boe = boe.encode_new(parent, allocation, child, storage);
    assert(passive_boe.encoded());
    assert(storage[36] == std::byte{0x24});
    assert(storage[47] == b('1'));
    assert(storage[57] == b('P'));

    auto invalid = child;
    invalid.dispatch_attempt_id = 0U;
    assert(boe.encode_new(parent, allocation, invalid, storage).status ==
           fastpath::FrameEncodeStatus::InvalidInput);
}

void test_active_native_identity_collisions_are_rejected() {
    using namespace sorx;
    using namespace sorx::dataplane;

    const auto candidates = native_candidate_table();
    const CoreConfiguration configuration = native_configuration();
    const auto run = [&]<typename Parser>(
                         const VenueId venue,
                         const std::uint64_t session_id,
                         const std::uint64_t session_epoch,
                         const WireIdentityPlanner planner,
                         const std::uint8_t expected_identity_size,
                         Parser&& parses) {
        const std::array bindings{
            WireIdentityPlannerBinding{
                kNativeNasdaqVenue,
                venue == kNativeNasdaqVenue ? session_id : 1001U,
                venue == kNativeNasdaqVenue ? session_epoch : 7U,
                venue == kNativeNasdaqVenue
                    ? planner
                    : &nq::plan_route_wire_identity},
            WireIdentityPlannerBinding{
                kNativeCboeVenue,
                venue == kNativeCboeVenue ? session_id : 2001U,
                venue == kNativeCboeVenue ? session_epoch : 8U,
                venue == kNativeCboeVenue
                    ? planner
                    : &cb::plan_route_wire_identity},
        };
        RouteKernel kernel(
            configuration,
            *candidates,
            AdaptivePolicyMode::Optional,
            WireIdentityMode::RequireRegistered,
            bindings);
        const std::array views{
            native_view(venue, Price{100}, Quantity{10U})};
        const PlanResult first = kernel.plan(
            native_parent(801U), views, 1'050U);
        assert(first.accepted());
        assert(first.plan.allocation_count == 1U);
        assert(first.plan.allocations[0].venue == venue);
        assert(first.plan.planned_children[0].wire_identity.size ==
               expected_identity_size);
        assert(parses(first.plan.planned_children[0].wire_identity));
        assert(kernel.commit(first.plan, views, 1'060U).committed());

        const PlanResult collision = kernel.plan(
            native_parent(802U), views, 1'070U);
        assert(collision.status == PlanStatus::WireIdentityRejected);
        assert(collision.wire_identity_status ==
               WireIdentityPlanStatus::InvalidOutput);
        assert(kernel.active_parent_count() == 1U);
    };

    run(
        kNativeNasdaqVenue,
        1001U,
        7U,
        &colliding_ouch_identity,
        static_cast<std::uint8_t>(nq::kOuchPlannedWireIdentityBytes),
        [](const InlineWireIdentity& identity) noexcept {
            nq::PlannedWireIdentity parsed;
            return nq::parse_planned_wire_identity(identity, parsed);
        });
    run(
        kNativeCboeVenue,
        2001U,
        8U,
        &colliding_boe_identity,
        static_cast<std::uint8_t>(cb::kBoePlannedWireIdentityBytes),
        [](const InlineWireIdentity& identity) noexcept {
            cb::PlannedWireIdentity parsed;
            return cb::parse_planned_wire_identity(identity, parsed);
        });
}

void test_native_identity_sequence_exhaustion_fails_closed() {
    using namespace sorx;
    using namespace sorx::dataplane;

    const std::uint64_t maximum_sequence =
        std::numeric_limits<std::uint32_t>::max();
    InlineWireIdentity identity;
    WireIdentityPlanRequest request{
        kNativeNasdaqVenue,
        ChildOrderId{1U},
        maximum_sequence,
        1001U,
        7U,
    };
    assert(nq::plan_route_wire_identity(request, identity) ==
           WireIdentityPlanStatus::Planned);
    nq::PlannedWireIdentity ouch;
    assert(nq::parse_planned_wire_identity(identity, ouch));
    assert(ouch.user_reference_number ==
           std::numeric_limits<std::uint32_t>::max());
    request.dispatch_attempt_id = maximum_sequence + 1U;
    assert(nq::plan_route_wire_identity(request, identity) ==
           WireIdentityPlanStatus::SequenceExhausted);

    request.venue = kNativeCboeVenue;
    request.venue_session_id = 2001U;
    request.venue_session_epoch = 8U;
    request.dispatch_attempt_id = maximum_sequence;
    assert(cb::plan_route_wire_identity(request, identity) ==
           WireIdentityPlanStatus::Planned);
    cb::PlannedWireIdentity boe;
    assert(cb::parse_planned_wire_identity(identity, boe));
    assert(boe.sequence_number ==
           std::numeric_limits<std::uint32_t>::max());
    request.dispatch_attempt_id = maximum_sequence + 1U;
    assert(cb::plan_route_wire_identity(request, identity) ==
           WireIdentityPlanStatus::SequenceExhausted);
}

[[nodiscard]] sorx::fastpath::CancelCommand native_cancel_command(
    const sorx::fastpath::ActiveChildSnapshot& child,
    const std::uint64_t request_id,
    const std::uint64_t sequence,
    const sorx::fastpath::CancelRequestKind kind =
        sorx::fastpath::CancelRequestKind::Cancel) noexcept {
    sorx::fastpath::CancelCommand output;
    output.client_request_id = request_id;
    output.client_sequence = sequence;
    output.client_session_epoch = 9U;
    output.account_context_id = kNativeAccountContext;
    output.parent_order_id = child.parent_order_id;
    output.child_id = child.child_id;
    output.venue_id = child.venue_id;
    output.expected_ownership_generation = child.ownership_generation;
    output.expected_venue_session_id = child.venue_session_id;
    output.expected_venue_session_epoch = child.venue_session_epoch;
    output.reason_code = 17U;
    output.kind = kind;
    assert(output.valid());
    return output;
}

void assert_same_native_ownership(
    const sorx::fastpath::ActiveChildSnapshot& left,
    const sorx::fastpath::ActiveChildSnapshot& right) {
    assert(left.account_context_id == right.account_context_id);
    assert(left.parent_order_id == right.parent_order_id);
    assert(left.child_id == right.child_id);
    assert(left.venue_id == right.venue_id);
    assert(left.venue_session_id == right.venue_session_id);
    assert(left.venue_session_epoch == right.venue_session_epoch);
    assert(left.ownership_generation == right.ownership_generation);
    assert(left.last_dispatch_attempt_id == right.last_dispatch_attempt_id);
    assert(left.wire_identity == right.wire_identity);
    assert(left.state == right.state);
    assert(left.pending_client_request_id == right.pending_client_request_id);
    assert(left.pending_client_sequence == right.pending_client_sequence);
    assert(left.pending_client_session_epoch ==
           right.pending_client_session_epoch);
    assert(left.pending_cancel_attempt_id == right.pending_cancel_attempt_id);
}

[[nodiscard]] sorx::recovery::RecoveryRecord native_cancel_record(
    const sorx::fastpath::CancelCommand& command,
    const sorx::fastpath::ActiveChildSnapshot& before,
    const std::uint64_t cancel_attempt,
    const std::uint64_t source_sequence,
    const std::uint8_t digest_seed) noexcept {
    sorx::recovery::RecoveryRecord output;
    output.type = sorx::recovery::RecoveryRecordType::CancelIntent;
    output.source_sequence = source_sequence;
    output.client_id = kNativeAccountContext;
    output.client_sequence = command.client_sequence;
    output.request_digest = native_digest(digest_seed);
    output.parent = command.parent_order_id;
    output.child = command.child_id;
    output.dispatch_attempt_id = cancel_attempt;
    output.prior_dispatch_attempt_id = before.last_dispatch_attempt_id;
    output.client_request_id = command.client_request_id;
    output.client_session_epoch = command.client_session_epoch;
    return output;
}

void test_native_dispatch_cancel_and_checkpoint_restart() {
    using namespace sorx;
    using namespace sorx::dataplane;
    using namespace sorx::fastpath;
    using namespace sorx::recovery;

    static_assert(kDispatchRecordVersion == 3U);
    static_assert(kRecoveryCheckpointVersion == 2U);

    const auto candidates = native_candidate_table();
    const CoreConfiguration configuration = native_configuration();
    const std::array identity_bindings{
        WireIdentityPlannerBinding{
            kNativeNasdaqVenue,
            1001U,
            7U,
            &nq::plan_route_wire_identity},
        WireIdentityPlannerBinding{
            kNativeCboeVenue,
            2001U,
            8U,
            &cb::plan_route_wire_identity},
    };

    RouteKernel kernel(
        configuration,
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::RequireRegistered,
        identity_bindings);
    nq::Ouch5ChildFrameEncoder ouch{
        kNativeNasdaqVenue, 'Y', 'A', 'Y'};
    assert(ouch.add_instrument({
        kNativeInstrument, space_padded<8U>("MSFT")}));
    assert(ouch.seal());
    cb::BoeChildFrameEncoder boe{
        kNativeCboeVenue,
        'P',
        nul_padded<4U>("R"),
        nul_padded<4U>("P"),
        nul_padded<16U>("ACCOUNT")};
    assert(boe.add_instrument({
        kNativeInstrument, nul_padded<8U>("MSFT")}));
    assert(boe.seal());

    VenueTxQueues nasdaq_queues;
    VenueTxQueues cboe_queues;
    FixedVenueRegistry dispatch_venues;
    assert(dispatch_venues.register_venue(
               kNativeNasdaqVenue, nasdaq_queues, ouch) ==
           VenueRegistrationStatus::Registered);
    assert(dispatch_venues.register_venue(
               kNativeCboeVenue, cboe_queues, boe) ==
           VenueRegistrationStatus::Registered);
    assert(dispatch_venues.seal());

    recorder::InMemoryMirroredTier0 tier0({16U, 4U, 4U, false});
    const DispatchPipelinePolicy policy{
        kNativeLeadershipEpoch,
        kNativeConfigurationGeneration,
        recorder::DurabilityDomain::VolatileSoftwareReference};
    DispatchPipeline dispatch(kernel, tier0, dispatch_venues, policy);
    const std::array views{
        native_view(kNativeNasdaqVenue, Price{100}, Quantity{4U}),
        native_view(kNativeCboeVenue, Price{101}, Quantity{6U}),
    };
    const std::uint64_t allocations_before =
        allocation_calls.load(std::memory_order_relaxed);
    const DispatchPipelineResult dispatched = dispatch.try_dispatch(
        native_parent(901U),
        views,
        DispatchRequestMetadata{1U, 1'050U, 1'060U, 1'055U});
    assert(allocation_calls.load(std::memory_order_relaxed) ==
           allocations_before);
    assert(dispatched.dispatched());
    assert(dispatched.child_count == 2U);
    assert(dispatched.token.dispatch_authorization.first_child_id == 1U);
    assert(dispatched.token.dispatch_authorization.first_attempt_id ==
           1U);

    RouteKernelActiveChildOwnership ownership{
        kernel, kNativeAccountContext};
    const OwnershipLookupResult first_lookup =
        ownership.lookup(ChildOrderId{1U});
    const OwnershipLookupResult second_lookup =
        ownership.lookup(ChildOrderId{2U});
    assert(first_lookup.status == OwnershipLookupStatus::Found);
    assert(second_lookup.status == OwnershipLookupStatus::Found);
    const ActiveChildSnapshot nasdaq_child =
        first_lookup.snapshot.venue_id == kNativeNasdaqVenue
        ? first_lookup.snapshot
        : second_lookup.snapshot;
    const ActiveChildSnapshot cboe_child =
        first_lookup.snapshot.venue_id == kNativeCboeVenue
        ? first_lookup.snapshot
        : second_lookup.snapshot;
    assert(nasdaq_child.venue_id == kNativeNasdaqVenue);
    assert(cboe_child.venue_id == kNativeCboeVenue);
    assert(nasdaq_child.venue_session_id == 1001U);
    assert(nasdaq_child.venue_session_epoch == 7U);
    assert(cboe_child.venue_session_id == 2001U);
    assert(cboe_child.venue_session_epoch == 8U);
    nq::PlannedWireIdentity nasdaq_identity;
    cb::PlannedWireIdentity cboe_identity;
    assert(nq::parse_planned_wire_identity(
        nasdaq_child.wire_identity, nasdaq_identity));
    assert(cb::parse_planned_wire_identity(
        cboe_child.wire_identity, cboe_identity));

    AuthorizedFrame nasdaq_new;
    AuthorizedFrame cboe_new;
    assert(nasdaq_queues.try_pop_normal(nasdaq_new));
    assert(cboe_queues.try_pop_normal(cboe_new));
    const auto assert_exact_permit = [&tier0](
        const AuthorizedFrame& authorized) {
        const EncodedFrame& frame = authorized.frame;
        assert(authorized.permit.authorizes_external_effect(
            frame.venue_id,
            frame.child_id,
            frame.dispatch_attempt_id,
            frame.venue_session_id,
            frame.venue_session_epoch,
            frame.ownership_generation,
            static_cast<std::uint8_t>(frame.dispatch_class),
            frame.bytes()));
        assert(tier0.authorizes_external_effect(
            authorized.permit,
            recorder::DurabilityDomain::VolatileSoftwareReference));
    };
    assert_exact_permit(nasdaq_new);
    assert_exact_permit(cboe_new);
    assert(nasdaq_new.frame.child_id == nasdaq_child.child_id.value());
    assert(cboe_new.frame.child_id == cboe_child.child_id.value());
    assert(nasdaq_new.frame.storage[2] == b('U'));
    assert(nasdaq_new.frame.storage[3] == b('O'));
    assert(std::equal(
        nasdaq_child.wire_identity.bytes.begin(),
        nasdaq_child.wire_identity.bytes.begin() + 4U,
        nasdaq_new.frame.storage.begin() + 4U));
    assert(std::equal(
        nasdaq_child.wire_identity.bytes.begin() + 4U,
        nasdaq_child.wire_identity.bytes.begin() +
            nq::kOuchPlannedWireIdentityBytes,
        nasdaq_new.frame.storage.begin() + 34U));
    assert(cboe_new.frame.storage[4] == std::byte{0x38U});
    assert(std::equal(
        cboe_child.wire_identity.bytes.begin(),
        cboe_child.wire_identity.bytes.begin() + 4U,
        cboe_new.frame.storage.begin() + 6U));
    assert(std::equal(
        cboe_child.wire_identity.bytes.begin() + 4U,
        cboe_child.wire_identity.bytes.begin() +
            cb::kBoePlannedWireIdentityBytes,
        cboe_new.frame.storage.begin() + 10U));

    VenueCancelQueueAdapter nasdaq_cancel_queue{nasdaq_queues};
    VenueCancelQueueAdapter cboe_cancel_queue{cboe_queues};
    FixedCancelVenueRegistry cancel_venues;
    assert(cancel_venues.bind({
        kNativeNasdaqVenue, &nasdaq_cancel_queue, &ouch}));
    assert(cancel_venues.bind({
        kNativeCboeVenue, &cboe_cancel_queue, &boe}));
    cancel_venues.seal();
    BoundedDispatchAttemptSource attempts{
        kernel.recovery_identifiers().next_dispatch_attempt_id,
        std::numeric_limits<std::uint32_t>::max()};
    CancelPipeline cancels{
        tier0, ownership, attempts, cancel_venues, policy};
    const CancelCommand nasdaq_command = native_cancel_command(
        nasdaq_child, 501U, 1U);
    const CancelCommand cboe_command = native_cancel_command(
        cboe_child, 502U, 2U);
    const CancelPipelineResult nasdaq_canceled = cancels.try_cancel(
        nasdaq_command, CancelRequestMetadata{2U, 1'100U});
    const CancelPipelineResult cboe_canceled = cancels.try_cancel(
        cboe_command, CancelRequestMetadata{3U, 1'110U});
    assert(nasdaq_canceled.enqueued());
    assert(cboe_canceled.enqueued());
    assert(nasdaq_canceled.dispatch_attempt_id == 3U);
    assert(cboe_canceled.dispatch_attempt_id == 4U);

    AuthorizedFrame nasdaq_cancel;
    AuthorizedFrame cboe_cancel;
    assert(nasdaq_queues.try_pop_cancel(nasdaq_cancel));
    assert(cboe_queues.try_pop_cancel(cboe_cancel));
    assert_exact_permit(nasdaq_cancel);
    assert_exact_permit(cboe_cancel);
    assert(nasdaq_cancel.frame.storage[2] == b('U'));
    assert(nasdaq_cancel.frame.storage[3] == b('X'));
    assert(std::equal(
        nasdaq_child.wire_identity.bytes.begin(),
        nasdaq_child.wire_identity.bytes.begin() + 4U,
        nasdaq_cancel.frame.storage.begin() + 4U));
    assert(cboe_cancel.frame.storage[4] == std::byte{0x39U});
    assert(get_le_u32(cboe_cancel.frame.bytes(), 6U) ==
           cboe_canceled.dispatch_attempt_id);
    assert(std::equal(
        cboe_child.wire_identity.bytes.begin() + 4U,
        cboe_child.wire_identity.bytes.begin() +
            cb::kBoePlannedWireIdentityBytes,
        cboe_cancel.frame.storage.begin() + 10U));

    const OwnershipLookupResult live_nasdaq_pending =
        ownership.lookup(nasdaq_child.child_id);
    const OwnershipLookupResult live_cboe_pending =
        ownership.lookup(cboe_child.child_id);
    assert(live_nasdaq_pending.status == OwnershipLookupStatus::Found);
    assert(live_cboe_pending.status == OwnershipLookupStatus::Found);
    assert(live_nasdaq_pending.snapshot.state ==
           ActiveChildState::CancelPending);
    assert(live_cboe_pending.snapshot.state ==
           ActiveChildState::CancelPending);
    assert(live_nasdaq_pending.snapshot.wire_identity ==
           nasdaq_child.wire_identity);
    assert(live_cboe_pending.snapshot.wire_identity ==
           cboe_child.wire_identity);

    recorder::DurableBatch dispatch_batch;
    recorder::DurableToken dispatch_token;
    assert(tier0.read(
               dispatched.token.batch_id,
               dispatch_batch,
               dispatch_token) == recorder::ReadStatus::Ok);
    RouteKernel recovered_kernel(
        configuration,
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::RequireRegistered,
        identity_bindings);
    RecoveryLedger recovered{
        recovered_kernel, native_recovery_configuration(*candidates)};
    assert(recovered.replay_dispatch_batch(
               dispatch_token, dispatch_batch).status ==
           DispatchReplayStatus::Applied);
    assert(recovered.apply_record(native_cancel_record(
               nasdaq_command,
               nasdaq_child,
               nasdaq_canceled.dispatch_attempt_id,
               1U,
               11U)).status == RecoveryRecordStatus::Applied);
    assert(recovered.apply_record(native_cancel_record(
               cboe_command,
               cboe_child,
               cboe_canceled.dispatch_attempt_id,
               2U,
               21U)).status == RecoveryRecordStatus::Applied);
    assert(recovered_kernel.note_recovered_cancel(
               nasdaq_command.parent_order_id,
               nasdaq_command.child_id,
               nasdaq_child.last_dispatch_attempt_id,
               nasdaq_canceled.dispatch_attempt_id,
               nasdaq_command.client_request_id,
               nasdaq_command.client_sequence,
               nasdaq_command.client_session_epoch) ==
           RecoveryAccountingStatus::AlreadyApplied);
    assert(recovered_kernel.note_recovered_cancel(
               nasdaq_command.parent_order_id,
               nasdaq_command.child_id,
               nasdaq_child.last_dispatch_attempt_id,
               nasdaq_canceled.dispatch_attempt_id,
               nasdaq_command.client_request_id + 1U,
               nasdaq_command.client_sequence,
               nasdaq_command.client_session_epoch) ==
           RecoveryAccountingStatus::InvariantViolation);
    RouteKernelActiveChildOwnership recovered_ownership{
        recovered_kernel, kNativeAccountContext};
    const OwnershipLookupResult recovered_nasdaq =
        recovered_ownership.lookup(nasdaq_child.child_id);
    const OwnershipLookupResult recovered_cboe =
        recovered_ownership.lookup(cboe_child.child_id);
    assert(recovered_nasdaq.status == OwnershipLookupStatus::Found);
    assert(recovered_cboe.status == OwnershipLookupStatus::Found);
    assert_same_native_ownership(
        recovered_nasdaq.snapshot, live_nasdaq_pending.snapshot);
    assert_same_native_ownership(
        recovered_cboe.snapshot, live_cboe_pending.snapshot);

    std::vector<std::byte> checkpoint(recovered.checkpoint_size_required());
    const CheckpointEncodeResult checkpoint_encoded =
        recovered.encode_checkpoint(checkpoint);
    assert(checkpoint_encoded.status == CheckpointEncodeStatus::Encoded);
    checkpoint.resize(checkpoint_encoded.encoded_size);

    RouteKernel restarted_kernel(
        configuration,
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::RequireRegistered,
        identity_bindings);
    RecoveryLedger restarted{
        restarted_kernel, native_recovery_configuration(*candidates)};
    assert(restarted.restore_checkpoint(checkpoint).status ==
           CheckpointRestoreStatus::Restored);
    RouteKernelActiveChildOwnership restarted_ownership{
        restarted_kernel, kNativeAccountContext};
    const OwnershipLookupResult restarted_nasdaq =
        restarted_ownership.lookup(nasdaq_child.child_id);
    const OwnershipLookupResult restarted_cboe =
        restarted_ownership.lookup(cboe_child.child_id);
    assert(restarted_nasdaq.status == OwnershipLookupStatus::Found);
    assert(restarted_cboe.status == OwnershipLookupStatus::Found);
    assert_same_native_ownership(
        restarted_nasdaq.snapshot, live_nasdaq_pending.snapshot);
    assert_same_native_ownership(
        restarted_cboe.snapshot, live_cboe_pending.snapshot);

    std::array<std::byte, kMaximumEncodedFrameBytes> restarted_frame{};
    const CancelCommand nasdaq_retry = native_cancel_command(
        restarted_nasdaq.snapshot,
        601U,
        3U,
        CancelRequestKind::Retry);
    const std::uint64_t final_allocations_before =
        allocation_calls.load(std::memory_order_relaxed);
    const FrameEncodeResult restarted_ouch = ouch.encode_cancel(
        nasdaq_retry,
        restarted_nasdaq.snapshot,
        restarted_nasdaq.snapshot.last_dispatch_attempt_id + 1U,
        restarted_frame);
    assert(restarted_ouch.encoded());
    assert(std::equal(
        restarted_nasdaq.snapshot.wire_identity.bytes.begin(),
        restarted_nasdaq.snapshot.wire_identity.bytes.begin() + 4U,
        restarted_frame.begin() + 4U));
    const CancelCommand cboe_retry = native_cancel_command(
        restarted_cboe.snapshot,
        602U,
        4U,
        CancelRequestKind::Retry);
    const std::uint64_t restarted_boe_attempt =
        restarted_cboe.snapshot.last_dispatch_attempt_id + 1U;
    const FrameEncodeResult restarted_boe = boe.encode_cancel(
        cboe_retry,
        restarted_cboe.snapshot,
        restarted_boe_attempt,
        restarted_frame);
    assert(restarted_boe.encoded());
    assert(get_le_u32(
               std::span<const std::byte>{
                   restarted_frame.data(), restarted_boe.size},
               6U) == restarted_boe_attempt);
    assert(std::equal(
        restarted_cboe.snapshot.wire_identity.bytes.begin() + 4U,
        restarted_cboe.snapshot.wire_identity.bytes.begin() +
            cb::kBoePlannedWireIdentityBytes,
        restarted_frame.begin() + 10U));
    assert(allocation_calls.load(std::memory_order_relaxed) ==
           final_allocations_before);
}

}  // namespace

int main() {
    static_assert(
        nq::kOuch5Capabilities.certification ==
        nq::CertificationStatus::Uncertified);
    static_assert(
        cb::kBoeCapabilities.certification ==
        cb::CertificationStatus::Uncertified);
    static_assert(noexcept(nq::encode_cancel_order(
        nq::CancelOrder{}, std::span<std::byte>{})));
    static_assert(noexcept(cb::encode_client_heartbeat(
        std::span<std::byte>{})));

    test_ouch_golden_and_malformed();
    test_ouch_response_and_sequence();
    test_boe_official_golden_vectors();
    test_boe_response_sequence_and_malformed();
    test_child_adapters_are_bounded_and_allocation_free();
    test_active_native_identity_collisions_are_rejected();
    test_native_identity_sequence_exhaustion_fails_closed();
    test_native_dispatch_cancel_and_checkpoint_restart();
    return 0;
}
