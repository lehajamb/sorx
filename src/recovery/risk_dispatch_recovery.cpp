#include "sorx/recovery/risk_dispatch_recovery.hpp"

#include "sorx/fastpath/contracts.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sorx::recovery {
namespace {

inline constexpr std::uint32_t kCheckpointMagic = 0x53525243U;  // SRRC
inline constexpr std::size_t kCheckpointFooterBytes = 36U;
inline constexpr std::uint16_t kLegacyRiskDispatchRecordVersion = 1U;
inline constexpr std::uint16_t kAdaptiveRiskDispatchRecordVersion = 2U;

// Schema coupling is intentional: a future durable-record revision must add
// an explicit decoder branch instead of being interpreted as v1, v2, or v3.
static_assert(fastpath::kRiskDispatchRecordVersion == 3U);

[[nodiscard]] constexpr bool all_zero(
    const std::span<const std::byte> bytes) noexcept {
    for (const std::byte value : bytes) {
        if (value != std::byte{}) {
            return false;
        }
    }
    return true;
}

void put_u16(std::byte* const destination, const std::uint16_t value) noexcept {
    destination[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
    destination[1] = static_cast<std::byte>(value & 0xffU);
}

void put_u32(std::byte* const destination, const std::uint32_t value) noexcept {
    destination[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    destination[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    destination[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    destination[3] = static_cast<std::byte>(value & 0xffU);
}

void put_u64(std::byte* const destination, const std::uint64_t value) noexcept {
    for (std::size_t index{}; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        destination[index] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] constexpr std::uint64_t mix_legacy_identity(
    std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] dataplane::InlineWireIdentity make_legacy_wire_identity(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t dispatch_attempt_id) noexcept {
    dataplane::InlineWireIdentity output;
    output.size = static_cast<std::uint8_t>(output.bytes.size());
    output.bytes[0U] = std::byte{'S'};
    output.bytes[1U] = std::byte{'R'};
    output.bytes[2U] = std::byte{'X'};
    output.bytes[3U] = std::byte{1U};
    put_u64(output.bytes.data() + 4U, parent.value());
    put_u64(output.bytes.data() + 12U, child.value());
    put_u64(output.bytes.data() + 20U, dispatch_attempt_id);
    const std::uint64_t checksum = mix_legacy_identity(
        parent.value() ^ child.value() ^ dispatch_attempt_id);
    for (std::size_t index{}; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((3U - index) * 8U);
        output.bytes[28U + index] = static_cast<std::byte>(checksum >> shift);
    }
    return output;
}

[[nodiscard]] std::uint16_t get_u16(
    const std::byte* const source) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(source[0])) << 8U) |
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(source[1])));
}

[[nodiscard]] std::uint32_t get_u32(
    const std::byte* const source) noexcept {
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[0])) << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[1])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[2])) << 8U) |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(source[3]));
}

[[nodiscard]] constexpr std::uint32_t rotate_right(
    const std::uint32_t value,
    const unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class Sha256 final {
public:
    void update(std::span<const std::byte> bytes) noexcept {
        total_bytes_ += bytes.size();
        while (!bytes.empty()) {
            const std::size_t available = block_.size() - block_size_;
            const std::size_t copied = std::min(available, bytes.size());
            std::copy_n(bytes.begin(), copied, block_.begin() + block_size_);
            block_size_ += copied;
            bytes = bytes.subspan(copied);
            if (block_size_ == block_.size()) {
                transform(block_);
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] recorder::HashChainDigest finish() noexcept {
        const std::uint64_t bit_length =
            static_cast<std::uint64_t>(total_bytes_) * 8U;
        block_[block_size_++] = std::byte{0x80U};
        if (block_size_ > 56U) {
            std::fill(block_.begin() + block_size_, block_.end(), std::byte{});
            transform(block_);
            block_size_ = 0U;
        }
        std::fill(
            block_.begin() + block_size_,
            block_.begin() + 56U,
            std::byte{});
        put_u64(block_.data() + 56U, bit_length);
        transform(block_);
        recorder::HashChainDigest output{};
        for (std::size_t index{}; index < state_.size(); ++index) {
            put_u32(output.data() + (index * 4U), state_[index]);
        }
        return output;
    }

private:
    void transform(const std::array<std::byte, 64U>& block) noexcept {
        static constexpr std::array<std::uint32_t, 64U> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64U> words{};
        for (std::size_t index{}; index < 16U; ++index) {
            words[index] = get_u32(block.data() + (index * 4U));
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t s0 =
                rotate_right(words[index - 15U], 7U) ^
                rotate_right(words[index - 15U], 18U) ^
                (words[index - 15U] >> 3U);
            const std::uint32_t s1 =
                rotate_right(words[index - 2U], 17U) ^
                rotate_right(words[index - 2U], 19U) ^
                (words[index - 2U] >> 10U);
            words[index] =
                words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        std::uint32_t a = state_[0U];
        std::uint32_t b = state_[1U];
        std::uint32_t c = state_[2U];
        std::uint32_t d = state_[3U];
        std::uint32_t e = state_[4U];
        std::uint32_t f = state_[5U];
        std::uint32_t g = state_[6U];
        std::uint32_t h = state_[7U];
        for (std::size_t index{}; index < words.size(); ++index) {
            const std::uint32_t upper_sigma1 =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                rotate_right(e, 25U);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + upper_sigma1 + choice + constants[index] + words[index];
            const std::uint32_t upper_sigma0 =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = upper_sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0U] += a;
        state_[1U] += b;
        state_[2U] += c;
        state_[3U] += d;
        state_[4U] += e;
        state_[5U] += f;
        state_[6U] += g;
        state_[7U] += h;
    }

    std::array<std::uint32_t, 8U> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<std::byte, 64U> block_{};
    std::size_t total_bytes_{};
    std::size_t block_size_{};
};

class Writer final {
public:
    explicit Writer(const std::span<std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8(const std::uint8_t value) noexcept {
        if (remaining() < 1U) {
            return false;
        }
        bytes_[offset_++] = static_cast<std::byte>(value);
        return true;
    }
    [[nodiscard]] bool boolean(const bool value) noexcept {
        return u8(value ? 1U : 0U);
    }
    [[nodiscard]] bool u16(const std::uint16_t value) noexcept {
        if (remaining() < 2U) {
            return false;
        }
        put_u16(bytes_.data() + offset_, value);
        offset_ += 2U;
        return true;
    }
    [[nodiscard]] bool u32(const std::uint32_t value) noexcept {
        if (remaining() < 4U) {
            return false;
        }
        put_u32(bytes_.data() + offset_, value);
        offset_ += 4U;
        return true;
    }
    [[nodiscard]] bool u64(const std::uint64_t value) noexcept {
        if (remaining() < 8U) {
            return false;
        }
        put_u64(bytes_.data() + offset_, value);
        offset_ += 8U;
        return true;
    }
    [[nodiscard]] bool i64(const std::int64_t value) noexcept {
        return u64(static_cast<std::uint64_t>(value));
    }
    [[nodiscard]] bool bytes(const std::span<const std::byte> value) noexcept {
        if (value.size() > remaining()) {
            return false;
        }
        std::copy(value.begin(), value.end(), bytes_.begin() + offset_);
        offset_ += value.size();
        return true;
    }
    [[nodiscard]] std::size_t position() const noexcept { return offset_; }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    std::span<std::byte> bytes_;
    std::size_t offset_{};
};

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& output) noexcept {
        if (remaining() < 1U) {
            return false;
        }
        output = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    [[nodiscard]] bool boolean(bool& output) noexcept {
        std::uint8_t value{};
        if (!u8(value) || value > 1U) {
            return false;
        }
        output = value != 0U;
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t& output) noexcept {
        if (remaining() < 2U) {
            return false;
        }
        output = get_u16(bytes_.data() + offset_);
        offset_ += 2U;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& output) noexcept {
        if (remaining() < 4U) {
            return false;
        }
        output = get_u32(bytes_.data() + offset_);
        offset_ += 4U;
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& output) noexcept {
        if (remaining() < 8U) {
            return false;
        }
        output = 0U;
        for (std::size_t index{}; index < 8U; ++index) {
            output = (output << 8U) |
                std::to_integer<std::uint8_t>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return true;
    }
    [[nodiscard]] bool i64(std::int64_t& output) noexcept {
        std::uint64_t value{};
        if (!u64(value)) {
            return false;
        }
        output = static_cast<std::int64_t>(value);
        return true;
    }
    [[nodiscard]] bool bytes(
        const std::size_t size,
        std::span<const std::byte>& output) noexcept {
        if (size > remaining()) {
            return false;
        }
        output = bytes_.subspan(offset_, size);
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool skip(const std::size_t size) noexcept {
        if (size > remaining()) {
            return false;
        }
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] bool token_matches_batch(
    const recorder::DurableToken& token,
    const recorder::DurableBatch& batch) noexcept {
    return recorder::verify_durable_token(token, batch);
}

[[nodiscard]] bool valid_side_value(const std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(Side::Buy) ||
           value == static_cast<std::uint8_t>(Side::Sell);
}

[[nodiscard]] bool valid_intent_value(const std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(RoutingIntent::AggressiveIOC) ||
           value == static_cast<std::uint8_t>(RoutingIntent::PassiveGTC);
}

[[nodiscard]] bool read_book_version(
    Reader& reader,
    md::BookVersion& output) noexcept {
    std::uint32_t venue{};
    std::uint32_t instrument{};
    if (!reader.u32(venue) || !reader.u32(instrument) ||
        !reader.u64(output.feed_session_epoch) ||
        !reader.u64(output.recovery_generation) ||
        !reader.u64(output.view_generation) ||
        !reader.u64(output.last_applied_sequence) ||
        !reader.u64(output.next_sequence)) {
        return false;
    }
    output.venue = VenueId{venue};
    output.instrument = InstrumentId{instrument};
    return output.venue && output.instrument &&
           output.feed_session_epoch != 0U &&
           output.recovery_generation != 0U &&
           output.view_generation != 0U && output.next_sequence != 0U &&
           output.last_applied_sequence < output.next_sequence;
}

struct PinState final {
    VenueId venue{};
    std::uint64_t adjacency_generation{};
    md::BookVersion book_version{};
    bool has_book_version{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t normal_tx_credits{};
    std::uint32_t cancel_tx_credits{};
    std::uint32_t throttle_credits{};
    std::array<std::uint64_t, 9U> adaptive_versions{};
    std::uint64_t adaptive_decision_monotonic_ns{};
    std::uint64_t adaptive_decision_utc_ns{};
    std::int64_t adaptive_executable_price{};
    std::uint64_t adaptive_available_depth{};
    std::uint64_t adaptive_quote_age_ns{};
    std::uint64_t adaptive_maximum_quote_age_ns{};
    std::uint16_t adaptive_rank{dataplane::kAdaptivePolicyUnranked};
    bool adaptive_applied{};
    bool adaptive_eligible{};
};

struct DecodedChild final {
    dataplane::RecoveredChildReservation reservation{};
    Quantity venue_outstanding_before{};
    Quantity venue_outstanding_after{};
    std::uint32_t reserved_normal_before{};
    std::uint32_t reserved_cancel_before{};
    std::uint32_t reserved_throttle_before{};
    dataplane::InlineWireIdentity wire_identity{};
    std::array<std::byte, fastpath::kMaximumEncodedFrameBytes> frame{};
    std::uint16_t frame_size{};
    std::uint8_t dispatch_class{};
};

struct DecodedRiskDispatch final {
    risk::RiskCommand command{};
    risk::RiskDelta risk_delta{};
    dataplane::RecoveredParentReservation route{};
    std::array<PinState, dataplane::kMaximumCandidatesPerInstrument> pins{};
    std::array<DecodedChild, dataplane::kMaximumCandidatesPerInstrument>
        children{};
    risk::ScopeRiskCounters client_before{};
    risk::ScopeRiskCounters client_after_full_reservation{};
    risk::ScopeRiskCounters client_after_remainder_release{};
    risk::ScopeRiskCounters account_before{};
    risk::ScopeRiskCounters account_after_full_reservation{};
    risk::ScopeRiskCounters account_after_remainder_release{};
    Quantity immediate_remainder_release{};
    std::uint16_t pin_count{};
    std::uint64_t producer_sequence{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint64_t decision_time_ns{};
    std::uint64_t commit_time_ns{};
    std::uint64_t fence_time_ns{};
    recorder::DurabilityDomain required_domain{
        recorder::DurabilityDomain::VolatileSoftwareReference};
    std::uint64_t first_child_id{};
    std::uint64_t first_attempt_id{};
    std::uint16_t dispatch_record_version{};
};

[[nodiscard]] const PinState* find_pin(
    const DecodedRiskDispatch& decoded,
    const VenueId venue) noexcept {
    for (std::size_t index{}; index < decoded.pin_count; ++index) {
        if (decoded.pins[index].venue == venue) {
            return &decoded.pins[index];
        }
    }
    return nullptr;
}

[[nodiscard]] bool read_risk_command(
    Reader& reader,
    risk::RiskCommand& command) noexcept {
    std::uint8_t kind{};
    std::span<const std::byte> digest;
    std::uint64_t parent{};
    std::uint32_t instrument{};
    std::uint8_t side{};
    if (!reader.u8(kind) || !reader.u64(command.request_id) ||
        !reader.bytes(command.payload_digest.size(), digest) ||
        !reader.u64(command.client_id) || !reader.u64(command.account_id) ||
        !reader.u64(parent) || !reader.u32(instrument) || !reader.u8(side) ||
        !reader.u64(command.quantity.units) ||
        !reader.i64(command.limit_price.units) ||
        kind > static_cast<std::uint8_t>(risk::RiskCommandKind::Cancel) ||
        !valid_side_value(side)) {
        return false;
    }
    std::copy(
        digest.begin(), digest.end(), command.payload_digest.begin());
    command.kind = static_cast<risk::RiskCommandKind>(kind);
    command.parent_order_id = ParentOrderId{parent};
    command.instrument = InstrumentId{instrument};
    command.side = static_cast<Side>(side);
    return command.request_id != 0U && command.client_id != 0U &&
           command.account_id != 0U && command.parent_order_id &&
           command.instrument;
}

[[nodiscard]] bool read_scope_counters(
    Reader& reader,
    risk::ScopeRiskCounters& counters) noexcept {
    return reader.boolean(counters.found) &&
           reader.u64(counters.open_quantity.units) &&
           reader.u64(counters.open_notional.units) &&
           reader.u32(counters.open_orders) &&
           reader.u32(counters.normal_tokens) &&
           reader.u32(counters.cancel_tokens);
}

[[nodiscard]] bool decode_command_record(
    const recorder::DurableRecordView& record,
    DecodedRiskDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader{record.payload};
    std::uint32_t magic{};
    std::uint16_t payload_version{};
    std::uint8_t domain{};
    std::uint8_t intent{};
    if (!reader.u32(magic) || !reader.u16(payload_version) ||
        !reader.u64(decoded.producer_sequence) ||
        !reader.u64(decoded.leadership_epoch) ||
        !reader.u64(decoded.configuration_generation) ||
        !reader.u64(decoded.decision_time_ns) ||
        !reader.u64(decoded.commit_time_ns) ||
        !reader.u64(decoded.fence_time_ns) || !reader.u8(domain) ||
        !read_risk_command(reader, decoded.command) ||
        !reader.u8(intent) || !reader.done()) {
        return false;
    }
    const risk::RiskCommand& command = decoded.command;
    if (magic != fastpath::kRiskDispatchSchemaMagic ||
        payload_version != dispatch_record_version ||
        decoded.producer_sequence == 0U || decoded.leadership_epoch == 0U ||
        decoded.configuration_generation == 0U ||
        decoded.decision_time_ns == 0U ||
        decoded.commit_time_ns < decoded.decision_time_ns ||
        decoded.fence_time_ns == 0U || domain > 1U ||
        command.kind != risk::RiskCommandKind::NewOrder ||
        !command.quantity || command.limit_price.units <= 0 ||
        !valid_intent_value(intent)) {
        return false;
    }
    decoded.required_domain =
        static_cast<recorder::DurabilityDomain>(domain);
    decoded.route.parent = ParentOrder{
        command.parent_order_id,
        command.instrument,
        command.side,
        command.quantity,
        command.limit_price,
        static_cast<RoutingIntent>(intent)};
    return true;
}

[[nodiscard]] bool risk_commands_equal(
    const risk::RiskCommand& left,
    const risk::RiskCommand& right) noexcept {
    return left.kind == right.kind && left.request_id == right.request_id &&
           left.payload_digest == right.payload_digest &&
           left.client_id == right.client_id &&
           left.account_id == right.account_id &&
           left.parent_order_id == right.parent_order_id &&
           left.instrument == right.instrument && left.side == right.side &&
           left.quantity == right.quantity &&
           left.limit_price == right.limit_price;
}

[[nodiscard]] bool counters_equal(
    const risk::ScopeRiskCounters& left,
    const risk::ScopeRiskCounters& right) noexcept {
    return left.found == right.found &&
           left.open_quantity == right.open_quantity &&
           left.open_notional == right.open_notional &&
           left.open_orders == right.open_orders &&
           left.normal_tokens == right.normal_tokens &&
           left.cancel_tokens == right.cancel_tokens;
}

[[nodiscard]] bool decode_risk_record(
    const recorder::DurableRecordView& record,
    DecodedRiskDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader{record.payload};
    std::uint8_t disposition{};
    std::uint8_t verdict{};
    risk::RiskCommand command{};
    std::uint64_t metadata_risk_generation{};
    std::uint64_t metadata_configuration_generation{};
    std::uint64_t metadata_state_generation{};
    std::uint64_t metadata_last_time{};
    bool release_required{};
    std::uint64_t release_parent{};
    std::uint64_t release_notional{};
    std::uint8_t release_status{};
    std::uint64_t state_after_full{};
    std::uint64_t state_after_final{};
    if (!reader.u8(disposition) || !reader.u8(verdict) ||
        !read_risk_command(reader, command) ||
        !reader.u64(decoded.risk_delta.command_fingerprint) ||
        !reader.u64(decoded.risk_delta.risk_generation) ||
        !reader.u64(decoded.risk_delta.configuration_generation) ||
        !reader.u64(decoded.risk_delta.kernel_state_generation) ||
        !reader.u64(decoded.risk_delta.monotonic_time_ns) ||
        !reader.u64(decoded.risk_delta.reservation_quantity.units) ||
        !reader.u64(decoded.risk_delta.reservation_notional.units) ||
        !reader.u32(decoded.risk_delta.normal_tokens_after) ||
        !reader.u32(decoded.risk_delta.cancel_tokens_after) ||
        !reader.u64(decoded.risk_delta.normal_refill_time_after) ||
        !reader.u64(decoded.risk_delta.cancel_refill_time_after) ||
        !reader.u64(decoded.risk_delta.authentication_tag) ||
        !reader.u64(metadata_risk_generation) ||
        !reader.u64(metadata_configuration_generation) ||
        !reader.u64(metadata_state_generation) ||
        !reader.u64(metadata_last_time) ||
        !read_scope_counters(reader, decoded.client_before) ||
        !read_scope_counters(
            reader, decoded.client_after_full_reservation) ||
        !read_scope_counters(reader, decoded.account_before) ||
        !read_scope_counters(
            reader, decoded.account_after_full_reservation)) {
        return false;
    }
    decoded.risk_delta.disposition =
        static_cast<risk::RiskPlanDisposition>(disposition);
    decoded.risk_delta.verdict = static_cast<risk::RiskVerdict>(verdict);
    decoded.risk_delta.command = command;
    if (disposition !=
            static_cast<std::uint8_t>(risk::RiskPlanDisposition::Planned) ||
        verdict != static_cast<std::uint8_t>(risk::RiskVerdict::Allowed) ||
        !risk_commands_equal(command, decoded.command) ||
        decoded.risk_delta.risk_generation == 0U ||
        decoded.risk_delta.configuration_generation == 0U ||
        decoded.risk_delta.kernel_state_generation == 0U ||
        decoded.risk_delta.monotonic_time_ns != decoded.decision_time_ns ||
        decoded.risk_delta.reservation_quantity != command.quantity ||
        metadata_risk_generation != decoded.risk_delta.risk_generation ||
        metadata_configuration_generation !=
            decoded.risk_delta.configuration_generation ||
        metadata_state_generation !=
            decoded.risk_delta.kernel_state_generation ||
        metadata_last_time > decoded.risk_delta.monotonic_time_ns ||
        !decoded.client_before.found || !decoded.account_before.found ||
        !decoded.client_after_full_reservation.found ||
        !decoded.account_after_full_reservation.found ||
        decoded.risk_delta.kernel_state_generation ==
            std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto reservation_notional =
        checked_notional(command.limit_price, command.quantity);
    if (!reservation_notional ||
        *reservation_notional != decoded.risk_delta.reservation_notional) {
        return false;
    }
    const auto client_full_quantity = checked_add(
        decoded.client_before.open_quantity,
        decoded.risk_delta.reservation_quantity);
    const auto account_full_quantity = checked_add(
        decoded.account_before.open_quantity,
        decoded.risk_delta.reservation_quantity);
    if (!client_full_quantity || !account_full_quantity ||
        decoded.risk_delta.reservation_notional.units >
            std::numeric_limits<std::uint64_t>::max() -
                decoded.client_before.open_notional.units ||
        decoded.risk_delta.reservation_notional.units >
            std::numeric_limits<std::uint64_t>::max() -
                decoded.account_before.open_notional.units ||
        decoded.client_before.open_orders ==
            std::numeric_limits<std::uint32_t>::max() ||
        decoded.account_before.open_orders ==
            std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const risk::ScopeRiskCounters expected_client_full{
        true,
        *client_full_quantity,
        Notional{decoded.client_before.open_notional.units +
                 decoded.risk_delta.reservation_notional.units},
        decoded.client_before.open_orders + 1U,
        decoded.risk_delta.normal_tokens_after,
        decoded.risk_delta.cancel_tokens_after};
    const risk::ScopeRiskCounters expected_account_full{
        true,
        *account_full_quantity,
        Notional{decoded.account_before.open_notional.units +
                 decoded.risk_delta.reservation_notional.units},
        decoded.account_before.open_orders + 1U,
        0U,
        0U};
    if (!counters_equal(
            decoded.client_after_full_reservation, expected_client_full) ||
        !counters_equal(
            decoded.account_after_full_reservation, expected_account_full)) {
        return false;
    }

    if (dispatch_record_version == kLegacyRiskDispatchRecordVersion) {
        if (!reader.done()) {
            return false;
        }
        decoded.client_after_remainder_release =
            decoded.client_after_full_reservation;
        decoded.account_after_remainder_release =
            decoded.account_after_full_reservation;
        return true;
    }
    if ((dispatch_record_version != kAdaptiveRiskDispatchRecordVersion &&
         dispatch_record_version != fastpath::kRiskDispatchRecordVersion) ||
        !reader.boolean(release_required) || !reader.u64(release_parent) ||
        !reader.u64(decoded.immediate_remainder_release.units) ||
        !reader.u64(release_notional) || !reader.u8(release_status) ||
        !reader.u64(state_after_full) || !reader.u64(state_after_final) ||
        !read_scope_counters(
            reader, decoded.client_after_remainder_release) ||
        !read_scope_counters(
            reader, decoded.account_after_remainder_release) ||
        !reader.done() ||
        !decoded.client_after_remainder_release.found ||
        !decoded.account_after_remainder_release.found ||
        release_parent != command.parent_order_id.value() ||
        state_after_full !=
            decoded.risk_delta.kernel_state_generation + 1U) {
        return false;
    }

    if (!release_required) {
        return decoded.immediate_remainder_release.units == 0U &&
               release_notional == 0U &&
               release_status == static_cast<std::uint8_t>(
                   fastpath::RemainderAccountingStatus::NotRequired) &&
               state_after_final == state_after_full &&
               counters_equal(
                   decoded.client_after_remainder_release,
                   decoded.client_after_full_reservation) &&
               counters_equal(
                   decoded.account_after_remainder_release,
                   decoded.account_after_full_reservation);
    }
    const auto expected_release_notional = checked_notional(
        command.limit_price, decoded.immediate_remainder_release);
    if (!decoded.immediate_remainder_release ||
        decoded.immediate_remainder_release >=
            decoded.risk_delta.reservation_quantity ||
        !expected_release_notional ||
        expected_release_notional->units != release_notional ||
        release_status != static_cast<std::uint8_t>(
                              fastpath::RemainderAccountingStatus::Applied) ||
        state_after_full == std::numeric_limits<std::uint64_t>::max() ||
        state_after_final != state_after_full + 1U ||
        expected_client_full.open_quantity <
            decoded.immediate_remainder_release ||
        expected_account_full.open_quantity <
            decoded.immediate_remainder_release ||
        expected_client_full.open_notional < *expected_release_notional ||
        expected_account_full.open_notional < *expected_release_notional) {
        return false;
    }
    const risk::ScopeRiskCounters expected_client_final{
        true,
        Quantity{expected_client_full.open_quantity.units -
                 decoded.immediate_remainder_release.units},
        Notional{expected_client_full.open_notional.units -
                 expected_release_notional->units},
        expected_client_full.open_orders,
        expected_client_full.normal_tokens,
        expected_client_full.cancel_tokens};
    const risk::ScopeRiskCounters expected_account_final{
        true,
        Quantity{expected_account_full.open_quantity.units -
                 decoded.immediate_remainder_release.units},
        Notional{expected_account_full.open_notional.units -
                 expected_release_notional->units},
        expected_account_full.open_orders,
        0U,
        0U};
    return counters_equal(
               decoded.client_after_remainder_release,
               expected_client_final) &&
           counters_equal(
               decoded.account_after_remainder_release,
               expected_account_final);
}

// Risk-dispatch v1 predates the explicit authenticated remainder operation.
// Its authenticated route record is nevertheless unambiguous for a partial
// aggressive IOC, so recovery migrates that historical batch into the current
// allocated-only conservation model. Passive/non-IOC remainders are rejected
// rather than guessed.
[[nodiscard]] bool derive_legacy_remainder_release(
    DecodedRiskDispatch& decoded,
    const Quantity authenticated_remainder) noexcept {
    if (decoded.dispatch_record_version !=
        kLegacyRiskDispatchRecordVersion) {
        return decoded.immediate_remainder_release == authenticated_remainder;
    }
    if (!authenticated_remainder) {
        decoded.client_after_remainder_release =
            decoded.client_after_full_reservation;
        decoded.account_after_remainder_release =
            decoded.account_after_full_reservation;
        return true;
    }
    if (decoded.route.parent.intent != RoutingIntent::AggressiveIOC ||
        authenticated_remainder >=
            decoded.risk_delta.reservation_quantity ||
        decoded.risk_delta.kernel_state_generation >
            std::numeric_limits<std::uint64_t>::max() - 2U) {
        return false;
    }
    const auto release_notional = checked_notional(
        decoded.command.limit_price, authenticated_remainder);
    if (!release_notional ||
        decoded.client_after_full_reservation.open_quantity <
            authenticated_remainder ||
        decoded.account_after_full_reservation.open_quantity <
            authenticated_remainder ||
        decoded.client_after_full_reservation.open_notional <
            *release_notional ||
        decoded.account_after_full_reservation.open_notional <
            *release_notional) {
        return false;
    }
    decoded.immediate_remainder_release = authenticated_remainder;
    decoded.client_after_remainder_release = risk::ScopeRiskCounters{
        true,
        Quantity{
            decoded.client_after_full_reservation.open_quantity.units -
            authenticated_remainder.units},
        Notional{
            decoded.client_after_full_reservation.open_notional.units -
            release_notional->units},
        decoded.client_after_full_reservation.open_orders,
        decoded.client_after_full_reservation.normal_tokens,
        decoded.client_after_full_reservation.cancel_tokens};
    decoded.account_after_remainder_release = risk::ScopeRiskCounters{
        true,
        Quantity{
            decoded.account_after_full_reservation.open_quantity.units -
            authenticated_remainder.units},
        Notional{
            decoded.account_after_full_reservation.open_notional.units -
            release_notional->units},
        decoded.account_after_full_reservation.open_orders,
        decoded.account_after_full_reservation.normal_tokens,
        decoded.account_after_full_reservation.cancel_tokens};
    return true;
}

[[nodiscard]] bool read_adaptive_policy_overlay(
    Reader& reader,
    const ParentOrder& parent,
    const std::uint64_t route_decision_time_ns,
    const std::uint64_t risk_generation,
    PinState& pin) noexcept {
    bool applied{};
    if (!reader.boolean(applied)) {
        return false;
    }
    pin.adaptive_applied = applied;
    if (!applied) {
        return true;
    }
    bool eligible{};
    std::uint16_t rank{};
    std::uint8_t exclusion{};
    if (!reader.boolean(eligible) || !reader.u16(rank) ||
        !reader.u8(exclusion)) {
        return false;
    }
    std::array<std::uint64_t, 9U> versions{};
    for (std::uint64_t& value : versions) {
        if (!reader.u64(value)) {
            return false;
        }
    }
    pin.adaptive_versions = versions;
    std::uint64_t parent_id{};
    std::uint32_t instrument{};
    std::uint8_t side{};
    std::uint8_t intent{};
    std::uint64_t quantity{};
    std::int64_t limit_price{};
    std::uint64_t decision_monotonic{};
    std::uint64_t decision_utc{};
    if (!reader.u64(parent_id) || !reader.u32(instrument) ||
        !reader.u8(side) || !reader.u8(intent) || !reader.u64(quantity) ||
        !reader.i64(limit_price) || !reader.u64(decision_monotonic) ||
        !reader.u64(decision_utc)) {
        return false;
    }
    pin.adaptive_decision_monotonic_ns = decision_monotonic;
    pin.adaptive_decision_utc_ns = decision_utc;
    std::uint32_t fact_venue{};
    std::array<std::uint64_t, 9U> fact_versions{};
    if (!reader.u32(fact_venue)) {
        return false;
    }
    for (std::uint64_t& value : fact_versions) {
        if (!reader.u64(value)) {
            return false;
        }
    }
    if (parent_id != parent.id.value() ||
        instrument != parent.instrument.value() ||
        side != static_cast<std::uint8_t>(parent.side) ||
        intent != static_cast<std::uint8_t>(parent.intent) ||
        quantity != parent.quantity.units ||
        limit_price != parent.limit_price.units ||
        decision_monotonic == 0U ||
        decision_monotonic > route_decision_time_ns || decision_utc == 0U ||
        fact_venue != pin.venue.value() ||
        exclusion > static_cast<std::uint8_t>(
            policy::PolicyExclusionReason::PassiveMarketableProtected) ||
        std::any_of(versions.begin(), versions.begin() + 7U,
                    [](const std::uint64_t value) { return value == 0U; }) ||
        (versions[8U] != 0U && versions[8U] <= versions[7U]) ||
        decision_utc < versions[7U] ||
        (versions[8U] != 0U && decision_utc >= versions[8U])) {
        return false;
    }
    if (!eligible) {
        pin.adaptive_rank = rank;
        return rank == dataplane::kAdaptivePolicyUnranked &&
               exclusion != static_cast<std::uint8_t>(
                                policy::PolicyExclusionReason::None);
    }
    if (!pin.has_book_version || fact_versions[0U] == 0U ||
        fact_versions[1U] == 0U || fact_versions[2U] == 0U ||
        fact_versions[3U] == 0U || fact_versions[4U] == 0U ||
        fact_versions[5U] == 0U || fact_versions[7U] == 0U ||
        fact_versions[8U] == 0U ||
        fact_versions[0U] != pin.adjacency_generation ||
        fact_versions[1U] != pin.book_version.view_generation ||
        fact_versions[3U] != risk_generation ||
        fact_versions[4U] != pin.throttle_generation ||
        fact_versions[5U] != pin.tx_credit_generation ||
        fact_versions[7U] != versions[1U] ||
        fact_versions[8U] != versions[2U]) {
        return false;
    }
    std::uint32_t evidence_venue{};
    std::int64_t executable_price{};
    std::uint64_t depth{};
    std::uint64_t quote_age{};
    std::uint64_t maximum_age{};
    std::uint32_t throttle{};
    std::uint32_t normal{};
    bool protected_healthy{};
    bool protected_present{};
    std::int64_t protected_price{};
    std::uint32_t fee{};
    std::uint32_t fill_quality{};
    std::uint32_t adverse_selection{};
    std::uint32_t reject_rate{};
    std::uint64_t latency{};
    std::array<std::int64_t, 7U> scores{};
    bool saturated{};
    std::uint32_t priority{};
    if (!reader.u32(evidence_venue) || !reader.i64(executable_price) ||
        !reader.u64(depth) || !reader.u64(quote_age) ||
        !reader.u64(maximum_age) || !reader.u32(throttle) ||
        !reader.u32(normal) || !reader.boolean(protected_healthy) ||
        !reader.boolean(protected_present) ||
        !reader.i64(protected_price) || !reader.u32(fee) ||
        !reader.u32(fill_quality) || !reader.u32(adverse_selection) ||
        !reader.u32(reject_rate) || !reader.u64(latency)) {
        return false;
    }
    for (std::int64_t& value : scores) {
        if (!reader.i64(value)) {
            return false;
        }
    }
    const std::int32_t signed_fee = static_cast<std::int32_t>(fee);
    constexpr std::int32_t maximum_fee =
        static_cast<std::int32_t>(policy::kMetricPartsPerMillion);
    (void)protected_healthy;
    (void)latency;
    (void)scores;
    if (!reader.boolean(saturated) || !reader.u32(priority) ||
        evidence_venue != pin.venue.value() ||
        signed_fee < -maximum_fee || signed_fee > maximum_fee ||
        rank >= dataplane::kMaximumCandidatesPerInstrument ||
        executable_price <= 0 || depth == 0U || maximum_age == 0U ||
        quote_age > maximum_age ||
        fill_quality > policy::kMetricPartsPerMillion ||
        adverse_selection > policy::kMetricPartsPerMillion ||
        reject_rate > policy::kMetricPartsPerMillion ||
        throttle != pin.throttle_credits || normal != pin.normal_tx_credits ||
        (protected_present && protected_price <= 0) ||
        exclusion !=
            static_cast<std::uint8_t>(policy::PolicyExclusionReason::None)) {
        return false;
    }
    pin.adaptive_rank = rank;
    pin.adaptive_eligible = true;
    pin.adaptive_executable_price = executable_price;
    pin.adaptive_available_depth = depth;
    pin.adaptive_quote_age_ns = quote_age;
    pin.adaptive_maximum_quote_age_ns = maximum_age;
    return true;
}

[[nodiscard]] bool decode_pinned_record(
    const recorder::DurableRecordView& record,
    DecodedRiskDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader{record.payload};
    if (!reader.u16(decoded.pin_count) || decoded.pin_count == 0U ||
        decoded.pin_count > decoded.pins.size()) {
        return false;
    }
    for (std::size_t index{}; index < decoded.pin_count; ++index) {
        PinState& pin = decoded.pins[index];
        std::uint32_t venue{};
        std::uint64_t adjacency{};
        bool has_version{};
        md::BookVersion pinned_version{};
        std::uint64_t feed_epoch{};
        std::uint64_t source_generation{};
        if (!reader.u32(venue) || !reader.u64(adjacency) ||
            !reader.boolean(has_version)) {
            return false;
        }
        pin.venue = VenueId{venue};
        pin.adjacency_generation = adjacency;
        pin.has_book_version = has_version;
        if (!pin.venue) {
            return false;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (decoded.pins[prior].venue == pin.venue) {
                return false;
            }
        }
        if (has_version &&
            (!read_book_version(reader, pinned_version) ||
             pinned_version.venue != pin.venue ||
             pinned_version.instrument != decoded.route.parent.instrument)) {
            return false;
        }
        pin.book_version = pinned_version;
        if (!reader.u64(feed_epoch) || !reader.u64(source_generation) ||
            !reader.u64(pin.tx_credit_generation) ||
            !reader.u64(pin.throttle_generation) ||
            !reader.u32(pin.normal_tx_credits) ||
            !reader.u32(pin.cancel_tx_credits) ||
            !reader.u32(pin.throttle_credits)) {
            return false;
        }
        if (dispatch_record_version >=
                kAdaptiveRiskDispatchRecordVersion &&
            !read_adaptive_policy_overlay(
                reader,
                decoded.route.parent,
                decoded.decision_time_ns,
                decoded.risk_delta.risk_generation,
                pin)) {
            return false;
        }

        bool view_present{};
        if (!reader.boolean(view_present)) {
            return false;
        }
        if (!view_present) {
            if (adjacency != 0U || has_version || feed_epoch != 0U ||
                source_generation != 0U || pin.tx_credit_generation != 0U ||
                pin.throttle_generation != 0U ||
                pin.normal_tx_credits != 0U || pin.cancel_tx_credits != 0U ||
                pin.throttle_credits != 0U || pin.adaptive_applied) {
                return false;
            }
            continue;
        }
        std::uint32_t view_venue{};
        std::uint32_t view_instrument{};
        std::uint64_t view_adjacency{};
        std::uint8_t market_state{};
        std::uint8_t order_entry_state{};
        std::uint8_t egress_state{};
        std::uint8_t instrument_state{};
        std::uint64_t view_feed_epoch{};
        std::uint64_t view_source_generation{};
        std::uint64_t receive_time{};
        std::uint64_t publication_time{};
        std::uint64_t maximum_age{};
        std::uint64_t view_tx_generation{};
        std::uint64_t view_throttle_generation{};
        std::uint32_t view_normal{};
        std::uint32_t view_cancel{};
        std::uint32_t view_throttle{};
        bool has_book{};
        bool source_healthy{};
        bool conversion_compatible{};
        bool risk_allowed{};
        if (!reader.u32(view_venue) || !reader.u32(view_instrument) ||
            !reader.u64(view_adjacency) || !reader.u8(market_state) ||
            !reader.u8(order_entry_state) || !reader.u8(egress_state) ||
            !reader.u8(instrument_state) || !reader.u64(view_feed_epoch) ||
            !reader.u64(view_source_generation) || !reader.u64(receive_time) ||
            !reader.u64(publication_time) || !reader.u64(maximum_age) ||
            !reader.u64(view_tx_generation) ||
            !reader.u64(view_throttle_generation) ||
            !reader.u32(view_normal) || !reader.u32(view_cancel) ||
            !reader.u32(view_throttle) || !reader.boolean(has_book) ||
            !reader.boolean(source_healthy) ||
            !reader.boolean(conversion_compatible) ||
            !reader.boolean(risk_allowed)) {
            return false;
        }
        if (view_venue != venue ||
            view_instrument != decoded.route.parent.instrument.value() ||
            view_adjacency != adjacency || market_state > 4U ||
            order_entry_state > 3U || egress_state > 1U ||
            instrument_state > 3U || view_feed_epoch != feed_epoch ||
            view_source_generation != source_generation ||
            view_tx_generation != pin.tx_credit_generation ||
            view_throttle_generation != pin.throttle_generation ||
            view_normal != pin.normal_tx_credits ||
            view_cancel != pin.cancel_tx_credits ||
            view_throttle != pin.throttle_credits || adjacency == 0U ||
            source_generation == 0U || pin.tx_credit_generation == 0U ||
            pin.throttle_generation == 0U || receive_time == 0U ||
            publication_time < receive_time || maximum_age == 0U ||
            has_book != has_version || (!has_book && source_healthy) ||
            (pin.adaptive_eligible &&
             (!has_book ||
              pin.adaptive_decision_monotonic_ns < receive_time ||
              pin.adaptive_quote_age_ns !=
                  pin.adaptive_decision_monotonic_ns - receive_time ||
              pin.adaptive_maximum_quote_age_ns != maximum_age))) {
            return false;
        }
        (void)source_healthy;
        (void)conversion_compatible;
        (void)risk_allowed;
        if (has_book) {
            md::BookVersion view_version{};
            std::uint32_t channel{};
            std::uint32_t profile{};
            std::int64_t exchange_time{};
            std::uint64_t local_receive{};
            std::uint64_t local_publication{};
            std::uint8_t feed_state{};
            std::uint8_t trading_state{};
            bool truncated{};
            if (!read_book_version(reader, view_version) ||
                !reader.u32(channel) || !reader.u32(profile) ||
                !reader.i64(exchange_time) || !reader.u64(local_receive) ||
                !reader.u64(local_publication) || !reader.u8(feed_state) ||
                !reader.u8(trading_state) || !reader.boolean(truncated) ||
                channel == 0U || profile == 0U || local_receive == 0U ||
                local_publication == 0U || feed_state > 4U ||
                trading_state > 3U || view_version.venue != pin.venue ||
                view_version.instrument != decoded.route.parent.instrument ||
                view_version != pinned_version ||
                view_version.feed_session_epoch != feed_epoch) {
                return false;
            }
            (void)exchange_time;
            (void)truncated;
        }
    }

    std::size_t adaptive_count{};
    std::size_t eligible_count{};
    const PinState* adaptive_reference = nullptr;
    std::array<bool, dataplane::kMaximumCandidatesPerInstrument> rank_seen{};
    for (std::size_t index{}; index < decoded.pin_count; ++index) {
        const PinState& pin = decoded.pins[index];
        if (!pin.adaptive_applied) {
            continue;
        }
        ++adaptive_count;
        if (adaptive_reference == nullptr) {
            adaptive_reference = &pin;
        } else if (
            pin.adaptive_versions != adaptive_reference->adaptive_versions ||
            pin.adaptive_decision_monotonic_ns !=
                adaptive_reference->adaptive_decision_monotonic_ns ||
            pin.adaptive_decision_utc_ns !=
                adaptive_reference->adaptive_decision_utc_ns) {
            return false;
        }
        if (!pin.adaptive_eligible) {
            continue;
        }
        if (pin.adaptive_rank >= decoded.pin_count ||
            rank_seen[pin.adaptive_rank]) {
            return false;
        }
        rank_seen[pin.adaptive_rank] = true;
        ++eligible_count;
    }
    if (adaptive_count != 0U && adaptive_count != decoded.pin_count) {
        return false;
    }
    for (std::size_t rank{}; rank < eligible_count; ++rank) {
        if (!rank_seen[rank]) {
            return false;
        }
    }

    std::uint16_t slice_count{};
    if (!reader.u16(slice_count) ||
        slice_count > fastpath::kMaximumRecordedDepthSlices) {
        return false;
    }
    const Side expected_side =
        decoded.route.parent.side == Side::Buy ? Side::Sell : Side::Buy;
    for (std::size_t index{}; index < slice_count; ++index) {
        std::uint32_t venue{};
        std::uint8_t side{};
        std::uint16_t level{};
        std::int64_t price{};
        std::uint64_t quantity{};
        std::uint32_t order_count{};
        if (!reader.u32(venue) || !reader.u8(side) || !reader.u16(level) ||
            !reader.i64(price) || !reader.u64(quantity) ||
            !reader.u32(order_count) || venue == 0U ||
            side != static_cast<std::uint8_t>(expected_side) ||
            level >= md::kMaxDepthLevels || price <= 0 || quantity == 0U ||
            order_count == 0U ||
            find_pin(decoded, VenueId{venue}) == nullptr) {
            return false;
        }
    }
    return reader.done();
}

[[nodiscard]] bool decode_route_record(
    const recorder::DurableRecordView& record,
    DecodedRiskDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader{record.payload};
    std::uint64_t decision_time{};
    std::uint64_t allocated{};
    std::uint64_t remainder{};
    std::uint8_t remainder_reason{};
    std::uint8_t reject_reason{};
    bool truncated{};
    std::uint16_t exclusion_count{};
    if (!reader.u64(decoded.route.kernel_state_generation) ||
        !reader.u64(decoded.route.candidate_table_generation) ||
        !reader.u64(decoded.route.risk_generation) ||
        !reader.u64(decision_time) ||
        !reader.u64(decoded.first_child_id) ||
        !reader.u64(decoded.first_attempt_id) ||
        !reader.u64(decoded.route.instrument_outstanding_before.units) ||
        !reader.u64(decoded.route.instrument_outstanding_after.units) ||
        !reader.u64(decoded.route.instrument_open_parents_before) ||
        !reader.u64(decoded.route.instrument_open_parents_after) ||
        !reader.u64(decoded.route.instrument_open_children_before) ||
        !reader.u64(decoded.route.instrument_open_children_after) ||
        !reader.u64(allocated) || !reader.u64(remainder) ||
        !reader.u8(remainder_reason) || !reader.u8(reject_reason) ||
        !reader.boolean(truncated) || !reader.u16(exclusion_count)) {
        return false;
    }
    if (decoded.route.kernel_state_generation == 0U ||
        decoded.route.candidate_table_generation == 0U ||
        decoded.route.risk_generation == 0U ||
        decision_time != decoded.decision_time_ns ||
        decoded.first_child_id == 0U || decoded.first_attempt_id == 0U ||
        remainder_reason >
            static_cast<std::uint8_t>(
                RemainderReason::PassiveFullQuantityUnavailable) ||
        reject_reason != static_cast<std::uint8_t>(RouteRejectReason::None) ||
        exclusion_count > dataplane::kMaximumCandidatesPerInstrument) {
        return false;
    }
    for (std::size_t index{}; index < exclusion_count; ++index) {
        std::uint32_t venue{};
        std::uint8_t reason{};
        if (!reader.u32(venue) || !reader.u8(reason) || venue == 0U ||
            reason >
                static_cast<std::uint8_t>(
                    CandidateExclusionReason::MaximumVenueCount) ||
            find_pin(decoded, VenueId{venue}) == nullptr) {
            return false;
        }
    }
    if (!reader.u16(decoded.route.child_count) ||
        decoded.route.child_count == 0U ||
        decoded.route.child_count > decoded.children.size()) {
        return false;
    }
    Quantity summed{};
    for (std::size_t index{}; index < decoded.route.child_count; ++index) {
        DecodedChild& child = decoded.children[index];
        std::uint32_t venue{};
        std::uint64_t quantity{};
        std::int64_t limit{};
        std::uint64_t venue_quantity{};
        std::int64_t venue_limit{};
        std::uint32_t allocation_order{};
        std::uint64_t adjacency_generation{};
        md::BookVersion book_version{};
        std::uint64_t child_id{};
        std::uint8_t wire_size{};
        std::span<const std::byte> wire;
        if (!reader.u32(venue) || !reader.u64(quantity) ||
            !reader.i64(limit) || !reader.u64(venue_quantity) ||
            !reader.i64(venue_limit) || !reader.u32(allocation_order) ||
            !reader.u64(adjacency_generation) ||
            !read_book_version(reader, book_version) ||
            !reader.u64(child.venue_outstanding_before.units) ||
            !reader.u64(child.venue_outstanding_after.units) ||
            !reader.u32(child.reserved_normal_before) ||
            !reader.u32(child.reserved_cancel_before) ||
            !reader.u32(child.reserved_throttle_before) ||
            !reader.u64(child_id) ||
            !reader.u64(child.reservation.dispatch_attempt_id) ||
            !reader.u8(wire_size) || wire_size == 0U ||
            wire_size > child.wire_identity.bytes.size() ||
            !reader.bytes(wire_size, wire) ||
            !reader.u64(child.reservation.tx_credit_generation) ||
            !reader.u64(child.reservation.throttle_generation) ||
            !reader.u16(child.reservation.normal_tx_credit_delta) ||
            !reader.u16(child.reservation.cancel_tx_credit_delta) ||
            !reader.u16(child.reservation.throttle_credit_delta)) {
            return false;
        }
        child.reservation.child_order_id = ChildOrderId{child_id};
        child.reservation.venue = VenueId{venue};
        child.reservation.remaining_quantity = Quantity{quantity};
        child.wire_identity.size = wire_size;
        std::copy(wire.begin(), wire.end(), child.wire_identity.bytes.begin());
        const PinState* const pin = find_pin(decoded, child.reservation.venue);
        if (!child.reservation.child_order_id ||
            child.reservation.dispatch_attempt_id == 0U ||
            !child.reservation.venue || !child.reservation.remaining_quantity ||
            limit <= 0 || venue_quantity == 0U || venue_limit <= 0 ||
            allocation_order >= decoded.route.child_count ||
            adjacency_generation == 0U || pin == nullptr ||
            adjacency_generation != pin->adjacency_generation ||
            !pin->has_book_version || book_version != pin->book_version ||
            book_version.venue != child.reservation.venue ||
            book_version.instrument != decoded.route.parent.instrument ||
            child.reservation.tx_credit_generation !=
                pin->tx_credit_generation ||
            child.reservation.throttle_generation !=
                pin->throttle_generation ||
            child.reservation.normal_tx_credit_delta != 1U ||
            child.reservation.cancel_tx_credit_delta != 0U ||
            child.reservation.throttle_credit_delta != 1U ||
            child_id != decoded.first_child_id + index ||
            child.reservation.dispatch_attempt_id !=
                decoded.first_attempt_id + index) {
            return false;
        }
        child.reservation.reported_normal_tx_credits =
            pin->normal_tx_credits;
        child.reservation.reported_cancel_tx_credits =
            pin->cancel_tx_credits;
        child.reservation.reported_throttle_credits = pin->throttle_credits;
        const auto venue_after = checked_add(
            child.venue_outstanding_before,
            child.reservation.remaining_quantity);
        const auto next_sum = checked_add(
            summed, child.reservation.remaining_quantity);
        if (!venue_after || *venue_after != child.venue_outstanding_after ||
            !next_sum) {
            return false;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (decoded.children[prior].reservation.venue ==
                    child.reservation.venue ||
                decoded.children[prior].reservation.child_order_id ==
                    child.reservation.child_order_id ||
                decoded.children[prior].reservation.dispatch_attempt_id ==
                    child.reservation.dispatch_attempt_id) {
                return false;
            }
        }
        summed = *next_sum;
        decoded.route.children[index] = child.reservation;
        decoded.route.venue_outstanding_before[index] =
            child.venue_outstanding_before;
        decoded.route.venue_outstanding_after[index] =
            child.venue_outstanding_after;
    }
    const auto instrument_after = checked_add(
        decoded.route.instrument_outstanding_before, Quantity{allocated});
    const auto parent_total =
        checked_add(Quantity{allocated}, Quantity{remainder});
    decoded.dispatch_record_version = dispatch_record_version;
    return derive_legacy_remainder_release(decoded, Quantity{remainder}) &&
           reader.done() && summed == Quantity{allocated} &&
           instrument_after &&
           *instrument_after == decoded.route.instrument_outstanding_after &&
           parent_total && *parent_total == decoded.route.parent.quantity &&
           decoded.route.instrument_open_parents_before !=
               std::numeric_limits<std::uint64_t>::max() &&
           decoded.route.instrument_open_parents_after ==
               decoded.route.instrument_open_parents_before + 1U &&
           decoded.route.child_count <=
               std::numeric_limits<std::uint64_t>::max() -
                   decoded.route.instrument_open_children_before &&
           decoded.route.instrument_open_children_after ==
               decoded.route.instrument_open_children_before +
                   decoded.route.child_count;
}

[[nodiscard]] bool decode_frames_record(
    const recorder::DurableRecordView& record,
    DecodedRiskDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader{record.payload};
    std::uint16_t count{};
    if (!reader.u16(count) || count != decoded.route.child_count) {
        return false;
    }
    for (std::size_t index{}; index < count; ++index) {
        std::uint32_t venue{};
        std::uint64_t child_id{};
        std::uint64_t attempt{};
        std::uint64_t epoch{};
        std::uint64_t generation{};
        std::uint8_t dispatch_class{};
        std::uint8_t wire_size{};
        std::span<const std::byte> wire;
        std::uint16_t frame_size{};
        std::span<const std::byte> frame;
        if (!reader.u32(venue) || !reader.u64(child_id) ||
            !reader.u64(attempt) || !reader.u64(epoch) ||
            !reader.u64(generation) || !reader.u8(dispatch_class) ||
            !reader.u8(wire_size) ||
            wire_size > dataplane::kInlineWireIdentityBytes ||
            !reader.bytes(wire_size, wire) || !reader.u16(frame_size) ||
            frame_size == 0U ||
            frame_size > fastpath::kMaximumEncodedFrameBytes ||
            !reader.bytes(frame_size, frame)) {
            return false;
        }
        const DecodedChild& expected = decoded.children[index];
        if (venue != expected.reservation.venue.value() ||
            child_id != expected.reservation.child_order_id.value() ||
            attempt != expected.reservation.dispatch_attempt_id ||
            epoch != decoded.leadership_epoch ||
            generation != decoded.configuration_generation ||
            dispatch_class != static_cast<std::uint8_t>(
                                  fastpath::DispatchClass::NewOrReplace) ||
            wire_size != expected.wire_identity.size ||
            !std::equal(
                wire.begin(), wire.end(), expected.wire_identity.bytes.begin())) {
            return false;
        }
        DecodedChild& captured = decoded.children[index];
        captured.frame_size = frame_size;
        captured.dispatch_class = dispatch_class;
        std::copy(frame.begin(), frame.end(), captured.frame.begin());
    }
    if (dispatch_record_version != fastpath::kRiskDispatchRecordVersion) {
        if (decoded.route.kernel_state_generation ==
            std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        for (std::size_t index{}; index < count; ++index) {
            DecodedChild& child = decoded.children[index];
            child.reservation.venue_session_id = 1U;
            child.reservation.venue_session_epoch = 1U;
            child.reservation.ownership_generation =
                decoded.route.kernel_state_generation + 1U;
            child.reservation.last_dispatch_attempt_id =
                child.reservation.dispatch_attempt_id;
            child.reservation.wire_identity = child.wire_identity;
            decoded.route.children[index] = child.reservation;
        }
        return reader.done();
    }
    std::uint16_t ownership_count{};
    if (!reader.u16(ownership_count) ||
        ownership_count != decoded.route.child_count) {
        return false;
    }
    for (std::size_t index{}; index < ownership_count; ++index) {
        std::uint32_t venue{};
        std::uint64_t child_id{};
        std::uint64_t venue_session_id{};
        std::uint64_t venue_session_epoch{};
        std::uint64_t ownership_generation{};
        std::uint64_t last_dispatch_attempt_id{};
        std::uint8_t wire_size{};
        std::span<const std::byte> wire;
        if (!reader.u32(venue) || !reader.u64(child_id) ||
            !reader.u64(venue_session_id) ||
            !reader.u64(venue_session_epoch) ||
            !reader.u64(ownership_generation) ||
            !reader.u64(last_dispatch_attempt_id) ||
            !reader.u8(wire_size) || wire_size == 0U ||
            wire_size > dataplane::kInlineWireIdentityBytes ||
            !reader.bytes(wire_size, wire)) {
            return false;
        }
        DecodedChild& expected = decoded.children[index];
        if (venue != expected.reservation.venue.value() ||
            child_id != expected.reservation.child_order_id.value() ||
            venue_session_id == 0U || venue_session_epoch == 0U ||
            ownership_generation == 0U ||
            last_dispatch_attempt_id !=
                expected.reservation.dispatch_attempt_id ||
            wire_size != expected.wire_identity.size ||
            !std::equal(
                wire.begin(), wire.end(), expected.wire_identity.bytes.begin())) {
            return false;
        }
        expected.reservation.last_dispatch_attempt_id =
            last_dispatch_attempt_id;
        expected.reservation.venue_session_id = venue_session_id;
        expected.reservation.venue_session_epoch = venue_session_epoch;
        expected.reservation.ownership_generation = ownership_generation;
        expected.reservation.wire_identity = expected.wire_identity;
        decoded.route.children[index] = expected.reservation;
    }
    return reader.done();
}

[[nodiscard]] bool validate_external_effect_envelope(
    const recorder::DurableToken& token,
    const DecodedRiskDispatch& decoded) noexcept {
    if (token.authorization_envelope_version ==
        recorder::AuthorizationEnvelopeVersion::LegacyRangeV0) {
        return decoded.dispatch_record_version ==
                   kLegacyRiskDispatchRecordVersion ||
               decoded.dispatch_record_version ==
                   kAdaptiveRiskDispatchRecordVersion;
    }
    if (token.authorization_envelope_version !=
            recorder::AuthorizationEnvelopeVersion::ExactEffectsV1 ||
        decoded.dispatch_record_version !=
            fastpath::kRiskDispatchRecordVersion ||
        token.external_effect_authorizations.count !=
            decoded.route.child_count) {
        return false;
    }
    for (std::size_t index{}; index < decoded.route.child_count; ++index) {
        const DecodedChild& child = decoded.children[index];
        const auto permit = token.effect_permit(index);
        if (!permit ||
            !permit->authorizes_external_effect(
                child.reservation.venue.value(),
                child.reservation.child_order_id.value(),
                child.reservation.dispatch_attempt_id,
                child.reservation.venue_session_id,
                child.reservation.venue_session_epoch,
                child.reservation.ownership_generation,
                child.dispatch_class,
                {child.frame.data(), child.frame_size})) {
            return false;
        }
    }
    return true;
}

enum class DecodeStatus : std::uint8_t {
    Ok,
    InvalidRecordSet,
    InvalidPayload,
};

[[nodiscard]] DecodeStatus decode_dispatch_batch(
    const recorder::DurableBatch& batch,
    DecodedRiskDispatch& decoded) noexcept {
    if (batch.record_count() != 5U) {
        return DecodeStatus::InvalidRecordSet;
    }
    std::array<recorder::DurableRecordView, 5U> records{};
    std::uint16_t dispatch_record_version{};
    for (std::size_t index{}; index < records.size(); ++index) {
        const auto record = batch.record(index);
        if (!record) {
            return DecodeStatus::InvalidRecordSet;
        }
        if (index == 0U) {
            dispatch_record_version = record->version;
            if (dispatch_record_version !=
                    kLegacyRiskDispatchRecordVersion &&
                dispatch_record_version !=
                    kAdaptiveRiskDispatchRecordVersion &&
                dispatch_record_version !=
                    fastpath::kRiskDispatchRecordVersion) {
                return DecodeStatus::InvalidRecordSet;
            }
        } else if (record->version != dispatch_record_version) {
            return DecodeStatus::InvalidRecordSet;
        }
        records[index] = *record;
    }
    if (records[0U].type != static_cast<std::uint16_t>(
            fastpath::RiskDispatchRecordType::ClientAccountCommand) ||
        records[1U].type != static_cast<std::uint16_t>(
            fastpath::RiskDispatchRecordType::AccountClientRiskDelta) ||
        records[2U].type != static_cast<std::uint16_t>(
            fastpath::RiskDispatchRecordType::PinnedRouteInputs) ||
        records[3U].type != static_cast<std::uint16_t>(
            fastpath::RiskDispatchRecordType::RouteDecision) ||
        records[4U].type != static_cast<std::uint16_t>(
            fastpath::RiskDispatchRecordType::ExactChildFrames)) {
        return DecodeStatus::InvalidRecordSet;
    }
    decoded.dispatch_record_version = dispatch_record_version;
    if (!decode_command_record(
            records[0U], decoded, dispatch_record_version) ||
        !decode_risk_record(
            records[1U], decoded, dispatch_record_version) ||
        !decode_pinned_record(
            records[2U], decoded, dispatch_record_version) ||
        !decode_route_record(
            records[3U], decoded, dispatch_record_version) ||
        !decode_frames_record(
            records[4U], decoded, dispatch_record_version)) {
        return DecodeStatus::InvalidPayload;
    }
    return DecodeStatus::Ok;
}

}  // namespace

struct RiskDispatchRecoveryLedger::Impl final {
    struct ParentEntry final {
        bool occupied{};
        dataplane::RecoveredParentReservation route{};
    };

    struct ReplayEntry final {
        bool occupied{};
        std::uint64_t batch_id{};
        std::uint64_t producer_sequence{};
        recorder::HashChainDigest batch_hash{};
        recorder::HashChainDigest chain_hash{};
    };

    risk::RiskKernel* risk_kernel{};
    dataplane::RouteKernel* route_kernel{};
    RiskDispatchRecoveryConfiguration configuration{};
    std::unique_ptr<ParentEntry[]> parents;
    std::unique_ptr<ParentEntry[]> restore_parents;
    std::unique_ptr<ReplayEntry[]> replay;
    std::unique_ptr<ReplayEntry[]> restore_replay;
    std::unique_ptr<risk::RecoveredAccountRiskState[]> risk_accounts;
    std::unique_ptr<risk::RecoveredClientRiskState[]> risk_clients;
    std::unique_ptr<risk::RecoveredParentRiskState[]> risk_parents;
    std::unique_ptr<risk::RecoveredDedupeRiskState[]> risk_dedupe;
    std::array<dataplane::RecoveryVenueCreditState,
               dataplane::kMaximumDataplaneVenues>
        venue_credits{};
    std::array<dataplane::RecoveryVenueCreditState,
               dataplane::kMaximumDataplaneVenues>
        restore_venue_credits{};
    std::size_t active_parents{};
    std::size_t active_children{};
    std::size_t replay_count{};
    std::size_t replay_next{};
    std::size_t venue_credit_count{};
    std::uint64_t replayed_through_batch_id{};
    std::uint64_t producer_sequence_high_water{};
    recorder::HashChainDigest latest_chain_hash{};
    bool ready{};
    bool fail_stopped{};
    bool checkpoint_restored{};
    bool recovery_completed{};

    Impl(
        risk::RiskKernel& account_risk,
        dataplane::RouteKernel& routing,
        const RiskDispatchRecoveryConfiguration& supplied)
        : risk_kernel(&account_risk),
          route_kernel(&routing),
          configuration(supplied),
          parents(supplied.active_parent_capacity == 0U
                      ? nullptr
                      : std::make_unique<ParentEntry[]>(
                            supplied.active_parent_capacity)),
          restore_parents(supplied.active_parent_capacity == 0U
                      ? nullptr
                      : std::make_unique<ParentEntry[]>(
                            supplied.active_parent_capacity)),
          replay(supplied.replay_dedupe_capacity == 0U
                     ? nullptr
                     : std::make_unique<ReplayEntry[]>(
                           supplied.replay_dedupe_capacity)),
          restore_replay(supplied.replay_dedupe_capacity == 0U
                     ? nullptr
                     : std::make_unique<ReplayEntry[]>(
                           supplied.replay_dedupe_capacity)),
          risk_accounts(supplied.risk_account_capacity == 0U
                      ? nullptr
                      : std::make_unique<risk::RecoveredAccountRiskState[]>(
                            supplied.risk_account_capacity)),
          risk_clients(supplied.risk_client_capacity == 0U
                      ? nullptr
                      : std::make_unique<risk::RecoveredClientRiskState[]>(
                            supplied.risk_client_capacity)),
          risk_parents(supplied.active_parent_capacity == 0U
                      ? nullptr
                      : std::make_unique<risk::RecoveredParentRiskState[]>(
                            supplied.active_parent_capacity)),
          risk_dedupe(supplied.risk_dedupe_capacity == 0U
                      ? nullptr
                      : std::make_unique<risk::RecoveredDedupeRiskState[]>(
                            supplied.risk_dedupe_capacity)),
          replayed_through_batch_id(
              supplied.replay_anchor.through_batch_id),
          producer_sequence_high_water(
              supplied.replay_anchor.producer_sequence_high_water),
          latest_chain_hash(supplied.replay_anchor.chain_hash) {
        const bool valid_domain =
            supplied.required_domain ==
                recorder::DurabilityDomain::VolatileSoftwareReference ||
            supplied.required_domain ==
                recorder::DurabilityDomain::HardwarePersistent;
        const bool valid_checkpoint_trust =
            supplied.checkpoint_trust_policy ==
                RiskDispatchCheckpointTrustPolicy::
                    RequireExternalProvenance ||
            (supplied.checkpoint_trust_policy ==
                 RiskDispatchCheckpointTrustPolicy::ReferenceSelfChecked &&
             supplied.required_domain ==
                 recorder::DurabilityDomain::VolatileSoftwareReference &&
             supplied.checkpoint_provenance_verifier == nullptr);
        const bool empty_anchor =
            supplied.replay_anchor.through_batch_id == 0U &&
            supplied.replay_anchor.producer_sequence_high_water == 0U &&
            all_zero(supplied.replay_anchor.chain_hash);
        const bool nonempty_anchor =
            supplied.replay_anchor.through_batch_id != 0U &&
            supplied.replay_anchor.producer_sequence_high_water != 0U &&
            !all_zero(supplied.replay_anchor.chain_hash);
        const risk::RiskKernelMetadata risk_metadata = account_risk.metadata();
        if (supplied.active_parent_capacity == 0U ||
            supplied.active_parent_capacity >
                dataplane::kMaximumActiveParents ||
            supplied.replay_dedupe_capacity == 0U ||
            supplied.risk_account_capacity == 0U ||
            supplied.risk_client_capacity == 0U ||
            supplied.risk_dedupe_capacity == 0U ||
            supplied.leadership_epoch == 0U ||
            supplied.configuration_generation == 0U ||
            supplied.candidate_table_generation == 0U ||
            supplied.risk_generation == 0U || !valid_domain ||
            !valid_checkpoint_trust ||
            (!empty_anchor && !nonempty_anchor) ||
            routing.candidate_table_generation() !=
                supplied.candidate_table_generation ||
            routing.risk_generation() != supplied.risk_generation ||
            risk_metadata.risk_generation != supplied.risk_generation ||
            risk_metadata.configuration_generation !=
                supplied.configuration_generation) {
            return;
        }
        const bool risk_started = account_risk.begin_recovery();
        const bool route_started = routing.begin_recovery();
        ready = risk_started && route_started;
        fail_stopped = !ready && (risk_started || route_started);
    }

    void latch_failure() noexcept {
        ready = false;
        fail_stopped = true;
    }

    [[nodiscard]] std::size_t parent_index(
        const ParentOrderId parent) const noexcept {
        for (std::size_t index{}; index < configuration.active_parent_capacity;
             ++index) {
            if (parents[index].occupied &&
                parents[index].route.parent.id == parent) {
                return index;
            }
        }
        return configuration.active_parent_capacity;
    }

    [[nodiscard]] std::size_t empty_parent_index() const noexcept {
        for (std::size_t index{}; index < configuration.active_parent_capacity;
             ++index) {
            if (!parents[index].occupied) {
                return index;
            }
        }
        return configuration.active_parent_capacity;
    }

    [[nodiscard]] std::size_t child_index(
        const std::size_t parent_slot,
        const ChildOrderId child) const noexcept {
        if (parent_slot >= configuration.active_parent_capacity ||
            !parents[parent_slot].occupied) {
            return dataplane::kMaximumCandidatesPerInstrument;
        }
        const dataplane::RecoveredParentReservation& route =
            parents[parent_slot].route;
        for (std::size_t index{}; index < route.child_count; ++index) {
            if (route.children[index].child_order_id == child &&
                route.children[index].remaining_quantity) {
                return index;
            }
        }
        return dataplane::kMaximumCandidatesPerInstrument;
    }

    void retire_child_if_terminal(
        const std::size_t parent_slot,
        const std::size_t child_slot) noexcept {
        ParentEntry& parent = parents[parent_slot];
        if (parent.route.children[child_slot].remaining_quantity) {
            return;
        }
        parent.route.children[child_slot].reconciled = true;
        if (active_children != 0U) {
            --active_children;
        }
        bool any_working{};
        for (std::size_t index{}; index < parent.route.child_count; ++index) {
            if (parent.route.children[index].remaining_quantity) {
                any_working = true;
                break;
            }
        }
        if (!any_working) {
            parent = ParentEntry{};
            if (active_parents != 0U) {
                --active_parents;
            }
        }
    }

    [[nodiscard]] std::size_t replay_index(
        const std::uint64_t batch_id) const noexcept {
        for (std::size_t index{}; index < configuration.replay_dedupe_capacity;
             ++index) {
            if (replay[index].occupied && replay[index].batch_id == batch_id) {
                return index;
            }
        }
        return configuration.replay_dedupe_capacity;
    }

    void remember_replay(const recorder::DurableToken& token) noexcept {
        ReplayEntry& entry = replay[replay_next];
        if (!entry.occupied) {
            ++replay_count;
        }
        entry = ReplayEntry{true,
                            token.batch_id,
                            token.producer_sequence,
                            token.batch_hash,
                            token.chain_hash};
        replay_next = (replay_next + 1U) %
                      configuration.replay_dedupe_capacity;
    }

    void update_venue_credit(const VenueId venue) noexcept {
        const dataplane::RecoveryVenueCreditState state =
            route_kernel->recovery_venue_credit_state(venue);
        if (!state.found) {
            return;
        }
        for (std::size_t index{}; index < venue_credit_count; ++index) {
            if (venue_credits[index].venue == venue) {
                venue_credits[index] = state;
                return;
            }
        }
        if (venue_credit_count < venue_credits.size()) {
            venue_credits[venue_credit_count++] = state;
        }
    }
};

RiskDispatchRecoveryLedger::RiskDispatchRecoveryLedger(
    risk::RiskKernel& account_risk,
    dataplane::RouteKernel& route_kernel,
    const RiskDispatchRecoveryConfiguration configuration)
    : impl_(std::make_unique<Impl>(
          account_risk, route_kernel, configuration)) {}

RiskDispatchRecoveryLedger::~RiskDispatchRecoveryLedger() = default;
RiskDispatchRecoveryLedger::RiskDispatchRecoveryLedger(
    RiskDispatchRecoveryLedger&&) noexcept = default;
RiskDispatchRecoveryLedger& RiskDispatchRecoveryLedger::operator=(
    RiskDispatchRecoveryLedger&&) noexcept = default;

bool RiskDispatchRecoveryLedger::ready() const noexcept {
    return impl_ && impl_->ready && !impl_->fail_stopped &&
           !impl_->recovery_completed;
}

bool RiskDispatchRecoveryLedger::fail_stopped() const noexcept {
    return !impl_ || impl_->fail_stopped;
}

RiskDispatchReplayResult RiskDispatchRecoveryLedger::replay_dispatch_batch(
    const recorder::DurableToken& token,
    const recorder::DurableBatch& batch,
    const fastpath::RecoveryCommitPhase observed_crash_phase) noexcept {
    RiskDispatchReplayResult result;
    result.observed_crash_phase = observed_crash_phase;
    result.batch_id = token.batch_id;
    if (!impl_ || !ready()) {
        result.status = impl_ && impl_->fail_stopped
                            ? RiskDispatchReplayStatus::FailStopped
                            : RiskDispatchReplayStatus::NotReady;
        return result;
    }
    const auto phase_value =
        static_cast<std::uint8_t>(observed_crash_phase);
    if (phase_value < static_cast<std::uint8_t>(
                          fastpath::RecoveryCommitPhase::DurableOnly) ||
        phase_value > static_cast<std::uint8_t>(
                          fastpath::RecoveryCommitPhase::
                              RiskAndRemainderReleased)) {
        result.status = RiskDispatchReplayStatus::InvalidBatch;
        impl_->latch_failure();
        return result;
    }
    if (!token_matches_batch(token, batch)) {
        result.status = RiskDispatchReplayStatus::IntegrityFailure;
        impl_->latch_failure();
        return result;
    }
    const std::size_t replayed = impl_->replay_index(token.batch_id);
    if (replayed != impl_->configuration.replay_dedupe_capacity) {
        const Impl::ReplayEntry& prior = impl_->replay[replayed];
        if (prior.batch_hash == token.batch_hash &&
            prior.chain_hash == token.chain_hash &&
            prior.producer_sequence == token.producer_sequence) {
            result.status = RiskDispatchReplayStatus::AlreadyApplied;
            return result;
        }
        result.status = RiskDispatchReplayStatus::DuplicateConflict;
        impl_->latch_failure();
        return result;
    }
    if (token.batch_id <= impl_->replayed_through_batch_id) {
        result.status = RiskDispatchReplayStatus::BatchSequenceRegression;
        impl_->latch_failure();
        return result;
    }
    if (token.producer_sequence <= impl_->producer_sequence_high_water) {
        result.status = RiskDispatchReplayStatus::ProducerSequenceRegression;
        impl_->latch_failure();
        return result;
    }
    if (token.previous_hash != impl_->latest_chain_hash) {
        result.status = RiskDispatchReplayStatus::ChainDiscontinuity;
        impl_->latch_failure();
        return result;
    }
    if (token.leadership_epoch != impl_->configuration.leadership_epoch) {
        result.status = RiskDispatchReplayStatus::LeadershipEpochMismatch;
        impl_->latch_failure();
        return result;
    }
    if (token.configuration_generation !=
        impl_->configuration.configuration_generation) {
        result.status = RiskDispatchReplayStatus::ConfigurationMismatch;
        impl_->latch_failure();
        return result;
    }
    if (token.domain != impl_->configuration.required_domain ||
        batch.lane() != recorder::DurabilityLane::Normal) {
        result.status = RiskDispatchReplayStatus::DomainMismatch;
        impl_->latch_failure();
        return result;
    }

    DecodedRiskDispatch decoded{};
    const DecodeStatus decoded_status = decode_dispatch_batch(batch, decoded);
    if (decoded_status != DecodeStatus::Ok) {
        result.status = decoded_status == DecodeStatus::InvalidRecordSet
                            ? RiskDispatchReplayStatus::InvalidRecordSet
                            : RiskDispatchReplayStatus::CanonicalDecodeFailed;
        impl_->latch_failure();
        return result;
    }
    if (!validate_external_effect_envelope(token, decoded)) {
        result.status = RiskDispatchReplayStatus::TokenMismatch;
        impl_->latch_failure();
        return result;
    }
    result.parent = decoded.route.parent.id;
    result.child_count = decoded.route.child_count;
    if (decoded.producer_sequence != token.producer_sequence ||
        decoded.leadership_epoch != token.leadership_epoch ||
        decoded.configuration_generation !=
            token.configuration_generation ||
        decoded.required_domain != impl_->configuration.required_domain) {
        result.status = RiskDispatchReplayStatus::TokenMismatch;
        impl_->latch_failure();
        return result;
    }
    if (decoded.route.candidate_table_generation !=
            impl_->configuration.candidate_table_generation ||
        decoded.route.risk_generation != impl_->configuration.risk_generation ||
        decoded.risk_delta.risk_generation !=
            impl_->configuration.risk_generation ||
        decoded.risk_delta.configuration_generation !=
            impl_->configuration.configuration_generation) {
        result.status = RiskDispatchReplayStatus::GenerationMismatch;
        impl_->latch_failure();
        return result;
    }
    const recorder::DispatchAuthorization authorization =
        batch.dispatch_authorization();
    if (authorization.first_child_id != decoded.first_child_id ||
        authorization.first_attempt_id != decoded.first_attempt_id ||
        authorization.dispatch_count != decoded.route.child_count) {
        result.status = RiskDispatchReplayStatus::TokenMismatch;
        impl_->latch_failure();
        return result;
    }
    if (impl_->parent_index(decoded.route.parent.id) !=
            impl_->configuration.active_parent_capacity ||
        impl_->active_parents == impl_->configuration.active_parent_capacity) {
        result.status = RiskDispatchReplayStatus::CapacityExceeded;
        impl_->latch_failure();
        return result;
    }

    const risk::ScopeRiskCounters client_before =
        impl_->risk_kernel->client_counters(decoded.command.client_id);
    const risk::ScopeRiskCounters account_before =
        impl_->risk_kernel->account_counters(decoded.command.account_id);
    const dataplane::InstrumentRiskCounters instrument_before =
        impl_->route_kernel->instrument_counters(
            decoded.route.parent.instrument);
    if (!counters_equal(client_before, decoded.client_before) ||
        !counters_equal(account_before, decoded.account_before) ||
        !instrument_before.found ||
        instrument_before.gross_outstanding !=
            decoded.route.instrument_outstanding_before ||
        instrument_before.open_parents !=
            decoded.route.instrument_open_parents_before ||
        instrument_before.open_children !=
            decoded.route.instrument_open_children_before) {
        result.status = RiskDispatchReplayStatus::GenerationMismatch;
        impl_->latch_failure();
        return result;
    }
    for (std::size_t index{}; index < decoded.route.child_count; ++index) {
        const DecodedChild& child = decoded.children[index];
        for (std::size_t parent_index{};
             parent_index < impl_->configuration.active_parent_capacity;
             ++parent_index) {
            if (!impl_->parents[parent_index].occupied) {
                continue;
            }
            const dataplane::RecoveredParentReservation& existing_parent =
                impl_->parents[parent_index].route;
            for (std::size_t child_index{};
                 child_index < existing_parent.child_count;
                 ++child_index) {
                const dataplane::RecoveredChildReservation& existing =
                    existing_parent.children[child_index];
                if (existing.child_order_id ==
                        child.reservation.child_order_id ||
                    existing.dispatch_attempt_id ==
                        child.reservation.dispatch_attempt_id ||
                    existing.last_dispatch_attempt_id ==
                        child.reservation.dispatch_attempt_id) {
                    result.status =
                        RiskDispatchReplayStatus::GenerationMismatch;
                    impl_->latch_failure();
                    return result;
                }
            }
        }
        const dataplane::VenueRiskCounters venue =
            impl_->route_kernel->venue_counters(child.reservation.venue);
        if (!venue.found ||
            venue.outstanding != child.venue_outstanding_before ||
            venue.reserved_normal_tx_credits !=
                child.reserved_normal_before ||
            venue.reserved_cancel_tx_credits != child.reserved_cancel_before ||
            venue.reserved_throttle_credits !=
                child.reserved_throttle_before) {
            result.status = RiskDispatchReplayStatus::GenerationMismatch;
            impl_->latch_failure();
            return result;
        }
    }

    result.risk_import_status =
        impl_->risk_kernel->restore_recovered_delta(decoded.risk_delta);
    if (result.risk_import_status != risk::RiskRecoveryImportStatus::Imported) {
        result.status = RiskDispatchReplayStatus::RiskKernelRejected;
        impl_->latch_failure();
        return result;
    }
    if (!counters_equal(
            impl_->risk_kernel->client_counters(decoded.command.client_id),
            decoded.client_after_full_reservation) ||
        !counters_equal(
            impl_->risk_kernel->account_counters(decoded.command.account_id),
            decoded.account_after_full_reservation) ||
        impl_->risk_kernel->metadata().kernel_state_generation !=
            decoded.risk_delta.kernel_state_generation + 1U) {
        result.status = RiskDispatchReplayStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }
    if (decoded.immediate_remainder_release) {
        const risk::RiskAccountingStatus released =
            impl_->risk_kernel->apply_recovered_release(
                decoded.command.parent_order_id,
                decoded.immediate_remainder_release);
        if (released != risk::RiskAccountingStatus::Applied) {
            result.status = RiskDispatchReplayStatus::PartialStateFailStop;
            impl_->latch_failure();
            return result;
        }
    }
    if (!counters_equal(
            impl_->risk_kernel->client_counters(decoded.command.client_id),
            decoded.client_after_remainder_release) ||
        !counters_equal(
            impl_->risk_kernel->account_counters(decoded.command.account_id),
            decoded.account_after_remainder_release) ||
        impl_->risk_kernel->metadata().kernel_state_generation !=
            decoded.risk_delta.kernel_state_generation + 1U +
                (decoded.immediate_remainder_release ? 1U : 0U)) {
        result.status = RiskDispatchReplayStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }

    result.route_import_status =
        impl_->route_kernel->restore_recovered_parent(decoded.route);
    if (result.route_import_status !=
        dataplane::RecoveryImportStatus::Imported) {
        result.status = RiskDispatchReplayStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }
    const std::size_t parent_slot = impl_->empty_parent_index();
    if (parent_slot == impl_->configuration.active_parent_capacity) {
        result.status = RiskDispatchReplayStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }
    impl_->parents[parent_slot] = Impl::ParentEntry{true, decoded.route};
    ++impl_->active_parents;
    impl_->active_children += decoded.route.child_count;
    for (std::size_t index{}; index < decoded.route.child_count; ++index) {
        impl_->update_venue_credit(decoded.route.children[index].venue);
    }
    impl_->remember_replay(token);
    impl_->replayed_through_batch_id = token.batch_id;
    impl_->producer_sequence_high_water = token.producer_sequence;
    impl_->latest_chain_hash = token.chain_hash;
    result.status = RiskDispatchReplayStatus::Applied;
    return result;
}

RiskDispatchAccountingResult
RiskDispatchRecoveryLedger::apply_recovered_fill(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t dispatch_attempt_id,
    const Quantity filled_quantity) noexcept {
    RiskDispatchAccountingResult result;
    if (!impl_ || !ready()) {
        return result;
    }
    if (!filled_quantity) {
        result.status = RiskDispatchAccountingStatus::InvalidQuantity;
        return result;
    }
    const std::size_t parent_slot = impl_->parent_index(parent);
    if (parent_slot == impl_->configuration.active_parent_capacity) {
        result.status = RiskDispatchAccountingStatus::ParentNotFound;
        return result;
    }
    const std::size_t child_slot = impl_->child_index(parent_slot, child);
    if (child_slot == dataplane::kMaximumCandidatesPerInstrument) {
        result.status = RiskDispatchAccountingStatus::ChildNotFound;
        return result;
    }
    dataplane::RecoveredChildReservation& recovered =
        impl_->parents[parent_slot].route.children[child_slot];
    if (recovered.dispatch_attempt_id != dispatch_attempt_id) {
        result.status = RiskDispatchAccountingStatus::AttemptMismatch;
        return result;
    }
    const risk::ActiveParentRisk risk_parent =
        impl_->risk_kernel->active_parent(parent);
    if (filled_quantity > recovered.remaining_quantity ||
        !risk_parent.found || filled_quantity > risk_parent.remaining_quantity) {
        result.status = RiskDispatchAccountingStatus::InvalidQuantity;
        return result;
    }
    result.route_status = impl_->route_kernel->apply_recovered_fill(
        parent, child, dispatch_attempt_id, filled_quantity);
    if (result.route_status != dataplane::RecoveryAccountingStatus::Applied) {
        result.status = RiskDispatchAccountingStatus::RouteKernelRejected;
        return result;
    }
    result.risk_status =
        impl_->risk_kernel->apply_recovered_fill(parent, filled_quantity);
    if (result.risk_status != risk::RiskAccountingStatus::Applied) {
        result.status = RiskDispatchAccountingStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }
    recovered.remaining_quantity.units -= filled_quantity.units;
    const auto remaining_notional = checked_notional(
        impl_->parents[parent_slot].route.parent.limit_price,
        recovered.remaining_quantity);
    if (!remaining_notional) {
        result.status = RiskDispatchAccountingStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }
    impl_->update_venue_credit(recovered.venue);
    impl_->retire_child_if_terminal(parent_slot, child_slot);
    result.status = RiskDispatchAccountingStatus::Applied;
    return result;
}

RiskDispatchAccountingResult
RiskDispatchRecoveryLedger::note_recovered_cancel(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t prior_dispatch_attempt_id,
    const std::uint64_t cancel_dispatch_attempt_id,
    const std::uint64_t client_request_id,
    const std::uint64_t client_sequence,
    const std::uint64_t client_session_epoch) noexcept {
    RiskDispatchAccountingResult result;
    if (!impl_ || !ready()) {
        return result;
    }
    const std::size_t parent_slot = impl_->parent_index(parent);
    if (parent_slot == impl_->configuration.active_parent_capacity) {
        result.status = RiskDispatchAccountingStatus::ParentNotFound;
        return result;
    }
    const std::size_t child_slot = impl_->child_index(parent_slot, child);
    if (child_slot == dataplane::kMaximumCandidatesPerInstrument) {
        result.status = RiskDispatchAccountingStatus::ChildNotFound;
        return result;
    }
    dataplane::RecoveredChildReservation& recovered =
        impl_->parents[parent_slot].route.children[child_slot];
    if (recovered.cancel_pending) {
        const bool same =
            recovered.pending_cancel_attempt_id ==
                cancel_dispatch_attempt_id &&
            recovered.pending_client_request_id == client_request_id &&
            recovered.pending_client_sequence == client_sequence &&
            recovered.pending_client_session_epoch == client_session_epoch;
        result.status = same
            ? RiskDispatchAccountingStatus::AlreadyApplied
            : RiskDispatchAccountingStatus::RouteKernelRejected;
        result.route_status = same
            ? dataplane::RecoveryAccountingStatus::AlreadyApplied
            : dataplane::RecoveryAccountingStatus::InvariantViolation;
        return result;
    }
    if (recovered.last_dispatch_attempt_id != prior_dispatch_attempt_id) {
        result.status = RiskDispatchAccountingStatus::AttemptMismatch;
        return result;
    }
    if (cancel_dispatch_attempt_id <
        impl_->route_kernel->recovery_identifiers()
            .next_dispatch_attempt_id) {
        result.status = RiskDispatchAccountingStatus::RouteKernelRejected;
        result.route_status =
            dataplane::RecoveryAccountingStatus::InvariantViolation;
        return result;
    }
    result.route_status = impl_->route_kernel->note_recovered_cancel(
        parent,
        child,
        prior_dispatch_attempt_id,
        cancel_dispatch_attempt_id,
        client_request_id,
        client_sequence,
        client_session_epoch);
    if (result.route_status != dataplane::RecoveryAccountingStatus::Applied) {
        result.status = RiskDispatchAccountingStatus::RouteKernelRejected;
        return result;
    }
    recovered.cancel_pending = true;
    recovered.last_dispatch_attempt_id = cancel_dispatch_attempt_id;
    ++recovered.ownership_generation;
    recovered.pending_client_request_id = client_request_id;
    recovered.pending_client_sequence = client_sequence;
    recovered.pending_client_session_epoch = client_session_epoch;
    recovered.pending_cancel_attempt_id = cancel_dispatch_attempt_id;
    result.status = RiskDispatchAccountingStatus::Applied;
    return result;
}

RiskDispatchAccountingResult
RiskDispatchRecoveryLedger::reconcile_recovered_child(
    const ParentOrderId parent,
    const ChildOrderId child,
    const std::uint64_t dispatch_attempt_id,
    const dataplane::RecoveredChildDisposition disposition) noexcept {
    RiskDispatchAccountingResult result;
    if (!impl_ || !ready()) {
        return result;
    }
    const std::size_t parent_slot = impl_->parent_index(parent);
    if (parent_slot == impl_->configuration.active_parent_capacity) {
        result.status = RiskDispatchAccountingStatus::ParentNotFound;
        return result;
    }
    const std::size_t child_slot = impl_->child_index(parent_slot, child);
    if (child_slot == dataplane::kMaximumCandidatesPerInstrument) {
        result.status = RiskDispatchAccountingStatus::ChildNotFound;
        return result;
    }
    dataplane::RecoveredChildReservation& recovered =
        impl_->parents[parent_slot].route.children[child_slot];
    if (recovered.dispatch_attempt_id != dispatch_attempt_id) {
        result.status = RiskDispatchAccountingStatus::AttemptMismatch;
        return result;
    }
    if (disposition == dataplane::RecoveredChildDisposition::Working &&
        recovered.reconciled) {
        result.status = RiskDispatchAccountingStatus::AlreadyApplied;
        result.route_status =
            dataplane::RecoveryAccountingStatus::AlreadyApplied;
        return result;
    }
    const Quantity terminal_quantity = recovered.remaining_quantity;
    if (disposition == dataplane::RecoveredChildDisposition::Terminal) {
        const risk::ActiveParentRisk risk_parent =
            impl_->risk_kernel->active_parent(parent);
        if (!risk_parent.found ||
            terminal_quantity > risk_parent.remaining_quantity) {
            result.status = RiskDispatchAccountingStatus::RiskKernelRejected;
            return result;
        }
    }
    result.route_status = impl_->route_kernel->reconcile_recovered_child(
        parent, child, dispatch_attempt_id, disposition);
    if (result.route_status != dataplane::RecoveryAccountingStatus::Applied &&
        result.route_status !=
            dataplane::RecoveryAccountingStatus::AlreadyApplied) {
        result.status = RiskDispatchAccountingStatus::RouteKernelRejected;
        return result;
    }
    if (disposition == dataplane::RecoveredChildDisposition::Terminal) {
        result.risk_status = impl_->risk_kernel->apply_recovered_release(
            parent, terminal_quantity);
        if (result.risk_status != risk::RiskAccountingStatus::Applied) {
            result.status = RiskDispatchAccountingStatus::PartialStateFailStop;
            impl_->latch_failure();
            return result;
        }
        recovered.remaining_quantity = Quantity{};
        impl_->update_venue_credit(recovered.venue);
        impl_->retire_child_if_terminal(parent_slot, child_slot);
    } else {
        recovered.reconciled = true;
    }
    result.status = result.route_status ==
                            dataplane::RecoveryAccountingStatus::AlreadyApplied
                        ? RiskDispatchAccountingStatus::AlreadyApplied
                        : RiskDispatchAccountingStatus::Applied;
    return result;
}

std::size_t RiskDispatchRecoveryLedger::checkpoint_size_required()
    const noexcept {
    if (!impl_ || !ready()) {
        return 0U;
    }
    const risk::RiskRecoveryExportResult risk_export =
        impl_->risk_kernel->export_recovery_snapshot(
            {impl_->risk_accounts.get(),
             impl_->configuration.risk_account_capacity},
            {impl_->risk_clients.get(),
             impl_->configuration.risk_client_capacity},
            {impl_->risk_parents.get(),
             impl_->configuration.active_parent_capacity},
            {impl_->risk_dedupe.get(),
             impl_->configuration.risk_dedupe_capacity});
    if (!risk_export.exported()) {
        return 0U;
    }
    constexpr std::size_t header_bytes = 174U;
    constexpr std::size_t parent_bytes = 32U;
    constexpr std::size_t child_bytes_without_wire = 129U;
    constexpr std::size_t venue_bytes = 44U;
    constexpr std::size_t account_bytes = 28U;
    constexpr std::size_t client_bytes = 60U;
    constexpr std::size_t risk_parent_bytes = 52U;
    constexpr std::size_t dedupe_bytes = 41U;
    constexpr std::size_t replay_bytes = 80U;
    std::size_t required = header_bytes + kCheckpointFooterBytes;
    const auto add = [&required](
                         const std::size_t count,
                         const std::size_t width) noexcept {
        if (count != 0U &&
            width > (std::numeric_limits<std::size_t>::max() - required) /
                        count) {
            return false;
        }
        required += count * width;
        return true;
    };
    if (!add(impl_->active_parents, parent_bytes) ||
        !add(impl_->active_children, child_bytes_without_wire) ||
        !add(impl_->venue_credit_count, venue_bytes) ||
        !add(risk_export.account_count, account_bytes) ||
        !add(risk_export.client_count, client_bytes) ||
        !add(risk_export.active_parent_count, risk_parent_bytes) ||
        !add(risk_export.dedupe_count, dedupe_bytes) ||
        !add(impl_->replay_count, replay_bytes)) {
        return 0U;
    }
    std::size_t measured_children{};
    for (std::size_t parent_index{};
         parent_index < impl_->configuration.active_parent_capacity;
         ++parent_index) {
        if (!impl_->parents[parent_index].occupied) {
            continue;
        }
        const dataplane::RecoveredParentReservation& parent =
            impl_->parents[parent_index].route;
        for (std::size_t child_index{}; child_index < parent.child_count;
             ++child_index) {
            const dataplane::RecoveredChildReservation& child =
                parent.children[child_index];
            if (!child.remaining_quantity) {
                continue;
            }
            if (child.wire_identity.size == 0U ||
                child.wire_identity.size > child.wire_identity.bytes.size() ||
                !add(1U, child.wire_identity.size)) {
                return 0U;
            }
            ++measured_children;
        }
    }
    if (measured_children != impl_->active_children) {
        return 0U;
    }
    return required;
}

RiskDispatchCheckpointEncodeResult
RiskDispatchRecoveryLedger::encode_checkpoint(
    const std::span<std::byte> destination) const noexcept {
    RiskDispatchCheckpointEncodeResult result;
    if (!impl_ || !ready()) {
        return result;
    }
    result.required_size = checkpoint_size_required();
    if (result.required_size == 0U) {
        result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
        return result;
    }
    if (destination.size() < result.required_size) {
        result.status = RiskDispatchCheckpointEncodeStatus::BufferTooSmall;
        return result;
    }
    const risk::RiskRecoveryExportResult risk_export =
        impl_->risk_kernel->export_recovery_snapshot(
            {impl_->risk_accounts.get(),
             impl_->configuration.risk_account_capacity},
            {impl_->risk_clients.get(),
             impl_->configuration.risk_client_capacity},
            {impl_->risk_parents.get(),
             impl_->configuration.active_parent_capacity},
            {impl_->risk_dedupe.get(),
             impl_->configuration.risk_dedupe_capacity});
    if (!risk_export.exported()) {
        result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
        return result;
    }
    const dataplane::RecoveryIdentifierState identifiers =
        impl_->route_kernel->recovery_identifiers();
    const risk::RiskRecoveryKernelState risk_state =
        impl_->risk_kernel->recovery_kernel_state();
    if (identifiers.next_child_order_id == 0U ||
        identifiers.next_dispatch_attempt_id == 0U ||
        risk_state.risk_generation != impl_->configuration.risk_generation ||
        risk_state.configuration_generation !=
            impl_->configuration.configuration_generation ||
        impl_->active_parents > std::numeric_limits<std::uint32_t>::max() ||
        impl_->venue_credit_count >
            std::numeric_limits<std::uint32_t>::max() ||
        risk_export.account_count >
            std::numeric_limits<std::uint32_t>::max() ||
        risk_export.client_count >
            std::numeric_limits<std::uint32_t>::max() ||
        risk_export.active_parent_count >
            std::numeric_limits<std::uint32_t>::max() ||
        risk_export.dedupe_count >
            std::numeric_limits<std::uint32_t>::max() ||
        impl_->replay_count > std::numeric_limits<std::uint32_t>::max()) {
        result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
        return result;
    }

    Writer writer{destination.first(result.required_size)};
    if (!writer.u32(kCheckpointMagic) ||
        !writer.u16(kRiskDispatchRecoveryCheckpointVersion) ||
        !writer.u16(0U) ||
        !writer.u64(impl_->configuration.leadership_epoch) ||
        !writer.u64(impl_->configuration.configuration_generation) ||
        !writer.u64(impl_->configuration.candidate_table_generation) ||
        !writer.u64(impl_->configuration.risk_generation) ||
        !writer.u8(static_cast<std::uint8_t>(
            impl_->configuration.required_domain)) ||
        !writer.u64(impl_->route_kernel->state_generation()) ||
        !writer.u64(identifiers.next_child_order_id) ||
        !writer.u64(identifiers.next_dispatch_attempt_id) ||
        !writer.u64(risk_state.risk_generation) ||
        !writer.u64(risk_state.configuration_generation) ||
        !writer.u64(risk_state.kernel_state_generation) ||
        !writer.u64(risk_state.last_committed_monotonic_time_ns) ||
        !writer.boolean(risk_state.kill_switch) ||
        !writer.u64(impl_->replayed_through_batch_id) ||
        !writer.u64(impl_->producer_sequence_high_water) ||
        !writer.bytes(impl_->latest_chain_hash) ||
        !writer.u32(static_cast<std::uint32_t>(impl_->active_parents)) ||
        !writer.u32(static_cast<std::uint32_t>(impl_->venue_credit_count)) ||
        !writer.u32(static_cast<std::uint32_t>(risk_export.account_count)) ||
        !writer.u32(static_cast<std::uint32_t>(risk_export.client_count)) ||
        !writer.u32(
            static_cast<std::uint32_t>(risk_export.active_parent_count)) ||
        !writer.u32(static_cast<std::uint32_t>(risk_export.dedupe_count)) ||
        !writer.u32(static_cast<std::uint32_t>(impl_->replay_count))) {
        result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
        return result;
    }

    std::uint64_t previous_parent{};
    for (std::size_t emitted{}; emitted < impl_->active_parents; ++emitted) {
        const Impl::ParentEntry* selected = nullptr;
        for (std::size_t index{};
             index < impl_->configuration.active_parent_capacity; ++index) {
            const Impl::ParentEntry& candidate = impl_->parents[index];
            if (!candidate.occupied ||
                candidate.route.parent.id.value() <= previous_parent) {
                continue;
            }
            if (selected == nullptr ||
                candidate.route.parent.id < selected->route.parent.id) {
                selected = &candidate;
            }
        }
        if (selected == nullptr) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
        const dataplane::RecoveredParentReservation& route = selected->route;
        std::uint16_t active_child_count{};
        for (std::size_t index{}; index < route.child_count; ++index) {
            if (route.children[index].remaining_quantity) {
                ++active_child_count;
            }
        }
        if (active_child_count == 0U ||
            !writer.u64(route.parent.id.value()) ||
            !writer.u32(route.parent.instrument.value()) ||
            !writer.u8(static_cast<std::uint8_t>(route.parent.side)) ||
            !writer.u8(static_cast<std::uint8_t>(route.parent.intent)) ||
            !writer.u16(active_child_count) ||
            !writer.u64(route.parent.quantity.units) ||
            !writer.i64(route.parent.limit_price.units)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
        for (std::size_t index{}; index < route.child_count; ++index) {
            const dataplane::RecoveredChildReservation& child =
                route.children[index];
            if (!child.remaining_quantity) {
                continue;
            }
            const bool pending_shape = child.cancel_pending
                ? child.pending_client_request_id != 0U &&
                      child.pending_client_sequence != 0U &&
                      child.pending_client_session_epoch != 0U &&
                      child.pending_cancel_attempt_id != 0U &&
                      child.last_dispatch_attempt_id ==
                          child.pending_cancel_attempt_id &&
                      child.pending_cancel_attempt_id >
                          child.dispatch_attempt_id
                : child.pending_client_request_id == 0U &&
                      child.pending_client_sequence == 0U &&
                      child.pending_client_session_epoch == 0U &&
                      child.pending_cancel_attempt_id == 0U &&
                      child.last_dispatch_attempt_id ==
                          child.dispatch_attempt_id;
            if (!child.child_order_id || child.dispatch_attempt_id == 0U ||
                child.last_dispatch_attempt_id == 0U || !child.venue ||
                child.venue_session_id == 0U ||
                child.venue_session_epoch == 0U ||
                child.ownership_generation == 0U ||
                child.wire_identity.size == 0U ||
                child.wire_identity.size > child.wire_identity.bytes.size() ||
                !pending_shape) {
                result.status =
                    RiskDispatchCheckpointEncodeStatus::StateInvalid;
                return result;
            }
            if (!writer.u64(child.child_order_id.value()) ||
                !writer.u64(child.dispatch_attempt_id) ||
                !writer.u64(child.last_dispatch_attempt_id) ||
                !writer.u32(child.venue.value()) ||
                !writer.u64(child.venue_session_id) ||
                !writer.u64(child.venue_session_epoch) ||
                !writer.u64(child.ownership_generation) ||
                !writer.u8(child.wire_identity.size) ||
                !writer.bytes({
                    child.wire_identity.bytes.data(),
                    child.wire_identity.size}) ||
                !writer.u64(child.remaining_quantity.units) ||
                !writer.u64(child.tx_credit_generation) ||
                !writer.u64(child.throttle_generation) ||
                !writer.u32(child.reported_normal_tx_credits) ||
                !writer.u32(child.reported_cancel_tx_credits) ||
                !writer.u32(child.reported_throttle_credits) ||
                !writer.u16(child.normal_tx_credit_delta) ||
                !writer.u16(child.cancel_tx_credit_delta) ||
                !writer.u16(child.throttle_credit_delta) ||
                !writer.boolean(child.reconciled) ||
                !writer.boolean(child.cancel_pending) ||
                !writer.u64(child.pending_client_request_id) ||
                !writer.u64(child.pending_client_sequence) ||
                !writer.u64(child.pending_client_session_epoch) ||
                !writer.u64(child.pending_cancel_attempt_id)) {
                result.status =
                    RiskDispatchCheckpointEncodeStatus::StateInvalid;
                return result;
            }
        }
        previous_parent = route.parent.id.value();
    }

    std::uint32_t previous_venue{};
    for (std::size_t emitted{}; emitted < impl_->venue_credit_count; ++emitted) {
        const dataplane::RecoveryVenueCreditState* selected = nullptr;
        for (std::size_t index{}; index < impl_->venue_credit_count; ++index) {
            const dataplane::RecoveryVenueCreditState& candidate =
                impl_->venue_credits[index];
            if (!candidate.found || candidate.venue.value() <= previous_venue) {
                continue;
            }
            if (selected == nullptr || candidate.venue < selected->venue) {
                selected = &candidate;
            }
        }
        if (selected == nullptr ||
            !writer.u32(selected->venue.value()) ||
            !writer.u64(selected->tx_credit_generation) ||
            !writer.u64(selected->throttle_generation) ||
            !writer.u32(selected->reported_normal_tx_credits) ||
            !writer.u32(selected->reported_cancel_tx_credits) ||
            !writer.u32(selected->reported_throttle_credits) ||
            !writer.u32(selected->reserved_normal_tx_credits) ||
            !writer.u32(selected->reserved_cancel_tx_credits) ||
            !writer.u32(selected->reserved_throttle_credits)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
        previous_venue = selected->venue.value();
    }

    for (std::size_t index{}; index < risk_export.account_count; ++index) {
        const risk::RecoveredAccountRiskState& state =
            impl_->risk_accounts[index];
        if (!writer.u64(state.account_id) ||
            !writer.u64(state.open_quantity.units) ||
            !writer.u64(state.open_notional.units) ||
            !writer.u32(state.open_orders)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
    }
    for (std::size_t index{}; index < risk_export.client_count; ++index) {
        const risk::RecoveredClientRiskState& state = impl_->risk_clients[index];
        if (!writer.u64(state.client_id) || !writer.u64(state.account_id) ||
            !writer.u64(state.open_quantity.units) ||
            !writer.u64(state.open_notional.units) ||
            !writer.u32(state.open_orders) ||
            !writer.u32(state.normal_tokens) ||
            !writer.u32(state.cancel_tokens) ||
            !writer.u64(state.normal_refill_time_ns) ||
            !writer.u64(state.cancel_refill_time_ns)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
    }
    for (std::size_t index{}; index < risk_export.active_parent_count;
         ++index) {
        const risk::RecoveredParentRiskState& state = impl_->risk_parents[index];
        if (!writer.u64(state.parent_order_id.value()) ||
            !writer.u64(state.client_id) || !writer.u64(state.account_id) ||
            !writer.u32(state.instrument.value()) ||
            !writer.u64(state.remaining_quantity.units) ||
            !writer.u64(state.remaining_notional.units) ||
            !writer.i64(state.limit_price.units)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
    }
    for (std::size_t index{}; index < risk_export.dedupe_count; ++index) {
        const risk::RecoveredDedupeRiskState& state = impl_->risk_dedupe[index];
        if (!writer.u64(state.client_id) || !writer.u64(state.request_id) ||
            !writer.u64(state.command_fingerprint) ||
            !writer.u8(static_cast<std::uint8_t>(state.verdict)) ||
            !writer.u64(state.reservation_quantity.units) ||
            !writer.u64(state.reservation_notional.units)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
    }

    std::uint64_t previous_batch{};
    for (std::size_t emitted{}; emitted < impl_->replay_count; ++emitted) {
        const Impl::ReplayEntry* selected = nullptr;
        for (std::size_t index{};
             index < impl_->configuration.replay_dedupe_capacity; ++index) {
            const Impl::ReplayEntry& candidate = impl_->replay[index];
            if (!candidate.occupied || candidate.batch_id <= previous_batch) {
                continue;
            }
            if (selected == nullptr || candidate.batch_id < selected->batch_id) {
                selected = &candidate;
            }
        }
        if (selected == nullptr || !writer.u64(selected->batch_id) ||
            !writer.u64(selected->producer_sequence) ||
            !writer.bytes(selected->batch_hash) ||
            !writer.bytes(selected->chain_hash)) {
            result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
            return result;
        }
        previous_batch = selected->batch_id;
    }

    const std::size_t hash_offset = writer.position();
    Sha256 sha;
    sha.update(destination.first(hash_offset));
    result.checkpoint_hash = sha.finish();
    if (!writer.bytes(result.checkpoint_hash)) {
        result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
        return result;
    }
    result.crc32c = recorder::crc32c(destination.first(writer.position()));
    if (!writer.u32(result.crc32c) ||
        writer.position() != result.required_size) {
        result.status = RiskDispatchCheckpointEncodeStatus::StateInvalid;
        return result;
    }
    result.encoded_size = writer.position();
    result.status = RiskDispatchCheckpointEncodeStatus::Encoded;
    return result;
}

RiskDispatchCheckpointRestoreResult
RiskDispatchRecoveryLedger::restore_checkpoint(
    const std::span<const std::byte> encoded) noexcept {
    RiskDispatchCheckpointRestoreResult result;
    if (!impl_ || !ready()) {
        return result;
    }
    if (impl_->active_parents != 0U || impl_->replay_count != 0U ||
        impl_->venue_credit_count != 0U || impl_->checkpoint_restored) {
        result.status = RiskDispatchCheckpointRestoreStatus::LedgerNotEmpty;
        return result;
    }
    if (encoded.size() < 174U + kCheckpointFooterBytes) {
        result.status = RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
        return result;
    }
    const std::size_t hash_offset = encoded.size() - kCheckpointFooterBytes;
    const std::size_t crc_offset = encoded.size() - 4U;
    const std::uint32_t stored_crc = get_u32(encoded.data() + crc_offset);
    if (recorder::crc32c(encoded.first(crc_offset)) != stored_crc) {
        result.status = RiskDispatchCheckpointRestoreStatus::IntegrityFailure;
        impl_->latch_failure();
        return result;
    }
    Sha256 sha;
    sha.update(encoded.first(hash_offset));
    const recorder::HashChainDigest expected_hash = sha.finish();
    if (!std::equal(
            expected_hash.begin(),
            expected_hash.end(),
            encoded.begin() + hash_offset)) {
        result.status = RiskDispatchCheckpointRestoreStatus::IntegrityFailure;
        impl_->latch_failure();
        return result;
    }
    if (impl_->configuration.checkpoint_trust_policy ==
        RiskDispatchCheckpointTrustPolicy::RequireExternalProvenance) {
        const IRiskDispatchCheckpointProvenanceVerifier* const verifier =
            impl_->configuration.checkpoint_provenance_verifier;
        if (verifier == nullptr) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::ProvenanceRequired;
            impl_->latch_failure();
            return result;
        }
        const RiskDispatchCheckpointProvenanceContext context{
            expected_hash,
            impl_->configuration.leadership_epoch,
            impl_->configuration.configuration_generation,
            impl_->configuration.required_domain};
        if (!verifier->verify_checkpoint(encoded, context)) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::ProvenanceRejected;
            impl_->latch_failure();
            return result;
        }
    }

    Reader reader{encoded.first(hash_offset)};
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint16_t flags{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t risk_generation{};
    std::uint8_t domain{};
    dataplane::RecoveryKernelState route_state{};
    risk::RiskRecoveryKernelState risk_state{};
    std::uint64_t replayed_through{};
    std::uint64_t producer_high_water{};
    std::span<const std::byte> chain;
    recorder::HashChainDigest checkpoint_chain{};
    std::uint32_t parent_count{};
    std::uint32_t venue_count{};
    std::uint32_t account_count{};
    std::uint32_t client_count{};
    std::uint32_t risk_parent_count{};
    std::uint32_t risk_dedupe_count{};
    std::uint32_t replay_count{};
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(flags) ||
        !reader.u64(leadership_epoch) ||
        !reader.u64(configuration_generation) ||
        !reader.u64(candidate_generation) || !reader.u64(risk_generation) ||
        !reader.u8(domain) || !reader.u64(route_state.state_generation) ||
        !reader.u64(route_state.identifiers.next_child_order_id) ||
        !reader.u64(route_state.identifiers.next_dispatch_attempt_id) ||
        !reader.u64(risk_state.risk_generation) ||
        !reader.u64(risk_state.configuration_generation) ||
        !reader.u64(risk_state.kernel_state_generation) ||
        !reader.u64(risk_state.last_committed_monotonic_time_ns) ||
        !reader.boolean(risk_state.kill_switch) ||
        !reader.u64(replayed_through) || !reader.u64(producer_high_water) ||
        !reader.bytes(recorder::HashChainDigest{}.size(), chain) ||
        !reader.u32(parent_count) || !reader.u32(venue_count) ||
        !reader.u32(account_count) || !reader.u32(client_count) ||
        !reader.u32(risk_parent_count) || !reader.u32(risk_dedupe_count) ||
        !reader.u32(replay_count) || magic != kCheckpointMagic ||
        (version != kRiskDispatchRecoveryMinimumCheckpointVersion &&
         version != kRiskDispatchRecoveryCheckpointVersion) ||
        flags != 0U) {
        result.status = RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
        impl_->latch_failure();
        return result;
    }
    std::copy(chain.begin(), chain.end(), checkpoint_chain.begin());
    if (leadership_epoch != impl_->configuration.leadership_epoch ||
        configuration_generation !=
            impl_->configuration.configuration_generation ||
        domain != static_cast<std::uint8_t>(
                      impl_->configuration.required_domain)) {
        result.status =
            RiskDispatchCheckpointRestoreStatus::ConfigurationMismatch;
        impl_->latch_failure();
        return result;
    }
    if (candidate_generation !=
            impl_->configuration.candidate_table_generation ||
        risk_generation != impl_->configuration.risk_generation ||
        risk_state.risk_generation != risk_generation ||
        risk_state.configuration_generation != configuration_generation) {
        result.status = RiskDispatchCheckpointRestoreStatus::GenerationMismatch;
        impl_->latch_failure();
        return result;
    }
    if (parent_count > impl_->configuration.active_parent_capacity ||
        venue_count > impl_->restore_venue_credits.size() ||
        account_count > impl_->configuration.risk_account_capacity ||
        client_count > impl_->configuration.risk_client_capacity ||
        risk_parent_count > impl_->configuration.active_parent_capacity ||
        risk_dedupe_count > impl_->configuration.risk_dedupe_capacity ||
        replay_count > impl_->configuration.replay_dedupe_capacity ||
        route_state.state_generation == 0U ||
        route_state.identifiers.next_child_order_id == 0U ||
        route_state.identifiers.next_dispatch_attempt_id == 0U ||
        risk_state.kernel_state_generation == 0U ||
        (replayed_through == 0U &&
         (producer_high_water != 0U || !all_zero(chain) ||
          replay_count != 0U)) ||
        (replayed_through != 0U &&
         (producer_high_water == 0U || all_zero(chain)))) {
        result.status = RiskDispatchCheckpointRestoreStatus::CapacityExceeded;
        impl_->latch_failure();
        return result;
    }
    for (std::size_t index{};
         index < impl_->configuration.active_parent_capacity; ++index) {
        impl_->restore_parents[index] = Impl::ParentEntry{};
    }
    for (std::size_t index{};
         index < impl_->configuration.replay_dedupe_capacity; ++index) {
        impl_->restore_replay[index] = Impl::ReplayEntry{};
    }
    std::fill(
        impl_->restore_venue_credits.begin(),
        impl_->restore_venue_credits.end(),
        dataplane::RecoveryVenueCreditState{});

    std::uint64_t previous_parent{};
    std::size_t parsed_children{};
    for (std::size_t index{}; index < parent_count; ++index) {
        Impl::ParentEntry& entry = impl_->restore_parents[index];
        dataplane::RecoveredParentReservation& route = entry.route;
        std::uint64_t parent{};
        std::uint32_t instrument{};
        std::uint8_t side{};
        std::uint8_t intent{};
        if (!reader.u64(parent) || !reader.u32(instrument) ||
            !reader.u8(side) || !reader.u8(intent) ||
            !reader.u16(route.child_count) ||
            !reader.u64(route.parent.quantity.units) ||
            !reader.i64(route.parent.limit_price.units) || parent == 0U ||
            parent <= previous_parent || instrument == 0U ||
            !valid_side_value(side) || !valid_intent_value(intent) ||
            route.child_count == 0U ||
            route.child_count > route.children.size() ||
            !route.parent.quantity || route.parent.limit_price.units <= 0) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        entry.occupied = true;
        route.parent.id = ParentOrderId{parent};
        route.parent.instrument = InstrumentId{instrument};
        route.parent.side = static_cast<Side>(side);
        route.parent.intent = static_cast<RoutingIntent>(intent);
        route.candidate_table_generation = candidate_generation;
        route.risk_generation = risk_generation;
        for (std::size_t child_index{}; child_index < route.child_count;
             ++child_index) {
            dataplane::RecoveredChildReservation& child =
                route.children[child_index];
            std::uint64_t child_id{};
            std::uint32_t venue{};
            if (version == kRiskDispatchRecoveryMinimumCheckpointVersion) {
                if (!reader.u64(child_id) ||
                    !reader.u64(child.dispatch_attempt_id) ||
                    !reader.u32(venue) ||
                    !reader.u64(child.remaining_quantity.units) ||
                    !reader.u64(child.tx_credit_generation) ||
                    !reader.u64(child.throttle_generation) ||
                    !reader.u32(child.reported_normal_tx_credits) ||
                    !reader.u32(child.reported_cancel_tx_credits) ||
                    !reader.u32(child.reported_throttle_credits) ||
                    !reader.u16(child.normal_tx_credit_delta) ||
                    !reader.u16(child.cancel_tx_credit_delta) ||
                    !reader.u16(child.throttle_credit_delta) ||
                    !reader.boolean(child.reconciled) ||
                    !reader.boolean(child.cancel_pending) ||
                    child.cancel_pending) {
                    result.status =
                        RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
                    impl_->latch_failure();
                    return result;
                }
                child.child_order_id = ChildOrderId{child_id};
                child.venue = VenueId{venue};
                child.last_dispatch_attempt_id = child.dispatch_attempt_id;
                child.venue_session_id = 1U;
                child.venue_session_epoch = 1U;
                child.ownership_generation = route_state.state_generation;
                child.wire_identity = make_legacy_wire_identity(
                    route.parent.id,
                    child.child_order_id,
                    child.dispatch_attempt_id);
            } else {
                std::uint8_t wire_size{};
                std::span<const std::byte> wire;
                if (!reader.u64(child_id) ||
                    !reader.u64(child.dispatch_attempt_id) ||
                    !reader.u64(child.last_dispatch_attempt_id) ||
                    !reader.u32(venue) ||
                    !reader.u64(child.venue_session_id) ||
                    !reader.u64(child.venue_session_epoch) ||
                    !reader.u64(child.ownership_generation) ||
                    !reader.u8(wire_size) || wire_size == 0U ||
                    wire_size > child.wire_identity.bytes.size() ||
                    !reader.bytes(wire_size, wire) ||
                    !reader.u64(child.remaining_quantity.units) ||
                    !reader.u64(child.tx_credit_generation) ||
                    !reader.u64(child.throttle_generation) ||
                    !reader.u32(child.reported_normal_tx_credits) ||
                    !reader.u32(child.reported_cancel_tx_credits) ||
                    !reader.u32(child.reported_throttle_credits) ||
                    !reader.u16(child.normal_tx_credit_delta) ||
                    !reader.u16(child.cancel_tx_credit_delta) ||
                    !reader.u16(child.throttle_credit_delta) ||
                    !reader.boolean(child.reconciled) ||
                    !reader.boolean(child.cancel_pending) ||
                    !reader.u64(child.pending_client_request_id) ||
                    !reader.u64(child.pending_client_sequence) ||
                    !reader.u64(child.pending_client_session_epoch) ||
                    !reader.u64(child.pending_cancel_attempt_id)) {
                    result.status =
                        RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
                    impl_->latch_failure();
                    return result;
                }
                child.child_order_id = ChildOrderId{child_id};
                child.venue = VenueId{venue};
                child.wire_identity.size = wire_size;
                std::copy(
                    wire.begin(), wire.end(), child.wire_identity.bytes.begin());
            }
            if (!child.child_order_id || !child.venue) {
                result.status =
                    RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
                impl_->latch_failure();
                return result;
            }
            const bool pending_shape = child.cancel_pending
                ? child.pending_client_request_id != 0U &&
                      child.pending_client_sequence != 0U &&
                      child.pending_client_session_epoch != 0U &&
                      child.pending_cancel_attempt_id != 0U &&
                      child.last_dispatch_attempt_id ==
                          child.pending_cancel_attempt_id &&
                      child.pending_cancel_attempt_id >
                          child.dispatch_attempt_id
                : child.pending_client_request_id == 0U &&
                      child.pending_client_sequence == 0U &&
                      child.pending_client_session_epoch == 0U &&
                      child.pending_cancel_attempt_id == 0U &&
                      child.last_dispatch_attempt_id ==
                          child.dispatch_attempt_id;
            if (!child.child_order_id || child.dispatch_attempt_id == 0U ||
                child.last_dispatch_attempt_id == 0U || !child.venue ||
                child.venue_session_id == 0U ||
                child.venue_session_epoch == 0U ||
                child.ownership_generation == 0U ||
                !child.remaining_quantity || !pending_shape ||
                child_id >= route_state.identifiers.next_child_order_id ||
                child.last_dispatch_attempt_id >=
                    route_state.identifiers.next_dispatch_attempt_id) {
                result.status =
                    RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
                impl_->latch_failure();
                return result;
            }
            for (std::size_t prior{}; prior < child_index; ++prior) {
                const dataplane::RecoveredChildReservation& existing =
                    route.children[prior];
                if (existing.child_order_id == child.child_order_id ||
                    existing.dispatch_attempt_id ==
                        child.dispatch_attempt_id ||
                    existing.last_dispatch_attempt_id ==
                        child.last_dispatch_attempt_id ||
                    existing.dispatch_attempt_id ==
                        child.last_dispatch_attempt_id ||
                    existing.last_dispatch_attempt_id ==
                        child.dispatch_attempt_id ||
                    route.children[prior].venue == child.venue) {
                    result.status =
                        RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
                    impl_->latch_failure();
                    return result;
                }
            }
            for (std::size_t prior_parent{}; prior_parent < index;
                 ++prior_parent) {
                const dataplane::RecoveredParentReservation& existing_parent =
                    impl_->restore_parents[prior_parent].route;
                for (std::size_t prior_child{};
                     prior_child < existing_parent.child_count;
                     ++prior_child) {
                    const dataplane::RecoveredChildReservation& existing =
                        existing_parent.children[prior_child];
                    if (existing.child_order_id == child.child_order_id ||
                        existing.dispatch_attempt_id ==
                            child.dispatch_attempt_id ||
                        existing.last_dispatch_attempt_id ==
                            child.last_dispatch_attempt_id ||
                        existing.dispatch_attempt_id ==
                            child.last_dispatch_attempt_id ||
                        existing.last_dispatch_attempt_id ==
                            child.dispatch_attempt_id) {
                        result.status =
                            RiskDispatchCheckpointRestoreStatus::
                                InvalidEncoding;
                        impl_->latch_failure();
                        return result;
                    }
                }
            }
            ++parsed_children;
        }
        previous_parent = parent;
    }

    std::uint32_t previous_venue{};
    for (std::size_t index{}; index < venue_count; ++index) {
        dataplane::RecoveryVenueCreditState& credit =
            impl_->restore_venue_credits[index];
        std::uint32_t venue{};
        if (!reader.u32(venue) ||
            !reader.u64(credit.tx_credit_generation) ||
            !reader.u64(credit.throttle_generation) ||
            !reader.u32(credit.reported_normal_tx_credits) ||
            !reader.u32(credit.reported_cancel_tx_credits) ||
            !reader.u32(credit.reported_throttle_credits) ||
            !reader.u32(credit.reserved_normal_tx_credits) ||
            !reader.u32(credit.reserved_cancel_tx_credits) ||
            !reader.u32(credit.reserved_throttle_credits) || venue == 0U ||
            venue <= previous_venue || credit.tx_credit_generation == 0U ||
            credit.throttle_generation == 0U ||
            credit.reserved_normal_tx_credits >
                credit.reported_normal_tx_credits ||
            credit.reserved_cancel_tx_credits >
                credit.reported_cancel_tx_credits ||
            credit.reserved_throttle_credits >
                credit.reported_throttle_credits) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        credit.venue = VenueId{venue};
        credit.found = true;
        previous_venue = venue;
    }

    std::uint64_t previous_account{};
    for (std::size_t index{}; index < account_count; ++index) {
        risk::RecoveredAccountRiskState& state = impl_->risk_accounts[index];
        if (!reader.u64(state.account_id) ||
            !reader.u64(state.open_quantity.units) ||
            !reader.u64(state.open_notional.units) ||
            !reader.u32(state.open_orders) || state.account_id == 0U ||
            state.account_id <= previous_account) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        previous_account = state.account_id;
    }
    std::uint64_t previous_client{};
    for (std::size_t index{}; index < client_count; ++index) {
        risk::RecoveredClientRiskState& state = impl_->risk_clients[index];
        if (!reader.u64(state.client_id) || !reader.u64(state.account_id) ||
            !reader.u64(state.open_quantity.units) ||
            !reader.u64(state.open_notional.units) ||
            !reader.u32(state.open_orders) ||
            !reader.u32(state.normal_tokens) ||
            !reader.u32(state.cancel_tokens) ||
            !reader.u64(state.normal_refill_time_ns) ||
            !reader.u64(state.cancel_refill_time_ns) ||
            state.client_id == 0U || state.client_id <= previous_client) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        previous_client = state.client_id;
    }
    std::uint64_t previous_risk_parent{};
    for (std::size_t index{}; index < risk_parent_count; ++index) {
        risk::RecoveredParentRiskState& state = impl_->risk_parents[index];
        std::uint64_t parent{};
        std::uint32_t instrument{};
        if (!reader.u64(parent) || !reader.u64(state.client_id) ||
            !reader.u64(state.account_id) || !reader.u32(instrument) ||
            !reader.u64(state.remaining_quantity.units) ||
            !reader.u64(state.remaining_notional.units) ||
            !reader.i64(state.limit_price.units) || parent == 0U ||
            parent <= previous_risk_parent || instrument == 0U) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        state.parent_order_id = ParentOrderId{parent};
        state.instrument = InstrumentId{instrument};
        previous_risk_parent = parent;
    }
    std::uint64_t prior_dedupe_client{};
    std::uint64_t prior_dedupe_request{};
    for (std::size_t index{}; index < risk_dedupe_count; ++index) {
        risk::RecoveredDedupeRiskState& state = impl_->risk_dedupe[index];
        std::uint8_t verdict{};
        if (!reader.u64(state.client_id) || !reader.u64(state.request_id) ||
            !reader.u64(state.command_fingerprint) || !reader.u8(verdict) ||
            !reader.u64(state.reservation_quantity.units) ||
            !reader.u64(state.reservation_notional.units) ||
            state.client_id == 0U || state.request_id == 0U ||
            (index != 0U &&
             (state.client_id < prior_dedupe_client ||
              (state.client_id == prior_dedupe_client &&
               state.request_id <= prior_dedupe_request)))) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        state.verdict = static_cast<risk::RiskVerdict>(verdict);
        prior_dedupe_client = state.client_id;
        prior_dedupe_request = state.request_id;
    }
    std::uint64_t previous_batch{};
    std::uint64_t previous_replay_producer{};
    for (std::size_t index{}; index < replay_count; ++index) {
        Impl::ReplayEntry& entry = impl_->restore_replay[index];
        std::span<const std::byte> batch_hash;
        std::span<const std::byte> entry_chain;
        if (!reader.u64(entry.batch_id) ||
            !reader.u64(entry.producer_sequence) ||
            !reader.bytes(entry.batch_hash.size(), batch_hash) ||
            !reader.bytes(entry.chain_hash.size(), entry_chain) ||
            entry.batch_id == 0U || entry.batch_id <= previous_batch ||
            entry.producer_sequence <= previous_replay_producer) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
        entry.occupied = true;
        std::copy(
            batch_hash.begin(), batch_hash.end(), entry.batch_hash.begin());
        std::copy(
            entry_chain.begin(), entry_chain.end(), entry.chain_hash.begin());
        previous_batch = entry.batch_id;
        previous_replay_producer = entry.producer_sequence;
    }
    if (!reader.done() ||
        (replay_count != 0U &&
         (impl_->restore_replay[replay_count - 1U].batch_id !=
              replayed_through ||
          impl_->restore_replay[replay_count - 1U].producer_sequence !=
              producer_high_water ||
          impl_->restore_replay[replay_count - 1U].chain_hash !=
              checkpoint_chain))) {
        result.status = RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
        impl_->latch_failure();
        return result;
    }

    // Every live route parent must have one matching risk reservation. Since
    // the unrouted remainder is released at dispatch, their remaining
    // quantities are exactly the sums of the still-working children.
    if (parent_count != risk_parent_count) {
        result.status = RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
        impl_->latch_failure();
        return result;
    }
    for (std::size_t index{}; index < parent_count; ++index) {
        const dataplane::RecoveredParentReservation& route =
            impl_->restore_parents[index].route;
        const risk::RecoveredParentRiskState& risk_parent =
            impl_->risk_parents[index];
        Quantity summed{};
        for (std::size_t child{}; child < route.child_count; ++child) {
            const auto next = checked_add(
                summed, route.children[child].remaining_quantity);
            if (!next) {
                result.status =
                    RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
                impl_->latch_failure();
                return result;
            }
            summed = *next;
        }
        if (route.parent.id != risk_parent.parent_order_id ||
            route.parent.instrument != risk_parent.instrument ||
            route.parent.limit_price != risk_parent.limit_price ||
            summed != risk_parent.remaining_quantity) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::InvalidEncoding;
            impl_->latch_failure();
            return result;
        }
    }

    // Parsing and cross-state validation are complete. RiskKernel snapshot
    // import is atomic. Any subsequent unexpected RouteKernel rejection is a
    // partial-state fail-stop and requires a clean process restart.
    impl_->ready = false;
    const risk::RiskRecoveryImportStatus risk_import =
        impl_->risk_kernel->import_recovery_snapshot(
            risk_state,
            {impl_->risk_accounts.get(), account_count},
            {impl_->risk_clients.get(), client_count},
            {impl_->risk_parents.get(), risk_parent_count},
            {impl_->risk_dedupe.get(), risk_dedupe_count});
    if (risk_import != risk::RiskRecoveryImportStatus::Imported) {
        result.status = RiskDispatchCheckpointRestoreStatus::RiskKernelRejected;
        impl_->latch_failure();
        return result;
    }
    for (std::size_t index{}; index < parent_count; ++index) {
        dataplane::RecoveredParentReservation& route =
            impl_->restore_parents[index].route;
        route.kernel_state_generation = 0U;
        const dataplane::InstrumentRiskCounters instrument =
            impl_->route_kernel->instrument_counters(route.parent.instrument);
        if (!instrument.found) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::PartialStateFailStop;
            impl_->latch_failure();
            return result;
        }
        route.instrument_outstanding_before = instrument.gross_outstanding;
        route.instrument_open_parents_before = instrument.open_parents;
        route.instrument_open_children_before = instrument.open_children;
        Quantity added{};
        for (std::size_t child_index{}; child_index < route.child_count;
             ++child_index) {
            const dataplane::RecoveredChildReservation& child =
                route.children[child_index];
            const dataplane::VenueRiskCounters venue =
                impl_->route_kernel->venue_counters(child.venue);
            const auto venue_after = checked_add(
                venue.outstanding, child.remaining_quantity);
            const auto next_added = checked_add(added, child.remaining_quantity);
            if (!venue.found || !venue_after || !next_added) {
                result.status =
                    RiskDispatchCheckpointRestoreStatus::PartialStateFailStop;
                impl_->latch_failure();
                return result;
            }
            route.venue_outstanding_before[child_index] = venue.outstanding;
            route.venue_outstanding_after[child_index] = *venue_after;
            added = *next_added;
        }
        const auto instrument_after = checked_add(
            route.instrument_outstanding_before, added);
        if (!instrument_after ||
            route.instrument_open_parents_before ==
                std::numeric_limits<std::uint64_t>::max() ||
            route.child_count >
                std::numeric_limits<std::uint64_t>::max() -
                    route.instrument_open_children_before) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::PartialStateFailStop;
            impl_->latch_failure();
            return result;
        }
        route.instrument_outstanding_after = *instrument_after;
        route.instrument_open_parents_after =
            route.instrument_open_parents_before + 1U;
        route.instrument_open_children_after =
            route.instrument_open_children_before + route.child_count;
        const dataplane::RecoveryImportStatus imported =
            impl_->route_kernel->restore_recovered_parent(route);
        if (imported != dataplane::RecoveryImportStatus::Imported) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::PartialStateFailStop;
            impl_->latch_failure();
            return result;
        }
    }
    for (std::size_t index{}; index < venue_count; ++index) {
        if (!impl_->route_kernel->restore_recovery_venue_credit_state(
                impl_->restore_venue_credits[index])) {
            result.status =
                RiskDispatchCheckpointRestoreStatus::PartialStateFailStop;
            impl_->latch_failure();
            return result;
        }
    }
    if (route_state.state_generation <
            impl_->route_kernel->state_generation() ||
        !impl_->route_kernel->restore_recovery_kernel_state(route_state)) {
        result.status =
            RiskDispatchCheckpointRestoreStatus::PartialStateFailStop;
        impl_->latch_failure();
        return result;
    }

    for (std::size_t index{};
         index < impl_->configuration.active_parent_capacity; ++index) {
        impl_->parents[index] = index < parent_count
            ? impl_->restore_parents[index]
            : Impl::ParentEntry{};
    }
    for (std::size_t index{};
         index < impl_->configuration.replay_dedupe_capacity; ++index) {
        impl_->replay[index] = index < replay_count
            ? impl_->restore_replay[index]
            : Impl::ReplayEntry{};
    }
    for (std::size_t index{}; index < impl_->venue_credits.size(); ++index) {
        impl_->venue_credits[index] = index < venue_count
            ? impl_->restore_venue_credits[index]
            : dataplane::RecoveryVenueCreditState{};
    }
    impl_->active_parents = parent_count;
    impl_->active_children = parsed_children;
    impl_->venue_credit_count = venue_count;
    impl_->replay_count = replay_count;
    impl_->replay_next = replay_count %
                         impl_->configuration.replay_dedupe_capacity;
    impl_->replayed_through_batch_id = replayed_through;
    impl_->producer_sequence_high_water = producer_high_water;
    impl_->latest_chain_hash = checkpoint_chain;
    impl_->checkpoint_restored = true;
    impl_->ready = true;
    result.replay_through_batch_id = replayed_through;
    result.restored_parents = parent_count;
    result.restored_risk_clients = client_count;
    result.restored_risk_dedupe_entries = risk_dedupe_count;
    result.status = RiskDispatchCheckpointRestoreStatus::Restored;
    return result;
}

bool RiskDispatchRecoveryLedger::complete_reconciliation() noexcept {
    if (!impl_ || !ready() ||
        impl_->route_kernel->unreconciled_child_count() != 0U ||
        !impl_->route_kernel->recovery_only() ||
        !impl_->risk_kernel->recovery_only()) {
        return false;
    }
    if (!impl_->route_kernel->complete_recovery()) {
        impl_->latch_failure();
        return false;
    }
    if (!impl_->risk_kernel->complete_recovery()) {
        impl_->latch_failure();
        return false;
    }
    impl_->ready = false;
    impl_->recovery_completed = true;
    return true;
}

RiskDispatchRecoveryMetadata RiskDispatchRecoveryLedger::metadata()
    const noexcept {
    RiskDispatchRecoveryMetadata result;
    if (!impl_) {
        result.fail_stopped = true;
        return result;
    }
    result.ready = ready();
    result.fail_stopped = impl_->fail_stopped;
    result.checkpoint_restored = impl_->checkpoint_restored;
    result.recovery_only = impl_->route_kernel->recovery_only() ||
                           impl_->risk_kernel->recovery_only();
    result.replayed_through_batch_id = impl_->replayed_through_batch_id;
    result.producer_sequence_high_water =
        impl_->producer_sequence_high_water;
    result.latest_chain_hash = impl_->latest_chain_hash;
    result.active_parents = impl_->active_parents;
    result.active_children = impl_->active_children;
    result.unreconciled_children =
        impl_->route_kernel->unreconciled_child_count();
    result.replay_dedupe_entries = impl_->replay_count;
    result.identifiers = impl_->route_kernel->recovery_identifiers();
    result.risk = impl_->risk_kernel->metadata();
    return result;
}

}  // namespace sorx::recovery
