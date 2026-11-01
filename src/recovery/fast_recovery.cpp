#include "sorx/recovery/fast_recovery.hpp"

#include "sorx/fastpath/contracts.hpp"
#include "sorx/fastpath/dispatch_pipeline.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sorx::recovery {
namespace {

inline constexpr std::uint32_t kDispatchMagic = 0x534f5258U;
inline constexpr std::uint32_t kCheckpointMagic = 0x53525843U;
inline constexpr std::size_t kCheckpointFooterBytes = 36U;
inline constexpr std::uint16_t kLegacyDispatchRecordVersion = 1U;
inline constexpr std::uint16_t kAdaptiveDispatchRecordVersion = 2U;

// Schema coupling is intentional: a future dispatch-record revision must add
// an explicit decoder branch instead of being interpreted as v1, v2, or v3.
static_assert(fastpath::kDispatchRecordVersion == 3U);

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
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
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
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8U> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<std::byte, 64U> block_{};
    std::size_t total_bytes_{};
    std::size_t block_size_{};
};

[[nodiscard]] bool token_matches_batch(
    const recorder::DurableToken& token,
    const recorder::DurableBatch& batch) noexcept {
    return recorder::verify_durable_token(token, batch);
}

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
        std::uint64_t bits{};
        if (!u64(bits)) {
            return false;
        }
        output = static_cast<std::int64_t>(bits);
        return true;
    }

    [[nodiscard]] bool bytes(
        const std::size_t count,
        std::span<const std::byte>& output) noexcept {
        if (count > remaining()) {
            return false;
        }
        output = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool done() const noexcept {
        return offset_ == bytes_.size();
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return offset_;
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
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

    [[nodiscard]] std::size_t position() const noexcept {
        return offset_;
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    std::span<std::byte> bytes_;
    std::size_t offset_{};
};

struct PinState final {
    VenueId venue{};
    std::uint64_t adjacency_generation{};
    bool has_book_version{};
    md::BookVersion book_version{};
    std::uint64_t tx_credit_generation{};
    std::uint64_t throttle_generation{};
    std::uint32_t normal_tx_credits{};
    std::uint32_t cancel_tx_credits{};
    std::uint32_t throttle_credits{};
    std::uint64_t receive_time_ns{};
    std::uint64_t maximum_quote_age_ns{};
    Price first_recorded_price{};
    Quantity first_recorded_quantity{};
    bool view_present{};
    bool has_book{};
    bool source_healthy{};
    bool conversion_compatible{};
    bool risk_allowed{};
    MarketDataState market_state{MarketDataState::Failed};
    OrderEntryState order_entry_state{OrderEntryState::Disconnected};
    EgressState egress_state{EgressState::Congested};
    InstrumentState instrument_state{InstrumentState::Unknown};
    bool first_slice_seen{};
};

struct DecodedChild final {
    dataplane::RecoveredChildReservation reservation{};
    Quantity venue_outstanding_before{};
    Quantity venue_outstanding_after{};
    Price allocation_limit{};
    Quantity venue_quantity{};
    Price venue_limit{};
    std::uint32_t allocation_order{};
    std::uint64_t adjacency_generation{};
    md::BookVersion book_version{};
    dataplane::InlineWireIdentity wire_identity{};
    std::array<std::byte, fastpath::kMaximumEncodedFrameBytes> frame{};
    std::uint16_t frame_size{};
    std::uint8_t dispatch_class{};
};

struct DecodedDispatch final {
    dataplane::RecoveredParentReservation route{};
    std::array<PinState, dataplane::kMaximumCandidatesPerInstrument> pins{};
    std::array<dataplane::AdaptivePolicyOverlay,
               dataplane::kMaximumCandidatesPerInstrument>
        adaptive_overlays{};
    std::array<DecodedChild, dataplane::kMaximumCandidatesPerInstrument>
        children{};
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

[[nodiscard]] bool valid_side_value(const std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(Side::Buy) ||
           value == static_cast<std::uint8_t>(Side::Sell);
}

[[nodiscard]] bool valid_intent_value(const std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(RoutingIntent::AggressiveIOC) ||
           value == static_cast<std::uint8_t>(RoutingIntent::PassiveGTC);
}

[[nodiscard]] bool valid_policy_exclusion_value(
    const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
        policy::PolicyExclusionReason::PassiveMarketableProtected);
}

[[nodiscard]] bool read_policy_versions(
    Reader& reader,
    policy::PolicyVersionPin& output) noexcept {
    return reader.u64(output.policy_generation) &&
           reader.u64(output.fee_generation) &&
           reader.u64(output.metric_generation) &&
           reader.u64(output.weight_generation) &&
           reader.u64(output.compliance_generation) &&
           reader.u64(output.compliance_rule_pack_id) &&
           reader.u64(output.compliance_rule_pack_version) &&
           reader.u64(output.compliance_effective_from_utc_ns) &&
           reader.u64(output.compliance_effective_until_utc_ns) &&
           output.valid();
}

[[nodiscard]] bool read_policy_order(
    Reader& reader,
    policy::PolicyOrder& output) noexcept {
    std::uint64_t parent{};
    std::uint32_t instrument{};
    std::uint8_t side{};
    std::uint8_t intent{};
    if (!reader.u64(parent) || !reader.u32(instrument) || !reader.u8(side) ||
        !reader.u8(intent) || !reader.u64(output.quantity.units) ||
        !reader.i64(output.limit_price.units) ||
        !reader.u64(output.decision_monotonic_ns) ||
        !reader.u64(output.decision_utc_ns) || !valid_side_value(side) ||
        !valid_intent_value(intent)) {
        return false;
    }
    output.parent_order_id = ParentOrderId{parent};
    output.instrument = InstrumentId{instrument};
    output.side = static_cast<Side>(side);
    output.intent = static_cast<RoutingIntent>(intent);
    return output.valid();
}

[[nodiscard]] bool read_venue_fact_pin(
    Reader& reader,
    policy::VenueFactPin& output) noexcept {
    std::uint32_t venue{};
    if (!reader.u32(venue) ||
        !reader.u64(output.versions.route_generation) ||
        !reader.u64(output.versions.market_generation) ||
        !reader.u64(output.versions.order_entry_generation) ||
        !reader.u64(output.versions.risk_generation) ||
        !reader.u64(output.versions.throttle_generation) ||
        !reader.u64(output.versions.tx_credit_generation) ||
        !reader.u64(output.versions.protected_quote_generation) ||
        !reader.u64(output.versions.fee_generation) ||
        !reader.u64(output.versions.metric_generation)) {
        return false;
    }
    output.venue = VenueId{venue};
    return static_cast<bool>(output.venue);
}

void add_score_component(
    std::int64_t& total,
    const std::int64_t component,
    bool& saturated) noexcept {
    if (component > 0 &&
        total > std::numeric_limits<std::int64_t>::max() - component) {
        total = std::numeric_limits<std::int64_t>::max();
        saturated = true;
        return;
    }
    if (component < 0 &&
        total < std::numeric_limits<std::int64_t>::min() - component) {
        total = std::numeric_limits<std::int64_t>::min();
        saturated = true;
        return;
    }
    total += component;
}

[[nodiscard]] bool valid_score_shape(
    const policy::ScoreBreakdown& score,
    const RoutingIntent intent) noexcept {
    if (score.depth_reward < 0 || score.fill_quality_reward < 0 ||
        score.adverse_selection_penalty > 0 || score.latency_penalty > 0 ||
        score.reject_penalty > 0 ||
        (intent == RoutingIntent::AggressiveIOC &&
         (score.fill_quality_reward != 0 ||
          score.adverse_selection_penalty != 0)) ||
        (intent == RoutingIntent::PassiveGTC && score.depth_reward != 0)) {
        return false;
    }
    const std::array components{
        score.depth_reward,
        score.fee_economics,
        score.fill_quality_reward,
        score.adverse_selection_penalty,
        score.latency_penalty,
        score.reject_penalty,
    };
    std::int64_t total{};
    bool sum_saturated{};
    bool extreme_component{};
    for (const std::int64_t component : components) {
        extreme_component = extreme_component ||
            component == std::numeric_limits<std::int64_t>::min() ||
            component == std::numeric_limits<std::int64_t>::max();
        add_score_component(total, component, sum_saturated);
    }
    if (total != score.total) {
        return false;
    }
    return score.saturated
        ? sum_saturated || extreme_component
        : !sum_saturated;
}

[[nodiscard]] bool read_ranked_evidence(
    Reader& reader,
    const policy::VenueFactPin& fact_pin,
    const RoutingIntent intent,
    policy::RankedCandidate& output) noexcept {
    std::uint32_t venue{};
    std::uint32_t fee_bits{};
    if (!reader.u32(venue) || !reader.i64(output.executable_price.units) ||
        !reader.u64(output.available_depth.units) ||
        !reader.u64(output.quote_age_ns) ||
        !reader.u64(output.maximum_quote_age_ns) ||
        !reader.u32(output.throttle_credits) ||
        !reader.u32(output.normal_tx_credits) ||
        !reader.boolean(output.protected_quote.view_healthy) ||
        !reader.boolean(output.protected_quote.best_opposite_present) ||
        !reader.i64(output.protected_quote.best_opposite_price.units) ||
        !reader.u32(fee_bits) ||
        !reader.u32(output.metrics.fill_quality_ppm) ||
        !reader.u32(output.metrics.adverse_selection_ppm) ||
        !reader.u32(output.metrics.reject_rate_ppm) ||
        !reader.u64(output.metrics.measured_latency_ns) ||
        !reader.i64(output.score.depth_reward) ||
        !reader.i64(output.score.fee_economics) ||
        !reader.i64(output.score.fill_quality_reward) ||
        !reader.i64(output.score.adverse_selection_penalty) ||
        !reader.i64(output.score.latency_penalty) ||
        !reader.i64(output.score.reject_penalty) ||
        !reader.i64(output.score.total) ||
        !reader.boolean(output.score.saturated) ||
        !reader.u32(output.configured_priority)) {
        return false;
    }
    const std::int64_t fee_value = fee_bits <= 0x7fffffffU
        ? static_cast<std::int64_t>(fee_bits)
        : static_cast<std::int64_t>(fee_bits) - (1LL << 32U);
    output.venue = VenueId{venue};
    output.pin = fact_pin;
    output.fee_ppm_used = static_cast<std::int32_t>(fee_value);
    return output.venue == fact_pin.venue &&
           output.executable_price.units > 0 &&
           (intent != RoutingIntent::AggressiveIOC ||
            static_cast<bool>(output.available_depth)) &&
           output.maximum_quote_age_ns != 0U &&
           output.quote_age_ns <= output.maximum_quote_age_ns &&
           output.throttle_credits != 0U &&
           output.normal_tx_credits != 0U &&
           (!output.protected_quote.best_opposite_present ||
            output.protected_quote.best_opposite_price.units > 0) &&
           fee_value >= -static_cast<std::int64_t>(
               policy::kMetricPartsPerMillion) &&
           fee_value <= static_cast<std::int64_t>(
               policy::kMetricPartsPerMillion) &&
           output.metrics.fill_quality_ppm <=
               policy::kMetricPartsPerMillion &&
           output.metrics.adverse_selection_ppm <=
               policy::kMetricPartsPerMillion &&
           output.metrics.reject_rate_ppm <=
               policy::kMetricPartsPerMillion &&
           valid_score_shape(output.score, intent);
}

[[nodiscard]] bool read_adaptive_overlay(
    Reader& reader,
    dataplane::AdaptivePolicyOverlay& output) noexcept {
    if (!reader.boolean(output.applied)) {
        return false;
    }
    if (!output.applied) {
        return output.valid_shape();
    }
    std::uint8_t exclusion{};
    if (!reader.boolean(output.eligible) || !reader.u16(output.rank) ||
        !reader.u8(exclusion) || !valid_policy_exclusion_value(exclusion) ||
        !read_policy_versions(reader, output.versions) ||
        !read_policy_order(reader, output.order) ||
        !read_venue_fact_pin(reader, output.fact_pin)) {
        return false;
    }
    output.exclusion_reason =
        static_cast<policy::PolicyExclusionReason>(exclusion);
    if (output.eligible && !read_ranked_evidence(
            reader, output.fact_pin, output.order.intent, output.evidence)) {
        return false;
    }
    return output.valid_shape();
}

[[nodiscard]] bool adaptive_candidate_precedes(
    const policy::RankedCandidate& lhs,
    const policy::RankedCandidate& rhs,
    const Side side,
    const RoutingIntent intent) noexcept {
    if (intent == RoutingIntent::AggressiveIOC &&
        lhs.executable_price != rhs.executable_price) {
        return side == Side::Buy
            ? lhs.executable_price.units < rhs.executable_price.units
            : lhs.executable_price.units > rhs.executable_price.units;
    }
    if (lhs.score.total != rhs.score.total) {
        return lhs.score.total > rhs.score.total;
    }
    if (intent == RoutingIntent::AggressiveIOC &&
        lhs.available_depth != rhs.available_depth) {
        return lhs.available_depth.units > rhs.available_depth.units;
    }
    if (lhs.configured_priority != rhs.configured_priority) {
        return lhs.configured_priority < rhs.configured_priority;
    }
    return lhs.venue < rhs.venue;
}

[[nodiscard]] const PinState* find_pin(
    const DecodedDispatch& decoded,
    const VenueId venue) noexcept {
    for (std::size_t index{}; index < decoded.pin_count; ++index) {
        if (decoded.pins[index].venue == venue) {
            return &decoded.pins[index];
        }
    }
    return nullptr;
}

[[nodiscard]] bool decode_parent_record(
    const recorder::DurableRecordView& record,
    DecodedDispatch& decoded) noexcept {
    Reader reader(record.payload);
    std::uint32_t magic{};
    std::uint8_t domain{};
    std::uint64_t parent_id{};
    std::uint32_t instrument{};
    std::uint8_t side{};
    std::uint64_t quantity{};
    std::int64_t price{};
    std::uint8_t intent{};
    if (!reader.u32(magic) || !reader.u64(decoded.producer_sequence) ||
        !reader.u64(decoded.leadership_epoch) ||
        !reader.u64(decoded.configuration_generation) ||
        !reader.u64(decoded.decision_time_ns) ||
        !reader.u64(decoded.commit_time_ns) ||
        !reader.u64(decoded.fence_time_ns) || !reader.u8(domain) ||
        !reader.u64(parent_id) || !reader.u32(instrument) ||
        !reader.u8(side) || !reader.u64(quantity) || !reader.i64(price) ||
        !reader.u8(intent) || !reader.done()) {
        return false;
    }
    if (magic != kDispatchMagic || decoded.producer_sequence == 0U ||
        decoded.leadership_epoch == 0U ||
        decoded.configuration_generation == 0U ||
        decoded.decision_time_ns == 0U ||
        decoded.commit_time_ns < decoded.decision_time_ns ||
        decoded.fence_time_ns == 0U || domain > 1U || parent_id == 0U ||
        instrument == 0U || quantity == 0U || price <= 0 ||
        !valid_side_value(side) || !valid_intent_value(intent)) {
        return false;
    }
    decoded.required_domain =
        static_cast<recorder::DurabilityDomain>(domain);
    decoded.route.parent = ParentOrder{
        ParentOrderId{parent_id},
        InstrumentId{instrument},
        static_cast<Side>(side),
        Quantity{quantity},
        Price{price},
        static_cast<RoutingIntent>(intent),
    };
    return true;
}

[[nodiscard]] bool decode_pinned_record(
    const recorder::DurableRecordView& record,
    DecodedDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader(record.payload);
    decoded.dispatch_record_version = dispatch_record_version;
    if (!reader.u16(decoded.pin_count) || decoded.pin_count == 0U ||
        decoded.pin_count > decoded.pins.size()) {
        return false;
    }
    std::size_t applied_count{};
    for (std::size_t index{}; index < decoded.pin_count; ++index) {
        PinState& pin = decoded.pins[index];
        dataplane::AdaptivePolicyOverlay& overlay =
            decoded.adaptive_overlays[index];
        std::uint32_t venue{};
        std::uint64_t feed_epoch{};
        std::uint64_t source_generation{};
        if (!reader.u32(venue) ||
            !reader.u64(pin.adjacency_generation) ||
            !reader.boolean(pin.has_book_version)) {
            return false;
        }
        pin.venue = VenueId{venue};
        if (!pin.venue || pin.adjacency_generation == 0U) {
            return false;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (decoded.pins[prior].venue == pin.venue) {
                return false;
            }
        }
        if (pin.has_book_version) {
            if (!read_book_version(reader, pin.book_version) ||
                pin.book_version.venue != pin.venue ||
                pin.book_version.instrument != decoded.route.parent.instrument) {
                return false;
            }
        }
        if (!reader.u64(feed_epoch) || !reader.u64(source_generation) ||
            !reader.u64(pin.tx_credit_generation) ||
            !reader.u64(pin.throttle_generation) ||
            !reader.u32(pin.normal_tx_credits) ||
            !reader.u32(pin.cancel_tx_credits) ||
            !reader.u32(pin.throttle_credits)) {
            return false;
        }
        if (dispatch_record_version >= kAdaptiveDispatchRecordVersion) {
            if (!read_adaptive_overlay(reader, overlay)) {
                return false;
            }
            applied_count += overlay.applied ? 1U : 0U;
            if (overlay.applied) {
                const policy::VenueFactVersions& facts =
                    overlay.fact_pin.versions;
                if (overlay.fact_pin.venue != pin.venue ||
                    overlay.order.parent_order_id != decoded.route.parent.id ||
                    overlay.order.instrument !=
                        decoded.route.parent.instrument ||
                    overlay.order.side != decoded.route.parent.side ||
                    overlay.order.intent != decoded.route.parent.intent ||
                    overlay.order.quantity != decoded.route.parent.quantity ||
                    overlay.order.limit_price !=
                        decoded.route.parent.limit_price ||
                    overlay.order.decision_monotonic_ns >
                        decoded.decision_time_ns ||
                    overlay.order.decision_utc_ns <
                        overlay.versions.compliance_effective_from_utc_ns ||
                    (overlay.versions.compliance_effective_until_utc_ns != 0U &&
                     overlay.order.decision_utc_ns >=
                         overlay.versions.compliance_effective_until_utc_ns) ||
                    (facts.route_generation != 0U &&
                     facts.route_generation != pin.adjacency_generation) ||
                    (facts.market_generation != 0U &&
                     (!pin.has_book_version ||
                      facts.market_generation !=
                          pin.book_version.view_generation)) ||
                    (facts.throttle_generation != 0U &&
                     facts.throttle_generation != pin.throttle_generation) ||
                    (facts.tx_credit_generation != 0U &&
                     facts.tx_credit_generation !=
                         pin.tx_credit_generation)) {
                    return false;
                }
                if (overlay.eligible &&
                    (!facts.base_valid() ||
                     facts.fee_generation !=
                         overlay.versions.fee_generation ||
                     facts.metric_generation !=
                         overlay.versions.metric_generation)) {
                    return false;
                }
            }
        }

        if (!reader.boolean(pin.view_present)) {
            return false;
        }
        if (!pin.view_present) {
            if (overlay.eligible) {
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
        std::uint64_t publication_time{};
        std::uint64_t view_tx_generation{};
        std::uint64_t view_throttle_generation{};
        std::uint32_t view_normal{};
        std::uint32_t view_cancel{};
        std::uint32_t view_throttle{};
        if (!reader.u32(view_venue) || !reader.u32(view_instrument) ||
            !reader.u64(view_adjacency) || !reader.u8(market_state) ||
            !reader.u8(order_entry_state) || !reader.u8(egress_state) ||
            !reader.u8(instrument_state) ||
            !reader.u64(view_feed_epoch) ||
            !reader.u64(view_source_generation) ||
            !reader.u64(pin.receive_time_ns) ||
            !reader.u64(publication_time) ||
            !reader.u64(pin.maximum_quote_age_ns) ||
            !reader.u64(view_tx_generation) ||
            !reader.u64(view_throttle_generation) ||
            !reader.u32(view_normal) || !reader.u32(view_cancel) ||
            !reader.u32(view_throttle) || !reader.boolean(pin.has_book) ||
            !reader.boolean(pin.source_healthy) ||
            !reader.boolean(pin.conversion_compatible) ||
            !reader.boolean(pin.risk_allowed)) {
            return false;
        }
        if (view_venue != venue ||
            view_instrument != decoded.route.parent.instrument.value() ||
            view_adjacency != pin.adjacency_generation || market_state > 4U ||
            order_entry_state > 3U || egress_state > 1U ||
            instrument_state > 3U || view_feed_epoch != feed_epoch ||
            view_source_generation != source_generation ||
            view_tx_generation != pin.tx_credit_generation ||
            view_throttle_generation != pin.throttle_generation ||
            view_normal != pin.normal_tx_credits ||
            view_cancel != pin.cancel_tx_credits ||
            view_throttle != pin.throttle_credits ||
            pin.receive_time_ns == 0U || publication_time == 0U ||
            publication_time < pin.receive_time_ns ||
            pin.maximum_quote_age_ns == 0U ||
            pin.has_book != pin.has_book_version) {
            return false;
        }
        pin.market_state = static_cast<MarketDataState>(market_state);
        pin.order_entry_state =
            static_cast<OrderEntryState>(order_entry_state);
        pin.egress_state = static_cast<EgressState>(egress_state);
        pin.instrument_state =
            static_cast<InstrumentState>(instrument_state);
        if (pin.has_book) {
            std::uint32_t channel{};
            std::uint32_t profile{};
            std::int64_t exchange_time{};
            bool truncated{};
            if (!reader.u32(channel) || !reader.u32(profile) ||
                !reader.i64(exchange_time) || !reader.boolean(truncated) ||
                channel == 0U || profile == 0U) {
                return false;
            }
        }
        if (overlay.eligible) {
            const policy::RankedCandidate& evidence = overlay.evidence;
            if (!pin.has_book || !pin.source_healthy ||
                !pin.conversion_compatible || !pin.risk_allowed ||
                pin.market_state != MarketDataState::Live ||
                pin.order_entry_state != OrderEntryState::Ready ||
                pin.egress_state != EgressState::Ready ||
                pin.instrument_state != InstrumentState::Trading ||
                overlay.order.decision_monotonic_ns < pin.receive_time_ns ||
                evidence.quote_age_ns !=
                    overlay.order.decision_monotonic_ns -
                        pin.receive_time_ns ||
                evidence.maximum_quote_age_ns !=
                    pin.maximum_quote_age_ns ||
                evidence.normal_tx_credits != pin.normal_tx_credits ||
                evidence.throttle_credits != pin.throttle_credits) {
                return false;
            }
        }
    }

    if (dispatch_record_version == fastpath::kDispatchRecordVersion &&
        applied_count != 0U) {
        if (applied_count != decoded.pin_count) {
            return false;
        }
        const dataplane::AdaptivePolicyOverlay& reference =
            decoded.adaptive_overlays[0U];
        std::array<const policy::RankedCandidate*,
                   dataplane::kMaximumCandidatesPerInstrument>
            evidence_by_rank{};
        std::array<bool, dataplane::kMaximumCandidatesPerInstrument>
            rank_seen{};
        std::size_t eligible_count{};
        for (std::size_t index{}; index < decoded.pin_count; ++index) {
            const dataplane::AdaptivePolicyOverlay& overlay =
                decoded.adaptive_overlays[index];
            if (!overlay.applied || overlay.versions != reference.versions ||
                overlay.order != reference.order) {
                return false;
            }
            if (!overlay.eligible) {
                continue;
            }
            if (overlay.rank >= decoded.pin_count || rank_seen[overlay.rank]) {
                return false;
            }
            rank_seen[overlay.rank] = true;
            evidence_by_rank[overlay.rank] = &overlay.evidence;
            ++eligible_count;
        }
        if (eligible_count == 0U) {
            return false;
        }
        for (std::size_t rank{}; rank < eligible_count; ++rank) {
            if (!rank_seen[rank] ||
                (rank != 0U && adaptive_candidate_precedes(
                    *evidence_by_rank[rank],
                    *evidence_by_rank[rank - 1U],
                    reference.order.side,
                    reference.order.intent))) {
                return false;
            }
        }
    }

    std::uint16_t slice_count{};
    if (!reader.u16(slice_count) ||
        slice_count > fastpath::kMaximumRecordedDepthSlices) {
        return false;
    }
    const Side expected_book_side =
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
            side != static_cast<std::uint8_t>(expected_book_side) ||
            level >= md::kMaxDepthLevels || price <= 0 || quantity == 0U ||
            order_count == 0U || find_pin(decoded, VenueId{venue}) == nullptr) {
            return false;
        }
        PinState* slice_pin{};
        for (std::size_t pin_index{}; pin_index < decoded.pin_count;
             ++pin_index) {
            if (decoded.pins[pin_index].venue == VenueId{venue}) {
                slice_pin = &decoded.pins[pin_index];
                break;
            }
        }
        if (slice_pin == nullptr ||
            (level == 0U && slice_pin->first_slice_seen)) {
            return false;
        }
        if (level == 0U) {
            slice_pin->first_slice_seen = true;
            slice_pin->first_recorded_price = Price{price};
            slice_pin->first_recorded_quantity = Quantity{quantity};
        }
    }
    if (!reader.done()) {
        return false;
    }
    if (dispatch_record_version >= kAdaptiveDispatchRecordVersion) {
        for (std::size_t index{}; index < decoded.pin_count; ++index) {
            const dataplane::AdaptivePolicyOverlay& overlay =
                decoded.adaptive_overlays[index];
            if (!overlay.eligible) {
                continue;
            }
            if (decoded.route.parent.intent == RoutingIntent::AggressiveIOC) {
                const PinState& pin = decoded.pins[index];
                if (!pin.first_slice_seen ||
                    overlay.evidence.executable_price !=
                        pin.first_recorded_price ||
                    overlay.evidence.available_depth !=
                        pin.first_recorded_quantity) {
                    return false;
                }
            } else if (overlay.evidence.executable_price !=
                       decoded.route.parent.limit_price) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool decode_decision_record(
    const recorder::DurableRecordView& record,
    DecodedDispatch& decoded) noexcept {
    Reader reader(record.payload);
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
        remainder_reason > 8U || reject_reason != 0U ||
        exclusion_count > dataplane::kMaximumCandidatesPerInstrument) {
        return false;
    }
    for (std::size_t index{}; index < exclusion_count; ++index) {
        std::uint32_t venue{};
        std::uint8_t reason{};
        if (!reader.u32(venue) || !reader.u8(reason) || venue == 0U ||
            reason > 18U || find_pin(decoded, VenueId{venue}) == nullptr) {
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
        std::uint64_t child_id{};
        std::uint8_t wire_size{};
        std::span<const std::byte> wire;
        if (!reader.u32(venue) || !reader.u64(quantity) ||
            !reader.i64(limit) || !reader.u64(venue_quantity) ||
            !reader.i64(venue_limit) ||
            !reader.u32(child.allocation_order) ||
            !reader.u64(child.adjacency_generation) ||
            !read_book_version(reader, child.book_version) ||
            !reader.u64(child.venue_outstanding_before.units) ||
            !reader.u64(child.venue_outstanding_after.units) ||
            !reader.u64(child_id) ||
            !reader.u64(child.reservation.dispatch_attempt_id) ||
            !reader.u8(wire_size) ||
            wire_size == 0U || wire_size > child.wire_identity.bytes.size() ||
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
        child.allocation_limit = Price{limit};
        child.venue_quantity = Quantity{venue_quantity};
        child.venue_limit = Price{venue_limit};
        child.wire_identity.size = wire_size;
        std::copy(wire.begin(), wire.end(), child.wire_identity.bytes.begin());
        child.reservation.wire_identity = child.wire_identity;
        const PinState* const pin = find_pin(decoded, child.reservation.venue);
        if (!child.reservation.child_order_id ||
            child.reservation.dispatch_attempt_id == 0U ||
            !child.reservation.venue ||
            !child.reservation.remaining_quantity || limit <= 0 ||
            venue_quantity == 0U || venue_limit <= 0 ||
            child.allocation_order >= decoded.route.child_count ||
            child.adjacency_generation == 0U || pin == nullptr ||
            child.book_version.venue != child.reservation.venue ||
            child.book_version.instrument != decoded.route.parent.instrument ||
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
                decoded.children[prior].allocation_order ==
                    child.allocation_order) {
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
    const auto parent_total = checked_add(Quantity{allocated}, Quantity{remainder});
    return reader.done() && summed == Quantity{allocated} &&
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
    DecodedDispatch& decoded,
    const std::uint16_t dispatch_record_version) noexcept {
    Reader reader(record.payload);
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
            dispatch_class !=
                static_cast<std::uint8_t>(
                    fastpath::DispatchClass::NewOrReplace) ||
            wire_size != expected.wire_identity.size ||
            !std::equal(
                wire.begin(),
                wire.end(),
                expected.wire_identity.bytes.begin())) {
            return false;
        }
        DecodedChild& captured = decoded.children[index];
        captured.frame_size = frame_size;
        captured.dispatch_class = dispatch_class;
        std::copy(frame.begin(), frame.end(), captured.frame.begin());
    }
    if (dispatch_record_version == fastpath::kDispatchRecordVersion) {
        std::uint16_t ownership_count{};
        if (!reader.u16(ownership_count) || ownership_count != count) {
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
                    wire.begin(),
                    wire.end(),
                    expected.wire_identity.bytes.begin())) {
                return false;
            }
            expected.reservation.venue_session_id = venue_session_id;
            expected.reservation.venue_session_epoch = venue_session_epoch;
            expected.reservation.ownership_generation = ownership_generation;
            expected.reservation.last_dispatch_attempt_id =
                last_dispatch_attempt_id;
            expected.reservation.wire_identity = expected.wire_identity;
            decoded.route.children[index] = expected.reservation;
        }
    } else {
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
    }
    return reader.done();
}

[[nodiscard]] bool validate_external_effect_envelope(
    const recorder::DurableToken& token,
    const DecodedDispatch& decoded) noexcept {
    if (token.authorization_envelope_version ==
        recorder::AuthorizationEnvelopeVersion::LegacyRangeV0) {
        return decoded.dispatch_record_version == kLegacyDispatchRecordVersion ||
               decoded.dispatch_record_version == kAdaptiveDispatchRecordVersion;
    }
    if (token.authorization_envelope_version !=
            recorder::AuthorizationEnvelopeVersion::ExactEffectsV1 ||
        decoded.dispatch_record_version != fastpath::kDispatchRecordVersion ||
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

[[nodiscard]] bool validate_adaptive_after_decision(
    const DecodedDispatch& decoded) noexcept {
    if (decoded.dispatch_record_version == kLegacyDispatchRecordVersion) {
        return true;
    }
    if (decoded.dispatch_record_version != kAdaptiveDispatchRecordVersion &&
        decoded.dispatch_record_version != fastpath::kDispatchRecordVersion) {
        return false;
    }
    for (std::size_t index{}; index < decoded.pin_count; ++index) {
        const dataplane::AdaptivePolicyOverlay& overlay =
            decoded.adaptive_overlays[index];
        if (!overlay.applied) {
            continue;
        }
        const std::uint64_t fact_risk_generation =
            overlay.fact_pin.versions.risk_generation;
        if ((fact_risk_generation != 0U &&
             fact_risk_generation != decoded.route.risk_generation) ||
            (overlay.eligible &&
             fact_risk_generation != decoded.route.risk_generation)) {
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
    DecodedDispatch& decoded) noexcept {
    if (batch.record_count() != 4U) {
        return DecodeStatus::InvalidRecordSet;
    }
    std::array<recorder::DurableRecordView, 4U> records{};
    std::uint16_t dispatch_record_version{};
    for (std::size_t index{}; index < records.size(); ++index) {
        const auto record = batch.record(index);
        if (!record) {
            return DecodeStatus::InvalidRecordSet;
        }
        if (index == 0U) {
            dispatch_record_version = record->version;
            if (dispatch_record_version != kLegacyDispatchRecordVersion &&
                dispatch_record_version != kAdaptiveDispatchRecordVersion &&
                dispatch_record_version !=
                    fastpath::kDispatchRecordVersion) {
                return DecodeStatus::InvalidRecordSet;
            }
        } else if (record->version != dispatch_record_version) {
            return DecodeStatus::InvalidRecordSet;
        }
        records[index] = *record;
    }
    if (records[0].type != static_cast<std::uint16_t>(
            fastpath::DispatchRecordType::ParentCommand) ||
        records[1].type != static_cast<std::uint16_t>(
            fastpath::DispatchRecordType::PinnedRoutingInputs) ||
        records[2].type != static_cast<std::uint16_t>(
            fastpath::DispatchRecordType::RouteAndRiskDecision) ||
        records[3].type != static_cast<std::uint16_t>(
            fastpath::DispatchRecordType::ChildDispatches)) {
        return DecodeStatus::InvalidRecordSet;
    }
    if (!decode_parent_record(records[0], decoded) ||
        !decode_pinned_record(
            records[1], decoded, dispatch_record_version) ||
        !decode_decision_record(records[2], decoded) ||
        !validate_adaptive_after_decision(decoded) ||
        !decode_frames_record(records[3], decoded, dispatch_record_version)) {
        return DecodeStatus::InvalidPayload;
    }
    return DecodeStatus::Ok;
}

}  // namespace

struct RecoveryLedger::Impl final {
    enum class SlotState : std::uint8_t {
        Empty,
        Occupied,
        Tombstone,
    };

    struct ParentEntry final {
        SlotState state{SlotState::Empty};
        dataplane::RecoveredParentReservation route{};
    };

    struct ClientEntry final {
        bool occupied{};
        std::uint64_t client_id{};
        std::uint64_t sequence_high_water{};
    };

    struct DedupeEntry final {
        bool occupied{};
        std::uint64_t client_id{};
        std::uint64_t client_sequence{};
        std::uint64_t source_sequence{};
        RecoveryRequestDigest digest{};
    };

    struct ReplayEntry final {
        bool occupied{};
        std::uint64_t batch_id{};
        recorder::HashChainDigest batch_hash{};
        recorder::HashChainDigest chain_hash{};
    };

    dataplane::RouteKernel* kernel{};
    RecoveryConfiguration configuration{};
    std::unique_ptr<ParentEntry[]> parents;
    std::unique_ptr<ClientEntry[]> clients;
    std::unique_ptr<DedupeEntry[]> dedupe;
    std::unique_ptr<ReplayEntry[]> replay;
    std::array<dataplane::RecoveryVenueCreditState,
               dataplane::kMaximumDataplaneVenues>
        venue_credits{};
    std::size_t active_parents{};
    std::size_t active_children{};
    std::size_t client_count{};
    std::size_t dedupe_count{};
    std::size_t replay_count{};
    std::size_t replay_next{};
    std::size_t venue_credit_count{};
    std::uint64_t replayed_through_batch_id{};
    std::uint64_t producer_sequence_high_water{};
    std::uint64_t source_sequence_high_water{};
    recorder::HashChainDigest latest_chain_hash{};
    bool ready{};
    bool checkpoint_restored{};

    Impl(
        dataplane::RouteKernel& route_kernel,
        const RecoveryConfiguration& source)
        : kernel(&route_kernel),
          configuration(source),
          parents(std::make_unique<ParentEntry[]>(
              source.active_parent_capacity)),
          clients(std::make_unique<ClientEntry[]>(source.client_capacity)),
          dedupe(std::make_unique<DedupeEntry[]>(
              source.client_dedupe_capacity)),
          replay(std::make_unique<ReplayEntry[]>(
              source.replay_dedupe_capacity)),
          replayed_through_batch_id(source.replay_anchor.through_batch_id),
          producer_sequence_high_water(
              source.replay_anchor.producer_sequence_high_water),
          latest_chain_hash(source.replay_anchor.chain_hash) {
        ready = kernel->candidate_table_generation() ==
                    configuration.candidate_table_generation &&
                kernel->risk_generation() == configuration.risk_generation &&
                kernel->begin_recovery();
    }

    [[nodiscard]] static constexpr std::uint64_t mix(
        std::uint64_t value) noexcept {
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::size_t parent_slot(
        const ParentOrderId parent) const noexcept {
        if (!parent || configuration.active_parent_capacity == 0U) {
            return configuration.active_parent_capacity;
        }
        std::size_t slot = static_cast<std::size_t>(
            mix(parent.value()) % configuration.active_parent_capacity);
        for (std::size_t probe{};
             probe < configuration.active_parent_capacity;
             ++probe) {
            const ParentEntry& entry = parents[slot];
            if (entry.state == SlotState::Empty) {
                return configuration.active_parent_capacity;
            }
            if (entry.state == SlotState::Occupied &&
                entry.route.parent.id == parent) {
                return slot;
            }
            slot = (slot + 1U) % configuration.active_parent_capacity;
        }
        return configuration.active_parent_capacity;
    }

    [[nodiscard]] std::size_t parent_insertion_slot(
        const ParentOrderId parent,
        bool& duplicate) const noexcept {
        duplicate = false;
        std::size_t first_tombstone = configuration.active_parent_capacity;
        std::size_t slot = static_cast<std::size_t>(
            mix(parent.value()) % configuration.active_parent_capacity);
        for (std::size_t probe{};
             probe < configuration.active_parent_capacity;
             ++probe) {
            const ParentEntry& entry = parents[slot];
            if (entry.state == SlotState::Occupied &&
                entry.route.parent.id == parent) {
                duplicate = true;
                return slot;
            }
            if (entry.state == SlotState::Tombstone &&
                first_tombstone == configuration.active_parent_capacity) {
                first_tombstone = slot;
            }
            if (entry.state == SlotState::Empty) {
                return first_tombstone == configuration.active_parent_capacity ?
                    slot : first_tombstone;
            }
            slot = (slot + 1U) % configuration.active_parent_capacity;
        }
        return first_tombstone;
    }

    [[nodiscard]] std::size_t client_slot(
        const std::uint64_t client_id) const noexcept {
        std::size_t slot = static_cast<std::size_t>(
            mix(client_id) % configuration.client_capacity);
        for (std::size_t probe{}; probe < configuration.client_capacity;
             ++probe) {
            const ClientEntry& entry = clients[slot];
            if (!entry.occupied) {
                return configuration.client_capacity;
            }
            if (entry.client_id == client_id) {
                return slot;
            }
            slot = (slot + 1U) % configuration.client_capacity;
        }
        return configuration.client_capacity;
    }

    [[nodiscard]] std::size_t client_insertion_slot(
        const std::uint64_t client_id,
        bool& duplicate) const noexcept {
        duplicate = false;
        std::size_t slot = static_cast<std::size_t>(
            mix(client_id) % configuration.client_capacity);
        for (std::size_t probe{}; probe < configuration.client_capacity;
             ++probe) {
            const ClientEntry& entry = clients[slot];
            if (!entry.occupied) {
                return slot;
            }
            if (entry.client_id == client_id) {
                duplicate = true;
                return slot;
            }
            slot = (slot + 1U) % configuration.client_capacity;
        }
        return configuration.client_capacity;
    }

    [[nodiscard]] std::size_t dedupe_slot(
        const std::uint64_t client_id,
        const std::uint64_t sequence) const noexcept {
        const std::uint64_t key = mix(client_id) ^ mix(sequence);
        std::size_t slot = static_cast<std::size_t>(
            mix(key) % configuration.client_dedupe_capacity);
        for (std::size_t probe{};
             probe < configuration.client_dedupe_capacity;
             ++probe) {
            const DedupeEntry& entry = dedupe[slot];
            if (!entry.occupied) {
                return configuration.client_dedupe_capacity;
            }
            if (entry.client_id == client_id &&
                entry.client_sequence == sequence) {
                return slot;
            }
            slot = (slot + 1U) % configuration.client_dedupe_capacity;
        }
        return configuration.client_dedupe_capacity;
    }

    [[nodiscard]] std::size_t dedupe_insertion_slot(
        const std::uint64_t client_id,
        const std::uint64_t sequence,
        bool& duplicate) const noexcept {
        duplicate = false;
        const std::uint64_t key = mix(client_id) ^ mix(sequence);
        std::size_t slot = static_cast<std::size_t>(
            mix(key) % configuration.client_dedupe_capacity);
        for (std::size_t probe{};
             probe < configuration.client_dedupe_capacity;
             ++probe) {
            const DedupeEntry& entry = dedupe[slot];
            if (!entry.occupied) {
                return slot;
            }
            if (entry.client_id == client_id &&
                entry.client_sequence == sequence) {
                duplicate = true;
                return slot;
            }
            slot = (slot + 1U) % configuration.client_dedupe_capacity;
        }
        return configuration.client_dedupe_capacity;
    }

    [[nodiscard]] const ReplayEntry* replay_entry(
        const std::uint64_t batch_id) const noexcept {
        for (std::size_t index{};
             index < configuration.replay_dedupe_capacity;
             ++index) {
            if (replay[index].occupied && replay[index].batch_id == batch_id) {
                return &replay[index];
            }
        }
        return nullptr;
    }

    void remember_replay(const recorder::DurableToken& token) noexcept {
        ReplayEntry& entry = replay[replay_next];
        entry.occupied = true;
        entry.batch_id = token.batch_id;
        entry.batch_hash = token.batch_hash;
        entry.chain_hash = token.chain_hash;
        replay_next =
            (replay_next + 1U) % configuration.replay_dedupe_capacity;
        replay_count = std::min(
            replay_count + 1U, configuration.replay_dedupe_capacity);
    }

    [[nodiscard]] std::size_t venue_credit_slot(
        const VenueId venue) const noexcept {
        for (std::size_t index{}; index < venue_credit_count; ++index) {
            if (venue_credits[index].venue == venue) {
                return index;
            }
        }
        return venue_credits.size();
    }

    [[nodiscard]] bool can_note_credit(
        const dataplane::RecoveredChildReservation& child) const noexcept {
        const std::size_t slot = venue_credit_slot(child.venue);
        if (slot == venue_credits.size()) {
            return venue_credit_count < venue_credits.size();
        }
        const dataplane::RecoveryVenueCreditState& credit =
            venue_credits[slot];
        const std::uint32_t normal_reserved =
            credit.tx_credit_generation == child.tx_credit_generation ?
                credit.reserved_normal_tx_credits : 0U;
        const std::uint32_t cancel_reserved =
            credit.tx_credit_generation == child.tx_credit_generation ?
                credit.reserved_cancel_tx_credits : 0U;
        const std::uint32_t throttle_reserved =
            credit.throttle_generation == child.throttle_generation ?
                credit.reserved_throttle_credits : 0U;
        return (credit.tx_credit_generation != child.tx_credit_generation ||
                (credit.reported_normal_tx_credits ==
                     child.reported_normal_tx_credits &&
                 credit.reported_cancel_tx_credits ==
                     child.reported_cancel_tx_credits)) &&
               (credit.throttle_generation != child.throttle_generation ||
                credit.reported_throttle_credits ==
                    child.reported_throttle_credits) &&
               child.normal_tx_credit_delta <=
                   child.reported_normal_tx_credits -
                       std::min(
                           child.reported_normal_tx_credits,
                           normal_reserved) &&
               child.cancel_tx_credit_delta <=
                   child.reported_cancel_tx_credits -
                       std::min(
                           child.reported_cancel_tx_credits,
                           cancel_reserved) &&
               child.throttle_credit_delta <=
                   child.reported_throttle_credits -
                       std::min(
                           child.reported_throttle_credits,
                           throttle_reserved);
    }

    void note_credit(
        const dataplane::RecoveredChildReservation& child) noexcept {
        std::size_t slot = venue_credit_slot(child.venue);
        if (slot == venue_credits.size()) {
            slot = venue_credit_count++;
            venue_credits[slot].venue = child.venue;
            venue_credits[slot].found = true;
        }
        dataplane::RecoveryVenueCreditState& credit = venue_credits[slot];
        if (credit.tx_credit_generation != child.tx_credit_generation) {
            credit.tx_credit_generation = child.tx_credit_generation;
            credit.reported_normal_tx_credits =
                child.reported_normal_tx_credits;
            credit.reported_cancel_tx_credits =
                child.reported_cancel_tx_credits;
            credit.reserved_normal_tx_credits = 0U;
            credit.reserved_cancel_tx_credits = 0U;
        }
        if (credit.throttle_generation != child.throttle_generation) {
            credit.throttle_generation = child.throttle_generation;
            credit.reported_throttle_credits =
                child.reported_throttle_credits;
            credit.reserved_throttle_credits = 0U;
        }
        credit.reserved_normal_tx_credits += child.normal_tx_credit_delta;
        credit.reserved_cancel_tx_credits += child.cancel_tx_credit_delta;
        credit.reserved_throttle_credits += child.throttle_credit_delta;
    }

    [[nodiscard]] dataplane::RecoveredChildReservation* find_child(
        ParentEntry& parent,
        const ChildOrderId child) noexcept {
        for (std::size_t index{}; index < parent.route.child_count; ++index) {
            if (parent.route.children[index].child_order_id == child) {
                return &parent.route.children[index];
            }
        }
        return nullptr;
    }

    void retire_parent_if_empty(ParentEntry& parent) noexcept {
        bool active{};
        for (std::size_t index{}; index < parent.route.child_count; ++index) {
            if (parent.route.children[index].remaining_quantity.units != 0U) {
                active = true;
                break;
            }
        }
        if (!active) {
            parent.state = SlotState::Tombstone;
            --active_parents;
        }
    }
};

namespace {

[[nodiscard]] bool valid_configuration(
    const RecoveryConfiguration& configuration) noexcept {
    if (configuration.active_parent_capacity == 0U ||
        configuration.active_parent_capacity >
            dataplane::kMaximumActiveParents ||
        configuration.client_capacity == 0U ||
        configuration.client_dedupe_capacity == 0U ||
        configuration.replay_dedupe_capacity == 0U ||
        configuration.leadership_epoch == 0U ||
        configuration.configuration_generation == 0U ||
        configuration.candidate_table_generation == 0U ||
        configuration.risk_generation == 0U) {
        return false;
    }
    if (configuration.replay_anchor.through_batch_id == 0U) {
        return configuration.replay_anchor.producer_sequence_high_water == 0U &&
               all_zero(configuration.replay_anchor.chain_hash);
    }
    return configuration.replay_anchor.producer_sequence_high_water != 0U &&
           !all_zero(configuration.replay_anchor.chain_hash);
}

[[nodiscard]] RecoveryRecordStatus map_accounting_status(
    const dataplane::RecoveryAccountingStatus status) noexcept {
    switch (status) {
        case dataplane::RecoveryAccountingStatus::Applied:
        case dataplane::RecoveryAccountingStatus::AlreadyApplied:
            return RecoveryRecordStatus::Applied;
        case dataplane::RecoveryAccountingStatus::ParentNotFound:
            return RecoveryRecordStatus::ParentNotFound;
        case dataplane::RecoveryAccountingStatus::ChildNotFound:
            return RecoveryRecordStatus::ChildNotFound;
        case dataplane::RecoveryAccountingStatus::AttemptMismatch:
            return RecoveryRecordStatus::AttemptMismatch;
        case dataplane::RecoveryAccountingStatus::InvalidQuantity:
            return RecoveryRecordStatus::InvalidRecord;
        case dataplane::RecoveryAccountingStatus::NotRecoveryOnly:
        case dataplane::RecoveryAccountingStatus::InvariantViolation:
        case dataplane::RecoveryAccountingStatus::GenerationExhausted:
            return RecoveryRecordStatus::InvariantViolation;
    }
    return RecoveryRecordStatus::InvariantViolation;
}

}  // namespace

RecoveryLedger::RecoveryLedger(
    dataplane::RouteKernel& kernel,
    const RecoveryConfiguration configuration) {
    if (!valid_configuration(configuration)) {
        throw std::invalid_argument("invalid fast-recovery configuration");
    }
    impl_ = std::make_unique<Impl>(kernel, configuration);
    if (!impl_->ready) {
        throw std::invalid_argument(
            "route kernel is not empty or recovery generations mismatch");
    }
}

RecoveryLedger::~RecoveryLedger() = default;
RecoveryLedger::RecoveryLedger(RecoveryLedger&&) noexcept = default;
RecoveryLedger& RecoveryLedger::operator=(RecoveryLedger&&) noexcept = default;

bool RecoveryLedger::ready() const noexcept {
    return impl_ && impl_->ready && impl_->kernel != nullptr &&
           impl_->kernel->recovery_only();
}

DispatchReplayResult RecoveryLedger::replay_dispatch_batch(
    const recorder::DurableToken& token,
    const recorder::DurableBatch& batch) noexcept {
    DispatchReplayResult result;
    result.batch_id = token.batch_id;
    if (!ready()) {
        return result;
    }
    if (const Impl::ReplayEntry* const replay =
            impl_->replay_entry(token.batch_id)) {
        result.status =
            replay->batch_hash == token.batch_hash &&
                    replay->chain_hash == token.chain_hash ?
                DispatchReplayStatus::AlreadyApplied :
                DispatchReplayStatus::IntegrityFailure;
        return result;
    }
    if (token.batch_id == impl_->replayed_through_batch_id &&
        token.chain_hash == impl_->latest_chain_hash) {
        result.status = DispatchReplayStatus::AlreadyApplied;
        return result;
    }
    if (!static_cast<bool>(token) || !batch.valid() ||
        token.batch_id == 0U ||
        token.producer_sequence != batch.producer_sequence() ||
        token.leadership_epoch != batch.leadership_epoch() ||
        token.configuration_generation !=
            batch.configuration_generation() ||
        token.dispatch_authorization != batch.dispatch_authorization() ||
        token.lane != recorder::DurabilityLane::Normal ||
        batch.lane() != recorder::DurabilityLane::Normal ||
        token.payload_size != batch.encoded_size()) {
        result.status = DispatchReplayStatus::TokenMismatch;
        return result;
    }
    if (!token_matches_batch(token, batch)) {
        result.status = DispatchReplayStatus::IntegrityFailure;
        return result;
    }
    if (token.leadership_epoch != impl_->configuration.leadership_epoch ||
        batch.leadership_epoch() != impl_->configuration.leadership_epoch) {
        result.status = DispatchReplayStatus::LeadershipEpochMismatch;
        return result;
    }
    if (token.configuration_generation !=
            impl_->configuration.configuration_generation ||
        batch.configuration_generation() !=
            impl_->configuration.configuration_generation) {
        result.status = DispatchReplayStatus::ConfigurationMismatch;
        return result;
    }
    if (token.batch_id <= impl_->replayed_through_batch_id) {
        result.status = DispatchReplayStatus::BatchSequenceRegression;
        return result;
    }
    if (token.producer_sequence <= impl_->producer_sequence_high_water) {
        result.status = DispatchReplayStatus::ProducerSequenceRegression;
        return result;
    }
    if (token.previous_hash != impl_->latest_chain_hash) {
        result.status = DispatchReplayStatus::ChainDiscontinuity;
        return result;
    }

    DecodedDispatch decoded;
    const DecodeStatus decoded_status = decode_dispatch_batch(batch, decoded);
    if (decoded_status == DecodeStatus::InvalidRecordSet) {
        result.status = DispatchReplayStatus::InvalidRecordSet;
        return result;
    }
    if (decoded_status != DecodeStatus::Ok) {
        result.status = DispatchReplayStatus::CanonicalDecodeFailed;
        return result;
    }
    if (!validate_external_effect_envelope(token, decoded)) {
        result.status = DispatchReplayStatus::TokenMismatch;
        return result;
    }
    result.parent = decoded.route.parent.id;
    result.child_count = decoded.route.child_count;
    const recorder::DispatchAuthorization authorization =
        token.dispatch_authorization;
    const bool durable_enough =
        decoded.required_domain ==
                recorder::DurabilityDomain::VolatileSoftwareReference ||
        token.domain == recorder::DurabilityDomain::HardwarePersistent;
    if (decoded.producer_sequence != token.producer_sequence ||
        decoded.leadership_epoch != token.leadership_epoch ||
        decoded.configuration_generation !=
            token.configuration_generation ||
        authorization.first_child_id != decoded.first_child_id ||
        authorization.first_attempt_id != decoded.first_attempt_id ||
        authorization.dispatch_count != decoded.route.child_count ||
        !durable_enough) {
        result.status = DispatchReplayStatus::TokenMismatch;
        return result;
    }
    if (decoded.route.candidate_table_generation !=
            impl_->configuration.candidate_table_generation ||
        decoded.route.risk_generation != impl_->configuration.risk_generation) {
        result.status = DispatchReplayStatus::GenerationMismatch;
        return result;
    }
    const dataplane::RecoveryIdentifierState identifiers =
        impl_->kernel->recovery_identifiers();
    if (decoded.first_child_id != identifiers.next_child_order_id ||
        decoded.first_attempt_id != identifiers.next_dispatch_attempt_id) {
        result.status = DispatchReplayStatus::BatchSequenceRegression;
        return result;
    }
    for (std::size_t index{}; index < decoded.route.child_count; ++index) {
        if (!impl_->can_note_credit(decoded.route.children[index])) {
            result.status = DispatchReplayStatus::CapacityExceeded;
            return result;
        }
    }
    bool duplicate{};
    const std::size_t parent_slot = impl_->parent_insertion_slot(
        decoded.route.parent.id, duplicate);
    if (duplicate || parent_slot == impl_->configuration.active_parent_capacity ||
        impl_->active_parents == impl_->configuration.active_parent_capacity) {
        result.status = DispatchReplayStatus::CapacityExceeded;
        return result;
    }
    result.import_status =
        impl_->kernel->restore_recovered_parent(decoded.route);
    if (result.import_status != dataplane::RecoveryImportStatus::Imported) {
        result.status = DispatchReplayStatus::RouteKernelRejected;
        return result;
    }
    Impl::ParentEntry& entry = impl_->parents[parent_slot];
    entry.state = Impl::SlotState::Occupied;
    entry.route = decoded.route;
    ++impl_->active_parents;
    impl_->active_children += decoded.route.child_count;
    for (std::size_t index{}; index < decoded.route.child_count; ++index) {
        impl_->note_credit(decoded.route.children[index]);
    }
    impl_->remember_replay(token);
    impl_->replayed_through_batch_id = token.batch_id;
    impl_->producer_sequence_high_water = token.producer_sequence;
    impl_->latest_chain_hash = token.chain_hash;
    result.status = DispatchReplayStatus::Applied;
    return result;
}

RecoveryRecordResult RecoveryLedger::apply_record(
    const RecoveryRecord& record) noexcept {
    RecoveryRecordResult result;
    if (!ready()) {
        return result;
    }
    const bool valid_type =
        record.type == RecoveryRecordType::Fill ||
        record.type == RecoveryRecordType::CancelIntent ||
        record.type == RecoveryRecordType::Terminal ||
        record.type == RecoveryRecordType::ReconcileWorking ||
        record.type == RecoveryRecordType::ReconcileTerminal;
    const bool quantity_valid =
        record.type == RecoveryRecordType::Fill ?
            record.quantity.units != 0U : record.quantity.units == 0U;
    const bool cancel_fields_valid =
        record.type == RecoveryRecordType::CancelIntent
        ? record.prior_dispatch_attempt_id != 0U &&
              record.dispatch_attempt_id >
                  record.prior_dispatch_attempt_id &&
              record.client_request_id != 0U &&
              record.client_session_epoch != 0U
        : record.prior_dispatch_attempt_id == 0U &&
              record.client_request_id == 0U &&
              record.client_session_epoch == 0U;
    if (!valid_type || record.source_sequence == 0U ||
        record.client_id == 0U || record.client_sequence == 0U ||
        all_zero(record.request_digest) || !record.parent || !record.child ||
        record.dispatch_attempt_id == 0U || !quantity_valid ||
        !cancel_fields_valid) {
        result.status = RecoveryRecordStatus::InvalidRecord;
        return result;
    }
    const std::size_t existing_dedupe = impl_->dedupe_slot(
        record.client_id, record.client_sequence);
    if (existing_dedupe != impl_->configuration.client_dedupe_capacity) {
        const Impl::DedupeEntry& entry = impl_->dedupe[existing_dedupe];
        result.status = entry.digest == record.request_digest ?
            RecoveryRecordStatus::DuplicateReplay :
            RecoveryRecordStatus::DuplicateConflict;
        return result;
    }
    const std::size_t existing_client =
        impl_->client_slot(record.client_id);
    if ((existing_client != impl_->configuration.client_capacity &&
         record.client_sequence <=
             impl_->clients[existing_client].sequence_high_water) ||
        record.source_sequence <= impl_->source_sequence_high_water) {
        result.status = RecoveryRecordStatus::TooOld;
        return result;
    }
    bool client_duplicate{};
    const std::size_t client_slot = impl_->client_insertion_slot(
        record.client_id, client_duplicate);
    bool dedupe_duplicate{};
    const std::size_t dedupe_slot = impl_->dedupe_insertion_slot(
        record.client_id, record.client_sequence, dedupe_duplicate);
    if (client_slot == impl_->configuration.client_capacity ||
        dedupe_slot == impl_->configuration.client_dedupe_capacity ||
        dedupe_duplicate ||
        (!client_duplicate &&
         impl_->client_count == impl_->configuration.client_capacity) ||
        impl_->dedupe_count ==
            impl_->configuration.client_dedupe_capacity) {
        result.status = RecoveryRecordStatus::CapacityExceeded;
        return result;
    }
    const std::size_t parent_slot = impl_->parent_slot(record.parent);
    if (parent_slot == impl_->configuration.active_parent_capacity) {
        result.status = RecoveryRecordStatus::ParentNotFound;
        return result;
    }
    Impl::ParentEntry& parent = impl_->parents[parent_slot];
    dataplane::RecoveredChildReservation* const child =
        impl_->find_child(parent, record.child);
    if (child == nullptr || child->remaining_quantity.units == 0U) {
        result.status = RecoveryRecordStatus::ChildNotFound;
        return result;
    }
    if ((record.type == RecoveryRecordType::CancelIntent &&
         child->last_dispatch_attempt_id !=
             record.prior_dispatch_attempt_id) ||
        (record.type != RecoveryRecordType::CancelIntent &&
         child->dispatch_attempt_id != record.dispatch_attempt_id)) {
        result.status = RecoveryRecordStatus::AttemptMismatch;
        return result;
    }

    switch (record.type) {
        case RecoveryRecordType::Fill:
            result.accounting_status = impl_->kernel->apply_recovered_fill(
                record.parent,
                record.child,
                record.dispatch_attempt_id,
                record.quantity);
            break;
        case RecoveryRecordType::CancelIntent:
            result.accounting_status = impl_->kernel->note_recovered_cancel(
                record.parent,
                record.child,
                record.prior_dispatch_attempt_id,
                record.dispatch_attempt_id,
                record.client_request_id,
                record.client_sequence,
                record.client_session_epoch);
            break;
        case RecoveryRecordType::Terminal:
        case RecoveryRecordType::ReconcileTerminal:
            result.accounting_status =
                impl_->kernel->reconcile_recovered_child(
                    record.parent,
                    record.child,
                    record.dispatch_attempt_id,
                    dataplane::RecoveredChildDisposition::Terminal);
            break;
        case RecoveryRecordType::ReconcileWorking:
            result.accounting_status =
                impl_->kernel->reconcile_recovered_child(
                    record.parent,
                    record.child,
                    record.dispatch_attempt_id,
                    dataplane::RecoveredChildDisposition::Working);
            break;
    }
    result.status = map_accounting_status(result.accounting_status);
    if (result.status != RecoveryRecordStatus::Applied) {
        return result;
    }

    switch (record.type) {
        case RecoveryRecordType::Fill:
            child->remaining_quantity = *checked_subtract(
                child->remaining_quantity, record.quantity);
            if (child->remaining_quantity.units == 0U) {
                child->reconciled = true;
                --impl_->active_children;
            }
            break;
        case RecoveryRecordType::CancelIntent:
            child->cancel_pending = true;
            child->pending_client_request_id = record.client_request_id;
            child->pending_client_sequence = record.client_sequence;
            child->pending_client_session_epoch =
                record.client_session_epoch;
            child->pending_cancel_attempt_id = record.dispatch_attempt_id;
            child->last_dispatch_attempt_id = record.dispatch_attempt_id;
            ++child->ownership_generation;
            break;
        case RecoveryRecordType::Terminal:
        case RecoveryRecordType::ReconcileTerminal:
            child->remaining_quantity = Quantity{};
            child->reconciled = true;
            --impl_->active_children;
            break;
        case RecoveryRecordType::ReconcileWorking:
            child->reconciled = true;
            break;
    }
    impl_->retire_parent_if_empty(parent);

    if (!client_duplicate) {
        impl_->clients[client_slot] = Impl::ClientEntry{
            true, record.client_id, record.client_sequence};
        ++impl_->client_count;
    } else {
        impl_->clients[client_slot].sequence_high_water = std::max(
            impl_->clients[client_slot].sequence_high_water,
            record.client_sequence);
    }
    impl_->dedupe[dedupe_slot] = Impl::DedupeEntry{
        true,
        record.client_id,
        record.client_sequence,
        record.source_sequence,
        record.request_digest,
    };
    ++impl_->dedupe_count;
    impl_->source_sequence_high_water = record.source_sequence;
    return result;
}

namespace {

inline constexpr std::size_t kCheckpointHeaderBytes = 136U;
inline constexpr std::size_t kCheckpointParentBytes = 32U;
inline constexpr std::size_t kCheckpointChildBytes = 168U;
inline constexpr std::size_t kCheckpointVenueCreditBytes = 44U;
inline constexpr std::size_t kCheckpointClientBytes = 16U;
inline constexpr std::size_t kCheckpointDedupeBytes = 40U;

[[nodiscard]] bool checked_size_add(
    std::size_t& total,
    const std::size_t count,
    const std::size_t width) noexcept {
    if (count != 0U &&
        width > (std::numeric_limits<std::size_t>::max() - total) / count) {
        return false;
    }
    total += count * width;
    return true;
}

[[nodiscard]] std::uint8_t child_flags(
    const dataplane::RecoveredChildReservation& child) noexcept {
    return static_cast<std::uint8_t>(
        (child.reconciled ? 0x01U : 0U) |
        (child.cancel_pending ? 0x02U : 0U));
}

[[nodiscard]] bool valid_checkpoint_ownership(
    const dataplane::RecoveredChildReservation& child) noexcept {
    const bool pending_shape = child.cancel_pending
        ? child.pending_client_request_id != 0U &&
              child.pending_client_sequence != 0U &&
              child.pending_client_session_epoch != 0U &&
              child.pending_cancel_attempt_id != 0U &&
              child.last_dispatch_attempt_id ==
                  child.pending_cancel_attempt_id &&
              child.pending_cancel_attempt_id > child.dispatch_attempt_id
        : child.pending_client_request_id == 0U &&
              child.pending_client_sequence == 0U &&
              child.pending_client_session_epoch == 0U &&
              child.pending_cancel_attempt_id == 0U &&
              child.last_dispatch_attempt_id == child.dispatch_attempt_id;
    if (child.venue_session_id == 0U || child.venue_session_epoch == 0U ||
        child.ownership_generation == 0U ||
        child.wire_identity.size == 0U ||
        child.wire_identity.size > child.wire_identity.bytes.size() ||
        !pending_shape) {
        return false;
    }
    return std::all_of(
        child.wire_identity.bytes.begin() + child.wire_identity.size,
        child.wire_identity.bytes.end(),
        [](const std::byte value) noexcept { return value == std::byte{}; });
}

}  // namespace

std::size_t RecoveryLedger::checkpoint_size_required() const noexcept {
    if (!ready()) {
        return 0U;
    }
    std::size_t total = kCheckpointHeaderBytes + kCheckpointFooterBytes;
    if (!checked_size_add(
            total,
            impl_->active_parents,
            kCheckpointParentBytes) ||
        !checked_size_add(
            total,
            impl_->active_children,
            kCheckpointChildBytes) ||
        !checked_size_add(
            total,
            impl_->venue_credit_count,
            kCheckpointVenueCreditBytes) ||
        !checked_size_add(
            total,
            impl_->client_count,
            kCheckpointClientBytes) ||
        !checked_size_add(
            total,
            impl_->dedupe_count,
            kCheckpointDedupeBytes)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return total;
}

CheckpointEncodeResult RecoveryLedger::encode_checkpoint(
    const std::span<std::byte> destination) const noexcept {
    CheckpointEncodeResult result;
    if (!ready()) {
        return result;
    }
    result.required_size = checkpoint_size_required();
    if (result.required_size == 0U ||
        result.required_size == std::numeric_limits<std::size_t>::max()) {
        result.status = CheckpointEncodeStatus::StateInvalid;
        return result;
    }
    if (destination.size() < result.required_size) {
        result.status = CheckpointEncodeStatus::BufferTooSmall;
        return result;
    }
    Writer writer(destination.first(result.required_size));
    const dataplane::RecoveryIdentifierState identifiers =
        impl_->kernel->recovery_identifiers();
    if (!writer.u32(kCheckpointMagic) ||
        !writer.u16(kRecoveryCheckpointVersion) || !writer.u16(0U) ||
        !writer.u64(impl_->configuration.leadership_epoch) ||
        !writer.u64(impl_->configuration.configuration_generation) ||
        !writer.u64(impl_->configuration.candidate_table_generation) ||
        !writer.u64(impl_->configuration.risk_generation) ||
        !writer.u64(impl_->kernel->state_generation()) ||
        !writer.u64(identifiers.next_child_order_id) ||
        !writer.u64(identifiers.next_dispatch_attempt_id) ||
        !writer.u64(impl_->replayed_through_batch_id) ||
        !writer.u64(impl_->producer_sequence_high_water) ||
        !writer.u64(impl_->source_sequence_high_water) ||
        !writer.bytes(impl_->latest_chain_hash) ||
        impl_->active_parents > std::numeric_limits<std::uint32_t>::max() ||
        impl_->venue_credit_count >
            std::numeric_limits<std::uint32_t>::max() ||
        impl_->client_count > std::numeric_limits<std::uint32_t>::max() ||
        impl_->dedupe_count > std::numeric_limits<std::uint32_t>::max() ||
        !writer.u32(static_cast<std::uint32_t>(impl_->active_parents)) ||
        !writer.u32(static_cast<std::uint32_t>(impl_->venue_credit_count)) ||
        !writer.u32(static_cast<std::uint32_t>(impl_->client_count)) ||
        !writer.u32(static_cast<std::uint32_t>(impl_->dedupe_count))) {
        result.status = CheckpointEncodeStatus::StateInvalid;
        return result;
    }

    std::size_t written_parents{};
    std::size_t written_children{};
    for (std::size_t slot{};
         slot < impl_->configuration.active_parent_capacity;
         ++slot) {
        const Impl::ParentEntry& entry = impl_->parents[slot];
        if (entry.state != Impl::SlotState::Occupied) {
            continue;
        }
        std::uint16_t child_count{};
        for (std::size_t index{}; index < entry.route.child_count; ++index) {
            if (entry.route.children[index].remaining_quantity.units != 0U) {
                ++child_count;
            }
        }
        if (child_count == 0U ||
            !writer.u64(entry.route.parent.id.value()) ||
            !writer.u32(entry.route.parent.instrument.value()) ||
            !writer.u8(static_cast<std::uint8_t>(entry.route.parent.side)) ||
            !writer.u8(static_cast<std::uint8_t>(entry.route.parent.intent)) ||
            !writer.u16(child_count) ||
            !writer.u64(entry.route.parent.quantity.units) ||
            !writer.i64(entry.route.parent.limit_price.units)) {
            result.status = CheckpointEncodeStatus::StateInvalid;
            return result;
        }
        ++written_parents;
        for (std::size_t index{}; index < entry.route.child_count; ++index) {
            const dataplane::RecoveredChildReservation& child =
                entry.route.children[index];
            if (child.remaining_quantity.units == 0U) {
                continue;
            }
            if (!writer.u64(child.child_order_id.value()) ||
                !writer.u64(child.dispatch_attempt_id) ||
                !writer.u32(child.venue.value()) ||
                !writer.u64(child.remaining_quantity.units) ||
                !writer.u64(child.tx_credit_generation) ||
                !writer.u64(child.throttle_generation) ||
                !writer.u32(child.reported_normal_tx_credits) ||
                !writer.u32(child.reported_cancel_tx_credits) ||
                !writer.u32(child.reported_throttle_credits) ||
                !writer.u16(child.normal_tx_credit_delta) ||
                !writer.u16(child.cancel_tx_credit_delta) ||
                !writer.u16(child.throttle_credit_delta) ||
                !writer.u8(child_flags(child)) || !writer.u8(0U) ||
                !valid_checkpoint_ownership(child) ||
                !writer.u64(child.venue_session_id) ||
                !writer.u64(child.venue_session_epoch) ||
                !writer.u64(child.ownership_generation) ||
                !writer.u64(child.last_dispatch_attempt_id) ||
                !writer.u64(child.pending_client_request_id) ||
                !writer.u64(child.pending_client_sequence) ||
                !writer.u64(child.pending_client_session_epoch) ||
                !writer.u64(child.pending_cancel_attempt_id) ||
                !writer.u8(child.wire_identity.size) ||
                !writer.bytes(child.wire_identity.bytes) ||
                !writer.u8(0U) || !writer.u8(0U) || !writer.u8(0U) ||
                !writer.u8(0U) || !writer.u8(0U) || !writer.u8(0U) ||
                !writer.u8(0U)) {
                result.status = CheckpointEncodeStatus::StateInvalid;
                return result;
            }
            ++written_children;
        }
    }
    std::size_t written_credits{};
    for (std::size_t index{}; index < impl_->venue_credit_count; ++index) {
        const dataplane::RecoveryVenueCreditState& credit =
            impl_->venue_credits[index];
        if (!credit.found || !credit.venue ||
            !writer.u32(credit.venue.value()) ||
            !writer.u64(credit.tx_credit_generation) ||
            !writer.u64(credit.throttle_generation) ||
            !writer.u32(credit.reported_normal_tx_credits) ||
            !writer.u32(credit.reported_cancel_tx_credits) ||
            !writer.u32(credit.reported_throttle_credits) ||
            !writer.u32(credit.reserved_normal_tx_credits) ||
            !writer.u32(credit.reserved_cancel_tx_credits) ||
            !writer.u32(credit.reserved_throttle_credits)) {
            result.status = CheckpointEncodeStatus::StateInvalid;
            return result;
        }
        ++written_credits;
    }
    std::size_t written_clients{};
    for (std::size_t slot{}; slot < impl_->configuration.client_capacity;
         ++slot) {
        const Impl::ClientEntry& client = impl_->clients[slot];
        if (!client.occupied) {
            continue;
        }
        if (!writer.u64(client.client_id) ||
            !writer.u64(client.sequence_high_water)) {
            result.status = CheckpointEncodeStatus::StateInvalid;
            return result;
        }
        ++written_clients;
    }
    std::size_t written_dedupe{};
    for (std::size_t slot{};
         slot < impl_->configuration.client_dedupe_capacity;
         ++slot) {
        const Impl::DedupeEntry& entry = impl_->dedupe[slot];
        if (!entry.occupied) {
            continue;
        }
        if (!writer.u64(entry.client_id) ||
            !writer.u64(entry.client_sequence) ||
            !writer.u64(entry.source_sequence) ||
            !writer.bytes(entry.digest)) {
            result.status = CheckpointEncodeStatus::StateInvalid;
            return result;
        }
        ++written_dedupe;
    }
    if (written_parents != impl_->active_parents ||
        written_children != impl_->active_children ||
        written_credits != impl_->venue_credit_count ||
        written_clients != impl_->client_count ||
        written_dedupe != impl_->dedupe_count ||
        writer.position() + kCheckpointFooterBytes != result.required_size) {
        result.status = CheckpointEncodeStatus::StateInvalid;
        return result;
    }

    Sha256 sha;
    sha.update(destination.first(writer.position()));
    result.checkpoint_hash = sha.finish();
    if (!writer.bytes(result.checkpoint_hash)) {
        result.status = CheckpointEncodeStatus::StateInvalid;
        return result;
    }
    result.crc32c = recorder::crc32c(destination.first(writer.position()));
    if (!writer.u32(result.crc32c) || writer.position() != result.required_size) {
        result.status = CheckpointEncodeStatus::StateInvalid;
        return result;
    }
    result.encoded_size = writer.position();
    result.status = CheckpointEncodeStatus::Encoded;
    return result;
}

CheckpointRestoreResult RecoveryLedger::restore_checkpoint(
    const std::span<const std::byte> encoded) noexcept {
    CheckpointRestoreResult result;
    if (!ready()) {
        return result;
    }
    if (impl_->active_parents != 0U || impl_->client_count != 0U ||
        impl_->dedupe_count != 0U || impl_->replay_count != 0U ||
        impl_->checkpoint_restored) {
        result.status = CheckpointRestoreStatus::LedgerNotEmpty;
        return result;
    }
    if (encoded.size() < kCheckpointHeaderBytes + kCheckpointFooterBytes) {
        result.status = CheckpointRestoreStatus::InvalidEncoding;
        return result;
    }
    const std::size_t hash_offset = encoded.size() - kCheckpointFooterBytes;
    const std::size_t crc_offset = encoded.size() - 4U;
    const std::uint32_t stored_crc = get_u32(encoded.data() + crc_offset);
    if (recorder::crc32c(encoded.first(crc_offset)) != stored_crc) {
        result.status = CheckpointRestoreStatus::IntegrityFailure;
        return result;
    }
    Sha256 sha;
    sha.update(encoded.first(hash_offset));
    const recorder::HashChainDigest expected_hash = sha.finish();
    if (!std::equal(
            expected_hash.begin(),
            expected_hash.end(),
            encoded.begin() + hash_offset)) {
        result.status = CheckpointRestoreStatus::IntegrityFailure;
        return result;
    }

    Reader reader(encoded.first(hash_offset));
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint16_t flags{};
    std::uint64_t leadership_epoch{};
    std::uint64_t configuration_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t risk_generation{};
    dataplane::RecoveryKernelState kernel_state{};
    std::uint64_t replayed_through{};
    std::uint64_t producer_high_water{};
    std::uint64_t source_high_water{};
    std::span<const std::byte> chain;
    std::uint32_t parent_count{};
    std::uint32_t venue_credit_count{};
    std::uint32_t client_count{};
    std::uint32_t dedupe_count{};
    if (!reader.u32(magic) || !reader.u16(version) ||
        !reader.u16(flags) || !reader.u64(leadership_epoch) ||
        !reader.u64(configuration_generation) ||
        !reader.u64(candidate_generation) || !reader.u64(risk_generation) ||
        !reader.u64(kernel_state.state_generation) ||
        !reader.u64(kernel_state.identifiers.next_child_order_id) ||
        !reader.u64(kernel_state.identifiers.next_dispatch_attempt_id) ||
        !reader.u64(replayed_through) ||
        !reader.u64(producer_high_water) ||
        !reader.u64(source_high_water) ||
        !reader.bytes(recorder::HashChainDigest{}.size(), chain) ||
        !reader.u32(parent_count) || !reader.u32(venue_credit_count) ||
        !reader.u32(client_count) || !reader.u32(dedupe_count) ||
        magic != kCheckpointMagic ||
        version != kRecoveryCheckpointVersion || flags != 0U) {
        result.status = CheckpointRestoreStatus::InvalidEncoding;
        return result;
    }
    if (leadership_epoch != impl_->configuration.leadership_epoch ||
        configuration_generation !=
            impl_->configuration.configuration_generation) {
        result.status = CheckpointRestoreStatus::ConfigurationMismatch;
        return result;
    }
    if (candidate_generation !=
            impl_->configuration.candidate_table_generation ||
        risk_generation != impl_->configuration.risk_generation) {
        result.status = CheckpointRestoreStatus::GenerationMismatch;
        return result;
    }
    if (parent_count > impl_->configuration.active_parent_capacity ||
        venue_credit_count > impl_->venue_credits.size() ||
        client_count > impl_->configuration.client_capacity ||
        dedupe_count > impl_->configuration.client_dedupe_capacity ||
        kernel_state.state_generation == 0U ||
        kernel_state.identifiers.next_child_order_id == 0U ||
        kernel_state.identifiers.next_dispatch_attempt_id == 0U ||
        (replayed_through == 0U &&
         (producer_high_water != 0U || !all_zero(chain))) ||
        (replayed_through != 0U &&
         (producer_high_water == 0U || all_zero(chain)))) {
        result.status = CheckpointRestoreStatus::CapacityExceeded;
        return result;
    }

    // From this point forward RouteKernel may be mutated. Any structural or
    // semantic failure leaves the ledger permanently not-ready while the
    // kernel remains recovery-gated; only the fully successful path reopens
    // the ledger.
    impl_->ready = false;
    for (std::size_t parent_index{}; parent_index < parent_count;
         ++parent_index) {
        dataplane::RecoveredParentReservation route;
        std::uint64_t parent_id{};
        std::uint32_t instrument{};
        std::uint8_t side{};
        std::uint8_t intent{};
        std::uint64_t parent_quantity{};
        std::int64_t parent_price{};
        if (!reader.u64(parent_id) || !reader.u32(instrument) ||
            !reader.u8(side) || !reader.u8(intent) ||
            !reader.u16(route.child_count) ||
            !reader.u64(parent_quantity) || !reader.i64(parent_price) ||
            parent_id == 0U || instrument == 0U ||
            !valid_side_value(side) || !valid_intent_value(intent) ||
            route.child_count == 0U ||
            route.child_count > route.children.size() ||
            parent_quantity == 0U || parent_price <= 0) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        route.parent = ParentOrder{
            ParentOrderId{parent_id},
            InstrumentId{instrument},
            static_cast<Side>(side),
            Quantity{parent_quantity},
            Price{parent_price},
            static_cast<RoutingIntent>(intent),
        };
        route.candidate_table_generation = candidate_generation;
        route.risk_generation = risk_generation;
        const dataplane::InstrumentRiskCounters instrument_before =
            impl_->kernel->instrument_counters(route.parent.instrument);
        if (!instrument_before.found) {
            result.status = CheckpointRestoreStatus::RouteKernelRejected;
            impl_->ready = false;
            return result;
        }
        route.instrument_outstanding_before =
            instrument_before.gross_outstanding;
        route.instrument_open_parents_before = instrument_before.open_parents;
        route.instrument_open_children_before = instrument_before.open_children;
        Quantity added{};
        for (std::size_t child_index{}; child_index < route.child_count;
             ++child_index) {
            dataplane::RecoveredChildReservation& child =
                route.children[child_index];
            std::uint64_t child_id{};
            std::uint32_t venue{};
            std::uint64_t quantity{};
            std::uint8_t child_state{};
            std::uint8_t reserved{};
            std::uint8_t wire_size{};
            std::span<const std::byte> wire;
            std::array<std::uint8_t, 7U> ownership_reserved{};
            if (!reader.u64(child_id) ||
                !reader.u64(child.dispatch_attempt_id) ||
                !reader.u32(venue) || !reader.u64(quantity) ||
                !reader.u64(child.tx_credit_generation) ||
                !reader.u64(child.throttle_generation) ||
                !reader.u32(child.reported_normal_tx_credits) ||
                !reader.u32(child.reported_cancel_tx_credits) ||
                !reader.u32(child.reported_throttle_credits) ||
                !reader.u16(child.normal_tx_credit_delta) ||
                !reader.u16(child.cancel_tx_credit_delta) ||
                !reader.u16(child.throttle_credit_delta) ||
                !reader.u8(child_state) || !reader.u8(reserved) ||
                !reader.u64(child.venue_session_id) ||
                !reader.u64(child.venue_session_epoch) ||
                !reader.u64(child.ownership_generation) ||
                !reader.u64(child.last_dispatch_attempt_id) ||
                !reader.u64(child.pending_client_request_id) ||
                !reader.u64(child.pending_client_sequence) ||
                !reader.u64(child.pending_client_session_epoch) ||
                !reader.u64(child.pending_cancel_attempt_id) ||
                !reader.u8(wire_size) ||
                !reader.bytes(dataplane::kInlineWireIdentityBytes, wire) ||
                !reader.u8(ownership_reserved[0]) ||
                !reader.u8(ownership_reserved[1]) ||
                !reader.u8(ownership_reserved[2]) ||
                !reader.u8(ownership_reserved[3]) ||
                !reader.u8(ownership_reserved[4]) ||
                !reader.u8(ownership_reserved[5]) ||
                !reader.u8(ownership_reserved[6]) ||
                child_id == 0U || child.dispatch_attempt_id == 0U ||
                venue == 0U || quantity == 0U || child_state > 3U ||
                reserved != 0U || wire_size == 0U ||
                wire_size > dataplane::kInlineWireIdentityBytes ||
                std::any_of(
                    ownership_reserved.begin(),
                    ownership_reserved.end(),
                    [](const std::uint8_t value) noexcept {
                        return value != 0U;
                    }) ||
                child_id >=
                    kernel_state.identifiers.next_child_order_id ||
                child.last_dispatch_attempt_id >=
                    kernel_state.identifiers.next_dispatch_attempt_id) {
                result.status = CheckpointRestoreStatus::InvalidEncoding;
                return result;
            }
            child.child_order_id = ChildOrderId{child_id};
            child.venue = VenueId{venue};
            child.remaining_quantity = Quantity{quantity};
            child.reconciled = (child_state & 0x01U) != 0U;
            child.cancel_pending = (child_state & 0x02U) != 0U;
            child.wire_identity.size = wire_size;
            std::copy(
                wire.begin(), wire.end(), child.wire_identity.bytes.begin());
            if (!valid_checkpoint_ownership(child)) {
                result.status = CheckpointRestoreStatus::InvalidEncoding;
                return result;
            }
            for (std::size_t prior{}; prior < child_index; ++prior) {
                if (route.children[prior].child_order_id ==
                        child.child_order_id ||
                    route.children[prior].dispatch_attempt_id ==
                        child.dispatch_attempt_id ||
                    route.children[prior].venue == child.venue) {
                    result.status = CheckpointRestoreStatus::InvalidEncoding;
                    return result;
                }
            }
            const dataplane::VenueRiskCounters venue_before =
                impl_->kernel->venue_counters(child.venue);
            const auto venue_after = checked_add(
                venue_before.outstanding, child.remaining_quantity);
            const auto next_added = checked_add(added, child.remaining_quantity);
            if (!venue_before.found || !venue_after || !next_added) {
                result.status = CheckpointRestoreStatus::RouteKernelRejected;
                impl_->ready = false;
                return result;
            }
            route.venue_outstanding_before[child_index] =
                venue_before.outstanding;
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
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        route.instrument_outstanding_after = *instrument_after;
        route.instrument_open_parents_after =
            route.instrument_open_parents_before + 1U;
        route.instrument_open_children_after =
            route.instrument_open_children_before + route.child_count;
        bool duplicate{};
        const std::size_t slot = impl_->parent_insertion_slot(
            route.parent.id, duplicate);
        if (duplicate ||
            slot == impl_->configuration.active_parent_capacity) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        const dataplane::RecoveryImportStatus imported =
            impl_->kernel->restore_recovered_parent(route);
        if (imported != dataplane::RecoveryImportStatus::Imported) {
            result.status = CheckpointRestoreStatus::RouteKernelRejected;
            impl_->ready = false;
            return result;
        }
        impl_->parents[slot].state = Impl::SlotState::Occupied;
        impl_->parents[slot].route = route;
        ++impl_->active_parents;
        impl_->active_children += route.child_count;
    }

    for (std::size_t index{}; index < venue_credit_count; ++index) {
        dataplane::RecoveryVenueCreditState credit;
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
            credit.tx_credit_generation == 0U ||
            credit.throttle_generation == 0U ||
            credit.reserved_normal_tx_credits >
                credit.reported_normal_tx_credits ||
            credit.reserved_cancel_tx_credits >
                credit.reported_cancel_tx_credits ||
            credit.reserved_throttle_credits >
                credit.reported_throttle_credits) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        credit.venue = VenueId{venue};
        credit.found = true;
        for (std::size_t prior{}; prior < impl_->venue_credit_count; ++prior) {
            if (impl_->venue_credits[prior].venue == credit.venue) {
                result.status = CheckpointRestoreStatus::InvalidEncoding;
                return result;
            }
        }
        if (!impl_->kernel->restore_recovery_venue_credit_state(credit)) {
            result.status = CheckpointRestoreStatus::RouteKernelRejected;
            impl_->ready = false;
            return result;
        }
        impl_->venue_credits[impl_->venue_credit_count++] = credit;
    }

    for (std::size_t index{}; index < client_count; ++index) {
        std::uint64_t client_id{};
        std::uint64_t high_water{};
        if (!reader.u64(client_id) || !reader.u64(high_water) ||
            client_id == 0U || high_water == 0U) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        bool duplicate{};
        const std::size_t slot =
            impl_->client_insertion_slot(client_id, duplicate);
        if (duplicate || slot == impl_->configuration.client_capacity) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        impl_->clients[slot] = Impl::ClientEntry{true, client_id, high_water};
        ++impl_->client_count;
    }
    for (std::size_t index{}; index < dedupe_count; ++index) {
        Impl::DedupeEntry entry;
        std::span<const std::byte> digest;
        if (!reader.u64(entry.client_id) ||
            !reader.u64(entry.client_sequence) ||
            !reader.u64(entry.source_sequence) ||
            !reader.bytes(entry.digest.size(), digest) ||
            entry.client_id == 0U || entry.client_sequence == 0U ||
            entry.source_sequence == 0U ||
            entry.source_sequence > source_high_water || all_zero(digest)) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        std::copy(digest.begin(), digest.end(), entry.digest.begin());
        const std::size_t client_slot = impl_->client_slot(entry.client_id);
        bool duplicate{};
        const std::size_t slot = impl_->dedupe_insertion_slot(
            entry.client_id, entry.client_sequence, duplicate);
        if (client_slot == impl_->configuration.client_capacity || duplicate ||
            slot == impl_->configuration.client_dedupe_capacity ||
            entry.client_sequence >
                impl_->clients[client_slot].sequence_high_water) {
            result.status = CheckpointRestoreStatus::InvalidEncoding;
            return result;
        }
        entry.occupied = true;
        impl_->dedupe[slot] = entry;
        ++impl_->dedupe_count;
    }
    if (!reader.done() ||
        !impl_->kernel->restore_recovery_kernel_state(kernel_state)) {
        result.status = CheckpointRestoreStatus::RouteKernelRejected;
        impl_->ready = false;
        return result;
    }
    impl_->replayed_through_batch_id = replayed_through;
    impl_->producer_sequence_high_water = producer_high_water;
    impl_->source_sequence_high_water = source_high_water;
    std::copy(chain.begin(), chain.end(), impl_->latest_chain_hash.begin());
    impl_->checkpoint_restored = true;
    impl_->ready = true;
    result.replay_through_batch_id = replayed_through;
    result.restored_parents = impl_->active_parents;
    result.restored_clients = impl_->client_count;
    result.restored_dedupe_entries = impl_->dedupe_count;
    result.status = CheckpointRestoreStatus::Restored;
    return result;
}

bool RecoveryLedger::complete_reconciliation() noexcept {
    return ready() && impl_->kernel->complete_recovery();
}

RecoveryMetadata RecoveryLedger::metadata() const noexcept {
    RecoveryMetadata output;
    if (!impl_) {
        return output;
    }
    output.ready = impl_->ready;
    output.checkpoint_restored = impl_->checkpoint_restored;
    output.recovery_only =
        impl_->kernel != nullptr && impl_->kernel->recovery_only();
    output.replayed_through_batch_id = impl_->replayed_through_batch_id;
    output.producer_sequence_high_water =
        impl_->producer_sequence_high_water;
    output.recovery_source_sequence_high_water =
        impl_->source_sequence_high_water;
    output.latest_chain_hash = impl_->latest_chain_hash;
    output.active_parents = impl_->active_parents;
    output.active_children = impl_->active_children;
    output.unreconciled_children = impl_->kernel == nullptr ? 0U :
        impl_->kernel->unreconciled_child_count();
    output.clients = impl_->client_count;
    output.client_dedupe_entries = impl_->dedupe_count;
    if (impl_->kernel != nullptr) {
        output.identifiers = impl_->kernel->recovery_identifiers();
    }
    return output;
}

std::uint64_t RecoveryLedger::client_sequence_high_water(
    const std::uint64_t client_id) const noexcept {
    if (!impl_ || client_id == 0U) {
        return 0U;
    }
    const std::size_t slot = impl_->client_slot(client_id);
    return slot == impl_->configuration.client_capacity ? 0U :
        impl_->clients[slot].sequence_high_water;
}

}  // namespace sorx::recovery
