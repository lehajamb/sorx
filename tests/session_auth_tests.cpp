#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/protocol/session_auth.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <utility>

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

[[nodiscard]] void* allocate(const std::size_t requested) {
    record();
    if (void* const memory = std::malloc(requested == 0U ? 1U : requested)) {
        return memory;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t requested,
    const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* const memory =
            _aligned_malloc(requested == 0U ? 1U : requested, alignment)) {
        return memory;
    }
#else
    const auto size = requested == 0U ? 1U : requested;
    const auto rounded = ((size + alignment - 1U) / alignment) * alignment;
    if (void* const memory = std::aligned_alloc(alignment, rounded)) {
        return memory;
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
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { armed.store(false, std::memory_order_relaxed); }

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

void operator delete(void* const memory, const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](void* const memory, const std::align_val_t) noexcept {
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

namespace auth = sorx::protocol::session_auth;
namespace wire = sorx::protocol::sobp_v1;

// Deterministic fixtures only. Neither class below is production cryptography.
class TestOnlyMacProvider final : public auth::IMacProvider {
public:
    [[nodiscard]] bool compute_tag(
        const std::span<const std::byte> session_key,
        const std::span<const std::byte> canonical_sobp_frame,
        const std::span<const std::byte> associated_data,
        const std::span<std::byte> output_tag) noexcept override {
        if (fail_) return false;
        std::array<std::uint64_t, 4U> state{
            0x243F6A8885A308D3ULL,
            0x13198A2E03707344ULL,
            0xA4093822299F31D0ULL,
            0x082EFA98EC4E6C89ULL};
        std::size_t cursor = 0U;
        const auto absorb = [&state, &cursor](
            const std::span<const std::byte> input) {
            for (const auto byte : input) {
                const auto lane = cursor & 3U;
                const auto next = (lane + 1U) & 3U;
                state[lane] ^= static_cast<std::uint64_t>(
                    std::to_integer<std::uint8_t>(byte)) + cursor;
                state[lane] *= 0x100000001B3ULL;
                state[next] ^= std::rotl(state[lane],
                                         static_cast<int>((cursor % 61U) + 1U));
                ++cursor;
            }
        };
        absorb(session_key);
        absorb(canonical_sobp_frame);
        absorb(associated_data);
        for (std::size_t index = 0U; index < output_tag.size(); ++index) {
            const auto lane = index & 3U;
            state[lane] ^= std::rotl(
                state[(lane + 1U) & 3U],
                static_cast<int>((index % 47U) + 1U));
            state[lane] *= 0x9E3779B185EBCA87ULL;
            output_tag[index] = static_cast<std::byte>(
                state[lane] >> ((index & 7U) * 8U));
        }
        return true;
    }

    void set_failure(const bool value) noexcept { fail_ = value; }

private:
    bool fail_{};
};

class TestOnlyKeyProvider final : public auth::IAuthenticationKeyProvider {
public:
    explicit TestOnlyKeyProvider(const std::uint64_t active_key_id) noexcept
        : active_key_id_(active_key_id) {}

    [[nodiscard]] auth::AuthenticationKeyStatus verify_logon_proof(
        const auth::AuthContext& context,
        const std::span<const std::byte> canonical_logon_transcript,
        const std::span<const std::byte> proof) noexcept override {
        if (!available_) {
            return auth::AuthenticationKeyStatus::BackendUnavailable;
        }
        if (context.key_id != active_key_id_) {
            return auth::AuthenticationKeyStatus::MissingKey;
        }
        if (retired_) {
            return auth::AuthenticationKeyStatus::RetiredOrRotated;
        }
        std::array<std::byte, 16U> expected{};
        make_proof(context, canonical_logon_transcript, expected);
        return auth::constant_time_equal(proof, expected) ?
            auth::AuthenticationKeyStatus::Success :
            auth::AuthenticationKeyStatus::InvalidProof;
    }

    [[nodiscard]] auth::AuthenticationKeyStatus derive_session_key(
        const auth::AuthContext& context,
        const auth::Direction direction,
        const std::span<std::byte> output,
        std::size_t& bytes_written,
        std::uint64_t& key_generation) noexcept override {
        bytes_written = 0U;
        key_generation = 0U;
        if (!available_) {
            return auth::AuthenticationKeyStatus::BackendUnavailable;
        }
        if (context.key_id != active_key_id_) {
            return auth::AuthenticationKeyStatus::MissingKey;
        }
        if (retired_) {
            return auth::AuthenticationKeyStatus::RetiredOrRotated;
        }
        if (output.size() < 32U) {
            return auth::AuthenticationKeyStatus::BackendUnavailable;
        }
        std::uint64_t state = context.session_id ^
            std::rotl(context.session_epoch, 17) ^
            std::rotl(context.key_id, 31) ^
            (static_cast<std::uint64_t>(direction) << 56U) ^
            generation_ ^ 0xD6E8FEB86659FD93ULL;
        for (std::size_t index = 0U; index < 32U; ++index) {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            state *= 0x2545F4914F6CDD1DULL;
            output[index] = static_cast<std::byte>(state >> 56U);
        }
        bytes_written = 32U;
        key_generation = generation_;
        return auth::AuthenticationKeyStatus::Success;
    }

    [[nodiscard]] bool is_key_current(
        const auth::AuthContext& context,
        const auth::Direction,
        const std::uint64_t key_generation) const noexcept override {
        return available_ && !retired_ && context.key_id == active_key_id_ &&
            key_generation == generation_;
    }

    void make_proof(
        const auth::AuthContext& context,
        const std::span<const std::byte> transcript,
        const std::span<std::byte> output) const noexcept {
        assert(output.size() == 16U);
        std::array<std::uint8_t, 16U> state{};
        for (std::size_t index = 0U; index < state.size(); ++index) {
            state[index] = static_cast<std::uint8_t>(
                (context.session_id >> ((index & 7U) * 8U)) ^
                (context.session_epoch >> (((index + 3U) & 7U) * 8U)) ^
                (context.key_id >> (((index + 5U) & 7U) * 8U)) ^
                generation_ ^ index);
        }
        for (std::size_t index = 0U; index < transcript.size(); ++index) {
            const auto lane = index & 15U;
            state[lane] = static_cast<std::uint8_t>(
                std::rotl(
                    static_cast<std::uint32_t>(state[lane] ^
                        std::to_integer<std::uint8_t>(transcript[index])),
                    static_cast<int>((index % 7U) + 1U)) & 0xFFU);
        }
        for (std::size_t index = 0U; index < output.size(); ++index) {
            output[index] = static_cast<std::byte>(state[index]);
        }
    }

    void rotate_generation() noexcept { ++generation_; }
    void retire() noexcept { retired_ = true; }
    void set_available(const bool value) noexcept { available_ = value; }

private:
    std::uint64_t active_key_id_{};
    std::uint64_t generation_{1U};
    bool retired_{};
    bool available_{true};
};

[[nodiscard]] wire::Message heartbeat(
    const std::uint64_t session_id,
    const std::uint64_t sequence) {
    wire::Message message;
    message.header.message_type = wire::MessageType::Heartbeat;
    message.header.session_id = session_id;
    message.header.sequence = sequence;
    message.header.send_time_ns = 1'000U + sequence;
    message.payload = wire::HeartbeatPayload{sequence - 1U, sequence};
    return message;
}

void authorize_session(
    TestOnlyKeyProvider& keys,
    const auth::AuthContext& context,
    auth::VerifiedLogonCapability& capability) {
    const std::array<std::byte, 8U> transcript{
        std::byte{0x53U}, std::byte{0x4fU}, std::byte{0x52U}, std::byte{0x58U},
        std::byte{0x4cU}, std::byte{0x4fU}, std::byte{0x47U}, std::byte{0x4eU}};
    std::array<std::byte, 16U> proof{};
    keys.make_proof(context, transcript, proof);
    assert(auth::verify_logon_proof(
               keys, context, transcript, proof, capability) ==
           auth::AuthError::None);
    assert(capability.valid());
}

void logon_proof_tests() {
    TestOnlyKeyProvider keys{41U};
    const auth::AuthContext context{77U, 9U, 41U};
    const std::array<std::byte, 8U> transcript{
        std::byte{1U}, std::byte{2U}, std::byte{3U}, std::byte{4U},
        std::byte{5U}, std::byte{6U}, std::byte{7U}, std::byte{8U}};
    std::array<std::byte, 16U> proof{};
    auth::VerifiedLogonCapability capability;
    keys.make_proof(context, transcript, proof);
    assert(auth::verify_logon_proof(
               keys, context, transcript, proof, capability) ==
           auth::AuthError::None);
    assert(capability.valid());
    proof[4U] ^= std::byte{1U};
    assert(auth::verify_logon_proof(
               keys, context, transcript, proof, capability) ==
           auth::AuthError::InvalidLogonProof);
    assert(!capability.valid());
    assert(auth::verify_logon_proof(
               keys, context, transcript, std::span<const std::byte>{},
               capability) ==
           auth::AuthError::InvalidLogonProof);
    assert(auth::verify_logon_proof(
               keys, auth::AuthContext{}, transcript, proof, capability) ==
           auth::AuthError::MissingContext);
    auto missing_key = context;
    ++missing_key.key_id;
    assert(auth::verify_logon_proof(
               keys, missing_key, transcript, proof, capability) ==
           auth::AuthError::MissingKey);
    keys.set_available(false);
    assert(auth::verify_logon_proof(
               keys, context, transcript, proof, capability) ==
           auth::AuthError::KeyProviderUnavailable);
}

void authentication_and_replay_tests() {
    TestOnlyKeyProvider keys{41U};
    TestOnlyMacProvider mac;
    const auth::AuthContext context{77U, 9U, 41U};
    auth::VerifiedLogonCapability client_logon;
    auth::VerifiedLogonCapability server_logon;
    authorize_session(keys, context, client_logon);
    authorize_session(keys, context, server_logon);
    auth::SessionAuthenticator client{
        std::move(client_logon), wire::EndpointRole::Client,
        auth::TagSize::Bytes16, mac};
    auth::SessionAuthenticator server{
        std::move(server_logon), wire::EndpointRole::Server,
        auth::TagSize::Bytes16, mac};
    assert(client.ready());
    assert(server.ready());
    assert(!client_logon.valid());
    assert(!server_logon.valid());
    assert(client.outbound_direction() == auth::Direction::ClientToServer);
    assert(server.inbound_direction() == auth::Direction::ClientToServer);

    auth::VerifiedLogonCapability unverified_logon;
    auth::SessionAuthenticator unverified{
        std::move(unverified_logon), wire::EndpointRole::Client,
        auth::TagSize::Bytes16, mac};
    assert(!unverified.ready());
    assert(unverified.initialization_error() ==
           auth::AuthError::MissingVerifiedLogon);
    auth::SessionAuthenticator consumed_capability{
        std::move(client_logon), wire::EndpointRole::Client,
        auth::TagSize::Bytes16, mac};
    assert(consumed_capability.initialization_error() ==
           auth::AuthError::MissingVerifiedLogon);
    auth::VerifiedLogonCapability invalid_role_logon;
    authorize_session(keys, context, invalid_role_logon);
    auth::SessionAuthenticator invalid_role{
        std::move(invalid_role_logon),
        static_cast<wire::EndpointRole>(99U),
        auth::TagSize::Bytes16,
        mac};
    assert(invalid_role.initialization_error() == auth::AuthError::InvalidRole);
    auth::VerifiedLogonCapability invalid_tag_logon;
    authorize_session(keys, context, invalid_tag_logon);
    auth::SessionAuthenticator invalid_tag{
        std::move(invalid_tag_logon),
        wire::EndpointRole::Client,
        static_cast<auth::TagSize>(17U),
        mac};
    assert(invalid_tag.initialization_error() ==
           auth::AuthError::InvalidTagSize);

    std::array<std::byte, auth::maximum_authenticated_frame_size> storage{};
    auto encoded = client.seal(heartbeat(context.session_id, 1U), storage);
    assert(encoded);
    assert(encoded.bytes_written ==
           wire::header_size + wire::payload_size(wire::MessageType::Heartbeat) +
               16U);
    const auto envelope = std::span<const std::byte>{storage}.first(
        encoded.bytes_written);
    assert(wire::decode(envelope.first(envelope.size() - 16U)));
    auto verified = server.verify(
        envelope, context, auth::Direction::ClientToServer);
    assert(verified);
    assert(verified.message->header.sequence == 1U);
    assert(server.verify(
               envelope, context, auth::Direction::ClientToServer)
               .error == auth::AuthError::Replay);

    encoded = client.seal(heartbeat(context.session_id, 2U), storage);
    assert(encoded);
    auto tampered = storage;
    tampered[encoded.bytes_written - 1U] ^= std::byte{1U};
    assert(server.verify(
               std::span<const std::byte>{tampered}.first(encoded.bytes_written),
               context,
               auth::Direction::ClientToServer)
               .error == auth::AuthError::TagMismatch);
    verified = server.verify(
        std::span<const std::byte>{storage}.first(encoded.bytes_written),
        context,
        auth::Direction::ClientToServer);
    assert(verified);

    encoded = client.seal(heartbeat(context.session_id, 3U), storage);
    assert(encoded);
    auto wrong = context;
    ++wrong.session_id;
    assert(server.verify(
               std::span<const std::byte>{storage}.first(encoded.bytes_written),
               wrong,
               auth::Direction::ClientToServer)
               .error == auth::AuthError::WrongSession);
    wrong = context;
    ++wrong.session_epoch;
    assert(server.verify(
               std::span<const std::byte>{storage}.first(encoded.bytes_written),
               wrong,
               auth::Direction::ClientToServer)
               .error == auth::AuthError::WrongEpoch);
    wrong = context;
    ++wrong.key_id;
    assert(server.verify(
               std::span<const std::byte>{storage}.first(encoded.bytes_written),
               wrong,
               auth::Direction::ClientToServer)
               .error == auth::AuthError::WrongKeyId);
    assert(server.verify(
               std::span<const std::byte>{storage}.first(encoded.bytes_written),
               context,
               auth::Direction::ServerToClient)
               .error == auth::AuthError::WrongDirection);
    assert(server.verify(
        std::span<const std::byte>{storage}.first(encoded.bytes_written),
        context,
        auth::Direction::ClientToServer));

    assert(client.seal(heartbeat(context.session_id + 1U, 4U), storage).error ==
           auth::AuthError::WrongSession);
    assert(client.seal(heartbeat(context.session_id, 5U), storage).error ==
           auth::AuthError::SequenceMismatch);
    encoded = client.seal(heartbeat(context.session_id, 4U), storage);
    assert(encoded);
    assert(server.verify(
        std::span<const std::byte>{storage}.first(encoded.bytes_written),
        context,
        auth::Direction::ClientToServer));

    encoded = client.seal(heartbeat(context.session_id, 5U), storage);
    assert(encoded);
    keys.rotate_generation();
    assert(server.verify(
               std::span<const std::byte>{storage}.first(encoded.bytes_written),
               context,
               auth::Direction::ClientToServer)
               .error == auth::AuthError::KeyRetiredOrRotated);
    assert(client.seal(heartbeat(context.session_id, 6U), storage).error ==
           auth::AuthError::KeyRetiredOrRotated);
}

void authenticated_credit_rejection_sequence_tests() {
    TestOnlyKeyProvider keys{52U};
    TestOnlyMacProvider mac;
    const auth::AuthContext context{701U, 3U, 52U};
    auth::VerifiedLogonCapability client_logon_capability;
    auth::VerifiedLogonCapability server_logon_capability;
    authorize_session(keys, context, client_logon_capability);
    authorize_session(keys, context, server_logon_capability);
    auth::SessionAuthenticator client_auth{
        std::move(client_logon_capability), wire::EndpointRole::Client,
        auth::TagSize::Bytes16, mac};
    auth::SessionAuthenticator server_auth{
        std::move(server_logon_capability), wire::EndpointRole::Server,
        auth::TagSize::Bytes16, mac};

    wire::SessionPolicy server_policy;
    server_policy.local_role = wire::EndpointRole::Server;
    server_policy.session_id = context.session_id;
    server_policy.maximum_normal_credits = 4U;
    server_policy.maximum_cancel_credits = 4U;
    server_policy.request_history_capacity = 4U;
    wire::SessionValidator server_session{server_policy};

    wire::Message client_logon;
    client_logon.header.message_type = wire::MessageType::Logon;
    client_logon.header.session_id = context.session_id;
    client_logon.header.sequence = 1U;
    client_logon.header.send_time_ns = 10U;
    wire::LogonPayload client_logon_payload;
    client_logon_payload.client_instance_id = 9U;
    client_logon_payload.heartbeat_interval_ms = 100U;
    assert(wire::assign_text(client_logon_payload.account_context, "ACCOUNT-A"));
    client_logon.payload = client_logon_payload;

    std::array<std::byte, auth::maximum_authenticated_frame_size> envelope{};
    auto encoded = client_auth.seal(client_logon, envelope);
    assert(encoded);
    auto verified = server_auth.verify(
        std::span<const std::byte>{envelope}.first(encoded.bytes_written),
        context,
        auth::Direction::ClientToServer);
    assert(verified);
    assert(server_session.validate_inbound(*verified.message, 100U).accepted());

    wire::Message server_logon;
    server_logon.header.message_type = wire::MessageType::Logon;
    server_logon.header.flags = wire::flag(wire::HeaderFlag::Response);
    server_logon.header.session_id = context.session_id;
    server_logon.header.sequence = 1U;
    server_logon.header.send_time_ns = 20U;
    server_logon.payload = client_logon_payload;
    assert(server_session.validate_outbound(server_logon).accepted());
    assert(server_session.state() == wire::SessionState::Established);
    assert(server_session.peer_credits().normal == 0U);

    wire::Message order;
    order.header.message_type = wire::MessageType::NewOrder;
    order.header.session_id = context.session_id;
    order.header.sequence = 2U;
    order.header.request_id = 9001U;
    order.header.send_time_ns = 30U;
    order.header.normal_credit_consumed = 1U;
    wire::NewOrderPayload order_payload;
    order_payload.parent_order_id = 1001U;
    order_payload.instrument_id = 7U;
    order_payload.side = wire::Side::Buy;
    order_payload.intent = wire::RoutingIntent::AggressiveIoc;
    order_payload.quantity = 25U;
    order_payload.limit_price = 10'050;
    assert(wire::assign_text(order_payload.client_order_id, "ORDER-1"));
    order.payload = order_payload;

    encoded = client_auth.seal(order, envelope);
    assert(encoded);
    verified = server_auth.verify(
        std::span<const std::byte>{envelope}.first(encoded.bytes_written),
        context,
        auth::Direction::ClientToServer);
    assert(verified);
    const auto rejected =
        server_session.validate_inbound(*verified.message, 200U);
    assert(rejected.disposition == wire::SessionDisposition::Reject);
    assert(rejected.error == wire::SessionError::NormalCreditsExhausted);
    assert(server_auth.next_inbound_sequence() == 3U);
    assert(server_session.next_inbound_sequence() == 3U);
    assert(server_session.state() == wire::SessionState::Established);
    assert(server_auth.verify(
               std::span<const std::byte>{envelope}.first(encoded.bytes_written),
               context,
               auth::Direction::ClientToServer)
               .error == auth::AuthError::Replay);
}

void corruption_boundary_and_tag_size_tests() {
    TestOnlyKeyProvider keys{73U};
    TestOnlyMacProvider mac;
    const auth::AuthContext context{88U, 12U, 73U};
    auth::VerifiedLogonCapability client_logon;
    auth::VerifiedLogonCapability server_logon;
    authorize_session(keys, context, client_logon);
    authorize_session(keys, context, server_logon);
    auth::SessionAuthenticator client{
        std::move(client_logon), wire::EndpointRole::Client,
        auth::TagSize::Bytes32, mac};
    auth::SessionAuthenticator server{
        std::move(server_logon), wire::EndpointRole::Server,
        auth::TagSize::Bytes32, mac};
    assert(client.ready() && server.ready());
    assert(client.tag_size() == 32U);

    std::array<std::byte, auth::maximum_authenticated_frame_size> envelope{};
    const auto encoded = client.seal(
        heartbeat(context.session_id, 1U), envelope);
    assert(encoded);
    const auto frame_size = encoded.bytes_written - 32U;
    envelope[wire::crc_offset] ^= std::byte{1U};

    std::array<std::byte, auth::maximum_derived_key_size> key{};
    std::size_t key_size = 0U;
    std::uint64_t generation = 0U;
    assert(keys.derive_session_key(
               context,
               auth::Direction::ClientToServer,
               key,
               key_size,
               generation) == auth::AuthenticationKeyStatus::Success);
    const auto associated = auth::authentication_associated_data(
        context, auth::Direction::ClientToServer, auth::TagSize::Bytes32);
    assert(mac.compute_tag(
        std::span<const std::byte>{key}.first(key_size),
        std::span<const std::byte>{envelope}.first(frame_size),
        associated,
        std::span<std::byte>{envelope}.subspan(frame_size, 32U)));

    const auto verified = server.verify(
        std::span<const std::byte>{envelope}.first(encoded.bytes_written),
        context,
        auth::Direction::ClientToServer);
    assert(verified.error == auth::AuthError::SobpProtocolError);
    assert(verified.protocol_error == wire::ProtocolError::ChecksumMismatch);

    std::array<std::byte, 16U> short_envelope{};
    assert(server.verify(
               short_envelope, context, auth::Direction::ClientToServer)
               .error == auth::AuthError::EnvelopeTooShort);
    std::array<std::byte, 8U> small_output{};
    const auth::AuthContext small_output_context{99U, 1U, 73U};
    auth::VerifiedLogonCapability small_output_logon;
    authorize_session(keys, small_output_context, small_output_logon);
    auth::SessionAuthenticator small_output_client{
        std::move(small_output_logon),
        wire::EndpointRole::Client,
        auth::TagSize::Bytes32,
        mac};
    assert(small_output_client.seal(heartbeat(99U, 1U), small_output).error ==
           auth::AuthError::OutputTooSmall);

    std::array<std::byte, 4U> equal_left{
        std::byte{1U}, std::byte{2U}, std::byte{3U}, std::byte{4U}};
    auto equal_right = equal_left;
    assert(auth::constant_time_equal(equal_left, equal_right));
    equal_right[2U] ^= std::byte{1U};
    assert(!auth::constant_time_equal(equal_left, equal_right));
}

void zero_hot_allocation_tests() {
    TestOnlyKeyProvider keys{101U};
    TestOnlyMacProvider mac;
    const auth::AuthContext context{123U, 456U, 101U};
    auth::VerifiedLogonCapability client_logon;
    auth::VerifiedLogonCapability server_logon;
    authorize_session(keys, context, client_logon);
    authorize_session(keys, context, server_logon);
    auth::SessionAuthenticator client{
        std::move(client_logon), wire::EndpointRole::Client,
        auth::TagSize::Bytes16, mac};
    auth::SessionAuthenticator server{
        std::move(server_logon), wire::EndpointRole::Server,
        auth::TagSize::Bytes16, mac};
    assert(client.ready() && server.ready());
    std::array<std::byte, auth::maximum_authenticated_frame_size> envelope{};
    std::size_t observed_allocations = 0U;
    {
        allocation_probe::Scope probe;
        for (std::uint64_t sequence = 1U; sequence <= 8U; ++sequence) {
            const auto encoded = client.seal(
                heartbeat(context.session_id, sequence), envelope);
            assert(encoded);
            const auto verified = server.verify(
                std::span<const std::byte>{envelope}.first(
                    encoded.bytes_written),
                context,
                auth::Direction::ClientToServer);
            assert(verified);
        }
        observed_allocations = probe.allocations();
    }
    assert(observed_allocations == 0U);

    TestOnlyKeyProvider failing_keys{101U};
    TestOnlyMacProvider failing_mac;
    auth::VerifiedLogonCapability failing_logon;
    authorize_session(failing_keys, context, failing_logon);
    auth::SessionAuthenticator failing_client{
        std::move(failing_logon),
        wire::EndpointRole::Client,
        auth::TagSize::Bytes16,
        failing_mac};
    failing_mac.set_failure(true);
    assert(failing_client.seal(heartbeat(context.session_id, 1U), envelope).error ==
           auth::AuthError::MacProviderFailure);
    failing_keys.retire();
    assert(failing_client.seal(heartbeat(context.session_id, 1U), envelope).error ==
           auth::AuthError::KeyRetiredOrRotated);
}

}  // namespace

int main() {
    logon_proof_tests();
    authentication_and_replay_tests();
    authenticated_credit_rejection_sequence_tests();
    corruption_boundary_and_tag_size_tests();
    zero_hot_allocation_tests();
    return 0;
}
