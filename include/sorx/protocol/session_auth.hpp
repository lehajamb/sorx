#pragma once

#include "sorx/protocol/sobp_v1.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sorx::protocol::session_auth {

inline constexpr std::size_t maximum_derived_key_size = 64U;
inline constexpr std::size_t authentication_associated_data_size = 40U;
inline constexpr std::size_t maximum_authentication_tag_size = 32U;
inline constexpr std::size_t maximum_authenticated_frame_size =
    sobp_v1::maximum_frame_size + maximum_authentication_tag_size;

enum class Direction : std::uint8_t {
    ClientToServer = 1U,
    ServerToClient = 2U,
};

enum class TagSize : std::uint8_t {
    Bytes16 = 16U,
    Bytes32 = 32U,
};

[[nodiscard]] constexpr std::size_t tag_size_bytes(
    const TagSize value) noexcept {
    switch (value) {
        case TagSize::Bytes16: return 16U;
        case TagSize::Bytes32: return 32U;
    }
    return 0U;
}

struct AuthContext final {
    std::uint64_t session_id{};
    std::uint64_t session_epoch{};
    std::uint64_t key_id{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return session_id != 0U && session_epoch != 0U && key_id != 0U;
    }

    constexpr auto operator<=>(const AuthContext&) const noexcept = default;
};

enum class AuthenticationKeyStatus : std::uint8_t {
    Success,
    MissingKey,
    RetiredOrRotated,
    BackendUnavailable,
    InvalidProof,
};

// Production implementations are adapters over TPM, PKCS#11, or an HSM.
// Implementations must not export a long-lived root key. derive_session_key()
// writes short-lived direction-specific material into caller-owned storage and
// returns an opaque generation used to detect rotation after session setup.
class IAuthenticationKeyProvider {
public:
    virtual ~IAuthenticationKeyProvider() = default;

    [[nodiscard]] virtual AuthenticationKeyStatus verify_logon_proof(
        const AuthContext& context,
        std::span<const std::byte> canonical_logon_transcript,
        std::span<const std::byte> proof) noexcept = 0;

    [[nodiscard]] virtual AuthenticationKeyStatus derive_session_key(
        const AuthContext& context,
        Direction direction,
        std::span<std::byte> output,
        std::size_t& bytes_written,
        std::uint64_t& key_generation) noexcept = 0;

    [[nodiscard]] virtual bool is_key_current(
        const AuthContext& context,
        Direction direction,
        std::uint64_t key_generation) const noexcept = 0;
};

// This seam deliberately supplies no production MAC algorithm. Deployments
// must bind it to a reviewed cryptographic implementation (for example an HSM
// mechanism or a vetted library implementing HMAC-SHA-256/GMAC). The provider
// computes a tag over both spans in order, without concatenating or allocating.
class IMacProvider {
public:
    virtual ~IMacProvider() = default;

    [[nodiscard]] virtual bool compute_tag(
        std::span<const std::byte> session_key,
        std::span<const std::byte> canonical_sobp_frame,
        std::span<const std::byte> associated_data,
        std::span<std::byte> output_tag) noexcept = 0;
};

enum class AuthError : std::uint8_t {
    None,
    MissingContext,
    MissingVerifiedLogon,
    InvalidRole,
    InvalidTagSize,
    MissingKey,
    KeyRetiredOrRotated,
    KeyProviderUnavailable,
    InvalidLogonProof,
    MacProviderFailure,
    OutputTooSmall,
    EnvelopeTooShort,
    WrongSession,
    WrongEpoch,
    WrongKeyId,
    WrongDirection,
    Replay,
    SequenceMismatch,
    SobpProtocolError,
    TagMismatch,
};

struct AuthEncodeResult final {
    AuthError error{AuthError::None};
    sobp_v1::ProtocolError protocol_error{sobp_v1::ProtocolError::None};
    std::size_t bytes_written{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == AuthError::None;
    }
};

struct AuthVerifyResult final {
    AuthError error{AuthError::None};
    sobp_v1::ProtocolError protocol_error{sobp_v1::ProtocolError::None};
    std::optional<sobp_v1::Message> message{};
    std::size_t authenticated_frame_size{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == AuthError::None && message.has_value();
    }
};

// A single-use attestation that the exact logon context was authenticated by
// the key provider. It is intentionally default-constructible only as an
// invalid output holder; callers cannot populate or copy it.
class VerifiedLogonCapability final {
public:
    VerifiedLogonCapability() noexcept = default;

    VerifiedLogonCapability(const VerifiedLogonCapability&) = delete;
    VerifiedLogonCapability& operator=(const VerifiedLogonCapability&) = delete;
    VerifiedLogonCapability(VerifiedLogonCapability&&) = delete;
    VerifiedLogonCapability& operator=(VerifiedLogonCapability&&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return verified_ && context_.valid() && key_provider_ != nullptr;
    }

private:
    friend class SessionAuthenticator;
    friend AuthError verify_logon_proof(
        IAuthenticationKeyProvider& key_provider,
        const AuthContext& context,
        std::span<const std::byte> canonical_logon_transcript,
        std::span<const std::byte> proof,
        VerifiedLogonCapability& capability) noexcept;

    void clear() noexcept {
        context_ = {};
        key_provider_ = nullptr;
        verified_ = false;
    }

    AuthContext context_{};
    IAuthenticationKeyProvider* key_provider_{};
    bool verified_{};
};

// Fixed, versioned domain-separation data MACed after the canonical SOBP frame.
// It binds the otherwise out-of-band epoch, direction, key ID, and tag size.
[[nodiscard]] std::array<std::byte, authentication_associated_data_size>
authentication_associated_data(
    const AuthContext& context,
    Direction direction,
    TagSize tag_size) noexcept;

[[nodiscard]] bool constant_time_equal(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept;

[[nodiscard]] AuthError verify_logon_proof(
    IAuthenticationKeyProvider& key_provider,
    const AuthContext& context,
    std::span<const std::byte> canonical_logon_transcript,
    std::span<const std::byte> proof,
    VerifiedLogonCapability& capability) noexcept;

// Owns direction-specific, caller-provider-derived session keys. Construction
// is the cold path; seal() and verify() perform no dynamic allocation. Provider
// and MAC objects must outlive this object.
//
// CRC32C remains part of the unchanged SOBP frame and detects accidental wire
// corruption. The appended MAC authenticates the frame and session metadata;
// CRC32C must never be treated as an authentication primitive.
class SessionAuthenticator final {
public:
    SessionAuthenticator(
        VerifiedLogonCapability&& verified_logon,
        sobp_v1::EndpointRole local_role,
        TagSize tag_size,
        IMacProvider& mac_provider,
        std::uint64_t initial_outbound_sequence = 1U,
        std::uint64_t initial_inbound_sequence = 1U) noexcept;
    ~SessionAuthenticator();

    SessionAuthenticator(const SessionAuthenticator&) = delete;
    SessionAuthenticator& operator=(const SessionAuthenticator&) = delete;
    SessionAuthenticator(SessionAuthenticator&&) = delete;
    SessionAuthenticator& operator=(SessionAuthenticator&&) = delete;

    [[nodiscard]] bool ready() const noexcept {
        return initialization_error_ == AuthError::None;
    }
    [[nodiscard]] AuthError initialization_error() const noexcept {
        return initialization_error_;
    }
    [[nodiscard]] const AuthContext& context() const noexcept { return context_; }
    [[nodiscard]] Direction outbound_direction() const noexcept {
        return outbound_direction_;
    }
    [[nodiscard]] Direction inbound_direction() const noexcept {
        return inbound_direction_;
    }
    [[nodiscard]] std::size_t tag_size() const noexcept { return tag_size_; }
    [[nodiscard]] std::uint64_t next_outbound_sequence() const noexcept {
        return next_outbound_sequence_;
    }
    [[nodiscard]] std::uint64_t next_inbound_sequence() const noexcept {
        return next_inbound_sequence_;
    }

    [[nodiscard]] AuthEncodeResult seal(
        const sobp_v1::Message& message,
        std::span<std::byte> destination) noexcept;

    [[nodiscard]] AuthVerifyResult verify(
        std::span<const std::byte> authenticated_frame,
        const AuthContext& presented_context,
        Direction presented_direction) noexcept;

private:
    struct DirectionKey final {
        std::array<std::byte, maximum_derived_key_size> bytes{};
        std::size_t size{};
        std::uint64_t generation{};
    };

    [[nodiscard]] AuthError derive_key(
        Direction direction,
        DirectionKey& output) noexcept;
    [[nodiscard]] bool key_current(
        Direction direction,
        const DirectionKey& key) const noexcept;
    static void clear_key(DirectionKey& key) noexcept;

    AuthContext context_;
    Direction outbound_direction_{Direction::ClientToServer};
    Direction inbound_direction_{Direction::ServerToClient};
    std::size_t tag_size_{};
    IAuthenticationKeyProvider* key_provider_{};
    IMacProvider* mac_provider_{};
    DirectionKey outbound_key_;
    DirectionKey inbound_key_;
    AuthError initialization_error_{AuthError::MissingContext};
    std::uint64_t next_outbound_sequence_{1U};
    std::uint64_t next_inbound_sequence_{1U};
    bool outbound_sequence_exhausted_{};
    bool inbound_sequence_exhausted_{};
};

}  // namespace sorx::protocol::session_auth
