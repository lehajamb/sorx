#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/appliance/device_config.hpp"
#include "sorx/protocol/sobp_v1.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> allocation_count{0U};

void record_allocation() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t requested_size) {
    record_allocation();
    if (void* const memory = std::malloc(requested_size == 0U ? 1U : requested_size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t requested_size,
    const std::size_t alignment) {
    record_allocation();
#if defined(_MSC_VER)
    if (void* const memory =
            _aligned_malloc(requested_size == 0U ? 1U : requested_size, alignment)) {
        return memory;
    }
#else
    const auto nonzero_size = requested_size == 0U ? 1U : requested_size;
    const auto rounded_size =
        ((nonzero_size + alignment - 1U) / alignment) * alignment;
    if (void* const memory = std::aligned_alloc(alignment, rounded_size)) {
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
        allocation_count.store(0U, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() { armed.store(false, std::memory_order_relaxed); }

    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocation_count.load(std::memory_order_relaxed);
    }
};

}  // namespace allocation_probe

void* operator new(const std::size_t size) {
    return allocation_probe::allocate(size);
}

void* operator new[](const std::size_t size) {
    return allocation_probe::allocate(size);
}

void operator delete(void* const memory) noexcept {
    std::free(memory);
}

void operator delete[](void* const memory) noexcept {
    std::free(memory);
}

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

void operator delete(
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}

void operator delete[](
    void* const memory,
    const std::align_val_t) noexcept {
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

namespace wire = sorx::protocol::sobp_v1;
namespace appliance = sorx::appliance;

template <typename Payload>
[[nodiscard]] wire::Message message(
    Payload payload,
    const std::uint64_t sequence,
    const std::uint64_t request_id = 0U) {
    wire::Message result;
    result.payload = std::move(payload);
    result.header.message_type = wire::message_type_of(result.payload);
    result.header.session_id = 77U;
    result.header.sequence = sequence;
    result.header.request_id = request_id;
    result.header.send_time_ns = 123'456U + sequence;
    return result;
}

[[nodiscard]] wire::LogonPayload logon_payload() {
    wire::LogonPayload value;
    value.client_instance_id = 9001U;
    value.heartbeat_interval_ms = 100U;
    assert(wire::assign_text(value.account_context, "ACCOUNT-A"));
    return value;
}

[[nodiscard]] wire::NewOrderPayload new_order(
    const std::uint64_t quantity = 25U) {
    wire::NewOrderPayload value;
    value.parent_order_id = 1001U;
    value.instrument_id = 7U;
    value.side = wire::Side::Buy;
    value.intent = wire::RoutingIntent::AggressiveIoc;
    value.quantity = quantity;
    value.limit_price = 10'050;
    assert(wire::assign_text(value.client_order_id, "CLIENT-ORDER-1"));
    return value;
}

void codec_round_trip_tests() {
    wire::CancelOrderPayload cancel;
    cancel.parent_order_id = 1001U;
    assert(wire::assign_text(cancel.client_order_id, "CLIENT-CANCEL-1"));
    wire::ReplaceOrderPayload replace;
    replace.parent_order_id = 1001U;
    replace.new_total_quantity = 30U;
    replace.new_limit_price = 10'040;
    assert(wire::assign_text(replace.client_order_id, "CLIENT-REPLACE-1"));
    wire::MassCancelPayload mass_cancel;
    mass_cancel.scope = wire::MassCancelScope::InstrumentAndVenue;
    mass_cancel.instrument_id = 7U;
    mass_cancel.venue_id = 2U;
    assert(wire::assign_text(mass_cancel.client_order_id, "CLIENT-MASS-1"));
    wire::RejectPayload reject;
    reject.rejected_request_id = 10U;
    reject.code = wire::RejectCode::RiskRejected;
    reject.rejected_type = wire::MessageType::NewOrder;
    assert(wire::assign_text(reject.detail, "notional limit"));
    wire::ExecutionPayload execution;
    execution.originating_request_id = 10U;
    execution.parent_order_id = 1001U;
    execution.child_order_id = 2001U;
    execution.venue_id = 2U;
    execution.status = wire::ExecutionStatus::PartiallyFilled;
    execution.side = wire::Side::Buy;
    execution.last_quantity = 5U;
    execution.last_price = 10'050;
    execution.cumulative_quantity = 5U;
    execution.leaves_quantity = 20U;
    assert(wire::assign_text(execution.execution_id, "EXEC-1"));
    wire::StatusPayload status;
    status.status = wire::ApplianceStatus::Ready;
    status.routing_enabled = true;
    status.last_ingress_sequence = 88U;
    status.normal_credits_available = 100U;
    status.cancel_credits_available = 20U;
    assert(wire::assign_text(status.detail, "operational"));

    std::vector<wire::Message> messages;
    messages.push_back(message(logon_payload(), 1U));
    messages.push_back(message(wire::HeartbeatPayload{1U, 2U}, 2U));
    messages.push_back(message(new_order(), 3U, 10U));
    messages.push_back(message(cancel, 4U, 11U));
    messages.push_back(message(replace, 5U, 12U));
    messages.push_back(message(mass_cancel, 6U, 13U));
    messages.push_back(message(
        wire::DurableAckPayload{10U, 50U, 60U, wire::MessageType::NewOrder},
        7U,
        10U));
    messages.push_back(message(reject, 8U, 10U));
    messages.push_back(message(execution, 9U, 10U));
    messages.push_back(message(status, 10U, 14U));

    std::size_t hot_path_allocations = 0U;
    {
        allocation_probe::Scope allocation_scope;
        for (const auto& original : messages) {
            std::array<std::byte, wire::maximum_frame_size> storage{};
            const auto encoded = wire::encode_to(original, storage);
            assert(encoded);
            assert(encoded.bytes_written == wire::header_size +
                       wire::payload_size(original.header.message_type));
            const auto frame = std::span<const std::byte>{storage}.first(
                encoded.bytes_written);
            const auto decoded = wire::decode(frame);
            assert(decoded);
            assert(decoded.message->header.message_type ==
                   original.header.message_type);
            assert(decoded.message->header.session_id == original.header.session_id);
            assert(decoded.message->header.sequence == original.header.sequence);
            assert(decoded.message->header.request_id == original.header.request_id);
            assert(decoded.message->payload == original.payload);
            assert(wire::request_fingerprint(original) ==
                   wire::request_fingerprint(
                       original.header.message_type,
                       frame.subspan(wire::header_size)));
            const auto stored_checksum =
                (static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(frame[wire::crc_offset])) << 24U) |
                (static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(frame[wire::crc_offset + 1U])) <<
                 16U) |
                (static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(frame[wire::crc_offset + 2U])) <<
                 8U) |
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(frame[wire::crc_offset + 3U]));
            assert(wire::frame_crc32c(frame) == stored_checksum);
        }
        hot_path_allocations = allocation_scope.allocations();
    }
    assert(hot_path_allocations == 0U);

    const std::array<std::byte, 9U> check{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'},
        std::byte{'5'}, std::byte{'6'}, std::byte{'7'}, std::byte{'8'},
        std::byte{'9'}};
    assert(wire::crc32c(check) == 0xE3069283U);

    auto encoded = wire::encode(messages[2]);
    assert(encoded);
    encoded.bytes.back() ^= std::byte{1U};
    assert(wire::decode(encoded.bytes).error == wire::ProtocolError::ChecksumMismatch);
    assert(wire::decode(std::span<const std::byte>{encoded.bytes}.first(63U)).error ==
           wire::ProtocolError::FrameTooShort);

    auto invalid = message(new_order(), 1U, 1U);
    std::get<wire::NewOrderPayload>(invalid.payload).side =
        static_cast<wire::Side>(99U);
    encoded = wire::encode(invalid);
    assert(encoded);
    assert(wire::decode(encoded.bytes).error == wire::ProtocolError::InvalidValue);

    std::array<std::byte, 8U> too_small{};
    assert(wire::encode_to(messages[0], too_small).error ==
           wire::ProtocolError::FrameTooShort);
}

void session_and_credit_tests() {
    wire::SessionPolicy client_policy;
    client_policy.local_role = wire::EndpointRole::Client;
    client_policy.session_id = 77U;
    client_policy.maximum_normal_credits = 8U;
    client_policy.maximum_cancel_credits = 8U;
    client_policy.request_history_capacity = 8U;
    wire::SessionPolicy server_policy = client_policy;
    server_policy.local_role = wire::EndpointRole::Server;
    wire::SessionValidator client{client_policy};
    wire::SessionValidator server{server_policy};
    const auto client_in_capacity = client.inbound_request_storage_capacity();
    const auto client_out_capacity = client.outbound_request_storage_capacity();
    const auto server_in_capacity = server.inbound_request_storage_capacity();

    std::size_t hot_path_allocations = 0U;
    {
        allocation_probe::Scope allocation_scope;
        auto client_logon = message(logon_payload(), 1U);
        assert(client.validate_outbound(client_logon).accepted());
        assert(server.validate_inbound(client_logon, 900'000'000U).accepted());
        auto server_logon = message(logon_payload(), 1U);
        server_logon.header.flags = wire::flag(wire::HeaderFlag::Response);
        server_logon.header.normal_credit_grant = 4U;
        server_logon.header.cancel_credit_grant = 2U;
        assert(server.validate_outbound(server_logon).accepted());
        assert(client.validate_inbound(server_logon, 1'000'000'000U).accepted());
        assert(client.state() == wire::SessionState::Established);
        assert(server.state() == wire::SessionState::Established);
        assert(client.local_credits() == (wire::CreditBalance{4U, 2U}));
        assert(server.peer_credits() == (wire::CreditBalance{4U, 2U}));

        auto order = message(new_order(), 2U, 101U);
        order.header.normal_credit_consumed = 1U;
        assert(client.validate_outbound(order).disposition ==
               wire::SessionDisposition::Accept);
        assert(server.validate_inbound(order, 1'010'000'000U).disposition ==
               wire::SessionDisposition::Accept);

        order.header.sequence = 3U;
        order.header.flags = wire::flag(wire::HeaderFlag::Retransmission);
        assert(client.validate_outbound(order).disposition ==
               wire::SessionDisposition::IdempotentReplay);
        assert(server.validate_inbound(order, 1'020'000'000U).disposition ==
               wire::SessionDisposition::IdempotentReplay);

        wire::CancelOrderPayload cancel;
        cancel.parent_order_id = 1001U;
        assert(wire::assign_text(cancel.client_order_id, "CANCEL-1"));
        auto cancel_message = message(cancel, 4U, 102U);
        cancel_message.header.cancel_credit_consumed = 1U;
        assert(client.validate_outbound(cancel_message).accepted());
        assert(server.validate_inbound(cancel_message, 1'030'000'000U).accepted());

        assert(client.inbound_request_storage_capacity() == client_in_capacity);
        assert(client.outbound_request_storage_capacity() == client_out_capacity);
        assert(server.inbound_request_storage_capacity() == server_in_capacity);
        assert(client.heartbeat_expired(1'301'000'001U));

        auto conflicting = message(new_order(26U), 5U, 101U);
        conflicting.header.normal_credit_consumed = 1U;
        const auto conflict = client.validate_outbound(conflicting);
        assert(!conflict.accepted());
        assert(conflict.error == wire::SessionError::DuplicateRequestConflict);
        assert(client.state() == wire::SessionState::Failed);
        hot_path_allocations = allocation_scope.allocations();
    }
    assert(hot_path_allocations == 0U);

    wire::SessionValidator exhausted_client{client_policy};
    wire::SessionValidator exhausted_server{server_policy};
    auto exhausted_client_logon = message(logon_payload(), 1U);
    assert(exhausted_client.validate_outbound(exhausted_client_logon).accepted());
    assert(exhausted_server.validate_inbound(
               exhausted_client_logon, 2'000'000'000U)
               .accepted());
    auto exhausted_server_logon = message(logon_payload(), 1U);
    exhausted_server_logon.header.flags =
        wire::flag(wire::HeaderFlag::Response);
    assert(exhausted_server.validate_outbound(exhausted_server_logon).accepted());
    assert(exhausted_client.validate_inbound(
               exhausted_server_logon, 2'000'000'001U)
               .accepted());

    auto exhausted_order = message(new_order(), 2U, 201U);
    exhausted_order.header.normal_credit_consumed = 1U;
    assert(exhausted_client.validate_outbound(exhausted_order).error ==
           wire::SessionError::NormalCreditsExhausted);
    assert(exhausted_server.validate_inbound(
               exhausted_order, 2'000'000'002U)
               .error == wire::SessionError::NormalCreditsExhausted);
    assert(exhausted_client.next_outbound_sequence() == 3U);
    assert(exhausted_server.next_inbound_sequence() == 3U);

    wire::CancelOrderPayload exhausted_cancel;
    exhausted_cancel.parent_order_id = 1001U;
    assert(wire::assign_text(exhausted_cancel.client_order_id, "CANCEL-EMPTY"));
    auto exhausted_cancel_message = message(exhausted_cancel, 3U, 202U);
    exhausted_cancel_message.header.cancel_credit_consumed = 1U;
    assert(exhausted_client.validate_outbound(exhausted_cancel_message).error ==
           wire::SessionError::CancelCreditsExhausted);
    assert(exhausted_server.validate_inbound(
               exhausted_cancel_message, 2'000'000'003U)
               .error == wire::SessionError::CancelCreditsExhausted);
    assert(exhausted_client.next_outbound_sequence() == 4U);
    assert(exhausted_server.next_inbound_sequence() == 4U);
    assert(exhausted_client.state() == wire::SessionState::Established);
    assert(exhausted_server.state() == wire::SessionState::Established);
}

[[nodiscard]] appliance::AuditMetadata audit(
    const std::string& request,
    const std::string& reason,
    const std::uint64_t time) {
    return appliance::AuditMetadata{
        "operator-a", "risk-admin", "console", request, reason, time};
}

[[nodiscard]] appliance::AuditMetadata audit_as(
    const std::string& principal,
    const std::string& request,
    const std::string& reason,
    const std::uint64_t time) {
    return appliance::AuditMetadata{
        principal, "risk-admin", "console", request, reason, time};
}

[[nodiscard]] appliance::SignedManifestReference manifest(
    const std::string& name,
    const char hash_digit) {
    return appliance::SignedManifestReference{
        "PKCS11:" + name + "-MANIFEST",
        std::string(64U, hash_digit),
        "PKCS11:" + name + "-SIGNATURE"};
}

[[nodiscard]] appliance::RiskLimitConfig risk_limits(
    const std::uint64_t multiplier = 1U) {
    appliance::RiskLimitConfig risk;
    risk.maximum_single_order_quantity = 1'000U * multiplier;
    risk.maximum_single_order_notional = 10'000'000U * multiplier;
    risk.maximum_open_quantity = 10'000U * multiplier;
    risk.maximum_open_notional = 100'000'000U * multiplier;
    risk.maximum_open_orders = static_cast<std::uint32_t>(100U * multiplier);
    risk.maximum_orders_per_second = static_cast<std::uint32_t>(1'000U * multiplier);
    return risk;
}

void seal(appliance::DeviceConfigV2& config) {
    config.signed_manifest.sha256 =
        appliance::route_critical_sha256(config);
}

[[nodiscard]] appliance::CommitPolicy commit_policy_for(
    const appliance::ConfigStore& store) {
    appliance::CommitPolicy policy;
    policy.expected_running_revision = store.running().revision;
    policy.expected_candidate_revision = store.candidate().revision;
    policy.expected_candidate_sha256 =
        appliance::device_config_sha256(store.candidate().config);
    return policy;
}

[[nodiscard]] appliance::DualAuthorizationCapability approval_for(
    const appliance::ConfigStore& store,
    const appliance::CommitPolicy& policy,
    const std::string& principal,
    const std::string& request_id,
    const std::uint64_t expires_at_monotonic_ns) {
    appliance::DualAuthorizationCapability capability;
    capability.action = appliance::AuditOperation::CommitCandidate;
    capability.running_revision = policy.expected_running_revision;
    capability.candidate_revision = policy.expected_candidate_revision;
    capability.candidate_sha256 = policy.expected_candidate_sha256;
    capability.diff_sha256 = appliance::config_diff_sha256(
        appliance::diff_device_config(
            store.running().config, store.candidate().config));
    capability.expires_at_monotonic_ns = expires_at_monotonic_ns;
    capability.approver = audit_as(
        principal, request_id, "approve exact candidate", 9U);
    capability.authenticated = true;
    return capability;
}

[[nodiscard]] appliance::DeviceConfigV2 valid_device_config() {
    appliance::DeviceConfigV2 config;
    config.device_id = "SORX-TEST-1";
    config.hostname = "sorx-test-1";
    config.deployment_mode = appliance::DeploymentMode::Production;
    config.management.interface_name = "mgmt0";
    config.management.listen_address = "192.0.2.10";
    config.management.server_certificate_reference = "PKCS11:MGMT-CERT";
    config.management.private_key_reference = "PKCS11:MGMT-KEY";
    config.management.client_ca_reference = "TPM:MGMT-CA";
    config.hardware.profile_id = "X86-TEST";
    config.hardware.realtime_cores = {2U, 3U};
    config.hardware.management_cores = {0U, 1U};
    config.hardware.data_interfaces = {"order0", "market0", "venue0"};
    config.hardware.audit_interfaces = {"audit0"};
    config.hardware.huge_pages_2mb = 128U;
    config.hardware.journal_device = "/dev/test-journal";
    config.capacities.parent_order_pool = 65'536U;
    config.capacities.child_order_pool = 262'144U;
    config.capacities.request_history_pool = 65'536U;
    config.capacities.ingress_ring_entries = 4'096U;
    config.capacities.market_data_ring_entries = 16'384U;
    config.capacities.route_command_ring_entries = 4'096U;
    config.capacities.execution_ring_entries = 4'096U;
    config.capacities.journal_ring_entries = 8'192U;
    config.time.ptp_interface = "market0";
    config.recorder.storage_path = "/var/lib/sorx/recorder";
    config.recorder.audit_interface_name = "audit0";
    config.recorder.audit_listen_address = "192.0.2.30";
    config.recorder.audit_listen_port = 9'200U;
    config.recorder.audit_require_mutual_tls = true;
    config.recorder.audit_server_certificate_reference = "PKCS11:AUDIT-CERT";
    config.recorder.audit_private_key_reference = "PKCS11:AUDIT-KEY";
    config.recorder.audit_client_ca_reference = "TPM:AUDIT-CA";
    config.recorder.retention_seconds = 900U;
    config.recorder.qualified_maximum_bytes_per_second = 500'000'000U;
    config.recorder.tier0_usable_bytes = 4'294'967'296ULL;
    config.recorder.tier0_cancel_reserved_bytes = 268'435'456U;
    config.recorder.tier0_reconciliation_reserved_bytes = 268'435'456U;
    config.recorder.maximum_storage_bytes = 1'099'511'627'776ULL;
    config.update.trust_root_reference = "TPM:UPDATE-ROOT";
    config.update.image_manifest = manifest("IMAGE", 'b');
    config.licensing.entitlement_reference = "TPM:ENTITLEMENT";
    config.signed_manifest = manifest("DEVICE", '0');

    appliance::IngressConnectorConfig ingress;
    ingress.connector_id = "sobp-a";
    ingress.listen_address = "198.51.100.10";
    ingress.listen_port = 19'000U;
    ingress.interface_name = "order0";
    ingress.credential_reference = "TPM:CLIENT-CA";

    appliance::VenueConnectorConfig venue;
    venue.venue_id = 1U;
    venue.venue_name = "VENUE-A";
    venue.connector_id = "venue-a";
    venue.connector_version = "1.0.0";
    venue.connector_type = appliance::VenueConnectorType::NativeBinary;
    venue.feed_a = {"239.0.0.1", 18'000U, "market0"};
    venue.feed_b = {"239.0.0.2", 18'001U, "market0"};
    venue.recovery = {"203.0.113.20", 18'002U, "market0"};
    venue.order_entry_host = "203.0.113.10";
    venue.order_entry_port = 19'001U;
    venue.order_entry_interface = "venue0";
    venue.quickfix_settings_reference = "/etc/sorx/quickfix.ini";
    venue.credential_reference = "TPM:VENUE-A";
    venue.dictionary_sha256 = std::string(64U, 'a');
    venue.connector_manifest = manifest("CONNECTOR-A", 'c');
    venue.supports_reconciliation = true;

    appliance::RoutingContextConfig context;
    context.context_id = "primary";
    context.account_context = "ACCOUNT-A";
    context.routing_config_path = "/etc/sorx/routes.ini";
    context.journal_path = "/var/lib/sorx/orders.journal";
    context.checkpoint_path = "/var/lib/sorx/orders.checkpoint";
    context.context_risk = risk_limits(4U);
    context.account_risk_limits.push_back(
        appliance::ScopedRiskLimitConfig{"ACCOUNT-A", risk_limits(2U)});
    context.client_risk_limits.push_back(
        appliance::ScopedRiskLimitConfig{"CLIENT-A", risk_limits()});
    context.client_risk_limits.push_back(
        appliance::ScopedRiskLimitConfig{"CLIENT-B", risk_limits()});
    context.routing_generations.policy_generation = 11U;
    context.routing_generations.fee_generation = 12U;
    context.routing_generations.metric_generation = 13U;
    context.routing_generations.weight_generation = 14U;
    context.routing_generations.manifest = manifest("ROUTING-A", 'd');
    context.ingress_connectors.push_back(std::move(ingress));
    context.venues.push_back(std::move(venue));
    config.routing_contexts.push_back(std::move(context));
    seal(config);
    return config;
}

void device_config_and_store_tests() {
    const auto initial = valid_device_config();
    assert(appliance::validate_device_config(initial).empty());
    const auto has_issue = [](const std::vector<appliance::ConfigIssue>& issues,
                              const appliance::ConfigIssueCode code,
                              const std::string_view path) {
        return std::any_of(
            issues.begin(), issues.end(), [code, path](const auto& issue) {
                return issue.code == code && issue.path == path;
            });
    };
    assert(initial.signed_manifest.sha256.size() == 64U);
    assert(initial.signed_manifest.sha256 ==
           "5277b302393f2a90decb7e73f960e9f09437cef2031107a3a6c8d7448ea68a4a");

    for (const std::string invalid_secret : {
             "password", "ENV:KEY", "env:KEY", "FILE:/tmp/key", "TPM:",
             "PKCS12:key"}) {
        auto invalid = initial;
        invalid.management.private_key_reference = invalid_secret;
        assert(!appliance::validate_device_config(invalid).empty());
    }
    auto normalized_secret = initial;
    normalized_secret.management.private_key_reference = "tPm2:MGMT-KEY";
    assert(appliance::validate_device_config(normalized_secret).empty());

    auto multiple_contexts = initial;
    multiple_contexts.routing_contexts.push_back(initial.routing_contexts.front());
    seal(multiple_contexts);
    assert(has_issue(
        appliance::validate_device_config(multiple_contexts),
        appliance::ConfigIssueCode::InvalidValue,
        "routing_contexts"));

    auto shared_audit_interface = initial;
    shared_audit_interface.recorder.audit_interface_name =
        initial.management.interface_name;
    assert(has_issue(
        appliance::validate_device_config(shared_audit_interface),
        appliance::ConfigIssueCode::ConflictingResource,
        "recorder.audit_interface_name"));
    shared_audit_interface = initial;
    shared_audit_interface.recorder.audit_interface_name =
        initial.hardware.data_interfaces.front();
    assert(has_issue(
        appliance::validate_device_config(shared_audit_interface),
        appliance::ConfigIssueCode::ConflictingResource,
        "recorder.audit_interface_name"));
    auto unknown_audit_interface = initial;
    unknown_audit_interface.recorder.audit_interface_name = "audit1";
    assert(has_issue(
        appliance::validate_device_config(unknown_audit_interface),
        appliance::ConfigIssueCode::ConflictingResource,
        "recorder.audit_interface_name"));
    auto missing_hardware_audit_interface = initial;
    missing_hardware_audit_interface.hardware.audit_interfaces.clear();
    assert(has_issue(
        appliance::validate_device_config(missing_hardware_audit_interface),
        appliance::ConfigIssueCode::MissingRequired,
        "hardware"));
    auto shared_hardware_interface = initial;
    shared_hardware_interface.hardware.audit_interfaces = {"market0"};
    assert(has_issue(
        appliance::validate_device_config(shared_hardware_interface),
        appliance::ConfigIssueCode::ConflictingResource,
        "hardware.audit_interfaces"));
    auto shared_management_interface = initial;
    shared_management_interface.management.interface_name = "market0";
    assert(has_issue(
        appliance::validate_device_config(shared_management_interface),
        appliance::ConfigIssueCode::ConflictingResource,
        "management.interface_name"));

    auto weak_audit_tls = initial;
    weak_audit_tls.recorder.audit_require_mutual_tls = false;
    assert(has_issue(
        appliance::validate_device_config(weak_audit_tls),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.audit_tls"));
    weak_audit_tls = initial;
    weak_audit_tls.recorder.audit_private_key_reference = "ENV:AUDIT_KEY";
    assert(has_issue(
        appliance::validate_device_config(weak_audit_tls),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.audit_tls"));

    auto short_retention = initial;
    short_retention.recorder.retention_seconds = 899U;
    seal(short_retention);
    assert(has_issue(
        appliance::validate_device_config(short_retention),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.retention"));
    auto underqualified_rate = initial;
    underqualified_rate.recorder.qualified_maximum_bytes_per_second =
        499'999'999U;
    seal(underqualified_rate);
    assert(has_issue(
        appliance::validate_device_config(underqualified_rate),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.retention"));
    auto undersized_tier0 = initial;
    undersized_tier0.recorder.tier0_usable_bytes = 4'294'967'295ULL;
    seal(undersized_tier0);
    assert(has_issue(
        appliance::validate_device_config(undersized_tier0),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.tier0_capacity"));
    auto missing_cancel_reserve = initial;
    missing_cancel_reserve.recorder.tier0_cancel_reserved_bytes = 0U;
    seal(missing_cancel_reserve);
    assert(has_issue(
        appliance::validate_device_config(missing_cancel_reserve),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.tier0_capacity"));
    auto missing_reconciliation_reserve = initial;
    missing_reconciliation_reserve.recorder
        .tier0_reconciliation_reserved_bytes = 0U;
    seal(missing_reconciliation_reserve);
    assert(has_issue(
        appliance::validate_device_config(missing_reconciliation_reserve),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.tier0_capacity"));
    auto exhausted_normal_lane = initial;
    exhausted_normal_lane.recorder.tier0_cancel_reserved_bytes =
        exhausted_normal_lane.recorder.tier0_usable_bytes - 1U;
    exhausted_normal_lane.recorder.tier0_reconciliation_reserved_bytes = 1U;
    seal(exhausted_normal_lane);
    assert(has_issue(
        appliance::validate_device_config(exhausted_normal_lane),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.tier0_capacity"));
    auto insufficient_storage = initial;
    insufficient_storage.recorder.maximum_storage_bytes =
        insufficient_storage.recorder.retention_seconds *
            insufficient_storage.recorder
                .qualified_maximum_bytes_per_second +
        insufficient_storage.recorder.tier0_usable_bytes - 1U;
    seal(insufficient_storage);
    assert(has_issue(
        appliance::validate_device_config(insufficient_storage),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.maximum_storage_bytes"));
    auto exact_storage_budget = initial;
    exact_storage_budget.recorder.maximum_storage_bytes =
        exact_storage_budget.recorder.retention_seconds *
            exact_storage_budget.recorder
                .qualified_maximum_bytes_per_second +
        exact_storage_budget.recorder.tier0_usable_bytes;
    seal(exact_storage_budget);
    assert(appliance::validate_device_config(exact_storage_budget).empty());
    auto overflowing_storage_budget = initial;
    overflowing_storage_budget.recorder.retention_seconds =
        std::numeric_limits<std::uint64_t>::max();
    overflowing_storage_budget.recorder.qualified_maximum_bytes_per_second =
        std::numeric_limits<std::uint64_t>::max();
    overflowing_storage_budget.recorder.maximum_storage_bytes =
        std::numeric_limits<std::uint64_t>::max();
    seal(overflowing_storage_budget);
    assert(has_issue(
        appliance::validate_device_config(overflowing_storage_budget),
        appliance::ConfigIssueCode::InvalidValue,
        "recorder.maximum_storage_bytes"));

    const auto route_hash = appliance::route_critical_sha256(initial);
    const auto route_hash_changes = [&route_hash](
                                        appliance::DeviceConfigV2 changed) {
        return appliance::route_critical_sha256(changed) != route_hash;
    };
    const auto initial_config_hash = appliance::device_config_sha256(initial);
    const auto assert_recorder_maintenance = [&initial, &initial_config_hash](
                                                 appliance::DeviceConfigV2 changed) {
        assert(appliance::device_config_sha256(changed) != initial_config_hash);
        const auto diff = appliance::diff_device_config(initial, changed);
        assert(diff.overall == appliance::ChangeClass::MaintenanceReboot);
        assert(diff.changes.size() == 1U);
        assert(diff.changes.front().path == "recorder");
        assert(appliance::config_diff_sha256(diff) !=
               appliance::config_diff_sha256({}));
    };
    auto recorder_route_change = initial;
    recorder_route_change.recorder.enabled = false;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    recorder_route_change.recorder.storage_path += "-next";
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    ++recorder_route_change.recorder.retention_seconds;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    ++recorder_route_change.recorder.qualified_maximum_bytes_per_second;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    ++recorder_route_change.recorder.tier0_usable_bytes;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    ++recorder_route_change.recorder.tier0_cancel_reserved_bytes;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    ++recorder_route_change.recorder.tier0_reconciliation_reserved_bytes;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);
    recorder_route_change = initial;
    ++recorder_route_change.recorder.maximum_storage_bytes;
    assert(route_hash_changes(recorder_route_change));
    assert_recorder_maintenance(recorder_route_change);

    auto audit_lifecycle_change = initial;
    audit_lifecycle_change.recorder.audit_interface_name = "audit1";
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    audit_lifecycle_change = initial;
    audit_lifecycle_change.recorder.audit_listen_address = "192.0.2.31";
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    audit_lifecycle_change = initial;
    ++audit_lifecycle_change.recorder.audit_listen_port;
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    audit_lifecycle_change = initial;
    audit_lifecycle_change.recorder.audit_require_mutual_tls = false;
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    audit_lifecycle_change = initial;
    audit_lifecycle_change.recorder.audit_server_certificate_reference =
        "PKCS11:AUDIT-CERT-NEXT";
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    audit_lifecycle_change = initial;
    audit_lifecycle_change.recorder.audit_private_key_reference =
        "PKCS11:AUDIT-KEY-NEXT";
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    audit_lifecycle_change = initial;
    audit_lifecycle_change.recorder.audit_client_ca_reference =
        "TPM:AUDIT-CA-NEXT";
    assert(appliance::route_critical_sha256(audit_lifecycle_change) == route_hash);
    assert_recorder_maintenance(audit_lifecycle_change);
    auto hardware_audit_change = initial;
    hardware_audit_change.hardware.audit_interfaces.push_back("audit1");
    const auto hardware_audit_diff = appliance::diff_device_config(
        initial, hardware_audit_change);
    assert(hardware_audit_diff.overall ==
           appliance::ChangeClass::MaintenanceReboot);
    assert(hardware_audit_diff.changes.size() == 1U);
    assert(hardware_audit_diff.changes.front().path ==
           "hardware_or_capacities");
    assert(appliance::device_config_sha256(hardware_audit_change) !=
           initial_config_hash);

    auto reordered = initial;
    std::reverse(
        reordered.routing_contexts[0].client_risk_limits.begin(),
        reordered.routing_contexts[0].client_risk_limits.end());
    assert(appliance::route_critical_sha256(reordered) ==
           appliance::route_critical_sha256(initial));

    appliance::YamlDeviceConfigCodec codec;
    const auto document = codec.encode(initial);
    assert(document.starts_with("---\nformat: \"SORX_DEVICE_CONFIG_V2\""));
    const auto decoded = codec.decode(document);
    assert(decoded);
    assert(*decoded.config == initial);
    appliance::LegacyIniMigrationCodec legacy_codec;
    const auto legacy_decoded = legacy_codec.decode(legacy_codec.encode(initial));
    assert(legacy_decoded);
    assert(*legacy_decoded.config == initial);

    auto unknown_yaml = document;
    unknown_yaml += "\"unknown/field\": \"rejected\"\n";
    assert(!codec.decode(unknown_yaml));

    auto tampered = initial;
    ++tampered.routing_contexts[0].context_risk.maximum_open_orders;
    assert(!appliance::validate_device_config(tampered).empty());

    auto unsafe_ha = initial;
    unsafe_ha.ha.mode = appliance::HaMode::ActiveStandbySynchronous;
    unsafe_ha.ha.node_id = "node-a";
    unsafe_ha.ha.peer_endpoint = "192.0.2.11:20000";
    unsafe_ha.ha.witness_endpoint = "192.0.2.12:20001";
    unsafe_ha.ha.replication_interface = "venue0";
    unsafe_ha.routing_contexts[0].venues[0].supports_reconciliation = false;
    seal(unsafe_ha);
    assert(!appliance::validate_device_config(unsafe_ha).empty());

    appliance::ConfigStore kill_switch_store{
        initial, audit("kill-bootstrap", "kill-switch test bootstrap", 1U)};
    auto killed = initial;
    killed.routing_contexts[0]
        .client_risk_limits[0].limits.kill_switch_engaged = true;
    seal(killed);
    auto kill_stage = kill_switch_store.stage_candidate(
        killed, audit("kill-stage", "engage client kill switch", 2U));
    assert(kill_stage.accepted());
    appliance::CommitPolicy kill_policy = commit_policy_for(kill_switch_store);
    assert(kill_switch_store.commit_candidate(
               kill_policy,
               audit("kill-commit", "engage client kill switch", 3U))
               .accepted());
    auto released = killed;
    released.routing_contexts[0]
        .client_risk_limits[0].limits.kill_switch_engaged = false;
    seal(released);
    kill_stage = kill_switch_store.stage_candidate(
        released, audit("release-stage", "release client kill switch", 4U));
    assert(kill_stage.accepted());
    kill_policy = commit_policy_for(kill_switch_store);
    kill_policy.monotonic_now_ns = 100U;
    assert(kill_switch_store.commit_candidate(
               kill_policy,
               audit("release-denied", "release client kill switch", 5U))
               .code == appliance::ConfigStoreCode::DualAuthorizationRequired);

    appliance::ConfigStore generation_store{
        initial, audit("generation-bootstrap", "generation test bootstrap", 1U)};
    auto regressed_generation = initial;
    --regressed_generation.routing_contexts[0]
          .routing_generations.policy_generation;
    seal(regressed_generation);
    assert(generation_store.stage_candidate(
               regressed_generation,
               audit("generation-stage", "attempt stale routing policy", 2U))
               .accepted());
    appliance::CommitPolicy generation_policy =
        commit_policy_for(generation_store);
    assert(generation_store.commit_candidate(
               generation_policy,
               audit("generation-denied", "attempt stale routing policy", 3U))
               .code == appliance::ConfigStoreCode::GenerationRegression);

    appliance::ConfigStore interleaving_store{
        initial, audit("interleave-bootstrap", "interleaving test", 1U)};
    auto first_candidate = initial;
    first_candidate.telemetry.snmp_enabled = true;
    auto interleaved_stage = interleaving_store.stage_candidate(
        first_candidate,
        audit("interleave-stage-a", "stage first candidate", 2U));
    assert(interleaved_stage.accepted());
    const auto stale_policy = commit_policy_for(interleaving_store);
    auto second_candidate = initial;
    second_candidate.telemetry.vendor_upload_enabled = true;
    interleaved_stage = interleaving_store.stage_candidate(
        second_candidate,
        audit("interleave-stage-b", "replace staged candidate", 3U));
    assert(interleaved_stage.accepted());
    assert(interleaving_store.commit_candidate(
               stale_policy,
               audit("interleave-commit-a", "commit inspected candidate", 4U))
               .code ==
           appliance::ConfigStoreCode::CandidateRevisionConflict);
    auto wrong_hash_policy = commit_policy_for(interleaving_store);
    wrong_hash_policy.expected_candidate_sha256[0] =
        wrong_hash_policy.expected_candidate_sha256[0] == '0' ? '1' : '0';
    assert(interleaving_store.commit_candidate(
               wrong_hash_policy,
               audit("interleave-hash", "reject substituted hash", 5U))
               .code == appliance::ConfigStoreCode::CandidateHashMismatch);
    assert(interleaving_store.running().config == initial);

    appliance::ConfigStore replay_store{
        initial, audit("approval-bootstrap", "approval replay test", 1U)};
    auto first_increase = initial;
    ++first_increase.routing_contexts[0]
          .client_risk_limits[0].limits.maximum_open_orders;
    seal(first_increase);
    assert(replay_store.stage_candidate(
               first_increase,
               audit("approval-stage-1", "first approved increase", 2U))
               .accepted());
    auto replay_policy = commit_policy_for(replay_store);
    replay_policy.monotonic_now_ns = 100U;
    replay_policy.secondary_authorization = approval_for(
        replay_store, replay_policy, "operator-b", "approval-reused", 500U);
    const auto approval_document =
        appliance::canonical_dual_authorization_document(
            *replay_policy.secondary_authorization);
    auto rebound = *replay_policy.secondary_authorization;
    ++rebound.candidate_revision;
    assert(appliance::canonical_dual_authorization_document(rebound) !=
           approval_document);
    assert(replay_store.commit_candidate(
               replay_policy,
               audit("approval-commit-1", "consume approval", 3U))
               .accepted());
    auto second_increase = first_increase;
    ++second_increase.routing_contexts[0]
          .client_risk_limits[0].limits.maximum_open_orders;
    seal(second_increase);
    assert(replay_store.stage_candidate(
               second_increase,
               audit("approval-stage-2", "second approved increase", 4U))
               .accepted());
    replay_policy = commit_policy_for(replay_store);
    replay_policy.monotonic_now_ns = 200U;
    replay_policy.secondary_authorization = approval_for(
        replay_store, replay_policy, "operator-b", "approval-reused", 500U);
    assert(replay_store.commit_candidate(
               replay_policy,
               audit("approval-commit-2", "reject replayed approval", 5U))
               .code == appliance::ConfigStoreCode::AuthorizationReplay);

    appliance::ConfigStore management_store{
        initial, audit("management-bootstrap", "commit-confirm test", 1U)};
    auto management_change = initial;
    management_change.management.listen_address = "192.0.2.20";
    seal(management_change);
    assert(management_store.stage_candidate(
               management_change,
               audit("management-stage", "move management address", 2U))
               .accepted());
    auto management_policy = commit_policy_for(management_store);
    management_policy.maintenance_reboot_authorized = true;
    assert(management_store.commit_candidate(
               management_policy,
               audit("management-no-confirm", "reject unsafe management move", 3U))
               .code == appliance::ConfigStoreCode::InvalidCommitPolicy);
    assert(management_store.running().config == initial);
    management_policy = commit_policy_for(management_store);
    management_policy.maintenance_reboot_authorized = true;
    management_policy.monotonic_now_ns = 1'000U;
    management_policy.confirmation_timeout_ns = 100U;
    const auto management_commit = management_store.commit_candidate(
        management_policy,
        audit("management-confirmed", "move management address safely", 4U));
    assert(management_commit.accepted());
    assert(management_store.pending_commit().has_value());
    assert(management_store.rollback_unconfirmed(1'100U, {}).code ==
           appliance::ConfigStoreCode::InvalidAuditMetadata);
    assert(management_store.pending_commit().has_value());
    const auto fail_safe_rollback =
        management_store.rollback_unconfirmed(1'101U, {});
    assert(fail_safe_rollback.accepted());
    assert(!management_store.pending_commit().has_value());
    assert(management_store.running().config == initial);
    const auto& rollback_audit = management_store.audit_log().back();
    assert(rollback_audit.accepted);
    assert(rollback_audit.operation == appliance::AuditOperation::AutomaticRollback);
    assert(rollback_audit.metadata.principal == "sorx-watchdog");
    assert(rollback_audit.metadata.role == "system");
    assert(rollback_audit.detail.find("invalid caller audit metadata") !=
           std::string::npos);

    appliance::ConfigStore store{
        initial, audit("bootstrap-1", "factory provisioning", 1U)};
    assert(store.running().revision == 1U);
    assert(store.startup().revision == 1U);

    auto telemetry = initial;
    telemetry.telemetry.snmp_enabled = true;
    auto staged = store.stage_candidate(
        telemetry, audit("stage-2", "enable NOC telemetry", 2U));
    assert(staged.accepted());
    assert(staged.diff.overall == appliance::ChangeClass::LiveAtomic);
    appliance::CommitPolicy policy = commit_policy_for(store);
    auto committed = store.commit_candidate(
        policy, audit("commit-2", "enable NOC telemetry", 3U));
    assert(committed.accepted());
    assert(store.running().revision == staged.revision);
    assert(store.startup().revision == 1U);

    auto venue_change = telemetry;
    venue_change.routing_contexts[0].venues[0].order_entry_host =
        "203.0.113.11";
    seal(venue_change);
    staged = store.stage_candidate(
        venue_change, audit("stage-3", "certified endpoint migration", 4U));
    assert(staged.accepted());
    assert(staged.diff.overall ==
           appliance::ChangeClass::DrainReconcileRestart);
    const auto current_revision = store.running().revision;
    policy = commit_policy_for(store);
    assert(policy.expected_running_revision == current_revision);
    auto denied = store.commit_candidate(
        policy,
        audit("commit-3a", "certified endpoint migration", 5U));
    assert(!denied.accepted());
    assert(denied.code ==
           appliance::ConfigStoreCode::ChangeRequiresDrainReconcileRestart);
    policy.drain_reconcile_restart_completed = true;
    policy.promote_to_startup = true;
    committed = store.commit_candidate(
        policy,
        audit("commit-3b", "venue drained and reconciled", 6U));
    assert(committed.accepted());
    assert(store.startup().revision == store.running().revision);

    auto risk_increase = venue_change;
    ++risk_increase.routing_contexts[0]
          .client_risk_limits[0].limits.maximum_open_orders;
    seal(risk_increase);
    staged = store.stage_candidate(
        risk_increase, audit("stage-4", "raise client open-order cap", 7U));
    assert(staged.accepted());
    assert(staged.diff.overall == appliance::ChangeClass::DualAuthorized);
    policy = commit_policy_for(store);
    policy.monotonic_now_ns = 1'000U;
    denied = store.commit_candidate(
        policy, audit("commit-4a", "raise client open-order cap", 8U));
    assert(denied.code == appliance::ConfigStoreCode::DualAuthorizationRequired);
    policy.secondary_authorization = approval_for(
        store, policy, "operator-a", "approval-4a", 2'000U);
    denied = store.commit_candidate(
        policy, audit("commit-4b", "raise client open-order cap", 10U));
    assert(denied.code == appliance::ConfigStoreCode::DualAuthorizationRequired);
    policy.secondary_authorization = approval_for(
        store, policy, "operator-b", "approval-4-diff", 2'000U);
    policy.secondary_authorization->diff_sha256[0] =
        policy.secondary_authorization->diff_sha256[0] == '0' ? '1' : '0';
    denied = store.commit_candidate(
        policy, audit("commit-4-diff", "reject substituted diff", 10U));
    assert(denied.code == appliance::ConfigStoreCode::DualAuthorizationRequired);
    policy.secondary_authorization = approval_for(
        store, policy, "operator-b", "approval-4-expired", 999U);
    denied = store.commit_candidate(
        policy, audit("commit-4-expired", "reject expired approval", 10U));
    assert(denied.code == appliance::ConfigStoreCode::AuthorizationExpired);
    policy.secondary_authorization = approval_for(
        store, policy, "operator-b", "approval-4b", 2'000U);
    policy.confirmation_timeout_ns = 100U;
    policy.monotonic_now_ns = 1'000U;
    policy.promote_to_startup = true;
    const auto startup_before_pending = store.startup().revision;
    committed = store.commit_candidate(
        policy, audit("commit-4c", "raise client open-order cap", 12U));
    assert(committed.accepted());
    assert(store.pending_commit().has_value());
    assert(store.pending_commit()->revision == committed.revision);
    assert(store.startup().revision == startup_before_pending);
    assert(store.stage_candidate(
               venue_change,
               audit("stage-blocked", "must wait for confirmation", 13U))
               .code == appliance::ConfigStoreCode::PendingConfirmationExists);
    auto expiration = store.rollback_unconfirmed(
        1'100U, audit("expire-4a", "watchdog check", 14U));
    assert(expiration.code == appliance::ConfigStoreCode::ConfirmationNotExpired);
    const auto late_confirmation = store.confirm_commit(
        committed.revision, 1'101U,
        audit("confirm-4-late", "late health confirmation", 15U));
    assert(late_confirmation.code == appliance::ConfigStoreCode::ConfirmationExpired);
    expiration = store.rollback_unconfirmed(
        1'101U, audit("expire-4b", "watchdog automatic rollback", 16U));
    assert(expiration.accepted());
    assert(!store.pending_commit().has_value());
    assert(store.running().config == venue_change);
    assert(store.startup().revision == startup_before_pending);

    auto confirmed_config = venue_change;
    confirmed_config.telemetry.vendor_upload_enabled = true;
    staged = store.stage_candidate(
        confirmed_config, audit("stage-5", "enable support telemetry", 17U));
    assert(staged.accepted());
    policy = commit_policy_for(store);
    policy.confirmation_timeout_ns = 100U;
    policy.monotonic_now_ns = 2'000U;
    policy.promote_to_startup = true;
    committed = store.commit_candidate(
        policy, audit("commit-5", "enable support telemetry", 18U));
    assert(committed.accepted());
    const auto confirmed = store.confirm_commit(
        committed.revision, 2'050U,
        audit("confirm-5", "health probes passed", 19U));
    assert(confirmed.accepted());
    assert(!store.pending_commit().has_value());
    assert(store.startup().revision == store.running().revision);

    const auto before_rollback = store.running().revision;
    policy = {};
    policy.expected_running_revision = before_rollback;
    policy.expected_candidate_revision = 1U;
    policy.expected_candidate_sha256 = appliance::device_config_sha256(initial);
    policy.drain_reconcile_restart_completed = true;
    policy.promote_to_startup = true;
    const auto rolled_back = store.rollback(
        1U, policy,
        audit("rollback-1", "restore known-good endpoint", 20U));
    assert(rolled_back.accepted());
    assert(store.running().revision > before_rollback);
    assert(store.running().config == initial);
    assert(!store.audit_log().empty());
    assert(store.audit_log().back().metadata.reason ==
           "restore known-good endpoint");
}

}  // namespace

int main() {
    codec_round_trip_tests();
    session_and_credit_tests();
    device_config_and_store_tests();
    return 0;
}
