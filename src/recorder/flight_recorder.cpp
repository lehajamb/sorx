#include "sorx/recorder/flight_recorder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sorx::recorder {
namespace {

constexpr std::size_t kRecordHeaderBytes = 8U;

[[nodiscard]] constexpr bool valid_lane(const DurabilityLane lane) noexcept {
    return lane == DurabilityLane::Normal || lane == DurabilityLane::Cancel ||
           lane == DurabilityLane::Reconciliation;
}

[[nodiscard]] constexpr bool valid_authorization_envelope_version(
    const AuthorizationEnvelopeVersion version) noexcept {
    return version == AuthorizationEnvelopeVersion::LegacyRangeV0 ||
           version == AuthorizationEnvelopeVersion::ExactEffectsV1;
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
    for (std::size_t index = 0U; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        destination[index] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* const source) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[0]))
         << 8U) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[1])));
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* const source) noexcept {
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[0]))
            << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[1]))
            << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[2]))
            << 8U) |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(source[3]));
}

class Crc32cAccumulator final {
public:
    void update(const std::span<const std::byte> bytes) noexcept {
        for (const std::byte byte : bytes) {
            value_ ^= std::to_integer<std::uint8_t>(byte);
            for (unsigned bit = 0U; bit < 8U; ++bit) {
                const std::uint32_t mask =
                    0U - static_cast<std::uint32_t>(value_ & 1U);
                value_ = (value_ >> 1U) ^ (0x82f63b78U & mask);
            }
        }
    }

    [[nodiscard]] std::uint32_t finish() const noexcept {
        return value_ ^ 0xffffffffU;
    }

private:
    std::uint32_t value_{0xffffffffU};
};

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

    [[nodiscard]] HashChainDigest finish() noexcept {
        const std::uint64_t bit_length =
            static_cast<std::uint64_t>(total_bytes_) * 8U;
        block_[block_size_++] = std::byte{0x80U};
        if (block_size_ > 56U) {
            std::fill(block_.begin() + block_size_, block_.end(), std::byte{});
            transform(block_);
            block_size_ = 0U;
        }
        std::fill(block_.begin() + block_size_, block_.begin() + 56U,
                  std::byte{});
        put_u64(block_.data() + 56U, bit_length);
        transform(block_);

        HashChainDigest result{};
        for (std::size_t index = 0U; index < state_.size(); ++index) {
            put_u32(result.data() + (index * 4U), state_[index]);
        }
        return result;
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
        for (std::size_t index = 0U; index < 16U; ++index) {
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
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0U; index < words.size(); ++index) {
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
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::byte, 64U> block_{};
    std::size_t total_bytes_{};
    std::size_t block_size_{};
};

template <typename Accumulator>
void update_u16(Accumulator& accumulator, const std::uint16_t value) noexcept {
    std::array<std::byte, 2U> bytes{};
    put_u16(bytes.data(), value);
    accumulator.update(bytes);
}

template <typename Accumulator>
void update_u32(Accumulator& accumulator, const std::uint32_t value) noexcept {
    std::array<std::byte, 4U> bytes{};
    put_u32(bytes.data(), value);
    accumulator.update(bytes);
}

template <typename Accumulator>
void update_u64(Accumulator& accumulator, const std::uint64_t value) noexcept {
    std::array<std::byte, 8U> bytes{};
    put_u64(bytes.data(), value);
    accumulator.update(bytes);
}

template <typename Accumulator>
void update_lane(Accumulator& accumulator, const DurabilityLane lane) noexcept {
    const std::array<std::byte, 1U> bytes{
        static_cast<std::byte>(static_cast<std::uint8_t>(lane))};
    accumulator.update(bytes);
}

template <typename Accumulator>
void update_dispatch_authorization(
    Accumulator& accumulator,
    const DispatchAuthorization authorization) noexcept {
    update_u64(accumulator, authorization.first_child_id);
    update_u64(accumulator, authorization.first_attempt_id);
    update_u32(accumulator, authorization.dispatch_count);
}

template <typename Accumulator>
void update_external_effect_authorizations(
    Accumulator& accumulator,
    const ExternalEffectAuthorizationList& authorizations) noexcept {
    update_u16(accumulator, authorizations.count);
    for (const ExternalEffectAuthorization& authorization :
         authorizations.values()) {
        update_u32(accumulator, authorization.venue_id);
        update_u64(accumulator, authorization.child_id);
        update_u64(accumulator, authorization.dispatch_attempt_id);
        update_u64(accumulator, authorization.venue_session_id);
        update_u64(accumulator, authorization.venue_session_epoch);
        update_u64(accumulator, authorization.ownership_generation);
        const std::array<std::byte, 1U> dispatch_class{
            static_cast<std::byte>(authorization.dispatch_class)};
        accumulator.update(dispatch_class);
        update_u16(accumulator, authorization.frame_size);
        accumulator.update(authorization.frame_hash);
    }
}

template <typename Accumulator>
void update_authorization_envelope(
    Accumulator& accumulator,
    const AuthorizationEnvelopeVersion version,
    const ExternalEffectAuthorizationList& authorizations) noexcept {
    if (version == AuthorizationEnvelopeVersion::LegacyRangeV0) {
        return;
    }
    const std::array<std::byte, 1U> version_byte{
        static_cast<std::byte>(static_cast<std::uint8_t>(version))};
    accumulator.update(version_byte);
    update_external_effect_authorizations(accumulator, authorizations);
}

[[nodiscard]] HashChainDigest frame_content_hash(
    const std::span<const std::byte> frame) noexcept {
    Sha256 sha;
    sha.update(frame);
    return sha.finish();
}

[[nodiscard]] std::uint32_t batch_crc(
    const std::uint64_t batch_id,
    const std::uint64_t producer_sequence,
    const DurabilityLane lane,
    const std::uint64_t leadership_epoch,
    const std::uint64_t configuration_generation,
    const DispatchAuthorization dispatch_authorization,
    const AuthorizationEnvelopeVersion authorization_envelope_version,
    const ExternalEffectAuthorizationList& external_effect_authorizations,
    const std::uint16_t record_count,
    const std::span<const std::byte> encoded) noexcept {
    Crc32cAccumulator accumulator;
    update_u64(accumulator, batch_id);
    update_u64(accumulator, producer_sequence);
    update_lane(accumulator, lane);
    update_u64(accumulator, leadership_epoch);
    update_u64(accumulator, configuration_generation);
    update_dispatch_authorization(accumulator, dispatch_authorization);
    update_authorization_envelope(
        accumulator,
        authorization_envelope_version,
        external_effect_authorizations);
    update_u16(accumulator, record_count);
    update_u32(accumulator, static_cast<std::uint32_t>(encoded.size()));
    accumulator.update(encoded);
    return accumulator.finish();
}

[[nodiscard]] HashChainDigest batch_content_hash(
    const std::uint64_t batch_id,
    const std::uint64_t producer_sequence,
    const DurabilityLane lane,
    const std::uint64_t leadership_epoch,
    const std::uint64_t configuration_generation,
    const DispatchAuthorization dispatch_authorization,
    const AuthorizationEnvelopeVersion authorization_envelope_version,
    const ExternalEffectAuthorizationList& external_effect_authorizations,
    const std::uint16_t record_count,
    const std::uint32_t checksum,
    const std::span<const std::byte> encoded) noexcept {
    Sha256 sha;
    update_u64(sha, batch_id);
    update_u64(sha, producer_sequence);
    update_lane(sha, lane);
    update_u64(sha, leadership_epoch);
    update_u64(sha, configuration_generation);
    update_dispatch_authorization(sha, dispatch_authorization);
    update_authorization_envelope(
        sha,
        authorization_envelope_version,
        external_effect_authorizations);
    update_u16(sha, record_count);
    update_u32(sha, static_cast<std::uint32_t>(encoded.size()));
    update_u32(sha, checksum);
    sha.update(encoded);
    return sha.finish();
}

[[nodiscard]] HashChainDigest batch_chain_hash(
    const HashChainDigest& previous,
    const HashChainDigest& batch_hash) noexcept {
    Sha256 sha;
    sha.update(previous);
    sha.update(batch_hash);
    return sha.finish();
}

[[nodiscard]] bool token_matches_batch_impl(
    const DurableToken& token,
    const DurableBatch& batch) noexcept {
    return token && token.producer_sequence == batch.producer_sequence() &&
           token.leadership_epoch == batch.leadership_epoch() &&
           token.configuration_generation ==
               batch.configuration_generation() &&
           token.dispatch_authorization == batch.dispatch_authorization() &&
           token.authorization_envelope_version ==
               batch.authorization_envelope_version() &&
           token.external_effect_authorizations ==
               batch.external_effect_authorizations() &&
           token.lane == batch.lane() &&
           token.payload_size == batch.encoded_size() && batch.valid() &&
           token.payload_crc32c ==
               batch_crc(token.batch_id, batch.producer_sequence(), batch.lane(),
                         batch.leadership_epoch(),
                         batch.configuration_generation(),
                         batch.dispatch_authorization(),
                         batch.authorization_envelope_version(),
                         batch.external_effect_authorizations(),
                         static_cast<std::uint16_t>(batch.record_count()),
                         batch.encoded_bytes()) &&
           token.batch_hash ==
               batch_content_hash(
                   token.batch_id, batch.producer_sequence(), batch.lane(),
                   batch.leadership_epoch(),
                   batch.configuration_generation(),
                   batch.dispatch_authorization(),
                   batch.authorization_envelope_version(),
                   batch.external_effect_authorizations(),
                   static_cast<std::uint16_t>(batch.record_count()),
                   token.payload_crc32c, batch.encoded_bytes()) &&
           token.chain_hash ==
               batch_chain_hash(token.previous_hash, token.batch_hash);
}

}  // namespace

bool DispatchAuthorization::valid() const noexcept {
    if (first_child_id == 0U || first_attempt_id == 0U ||
        dispatch_count == 0U) {
        return false;
    }
    const std::uint64_t maximum_offset = dispatch_count - 1U;
    return maximum_offset <=
               std::numeric_limits<std::uint64_t>::max() - first_child_id &&
           maximum_offset <=
               std::numeric_limits<std::uint64_t>::max() - first_attempt_id;
}

bool DispatchAuthorization::authorizes(
    const std::uint64_t candidate_child_id,
    const std::uint64_t candidate_attempt_id) const noexcept {
    if (!valid() || candidate_child_id < first_child_id) {
        return false;
    }
    if (candidate_attempt_id < first_attempt_id) {
        return false;
    }
    const std::uint64_t child_offset = candidate_child_id - first_child_id;
    const std::uint64_t attempt_offset =
        candidate_attempt_id - first_attempt_id;
    return child_offset == attempt_offset && child_offset < dispatch_count;
}

bool ExternalEffectAuthorization::valid() const noexcept {
    return venue_id != 0U && child_id != 0U && dispatch_attempt_id != 0U &&
           venue_session_id != 0U && venue_session_epoch != 0U &&
           ownership_generation != 0U && dispatch_class <= 1U &&
           frame_size != 0U && frame_size <= kMaximumAuthorizedFrameBytes;
}

bool ExternalEffectAuthorizationList::valid(
    const DispatchAuthorization dispatch_authorization) const noexcept {
    if (!dispatch_authorization.valid() || count == 0U ||
        count > entries.size() ||
        count != dispatch_authorization.dispatch_count) {
        return false;
    }
    for (std::size_t index{}; index < count; ++index) {
        const ExternalEffectAuthorization& authorization = entries[index];
        if (!authorization.valid() ||
            authorization.child_id !=
                dispatch_authorization.first_child_id + index ||
            authorization.dispatch_attempt_id !=
                dispatch_authorization.first_attempt_id + index) {
            return false;
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (entries[prior].venue_id == authorization.venue_id &&
                entries[prior].child_id == authorization.child_id &&
                entries[prior].dispatch_attempt_id ==
                    authorization.dispatch_attempt_id) {
                return false;
            }
        }
    }
    return true;
}

DurableEffectPermit::operator bool() const noexcept {
    return batch_id != 0U && producer_sequence != 0U &&
           leadership_epoch != 0U && configuration_generation != 0U &&
           effect_ordinal < kMaximumExternalEffectAuthorizations &&
           valid_lane(lane) && authorization.valid();
}

bool DurableEffectPermit::authorizes_external_effect(
    const std::uint32_t candidate_venue_id,
    const std::uint64_t candidate_child_id,
    const std::uint64_t candidate_attempt_id,
    const std::uint64_t candidate_venue_session_id,
    const std::uint64_t candidate_venue_session_epoch,
    const std::uint64_t candidate_ownership_generation,
    const std::uint8_t candidate_dispatch_class,
    const std::span<const std::byte> candidate_frame) const noexcept {
    return static_cast<bool>(*this) &&
           candidate_venue_id == authorization.venue_id &&
           candidate_child_id == authorization.child_id &&
           candidate_attempt_id == authorization.dispatch_attempt_id &&
           candidate_venue_session_id == authorization.venue_session_id &&
           candidate_venue_session_epoch ==
               authorization.venue_session_epoch &&
           candidate_ownership_generation ==
               authorization.ownership_generation &&
           candidate_dispatch_class == authorization.dispatch_class &&
           candidate_frame.size() == authorization.frame_size &&
           frame_content_hash(candidate_frame) == authorization.frame_hash;
}

DurableBatch::DurableBatch(
    const DurabilityLane lane,
    const std::uint64_t producer_sequence) noexcept
    : lane_(lane), producer_sequence_(producer_sequence) {}

bool DurableBatch::try_append_record(
    const std::uint16_t type,
    const std::uint16_t version,
    const std::span<const std::byte> payload) noexcept {
    if (type == 0U || record_count_ >= kMaximumDurableRecordsPerBatch ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const std::size_t required = kRecordHeaderBytes + payload.size();
    if (required > (encoded_.size() - encoded_size_)) {
        return false;
    }

    const std::size_t offset = encoded_size_;
    record_offsets_[record_count_] = static_cast<std::uint32_t>(offset);
    put_u16(encoded_.data() + offset, type);
    put_u16(encoded_.data() + offset + 2U, version);
    put_u32(encoded_.data() + offset + 4U,
            static_cast<std::uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(),
              encoded_.begin() + offset + kRecordHeaderBytes);
    encoded_size_ += static_cast<std::uint32_t>(required);
    ++record_count_;
    return true;
}

void DurableBatch::clear() noexcept {
    producer_sequence_ = 0U;
    leadership_epoch_ = 0U;
    configuration_generation_ = 0U;
    dispatch_authorization_ = {};
    authorization_envelope_version_ =
        AuthorizationEnvelopeVersion::ExactEffectsV1;
    external_effect_authorizations_ = {};
    record_count_ = 0U;
    encoded_size_ = 0U;
    record_offsets_.fill(0U);
}

DurabilityLane DurableBatch::lane() const noexcept { return lane_; }

std::uint64_t DurableBatch::producer_sequence() const noexcept {
    return producer_sequence_;
}

void DurableBatch::set_producer_sequence(const std::uint64_t value) noexcept {
    producer_sequence_ = value;
}

std::uint64_t DurableBatch::leadership_epoch() const noexcept {
    return leadership_epoch_;
}

void DurableBatch::set_leadership_epoch(const std::uint64_t value) noexcept {
    leadership_epoch_ = value;
}

std::uint64_t DurableBatch::configuration_generation() const noexcept {
    return configuration_generation_;
}

void DurableBatch::set_configuration_generation(
    const std::uint64_t value) noexcept {
    configuration_generation_ = value;
}

bool DurableBatch::set_authorization_envelope_version(
    const AuthorizationEnvelopeVersion version) noexcept {
    if (!valid_authorization_envelope_version(version) ||
        external_effect_authorizations_.count != 0U || record_count_ != 0U) {
        return false;
    }
    authorization_envelope_version_ = version;
    return true;
}

AuthorizationEnvelopeVersion
DurableBatch::authorization_envelope_version() const noexcept {
    return authorization_envelope_version_;
}

DispatchAuthorization DurableBatch::dispatch_authorization() const noexcept {
    return dispatch_authorization_;
}

bool DurableBatch::set_dispatch_authorization(
    const DispatchAuthorization authorization) noexcept {
    if (!authorization.valid() || external_effect_authorizations_.count != 0U) {
        return false;
    }
    dispatch_authorization_ = authorization;
    return true;
}

bool DurableBatch::try_add_external_effect_authorization(
    const std::uint32_t venue_id,
    const std::uint64_t child_id,
    const std::uint64_t dispatch_attempt_id,
    const std::uint64_t venue_session_id,
    const std::uint64_t venue_session_epoch,
    const std::uint64_t ownership_generation,
    const std::uint8_t dispatch_class,
    const std::span<const std::byte> frame) noexcept {
    const std::size_t index = external_effect_authorizations_.count;
    if (authorization_envelope_version_ !=
            AuthorizationEnvelopeVersion::ExactEffectsV1 ||
        !dispatch_authorization_.valid() ||
        index >= external_effect_authorizations_.entries.size() ||
        index >= dispatch_authorization_.dispatch_count || frame.empty() ||
        frame.size() > kMaximumAuthorizedFrameBytes ||
        index > std::numeric_limits<std::uint64_t>::max() -
                    dispatch_authorization_.first_child_id ||
        index > std::numeric_limits<std::uint64_t>::max() -
                    dispatch_authorization_.first_attempt_id ||
        child_id != dispatch_authorization_.first_child_id + index ||
        dispatch_attempt_id !=
            dispatch_authorization_.first_attempt_id + index) {
        return false;
    }
    ExternalEffectAuthorization authorization{
        venue_id,
        child_id,
        dispatch_attempt_id,
        venue_session_id,
        venue_session_epoch,
        ownership_generation,
        dispatch_class,
        static_cast<std::uint16_t>(frame.size()),
        frame_content_hash(frame),
    };
    return try_add_external_effect_authorization(authorization);
}

bool DurableBatch::try_add_external_effect_authorization(
    const ExternalEffectAuthorization& authorization) noexcept {
    const std::size_t index = external_effect_authorizations_.count;
    if (!authorization.valid()) {
        return false;
    }
    if (authorization_envelope_version_ !=
            AuthorizationEnvelopeVersion::ExactEffectsV1 ||
        !dispatch_authorization_.valid() ||
        index >= external_effect_authorizations_.entries.size() ||
        index >= dispatch_authorization_.dispatch_count ||
        index > std::numeric_limits<std::uint64_t>::max() -
                    dispatch_authorization_.first_child_id ||
        index > std::numeric_limits<std::uint64_t>::max() -
                    dispatch_authorization_.first_attempt_id ||
        authorization.child_id !=
            dispatch_authorization_.first_child_id + index ||
        authorization.dispatch_attempt_id !=
            dispatch_authorization_.first_attempt_id + index) {
        return false;
    }
    for (const ExternalEffectAuthorization& existing :
         external_effect_authorizations_.values()) {
        if (existing.venue_id == authorization.venue_id &&
            existing.child_id == authorization.child_id &&
            existing.dispatch_attempt_id ==
                authorization.dispatch_attempt_id) {
            return false;
        }
    }
    external_effect_authorizations_.entries[index] = authorization;
    ++external_effect_authorizations_.count;
    return true;
}

const ExternalEffectAuthorizationList&
DurableBatch::external_effect_authorizations() const noexcept {
    return external_effect_authorizations_;
}

std::size_t DurableBatch::record_count() const noexcept {
    return record_count_;
}

std::size_t DurableBatch::encoded_size() const noexcept {
    return encoded_size_;
}

std::span<const std::byte> DurableBatch::encoded_bytes() const noexcept {
    return {encoded_.data(), encoded_size_};
}

std::optional<DurableRecordView> DurableBatch::record(
    const std::size_t index) const noexcept {
    if (index >= record_count_) {
        return std::nullopt;
    }
    const std::size_t offset = record_offsets_[index];
    if (offset > encoded_size_ ||
        kRecordHeaderBytes > (encoded_size_ - offset)) {
        return std::nullopt;
    }
    const std::uint32_t size = get_u32(encoded_.data() + offset + 4U);
    if (size > encoded_size_ - offset - kRecordHeaderBytes) {
        return std::nullopt;
    }
    return DurableRecordView{
        get_u16(encoded_.data() + offset),
        get_u16(encoded_.data() + offset + 2U),
        std::span<const std::byte>{
            encoded_.data() + offset + kRecordHeaderBytes, size}};
}

bool DurableBatch::valid() const noexcept {
    if (!valid_lane(lane_) || producer_sequence_ == 0U ||
        leadership_epoch_ == 0U || configuration_generation_ == 0U ||
        !dispatch_authorization_.valid() ||
        !valid_authorization_envelope_version(
            authorization_envelope_version_) ||
        (authorization_envelope_version_ ==
                 AuthorizationEnvelopeVersion::LegacyRangeV0
             ? external_effect_authorizations_.count != 0U
             : !external_effect_authorizations_.valid(
                   dispatch_authorization_)) ||
        record_count_ == 0U ||
        record_count_ > kMaximumDurableRecordsPerBatch ||
        encoded_size_ > encoded_.size()) {
        return false;
    }
    std::size_t offset = 0U;
    for (std::size_t index = 0U; index < record_count_; ++index) {
        if (record_offsets_[index] != offset ||
            kRecordHeaderBytes > encoded_size_ - offset) {
            return false;
        }
        if (get_u16(encoded_.data() + offset) == 0U) {
            return false;
        }
        const std::uint32_t size = get_u32(encoded_.data() + offset + 4U);
        if (size > encoded_size_ - offset - kRecordHeaderBytes) {
            return false;
        }
        offset += kRecordHeaderBytes + size;
    }
    return offset == encoded_size_;
}

bool DurableBatch::assign_encoded(
    const DurabilityLane lane,
    const std::uint64_t producer_sequence,
    const std::uint64_t leadership_epoch,
    const std::uint64_t configuration_generation,
    const DispatchAuthorization dispatch_authorization,
    const AuthorizationEnvelopeVersion authorization_envelope_version,
    const ExternalEffectAuthorizationList external_effect_authorizations,
    const std::uint16_t record_count,
    const std::span<const std::byte> bytes) noexcept {
    if (!valid_lane(lane) || producer_sequence == 0U ||
        leadership_epoch == 0U || configuration_generation == 0U ||
        !dispatch_authorization.valid() ||
        !valid_authorization_envelope_version(
            authorization_envelope_version) ||
        (authorization_envelope_version ==
                 AuthorizationEnvelopeVersion::LegacyRangeV0
             ? external_effect_authorizations.count != 0U
             : !external_effect_authorizations.valid(
                   dispatch_authorization)) ||
        record_count == 0U ||
        record_count > kMaximumDurableRecordsPerBatch ||
        bytes.size() > encoded_.size()) {
        return false;
    }

    lane_ = lane;
    producer_sequence_ = producer_sequence;
    leadership_epoch_ = leadership_epoch;
    configuration_generation_ = configuration_generation;
    dispatch_authorization_ = dispatch_authorization;
    authorization_envelope_version_ = authorization_envelope_version;
    external_effect_authorizations_ = external_effect_authorizations;
    record_count_ = record_count;
    encoded_size_ = static_cast<std::uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), encoded_.begin());

    std::size_t offset = 0U;
    for (std::size_t index = 0U; index < record_count_; ++index) {
        record_offsets_[index] = static_cast<std::uint32_t>(offset);
        if (offset > encoded_size_ ||
            kRecordHeaderBytes > encoded_size_ - offset) {
            return false;
        }
        const std::uint32_t size = get_u32(encoded_.data() + offset + 4U);
        if (size > encoded_size_ - offset - kRecordHeaderBytes) {
            return false;
        }
        offset += kRecordHeaderBytes + size;
    }
    return valid();
}

bool DurableToken::authorizes_dispatch(
    const std::uint64_t candidate_child_id,
    const std::uint64_t candidate_attempt_id,
    const std::uint64_t candidate_leadership_epoch,
    const std::uint64_t candidate_configuration_generation) const noexcept {
    return static_cast<bool>(*this) &&
           candidate_leadership_epoch == leadership_epoch &&
           candidate_configuration_generation == configuration_generation &&
           dispatch_authorization.authorizes(candidate_child_id,
                                              candidate_attempt_id);
}

bool DurableToken::authorizes_external_effect(
    const std::uint32_t candidate_venue_id,
    const std::uint64_t candidate_child_id,
    const std::uint64_t candidate_attempt_id,
    const std::uint64_t candidate_venue_session_id,
    const std::uint64_t candidate_venue_session_epoch,
    const std::uint64_t candidate_ownership_generation,
    const std::uint8_t candidate_dispatch_class,
    const std::span<const std::byte> candidate_frame) const noexcept {
    if (!static_cast<bool>(*this) ||
        authorization_envelope_version !=
            AuthorizationEnvelopeVersion::ExactEffectsV1) {
        return false;
    }
    for (const ExternalEffectAuthorization& authorization :
         external_effect_authorizations.values()) {
        if (authorization.venue_id == candidate_venue_id &&
            authorization.child_id == candidate_child_id &&
            authorization.dispatch_attempt_id == candidate_attempt_id &&
            authorization.venue_session_id == candidate_venue_session_id &&
            authorization.venue_session_epoch ==
                candidate_venue_session_epoch &&
            authorization.ownership_generation ==
                candidate_ownership_generation &&
            authorization.dispatch_class == candidate_dispatch_class &&
            authorization.frame_size == candidate_frame.size() &&
            authorization.frame_hash == frame_content_hash(candidate_frame)) {
            return true;
        }
    }
    return false;
}

std::optional<DurableEffectPermit> DurableToken::effect_permit(
    const std::size_t ordinal) const noexcept {
    if (!static_cast<bool>(*this) ||
        authorization_envelope_version !=
            AuthorizationEnvelopeVersion::ExactEffectsV1 ||
        ordinal >= external_effect_authorizations.count) {
        return std::nullopt;
    }
    return DurableEffectPermit{
        batch_id,
        producer_sequence,
        leadership_epoch,
        configuration_generation,
        static_cast<std::uint16_t>(ordinal),
        lane,
        domain,
        external_effect_authorizations.entries[ordinal],
        batch_hash,
    };
}

CompletionState CompletionSlot::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

CompletionError CompletionSlot::error() const noexcept {
    if (state() != CompletionState::Rejected) {
        return CompletionError::None;
    }
    return error_.load(std::memory_order_relaxed);
}

std::optional<DurableToken> CompletionSlot::token() const noexcept {
    if (state_.load(std::memory_order_acquire) != CompletionState::Committed) {
        return std::nullopt;
    }
    return token_;
}

bool CompletionSlot::reset() noexcept {
    const CompletionState current = state_.load(std::memory_order_acquire);
    if (current == CompletionState::Pending) {
        return false;
    }
    token_ = {};
    error_.store(CompletionError::None, std::memory_order_relaxed);
    state_.store(CompletionState::Empty, std::memory_order_release);
    return true;
}

bool CompletionSlot::try_begin() noexcept {
    CompletionState expected = CompletionState::Empty;
    if (!state_.compare_exchange_strong(
            expected, CompletionState::Pending, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    error_.store(CompletionError::None, std::memory_order_relaxed);
    return true;
}

void CompletionSlot::publish_commit(const DurableToken& token) noexcept {
    token_ = token;
    error_.store(CompletionError::None, std::memory_order_relaxed);
    state_.store(CompletionState::Committed, std::memory_order_release);
}

void CompletionSlot::publish_rejection(const CompletionError error) noexcept {
    error_.store(error, std::memory_order_relaxed);
    state_.store(CompletionState::Rejected, std::memory_order_release);
}

struct InMemoryMirroredTier0::Impl final {
    struct Slot final {
        std::atomic<std::uint64_t> commit_batch_id{};
        std::uint64_t producer_sequence{};
        std::uint64_t leadership_epoch{};
        std::uint64_t configuration_generation{};
        DispatchAuthorization dispatch_authorization{};
        AuthorizationEnvelopeVersion authorization_envelope_version{
            AuthorizationEnvelopeVersion::ExactEffectsV1};
        ExternalEffectAuthorizationList external_effect_authorizations{};
        std::uint32_t checksum{};
        std::uint32_t encoded_size{};
        std::uint16_t record_count{};
        DurabilityLane lane{DurabilityLane::Normal};
        HashChainDigest batch_hash{};
        HashChainDigest previous_hash{};
        HashChainDigest chain_hash{};
        std::array<std::byte, kMaximumDurableBatchBytes> encoded{};
    };

    enum class Validation : std::uint8_t {
        Ok,
        Torn,
        MirrorMismatch,
        Corrupt,
    };

    explicit Impl(const InMemoryTier0Configuration supplied)
        : configuration(supplied),
          normal_capacity(
              supplied.capacity - supplied.cancel_reserved_capacity -
              supplied.reconciliation_reserved_capacity),
          primary(std::make_unique<Slot[]>(supplied.capacity)),
          secondary(std::make_unique<Slot[]>(supplied.capacity)) {}

    [[nodiscard]] std::size_t index_for(const std::uint64_t id) const noexcept {
        return static_cast<std::size_t>((id - 1U) % configuration.capacity);
    }

    [[nodiscard]] bool admission_would_consume_reservation(
        const DurabilityLane lane) const noexcept {
        const std::size_t protected_total =
            protected_normal + protected_cancel + protected_reconciliation;
        switch (lane) {
            case DurabilityLane::Normal:
                return protected_normal >= normal_capacity;
            case DurabilityLane::Cancel:
                return protected_total >=
                       configuration.capacity -
                           (configuration.reconciliation_reserved_capacity >
                                    protected_reconciliation
                                ? configuration
                                          .reconciliation_reserved_capacity -
                                      protected_reconciliation
                                : 0U);
            case DurabilityLane::Reconciliation:
                return protected_total >=
                       configuration.capacity -
                           (configuration.cancel_reserved_capacity >
                                    protected_cancel
                                ? configuration.cancel_reserved_capacity -
                                      protected_cancel
                                : 0U);
        }
        return true;
    }

    [[nodiscard]] std::size_t& protected_for(
        const DurabilityLane lane) noexcept {
        switch (lane) {
            case DurabilityLane::Normal:
                return protected_normal;
            case DurabilityLane::Cancel:
                return protected_cancel;
            case DurabilityLane::Reconciliation:
                return protected_reconciliation;
        }
        return protected_normal;
    }

    [[nodiscard]] std::size_t protected_for(
        const DurabilityLane lane) const noexcept {
        switch (lane) {
            case DurabilityLane::Normal:
                return protected_normal;
            case DurabilityLane::Cancel:
                return protected_cancel;
            case DurabilityLane::Reconciliation:
                return protected_reconciliation;
        }
        return 0U;
    }

    static void write_uncommitted(
        Slot& destination,
        const std::uint64_t batch_id,
        const DurableBatch& batch,
        const std::uint32_t checksum,
        const HashChainDigest& batch_hash,
        const HashChainDigest& previous,
        const HashChainDigest& chain) noexcept {
        destination.commit_batch_id.store(0U, std::memory_order_release);
        destination.producer_sequence = batch.producer_sequence();
        destination.leadership_epoch = batch.leadership_epoch();
        destination.configuration_generation =
            batch.configuration_generation();
        destination.dispatch_authorization = batch.dispatch_authorization();
        destination.authorization_envelope_version =
            batch.authorization_envelope_version();
        destination.external_effect_authorizations =
            batch.external_effect_authorizations();
        destination.checksum = checksum;
        destination.encoded_size =
            static_cast<std::uint32_t>(batch.encoded_size());
        destination.record_count =
            static_cast<std::uint16_t>(batch.record_count());
        destination.lane = batch.lane();
        destination.batch_hash = batch_hash;
        destination.previous_hash = previous;
        destination.chain_hash = chain;
        std::copy(batch.encoded_bytes().begin(), batch.encoded_bytes().end(),
                  destination.encoded.begin());
        static_cast<void>(batch_id);
    }

    static void commit(Slot& destination, const std::uint64_t id) noexcept {
        std::atomic_thread_fence(std::memory_order_release);
        destination.commit_batch_id.store(id, std::memory_order_release);
    }

    [[nodiscard]] Validation validate(
        const std::uint64_t id,
        DurableToken& token,
        const Slot*& selected) const noexcept {
        const std::size_t index = index_for(id);
        const Slot& left = primary[index];
        const Slot& right = secondary[index];
        const std::uint64_t left_commit =
            left.commit_batch_id.load(std::memory_order_acquire);
        const std::uint64_t right_commit =
            right.commit_batch_id.load(std::memory_order_acquire);
        if (left_commit != id || right_commit != id) {
            return Validation::Torn;
        }
        if (left.producer_sequence != right.producer_sequence ||
            left.leadership_epoch != right.leadership_epoch ||
            left.configuration_generation !=
                right.configuration_generation ||
            left.dispatch_authorization != right.dispatch_authorization ||
            left.authorization_envelope_version !=
                right.authorization_envelope_version ||
            left.external_effect_authorizations !=
                right.external_effect_authorizations ||
            left.checksum != right.checksum ||
            left.encoded_size != right.encoded_size ||
            left.record_count != right.record_count || left.lane != right.lane ||
            left.batch_hash != right.batch_hash ||
            left.previous_hash != right.previous_hash ||
            left.chain_hash != right.chain_hash ||
            left.encoded_size > kMaximumDurableBatchBytes ||
            std::memcmp(left.encoded.data(), right.encoded.data(),
                        left.encoded_size) != 0) {
            return Validation::MirrorMismatch;
        }
        if (!valid_lane(left.lane) || left.producer_sequence == 0U ||
            left.leadership_epoch == 0U ||
            left.configuration_generation == 0U ||
            !left.dispatch_authorization.valid() ||
            !valid_authorization_envelope_version(
                left.authorization_envelope_version) ||
            (left.authorization_envelope_version ==
                     AuthorizationEnvelopeVersion::LegacyRangeV0
                 ? left.external_effect_authorizations.count != 0U
                 : !left.external_effect_authorizations.valid(
                       left.dispatch_authorization)) ||
            left.record_count == 0U ||
            left.record_count > kMaximumDurableRecordsPerBatch ||
            left.encoded_size == 0U) {
            return Validation::Corrupt;
        }
        const std::span<const std::byte> encoded{left.encoded.data(),
                                                 left.encoded_size};
        if (left.checksum !=
            batch_crc(id, left.producer_sequence, left.lane,
                      left.leadership_epoch, left.configuration_generation,
                      left.dispatch_authorization,
                      left.authorization_envelope_version,
                      left.external_effect_authorizations,
                      left.record_count, encoded)) {
            return Validation::Corrupt;
        }
        if (left.batch_hash !=
                batch_content_hash(
                    id, left.producer_sequence, left.lane,
                    left.leadership_epoch, left.configuration_generation,
                    left.dispatch_authorization,
                    left.authorization_envelope_version,
                    left.external_effect_authorizations,
                    left.record_count,
                    left.checksum, encoded) ||
            left.chain_hash !=
                batch_chain_hash(left.previous_hash, left.batch_hash)) {
            return Validation::Corrupt;
        }
        token = DurableToken{id,
                             left.producer_sequence,
                             left.leadership_epoch,
                             left.configuration_generation,
                             left.dispatch_authorization,
                             left.authorization_envelope_version,
                             left.external_effect_authorizations,
                             left.checksum,
                             left.encoded_size,
                             left.lane,
                             DurabilityDomain::VolatileSoftwareReference,
                             left.batch_hash,
                             left.previous_hash,
                             left.chain_hash};
        selected = &left;
        return Validation::Ok;
    }

    [[nodiscard]] bool is_retained(const std::uint64_t id) const noexcept {
        return retained_count != 0U && id >= oldest_id && id <= newest_id;
    }

    void release_newly_eligible(const std::uint64_t target) noexcept {
        if (target <= overwrite_eligible_through) {
            return;
        }
        const std::uint64_t first = overwrite_eligible_through + 1U;
        const std::uint64_t last = target;
        if (retained_count != 0U) {
            const std::uint64_t retained_first = std::max(first, oldest_id);
            const std::uint64_t retained_last = std::min(last, newest_id);
            if (retained_first <= retained_last) {
                for (std::uint64_t id = retained_first;; ++id) {
                    const Slot& slot = primary[index_for(id)];
                    if (slot.commit_batch_id.load(std::memory_order_acquire) ==
                        id) {
                        std::size_t& count = protected_for(slot.lane);
                        if (count != 0U) {
                            --count;
                        }
                    }
                    if (id == retained_last) {
                        break;
                    }
                }
            }
        }
        overwrite_eligible_through = target;
    }

    void recompute_overwrite_eligibility() noexcept {
        const std::uint64_t target =
            configuration.require_tier1_before_overwrite
                ? tier1_drained_through
                : acknowledged_through;
        release_newly_eligible(target);
    }

    InMemoryTier0Configuration configuration;
    std::size_t normal_capacity{};
    std::unique_ptr<Slot[]> primary;
    std::unique_ptr<Slot[]> secondary;
    mutable std::mutex mutex;
    std::uint64_t next_batch_id{1U};
    std::uint64_t oldest_id{};
    std::uint64_t newest_id{};
    std::size_t retained_count{};
    std::size_t protected_normal{};
    std::size_t protected_cancel{};
    std::size_t protected_reconciliation{};
    std::uint64_t acknowledged_through{};
    std::uint64_t tier1_drained_through{};
    std::uint64_t overwrite_eligible_through{};
    std::uint64_t overwritten_batches{};
    std::uint64_t rejected_appends{};
    std::uint64_t next_segment_id{1U};
    HashChainDigest retained_anchor{};
    HashChainDigest latest_hash{};
    ReferenceFaultPoint next_fault{ReferenceFaultPoint::None};
};

InMemoryMirroredTier0::InMemoryMirroredTier0(
    const InMemoryTier0Configuration configuration) {
    if (configuration.capacity == 0U ||
        configuration.cancel_reserved_capacity > configuration.capacity ||
        configuration.reconciliation_reserved_capacity >
            configuration.capacity -
                configuration.cancel_reserved_capacity ||
        configuration.cancel_reserved_capacity +
                configuration.reconciliation_reserved_capacity >=
            configuration.capacity) {
        throw std::invalid_argument(
            "Tier0 capacity must leave at least one normal-lane slot");
    }
    impl_ = std::make_unique<Impl>(configuration);
}

InMemoryMirroredTier0::~InMemoryMirroredTier0() = default;
InMemoryMirroredTier0::InMemoryMirroredTier0(
    InMemoryMirroredTier0&&) noexcept = default;
InMemoryMirroredTier0& InMemoryMirroredTier0::operator=(
    InMemoryMirroredTier0&&) noexcept = default;

AppendStatus InMemoryMirroredTier0::try_append(
    const DurableBatch& batch,
    CompletionSlot& completion) noexcept {
    if (!completion.try_begin()) {
        return AppendStatus::CompletionSlotBusy;
    }
    std::lock_guard lock{impl_->mutex};
    const auto reject = [&](const AppendStatus status,
                            const CompletionError error) noexcept {
        ++impl_->rejected_appends;
        completion.publish_rejection(error);
        return status;
    };

    if (!batch.valid()) {
        return reject(AppendStatus::InvalidBatch,
                      CompletionError::InvalidBatch);
    }
    if (impl_->next_batch_id == std::numeric_limits<std::uint64_t>::max()) {
        return reject(AppendStatus::BackendFailure,
                      CompletionError::BackendFailure);
    }

    if (impl_->retained_count == impl_->configuration.capacity) {
        if (impl_->oldest_id > impl_->overwrite_eligible_through) {
            return reject(AppendStatus::CapacityExhausted,
                          CompletionError::CapacityExhausted);
        }
    }
    if (impl_->admission_would_consume_reservation(batch.lane())) {
        return reject(AppendStatus::CapacityReserved,
                      CompletionError::CapacityReserved);
    }

    if (impl_->retained_count == impl_->configuration.capacity) {
        DurableToken evicted_token{};
        const Impl::Slot* evicted_slot = nullptr;
        if (impl_->validate(impl_->oldest_id, evicted_token, evicted_slot) !=
            Impl::Validation::Ok) {
            return reject(AppendStatus::MirrorMismatch,
                          CompletionError::MirrorMismatch);
        }
        static_cast<void>(evicted_slot);
        impl_->retained_anchor = evicted_token.chain_hash;
        ++impl_->oldest_id;
        --impl_->retained_count;
        ++impl_->overwritten_batches;
    }

    const std::uint64_t id = impl_->next_batch_id;
    const std::uint32_t checksum =
        batch_crc(id, batch.producer_sequence(), batch.lane(),
                  batch.leadership_epoch(),
                  batch.configuration_generation(),
                  batch.dispatch_authorization(),
                  batch.authorization_envelope_version(),
                  batch.external_effect_authorizations(),
                  static_cast<std::uint16_t>(batch.record_count()),
                  batch.encoded_bytes());
    const HashChainDigest content_hash =
        batch_content_hash(
            id, batch.producer_sequence(), batch.lane(),
            batch.leadership_epoch(), batch.configuration_generation(),
            batch.dispatch_authorization(),
            batch.authorization_envelope_version(),
            batch.external_effect_authorizations(),
            static_cast<std::uint16_t>(batch.record_count()), checksum,
            batch.encoded_bytes());
    const HashChainDigest chain =
        batch_chain_hash(impl_->latest_hash, content_hash);
    const std::size_t index = impl_->index_for(id);
    Impl::Slot& primary = impl_->primary[index];
    Impl::Slot& secondary = impl_->secondary[index];
    primary.commit_batch_id.store(0U, std::memory_order_release);
    secondary.commit_batch_id.store(0U, std::memory_order_release);
    Impl::write_uncommitted(primary, id, batch, checksum, content_hash,
                            impl_->latest_hash, chain);

    const ReferenceFaultPoint fault = impl_->next_fault;
    impl_->next_fault = ReferenceFaultPoint::None;
    if (fault == ReferenceFaultPoint::AfterPrimaryWrite) {
        return reject(AppendStatus::TornCommitRejected,
                      CompletionError::TornCommit);
    }
    Impl::commit(primary, id);
    if (fault == ReferenceFaultPoint::AfterPrimaryCommit) {
        return reject(AppendStatus::TornCommitRejected,
                      CompletionError::TornCommit);
    }

    Impl::write_uncommitted(secondary, id, batch, checksum, content_hash,
                            impl_->latest_hash, chain);
    Impl::commit(secondary, id);
    DurableToken verified{};
    const Impl::Slot* verified_slot = nullptr;
    if (impl_->validate(id, verified, verified_slot) != Impl::Validation::Ok) {
        return reject(AppendStatus::MirrorMismatch,
                      CompletionError::MirrorMismatch);
    }
    static_cast<void>(verified_slot);

    if (impl_->retained_count == 0U) {
        impl_->oldest_id = id;
        impl_->retained_anchor = impl_->latest_hash;
    }
    impl_->newest_id = id;
    ++impl_->next_batch_id;
    ++impl_->retained_count;
    ++impl_->protected_for(batch.lane());
    impl_->latest_hash = chain;
    completion.publish_commit(verified);
    return AppendStatus::Committed;
}

bool InMemoryMirroredTier0::authorizes_external_effect(
    const DurableToken& token,
    const DurabilityDomain required_domain) const noexcept {
    if (token.authorization_envelope_version !=
            AuthorizationEnvelopeVersion::ExactEffectsV1 ||
        required_domain != DurabilityDomain::VolatileSoftwareReference ||
        token.domain != DurabilityDomain::VolatileSoftwareReference) {
        return false;
    }
    std::lock_guard lock{impl_->mutex};
    if (!impl_->is_retained(token.batch_id)) {
        return false;
    }
    DurableToken verified{};
    const Impl::Slot* selected = nullptr;
    return impl_->validate(token.batch_id, verified, selected) ==
               Impl::Validation::Ok &&
           selected != nullptr && verified == token;
}

bool InMemoryMirroredTier0::authorizes_external_effect(
    const DurableEffectPermit& permit,
    const DurabilityDomain required_domain) const noexcept {
    if (!static_cast<bool>(permit) ||
        required_domain != DurabilityDomain::VolatileSoftwareReference ||
        permit.domain != DurabilityDomain::VolatileSoftwareReference) {
        return false;
    }
    std::lock_guard lock{impl_->mutex};
    if (!impl_->is_retained(permit.batch_id)) {
        return false;
    }
    DurableToken verified{};
    const Impl::Slot* selected = nullptr;
    return impl_->validate(permit.batch_id, verified, selected) ==
               Impl::Validation::Ok &&
           selected != nullptr &&
           verified.producer_sequence == permit.producer_sequence &&
           verified.leadership_epoch == permit.leadership_epoch &&
           verified.configuration_generation ==
               permit.configuration_generation &&
           verified.lane == permit.lane && verified.domain == permit.domain &&
           verified.batch_hash == permit.batch_hash &&
           permit.effect_ordinal <
               verified.external_effect_authorizations.count &&
           verified.external_effect_authorizations
                   .entries[permit.effect_ordinal] == permit.authorization;
}

ReadStatus InMemoryMirroredTier0::read(
    const std::uint64_t batch_id,
    DurableBatch& batch,
    DurableToken& token) const noexcept {
    if (batch_id == 0U) {
        return ReadStatus::InvalidBatchId;
    }
    std::lock_guard lock{impl_->mutex};
    if (!impl_->is_retained(batch_id)) {
        return ReadStatus::NotRetained;
    }
    DurableToken verified{};
    const Impl::Slot* selected = nullptr;
    if (impl_->validate(batch_id, verified, selected) != Impl::Validation::Ok ||
        selected == nullptr ||
        !batch.assign_encoded(
            selected->lane, selected->producer_sequence,
            selected->leadership_epoch,
            selected->configuration_generation,
            selected->dispatch_authorization,
            selected->authorization_envelope_version,
            selected->external_effect_authorizations,
            selected->record_count,
            {selected->encoded.data(), selected->encoded_size})) {
        return ReadStatus::Corrupt;
    }
    token = verified;
    return ReadStatus::Ok;
}

ReplayStatus InMemoryMirroredTier0::replay(
    const ReplayRange range,
    IReplaySink& sink) const noexcept {
    std::lock_guard lock{impl_->mutex};
    if (impl_->retained_count == 0U) {
        return ReplayStatus::Empty;
    }
    if (range.first_batch_id == 0U ||
        range.last_batch_id < range.first_batch_id) {
        return ReplayStatus::InvalidRange;
    }
    if (range.first_batch_id < impl_->oldest_id ||
        range.last_batch_id > impl_->newest_id) {
        return ReplayStatus::NotRetained;
    }

    HashChainDigest expected_previous{};
    bool have_previous = false;
    for (std::uint64_t id = range.first_batch_id;; ++id) {
        DurableToken token{};
        const Impl::Slot* selected = nullptr;
        if (impl_->validate(id, token, selected) != Impl::Validation::Ok ||
            selected == nullptr ||
            (have_previous && token.previous_hash != expected_previous)) {
            return ReplayStatus::Corrupt;
        }
        DurableBatch batch;
        if (!batch.assign_encoded(selected->lane, selected->producer_sequence,
                                  selected->leadership_epoch,
                                  selected->configuration_generation,
                                  selected->dispatch_authorization,
                                  selected->authorization_envelope_version,
                                  selected->external_effect_authorizations,
                                  selected->record_count,
                                  {selected->encoded.data(),
                                   selected->encoded_size})) {
            return ReplayStatus::Corrupt;
        }
        if (!sink.on_batch(token, batch)) {
            return ReplayStatus::SinkStopped;
        }
        expected_previous = token.chain_hash;
        have_previous = true;
        if (id == range.last_batch_id) {
            break;
        }
    }
    return ReplayStatus::Ok;
}

AcknowledgeStatus InMemoryMirroredTier0::acknowledge(
    const std::uint64_t through_batch_id) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (through_batch_id == 0U || through_batch_id > impl_->newest_id) {
        return AcknowledgeStatus::InvalidBatchId;
    }
    if (through_batch_id <= impl_->acknowledged_through) {
        return AcknowledgeStatus::Unchanged;
    }
    impl_->acknowledged_through = through_batch_id;
    impl_->recompute_overwrite_eligibility();
    return AcknowledgeStatus::Advanced;
}

DrainStatus InMemoryMirroredTier0::drain_to_tier1(
    const ReplayRange range,
    ITier1SegmentSink& sink) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (impl_->retained_count == 0U) {
        return DrainStatus::Empty;
    }
    if (range.first_batch_id == 0U ||
        range.last_batch_id < range.first_batch_id ||
        range.last_batch_id - range.first_batch_id >=
            std::numeric_limits<std::uint32_t>::max()) {
        return DrainStatus::InvalidRange;
    }
    if (range.first_batch_id < impl_->oldest_id ||
        range.last_batch_id > impl_->newest_id) {
        return DrainStatus::NotRetained;
    }

    DurableToken first_token{};
    DurableToken final_token{};
    HashChainDigest expected_previous{};
    bool have_previous = false;
    for (std::uint64_t id = range.first_batch_id;; ++id) {
        DurableToken token{};
        const Impl::Slot* selected = nullptr;
        if (impl_->validate(id, token, selected) != Impl::Validation::Ok ||
            selected == nullptr ||
            (have_previous && token.previous_hash != expected_previous)) {
            return DrainStatus::Corrupt;
        }
        if (!have_previous) {
            first_token = token;
        }
        final_token = token;
        expected_previous = token.chain_hash;
        have_previous = true;
        if (id == range.last_batch_id) {
            break;
        }
    }

    const Tier1SegmentMetadata descriptor{
        impl_->next_segment_id,
        range.first_batch_id,
        range.last_batch_id,
        static_cast<std::uint32_t>(range.last_batch_id -
                                   range.first_batch_id + 1U),
        first_token.previous_hash,
        final_token.chain_hash,
        DurabilityDomain::VolatileSoftwareReference};
    if (!sink.begin_segment(descriptor)) {
        return DrainStatus::SinkRejected;
    }
    for (std::uint64_t id = range.first_batch_id;; ++id) {
        DurableToken token{};
        const Impl::Slot* selected = nullptr;
        if (impl_->validate(id, token, selected) != Impl::Validation::Ok ||
            selected == nullptr) {
            sink.abort_segment();
            return DrainStatus::Corrupt;
        }
        DurableBatch batch;
        if (!batch.assign_encoded(selected->lane, selected->producer_sequence,
                                  selected->leadership_epoch,
                                  selected->configuration_generation,
                                  selected->dispatch_authorization,
                                  selected->authorization_envelope_version,
                                  selected->external_effect_authorizations,
                                  selected->record_count,
                                  {selected->encoded.data(),
                                   selected->encoded_size}) ||
            !sink.append_batch(token, batch)) {
            sink.abort_segment();
            return DrainStatus::SinkRejected;
        }
        if (id == range.last_batch_id) {
            break;
        }
    }
    if (!sink.commit_segment(descriptor)) {
        sink.abort_segment();
        return DrainStatus::SinkRejected;
    }

    ++impl_->next_segment_id;
    if (range.first_batch_id <= impl_->tier1_drained_through + 1U &&
        range.last_batch_id > impl_->tier1_drained_through) {
        impl_->tier1_drained_through = range.last_batch_id;
        impl_->recompute_overwrite_eligibility();
    }
    return DrainStatus::Ok;
}

FlightRecorderMetadata InMemoryMirroredTier0::metadata() const noexcept {
    std::lock_guard lock{impl_->mutex};
    FlightRecorderMetadata result{};
    result.tier0_capacity = impl_->configuration.capacity;
    result.normal_capacity = impl_->normal_capacity;
    result.cancel_reserved_capacity =
        impl_->configuration.cancel_reserved_capacity;
    result.reconciliation_reserved_capacity =
        impl_->configuration.reconciliation_reserved_capacity;
    result.retained_batches = impl_->retained_count;
    result.protected_normal_batches = impl_->protected_normal;
    result.protected_cancel_batches = impl_->protected_cancel;
    result.protected_reconciliation_batches =
        impl_->protected_reconciliation;
    result.newest_batch_id = impl_->newest_id;
    result.acknowledged_through = impl_->acknowledged_through;
    result.tier1_drained_through = impl_->tier1_drained_through;
    result.overwrite_eligible_through = impl_->overwrite_eligible_through;
    result.overwritten_batches = impl_->overwritten_batches;
    result.rejected_appends = impl_->rejected_appends;
    result.retained.empty = impl_->retained_count == 0U;
    if (!result.retained.empty) {
        result.retained.first_batch_id = impl_->oldest_id;
        result.retained.last_batch_id = impl_->newest_id;
    }
    result.retained_anchor_hash = impl_->retained_anchor;
    result.latest_chain_hash = impl_->latest_hash;
    result.tier1_required_before_overwrite =
        impl_->configuration.require_tier1_before_overwrite;
    result.domain = DurabilityDomain::VolatileSoftwareReference;
    return result;
}

DurabilityDomain InMemoryMirroredTier0::durability_domain() const noexcept {
    return DurabilityDomain::VolatileSoftwareReference;
}

void InMemoryMirroredTier0::set_next_append_fault_for_testing(
    const ReferenceFaultPoint point) noexcept {
    std::lock_guard lock{impl_->mutex};
    impl_->next_fault = point;
}

bool InMemoryMirroredTier0::corrupt_payload_for_testing(
    const std::uint64_t batch_id,
    const MirrorCopy copy,
    const std::size_t byte_offset) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (!impl_->is_retained(batch_id)) {
        return false;
    }
    Impl::Slot& slot = copy == MirrorCopy::Primary
                           ? impl_->primary[impl_->index_for(batch_id)]
                           : impl_->secondary[impl_->index_for(batch_id)];
    if (slot.commit_batch_id.load(std::memory_order_acquire) != batch_id ||
        byte_offset >= slot.encoded_size) {
        return false;
    }
    slot.encoded[byte_offset] ^= std::byte{0x01U};
    return true;
}

bool InMemoryMirroredTier0::corrupt_external_effect_for_testing(
    const std::uint64_t batch_id,
    const MirrorCopy copy,
    const std::size_t effect_ordinal) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (!impl_->is_retained(batch_id)) {
        return false;
    }
    Impl::Slot& slot = copy == MirrorCopy::Primary
        ? impl_->primary[impl_->index_for(batch_id)]
        : impl_->secondary[impl_->index_for(batch_id)];
    if (slot.commit_batch_id.load(std::memory_order_acquire) != batch_id ||
        effect_ordinal >= slot.external_effect_authorizations.count) {
        return false;
    }
    slot.external_effect_authorizations.entries[effect_ordinal]
        .frame_hash[0] ^= std::byte{0x01U};
    return true;
}

bool InMemoryMirroredTier0::tear_commit_for_testing(
    const std::uint64_t batch_id,
    const MirrorCopy copy) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (!impl_->is_retained(batch_id)) {
        return false;
    }
    Impl::Slot& slot = copy == MirrorCopy::Primary
                           ? impl_->primary[impl_->index_for(batch_id)]
                           : impl_->secondary[impl_->index_for(batch_id)];
    if (slot.commit_batch_id.load(std::memory_order_acquire) != batch_id) {
        return false;
    }
    slot.commit_batch_id.store(0U, std::memory_order_release);
    return true;
}

struct InMemoryTier1Archive::Impl final {
    struct StoredBatch final {
        DurableToken token{};
        DurableBatch batch{};
    };

    Impl(const std::size_t supplied_batch_capacity,
         const std::size_t supplied_segment_capacity)
        : batch_capacity(supplied_batch_capacity),
          segment_capacity(supplied_segment_capacity),
          batches(std::make_unique<StoredBatch[]>(supplied_batch_capacity)),
          segments(std::make_unique<Tier1SegmentMetadata[]>(
              supplied_segment_capacity)) {}

    std::size_t batch_capacity{};
    std::size_t segment_capacity{};
    std::unique_ptr<StoredBatch[]> batches;
    std::unique_ptr<Tier1SegmentMetadata[]> segments;
    mutable std::mutex mutex;
    std::size_t committed_batches{};
    std::size_t committed_segments{};
    bool pending{};
    Tier1SegmentMetadata pending_metadata{};
    std::size_t pending_start{};
    std::size_t pending_count{};
};

InMemoryTier1Archive::InMemoryTier1Archive(
    const std::size_t batch_capacity,
    const std::size_t segment_capacity) {
    if (batch_capacity == 0U || segment_capacity == 0U) {
        throw std::invalid_argument("Tier1 reference capacities must be nonzero");
    }
    impl_ = std::make_unique<Impl>(batch_capacity, segment_capacity);
}

InMemoryTier1Archive::~InMemoryTier1Archive() = default;
InMemoryTier1Archive::InMemoryTier1Archive(
    InMemoryTier1Archive&&) noexcept = default;
InMemoryTier1Archive& InMemoryTier1Archive::operator=(
    InMemoryTier1Archive&&) noexcept = default;

bool InMemoryTier1Archive::begin_segment(
    const Tier1SegmentMetadata& metadata) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (impl_->pending || metadata.segment_id == 0U ||
        metadata.first_batch_id == 0U ||
        metadata.last_batch_id < metadata.first_batch_id ||
        metadata.batch_count == 0U ||
        static_cast<std::uint64_t>(metadata.batch_count) !=
            metadata.last_batch_id - metadata.first_batch_id + 1U ||
        metadata.source_domain !=
            DurabilityDomain::VolatileSoftwareReference ||
        impl_->committed_segments >= impl_->segment_capacity ||
        metadata.batch_count >
            impl_->batch_capacity - impl_->committed_batches) {
        return false;
    }
    if (impl_->committed_segments != 0U) {
        const Tier1SegmentMetadata& previous =
            impl_->segments[impl_->committed_segments - 1U];
        if (previous.segment_id ==
                std::numeric_limits<std::uint64_t>::max() ||
            metadata.segment_id != previous.segment_id + 1U ||
            previous.last_batch_id ==
                std::numeric_limits<std::uint64_t>::max() ||
            metadata.first_batch_id != previous.last_batch_id + 1U ||
            metadata.previous_hash != previous.final_hash) {
            return false;
        }
    }
    impl_->pending = true;
    impl_->pending_metadata = metadata;
    impl_->pending_start = impl_->committed_batches;
    impl_->pending_count = 0U;
    return true;
}

bool InMemoryTier1Archive::append_batch(
    const DurableToken& token,
    const DurableBatch& batch) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (!impl_->pending ||
        impl_->pending_count >= impl_->pending_metadata.batch_count ||
        !token_matches_batch_impl(token, batch)) {
        return false;
    }
    const std::uint64_t expected_id =
        impl_->pending_metadata.first_batch_id + impl_->pending_count;
    const std::size_t destination =
        impl_->pending_start + impl_->pending_count;
    if (token.batch_id != expected_id ||
        token.domain != impl_->pending_metadata.source_domain ||
        (impl_->pending_count == 0U
             ? token.previous_hash != impl_->pending_metadata.previous_hash
             : token.previous_hash !=
                   impl_->batches[destination - 1U]
                       .token.chain_hash)) {
        return false;
    }
    impl_->batches[destination].token = token;
    impl_->batches[destination].batch = batch;
    ++impl_->pending_count;
    return true;
}

bool InMemoryTier1Archive::commit_segment(
    const Tier1SegmentMetadata& metadata) noexcept {
    std::lock_guard lock{impl_->mutex};
    if (!impl_->pending || metadata.segment_id !=
                              impl_->pending_metadata.segment_id ||
        metadata.first_batch_id !=
            impl_->pending_metadata.first_batch_id ||
        metadata.last_batch_id != impl_->pending_metadata.last_batch_id ||
        metadata.batch_count != impl_->pending_metadata.batch_count ||
        metadata.previous_hash != impl_->pending_metadata.previous_hash ||
        metadata.final_hash != impl_->pending_metadata.final_hash ||
        metadata.source_domain != impl_->pending_metadata.source_domain ||
        impl_->pending_count != metadata.batch_count ||
        impl_->pending_count == 0U ||
        impl_->batches[impl_->pending_start + impl_->pending_count - 1U]
                .token.chain_hash !=
            metadata.final_hash) {
        return false;
    }
    impl_->segments[impl_->committed_segments] = metadata;
    ++impl_->committed_segments;
    impl_->committed_batches += impl_->pending_count;
    impl_->pending = false;
    impl_->pending_count = 0U;
    return true;
}

void InMemoryTier1Archive::abort_segment() noexcept {
    std::lock_guard lock{impl_->mutex};
    if (!impl_->pending) {
        return;
    }
    impl_->committed_batches = impl_->pending_start;
    impl_->pending = false;
    impl_->pending_count = 0U;
}

std::size_t InMemoryTier1Archive::batch_count() const noexcept {
    std::lock_guard lock{impl_->mutex};
    return impl_->committed_batches;
}

std::size_t InMemoryTier1Archive::segment_count() const noexcept {
    std::lock_guard lock{impl_->mutex};
    return impl_->committed_segments;
}

std::optional<Tier1SegmentMetadata> InMemoryTier1Archive::segment(
    const std::size_t index) const noexcept {
    std::lock_guard lock{impl_->mutex};
    if (index >= impl_->committed_segments) {
        return std::nullopt;
    }
    return impl_->segments[index];
}

ReadStatus InMemoryTier1Archive::read(
    const std::uint64_t batch_id,
    DurableBatch& batch,
    DurableToken& token) const noexcept {
    if (batch_id == 0U) {
        return ReadStatus::InvalidBatchId;
    }
    std::lock_guard lock{impl_->mutex};
    for (std::size_t index = 0U; index < impl_->committed_batches; ++index) {
        const Impl::StoredBatch& stored = impl_->batches[index];
        if (stored.token.batch_id == batch_id) {
            if (!token_matches_batch_impl(stored.token, stored.batch)) {
                return ReadStatus::Corrupt;
            }
            batch = stored.batch;
            token = stored.token;
            return ReadStatus::Ok;
        }
    }
    return ReadStatus::NotRetained;
}

bool InMemoryTier1Archive::corrupt_external_effect_for_testing(
    const std::uint64_t batch_id,
    const std::size_t effect_ordinal) noexcept {
    std::lock_guard lock{impl_->mutex};
    for (std::size_t index{}; index < impl_->committed_batches; ++index) {
        Impl::StoredBatch& stored = impl_->batches[index];
        if (stored.token.batch_id == batch_id) {
            if (effect_ordinal >=
                stored.token.external_effect_authorizations.count) {
                return false;
            }
            stored.token.external_effect_authorizations
                .entries[effect_ordinal].frame_hash[0] ^= std::byte{0x01U};
            return true;
        }
    }
    return false;
}

DurabilityDomain InMemoryTier1Archive::durability_domain() const noexcept {
    return DurabilityDomain::VolatileSoftwareReference;
}

std::uint32_t crc32c(const std::span<const std::byte> bytes) noexcept {
    Crc32cAccumulator accumulator;
    accumulator.update(bytes);
    return accumulator.finish();
}

bool verify_durable_token(
    const DurableToken& token,
    const DurableBatch& batch) noexcept {
    return token_matches_batch_impl(token, batch);
}

}  // namespace sorx::recorder
