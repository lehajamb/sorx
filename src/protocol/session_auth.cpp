#include "sorx/protocol/session_auth.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace sorx::protocol::session_auth {
namespace {

void write_u64(
    std::array<std::byte, authentication_associated_data_size>& output,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        output[offset + index] = static_cast<std::byte>(
            value >> ((7U - index) * 8U));
    }
}

[[nodiscard]] AuthError map_key_status(
    const AuthenticationKeyStatus status) noexcept {
    switch (status) {
        case AuthenticationKeyStatus::Success: return AuthError::None;
        case AuthenticationKeyStatus::MissingKey: return AuthError::MissingKey;
        case AuthenticationKeyStatus::RetiredOrRotated:
            return AuthError::KeyRetiredOrRotated;
        case AuthenticationKeyStatus::BackendUnavailable:
            return AuthError::KeyProviderUnavailable;
        case AuthenticationKeyStatus::InvalidProof:
            return AuthError::InvalidLogonProof;
    }
    return AuthError::KeyProviderUnavailable;
}

[[nodiscard]] Direction client_direction(
    const sobp_v1::EndpointRole role) noexcept {
    return role == sobp_v1::EndpointRole::Client ?
        Direction::ClientToServer : Direction::ServerToClient;
}

[[nodiscard]] Direction peer_direction(
    const sobp_v1::EndpointRole role) noexcept {
    return role == sobp_v1::EndpointRole::Client ?
        Direction::ServerToClient : Direction::ClientToServer;
}

}  // namespace

std::array<std::byte, authentication_associated_data_size>
authentication_associated_data(
    const AuthContext& context,
    const Direction direction,
    const TagSize tag_size) noexcept {
    std::array<std::byte, authentication_associated_data_size> output{};
    constexpr std::array<std::byte, 8U> domain{
        std::byte{'S'}, std::byte{'O'}, std::byte{'R'}, std::byte{'X'},
        std::byte{'A'}, std::byte{'U'}, std::byte{'T'}, std::byte{'H'}};
    std::copy(domain.begin(), domain.end(), output.begin());
    output[8U] = std::byte{1U};
    output[9U] = static_cast<std::byte>(direction);
    output[10U] = static_cast<std::byte>(tag_size_bytes(tag_size));
    // Bytes 11..15 are reserved and remain zero. Their inclusion makes future
    // associated-data versions unambiguous.
    write_u64(output, 16U, context.session_id);
    write_u64(output, 24U, context.session_epoch);
    write_u64(output, 32U, context.key_id);
    return output;
}

bool constant_time_equal(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) return false;
    volatile std::uint8_t difference = 0U;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const auto accumulated = static_cast<std::uint8_t>(difference);
        difference = static_cast<std::uint8_t>(accumulated |
            static_cast<std::uint8_t>(
                std::to_integer<std::uint8_t>(left[index]) ^
                std::to_integer<std::uint8_t>(right[index])));
    }
    return difference == 0U;
}

AuthError verify_logon_proof(
    IAuthenticationKeyProvider& key_provider,
    const AuthContext& context,
    const std::span<const std::byte> canonical_logon_transcript,
    const std::span<const std::byte> proof,
    VerifiedLogonCapability& capability) noexcept {
    capability.clear();
    if (!context.valid()) return AuthError::MissingContext;
    if (canonical_logon_transcript.empty() || proof.empty()) {
        return AuthError::InvalidLogonProof;
    }
    const auto error = map_key_status(key_provider.verify_logon_proof(
        context, canonical_logon_transcript, proof));
    if (error != AuthError::None) return error;
    capability.context_ = context;
    capability.key_provider_ = &key_provider;
    capability.verified_ = true;
    return AuthError::None;
}

SessionAuthenticator::SessionAuthenticator(
    VerifiedLogonCapability&& verified_logon,
    const sobp_v1::EndpointRole local_role,
    const TagSize tag_size,
    IMacProvider& mac_provider,
    const std::uint64_t initial_outbound_sequence,
    const std::uint64_t initial_inbound_sequence) noexcept
    : context_(verified_logon.context_),
      outbound_direction_(client_direction(local_role)),
      inbound_direction_(peer_direction(local_role)),
      tag_size_(tag_size_bytes(tag_size)),
      key_provider_(verified_logon.key_provider_),
      mac_provider_(&mac_provider),
      next_outbound_sequence_(initial_outbound_sequence),
      next_inbound_sequence_(initial_inbound_sequence) {
    const bool verified = verified_logon.valid();
    verified_logon.clear();
    if (!verified) {
        initialization_error_ = AuthError::MissingVerifiedLogon;
        return;
    }
    if (local_role != sobp_v1::EndpointRole::Client &&
        local_role != sobp_v1::EndpointRole::Server) {
        initialization_error_ = AuthError::InvalidRole;
        return;
    }
    if (tag_size_ == 0U) {
        initialization_error_ = AuthError::InvalidTagSize;
        return;
    }
    if (initial_outbound_sequence == 0U || initial_inbound_sequence == 0U) {
        initialization_error_ = AuthError::SequenceMismatch;
        return;
    }
    initialization_error_ = derive_key(outbound_direction_, outbound_key_);
    if (initialization_error_ != AuthError::None) return;
    initialization_error_ = derive_key(inbound_direction_, inbound_key_);
    if (initialization_error_ != AuthError::None) {
        clear_key(outbound_key_);
        return;
    }
    if (!key_current(outbound_direction_, outbound_key_) ||
        !key_current(inbound_direction_, inbound_key_)) {
        clear_key(outbound_key_);
        clear_key(inbound_key_);
        initialization_error_ = AuthError::KeyRetiredOrRotated;
        return;
    }
    initialization_error_ = AuthError::None;
}

SessionAuthenticator::~SessionAuthenticator() {
    clear_key(outbound_key_);
    clear_key(inbound_key_);
}

AuthError SessionAuthenticator::derive_key(
    const Direction direction,
    DirectionKey& output) noexcept {
    clear_key(output);
    std::size_t bytes_written = 0U;
    std::uint64_t generation = 0U;
    const auto status = key_provider_->derive_session_key(
        context_, direction, output.bytes, bytes_written, generation);
    const auto error = map_key_status(status);
    if (error != AuthError::None) {
        clear_key(output);
        return error;
    }
    if (bytes_written == 0U || bytes_written > output.bytes.size() ||
        generation == 0U) {
        clear_key(output);
        return AuthError::MissingKey;
    }
    output.size = bytes_written;
    output.generation = generation;
    return AuthError::None;
}

bool SessionAuthenticator::key_current(
    const Direction direction,
    const DirectionKey& key) const noexcept {
    return key.size != 0U && key.generation != 0U &&
        key_provider_->is_key_current(context_, direction, key.generation);
}

void SessionAuthenticator::clear_key(DirectionKey& key) noexcept {
    volatile std::byte* const memory = key.bytes.data();
    for (std::size_t index = 0U; index < key.bytes.size(); ++index) {
        memory[index] = std::byte{0U};
    }
    key.size = 0U;
    key.generation = 0U;
}

AuthEncodeResult SessionAuthenticator::seal(
    const sobp_v1::Message& message,
    const std::span<std::byte> destination) noexcept {
    if (!ready()) return AuthEncodeResult{initialization_error_, {}, 0U};
    if (!key_current(outbound_direction_, outbound_key_)) {
        return AuthEncodeResult{AuthError::KeyRetiredOrRotated, {}, 0U};
    }
    if (message.header.session_id != context_.session_id) {
        return AuthEncodeResult{AuthError::WrongSession, {}, 0U};
    }
    if (outbound_sequence_exhausted_) {
        return AuthEncodeResult{AuthError::SequenceMismatch, {}, 0U};
    }
    if (message.header.sequence < next_outbound_sequence_) {
        return AuthEncodeResult{AuthError::Replay, {}, 0U};
    }
    if (message.header.sequence != next_outbound_sequence_) {
        return AuthEncodeResult{AuthError::SequenceMismatch, {}, 0U};
    }
    const auto required_size = sobp_v1::header_size +
        sobp_v1::payload_size(message.header.message_type) + tag_size_;
    if (destination.size() < required_size) {
        return AuthEncodeResult{AuthError::OutputTooSmall, {}, 0U};
    }
    const auto encoded = sobp_v1::encode_to(message, destination);
    if (!encoded) {
        return AuthEncodeResult{
            AuthError::SobpProtocolError, encoded.error, 0U};
    }
    const auto frame = std::span<const std::byte>{destination}.first(
        encoded.bytes_written);
    const auto associated = authentication_associated_data(
        context_, outbound_direction_,
        tag_size_ == 16U ? TagSize::Bytes16 : TagSize::Bytes32);
    const auto tag = destination.subspan(encoded.bytes_written, tag_size_);
    if (!mac_provider_->compute_tag(
            std::span<const std::byte>{outbound_key_.bytes}.first(
                outbound_key_.size),
            frame,
            associated,
            tag)) {
        std::fill(tag.begin(), tag.end(), std::byte{0U});
        return AuthEncodeResult{AuthError::MacProviderFailure, {}, 0U};
    }
    if (next_outbound_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        outbound_sequence_exhausted_ = true;
    } else {
        ++next_outbound_sequence_;
    }
    return AuthEncodeResult{
        AuthError::None, sobp_v1::ProtocolError::None,
        encoded.bytes_written + tag_size_};
}

AuthVerifyResult SessionAuthenticator::verify(
    const std::span<const std::byte> authenticated_frame,
    const AuthContext& presented_context,
    const Direction presented_direction) noexcept {
    if (!ready()) return AuthVerifyResult{initialization_error_};
    if (!presented_context.valid()) {
        return AuthVerifyResult{AuthError::MissingContext};
    }
    if (presented_context.session_id != context_.session_id) {
        return AuthVerifyResult{AuthError::WrongSession};
    }
    if (presented_context.session_epoch != context_.session_epoch) {
        return AuthVerifyResult{AuthError::WrongEpoch};
    }
    if (presented_context.key_id != context_.key_id) {
        return AuthVerifyResult{AuthError::WrongKeyId};
    }
    if (presented_direction != inbound_direction_) {
        return AuthVerifyResult{AuthError::WrongDirection};
    }
    if (!key_current(inbound_direction_, inbound_key_)) {
        return AuthVerifyResult{AuthError::KeyRetiredOrRotated};
    }
    if (authenticated_frame.size() < sobp_v1::header_size + tag_size_) {
        return AuthVerifyResult{AuthError::EnvelopeTooShort};
    }
    if (authenticated_frame.size() > maximum_authenticated_frame_size) {
        return AuthVerifyResult{
            AuthError::SobpProtocolError,
            sobp_v1::ProtocolError::PayloadSizeMismatch};
    }
    const auto frame_size = authenticated_frame.size() - tag_size_;
    const auto frame = authenticated_frame.first(frame_size);
    const auto provided_tag = authenticated_frame.last(tag_size_);
    std::array<std::byte, maximum_authentication_tag_size> expected_tag{};
    const auto expected_tag_view =
        std::span<std::byte>{expected_tag}.first(tag_size_);
    const auto associated = authentication_associated_data(
        context_, inbound_direction_,
        tag_size_ == 16U ? TagSize::Bytes16 : TagSize::Bytes32);
    if (!mac_provider_->compute_tag(
            std::span<const std::byte>{inbound_key_.bytes}.first(
                inbound_key_.size),
            frame,
            associated,
            expected_tag_view)) {
        return AuthVerifyResult{AuthError::MacProviderFailure};
    }
    if (!constant_time_equal(provided_tag, expected_tag_view)) {
        return AuthVerifyResult{AuthError::TagMismatch};
    }
    auto decoded = sobp_v1::decode(frame);
    if (!decoded) {
        return AuthVerifyResult{
            AuthError::SobpProtocolError, decoded.error, std::nullopt,
            authenticated_frame.size()};
    }
    if (decoded.message->header.session_id != context_.session_id) {
        return AuthVerifyResult{AuthError::WrongSession};
    }
    if (inbound_sequence_exhausted_) {
        return AuthVerifyResult{AuthError::SequenceMismatch};
    }
    if (decoded.message->header.sequence < next_inbound_sequence_) {
        return AuthVerifyResult{AuthError::Replay};
    }
    if (decoded.message->header.sequence != next_inbound_sequence_) {
        return AuthVerifyResult{AuthError::SequenceMismatch};
    }
    if (next_inbound_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        inbound_sequence_exhausted_ = true;
    } else {
        ++next_inbound_sequence_;
    }
    return AuthVerifyResult{
        AuthError::None,
        sobp_v1::ProtocolError::None,
        std::move(decoded.message),
        authenticated_frame.size()};
}

}  // namespace sorx::protocol::session_auth
