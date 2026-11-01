#pragma once

#include "sorx/marketdata/feed_arbiter.hpp"
#include "sorx/marketdata/fixed_l3_book.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace sorx::connectors::native_md {

enum class CertificationStatus : std::uint8_t {
    Uncertified,
};

enum class DecodeStatus : std::uint8_t {
    Ok,
    Heartbeat,
    EndOfSession,
    Truncated,
    InvalidEnvelope,
    InvalidSession,
    InvalidLength,
    InvalidType,
    InvalidField,
    UnsupportedMessage,
    OutputCapacityExceeded,
    UnknownInstrument,
    OrderStateUnavailable,
    QuantityUnderflow,
    SequenceOverflow,
    MissingTimeBase,
    PendingCommit,
    InvalidConfiguration,
};

enum class SequenceState : std::uint8_t {
    NotChecked,
    InOrder,
    Gap,
    Duplicate,
    Overlap,
    Rollover,
    Unsequenced,
};

enum class ControlKind : std::uint8_t {
    Heartbeat,
    EndOfSession,
    SystemEvent,
    Directory,
    TradingStatus,
    Execution,
    Trade,
    BrokenTrade,
    Administrative,
    GlimpseSnapshot,
    Time,
    TimeReference,
    UnitClear,
    GapResponse,
    SpinImageAvailable,
    SpinResponse,
    SpinFinished,
};

using Symbol8 = std::array<char, 8U>;

struct NativeOrderState final {
    InstrumentId instrument{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
};

using OrderLookupFunction = bool (*)(
    void* context,
    std::uint64_t order_reference,
    NativeOrderState& state) noexcept;

struct OrderLookup final {
    void* context{};
    OrderLookupFunction function{};

    [[nodiscard]] bool find(
        const std::uint64_t order_reference,
        NativeOrderState& state) const noexcept {
        return function != nullptr &&
               function(context, order_reference, state);
    }
};

struct RoutedL3Event final {
    InstrumentId instrument{};
    marketdata::FeedLeg leg{marketdata::FeedLeg::None};
    std::uint64_t source_sequence{};
    marketdata::L3Event event{};
};

enum class DecodedActionKind : std::uint8_t {
    L3Event,
    Control,
};

struct DecodedAction final {
    DecodedActionKind kind{DecodedActionKind::Control};
    std::uint64_t source_sequence{};
    std::uint32_t output_index{};
};

// This is deliberately a compact decoded control record, not a raw-message
// owner. The source packet remains caller-owned and is never retained.
struct ControlEvent final {
    ControlKind kind{ControlKind::Administrative};
    InstrumentId instrument{};
    marketdata::FeedLeg leg{marketdata::FeedLeg::None};
    std::uint64_t source_sequence{};
    std::int64_t exchange_timestamp_ns{};
    std::uint64_t order_reference{};
    std::uint64_t match_or_execution_id{};
    std::uint64_t quantity{};
    std::int64_t price_units{};
    std::uint32_t protocol_sequence{};
    std::uint32_t count{};
    std::uint16_t instrument_key{};
    std::uint8_t unit{};
    std::uint8_t wire_type{};
    std::array<char, 4U> code{};
    Symbol8 symbol{};
    bool printable{};
};

struct DecodeBuffers final {
    std::span<RoutedL3Event> l3_events;
    std::span<ControlEvent> controls;
    // Mandatory for semantic decode when any output is produced. It is the
    // canonical wire-order merge over the two typed output arrays.
    std::span<DecodedAction> actions;
};

struct DecodeResult final {
    DecodeStatus status{DecodeStatus::InvalidEnvelope};
    SequenceState sequence_state{SequenceState::NotChecked};
    std::size_t l3_event_count{};
    std::size_t control_count{};
    std::size_t action_count{};
    std::uint64_t first_sequence{};
    std::uint64_t next_sequence{};
    std::uint16_t wire_message_count{};
    std::uint16_t sequence_count{};
    std::uint8_t wire_unit{};
    bool contains_book_update{};
    bool payload_decoded{};
    bool has_extension_bytes{};
    std::uint64_t commit_token{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == DecodeStatus::Ok ||
               status == DecodeStatus::Heartbeat ||
               status == DecodeStatus::EndOfSession;
    }

    [[nodiscard]] constexpr marketdata::FeedPacketObservation observation(
        const marketdata::FeedLeg leg,
        const std::uint64_t feed_epoch,
        const std::uint64_t local_receive_time_ns) const noexcept {
        return marketdata::FeedPacketObservation{
            leg,
            feed_epoch,
            first_sequence,
            sequence_count,
            local_receive_time_ns,
            contains_book_update,
        };
    }
};

}  // namespace sorx::connectors::native_md

namespace sorx::connectors::nasdaq::itch5 {

inline constexpr std::string_view kSpecificationTitle =
    "Nasdaq TotalView-ITCH 5.0";
inline constexpr std::string_view kSpecificationVersion = "5.0";
inline constexpr std::string_view kSpecificationRevisionDate = "2026-06-11";
inline constexpr std::string_view kSpecificationUrl =
    "https://assets.ctfassets.net/mx0rke14e5yt/5Uz6MGJxbo4wRPou8KveFs/"
    "4d76437c8e57694acee9d767587a8dfa/6-11-26_TVITCH_5.0_1.pdf";
inline constexpr std::string_view kMoldSpecificationTitle =
    "MoldUDP64 Protocol Specification";
inline constexpr std::string_view kMoldSpecificationVersion = "1.00";
inline constexpr std::string_view kMoldSpecificationRevisionDate =
    "2024-08-02";
inline constexpr std::string_view kMoldSpecificationUrl =
    "https://www.nasdaqtrader.com/content/technicalsupport/specifications/"
    "dataproducts/moldudp64.pdf";
inline constexpr std::string_view kGlimpseSpecificationTitle =
    "Nasdaq GLIMPSE";
inline constexpr std::string_view kGlimpseSpecificationVersion = "5.0";
inline constexpr std::string_view kGlimpseSpecificationRevisionDate =
    "2026-06-11";
inline constexpr std::string_view kGlimpseSpecificationUrl =
    "https://assets.ctfassets.net/mx0rke14e5yt/1dHPeoYuayePQFxQcJFXtr/"
    "abd72648a068aa9a22631e8914a237dd/"
    "6-11-26_NQGlimpseSpecification_1.pdf";
inline constexpr std::string_view kTexasListingMarketAdvisoryUrl =
    "https://www.nasdaqtrader.com/TraderNews.aspx?id=DTN2026-10";
inline constexpr std::string_view kVerificationDate = "2026-08-15";

inline constexpr native_md::CertificationStatus kCertificationStatus =
    native_md::CertificationStatus::Uncertified;
inline constexpr std::size_t kMoldHeaderBytes = 20U;
inline constexpr std::size_t kMoldRequestBytes = 20U;
inline constexpr std::size_t kGlimpseSnapshotBytes = 21U;
inline constexpr std::uint64_t kNanosecondsPerDay =
    86'400'000'000'000ULL;
inline constexpr std::int64_t kMaximumPriceUnits = 2'000'000'000LL;

using MoldSession = std::array<char, 10U>;

enum class UnmappedInstrumentPolicy : std::uint8_t {
    Reject,
    Ignore,
};

struct SymbolBinding final {
    InstrumentId instrument{};
    native_md::Symbol8 symbol{};
};

struct LocateBinding final {
    std::uint16_t stock_locate{};
    InstrumentId instrument{};
};

struct DecoderConfig final {
    MoldSession expected_session{};
    std::span<const SymbolBinding> symbols;
    std::span<const LocateBinding> initial_locates;
    UnmappedInstrumentPolicy unmapped_policy{
        UnmappedInstrumentPolicy::Reject};
    std::size_t maximum_directory_updates_per_packet{1'024U};
};

enum class DirectoryCommitStatus : std::uint8_t {
    Committed,
    Aborted,
    NoPendingCommit,
    InvalidToken,
};

struct DecodeContext final {
    marketdata::FeedLeg leg{marketdata::FeedLeg::None};
    std::uint64_t expected_sequence{};
    // Leave false for the envelope/shape inspection pass. Set true only after
    // the arbiter has authorized this exact packet range for application.
    // This prevents duplicate A/B packets from consulting newer order state.
    bool decode_payload{};
};

struct MoldRerequest final {
    MoldSession session{};
    std::uint64_t first_sequence{};
    std::uint16_t message_count{};
};

struct GlimpseSnapshotCut final {
    std::uint64_t realtime_sequence{};
};

[[nodiscard]] native_md::DecodeStatus encode_mold_rerequest(
    const MoldRerequest& request,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] native_md::DecodeStatus decode_glimpse_snapshot(
    std::span<const std::byte> message,
    GlimpseSnapshotCut& snapshot) noexcept;

class Decoder final {
public:
    explicit Decoder(const DecoderConfig& config);
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;

    // Two-pass decoder: on failure result counts are zero and callers must
    // ignore destination contents. No payload byte is copied or retained.
    [[nodiscard]] native_md::DecodeResult decode_mold_packet(
        std::span<const std::byte> registered_packet,
        const DecodeContext& context,
        const native_md::OrderLookup& order_lookup,
        native_md::DecodeBuffers destination) noexcept;

    void clear_locates() noexcept;
    [[nodiscard]] DirectoryCommitStatus finish_directory_commit(
        std::uint64_t commit_token,
        bool application_succeeded) noexcept;
    [[nodiscard]] bool configured() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::connectors::nasdaq::itch5
