#pragma once

#include "sorx/connectors/nasdaq_itch5.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace sorx::connectors::cboe::pitch {

inline constexpr std::string_view kSpecificationTitle =
    "Cboe Titanium U.S. Equities/Options Multicast PITCH Specification";
inline constexpr std::string_view kSpecificationVersion = "2.41.74";
inline constexpr std::string_view kSpecificationRevisionDate = "2026-08-10";
inline constexpr std::string_view kSpecificationUrl =
    "https://www.cboe.com/document/tech-spec/document/"
    "technical-specifications/"
    "cboe-titanium-u.s.-equitiesoptions-multicast-pitch-specification";
inline constexpr std::string_view kVerificationDate = "2026-08-15";
inline constexpr native_md::CertificationStatus kCertificationStatus =
    native_md::CertificationStatus::Uncertified;

inline constexpr std::size_t kSequencedUnitHeaderBytes = 8U;
inline constexpr std::size_t kGapRequestMessageBytes = 9U;
inline constexpr std::size_t kGapResponseMessageBytes = 10U;
inline constexpr std::size_t kSpinRequestMessageBytes = 6U;
inline constexpr std::uint64_t kSequencesPerEpoch = 0xFFFF'FFFFULL;

enum class DecodeMode : std::uint8_t {
    RealtimeMulticast,
    GapMulticast,
    SpinServer,
    ControlSession,
};

enum class UnmappedInstrumentPolicy : std::uint8_t {
    Reject,
    // Reserved for a future bounded ignored-order directory. The current
    // foundation reports InvalidConfiguration if selected because symbol-less
    // lifecycle messages cannot otherwise be ignored safely.
    Ignore,
};

struct SymbolBinding final {
    InstrumentId instrument{};
    native_md::Symbol8 symbol{};
};

struct DecoderConfig final {
    std::uint8_t expected_unit{};
    std::span<const SymbolBinding> symbols;
    UnmappedInstrumentPolicy unmapped_policy{
        UnmappedInstrumentPolicy::Reject};
    // The 10-byte Time message and 18-byte Time Reference become applicable
    // to BZX/EDGX Equities only on the effective date stated by Cboe.
    bool allow_epoch_time_extension{};
};

struct DecodeContext final {
    marketdata::FeedLeg leg{marketdata::FeedLeg::None};
    DecodeMode mode{DecodeMode::RealtimeMulticast};
    std::uint32_t expected_sequence{};
    // One-based rollover epoch for normalizing native uint32 sequences into
    // the uint64 sequence domain consumed by FeedArbiter.
    std::uint64_t sequence_epoch{1U};
    std::uint32_t last_time_seconds{};
    bool has_time_base{};
    // Two-phase gate: false inspects framing/types/range only; true performs
    // order-state-dependent transformation after external authorization.
    bool decode_payload{};
};

struct DecodeState final {
    std::uint32_t last_time_seconds{};
    bool has_time_base{};
    std::uint32_t next_native_sequence{};
    std::uint64_t next_sequence_epoch{1U};
};

struct DecodeResult final {
    native_md::DecodeResult packet;
    DecodeState state;
};

struct GapRequest final {
    std::uint8_t unit{};
    std::uint32_t first_sequence{};
    std::uint16_t message_count{};
};

struct GapResponse final {
    std::uint8_t unit{};
    std::uint32_t first_sequence{};
    std::uint16_t message_count{};
    char status{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == 'A';
    }
};

struct SpinRequest final {
    std::uint32_t sequence{};
};

[[nodiscard]] native_md::DecodeStatus encode_gap_request_message(
    const GapRequest& request,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] native_md::DecodeStatus decode_gap_response_message(
    std::span<const std::byte> source,
    GapResponse& response) noexcept;

[[nodiscard]] native_md::DecodeStatus encode_spin_request_message(
    const SpinRequest& request,
    std::span<std::byte> destination) noexcept;

[[nodiscard]] std::uint32_t advance_sequence(
    std::uint32_t first,
    std::uint32_t count,
    std::uint32_t& rollovers) noexcept;

class Decoder final {
public:
    explicit Decoder(const DecoderConfig& config);
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;

    [[nodiscard]] DecodeResult decode_packet(
        std::span<const std::byte> registered_packet,
        const DecodeContext& context,
        const native_md::OrderLookup& order_lookup,
        native_md::DecodeBuffers destination) noexcept;

    // Cboe explicitly permits A/B packet framing to differ. Consequently the
    // packet observation is useful for health/gap detection, but overlapping
    // peer frames must be reframed/arbitrated by source_sequence per message;
    // FeedArbiter's packet ticket alone is not an A/B fill algorithm.

    [[nodiscard]] bool configured() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::connectors::cboe::pitch
