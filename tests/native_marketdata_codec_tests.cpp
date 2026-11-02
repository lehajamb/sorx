#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/connectors/cboe_pitch.hpp"
#include "sorx/connectors/nasdaq_itch5.hpp"
#include "sorx/marketdata/fixed_l3_book.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> count{0U};

void record() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t size) {
    record();
    if (void* const result = std::malloc(size == 0U ? 1U : size)) {
        return result;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t size,
    const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* const result =
            _aligned_malloc(size == 0U ? 1U : size, alignment)) {
        return result;
    }
#else
    const std::size_t nonzero = size == 0U ? 1U : size;
    const std::size_t rounded =
        ((nonzero + alignment - 1U) / alignment) * alignment;
    if (void* const result = std::aligned_alloc(alignment, rounded)) {
        return result;
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
        count.store(0U, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    [[nodiscard]] std::size_t allocations() const noexcept {
        return count.load(std::memory_order_relaxed);
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
void* operator new(
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::align_val_t) noexcept {
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

using Bytes = std::vector<std::byte>;
using namespace sorx;
namespace nmd = sorx::connectors::native_md;
namespace itch = sorx::connectors::nasdaq::itch5;
namespace pitch = sorx::connectors::cboe::pitch;

void append_u8(Bytes& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void append_be16(Bytes& bytes, const std::uint16_t value) {
    append_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
    append_u8(bytes, static_cast<std::uint8_t>(value));
}

void append_be64(Bytes& bytes, const std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        append_u8(
            bytes,
            static_cast<std::uint8_t>(
                value >> static_cast<unsigned>((7U - index) * 8U)));
    }
}

void append_le16(Bytes& bytes, const std::uint16_t value) {
    append_u8(bytes, static_cast<std::uint8_t>(value));
    append_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
}

void append_le32(Bytes& bytes, const std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        append_u8(
            bytes,
            static_cast<std::uint8_t>(
                value >> static_cast<unsigned>(index * 8U)));
    }
}

void put_be16(Bytes& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 1U] = static_cast<std::byte>(value);
}

void put_be32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            value >> static_cast<unsigned>((3U - index) * 8U));
    }
}

void put_be48(Bytes& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t index = 0U; index < 6U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            value >> static_cast<unsigned>((5U - index) * 8U));
    }
}

void put_be64(Bytes& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            value >> static_cast<unsigned>((7U - index) * 8U));
    }
}

void put_le16(Bytes& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void put_le32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void put_le64(Bytes& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

[[nodiscard]] nmd::Symbol8 symbol8(const std::string_view text) {
    nmd::Symbol8 result{};
    result.fill(' ');
    assert(text.size() <= result.size());
    std::copy(text.begin(), text.end(), result.begin());
    return result;
}

[[nodiscard]] Bytes itch_message(
    const char type,
    const std::size_t length,
    const std::uint16_t locate,
    const std::uint64_t timestamp) {
    Bytes message(length, std::byte{0U});
    message[0] = static_cast<std::byte>(static_cast<std::uint8_t>(type));
    put_be16(message, 1U, locate);
    put_be48(message, 5U, timestamp);
    return message;
}

void put_ascii(
    Bytes& bytes,
    const std::size_t offset,
    const std::string_view text,
    const std::size_t width) {
    assert(text.size() <= width);
    for (std::size_t index = 0U; index < width; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<std::uint8_t>(
                index < text.size() ? text[index] : ' '));
    }
}

[[nodiscard]] Bytes stock_directory(
    const std::uint16_t locate,
    const std::string_view symbol,
    const char category,
    const char ipo_flag) {
    auto message = itch_message('R', 39U, locate, 100U);
    put_ascii(message, 11U, symbol, 8U);
    message[19] = static_cast<std::byte>(static_cast<std::uint8_t>(category));
    message[20] = std::byte{'N'};
    put_be32(message, 21U, 100U);
    message[25] = std::byte{'N'};
    message[26] = std::byte{'C'};
    message[27] = std::byte{' '};
    message[28] = std::byte{' '};
    message[29] = std::byte{'P'};
    message[30] = std::byte{'N'};
    message[31] = static_cast<std::byte>(static_cast<std::uint8_t>(ipo_flag));
    message[32] = std::byte{'1'};
    message[33] = std::byte{'N'};
    put_be32(message, 34U, 1U);
    message[38] = std::byte{'N'};
    return message;
}

[[nodiscard]] Bytes itch_add(
    const std::uint16_t locate,
    const std::uint64_t reference,
    const std::uint32_t quantity,
    const std::uint32_t price) {
    auto message = itch_message('A', 36U, locate, 101U);
    put_be64(message, 11U, reference);
    message[19] = std::byte{'B'};
    put_be32(message, 20U, quantity);
    put_ascii(message, 24U, "TEST", 8U);
    put_be32(message, 32U, price);
    return message;
}

[[nodiscard]] Bytes itch_execute(
    const std::uint16_t locate,
    const std::uint64_t reference,
    const std::uint32_t quantity,
    const std::uint64_t match) {
    auto message = itch_message('E', 31U, locate, 102U);
    put_be64(message, 11U, reference);
    put_be32(message, 19U, quantity);
    put_be64(message, 23U, match);
    return message;
}

[[nodiscard]] Bytes itch_cancel(
    const std::uint16_t locate,
    const std::uint64_t reference,
    const std::uint32_t quantity) {
    auto message = itch_message('X', 23U, locate, 103U);
    put_be64(message, 11U, reference);
    put_be32(message, 19U, quantity);
    return message;
}

[[nodiscard]] Bytes itch_replace(
    const std::uint16_t locate,
    const std::uint64_t old_reference,
    const std::uint64_t new_reference,
    const std::uint32_t quantity,
    const std::uint32_t price) {
    auto message = itch_message('U', 35U, locate, 103U);
    put_be64(message, 11U, old_reference);
    put_be64(message, 19U, new_reference);
    put_be32(message, 27U, quantity);
    put_be32(message, 31U, price);
    return message;
}

[[nodiscard]] Bytes itch_trade_control(
    const std::uint16_t locate,
    const std::uint64_t match) {
    auto message = itch_message('P', 44U, locate, 104U);
    put_be64(message, 11U, 0U);
    message[19] = std::byte{'B'};
    put_be32(message, 20U, 10U);
    put_ascii(message, 24U, "TEST", 8U);
    put_be32(message, 32U, 123'450U);
    put_be64(message, 36U, match);
    return message;
}

[[nodiscard]] Bytes itch_broken_control(
    const std::uint16_t locate,
    const std::uint64_t match) {
    auto message = itch_message('B', 19U, locate, 105U);
    put_be64(message, 11U, match);
    return message;
}

[[nodiscard]] Bytes itch_zero_share_cross(
    const std::uint16_t locate,
    const std::uint64_t match) {
    auto message = itch_message('Q', 40U, locate, 107U);
    put_be64(message, 11U, 0U);
    put_ascii(message, 19U, "TEST", 8U);
    put_be32(message, 27U, 123'450U);
    put_be64(message, 31U, match);
    message[39] = std::byte{'I'};
    return message;
}

[[nodiscard]] Bytes itch_trading_status(const std::uint16_t locate) {
    auto message = itch_message('H', 25U, locate, 106U);
    put_ascii(message, 11U, "TEST", 8U);
    message[19] = std::byte{'T'};
    message[20] = std::byte{' '};
    put_ascii(message, 21U, "    ", 4U);
    return message;
}

[[nodiscard]] Bytes mold_packet(
    const itch::MoldSession& session,
    const std::uint64_t first_sequence,
    const std::span<const Bytes> messages,
    const std::uint16_t special_count = 0U) {
    Bytes packet;
    packet.reserve(itch::kMoldHeaderBytes + 256U);
    for (const auto value : session) {
        append_u8(packet, static_cast<std::uint8_t>(value));
    }
    append_be64(packet, first_sequence);
    append_be16(
        packet,
        special_count == 0U
            ? static_cast<std::uint16_t>(messages.size())
            : special_count);
    for (const auto& message : messages) {
        append_be16(packet, static_cast<std::uint16_t>(message.size()));
        packet.insert(packet.end(), message.begin(), message.end());
    }
    return packet;
}

[[nodiscard]] Bytes pitch_time(const std::uint32_t seconds) {
    Bytes message{std::byte{6U}, std::byte{0x20U}};
    append_le32(message, seconds);
    return message;
}

// Literal Add Order Long example bytes published in the Cboe PITCH spec.
[[nodiscard]] Bytes cboe_official_add_example() {
    constexpr std::array<std::uint8_t, 34U> vector{
        0x22U, 0x21U, 0x18U, 0xD2U, 0x06U, 0x00U,
        0x05U, 0x40U, 0x5BU, 0x77U, 0x8FU, 0x56U, 0x1DU, 0x0BU,
        0x42U, 0x20U, 0x4EU, 0x00U, 0x00U,
        0x5AU, 0x56U, 0x5AU, 0x5AU, 0x54U, 0x20U,
        0x5AU, 0x23U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U,
    };
    Bytes result;
    result.reserve(vector.size());
    for (const auto byte : vector) {
        append_u8(result, byte);
    }
    return result;
}

[[nodiscard]] Bytes pitch_add(
    const std::uint64_t reference,
    const std::uint32_t quantity,
    const std::uint64_t price) {
    Bytes message(34U, std::byte{0U});
    message[0] = std::byte{34U};
    message[1] = std::byte{0x21U};
    put_le32(message, 2U, 500U);
    put_le64(message, 6U, reference);
    message[14] = std::byte{'B'};
    put_le32(message, 15U, quantity);
    put_ascii(message, 19U, "ZVZZT", 6U);
    put_le64(message, 25U, price);
    message[33] = std::byte{1U};
    return message;
}

[[nodiscard]] Bytes pitch_execute(
    const std::uint64_t reference,
    const std::uint32_t quantity,
    const std::uint64_t execution) {
    Bytes message(26U, std::byte{0U});
    message[0] = std::byte{26U};
    message[1] = std::byte{0x23U};
    put_le32(message, 2U, 700U);
    put_le64(message, 6U, reference);
    put_le32(message, 14U, quantity);
    put_le64(message, 18U, execution);
    return message;
}

[[nodiscard]] Bytes pitch_modify(
    const std::uint64_t reference,
    const std::uint32_t quantity,
    const std::uint64_t price,
    const std::uint8_t flags) {
    Bytes message(27U, std::byte{0U});
    message[0] = std::byte{27U};
    message[1] = std::byte{0x27U};
    put_le32(message, 2U, 800U);
    put_le64(message, 6U, reference);
    put_le32(message, 14U, quantity);
    put_le64(message, 18U, price);
    message[26] = static_cast<std::byte>(flags);
    return message;
}

[[nodiscard]] Bytes pitch_execute_at_price(
    const std::uint64_t reference,
    const std::uint32_t executed,
    const std::uint32_t remaining,
    const std::uint64_t execution,
    const std::uint64_t price) {
    Bytes message(38U, std::byte{0U});
    message[0] = std::byte{38U};
    message[1] = std::byte{0x24U};
    put_le32(message, 2U, 900U);
    put_le64(message, 6U, reference);
    put_le32(message, 14U, executed);
    put_le32(message, 18U, remaining);
    put_le64(message, 22U, execution);
    put_le64(message, 30U, price);
    return message;
}

[[nodiscard]] Bytes pitch_trade(
    const std::uint64_t reference,
    const std::uint64_t execution) {
    Bytes message(41U, std::byte{0U});
    message[0] = std::byte{41U};
    message[1] = std::byte{0x2AU};
    put_le32(message, 2U, 1'000U);
    put_le64(message, 6U, reference);
    message[14] = std::byte{'B'};
    put_le32(message, 15U, 10U);
    put_ascii(message, 19U, "ZVZZT", 6U);
    put_le64(message, 25U, 9'100U);
    put_le64(message, 33U, execution);
    return message;
}

[[nodiscard]] Bytes pitch_break(const std::uint64_t execution) {
    Bytes message(14U, std::byte{0U});
    message[0] = std::byte{14U};
    message[1] = std::byte{0x2CU};
    put_le32(message, 2U, 1'100U);
    put_le64(message, 6U, execution);
    return message;
}

[[nodiscard]] Bytes pitch_status(const char status) {
    Bytes message(18U, std::byte{' '});
    message[0] = std::byte{18U};
    message[1] = std::byte{0x31U};
    put_le32(message, 2U, 1'200U);
    put_ascii(message, 6U, "ZVZZT", 8U);
    message[14] = static_cast<std::byte>(static_cast<std::uint8_t>(status));
    message[15] = std::byte{'0'};
    return message;
}

[[nodiscard]] Bytes pitch_administrative(
    const std::uint8_t type,
    const std::size_t length) {
    Bytes message(length, std::byte{0U});
    message[0] = static_cast<std::byte>(length);
    message[1] = static_cast<std::byte>(type);
    put_le32(message, 2U, 1'250U);
    put_ascii(message, 6U, "ZVZZT", 8U);
    message[14] = std::byte{'O'};
    return message;
}

[[nodiscard]] Bytes pitch_unit_clear() {
    Bytes message(6U, std::byte{0U});
    message[0] = std::byte{6U};
    message[1] = std::byte{0x97U};
    put_le32(message, 2U, 1'300U);
    return message;
}

[[nodiscard]] Bytes pitch_packet(
    const std::uint8_t unit,
    const std::uint32_t first_sequence,
    const std::span<const Bytes> messages) {
    Bytes packet;
    packet.reserve(128U);
    append_le16(packet, 0U);
    append_u8(packet, static_cast<std::uint8_t>(messages.size()));
    append_u8(packet, unit);
    append_le32(packet, first_sequence);
    for (const auto& message : messages) {
        packet.insert(packet.end(), message.begin(), message.end());
    }
    put_le16(packet, 0U, static_cast<std::uint16_t>(packet.size()));
    return packet;
}

[[nodiscard]] marketdata::FixedL3Book make_book(
    const InstrumentId instrument) {
    return marketdata::FixedL3Book{marketdata::FixedL3BookConfig{
        VenueId{1U},
        ChannelId{2U},
        ProfileId{3U},
        instrument,
        32U,
        64U,
        32U,
        32U,
        InstrumentState::Trading,
    }};
}

struct BookLookupContext final {
    marketdata::FixedL3Book* book{};
    InstrumentId instrument{};
};

[[nodiscard]] bool lookup_book(
    void* const opaque,
    const std::uint64_t reference,
    nmd::NativeOrderState& state) noexcept {
    const auto& context = *static_cast<BookLookupContext*>(opaque);
    const auto order = context.book->order(reference);
    if (!order.has_value()) {
        return false;
    }
    state.instrument = context.instrument;
    state.side = order->side;
    state.price = order->price;
    state.quantity = order->quantity;
    return true;
}

void apply_routed(
    marketdata::FixedL3Book& book,
    const std::span<const nmd::RoutedL3Event> routed,
    const InstrumentId instrument,
    const std::uint64_t last_sequence) {
    std::array<marketdata::L3Event, 32U> events{};
    assert(routed.size() <= events.size());
    for (std::size_t index = 0U; index < routed.size(); ++index) {
        assert(routed[index].instrument == instrument);
        events[index] = routed[index].event;
    }
    const auto result = book.apply_packet(
        marketdata::BookPacketContext{
            1U,
            1U,
            last_sequence,
            100'000'000'000'000ULL,
            100'000'000'000'001ULL,
            MarketDataState::Live,
        },
        std::span<const marketdata::L3Event>{events.data(), routed.size()});
    assert(result.accepted());
}

void test_spec_identity_and_recovery_metadata() {
    assert(itch::kCertificationStatus == nmd::CertificationStatus::Uncertified);
    assert(itch::kSpecificationRevisionDate == "2026-06-11");
    assert(itch::kMoldSpecificationVersion == "1.00");
    assert(itch::kGlimpseSpecificationRevisionDate == "2026-06-11");
    assert(pitch::kCertificationStatus == nmd::CertificationStatus::Uncertified);
    assert(pitch::kSpecificationVersion == "2.41.74");
    assert(pitch::kSpecificationRevisionDate == "2026-08-10");

    const itch::MoldSession session{'T', 'E', 'S', 'T', 'S', 'E', 'S', 'S', '0', '1'};
    std::array<std::byte, itch::kMoldRequestBytes> request_bytes{};
    assert(itch::encode_mold_rerequest(
               itch::MoldRerequest{session, 0x0102'0304'0506'0708ULL, 9U},
               request_bytes) == nmd::DecodeStatus::Ok);
    assert(std::to_integer<std::uint8_t>(request_bytes[10]) == 0x01U);
    assert(std::to_integer<std::uint8_t>(request_bytes[17]) == 0x08U);
    assert(std::to_integer<std::uint8_t>(request_bytes[19]) == 9U);

    std::array<std::byte, itch::kGlimpseSnapshotBytes> glimpse{};
    glimpse.fill(std::byte{' '});
    glimpse[0] = std::byte{'G'};
    constexpr std::string_view sequence = "184467";
    for (std::size_t index = 0U; index < sequence.size(); ++index) {
        glimpse[glimpse.size() - sequence.size() + index] =
            static_cast<std::byte>(static_cast<std::uint8_t>(sequence[index]));
    }
    itch::GlimpseSnapshotCut cut{};
    assert(itch::decode_glimpse_snapshot(glimpse, cut) == nmd::DecodeStatus::Ok);
    assert(cut.realtime_sequence == 184'467U);

    std::array<std::byte, pitch::kGapRequestMessageBytes> gap{};
    assert(pitch::encode_gap_request_message(
               pitch::GapRequest{7U, 0xFFFF'FFFEU, 4U}, gap) ==
           nmd::DecodeStatus::Ok);
    assert(std::to_integer<std::uint8_t>(gap[1]) == 0x03U);
    assert(std::to_integer<std::uint8_t>(gap[3]) == 0xFEU);

    std::array<std::byte, pitch::kGapResponseMessageBytes> response{
        std::byte{10U}, std::byte{0x04U}, std::byte{7U},
        std::byte{0xFEU}, std::byte{0xFFU}, std::byte{0xFFU},
        std::byte{0xFFU}, std::byte{4U}, std::byte{0U}, std::byte{'A'},
    };
    pitch::GapResponse decoded{};
    assert(pitch::decode_gap_response_message(response, decoded) ==
           nmd::DecodeStatus::Ok);
    assert(decoded.accepted() && decoded.message_count == 4U);

    std::array<std::byte, pitch::kSpinRequestMessageBytes> spin{};
    assert(pitch::encode_spin_request_message(
               pitch::SpinRequest{99U}, spin) == nmd::DecodeStatus::Ok);
    assert(std::to_integer<std::uint8_t>(spin[1]) == 0x81U);
}

void test_nasdaq_spec_vectors_transaction_and_gating() {
    constexpr InstrumentId instrument{42U};
    constexpr std::uint64_t reference = 0xFEDC'BA98'7654'3210ULL;
    const itch::MoldSession session{'T', 'E', 'S', 'T', 'S', 'E', 'S', 'S', '0', '1'};
    const std::array bindings{
        itch::SymbolBinding{instrument, symbol8("TEST")},
    };
    itch::Decoder decoder{itch::DecoderConfig{session, bindings, {}}};
    assert(decoder.configured());

    const std::array messages{
        stock_directory(17U, "TEST", 'F', 'Z'),
        itch_add(17U, reference, 100U, 123'450U),
        itch_execute(17U, reference, 25U, 0x1020'3040'5060'7080ULL),
    };
    const auto packet = mold_packet(session, 10U, messages);
    std::array<nmd::RoutedL3Event, 8U> l3{};
    std::array<nmd::ControlEvent, 8U> controls{};
    std::array<nmd::DecodedAction, 16U> actions{};
    const nmd::DecodeBuffers buffers{l3, controls, actions};

    // Inspection cannot commit the locate mapping or consult order state.
    const auto inspected = decoder.decode_mold_packet(
        packet,
        itch::DecodeContext{marketdata::FeedLeg::A, 10U, false},
        {},
        buffers);
    assert(inspected.accepted());
    assert(!inspected.payload_decoded);
    assert(inspected.sequence_state == nmd::SequenceState::InOrder);
    assert(inspected.first_sequence == 10U && inspected.next_sequence == 13U);
    assert(inspected.contains_book_update && inspected.l3_event_count == 0U);

    const auto decoded = decoder.decode_mold_packet(
        packet,
        itch::DecodeContext{marketdata::FeedLeg::A, 10U, true},
        {},
        buffers);
    assert(decoded.accepted() && decoded.payload_decoded);
    assert(decoded.l3_event_count == 2U && decoded.control_count == 2U);
    assert(decoded.action_count == 4U && decoded.commit_token != 0U);
    assert(actions[0].kind == nmd::DecodedActionKind::Control &&
           actions[0].source_sequence == 10U);
    assert(actions[1].kind == nmd::DecodedActionKind::L3Event &&
           actions[1].source_sequence == 11U);
    assert(l3[0].event.operation == marketdata::L3Operation::Add);
    assert(l3[0].event.order_reference == reference);
    assert(l3[0].event.price == Price{123'450});
    assert(l3[1].event.operation == marketdata::L3Operation::Modify);
    assert(l3[1].event.quantity == Quantity{75U});
    assert(controls[0].kind == nmd::ControlKind::Directory);
    assert(controls[0].code[0] == 'F' && controls[0].code[1] == 'Z');
    assert(controls[1].match_or_execution_id == 0x1020'3040'5060'7080ULL);

    auto book = make_book(instrument);
    apply_routed(
        book,
        std::span<const nmd::RoutedL3Event>{l3.data(), decoded.l3_event_count},
        instrument,
        12U);
    assert(decoder.finish_directory_commit(decoded.commit_token, true) ==
           itch::DirectoryCommitStatus::Committed);
    assert(book.order(reference)->quantity == Quantity{75U});

    BookLookupContext lookup_context{&book, instrument};
    const nmd::OrderLookup lookup{&lookup_context, &lookup_book};
    constexpr std::uint64_t replacement = 0xA123'4567'89AB'CDEFULL;
    const std::array replace_messages{
        itch_replace(17U, reference, replacement, 50U, 124'000U),
    };
    const auto replace_packet = mold_packet(session, 13U, replace_messages);
    const auto replace_decoded = decoder.decode_mold_packet(
        replace_packet,
        itch::DecodeContext{marketdata::FeedLeg::A, 13U, true},
        lookup,
        buffers);
    assert(replace_decoded.accepted() && replace_decoded.l3_event_count == 2U);
    assert(l3[0].event.operation == marketdata::L3Operation::Delete);
    assert(l3[1].event.operation == marketdata::L3Operation::Add);
    assert(l3[1].event.order_reference == replacement);
    apply_routed(
        book,
        std::span<const nmd::RoutedL3Event>{l3.data(), 2U},
        instrument,
        13U);
    assert(!book.order(reference).has_value());
    assert(book.order(replacement)->price == Price{124'000});

    const std::array cancel_messages{itch_cancel(17U, replacement, 25U)};
    const auto cancel_packet = mold_packet(session, 14U, cancel_messages);
    const auto cancel_decoded = decoder.decode_mold_packet(
        cancel_packet,
        itch::DecodeContext{marketdata::FeedLeg::B, 14U, true},
        lookup,
        buffers);
    assert(cancel_decoded.accepted() && cancel_decoded.l3_event_count == 1U);
    assert(l3[0].leg == marketdata::FeedLeg::B);
    assert(l3[0].event.quantity == Quantity{25U});

    constexpr std::uint64_t trade_match = 0x9999'8888'7777'6666ULL;
    const std::array control_messages{
        itch_trade_control(17U, trade_match),
        itch_broken_control(17U, trade_match),
        itch_trading_status(17U),
        itch_zero_share_cross(17U, trade_match + 1U),
    };
    const auto controls_packet = mold_packet(session, 15U, control_messages);
    const auto controls_decoded = decoder.decode_mold_packet(
        controls_packet,
        itch::DecodeContext{marketdata::FeedLeg::A, 15U, true},
        lookup,
        buffers);
    assert(controls_decoded.accepted() && controls_decoded.control_count == 4U);
    assert(controls[0].kind == nmd::ControlKind::Trade);
    assert(controls[1].kind == nmd::ControlKind::BrokenTrade);
    assert(controls[2].kind == nmd::ControlKind::TradingStatus);
    assert(controls[3].kind == nmd::ControlKind::Trade);
    assert(controls[3].quantity == 0U && controls[3].code[0] == 'I');

    const auto duplicate_inspection = decoder.decode_mold_packet(
        packet,
        itch::DecodeContext{marketdata::FeedLeg::B, 18U, false},
        {},
        buffers);
    assert(duplicate_inspection.accepted());
    assert(duplicate_inspection.sequence_state == nmd::SequenceState::Duplicate);
    assert(!duplicate_inspection.payload_decoded);

    const auto gap_inspection = decoder.decode_mold_packet(
        packet,
        itch::DecodeContext{marketdata::FeedLeg::B, 8U, false},
        {},
        buffers);
    assert(gap_inspection.sequence_state == nmd::SequenceState::Gap);

    const nmd::DecodeBuffers no_capacity{{}, {}, {}};
    assert(decoder.decode_mold_packet(
               packet,
               itch::DecodeContext{marketdata::FeedLeg::A, 10U, true},
               {},
               no_capacity)
               .status == nmd::DecodeStatus::OutputCapacityExceeded);
}

void test_nasdaq_malformed_capacity_heartbeat_and_corpus() {
    constexpr InstrumentId instrument{42U};
    const itch::MoldSession session{'T', 'E', 'S', 'T', 'S', 'E', 'S', 'S', '0', '1'};
    const std::array bindings{
        itch::SymbolBinding{instrument, symbol8("TEST")},
    };
    itch::Decoder decoder{itch::DecoderConfig{session, bindings, {}}};
    const std::array messages{stock_directory(17U, "TEST", 'M', 'N')};
    auto packet = mold_packet(session, 1U, messages);
    std::array<nmd::RoutedL3Event, 1U> l3{};
    std::array<nmd::ControlEvent, 1U> controls{};
    std::array<nmd::DecodedAction, 2U> actions{};
    const nmd::DecodeBuffers buffers{l3, controls, actions};

    auto truncated = packet;
    truncated.pop_back();
    assert(decoder.decode_mold_packet(
               truncated,
               itch::DecodeContext{marketdata::FeedLeg::A, 1U, false},
               {},
               buffers)
               .status == nmd::DecodeStatus::Truncated);

    auto unknown = packet;
    unknown[22] = std::byte{'?'};
    assert(decoder.decode_mold_packet(
               unknown,
               itch::DecodeContext{marketdata::FeedLeg::A, 1U, false},
               {},
               buffers)
               .status == nmd::DecodeStatus::UnsupportedMessage);

    auto wrong_session = packet;
    wrong_session[0] = std::byte{'X'};
    assert(decoder.decode_mold_packet(
               wrong_session,
               itch::DecodeContext{marketdata::FeedLeg::A, 1U, false},
               {},
               buffers)
               .status == nmd::DecodeStatus::InvalidSession);

    const auto heartbeat = mold_packet(session, 44U, {}, 0U);
    const auto heartbeat_result = decoder.decode_mold_packet(
        heartbeat,
        itch::DecodeContext{marketdata::FeedLeg::A, 44U, true},
        {},
        buffers);
    assert(heartbeat_result.status == nmd::DecodeStatus::Heartbeat);
    assert(heartbeat_result.control_count == 1U);

    const auto end = mold_packet(session, 45U, {}, 0xFFFFU);
    const auto end_result = decoder.decode_mold_packet(
        end,
        itch::DecodeContext{marketdata::FeedLeg::A, 45U, true},
        {},
        buffers);
    assert(end_result.status == nmd::DecodeStatus::EndOfSession);
    assert(end_result.wire_message_count == 0xFFFFU);
    assert(end_result.observation(
               marketdata::FeedLeg::A, 1U, 100U)
               .message_count == 0U);

    // A syntactically valid directory mapping is transactional: downstream
    // rejection aborts it and a later symbol-less lifecycle cannot use it.
    const auto directory_decode = decoder.decode_mold_packet(
        packet,
        itch::DecodeContext{marketdata::FeedLeg::A, 1U, true},
        {},
        buffers);
    assert(directory_decode.accepted() && directory_decode.commit_token != 0U);
    assert(decoder.finish_directory_commit(
               directory_decode.commit_token, false) ==
           itch::DirectoryCommitStatus::Aborted);
    const std::array add_only_messages{
        itch_add(17U, 0x1234'5678'9ABC'DEF0ULL, 10U, 100'000U),
    };
    const auto add_only = mold_packet(session, 2U, add_only_messages);
    assert(decoder.decode_mold_packet(
               add_only,
               itch::DecodeContext{marketdata::FeedLeg::A, 2U, true},
               {},
               buffers)
               .status == nmd::DecodeStatus::UnknownInstrument);

    // Fixed-seed malformed corpus: bounded inspection, no crashes or writes
    // outside caller buffers, and no post-construction allocation.
    std::array<std::byte, 96U> corpus{};
    assert(packet.size() <= corpus.size());
    std::copy(packet.begin(), packet.end(), corpus.begin());
    std::uint32_t seed = 0xC001'D00DU;
    allocation_probe::Scope scope;
    for (std::size_t iteration = 0U; iteration < 128U; ++iteration) {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        std::copy(packet.begin(), packet.end(), corpus.begin());
        const auto position = static_cast<std::size_t>(seed % packet.size());
        corpus[position] ^= static_cast<std::byte>((seed >> 8U) | 1U);
        const auto length = static_cast<std::size_t>(
            1U + (seed % static_cast<std::uint32_t>(packet.size())));
        const auto result = decoder.decode_mold_packet(
            std::span<const std::byte>{corpus.data(), length},
            itch::DecodeContext{marketdata::FeedLeg::A, 1U, false},
            {},
            buffers);
        static_cast<void>(result);
    }
    assert(scope.allocations() == 0U);
}

void test_cboe_official_vector_book_fifo_and_execution_semantics() {
    constexpr InstrumentId instrument{77U};
    constexpr std::uint8_t unit = 3U;
    constexpr std::uint64_t reference = 0x0B1D'568F'775B'4005ULL;
    const std::array bindings{
        pitch::SymbolBinding{instrument, symbol8("ZVZZT")},
    };
    pitch::Decoder decoder{pitch::DecoderConfig{unit, bindings}};
    assert(decoder.configured());

    const std::array messages{pitch_time(34'200U), cboe_official_add_example()};
    const auto packet = pitch_packet(unit, 100U, messages);
    std::array<nmd::RoutedL3Event, 16U> l3{};
    std::array<nmd::ControlEvent, 16U> controls{};
    std::array<nmd::DecodedAction, 32U> actions{};
    const nmd::DecodeBuffers buffers{l3, controls, actions};

    const auto inspected = decoder.decode_packet(
        packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            100U,
            1U,
            0U,
            false,
            false,
        },
        {},
        buffers);
    assert(inspected.packet.accepted());
    assert(!inspected.packet.payload_decoded);
    assert(inspected.packet.first_sequence == 100U);
    assert(inspected.packet.next_sequence == 102U);

    const auto decoded = decoder.decode_packet(
        packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            100U,
            1U,
            0U,
            false,
            true,
        },
        {},
        buffers);
    assert(decoded.packet.accepted() && decoded.packet.payload_decoded);
    assert(decoded.packet.l3_event_count == 1U);
    assert(decoded.packet.action_count == 2U);
    assert(actions[0].kind == nmd::DecodedActionKind::Control &&
           actions[0].source_sequence == 100U);
    assert(actions[1].kind == nmd::DecodedActionKind::L3Event &&
           actions[1].source_sequence == 101U);
    assert(l3[0].source_sequence == 101U);
    assert(l3[0].event.order_reference == reference);
    assert(l3[0].event.quantity == Quantity{20'000U});
    assert(l3[0].event.price == Price{9'050});
    assert(decoded.state.has_time_base &&
           decoded.state.last_time_seconds == 34'200U);

    auto book = make_book(instrument);
    apply_routed(
        book,
        std::span<const nmd::RoutedL3Event>{l3.data(), 1U},
        instrument,
        101U);
    BookLookupContext lookup_context{&book, instrument};
    const nmd::OrderLookup lookup{&lookup_context, &lookup_book};

    const std::array execute_messages{
        pitch_execute(reference, 5'000U, 0x1111'2222'3333'4444ULL),
    };
    const auto execute_packet = pitch_packet(unit, 102U, execute_messages);
    const auto execute_decoded = decoder.decode_packet(
        execute_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::B,
            pitch::DecodeMode::RealtimeMulticast,
            102U,
            1U,
            decoded.state.last_time_seconds,
            true,
            true,
        },
        lookup,
        buffers);
    assert(execute_decoded.packet.accepted());
    assert(l3[0].leg == marketdata::FeedLeg::B);
    assert(l3[0].event.quantity == Quantity{15'000U});
    apply_routed(
        book,
        std::span<const nmd::RoutedL3Event>{l3.data(), 1U},
        instrument,
        102U);

    constexpr std::uint64_t second_reference = 0xAA00'0000'0000'0002ULL;
    const std::array priority_messages{
        pitch_add(second_reference, 100U, 9'050U),
        pitch_modify(reference, 10'000U, 9'050U, 0x01U),
    };
    const auto priority_packet = pitch_packet(unit, 103U, priority_messages);
    const auto priority_decoded = decoder.decode_packet(
        priority_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            103U,
            1U,
            decoded.state.last_time_seconds,
            true,
            true,
        },
        lookup,
        buffers);
    assert(priority_decoded.packet.accepted());
    assert(priority_decoded.packet.l3_event_count == 3U);
    assert(l3[1].event.operation == marketdata::L3Operation::Delete);
    assert(l3[2].event.operation == marketdata::L3Operation::Add);
    apply_routed(
        book,
        std::span<const nmd::RoutedL3Event>{
            l3.data(), priority_decoded.packet.l3_event_count},
        instrument,
        104U);
    std::array<marketdata::OrderReference, 4U> fifo{};
    assert(book.copy_fifo(Side::Buy, Price{9'050}, fifo) == 2U);
    assert(fifo[0] == second_reference && fifo[1] == reference);

    constexpr std::uint64_t third_reference = 0xAA00'0000'0000'0003ULL;
    const std::array price_execution_messages{
        pitch_add(third_reference, 50U, 10'000U),
        pitch_execute_at_price(
            third_reference,
            75U,
            10U,
            0x5555'6666'7777'8888ULL,
            11'000U),
    };
    const auto price_execution_packet =
        pitch_packet(unit, 105U, price_execution_messages);
    const auto price_execution_decoded = decoder.decode_packet(
        price_execution_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            105U,
            1U,
            decoded.state.last_time_seconds,
            true,
            true,
        },
        lookup,
        buffers);
    assert(price_execution_decoded.packet.accepted());
    assert(price_execution_decoded.packet.l3_event_count == 2U);
    assert(l3[1].event.operation == marketdata::L3Operation::Modify);
    assert(l3[1].event.quantity == Quantity{10U});
    assert(l3[1].event.price == Price{10'000});
    assert(controls[0].price_units == 11'000);
}

void test_cboe_controls_rollover_malformed_and_allocations() {
    constexpr InstrumentId instrument{77U};
    constexpr std::uint8_t unit = 3U;
    const std::array bindings{
        pitch::SymbolBinding{instrument, symbol8("ZVZZT")},
    };
    pitch::Decoder decoder{pitch::DecoderConfig{unit, bindings}};
    std::array<nmd::RoutedL3Event, 16U> l3{};
    std::array<nmd::ControlEvent, 16U> controls{};
    std::array<nmd::DecodedAction, 32U> actions{};
    const nmd::DecodeBuffers buffers{l3, controls, actions};

    const std::array control_messages{
        pitch_trade(99U, 0x101U),
        pitch_break(0x101U),
        pitch_status('T'),
        pitch_administrative(0x95U, 47U),
        pitch_administrative(0x96U, 27U),
        pitch_administrative(0x98U, 15U),
    };
    const auto controls_packet = pitch_packet(unit, 200U, control_messages);
    const auto decoded = decoder.decode_packet(
        controls_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            200U,
            1U,
            36'000U,
            true,
            true,
        },
        {},
        buffers);
    assert(decoded.packet.accepted() && decoded.packet.control_count == 6U);
    assert(controls[0].kind == nmd::ControlKind::Trade);
    assert(controls[1].kind == nmd::ControlKind::BrokenTrade);
    assert(controls[2].kind == nmd::ControlKind::TradingStatus);
    assert(controls[3].wire_type == 0x95U &&
           controls[3].kind == nmd::ControlKind::Administrative);
    assert(controls[4].wire_type == 0x96U);
    assert(controls[5].wire_type == 0x98U);

    const auto gap = decoder.decode_packet(
        controls_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::B,
            pitch::DecodeMode::RealtimeMulticast,
            197U,
            1U,
            36'000U,
            true,
            false,
        },
        {},
        buffers);
    assert(gap.packet.sequence_state == nmd::SequenceState::Gap);
    const auto duplicate = decoder.decode_packet(
        controls_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::B,
            pitch::DecodeMode::RealtimeMulticast,
            207U,
            1U,
            36'000U,
            true,
            false,
        },
        {},
        buffers);
    assert(duplicate.packet.sequence_state == nmd::SequenceState::Duplicate);

    const std::array one_status{pitch_status('T')};
    const auto after_rollover_packet = pitch_packet(unit, 1U, one_status);
    const auto gap_across_rollover = decoder.decode_packet(
        after_rollover_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            std::numeric_limits<std::uint32_t>::max(),
            1U,
            36'000U,
            true,
            false,
        },
        {},
        buffers);
    assert(gap_across_rollover.packet.sequence_state == nmd::SequenceState::Gap);
    assert(gap_across_rollover.packet.first_sequence == 4'294'967'296ULL);

    const auto late_prior_packet = pitch_packet(
        unit, std::numeric_limits<std::uint32_t>::max(), one_status);
    const auto late_prior = decoder.decode_packet(
        late_prior_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::B,
            pitch::DecodeMode::RealtimeMulticast,
            2U,
            2U,
            36'000U,
            true,
            false,
        },
        {},
        buffers);
    assert(late_prior.packet.sequence_state == nmd::SequenceState::Duplicate);
    assert(late_prior.packet.first_sequence == 4'294'967'295ULL);

    auto extended_status = pitch_status('T');
    extended_status.push_back(std::byte{0xA5U});
    extended_status[0] = static_cast<std::byte>(extended_status.size());
    const std::array extended_messages{extended_status};
    const auto extended_packet = pitch_packet(unit, 300U, extended_messages);
    const auto extended = decoder.decode_packet(
        extended_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            300U,
            1U,
            36'000U,
            true,
            true,
        },
        {},
        buffers);
    assert(extended.packet.accepted() && extended.packet.has_extension_bytes);

    const std::array clear_then_add{
        pitch_unit_clear(), pitch_add(0xABC0U, 10U, 9'000U)};
    const auto clear_packet = pitch_packet(unit, 301U, clear_then_add);
    const auto clear_decoded = decoder.decode_packet(
        clear_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            301U,
            1U,
            36'000U,
            true,
            true,
        },
        {},
        buffers);
    assert(clear_decoded.packet.accepted() &&
           clear_decoded.packet.action_count == 2U);
    assert(actions[0].kind == nmd::DecodedActionKind::Control &&
           controls[actions[0].output_index].kind == nmd::ControlKind::UnitClear);
    assert(actions[1].kind == nmd::DecodedActionKind::L3Event);

    const std::array rollover_messages{pitch_time(36'001U), pitch_status('H')};
    const auto rollover_packet = pitch_packet(
        unit, std::numeric_limits<std::uint32_t>::max(), rollover_messages);
    const auto rollover = decoder.decode_packet(
        rollover_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::B,
            pitch::DecodeMode::RealtimeMulticast,
            std::numeric_limits<std::uint32_t>::max(),
            1U,
            36'000U,
            true,
            false,
        },
        {},
        buffers);
    assert(rollover.packet.accepted());
    assert(rollover.packet.sequence_state == nmd::SequenceState::Rollover);
    assert(rollover.state.next_native_sequence == 2U);
    assert(rollover.state.next_sequence_epoch == 2U);
    assert(rollover.packet.next_sequence == 4'294'967'297ULL);

    const auto zero_heartbeat = pitch_packet(unit, 0U, {});
    const auto heartbeat = decoder.decode_packet(
        zero_heartbeat,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            0U,
            1U,
            36'000U,
            true,
            true,
        },
        {},
        buffers);
    assert(heartbeat.packet.status == nmd::DecodeStatus::Heartbeat);
    assert(heartbeat.packet.sequence_state == nmd::SequenceState::Unsequenced);

    const std::array spin_messages{Bytes{
        std::byte{6U}, std::byte{0x80U}, std::byte{99U}, std::byte{0U},
        std::byte{0U}, std::byte{0U},
    }};
    const auto spin_packet = pitch_packet(0U, 0U, spin_messages);
    const auto spin = decoder.decode_packet(
        spin_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::SpinServer,
            0U,
            1U,
            0U,
            false,
            true,
        },
        {},
        buffers);
    assert(spin.packet.accepted());
    assert(controls[0].kind == nmd::ControlKind::SpinImageAvailable);
    assert(controls[0].protocol_sequence == 99U);

    auto truncated = controls_packet;
    truncated.pop_back();
    assert(decoder.decode_packet(
               truncated,
               pitch::DecodeContext{
                   marketdata::FeedLeg::A,
                   pitch::DecodeMode::RealtimeMulticast,
                   200U,
                   1U,
                   36'000U,
                   true,
                   false,
               },
               {},
               buffers)
               .packet.status == nmd::DecodeStatus::InvalidEnvelope);

    auto unknown = controls_packet;
    unknown[9] = std::byte{0x7FU};
    assert(decoder.decode_packet(
               unknown,
               pitch::DecodeContext{
                   marketdata::FeedLeg::A,
                   pitch::DecodeMode::RealtimeMulticast,
                   200U,
                   1U,
                   36'000U,
                   true,
                   false,
               },
               {},
               buffers)
               .packet.status == nmd::DecodeStatus::UnsupportedMessage);

    // The already-constructed decoder performs both inspection and semantic
    // decode without heap traffic.
    allocation_probe::Scope scope;
    const auto allocation_result = decoder.decode_packet(
        controls_packet,
        pitch::DecodeContext{
            marketdata::FeedLeg::A,
            pitch::DecodeMode::RealtimeMulticast,
            200U,
            1U,
            36'000U,
            true,
            true,
        },
        {},
        buffers);
    assert(allocation_result.packet.accepted());
    std::array<std::byte, 256U> corpus{};
    assert(controls_packet.size() <= corpus.size());
    std::uint32_t seed = 0x1357'9BDFU;
    for (std::size_t iteration = 0U; iteration < 128U; ++iteration) {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        std::copy(controls_packet.begin(), controls_packet.end(), corpus.begin());
        const auto position = static_cast<std::size_t>(
            seed % static_cast<std::uint32_t>(controls_packet.size()));
        corpus[position] ^= static_cast<std::byte>((seed >> 9U) | 1U);
        const auto length = static_cast<std::size_t>(
            1U + (seed % static_cast<std::uint32_t>(controls_packet.size())));
        const auto corpus_result = decoder.decode_packet(
            std::span<const std::byte>{corpus.data(), length},
            pitch::DecodeContext{
                marketdata::FeedLeg::A,
                pitch::DecodeMode::RealtimeMulticast,
                200U,
                1U,
                36'000U,
                true,
                false,
            },
            {},
            buffers);
        static_cast<void>(corpus_result);
    }
    assert(scope.allocations() == 0U);
}

}  // namespace

int main() {
    test_spec_identity_and_recovery_metadata();
    test_nasdaq_spec_vectors_transaction_and_gating();
    test_nasdaq_malformed_capacity_heartbeat_and_corpus();
    test_cboe_official_vector_book_fifo_and_execution_semantics();
    test_cboe_controls_rollover_malformed_and_allocations();
    return 0;
}
