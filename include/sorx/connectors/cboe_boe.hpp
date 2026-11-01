#pragma once

#include "sorx/fastpath/cancel_pipeline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sorx::connectors::cboe {

inline constexpr std::string_view kBoeSpecificationTitle =
    "Cboe Titanium U.S. Equities BOE Specification";
inline constexpr std::string_view kBoeSpecificationVersion = "2.4.56";
inline constexpr std::string_view kBoeSpecificationDate = "August 4, 2026";
inline constexpr std::string_view kBoeSpecificationUrl =
    "https://www.cboe.com/document/tech-spec/document/technical-specifications/"
    "cboe-titanium-u.s.-equities-boe-specification";

enum class CertificationStatus : std::uint8_t {
    Uncertified,
};

// This deliberately narrow profile requests no outbound return bitfields. It
// rejects optional return tails rather than guessing their layout. Widening the
// profile requires new golden vectors and Cboe certification.
struct BoeCapabilities final {
    CertificationStatus certification{CertificationStatus::Uncertified};
    bool new_order{true};
    bool cancel_order{true};
    bool modify_order{true};
    bool acknowledgment_reject_execution_decode{true};
    bool zero_return_bitfields_only{true};
    bool login_parameter_groups{false};
};

inline constexpr BoeCapabilities kBoeCapabilities{};
inline constexpr std::size_t kBoePlannedWireIdentityBytes = 24U;
inline constexpr std::uint32_t kMaximumOrderQuantity = 999'999U;

enum class CodecStatus : std::uint8_t {
    Ok,
    BufferTooSmall,
    Truncated,
    InvalidLength,
    InvalidType,
    InvalidField,
    UnsupportedReturnBitfields,
    SequenceMismatch,
    SequenceNotConfigured,
};

struct CodecResult final {
    CodecStatus status{CodecStatus::InvalidField};
    std::uint16_t size{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == CodecStatus::Ok;
    }
};

using Text20 = std::array<char, 20U>;
using Text16 = std::array<char, 16U>;
using Symbol = std::array<char, 8U>;
using RoutingInstruction = std::array<char, 4U>;

struct PlannedWireIdentity final {
    std::uint32_t sequence_number{};
    Text20 client_order_id{};
};

// Inline identity convention: 4-byte little-endian BOE sequence followed by
// the exact 20-byte NUL-padded ClOrdID. It is generated during route planning,
// not during frame encoding, so failed recording cannot consume a hidden
// session sequence and every durable child pins its venue wire identity.
[[nodiscard]] bool make_planned_wire_identity(
    std::uint32_t sequence_number,
    std::string_view client_order_id,
    dataplane::InlineWireIdentity& destination) noexcept;

[[nodiscard]] bool parse_planned_wire_identity(
    const dataplane::InlineWireIdentity& source,
    PlannedWireIdentity& destination) noexcept;

[[nodiscard]] dataplane::WireIdentityPlanStatus plan_route_wire_identity(
    const dataplane::WireIdentityPlanRequest& request,
    dataplane::InlineWireIdentity& destination) noexcept;

enum class BoeSide : char {
    Buy = '1',
    Sell = '2',
    SellShort = '5',
    SellShortExempt = '6',
};

struct NewOrder final {
    PlannedWireIdentity identity{};
    BoeSide side{BoeSide::Buy};
    std::uint32_t quantity{};
    std::int64_t price{};
    Symbol symbol{};
    char capacity{'A'};
    RoutingInstruction routing_instruction{};
    Text16 account{};
    char time_in_force{'3'};
    bool include_time_in_force{true};
};

struct CancelOrder final {
    std::uint32_t sequence_number{};
    Text20 original_client_order_id{};
};

struct ModifyOrder final {
    std::uint32_t sequence_number{};
    Text20 client_order_id{};
    Text20 original_client_order_id{};
    std::uint32_t quantity{};
    std::int64_t price{};
};

[[nodiscard]] CodecResult encode_new_order(
    const NewOrder& message,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_cancel_order(
    const CancelOrder& message,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_modify_order(
    const ModifyOrder& message,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_client_heartbeat(
    std::span<std::byte> destination) noexcept;

enum class ResponseKind : std::uint8_t {
    ServerHeartbeat,
    OrderAcknowledged,
    OrderRejected,
    OrderExecuted,
    OrderCanceled,
    CancelRejected,
};

struct OrderResponse final {
    ResponseKind kind{ResponseKind::OrderRejected};
    std::uint8_t matching_unit{};
    std::uint32_t sequence_number{};
    std::uint64_t transaction_time_ns{};
    Text20 client_order_id{};
    std::uint64_t order_id{};
    std::uint64_t execution_id{};
    std::uint32_t last_shares{};
    std::int64_t last_price{};
    std::uint32_t leaves_quantity{};
    char reason{};
    char base_liquidity_indicator{};
    char sub_liquidity_indicator{};
    std::array<char, 60U> text{};
};

[[nodiscard]] CodecStatus decode_order_response(
    std::span<const std::byte> source,
    OrderResponse& destination) noexcept;

class UnitSequenceTracker final {
public:
    [[nodiscard]] bool set_next(
        std::uint8_t matching_unit,
        std::uint32_t next_sequence) noexcept;
    [[nodiscard]] CodecStatus accept(const OrderResponse& response) noexcept;
    [[nodiscard]] std::uint32_t next(
        std::uint8_t matching_unit) const noexcept;

private:
    std::array<std::uint32_t, 256U> next_{};
};

struct InstrumentSymbol final {
    InstrumentId instrument{};
    Symbol symbol{};
};

class BoeChildFrameEncoder final :
    public fastpath::IChildFrameEncoder,
    public fastpath::INativeCancelFrameEncoder {
public:
    BoeChildFrameEncoder(
        VenueId venue,
        char capacity,
        RoutingInstruction aggressive_routing_instruction,
        RoutingInstruction passive_routing_instruction,
        Text16 account) noexcept;

    [[nodiscard]] bool add_instrument(InstrumentSymbol binding) noexcept;
    [[nodiscard]] bool seal() noexcept;
    [[nodiscard]] bool sealed() const noexcept;

    [[nodiscard]] fastpath::FrameEncodeResult encode_new(
        const ParentOrder& parent,
        const ChildAllocation& allocation,
        const dataplane::PlannedChild& child,
        std::span<std::byte> destination) noexcept override;

    [[nodiscard]] fastpath::FrameEncodeResult encode_cancel(
        const fastpath::CancelCommand& command,
        const fastpath::ActiveChildSnapshot& child,
        std::uint64_t dispatch_attempt_id,
        std::span<std::byte> destination) noexcept override;

private:
    struct Slot final {
        InstrumentSymbol binding{};
        bool occupied{};
    };

    [[nodiscard]] const Symbol* find_symbol(InstrumentId instrument) const noexcept;

    static constexpr std::size_t kSymbolSlots = 512U;
    VenueId venue_{};
    char capacity_{};
    RoutingInstruction aggressive_routing_instruction_{};
    RoutingInstruction passive_routing_instruction_{};
    Text16 account_{};
    std::array<Slot, kSymbolSlots> symbols_{};
    std::uint16_t symbol_count_{};
    bool sealed_{};
};

}  // namespace sorx::connectors::cboe
