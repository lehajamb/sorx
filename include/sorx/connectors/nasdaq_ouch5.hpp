#pragma once

#include "sorx/fastpath/cancel_pipeline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sorx::connectors::nasdaq {

// Source-controlled wire profile. A matching title/version/URL is deliberately
// part of the public contract so a connector manifest can pin what was coded.
inline constexpr std::string_view kOuchSpecificationTitle =
    "OUCH 5.0 Order Entry Specification";
inline constexpr std::string_view kOuchSpecificationVersion = "5.0";
inline constexpr std::string_view kOuchSpecificationDate =
    "Updated October, 2025";
inline constexpr std::string_view kOuchSpecificationUrl =
    "https://www.nasdaqtrader.com/content/technicalsupport/specifications/"
    "TradingProducts/OUCH5.0.pdf";
inline constexpr std::string_view kSoupSpecificationTitle = "SoupBinTCP";
inline constexpr std::string_view kSoupSpecificationVersion = "4.00";
inline constexpr std::string_view kSoupSpecificationDate = "07/12/2010";
inline constexpr std::string_view kSoupSpecificationUrl =
    "https://nasdaqtrader.com/content/technicalsupport/specifications/"
    "tradingproducts/soupbintcp4_0.pdf";

enum class CertificationStatus : std::uint8_t {
    Uncertified,
};

struct Ouch5Capabilities final {
    CertificationStatus certification{CertificationStatus::Uncertified};
    bool enter_order{true};
    bool cancel_order{true};
    bool replace_order{true};
    bool modify_order{true};
    bool accepted_rejected_execution_decode{true};
    bool post_only_tag_value{true};
    bool other_optional_tag_values{false};
    bool soup_login{false};
};

inline constexpr Ouch5Capabilities kOuch5Capabilities{};
inline constexpr std::uint64_t kOuchMaximumLimitPrice = 1'999'999'900ULL;
inline constexpr std::uint64_t kNanosecondsPerDay = 86'400'000'000'000ULL;
inline constexpr std::size_t kOuchEnterOrderBytes = 47U;
inline constexpr std::size_t kOuchEnterOrderPostOnlyBytes = 50U;
inline constexpr std::size_t kOuchReplaceOrderBytes = 40U;
inline constexpr std::size_t kOuchCancelOrderBytes = 11U;
inline constexpr std::size_t kOuchModifyOrderBytes = 12U;
inline constexpr std::size_t kOuchPlannedWireIdentityBytes = 18U;

enum class CodecStatus : std::uint8_t {
    Ok,
    BufferTooSmall,
    Truncated,
    InvalidLength,
    InvalidType,
    InvalidField,
    Unsupported,
    SequenceMismatch,
};

struct CodecResult final {
    CodecStatus status{CodecStatus::InvalidField};
    std::uint16_t size{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == CodecStatus::Ok;
    }
};

using Symbol = std::array<char, 8U>;
using ClientOrderId = std::array<char, 14U>;

enum class OuchSide : char {
    Buy = 'B',
    Sell = 'S',
    SellShort = 'T',
    SellShortExempt = 'E',
};

struct PlannedWireIdentity final {
    std::uint32_t user_reference_number{};
    ClientOrderId client_order_id{};
};

// The inline identity is exactly: 4-byte big-endian UserRefNum followed by the
// 14-byte, right-space-padded ClOrdID. The route plan therefore pins both OUCH
// wire identifiers before durable authorization.
[[nodiscard]] bool make_planned_wire_identity(
    std::uint32_t user_reference_number,
    std::string_view client_order_id,
    dataplane::InlineWireIdentity& destination) noexcept;

[[nodiscard]] bool parse_planned_wire_identity(
    const dataplane::InlineWireIdentity& source,
    PlannedWireIdentity& destination) noexcept;

[[nodiscard]] dataplane::WireIdentityPlanStatus plan_route_wire_identity(
    const dataplane::WireIdentityPlanRequest& request,
    dataplane::InlineWireIdentity& destination) noexcept;

struct EnterOrder final {
    PlannedWireIdentity identity{};
    OuchSide side{OuchSide::Buy};
    std::uint32_t quantity{};
    Symbol symbol{};
    std::uint64_t price{};
    char time_in_force{'3'};
    char display{'Y'};
    char capacity{'A'};
    char intermarket_sweep_eligibility{'Y'};
    char cross_type{'N'};
    bool post_only{};
};

struct ReplaceOrder final {
    std::uint32_t original_user_reference_number{};
    PlannedWireIdentity replacement_identity{};
    std::uint32_t total_quantity{};
    std::uint64_t price{};
    char time_in_force{'3'};
    char display{'Y'};
    char intermarket_sweep_eligibility{'Y'};
};

struct CancelOrder final {
    std::uint32_t user_reference_number{};
    std::uint32_t intended_total_quantity{};
};

struct ModifyOrder final {
    std::uint32_t user_reference_number{};
    OuchSide side{OuchSide::Sell};
    std::uint32_t intended_total_quantity{};
};

[[nodiscard]] CodecResult encode_enter_order(
    const EnterOrder& message,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_replace_order(
    const ReplaceOrder& message,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_cancel_order(
    const CancelOrder& message,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_modify_order(
    const ModifyOrder& message,
    std::span<std::byte> destination) noexcept;

enum class SoupPacketType : char {
    SequencedData = 'S',
    UnsequencedData = 'U',
    ServerHeartbeat = 'H',
    ClientHeartbeat = 'R',
    EndOfSession = 'Z',
};

struct SoupPacketView final {
    SoupPacketType type{SoupPacketType::UnsequencedData};
    std::span<const std::byte> payload{};
};

[[nodiscard]] CodecResult encode_soup_unsequenced(
    std::span<const std::byte> payload,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecResult encode_soup_client_heartbeat(
    std::span<std::byte> destination) noexcept;
[[nodiscard]] CodecStatus decode_soup_packet(
    std::span<const std::byte> source,
    SoupPacketView& destination) noexcept;

class SoupSequenceTracker final {
public:
    explicit constexpr SoupSequenceTracker(
        const std::uint64_t next_sequence = 1U) noexcept
        : next_sequence_(next_sequence) {}

    [[nodiscard]] CodecStatus accept(
        const SoupPacketView& packet,
        std::uint64_t& packet_sequence) noexcept;
    [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
        return next_sequence_;
    }

private:
    std::uint64_t next_sequence_{1U};
};

enum class ResponseKind : std::uint8_t {
    Accepted,
    Rejected,
    Executed,
    Canceled,
};

struct OrderResponse final {
    ResponseKind kind{ResponseKind::Rejected};
    std::uint64_t timestamp_ns{};
    std::uint32_t user_reference_number{};
    OuchSide side{OuchSide::Buy};
    std::uint32_t quantity{};
    Symbol symbol{};
    std::uint64_t price{};
    std::uint64_t order_reference_number{};
    std::uint64_t match_number{};
    ClientOrderId client_order_id{};
    std::uint16_t reject_reason{};
    char liquidity_flag{};
    char cancel_reason{};
    char order_state{};
};

[[nodiscard]] CodecStatus decode_order_response(
    std::span<const std::byte> source,
    OrderResponse& destination) noexcept;

struct InstrumentSymbol final {
    InstrumentId instrument{};
    Symbol symbol{};
};

class Ouch5ChildFrameEncoder final :
    public fastpath::IChildFrameEncoder,
    public fastpath::INativeCancelFrameEncoder {
public:
    Ouch5ChildFrameEncoder(
        VenueId venue,
        char display,
        char capacity,
        char intermarket_sweep_eligibility) noexcept;

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
    char display_{};
    char capacity_{};
    char intermarket_sweep_eligibility_{};
    std::array<Slot, kSymbolSlots> symbols_{};
    std::uint16_t symbol_count_{};
    bool sealed_{};
};

}  // namespace sorx::connectors::nasdaq
