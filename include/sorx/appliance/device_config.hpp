#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sorx::appliance {

inline constexpr std::uint32_t device_config_schema_version = 2U;

enum class DeploymentMode : std::uint8_t {
    Development,
    Certification,
    Production,
};

enum class HaMode : std::uint8_t {
    Standalone,
    ActiveStandbySynchronous,
    ActiveStandbyAsynchronous,
};

enum class IngressProtocol : std::uint8_t {
    SobpV1,
    Fix44,
};

enum class VenueConnectorType : std::uint8_t {
    NativeBinary,
    Fix44,
};

struct SignedManifestReference final {
    std::string manifest_reference;
    std::string sha256;
    std::string signature_reference;

    bool operator==(const SignedManifestReference&) const = default;
};

struct ManagementPlaneConfig final {
    std::string interface_name;
    std::string listen_address;
    std::uint16_t listen_port{8'443U};
    bool require_mutual_tls{true};
    std::string server_certificate_reference;
    std::string private_key_reference;
    std::string client_ca_reference;
    std::string aaa_provider{"LOCAL"};
    bool remote_support_enabled{};

    bool operator==(const ManagementPlaneConfig&) const = default;
};

struct HardwareProfileConfig final {
    std::string profile_id;
    std::vector<std::uint32_t> realtime_cores;
    std::vector<std::uint32_t> management_cores;
    std::vector<std::string> data_interfaces;
    std::vector<std::string> audit_interfaces;
    std::uint32_t huge_pages_2mb{};
    std::string journal_device;

    bool operator==(const HardwareProfileConfig&) const = default;
};

struct CapacityConfig final {
    std::uint32_t parent_order_pool{};
    std::uint32_t child_order_pool{};
    std::uint32_t request_history_pool{};
    std::uint32_t ingress_ring_entries{};
    std::uint32_t market_data_ring_entries{};
    std::uint32_t route_command_ring_entries{};
    std::uint32_t execution_ring_entries{};
    std::uint32_t journal_ring_entries{};

    bool operator==(const CapacityConfig&) const = default;
};

struct TimeDisciplineConfig final {
    std::string ptp_interface;
    std::uint64_t maximum_offset_ns{1'000U};
    bool require_lock_before_routing{true};

    bool operator==(const TimeDisciplineConfig&) const = default;
};

struct HaConfig final {
    HaMode mode{HaMode::Standalone};
    std::string node_id;
    std::string peer_endpoint;
    std::string witness_endpoint;
    std::string replication_interface;
    std::uint32_t election_timeout_ms{2'000U};

    bool operator==(const HaConfig&) const = default;
};

struct TelemetryConfig final {
    std::string metrics_listen_address{"127.0.0.1"};
    std::uint16_t metrics_listen_port{9'100U};
    std::string syslog_endpoint;
    bool snmp_enabled{};
    bool vendor_upload_enabled{};

    bool operator==(const TelemetryConfig&) const = default;
};

struct RecorderConfig final {
    bool enabled{true};
    std::string storage_path;
    std::string audit_interface_name;
    std::string audit_listen_address{"127.0.0.1"};
    std::uint16_t audit_listen_port{};
    bool audit_require_mutual_tls{true};
    std::string audit_server_certificate_reference;
    std::string audit_private_key_reference;
    std::string audit_client_ca_reference;
    std::uint64_t retention_seconds{};
    std::uint64_t qualified_maximum_bytes_per_second{};
    std::uint64_t tier0_usable_bytes{};
    std::uint64_t tier0_cancel_reserved_bytes{};
    std::uint64_t tier0_reconciliation_reserved_bytes{};
    std::uint64_t maximum_storage_bytes{};

    bool operator==(const RecorderConfig&) const = default;
};

struct UpdateConfig final {
    std::string channel{"stable"};
    std::string trust_root_reference;
    SignedManifestReference image_manifest;
    bool automatic_download{};
    bool automatic_activation{};

    bool operator==(const UpdateConfig&) const = default;
};

struct LicensingConfig final {
    std::string entitlement_reference;
    std::uint32_t expiry_grace_hours{168U};

    bool operator==(const LicensingConfig&) const = default;
};

struct IngressConnectorConfig final {
    std::string connector_id;
    IngressProtocol protocol{IngressProtocol::SobpV1};
    std::string listen_address;
    std::uint16_t listen_port{};
    std::string interface_name;
    std::string credential_reference;
    std::string sender_comp_id;
    std::string target_comp_id;
    std::uint32_t normal_credit_limit{1'024U};
    std::uint32_t cancel_credit_limit{128U};

    bool operator==(const IngressConnectorConfig&) const = default;
};

struct FeedEndpointConfig final {
    std::string host;
    std::uint16_t port{};
    std::string interface_name;

    bool operator==(const FeedEndpointConfig&) const = default;
};

struct VenueConnectorConfig final {
    std::uint32_t venue_id{};
    std::string venue_name;
    std::string connector_id;
    std::string connector_version;
    VenueConnectorType connector_type{VenueConnectorType::NativeBinary};
    FeedEndpointConfig feed_a;
    FeedEndpointConfig feed_b;
    FeedEndpointConfig recovery;
    std::string order_entry_host;
    std::uint16_t order_entry_port{};
    std::string order_entry_interface;
    std::string quickfix_settings_reference;
    std::string credential_reference;
    std::string dictionary_sha256;
    SignedManifestReference connector_manifest;
    bool supports_reconciliation{};

    bool operator==(const VenueConnectorConfig&) const = default;
};

struct RiskLimitConfig final {
    std::uint64_t maximum_single_order_quantity{};
    std::uint64_t maximum_single_order_notional{};
    std::uint64_t maximum_open_quantity{};
    std::uint64_t maximum_open_notional{};
    std::uint32_t maximum_open_orders{};
    std::uint32_t maximum_orders_per_second{};
    bool kill_switch_engaged{};

    bool operator==(const RiskLimitConfig&) const = default;
};

struct ScopedRiskLimitConfig final {
    std::string scope_id;
    RiskLimitConfig limits;

    bool operator==(const ScopedRiskLimitConfig&) const = default;
};

struct RoutingGenerationConfig final {
    std::uint64_t policy_generation{};
    std::uint64_t fee_generation{};
    std::uint64_t metric_generation{};
    std::uint64_t weight_generation{};
    SignedManifestReference manifest;

    bool operator==(const RoutingGenerationConfig&) const = default;
};

struct RoutingContextConfig final {
    std::string context_id;
    std::string account_context;
    std::string routing_config_path;
    std::string journal_path;
    std::string checkpoint_path;
    RiskLimitConfig context_risk;
    std::vector<ScopedRiskLimitConfig> account_risk_limits;
    std::vector<ScopedRiskLimitConfig> client_risk_limits;
    RoutingGenerationConfig routing_generations;
    std::vector<IngressConnectorConfig> ingress_connectors;
    std::vector<VenueConnectorConfig> venues;

    bool operator==(const RoutingContextConfig&) const = default;
};

struct DeviceConfigV2 final {
    std::uint32_t schema_version{device_config_schema_version};
    std::string device_id;
    std::string hostname;
    DeploymentMode deployment_mode{DeploymentMode::Production};
    ManagementPlaneConfig management;
    HardwareProfileConfig hardware;
    CapacityConfig capacities;
    TimeDisciplineConfig time;
    HaConfig ha;
    TelemetryConfig telemetry;
    RecorderConfig recorder;
    UpdateConfig update;
    LicensingConfig licensing;
    SignedManifestReference signed_manifest;
    std::vector<RoutingContextConfig> routing_contexts;

    bool operator==(const DeviceConfigV2&) const = default;
};

enum class ConfigIssueCode : std::uint8_t {
    Syntax,
    UnknownSection,
    UnknownField,
    DuplicateSection,
    DuplicateField,
    MissingRequired,
    InvalidValue,
    DuplicateIdentity,
    ConflictingResource,
    UnsafeHaConfiguration,
};

struct ConfigIssue final {
    ConfigIssueCode code{ConfigIssueCode::InvalidValue};
    std::size_t line{};
    std::string path;
    std::string message;
};

[[nodiscard]] std::vector<ConfigIssue> validate_device_config(
    const DeviceConfigV2& config);

struct ConfigDecodeResult final {
    std::optional<DeviceConfigV2> config;
    std::vector<ConfigIssue> issues;

    [[nodiscard]] explicit operator bool() const noexcept {
        return config.has_value() && issues.empty();
    }
};

// DeviceConfigV2 and its transitively referenced types above are the normative
// runtime contract. Serialization is deliberately behind this seam. The
// shipped schemas/device_config_v2.proto is an exact wire-model mirror checked
// by tests/check_device_config_schema_contract.py; codecs keep the in-process
// model independent of a particular protobuf or RPC runtime.
class IDeviceConfigCodec {
public:
    virtual ~IDeviceConfigCodec() = default;
    [[nodiscard]] virtual ConfigDecodeResult decode(
        std::string_view document) const = 0;
    [[nodiscard]] virtual std::string encode(
        const DeviceConfigV2& config) const = 0;
};

// Offline import/export format. It uses a strict, dependency-free YAML 1.2
// subset: one deterministic flat scalar mapping, with quoted string values.
class YamlDeviceConfigCodec final : public IDeviceConfigCodec {
public:
    [[nodiscard]] ConfigDecodeResult decode(
        std::string_view document) const override;
    [[nodiscard]] std::string encode(
        const DeviceConfigV2& config) const override;
};

// Read/write support for pre-v2 provisioning files only. New deployments and
// management APIs must use YamlDeviceConfigCodec.
class LegacyIniMigrationCodec final : public IDeviceConfigCodec {
public:
    [[nodiscard]] ConfigDecodeResult decode(
        std::string_view document) const override;
    [[nodiscard]] std::string encode(
        const DeviceConfigV2& config) const override;
};

enum class ChangeClass : std::uint8_t {
    LiveAtomic,
    DualAuthorized,
    DrainReconcileRestart,
    MaintenanceReboot,
};

struct ConfigChange final {
    std::string path;
    ChangeClass classification{ChangeClass::LiveAtomic};
};

struct ConfigDiff final {
    ChangeClass overall{ChangeClass::LiveAtomic};
    std::vector<ConfigChange> changes;

    [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
};

[[nodiscard]] ConfigDiff diff_device_config(
    const DeviceConfigV2& before,
    const DeviceConfigV2& after);

// Deterministic, identity-sorted, length-delimited representation used for
// signatures and drift detection. The signed_manifest envelope is excluded to
// avoid a self-referential digest.
[[nodiscard]] std::string canonical_route_critical_document(
    const DeviceConfigV2& config);
[[nodiscard]] std::string route_critical_sha256(
    const DeviceConfigV2& config);
// Hashes the complete canonical YAML document, including management-plane
// fields and the signed-manifest envelope. Commit authorization binds this
// digest rather than the route-critical digest so an approval cannot be
// reused for a candidate with unrelated control-plane changes.
[[nodiscard]] std::string device_config_sha256(
    const DeviceConfigV2& config);

struct AuditMetadata final {
    std::string principal;
    std::string role;
    std::string source;
    std::string request_id;
    std::string reason;
    std::uint64_t timestamp_ns{};

    bool operator==(const AuditMetadata&) const = default;
};

enum class AuditOperation : std::uint8_t {
    Bootstrap,
    StageCandidate,
    CommitCandidate,
    DiscardCandidate,
    SaveStartup,
    Rollback,
    ConfirmCommit,
    AutomaticRollback,
};

// The external AAA/signature verifier attests this capability. ConfigStore
// still validates every bound field and consumes an accepted approval ID only
// once. The canonical document deliberately excludes `authenticated`, which
// is verifier state rather than signed input.
struct DualAuthorizationCapability final {
    AuditOperation action{AuditOperation::CommitCandidate};
    std::uint64_t running_revision{};
    std::uint64_t candidate_revision{};
    std::string candidate_sha256;
    std::string diff_sha256;
    std::uint64_t expires_at_monotonic_ns{};
    AuditMetadata approver;
    bool authenticated{};

    bool operator==(const DualAuthorizationCapability&) const = default;
};

[[nodiscard]] std::string config_diff_sha256(const ConfigDiff& diff);
[[nodiscard]] std::string canonical_dual_authorization_document(
    const DualAuthorizationCapability& capability);

struct AuditRecord final {
    std::uint64_t sequence{};
    AuditOperation operation{AuditOperation::Bootstrap};
    AuditMetadata metadata;
    bool accepted{};
    std::uint64_t from_revision{};
    std::uint64_t to_revision{};
    ConfigDiff diff;
    std::string detail;
    std::optional<DualAuthorizationCapability> secondary_authorization;
};

struct VersionedDeviceConfig final {
    std::uint64_t revision{};
    DeviceConfigV2 config;
};

struct CommitPolicy final {
    std::uint64_t expected_running_revision{};
    std::uint64_t expected_candidate_revision{};
    std::string expected_candidate_sha256;
    bool drain_reconcile_restart_completed{};
    bool maintenance_reboot_authorized{};
    bool promote_to_startup{};
    std::uint64_t confirmation_timeout_ns{};
    std::uint64_t monotonic_now_ns{};
    std::optional<DualAuthorizationCapability> secondary_authorization;
};

enum class ConfigStoreCode : std::uint8_t {
    Success,
    InvalidAuditMetadata,
    ValidationFailed,
    RevisionConflict,
    CandidateRevisionConflict,
    CandidateHashMismatch,
    InvalidCommitPolicy,
    GenerationRegression,
    CandidateNotStaged,
    DualAuthorizationRequired,
    AuthorizationExpired,
    AuthorizationReplay,
    ChangeRequiresDrainReconcileRestart,
    ChangeRequiresMaintenanceReboot,
    RevisionNotFound,
    PendingConfirmationExists,
    NoPendingConfirmation,
    ConfirmationRevisionMismatch,
    ConfirmationExpired,
    ConfirmationNotExpired,
};

struct ConfigStoreResult final {
    ConfigStoreCode code{ConfigStoreCode::Success};
    std::uint64_t revision{};
    ConfigDiff diff;
    std::vector<ConfigIssue> validation_issues;
    std::string detail;

    [[nodiscard]] bool accepted() const noexcept {
        return code == ConfigStoreCode::Success;
    }
};

struct PendingCommitStatus final {
    std::uint64_t revision{};
    std::uint64_t rollback_revision{};
    std::uint64_t deadline_monotonic_ns{};
    bool promote_to_startup_on_confirmation{};

    bool operator==(const PendingCommitStatus&) const = default;
};

// ConfigStore is a single-writer management-plane object. The forwarding plane
// consumes immutable VersionedDeviceConfig snapshots after an accepted commit.
class ConfigStore final {
public:
    ConfigStore(DeviceConfigV2 initial, AuditMetadata bootstrap_metadata);

    [[nodiscard]] ConfigStoreResult stage_candidate(
        DeviceConfigV2 candidate,
        AuditMetadata metadata);
    [[nodiscard]] ConfigStoreResult commit_candidate(
        CommitPolicy policy,
        AuditMetadata metadata);
    [[nodiscard]] ConfigStoreResult discard_candidate(AuditMetadata metadata);
    [[nodiscard]] ConfigStoreResult save_startup(AuditMetadata metadata);
    [[nodiscard]] ConfigStoreResult rollback(
        std::uint64_t target_revision,
        CommitPolicy policy,
        AuditMetadata metadata);
    [[nodiscard]] ConfigStoreResult confirm_commit(
        std::uint64_t revision,
        std::uint64_t monotonic_now_ns,
        AuditMetadata metadata);
    [[nodiscard]] ConfigStoreResult rollback_unconfirmed(
        std::uint64_t monotonic_now_ns,
        AuditMetadata metadata);

    [[nodiscard]] const VersionedDeviceConfig& running() const noexcept {
        return running_;
    }
    [[nodiscard]] const VersionedDeviceConfig& candidate() const noexcept {
        return candidate_;
    }
    [[nodiscard]] const VersionedDeviceConfig& startup() const noexcept {
        return startup_;
    }
    [[nodiscard]] bool candidate_dirty() const noexcept {
        return candidate_dirty_;
    }
    [[nodiscard]] const std::vector<VersionedDeviceConfig>& history() const noexcept {
        return history_;
    }
    [[nodiscard]] const std::vector<AuditRecord>& audit_log() const noexcept {
        return audit_log_;
    }
    [[nodiscard]] std::optional<PendingCommitStatus> pending_commit() const noexcept;

private:
    [[nodiscard]] ConfigStoreResult commit_version(
        VersionedDeviceConfig version,
        std::uint64_t authorization_candidate_revision,
        CommitPolicy policy,
        AuditMetadata metadata,
        AuditOperation operation);
    void append_audit(
        AuditOperation operation,
        AuditMetadata metadata,
        bool accepted,
        std::uint64_t from_revision,
        std::uint64_t to_revision,
        ConfigDiff diff,
        std::string detail,
        std::optional<DualAuthorizationCapability> secondary_authorization =
            std::nullopt);

    struct PendingCommitState final {
        PendingCommitStatus status;
        VersionedDeviceConfig rollback_version;
    };

    VersionedDeviceConfig running_;
    VersionedDeviceConfig candidate_;
    VersionedDeviceConfig startup_;
    bool candidate_dirty_{};
    std::uint64_t next_revision_{2U};
    std::uint64_t next_audit_sequence_{1U};
    std::vector<VersionedDeviceConfig> history_;
    std::vector<AuditRecord> audit_log_;
    std::optional<PendingCommitState> pending_commit_;
};

[[nodiscard]] std::string_view to_string(DeploymentMode value) noexcept;
[[nodiscard]] std::string_view to_string(HaMode value) noexcept;
[[nodiscard]] std::string_view to_string(IngressProtocol value) noexcept;
[[nodiscard]] std::string_view to_string(VenueConnectorType value) noexcept;
[[nodiscard]] std::string_view to_string(ChangeClass value) noexcept;

}  // namespace sorx::appliance
