#include "sorx/appliance/device_config.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace sorx::appliance {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] bool printable_token(
    const std::string_view value,
    const bool permit_colon = false) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [permit_colon](const char item) {
        const auto byte = static_cast<unsigned char>(item);
        return std::isalnum(byte) != 0 || item == '-' || item == '_' ||
            item == '.' || (permit_colon && (item == ':' || item == '/'));
    });
}

[[nodiscard]] bool endpoint_text(const std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](const char item) {
        const auto byte = static_cast<unsigned char>(item);
        return byte >= 0x21U && byte <= 0x7EU;
    });
}

[[nodiscard]] bool absolute_device_path(const std::string_view value) noexcept {
    return !value.empty() && value.front() == '/' && value.find("..") == value.npos;
}

[[nodiscard]] bool sha256_hex(const std::string_view value) noexcept {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](const char item) {
            return std::isxdigit(static_cast<unsigned char>(item)) != 0;
        });
}

[[nodiscard]] bool production_secret_reference(
    const std::string_view value) noexcept {
    if (!endpoint_text(value)) {
        return false;
    }
    const auto separator = value.find(':');
    if (separator == value.npos || separator == 0U ||
        separator + 1U == value.size()) {
        return false;
    }
    std::array<char, 8U> normalized{};
    if (separator >= normalized.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < separator; ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (std::isalnum(byte) == 0) {
            return false;
        }
        normalized[index] = static_cast<char>(std::toupper(byte));
    }
    const std::string_view scheme{normalized.data(), separator};
    return scheme == "TPM" || scheme == "TPM2" || scheme == "PKCS11";
}

[[nodiscard]] bool signed_manifest_valid(
    const SignedManifestReference& value) noexcept {
    return endpoint_text(value.manifest_reference) && sha256_hex(value.sha256) &&
        production_secret_reference(value.signature_reference);
}

[[nodiscard]] bool power_of_two(const std::uint32_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] bool risk_limits_valid(const RiskLimitConfig& value) noexcept {
    return value.maximum_single_order_quantity != 0U &&
        value.maximum_single_order_notional != 0U &&
        value.maximum_open_quantity >= value.maximum_single_order_quantity &&
        value.maximum_open_notional >= value.maximum_single_order_notional &&
        value.maximum_open_orders != 0U &&
        value.maximum_orders_per_second != 0U;
}

[[nodiscard]] bool risk_increased(
    const RiskLimitConfig& before,
    const RiskLimitConfig& after) noexcept {
    return after.maximum_single_order_quantity >
            before.maximum_single_order_quantity ||
        after.maximum_single_order_notional >
            before.maximum_single_order_notional ||
        after.maximum_open_quantity > before.maximum_open_quantity ||
        after.maximum_open_notional > before.maximum_open_notional ||
        after.maximum_open_orders > before.maximum_open_orders ||
        after.maximum_orders_per_second > before.maximum_orders_per_second ||
        (before.kill_switch_engaged && !after.kill_switch_engaged);
}

[[nodiscard]] bool risk_within(
    const RiskLimitConfig& child,
    const RiskLimitConfig& parent) noexcept {
    return child.maximum_single_order_quantity <=
            parent.maximum_single_order_quantity &&
        child.maximum_single_order_notional <=
            parent.maximum_single_order_notional &&
        child.maximum_open_quantity <= parent.maximum_open_quantity &&
        child.maximum_open_notional <= parent.maximum_open_notional &&
        child.maximum_open_orders <= parent.maximum_open_orders &&
        child.maximum_orders_per_second <= parent.maximum_orders_per_second;
}

void issue(
    std::vector<ConfigIssue>& issues,
    const ConfigIssueCode code,
    std::string path,
    std::string message,
    const std::size_t line = 0U) {
    issues.push_back(ConfigIssue{
        code, line, std::move(path), std::move(message)});
}

template <typename Value, typename Key>
void require_unique(
    const std::vector<Value>& values,
    Key key,
    const std::string_view path,
    std::vector<ConfigIssue>& issues) {
    using KeyType = std::decay_t<decltype(key(values.front()))>;
    std::set<KeyType> seen;
    for (const auto& value : values) {
        const auto identity = key(value);
        if (!seen.insert(identity).second) {
            issue(
                issues,
                ConfigIssueCode::DuplicateIdentity,
                std::string{path},
                "duplicate identity: " + std::to_string(identity));
        }
    }
}

template <typename Value>
void require_unique_strings(
    const std::vector<Value>& values,
    const std::string_view path,
    std::vector<ConfigIssue>& issues) {
    std::set<std::string> seen;
    for (const auto& value : values) {
        if (!seen.insert(value).second) {
            issue(
                issues,
                ConfigIssueCode::DuplicateIdentity,
                std::string{path},
                "duplicate identity: " + value);
        }
    }
}

[[nodiscard]] ChangeClass maximum(
    const ChangeClass left,
    const ChangeClass right) noexcept {
    return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ?
        left : right;
}

void add_change(
    ConfigDiff& diff,
    std::string path,
    const ChangeClass classification) {
    diff.overall = maximum(diff.overall, classification);
    diff.changes.push_back(ConfigChange{std::move(path), classification});
}

[[nodiscard]] bool valid_audit(const AuditMetadata& metadata) noexcept {
    return printable_token(metadata.principal, true) &&
        printable_token(metadata.role, true) &&
        endpoint_text(metadata.source) && endpoint_text(metadata.request_id) &&
        !trim(metadata.reason).empty() && metadata.timestamp_ns != 0U;
}

[[nodiscard]] bool risk_authorizer(const AuditMetadata& metadata) noexcept {
    return valid_audit(metadata) &&
        (metadata.role == "risk-admin" ||
         metadata.role == "senior-risk-admin" ||
         metadata.role == "security-admin" ||
         metadata.role == "change-approver");
}

[[nodiscard]] bool needs_dual_authorization(
    const ConfigDiff& diff) noexcept {
    return std::any_of(
        diff.changes.begin(), diff.changes.end(), [](const ConfigChange& change) {
            return change.classification == ChangeClass::DualAuthorized;
        });
}

[[nodiscard]] bool changes_management_plane(
    const ConfigDiff& diff) noexcept {
    return std::any_of(
        diff.changes.begin(), diff.changes.end(), [](const ConfigChange& change) {
            return change.path == "management";
        });
}

[[nodiscard]] bool routing_generation_regressed(
    const DeviceConfigV2& before,
    const DeviceConfigV2& after) noexcept {
    for (const auto& old_context : before.routing_contexts) {
        const auto replacement = std::find_if(
            after.routing_contexts.begin(), after.routing_contexts.end(),
            [&old_context](const RoutingContextConfig& value) {
                return value.context_id == old_context.context_id;
            });
        if (replacement == after.routing_contexts.end()) continue;
        const auto& old_value = old_context.routing_generations;
        const auto& new_value = replacement->routing_generations;
        if (new_value.policy_generation < old_value.policy_generation ||
            new_value.fee_generation < old_value.fee_generation ||
            new_value.metric_generation < old_value.metric_generation ||
            new_value.weight_generation < old_value.weight_generation) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ConfigStoreCode policy_error(
    const ConfigDiff& diff,
    const CommitPolicy& policy) noexcept {
    switch (diff.overall) {
        case ChangeClass::LiveAtomic:
        case ChangeClass::DualAuthorized:
            return ConfigStoreCode::Success;
        case ChangeClass::DrainReconcileRestart:
            return policy.drain_reconcile_restart_completed ?
                ConfigStoreCode::Success :
                ConfigStoreCode::ChangeRequiresDrainReconcileRestart;
        case ChangeClass::MaintenanceReboot:
            return policy.maintenance_reboot_authorized ?
                ConfigStoreCode::Success :
                ConfigStoreCode::ChangeRequiresMaintenanceReboot;
    }
    return ConfigStoreCode::ChangeRequiresMaintenanceReboot;
}

struct RawField final {
    std::string value;
    std::size_t line{};
};

struct RawSection final {
    std::string kind;
    std::vector<std::string> arguments;
    std::size_t line{};
    std::map<std::string, RawField> fields;
};

[[nodiscard]] std::vector<std::string> split_words(
    const std::string_view value) {
    std::vector<std::string> output;
    std::size_t cursor = 0U;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               std::isspace(static_cast<unsigned char>(value[cursor])) != 0) {
            ++cursor;
        }
        const auto begin = cursor;
        while (cursor < value.size() &&
               std::isspace(static_cast<unsigned char>(value[cursor])) == 0) {
            ++cursor;
        }
        if (begin != cursor) {
            output.emplace_back(value.substr(begin, cursor - begin));
        }
    }
    return output;
}

[[nodiscard]] std::vector<std::string> split_list(
    const std::string_view value) {
    std::vector<std::string> output;
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        const auto item = trim(value.substr(
            begin, end == value.npos ? value.size() - begin : end - begin));
        if (!item.empty()) {
            output.emplace_back(item);
        }
        if (end == value.npos) {
            break;
        }
        begin = end + 1U;
    }
    return output;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_unsigned(
    const std::string_view value) noexcept {
    Integer output{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::optional<bool> parse_bool(
    const std::string_view value) noexcept {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<DeploymentMode> parse_deployment(
    const std::string_view value) noexcept {
    if (value == "DEVELOPMENT") return DeploymentMode::Development;
    if (value == "CERTIFICATION") return DeploymentMode::Certification;
    if (value == "PRODUCTION") return DeploymentMode::Production;
    return std::nullopt;
}

[[nodiscard]] std::optional<HaMode> parse_ha(
    const std::string_view value) noexcept {
    if (value == "STANDALONE") return HaMode::Standalone;
    if (value == "ACTIVE_STANDBY_SYNC") {
        return HaMode::ActiveStandbySynchronous;
    }
    if (value == "ACTIVE_STANDBY_ASYNC") {
        return HaMode::ActiveStandbyAsynchronous;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<IngressProtocol> parse_ingress(
    const std::string_view value) noexcept {
    if (value == "SOBP_V1") return IngressProtocol::SobpV1;
    if (value == "FIX44") return IngressProtocol::Fix44;
    return std::nullopt;
}

[[nodiscard]] std::optional<VenueConnectorType> parse_connector_type(
    const std::string_view value) noexcept {
    if (value == "NATIVE_BINARY") return VenueConnectorType::NativeBinary;
    if (value == "FIX44") return VenueConnectorType::Fix44;
    return std::nullopt;
}

[[nodiscard]] const RawField* find_field(
    const RawSection& section,
    const std::string_view key) noexcept {
    const auto found = section.fields.find(std::string{key});
    return found == section.fields.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string required_text(
    const RawSection& section,
    const std::string_view key,
    std::vector<ConfigIssue>& issues) {
    const auto* field = find_field(section, key);
    if (field == nullptr || trim(field->value).empty()) {
        issue(
            issues,
            ConfigIssueCode::MissingRequired,
            section.kind + "." + std::string{key},
            "required field is missing",
            section.line);
        return {};
    }
    return std::string{trim(field->value)};
}

template <typename Integer>
[[nodiscard]] Integer required_integer(
    const RawSection& section,
    const std::string_view key,
    std::vector<ConfigIssue>& issues) {
    const auto text = required_text(section, key, issues);
    const auto value = parse_unsigned<Integer>(text);
    if (!text.empty() && !value.has_value()) {
        issue(
            issues,
            ConfigIssueCode::InvalidValue,
            section.kind + "." + std::string{key},
            "expected an unsigned integer",
            find_field(section, key)->line);
    }
    return value.value_or(Integer{});
}

[[nodiscard]] bool optional_bool(
    const RawSection& section,
    const std::string_view key,
    const bool fallback,
    std::vector<ConfigIssue>& issues) {
    const auto* field = find_field(section, key);
    if (field == nullptr) {
        return fallback;
    }
    const auto value = parse_bool(trim(field->value));
    if (!value.has_value()) {
        issue(
            issues,
            ConfigIssueCode::InvalidValue,
            section.kind + "." + std::string{key},
            "expected true or false",
            field->line);
        return fallback;
    }
    return *value;
}

[[nodiscard]] RiskLimitConfig parse_risk_fields(
    const RawSection& section,
    const std::string_view prefix,
    std::vector<ConfigIssue>& issues) {
    const auto key = [prefix](const std::string_view suffix) {
        return std::string{prefix} + std::string{suffix};
    };
    RiskLimitConfig limits;
    limits.maximum_single_order_quantity = required_integer<std::uint64_t>(
        section, key("maximum_single_order_quantity"), issues);
    limits.maximum_single_order_notional = required_integer<std::uint64_t>(
        section, key("maximum_single_order_notional"), issues);
    limits.maximum_open_quantity = required_integer<std::uint64_t>(
        section, key("maximum_open_quantity"), issues);
    limits.maximum_open_notional = required_integer<std::uint64_t>(
        section, key("maximum_open_notional"), issues);
    limits.maximum_open_orders = required_integer<std::uint32_t>(
        section, key("maximum_open_orders"), issues);
    limits.maximum_orders_per_second = required_integer<std::uint32_t>(
        section, key("maximum_orders_per_second"), issues);
    limits.kill_switch_engaged = optional_bool(
        section, key("kill_switch_engaged"), false, issues);
    return limits;
}

[[nodiscard]] SignedManifestReference parse_manifest_fields(
    const RawSection& section,
    const std::string_view prefix,
    std::vector<ConfigIssue>& issues) {
    const auto key = [prefix](const std::string_view suffix) {
        return std::string{prefix} + std::string{suffix};
    };
    return SignedManifestReference{
        required_text(section, key("manifest_reference"), issues),
        required_text(section, key("sha256"), issues),
        required_text(section, key("signature_reference"), issues)};
}

void reject_unknown_fields(
    const RawSection& section,
    const std::set<std::string_view>& allowed,
    std::vector<ConfigIssue>& issues) {
    for (const auto& [key, value] : section.fields) {
        if (!allowed.contains(key)) {
            issue(
                issues,
                ConfigIssueCode::UnknownField,
                section.kind + "." + key,
                "unknown field",
                value.line);
        }
    }
}

[[nodiscard]] const RawSection* singleton(
    const std::vector<RawSection>& sections,
    const std::string_view kind,
    std::vector<ConfigIssue>& issues) {
    const RawSection* found = nullptr;
    for (const auto& section : sections) {
        if (section.kind != kind) continue;
        if (found != nullptr) {
            issue(
                issues,
                ConfigIssueCode::DuplicateSection,
                std::string{kind},
                "section may appear only once",
                section.line);
        } else {
            found = &section;
        }
    }
    if (found == nullptr) {
        issue(
            issues,
            ConfigIssueCode::MissingRequired,
            std::string{kind},
            "required section is missing");
    }
    return found;
}

[[nodiscard]] std::string bool_text(const bool value) {
    return value ? "true" : "false";
}

template <typename Value>
[[nodiscard]] std::string join_values(const std::vector<Value>& values) {
    std::ostringstream output;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) output << ',';
        output << values[index];
    }
    return output.str();
}

}  // namespace

std::vector<ConfigIssue> validate_device_config(const DeviceConfigV2& config) {
    std::vector<ConfigIssue> issues;
    if (config.schema_version != device_config_schema_version) {
        issue(issues, ConfigIssueCode::InvalidValue, "device.schema_version",
              "schema_version must be 2");
    }
    if (!printable_token(config.device_id)) {
        issue(issues, ConfigIssueCode::InvalidValue, "device.device_id",
              "device_id must be a nonempty portable identifier");
    }
    if (!printable_token(config.hostname)) {
        issue(issues, ConfigIssueCode::InvalidValue, "device.hostname",
              "hostname must be a nonempty portable identifier");
    }
    if (!printable_token(config.management.interface_name) ||
        !endpoint_text(config.management.listen_address) ||
        config.management.listen_port == 0U) {
        issue(issues, ConfigIssueCode::InvalidValue, "management",
              "management interface, address, and port must be configured");
    }
    if (config.deployment_mode == DeploymentMode::Production &&
        (!config.management.require_mutual_tls ||
         !production_secret_reference(
             config.management.server_certificate_reference) ||
         !production_secret_reference(config.management.private_key_reference) ||
         !production_secret_reference(config.management.client_ca_reference))) {
        issue(issues, ConfigIssueCode::InvalidValue, "management.tls",
              "production management requires mTLS and non-environment secret references");
    }
    if (!printable_token(config.hardware.profile_id) ||
        config.hardware.realtime_cores.empty() ||
        config.hardware.management_cores.empty() ||
        config.hardware.data_interfaces.empty() ||
        (config.deployment_mode == DeploymentMode::Production &&
         config.hardware.audit_interfaces.empty()) ||
        config.hardware.huge_pages_2mb == 0U ||
        config.hardware.journal_device.empty()) {
        issue(issues, ConfigIssueCode::MissingRequired, "hardware",
              "hardware profile, core sets, data interfaces, production audit interfaces, huge pages, and journal device are required");
    }
    constexpr std::uint32_t maximum_pool_entries = 16U * 1024U * 1024U;
    constexpr std::uint32_t maximum_ring_entries = 1U * 1024U * 1024U;
    const auto pool_valid = [maximum_pool_entries](const std::uint32_t value) {
        return value != 0U && value <= maximum_pool_entries;
    };
    const auto ring_valid = [maximum_ring_entries](const std::uint32_t value) {
        return value >= 64U && value <= maximum_ring_entries && power_of_two(value);
    };
    if (!pool_valid(config.capacities.parent_order_pool) ||
        !pool_valid(config.capacities.child_order_pool) ||
        !pool_valid(config.capacities.request_history_pool) ||
        !ring_valid(config.capacities.ingress_ring_entries) ||
        !ring_valid(config.capacities.market_data_ring_entries) ||
        !ring_valid(config.capacities.route_command_ring_entries) ||
        !ring_valid(config.capacities.execution_ring_entries) ||
        !ring_valid(config.capacities.journal_ring_entries)) {
        issue(issues, ConfigIssueCode::InvalidValue, "capacities",
              "pools must be 1..16777216 entries and rings must be powers of two in 64..1048576");
    }
    require_unique_strings(config.hardware.data_interfaces, "hardware.data_interfaces", issues);
    require_unique_strings(config.hardware.audit_interfaces,
                           "hardware.audit_interfaces", issues);
    if (config.deployment_mode == DeploymentMode::Production &&
        std::find(config.hardware.data_interfaces.begin(),
                  config.hardware.data_interfaces.end(),
                  config.management.interface_name) !=
            config.hardware.data_interfaces.end()) {
        issue(issues, ConfigIssueCode::ConflictingResource,
              "management.interface_name",
              "production management interface must be distinct from dataplane interfaces");
    }
    for (const auto& audit_interface : config.hardware.audit_interfaces) {
        if (!printable_token(audit_interface) ||
            audit_interface == config.management.interface_name ||
            std::find(config.hardware.data_interfaces.begin(),
                      config.hardware.data_interfaces.end(),
                      audit_interface) != config.hardware.data_interfaces.end()) {
            issue(issues, ConfigIssueCode::ConflictingResource,
                  "hardware.audit_interfaces",
                  "audit interfaces must be portable identifiers distinct from management and dataplane interfaces");
        }
    }
    {
        std::set<std::uint32_t> cores;
        for (const auto core : config.hardware.realtime_cores) {
            if (!cores.insert(core).second) {
                issue(issues, ConfigIssueCode::DuplicateIdentity,
                      "hardware.realtime_cores", "duplicate CPU core");
            }
        }
        for (const auto core : config.hardware.management_cores) {
            if (!cores.insert(core).second) {
                issue(issues, ConfigIssueCode::ConflictingResource,
                      "hardware.management_cores",
                      "management and realtime CPU sets must not overlap");
            }
        }
    }
    if (!printable_token(config.time.ptp_interface) ||
        config.time.maximum_offset_ns == 0U ||
        std::find(config.hardware.data_interfaces.begin(),
                  config.hardware.data_interfaces.end(),
                  config.time.ptp_interface) ==
            config.hardware.data_interfaces.end()) {
        issue(issues, ConfigIssueCode::InvalidValue, "time",
              "PTP interface and positive maximum offset are required");
    }
    if (config.deployment_mode == DeploymentMode::Production &&
        !config.time.require_lock_before_routing) {
        issue(issues, ConfigIssueCode::InvalidValue,
              "time.require_lock_before_routing",
              "production routing must require clock lock");
    }
    if (config.ha.mode != HaMode::Standalone) {
        if (!printable_token(config.ha.node_id) ||
            !endpoint_text(config.ha.peer_endpoint) ||
            !endpoint_text(config.ha.witness_endpoint) ||
            !printable_token(config.ha.replication_interface) ||
            std::find(config.hardware.data_interfaces.begin(),
                      config.hardware.data_interfaces.end(),
                      config.ha.replication_interface) ==
                config.hardware.data_interfaces.end() ||
            config.ha.election_timeout_ms < 500U) {
            issue(issues, ConfigIssueCode::UnsafeHaConfiguration, "ha",
                  "HA requires node, peer, witness, replication interface, and an election timeout of at least 500ms");
        }
    }
    if (config.telemetry.metrics_listen_port == 0U ||
        !endpoint_text(config.telemetry.metrics_listen_address)) {
        issue(issues, ConfigIssueCode::InvalidValue, "telemetry.metrics",
              "metrics address and port are required");
    }
    if (config.deployment_mode == DeploymentMode::Production) {
        if (!config.recorder.enabled ||
            !absolute_device_path(config.recorder.storage_path) ||
            !printable_token(config.recorder.audit_interface_name) ||
            !endpoint_text(config.recorder.audit_listen_address) ||
            config.recorder.audit_listen_port == 0U) {
            issue(issues, ConfigIssueCode::MissingRequired, "recorder",
                  "production requires an enabled recorder, absolute storage path, and dedicated audit interface/address/port");
        }
        if (config.recorder.audit_interface_name ==
                config.management.interface_name ||
            std::find(config.hardware.data_interfaces.begin(),
                      config.hardware.data_interfaces.end(),
                      config.recorder.audit_interface_name) !=
                config.hardware.data_interfaces.end() ||
            std::find(config.hardware.audit_interfaces.begin(),
                      config.hardware.audit_interfaces.end(),
                      config.recorder.audit_interface_name) ==
                config.hardware.audit_interfaces.end()) {
            issue(issues, ConfigIssueCode::ConflictingResource,
                  "recorder.audit_interface_name",
                  "production audit interface must be declared by the hardware profile and distinct from management and all dataplane interfaces");
        }
        if (!config.recorder.audit_require_mutual_tls ||
            !production_secret_reference(
                config.recorder.audit_server_certificate_reference) ||
            !production_secret_reference(
                config.recorder.audit_private_key_reference) ||
            !production_secret_reference(
                config.recorder.audit_client_ca_reference)) {
            issue(issues, ConfigIssueCode::InvalidValue,
                  "recorder.audit_tls",
                  "production audit export requires mTLS and opaque TPM/PKCS11 certificate, key, and client-CA references");
        }
        constexpr std::uint64_t minimum_retention_seconds = 900U;
        constexpr std::uint64_t minimum_qualified_bytes_per_second =
            500'000'000U;
        if (config.recorder.retention_seconds < minimum_retention_seconds ||
            config.recorder.qualified_maximum_bytes_per_second <
                minimum_qualified_bytes_per_second) {
            issue(issues, ConfigIssueCode::InvalidValue,
                  "recorder.retention",
                  "production retention must be at least 900 seconds at a qualified maximum recorder rate of at least 500000000 bytes/second");
        }
        constexpr std::uint64_t minimum_tier0_usable_bytes =
            4'294'967'296ULL;
        const bool tier0_lanes_valid =
            config.recorder.tier0_usable_bytes >= minimum_tier0_usable_bytes &&
            config.recorder.tier0_cancel_reserved_bytes != 0U &&
            config.recorder.tier0_reconciliation_reserved_bytes != 0U &&
            config.recorder.tier0_cancel_reserved_bytes <
                config.recorder.tier0_usable_bytes &&
            config.recorder.tier0_reconciliation_reserved_bytes <
                config.recorder.tier0_usable_bytes -
                    config.recorder.tier0_cancel_reserved_bytes;
        if (!tier0_lanes_valid) {
            issue(issues, ConfigIssueCode::InvalidValue,
                  "recorder.tier0_capacity",
                  "production Tier-0 usable capacity must be at least 4 GiB; cancel-reserved, reconciliation-reserved, and remaining normal capacities must all be nonzero");
        }
        bool storage_budget_valid = false;
        if (config.recorder.retention_seconds != 0U &&
            config.recorder.qualified_maximum_bytes_per_second != 0U) {
            const auto room_after_reserve =
                std::numeric_limits<std::uint64_t>::max() -
                config.recorder.tier0_usable_bytes;
            if (config.recorder.qualified_maximum_bytes_per_second <=
                room_after_reserve / config.recorder.retention_seconds) {
                const auto required_storage =
                    config.recorder.retention_seconds *
                        config.recorder.qualified_maximum_bytes_per_second +
                    config.recorder.tier0_usable_bytes;
                storage_budget_valid =
                    config.recorder.maximum_storage_bytes >= required_storage;
            }
        }
        if (!storage_budget_valid) {
            issue(issues, ConfigIssueCode::InvalidValue,
                  "recorder.maximum_storage_bytes",
                  "maximum storage must cover retention_seconds * qualified maximum bytes/second plus one bounded Tier-0 usable-capacity reserve without overflow");
        }
    }
    if (config.recorder.audit_listen_address ==
            config.management.listen_address &&
        config.recorder.audit_listen_port == config.management.listen_port) {
        issue(issues, ConfigIssueCode::ConflictingResource,
              "recorder.audit_listen_port",
              "recorder audit endpoint conflicts with management endpoint");
    }
    if (config.update.channel.empty() ||
        (config.deployment_mode == DeploymentMode::Production &&
         (!production_secret_reference(config.update.trust_root_reference) ||
          !signed_manifest_valid(config.update.image_manifest)))) {
        issue(issues, ConfigIssueCode::MissingRequired, "update",
              "update channel, production trust root, and signed image manifest are required");
    }
    if (config.update.automatic_activation) {
        issue(issues, ConfigIssueCode::InvalidValue,
              "update.automatic_activation",
              "automatic activation is prohibited; activation requires an audited operator commit");
    }
    if (config.deployment_mode == DeploymentMode::Production &&
        !production_secret_reference(config.licensing.entitlement_reference)) {
        issue(issues, ConfigIssueCode::MissingRequired,
              "licensing.entitlement_reference",
              "production requires a non-environment entitlement reference");
    }
    if (config.deployment_mode == DeploymentMode::Production) {
        if (!signed_manifest_valid(config.signed_manifest)) {
            issue(issues, ConfigIssueCode::MissingRequired, "signed_manifest",
                  "production requires a signed device manifest and SHA-256");
        } else if (config.signed_manifest.sha256 !=
                   route_critical_sha256(config)) {
            issue(issues, ConfigIssueCode::InvalidValue,
                  "signed_manifest.sha256",
                  "signed manifest hash does not match canonical route-critical configuration");
        }
    }
    if (config.routing_contexts.empty()) {
        issue(issues, ConfigIssueCode::MissingRequired, "routing_contexts",
              "at least one routing context is required");
    } else if (config.deployment_mode == DeploymentMode::Production &&
               config.routing_contexts.size() != 1U) {
        issue(issues, ConfigIssueCode::InvalidValue, "routing_contexts",
              "production appliance requires exactly one routing context");
    }

    std::set<std::string> context_ids;
    std::set<std::string> account_contexts;
    std::set<std::string> persistence_paths;
    std::set<std::tuple<std::string, std::uint16_t>> listeners;
    listeners.emplace(
        config.management.listen_address, config.management.listen_port);
    listeners.emplace(
        config.recorder.audit_listen_address,
        config.recorder.audit_listen_port);
    for (const auto& context : config.routing_contexts) {
        const std::string base = "routing_contexts." + context.context_id;
        if (!printable_token(context.context_id) ||
            !printable_token(context.account_context, true)) {
            issue(issues, ConfigIssueCode::InvalidValue, base,
                  "context and account identities must be portable identifiers");
        }
        if (!context_ids.insert(context.context_id).second ||
            !account_contexts.insert(context.account_context).second) {
            issue(issues, ConfigIssueCode::DuplicateIdentity, base,
                  "context and account identities must be unique");
        }
        for (const auto& [name, path] : std::array{
                 std::pair{"routing_config_path", context.routing_config_path},
                 std::pair{"journal_path", context.journal_path},
                 std::pair{"checkpoint_path", context.checkpoint_path}}) {
            if (config.deployment_mode == DeploymentMode::Production &&
                !absolute_device_path(path)) {
                issue(issues, ConfigIssueCode::InvalidValue,
                      base + "." + name,
                      "production paths must be absolute and contain no '..'");
            }
        }
        if (!persistence_paths.insert(context.journal_path).second ||
            !persistence_paths.insert(context.checkpoint_path).second) {
            issue(issues, ConfigIssueCode::ConflictingResource, base,
                  "journal and checkpoint paths must be unique across contexts");
        }
        if (!risk_limits_valid(context.context_risk)) {
            issue(issues, ConfigIssueCode::InvalidValue, base + ".context_risk",
                  "context risk limits must be positive and aggregate limits must cover a single order");
        }
        if (context.account_risk_limits.empty() ||
            context.client_risk_limits.empty()) {
            issue(issues, ConfigIssueCode::MissingRequired, base + ".risk_limits",
                  "at least one account and one client risk limit are required");
        }
        const auto validate_scoped_risk = [&issues, &base, &context](
            const std::vector<ScopedRiskLimitConfig>& limits,
            const std::string_view scope_name) {
            std::set<std::string> identities;
            for (const auto& scoped : limits) {
                const auto path = base + "." + std::string{scope_name} + "." +
                    scoped.scope_id;
                if (!printable_token(scoped.scope_id, true) ||
                    !risk_limits_valid(scoped.limits)) {
                    issue(issues, ConfigIssueCode::InvalidValue, path,
                          "risk scope identity and positive limits are required");
                }
                if (!risk_within(scoped.limits, context.context_risk)) {
                    issue(issues, ConfigIssueCode::InvalidValue, path,
                          "scoped risk limits must not exceed context limits");
                }
                if (!identities.insert(scoped.scope_id).second) {
                    issue(issues, ConfigIssueCode::DuplicateIdentity, path,
                          "duplicate risk scope identity");
                }
            }
        };
        validate_scoped_risk(context.account_risk_limits, "account_risk");
        validate_scoped_risk(context.client_risk_limits, "client_risk");
        const auto account = std::find_if(
            context.account_risk_limits.begin(),
            context.account_risk_limits.end(),
            [&context](const ScopedRiskLimitConfig& item) {
                return item.scope_id == context.account_context;
            });
        if (account == context.account_risk_limits.end()) {
            issue(issues, ConfigIssueCode::MissingRequired,
                  base + ".account_risk",
                  "account_context must have an explicit account risk limit");
        } else {
            for (const auto& client : context.client_risk_limits) {
                if (!risk_within(client.limits, account->limits)) {
                    issue(issues, ConfigIssueCode::InvalidValue,
                          base + ".client_risk." + client.scope_id,
                          "client risk limits must not exceed account limits");
                }
            }
        }
        const auto& generations = context.routing_generations;
        if (generations.policy_generation == 0U ||
            generations.fee_generation == 0U ||
            generations.metric_generation == 0U ||
            generations.weight_generation == 0U ||
            !signed_manifest_valid(generations.manifest)) {
            issue(issues, ConfigIssueCode::MissingRequired,
                  base + ".routing_generations",
                  "policy, fee, metric, and weight generations plus signed manifest are required");
        }
        if (context.ingress_connectors.empty()) {
            issue(issues, ConfigIssueCode::MissingRequired,
                  base + ".ingress_connectors",
                  "at least one northbound ingress connector is required");
        }
        std::set<std::string> ingress_ids;
        for (const auto& ingress : context.ingress_connectors) {
            const auto path = base + ".ingress." + ingress.connector_id;
            if (!printable_token(ingress.connector_id) ||
                !endpoint_text(ingress.listen_address) ||
                ingress.listen_port == 0U ||
                !printable_token(ingress.interface_name) ||
                std::find(config.hardware.data_interfaces.begin(),
                          config.hardware.data_interfaces.end(),
                          ingress.interface_name) ==
                    config.hardware.data_interfaces.end() ||
                ingress.normal_credit_limit == 0U ||
                ingress.normal_credit_limit > 65'535U ||
                ingress.cancel_credit_limit == 0U ||
                ingress.cancel_credit_limit > 65'535U) {
                issue(issues, ConfigIssueCode::InvalidValue, path,
                      "ingress identity, endpoint, interface, and 16-bit credit limits are required");
            }
            if (!ingress_ids.insert(ingress.connector_id).second) {
                issue(issues, ConfigIssueCode::DuplicateIdentity, path,
                      "duplicate ingress connector identity");
            }
            if (!listeners.emplace(
                    ingress.listen_address, ingress.listen_port).second) {
                issue(issues, ConfigIssueCode::ConflictingResource, path,
                      "listen address and port are already assigned");
            }
            if (config.deployment_mode == DeploymentMode::Production &&
                !production_secret_reference(ingress.credential_reference)) {
                issue(issues, ConfigIssueCode::InvalidValue,
                      path + ".credential_reference",
                      "production ingress requires a non-environment credential reference");
            }
            if (ingress.protocol == IngressProtocol::Fix44 &&
                (ingress.sender_comp_id.empty() || ingress.target_comp_id.empty())) {
                issue(issues, ConfigIssueCode::MissingRequired, path,
                      "FIX ingress requires sender and target CompIDs");
            }
        }
        if (context.venues.empty()) {
            issue(issues, ConfigIssueCode::MissingRequired, base + ".venues",
                  "at least one venue connector is required");
        }
        std::set<std::uint32_t> venue_ids;
        for (const auto& venue : context.venues) {
            const auto path = base + ".venue." + std::to_string(venue.venue_id);
            const bool connector_type_valid =
                venue.connector_type == VenueConnectorType::NativeBinary ||
                venue.connector_type == VenueConnectorType::Fix44;
            const std::array<std::string_view, 4U> venue_interfaces{
                venue.feed_a.interface_name,
                venue.feed_b.interface_name,
                venue.recovery.interface_name,
                venue.order_entry_interface};
            const bool feed_interfaces_known = std::all_of(
                venue_interfaces.begin(), venue_interfaces.end(),
                [&config](const std::string_view interface_name) {
                    return std::find(
                        config.hardware.data_interfaces.begin(),
                        config.hardware.data_interfaces.end(),
                        interface_name) != config.hardware.data_interfaces.end();
                });
            if (venue.venue_id == 0U || !printable_token(venue.venue_name) ||
                !printable_token(venue.connector_id) ||
                !printable_token(venue.connector_version) ||
                !connector_type_valid || !feed_interfaces_known ||
                !endpoint_text(venue.feed_a.host) ||
                venue.feed_a.port == 0U ||
                !printable_token(venue.feed_a.interface_name) ||
                !endpoint_text(venue.feed_b.host) ||
                venue.feed_b.port == 0U ||
                !printable_token(venue.feed_b.interface_name) ||
                !endpoint_text(venue.recovery.host) ||
                venue.recovery.port == 0U ||
                !printable_token(venue.recovery.interface_name) ||
                !endpoint_text(venue.order_entry_host) ||
                venue.order_entry_port == 0U ||
                !printable_token(venue.order_entry_interface) ||
                venue.quickfix_settings_reference.empty() ||
                !sha256_hex(venue.dictionary_sha256) ||
                !signed_manifest_valid(venue.connector_manifest)) {
                issue(issues, ConfigIssueCode::InvalidValue, path,
                      "venue identity, native type, A/B feeds, recovery, order entry, settings, dictionary, and signed connector manifest are required");
            }
            const std::set<std::tuple<std::string, std::uint16_t>> feed_endpoints{
                {venue.feed_a.host, venue.feed_a.port},
                {venue.feed_b.host, venue.feed_b.port},
                {venue.recovery.host, venue.recovery.port}};
            if (feed_endpoints.size() != 3U) {
                issue(issues, ConfigIssueCode::ConflictingResource,
                      path + ".feeds",
                      "A, B, and recovery feeds must use distinct endpoints");
            }
            if (!venue_ids.insert(venue.venue_id).second) {
                issue(issues, ConfigIssueCode::DuplicateIdentity, path,
                      "duplicate venue identity");
            }
            if (config.deployment_mode == DeploymentMode::Production &&
                !production_secret_reference(venue.credential_reference)) {
                issue(issues, ConfigIssueCode::InvalidValue,
                      path + ".credential_reference",
                      "production venue requires a non-environment credential reference");
            }
            if (config.ha.mode != HaMode::Standalone &&
                !venue.supports_reconciliation) {
                issue(issues, ConfigIssueCode::UnsafeHaConfiguration, path,
                      "HA is prohibited for a venue without certified reconciliation");
            }
        }
    }
    return issues;
}

namespace {

void add_risk_change(
    ConfigDiff& diff,
    std::string path,
    const RiskLimitConfig& before,
    const RiskLimitConfig& after) {
    if (before == after) return;
    add_change(
        diff,
        std::move(path),
        risk_increased(before, after) ?
            ChangeClass::DualAuthorized : ChangeClass::LiveAtomic);
}

void add_scoped_risk_changes(
    ConfigDiff& diff,
    const std::string& base,
    const std::vector<ScopedRiskLimitConfig>& before,
    const std::vector<ScopedRiskLimitConfig>& after) {
    std::set<std::string> identities;
    for (const auto& item : before) identities.insert(item.scope_id);
    for (const auto& item : after) identities.insert(item.scope_id);
    for (const auto& identity : identities) {
        const auto old_value = std::find_if(
            before.begin(), before.end(), [&identity](const auto& item) {
                return item.scope_id == identity;
            });
        const auto new_value = std::find_if(
            after.begin(), after.end(), [&identity](const auto& item) {
                return item.scope_id == identity;
            });
        const auto path = base + "." + identity;
        if (old_value == before.end() || new_value == after.end()) {
            // Adding a trading identity expands access; removing its explicit
            // cap can make the effective policy less restrictive. Both need
            // two independent risk authorizers.
            add_change(diff, path, ChangeClass::DualAuthorized);
        } else {
            add_risk_change(diff, path, old_value->limits, new_value->limits);
        }
    }
}

}  // namespace

ConfigDiff diff_device_config(
    const DeviceConfigV2& before,
    const DeviceConfigV2& after) {
    ConfigDiff diff;
    if (before.schema_version != after.schema_version ||
        before.device_id != after.device_id || before.hostname != after.hostname ||
        before.deployment_mode != after.deployment_mode) {
        add_change(diff, "device", ChangeClass::MaintenanceReboot);
    }
    if (before.management != after.management) {
        add_change(diff, "management", ChangeClass::MaintenanceReboot);
    }
    if (before.hardware != after.hardware ||
        before.capacities != after.capacities) {
        add_change(diff, "hardware_or_capacities", ChangeClass::MaintenanceReboot);
    }
    if (before.time != after.time) {
        add_change(diff, "time", ChangeClass::MaintenanceReboot);
    }
    if (before.ha != after.ha) {
        add_change(diff, "ha", ChangeClass::MaintenanceReboot);
    }
    if (before.telemetry != after.telemetry) {
        add_change(diff, "telemetry", ChangeClass::LiveAtomic);
    }
    if (before.recorder != after.recorder) {
        add_change(diff, "recorder", ChangeClass::MaintenanceReboot);
    }
    if (before.update != after.update) {
        add_change(diff, "update", ChangeClass::LiveAtomic);
    }
    if (before.licensing != after.licensing) {
        add_change(diff, "licensing", ChangeClass::LiveAtomic);
    }
    if (before.signed_manifest != after.signed_manifest) {
        add_change(diff, "signed_manifest", ChangeClass::LiveAtomic);
    }

    std::set<std::string> all_contexts;
    for (const auto& context : before.routing_contexts) {
        all_contexts.insert(context.context_id);
    }
    for (const auto& context : after.routing_contexts) {
        all_contexts.insert(context.context_id);
    }
    for (const auto& context_id : all_contexts) {
        const auto old_context = std::find_if(
            before.routing_contexts.begin(), before.routing_contexts.end(),
            [&context_id](const auto& item) { return item.context_id == context_id; });
        const auto new_context = std::find_if(
            after.routing_contexts.begin(), after.routing_contexts.end(),
            [&context_id](const auto& item) { return item.context_id == context_id; });
        const auto base = "routing_contexts." + context_id;
        if (old_context == before.routing_contexts.end()) {
            add_change(diff, base, ChangeClass::DrainReconcileRestart);
            add_change(diff, base + ".risk_admission",
                       ChangeClass::DualAuthorized);
            continue;
        }
        if (new_context == after.routing_contexts.end()) {
            add_change(diff, base, ChangeClass::DrainReconcileRestart);
            continue;
        }
        if (old_context->account_context != new_context->account_context) {
            add_change(diff, base + ".account_context",
                       ChangeClass::DualAuthorized);
        }
        if (old_context->account_context != new_context->account_context ||
            old_context->routing_config_path != new_context->routing_config_path ||
            old_context->journal_path != new_context->journal_path ||
            old_context->checkpoint_path != new_context->checkpoint_path) {
            add_change(diff, base + ".identity_or_persistence",
                       ChangeClass::DrainReconcileRestart);
        }
        add_risk_change(
            diff, base + ".context_risk",
            old_context->context_risk, new_context->context_risk);
        add_scoped_risk_changes(
            diff, base + ".account_risk",
            old_context->account_risk_limits,
            new_context->account_risk_limits);
        add_scoped_risk_changes(
            diff, base + ".client_risk",
            old_context->client_risk_limits,
            new_context->client_risk_limits);
        if (old_context->routing_generations !=
            new_context->routing_generations) {
            add_change(diff, base + ".routing_generations",
                       ChangeClass::LiveAtomic);
        }
        if (old_context->ingress_connectors != new_context->ingress_connectors) {
            add_change(diff, base + ".ingress_connectors",
                       ChangeClass::DrainReconcileRestart);
        }
        std::set<std::uint32_t> venues;
        for (const auto& venue : old_context->venues) venues.insert(venue.venue_id);
        for (const auto& venue : new_context->venues) venues.insert(venue.venue_id);
        for (const auto venue_id : venues) {
            const auto old_venue = std::find_if(
                old_context->venues.begin(), old_context->venues.end(),
                [venue_id](const auto& item) { return item.venue_id == venue_id; });
            const auto new_venue = std::find_if(
                new_context->venues.begin(), new_context->venues.end(),
                [venue_id](const auto& item) { return item.venue_id == venue_id; });
            const auto path = base + ".venue." + std::to_string(venue_id);
            if (old_venue == old_context->venues.end() ||
                new_venue == new_context->venues.end() ||
                *old_venue != *new_venue) {
                add_change(diff, path, ChangeClass::DrainReconcileRestart);
            }
        }
    }
    std::sort(diff.changes.begin(), diff.changes.end(), [](const auto& left, const auto& right) {
        return left.path < right.path;
    });
    return diff;
}

namespace {

class CanonicalWriter final {
public:
    void text(const std::string_view key, const std::string_view value) {
        document_ += key;
        document_.push_back('=');
        document_ += std::to_string(value.size());
        document_.push_back(':');
        document_.append(value);
        document_.push_back(';');
    }

    template <typename Integer>
    void integer(const std::string_view key, const Integer value) {
        text(key, std::to_string(value));
    }

    void boolean(const std::string_view key, const bool value) {
        text(key, value ? "1" : "0");
    }

    [[nodiscard]] std::string finish() && { return std::move(document_); }

private:
    std::string document_;
};

void canonical_manifest(
    CanonicalWriter& writer,
    const std::string_view prefix,
    const SignedManifestReference& manifest) {
    writer.text(std::string{prefix} + ".reference", manifest.manifest_reference);
    writer.text(std::string{prefix} + ".sha256", manifest.sha256);
    writer.text(std::string{prefix} + ".signature", manifest.signature_reference);
}

void canonical_risk(
    CanonicalWriter& writer,
    const std::string_view prefix,
    const RiskLimitConfig& risk) {
    writer.integer(std::string{prefix} + ".single_quantity",
                   risk.maximum_single_order_quantity);
    writer.integer(std::string{prefix} + ".single_notional",
                   risk.maximum_single_order_notional);
    writer.integer(std::string{prefix} + ".open_quantity",
                   risk.maximum_open_quantity);
    writer.integer(std::string{prefix} + ".open_notional",
                   risk.maximum_open_notional);
    writer.integer(std::string{prefix} + ".open_orders",
                   risk.maximum_open_orders);
    writer.integer(std::string{prefix} + ".orders_per_second",
                   risk.maximum_orders_per_second);
    writer.boolean(std::string{prefix} + ".kill_switch",
                   risk.kill_switch_engaged);
}

void sha256_transform(
    std::array<std::uint32_t, 8U>& state,
    const std::byte* const block) noexcept {
    static constexpr std::array<std::uint32_t, 64U> constants{
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
        0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
        0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
        0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
        0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
        0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
        0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
        0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
        0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
        0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
        0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
        0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
        0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
        0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
        0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
        0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U};
    std::array<std::uint32_t, 64U> schedule{};
    for (std::size_t index = 0U; index < 16U; ++index) {
        const auto offset = index * 4U;
        schedule[index] =
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(block[offset])) << 24U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(block[offset + 1U])) << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(block[offset + 2U])) << 8U) |
            static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(block[offset + 3U]));
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
        const auto s0 = std::rotr(schedule[index - 15U], 7) ^
            std::rotr(schedule[index - 15U], 18) ^
            (schedule[index - 15U] >> 3U);
        const auto s1 = std::rotr(schedule[index - 2U], 17) ^
            std::rotr(schedule[index - 2U], 19) ^
            (schedule[index - 2U] >> 10U);
        schedule[index] = schedule[index - 16U] + s0 +
            schedule[index - 7U] + s1;
    }
    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0U; index < schedule.size(); ++index) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto temporary1 = h + sum1 + choice + constants[index] + schedule[index];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

[[nodiscard]] std::string sha256(const std::string_view input) {
    std::array<std::uint32_t, 8U> state{
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
    const auto* const bytes = reinterpret_cast<const std::byte*>(input.data());
    std::size_t offset = 0U;
    while (input.size() - offset >= 64U) {
        sha256_transform(state, bytes + offset);
        offset += 64U;
    }
    std::array<std::byte, 128U> tail{};
    const auto remainder = input.size() - offset;
    std::copy(bytes + offset, bytes + input.size(), tail.begin());
    tail[remainder] = std::byte{0x80U};
    const auto padded_size = remainder < 56U ? 64U : 128U;
    const auto bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        tail[padded_size - 1U - index] =
            static_cast<std::byte>(bit_length >> (index * 8U));
    }
    sha256_transform(state, tail.data());
    if (padded_size == 128U) sha256_transform(state, tail.data() + 64U);
    static constexpr char hex[] = "0123456789abcdef";
    std::string output(64U, '0');
    std::size_t cursor = 0U;
    for (const auto word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            output[cursor++] = hex[(word >> shift) & 0xFU];
        }
    }
    return output;
}

}  // namespace

std::string canonical_route_critical_document(const DeviceConfigV2& config) {
    CanonicalWriter writer;
    writer.integer("schema", config.schema_version);
    writer.text("device", config.device_id);
    writer.text("deployment", to_string(config.deployment_mode));
    writer.integer("capacity.parent", config.capacities.parent_order_pool);
    writer.integer("capacity.child", config.capacities.child_order_pool);
    writer.integer("capacity.requests", config.capacities.request_history_pool);
    writer.integer("capacity.ingress", config.capacities.ingress_ring_entries);
    writer.integer("capacity.market_data", config.capacities.market_data_ring_entries);
    writer.integer("capacity.route", config.capacities.route_command_ring_entries);
    writer.integer("capacity.execution", config.capacities.execution_ring_entries);
    writer.integer("capacity.journal", config.capacities.journal_ring_entries);
    writer.boolean("recorder.enabled", config.recorder.enabled);
    writer.text("recorder.storage_path", config.recorder.storage_path);
    writer.integer("recorder.retention_seconds",
                   config.recorder.retention_seconds);
    writer.integer("recorder.qualified_maximum_bytes_per_second",
                   config.recorder.qualified_maximum_bytes_per_second);
    writer.integer("recorder.tier0_usable_bytes",
                   config.recorder.tier0_usable_bytes);
    writer.integer("recorder.tier0_cancel_reserved_bytes",
                   config.recorder.tier0_cancel_reserved_bytes);
    writer.integer("recorder.tier0_reconciliation_reserved_bytes",
                   config.recorder.tier0_reconciliation_reserved_bytes);
    writer.integer("recorder.maximum_storage_bytes",
                   config.recorder.maximum_storage_bytes);
    writer.text("time.interface", config.time.ptp_interface);
    writer.integer("time.maximum_offset", config.time.maximum_offset_ns);
    writer.boolean("time.lock_required", config.time.require_lock_before_routing);
    writer.text("ha.mode", to_string(config.ha.mode));
    writer.text("ha.node", config.ha.node_id);
    writer.text("ha.peer", config.ha.peer_endpoint);
    writer.text("ha.witness", config.ha.witness_endpoint);
    writer.text("ha.interface", config.ha.replication_interface);
    writer.integer("ha.election_ms", config.ha.election_timeout_ms);

    std::vector<const RoutingContextConfig*> contexts;
    contexts.reserve(config.routing_contexts.size());
    for (const auto& context : config.routing_contexts) contexts.push_back(&context);
    std::sort(contexts.begin(), contexts.end(), [](const auto* left, const auto* right) {
        return left->context_id < right->context_id;
    });
    for (const auto* const context : contexts) {
        const auto base = "context." + context->context_id;
        writer.text(base + ".id", context->context_id);
        writer.text(base + ".account", context->account_context);
        writer.text(base + ".routing_path", context->routing_config_path);
        writer.text(base + ".journal", context->journal_path);
        writer.text(base + ".checkpoint", context->checkpoint_path);
        canonical_risk(writer, base + ".risk", context->context_risk);
        writer.integer(base + ".generation.policy",
                       context->routing_generations.policy_generation);
        writer.integer(base + ".generation.fee",
                       context->routing_generations.fee_generation);
        writer.integer(base + ".generation.metric",
                       context->routing_generations.metric_generation);
        writer.integer(base + ".generation.weight",
                       context->routing_generations.weight_generation);
        canonical_manifest(
            writer, base + ".routing_manifest",
            context->routing_generations.manifest);

        const auto append_scoped = [&writer, &base](
            const std::string_view name,
            const std::vector<ScopedRiskLimitConfig>& values) {
            std::vector<const ScopedRiskLimitConfig*> sorted;
            sorted.reserve(values.size());
            for (const auto& value : values) sorted.push_back(&value);
            std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
                return left->scope_id < right->scope_id;
            });
            for (const auto* const value : sorted) {
                const auto prefix = base + "." + std::string{name} + "." +
                    value->scope_id;
                writer.text(prefix + ".id", value->scope_id);
                canonical_risk(writer, prefix, value->limits);
            }
        };
        append_scoped("account", context->account_risk_limits);
        append_scoped("client", context->client_risk_limits);

        std::vector<const IngressConnectorConfig*> ingress;
        ingress.reserve(context->ingress_connectors.size());
        for (const auto& value : context->ingress_connectors) ingress.push_back(&value);
        std::sort(ingress.begin(), ingress.end(), [](const auto* left, const auto* right) {
            return left->connector_id < right->connector_id;
        });
        for (const auto* const value : ingress) {
            const auto prefix = base + ".ingress." + value->connector_id;
            writer.text(prefix + ".protocol", to_string(value->protocol));
            writer.text(prefix + ".address", value->listen_address);
            writer.integer(prefix + ".port", value->listen_port);
            writer.text(prefix + ".interface", value->interface_name);
            writer.text(prefix + ".credential", value->credential_reference);
            writer.text(prefix + ".sender", value->sender_comp_id);
            writer.text(prefix + ".target", value->target_comp_id);
            writer.integer(prefix + ".normal_credits", value->normal_credit_limit);
            writer.integer(prefix + ".cancel_credits", value->cancel_credit_limit);
        }

        std::vector<const VenueConnectorConfig*> venues;
        venues.reserve(context->venues.size());
        for (const auto& value : context->venues) venues.push_back(&value);
        std::sort(venues.begin(), venues.end(), [](const auto* left, const auto* right) {
            return left->venue_id < right->venue_id;
        });
        for (const auto* const value : venues) {
            const auto prefix = base + ".venue." + std::to_string(value->venue_id);
            writer.integer(prefix + ".id", value->venue_id);
            writer.text(prefix + ".name", value->venue_name);
            writer.text(prefix + ".connector", value->connector_id);
            writer.text(prefix + ".version", value->connector_version);
            writer.text(prefix + ".type", to_string(value->connector_type));
            const auto append_feed = [&writer, &prefix](
                const std::string_view name,
                const FeedEndpointConfig& feed) {
                writer.text(prefix + "." + std::string{name} + ".host", feed.host);
                writer.integer(prefix + "." + std::string{name} + ".port", feed.port);
                writer.text(prefix + "." + std::string{name} + ".interface",
                            feed.interface_name);
            };
            append_feed("feed_a", value->feed_a);
            append_feed("feed_b", value->feed_b);
            append_feed("recovery", value->recovery);
            writer.text(prefix + ".order_host", value->order_entry_host);
            writer.integer(prefix + ".order_port", value->order_entry_port);
            writer.text(prefix + ".order_interface", value->order_entry_interface);
            writer.text(prefix + ".settings", value->quickfix_settings_reference);
            writer.text(prefix + ".credential", value->credential_reference);
            writer.text(prefix + ".dictionary", value->dictionary_sha256);
            writer.boolean(prefix + ".reconciliation", value->supports_reconciliation);
            canonical_manifest(writer, prefix + ".manifest", value->connector_manifest);
        }
    }
    return std::move(writer).finish();
}

std::string route_critical_sha256(const DeviceConfigV2& config) {
    return sha256(canonical_route_critical_document(config));
}

std::string config_diff_sha256(const ConfigDiff& diff) {
    CanonicalWriter writer;
    writer.integer("overall", static_cast<std::uint8_t>(diff.overall));
    writer.integer("count", diff.changes.size());
    for (std::size_t index = 0U; index < diff.changes.size(); ++index) {
        const auto prefix = "change." + std::to_string(index);
        writer.text(prefix + ".path", diff.changes[index].path);
        writer.integer(
            prefix + ".class",
            static_cast<std::uint8_t>(diff.changes[index].classification));
    }
    return sha256(std::move(writer).finish());
}

std::string canonical_dual_authorization_document(
    const DualAuthorizationCapability& capability) {
    CanonicalWriter writer;
    writer.integer("action", static_cast<std::uint8_t>(capability.action));
    writer.integer("running_revision", capability.running_revision);
    writer.integer("candidate_revision", capability.candidate_revision);
    writer.text("candidate_sha256", capability.candidate_sha256);
    writer.text("diff_sha256", capability.diff_sha256);
    writer.integer("expires_at_monotonic_ns",
                   capability.expires_at_monotonic_ns);
    writer.text("approver.principal", capability.approver.principal);
    writer.text("approver.role", capability.approver.role);
    writer.text("approver.source", capability.approver.source);
    writer.text("approver.request_id", capability.approver.request_id);
    writer.text("approver.reason", capability.approver.reason);
    writer.integer("approver.timestamp_ns", capability.approver.timestamp_ns);
    return std::move(writer).finish();
}

ConfigDecodeResult LegacyIniMigrationCodec::decode(
    const std::string_view document) const {
    std::vector<ConfigIssue> issues;
    std::vector<RawSection> sections;
    RawSection* current = nullptr;
    std::set<std::string> section_identities;
    std::size_t line_number = 0U;
    std::size_t begin = 0U;
    while (begin <= document.size()) {
        const auto end = document.find('\n', begin);
        ++line_number;
        auto line = trim(document.substr(
            begin, end == document.npos ? document.size() - begin : end - begin));
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            if (line.front() == '[') {
                if (line.size() < 3U || line.back() != ']') {
                    issue(issues, ConfigIssueCode::Syntax, "", "malformed section", line_number);
                    current = nullptr;
                } else {
                    const auto words = split_words(trim(line.substr(1U, line.size() - 2U)));
                    if (words.empty()) {
                        issue(issues, ConfigIssueCode::Syntax, "", "empty section", line_number);
                        current = nullptr;
                    } else {
                        RawSection section;
                        section.kind = words.front();
                        section.arguments.assign(words.begin() + 1, words.end());
                        section.line = line_number;
                        std::string identity = section.kind;
                        for (const auto& argument : section.arguments) {
                            identity += "\n" + argument;
                        }
                        if (!section_identities.insert(identity).second) {
                            issue(issues, ConfigIssueCode::DuplicateSection,
                                  section.kind, "duplicate section", line_number);
                            current = nullptr;
                        } else {
                            sections.push_back(std::move(section));
                            current = &sections.back();
                        }
                    }
                }
            } else {
                const auto equals = line.find('=');
                if (current == nullptr || equals == line.npos) {
                    issue(issues, ConfigIssueCode::Syntax, "",
                          "field appears outside a section or lacks '='", line_number);
                } else {
                    const auto key = trim(line.substr(0U, equals));
                    const auto value = trim(line.substr(equals + 1U));
                    if (key.empty() || value.empty()) {
                        issue(issues, ConfigIssueCode::Syntax, current->kind,
                              "field name and value must be nonempty", line_number);
                    } else if (!current->fields.emplace(
                                   std::string{key},
                                   RawField{std::string{value}, line_number}).second) {
                        issue(issues, ConfigIssueCode::DuplicateField,
                              current->kind + "." + std::string{key},
                              "duplicate field", line_number);
                    }
                }
            }
        }
        if (end == document.npos) break;
        begin = end + 1U;
    }

    DeviceConfigV2 config;
    const auto* device = singleton(sections, "device", issues);
    const auto* management = singleton(sections, "management", issues);
    const auto* hardware = singleton(sections, "hardware", issues);
    const auto* capacities = singleton(sections, "capacities", issues);
    const auto* time = singleton(sections, "time", issues);
    const auto* ha = singleton(sections, "ha", issues);
    const auto* telemetry = singleton(sections, "telemetry", issues);
    const auto* recorder = singleton(sections, "recorder", issues);
    const auto* update = singleton(sections, "update", issues);
    const auto* licensing = singleton(sections, "licensing", issues);
    const auto* signed_manifest = singleton(sections, "signed-manifest", issues);

    if (device != nullptr) {
        reject_unknown_fields(*device,
            {"schema_version", "device_id", "hostname", "deployment_mode"}, issues);
        config.schema_version = required_integer<std::uint32_t>(*device, "schema_version", issues);
        config.device_id = required_text(*device, "device_id", issues);
        config.hostname = required_text(*device, "hostname", issues);
        const auto value = required_text(*device, "deployment_mode", issues);
        const auto parsed = parse_deployment(value);
        if (!value.empty() && !parsed.has_value()) {
            issue(issues, ConfigIssueCode::InvalidValue, "device.deployment_mode",
                  "expected DEVELOPMENT, CERTIFICATION, or PRODUCTION",
                  find_field(*device, "deployment_mode")->line);
        } else if (parsed) {
            config.deployment_mode = *parsed;
        }
    }
    if (management != nullptr) {
        reject_unknown_fields(*management,
            {"interface", "listen_address", "listen_port", "require_mutual_tls",
             "server_certificate_reference", "private_key_reference",
             "client_ca_reference", "aaa_provider", "remote_support_enabled"}, issues);
        config.management.interface_name = required_text(*management, "interface", issues);
        config.management.listen_address = required_text(*management, "listen_address", issues);
        config.management.listen_port = required_integer<std::uint16_t>(*management, "listen_port", issues);
        config.management.require_mutual_tls = optional_bool(
            *management, "require_mutual_tls", true, issues);
        config.management.server_certificate_reference = required_text(
            *management, "server_certificate_reference", issues);
        config.management.private_key_reference = required_text(
            *management, "private_key_reference", issues);
        config.management.client_ca_reference = required_text(
            *management, "client_ca_reference", issues);
        config.management.aaa_provider = required_text(*management, "aaa_provider", issues);
        config.management.remote_support_enabled = optional_bool(
            *management, "remote_support_enabled", false, issues);
    }
    if (hardware != nullptr) {
        reject_unknown_fields(*hardware,
            {"profile_id", "realtime_cores", "management_cores", "data_interfaces",
             "audit_interfaces", "huge_pages_2mb", "journal_device"}, issues);
        config.hardware.profile_id = required_text(*hardware, "profile_id", issues);
        for (const auto& item : split_list(required_text(*hardware, "realtime_cores", issues))) {
            const auto value = parse_unsigned<std::uint32_t>(item);
            if (value) config.hardware.realtime_cores.push_back(*value);
            else issue(issues, ConfigIssueCode::InvalidValue, "hardware.realtime_cores",
                       "core list contains a non-integer", hardware->line);
        }
        for (const auto& item : split_list(required_text(*hardware, "management_cores", issues))) {
            const auto value = parse_unsigned<std::uint32_t>(item);
            if (value) config.hardware.management_cores.push_back(*value);
            else issue(issues, ConfigIssueCode::InvalidValue, "hardware.management_cores",
                       "core list contains a non-integer", hardware->line);
        }
        config.hardware.data_interfaces = split_list(
            required_text(*hardware, "data_interfaces", issues));
        config.hardware.audit_interfaces = split_list(
            required_text(*hardware, "audit_interfaces", issues));
        config.hardware.huge_pages_2mb = required_integer<std::uint32_t>(
            *hardware, "huge_pages_2mb", issues);
        config.hardware.journal_device = required_text(*hardware, "journal_device", issues);
    }
    if (capacities != nullptr) {
        reject_unknown_fields(*capacities,
            {"parent_order_pool", "child_order_pool", "request_history_pool",
             "ingress_ring_entries", "market_data_ring_entries",
             "route_command_ring_entries", "execution_ring_entries",
             "journal_ring_entries"}, issues);
        config.capacities.parent_order_pool = required_integer<std::uint32_t>(
            *capacities, "parent_order_pool", issues);
        config.capacities.child_order_pool = required_integer<std::uint32_t>(
            *capacities, "child_order_pool", issues);
        config.capacities.request_history_pool = required_integer<std::uint32_t>(
            *capacities, "request_history_pool", issues);
        config.capacities.ingress_ring_entries = required_integer<std::uint32_t>(
            *capacities, "ingress_ring_entries", issues);
        config.capacities.market_data_ring_entries = required_integer<std::uint32_t>(
            *capacities, "market_data_ring_entries", issues);
        config.capacities.route_command_ring_entries = required_integer<std::uint32_t>(
            *capacities, "route_command_ring_entries", issues);
        config.capacities.execution_ring_entries = required_integer<std::uint32_t>(
            *capacities, "execution_ring_entries", issues);
        config.capacities.journal_ring_entries = required_integer<std::uint32_t>(
            *capacities, "journal_ring_entries", issues);
    }
    if (time != nullptr) {
        reject_unknown_fields(*time,
            {"ptp_interface", "maximum_offset_ns", "require_lock_before_routing"}, issues);
        config.time.ptp_interface = required_text(*time, "ptp_interface", issues);
        config.time.maximum_offset_ns = required_integer<std::uint64_t>(
            *time, "maximum_offset_ns", issues);
        config.time.require_lock_before_routing = optional_bool(
            *time, "require_lock_before_routing", true, issues);
    }
    if (ha != nullptr) {
        reject_unknown_fields(*ha,
            {"mode", "node_id", "peer_endpoint", "witness_endpoint",
             "replication_interface", "election_timeout_ms"}, issues);
        const auto value = required_text(*ha, "mode", issues);
        const auto parsed = parse_ha(value);
        if (!value.empty() && !parsed.has_value()) {
            issue(issues, ConfigIssueCode::InvalidValue, "ha.mode",
                  "unknown HA mode", find_field(*ha, "mode")->line);
        } else if (parsed) {
            config.ha.mode = *parsed;
        }
        if (const auto* field = find_field(*ha, "node_id")) config.ha.node_id = field->value;
        if (const auto* field = find_field(*ha, "peer_endpoint")) config.ha.peer_endpoint = field->value;
        if (const auto* field = find_field(*ha, "witness_endpoint")) config.ha.witness_endpoint = field->value;
        if (const auto* field = find_field(*ha, "replication_interface")) config.ha.replication_interface = field->value;
        if (find_field(*ha, "election_timeout_ms") != nullptr) {
            config.ha.election_timeout_ms = required_integer<std::uint32_t>(
                *ha, "election_timeout_ms", issues);
        }
    }
    if (telemetry != nullptr) {
        reject_unknown_fields(*telemetry,
            {"metrics_listen_address", "metrics_listen_port", "syslog_endpoint",
             "snmp_enabled", "vendor_upload_enabled"}, issues);
        config.telemetry.metrics_listen_address = required_text(
            *telemetry, "metrics_listen_address", issues);
        config.telemetry.metrics_listen_port = required_integer<std::uint16_t>(
            *telemetry, "metrics_listen_port", issues);
        if (const auto* field = find_field(*telemetry, "syslog_endpoint")) {
            config.telemetry.syslog_endpoint = field->value;
        }
        config.telemetry.snmp_enabled = optional_bool(
            *telemetry, "snmp_enabled", false, issues);
        config.telemetry.vendor_upload_enabled = optional_bool(
            *telemetry, "vendor_upload_enabled", false, issues);
    }
    if (recorder != nullptr) {
        reject_unknown_fields(*recorder,
            {"enabled", "storage_path", "audit_interface_name",
             "audit_listen_address", "audit_listen_port",
             "audit_require_mutual_tls",
             "audit_server_certificate_reference",
             "audit_private_key_reference", "audit_client_ca_reference",
             "retention_seconds", "qualified_maximum_bytes_per_second",
             "tier0_usable_bytes", "tier0_cancel_reserved_bytes",
             "tier0_reconciliation_reserved_bytes", "maximum_storage_bytes"},
            issues);
        config.recorder.enabled = optional_bool(*recorder, "enabled", true, issues);
        config.recorder.storage_path = required_text(*recorder, "storage_path", issues);
        config.recorder.audit_interface_name = required_text(
            *recorder, "audit_interface_name", issues);
        config.recorder.audit_listen_address = required_text(
            *recorder, "audit_listen_address", issues);
        config.recorder.audit_listen_port = required_integer<std::uint16_t>(
            *recorder, "audit_listen_port", issues);
        config.recorder.audit_require_mutual_tls = optional_bool(
            *recorder, "audit_require_mutual_tls", true, issues);
        config.recorder.audit_server_certificate_reference = required_text(
            *recorder, "audit_server_certificate_reference", issues);
        config.recorder.audit_private_key_reference = required_text(
            *recorder, "audit_private_key_reference", issues);
        config.recorder.audit_client_ca_reference = required_text(
            *recorder, "audit_client_ca_reference", issues);
        config.recorder.retention_seconds = required_integer<std::uint64_t>(
            *recorder, "retention_seconds", issues);
        config.recorder.qualified_maximum_bytes_per_second =
            required_integer<std::uint64_t>(
                *recorder, "qualified_maximum_bytes_per_second", issues);
        config.recorder.tier0_usable_bytes = required_integer<std::uint64_t>(
            *recorder, "tier0_usable_bytes", issues);
        config.recorder.tier0_cancel_reserved_bytes =
            required_integer<std::uint64_t>(
                *recorder, "tier0_cancel_reserved_bytes", issues);
        config.recorder.tier0_reconciliation_reserved_bytes =
            required_integer<std::uint64_t>(
                *recorder, "tier0_reconciliation_reserved_bytes", issues);
        config.recorder.maximum_storage_bytes = required_integer<std::uint64_t>(
            *recorder, "maximum_storage_bytes", issues);
    }
    if (update != nullptr) {
        reject_unknown_fields(*update,
            {"channel", "trust_root_reference", "automatic_download",
             "automatic_activation", "image_manifest_reference",
             "image_sha256", "image_signature_reference"}, issues);
        config.update.channel = required_text(*update, "channel", issues);
        config.update.trust_root_reference = required_text(
            *update, "trust_root_reference", issues);
        config.update.image_manifest = parse_manifest_fields(
            *update, "image_", issues);
        config.update.automatic_download = optional_bool(
            *update, "automatic_download", false, issues);
        config.update.automatic_activation = optional_bool(
            *update, "automatic_activation", false, issues);
    }
    if (licensing != nullptr) {
        reject_unknown_fields(*licensing,
            {"entitlement_reference", "expiry_grace_hours"}, issues);
        config.licensing.entitlement_reference = required_text(
            *licensing, "entitlement_reference", issues);
        config.licensing.expiry_grace_hours = required_integer<std::uint32_t>(
            *licensing, "expiry_grace_hours", issues);
    }
    if (signed_manifest != nullptr) {
        reject_unknown_fields(*signed_manifest,
            {"manifest_reference", "sha256", "signature_reference"}, issues);
        config.signed_manifest = parse_manifest_fields(
            *signed_manifest, "", issues);
    }

    std::map<std::string, std::size_t> context_indexes;
    for (const auto& section : sections) {
        if (section.kind != "routing-context") continue;
        reject_unknown_fields(section,
            {"account_context", "routing_config_path", "journal_path",
             "checkpoint_path", "context_maximum_single_order_quantity",
             "context_maximum_single_order_notional",
             "context_maximum_open_quantity", "context_maximum_open_notional",
             "context_maximum_open_orders", "context_maximum_orders_per_second",
             "context_kill_switch_engaged", "policy_generation",
             "fee_generation", "metric_generation", "weight_generation",
             "routing_manifest_reference", "routing_sha256",
             "routing_signature_reference"}, issues);
        if (section.arguments.size() != 1U) {
            issue(issues, ConfigIssueCode::Syntax, "routing-context",
                  "section requires exactly one context ID", section.line);
            continue;
        }
        RoutingContextConfig context;
        context.context_id = section.arguments[0];
        context.account_context = required_text(section, "account_context", issues);
        context.routing_config_path = required_text(section, "routing_config_path", issues);
        context.journal_path = required_text(section, "journal_path", issues);
        context.checkpoint_path = required_text(section, "checkpoint_path", issues);
        context.context_risk = parse_risk_fields(section, "context_", issues);
        context.routing_generations.policy_generation =
            required_integer<std::uint64_t>(section, "policy_generation", issues);
        context.routing_generations.fee_generation =
            required_integer<std::uint64_t>(section, "fee_generation", issues);
        context.routing_generations.metric_generation =
            required_integer<std::uint64_t>(section, "metric_generation", issues);
        context.routing_generations.weight_generation =
            required_integer<std::uint64_t>(section, "weight_generation", issues);
        context.routing_generations.manifest = parse_manifest_fields(
            section, "routing_", issues);
        context_indexes.emplace(context.context_id, config.routing_contexts.size());
        config.routing_contexts.push_back(std::move(context));
    }
    for (const auto& section : sections) {
        if (section.kind != "ingress" && section.kind != "venue" &&
            section.kind != "account-risk" && section.kind != "client-risk") {
            continue;
        }
        if (section.arguments.size() != 2U) {
            issue(issues, ConfigIssueCode::Syntax, section.kind,
                  "section requires context ID and connector/venue ID", section.line);
            continue;
        }
        const auto context = context_indexes.find(section.arguments[0]);
        if (context == context_indexes.end()) {
            issue(issues, ConfigIssueCode::InvalidValue, section.kind,
                  "referenced routing context does not exist", section.line);
            continue;
        }
        auto& target = config.routing_contexts[context->second];
        if (section.kind == "account-risk" || section.kind == "client-risk") {
            reject_unknown_fields(section,
                {"maximum_single_order_quantity", "maximum_single_order_notional",
                 "maximum_open_quantity", "maximum_open_notional",
                 "maximum_open_orders", "maximum_orders_per_second",
                 "kill_switch_engaged"}, issues);
            ScopedRiskLimitConfig scoped;
            scoped.scope_id = section.arguments[1];
            scoped.limits = parse_risk_fields(section, "", issues);
            auto& limits = section.kind == "account-risk" ?
                target.account_risk_limits : target.client_risk_limits;
            limits.push_back(std::move(scoped));
        } else if (section.kind == "ingress") {
            reject_unknown_fields(section,
                {"protocol", "listen_address", "listen_port", "interface",
                 "credential_reference", "sender_comp_id", "target_comp_id",
                 "normal_credit_limit", "cancel_credit_limit"}, issues);
            IngressConnectorConfig ingress;
            ingress.connector_id = section.arguments[1];
            const auto value = required_text(section, "protocol", issues);
            const auto parsed = parse_ingress(value);
            if (!value.empty() && !parsed.has_value()) {
                issue(issues, ConfigIssueCode::InvalidValue, "ingress.protocol",
                      "expected SOBP_V1 or FIX44", find_field(section, "protocol")->line);
            } else if (parsed) {
                ingress.protocol = *parsed;
            }
            ingress.listen_address = required_text(section, "listen_address", issues);
            ingress.listen_port = required_integer<std::uint16_t>(section, "listen_port", issues);
            ingress.interface_name = required_text(section, "interface", issues);
            ingress.credential_reference = required_text(section, "credential_reference", issues);
            if (const auto* field = find_field(section, "sender_comp_id")) ingress.sender_comp_id = field->value;
            if (const auto* field = find_field(section, "target_comp_id")) ingress.target_comp_id = field->value;
            ingress.normal_credit_limit = required_integer<std::uint32_t>(
                section, "normal_credit_limit", issues);
            ingress.cancel_credit_limit = required_integer<std::uint32_t>(
                section, "cancel_credit_limit", issues);
            target.ingress_connectors.push_back(std::move(ingress));
        } else {
            reject_unknown_fields(section,
                {"venue_name", "connector_id", "connector_version", "connector_type",
                 "feed_a_host", "feed_a_port", "feed_a_interface",
                 "feed_b_host", "feed_b_port", "feed_b_interface",
                 "recovery_host", "recovery_port", "recovery_interface",
                 "order_entry_host", "order_entry_port", "order_entry_interface",
                 "quickfix_settings_reference", "credential_reference",
                 "dictionary_sha256", "connector_manifest_reference",
                 "connector_sha256", "connector_signature_reference",
                 "supports_reconciliation"}, issues);
            VenueConnectorConfig venue;
            const auto venue_id = parse_unsigned<std::uint32_t>(section.arguments[1]);
            if (!venue_id) {
                issue(issues, ConfigIssueCode::InvalidValue, "venue",
                      "venue ID must be an unsigned integer", section.line);
            } else {
                venue.venue_id = *venue_id;
            }
            venue.venue_name = required_text(section, "venue_name", issues);
            venue.connector_id = required_text(section, "connector_id", issues);
            venue.connector_version = required_text(section, "connector_version", issues);
            const auto connector_type = required_text(section, "connector_type", issues);
            const auto parsed_type = parse_connector_type(connector_type);
            if (!connector_type.empty() && !parsed_type.has_value()) {
                issue(issues, ConfigIssueCode::InvalidValue,
                      "venue.connector_type", "expected NATIVE_BINARY or FIX44",
                      find_field(section, "connector_type")->line);
            } else if (parsed_type) {
                venue.connector_type = *parsed_type;
            }
            venue.feed_a.host = required_text(section, "feed_a_host", issues);
            venue.feed_a.port = required_integer<std::uint16_t>(section, "feed_a_port", issues);
            venue.feed_a.interface_name = required_text(section, "feed_a_interface", issues);
            venue.feed_b.host = required_text(section, "feed_b_host", issues);
            venue.feed_b.port = required_integer<std::uint16_t>(section, "feed_b_port", issues);
            venue.feed_b.interface_name = required_text(section, "feed_b_interface", issues);
            venue.recovery.host = required_text(section, "recovery_host", issues);
            venue.recovery.port = required_integer<std::uint16_t>(section, "recovery_port", issues);
            venue.recovery.interface_name = required_text(section, "recovery_interface", issues);
            venue.order_entry_host = required_text(section, "order_entry_host", issues);
            venue.order_entry_port = required_integer<std::uint16_t>(section, "order_entry_port", issues);
            venue.order_entry_interface = required_text(section, "order_entry_interface", issues);
            venue.quickfix_settings_reference = required_text(section, "quickfix_settings_reference", issues);
            venue.credential_reference = required_text(section, "credential_reference", issues);
            venue.dictionary_sha256 = required_text(section, "dictionary_sha256", issues);
            venue.connector_manifest = parse_manifest_fields(
                section, "connector_", issues);
            venue.supports_reconciliation = optional_bool(
                section, "supports_reconciliation", false, issues);
            target.venues.push_back(std::move(venue));
        }
    }
    for (const auto& section : sections) {
        const bool known = section.kind == "device" || section.kind == "management" ||
            section.kind == "hardware" || section.kind == "time" ||
            section.kind == "capacities" || section.kind == "ha" ||
            section.kind == "telemetry" || section.kind == "recorder" ||
            section.kind == "update" || section.kind == "licensing" ||
            section.kind == "signed-manifest" ||
            section.kind == "routing-context" || section.kind == "ingress" ||
            section.kind == "venue" || section.kind == "account-risk" ||
            section.kind == "client-risk";
        if (!known) {
            issue(issues, ConfigIssueCode::UnknownSection, section.kind,
                  "unknown section", section.line);
        }
    }
    const auto validation = validate_device_config(config);
    issues.insert(issues.end(), validation.begin(), validation.end());
    if (!issues.empty()) {
        return ConfigDecodeResult{std::nullopt, std::move(issues)};
    }
    return ConfigDecodeResult{std::move(config), {}};
}

std::string LegacyIniMigrationCodec::encode(const DeviceConfigV2& config) const {
    std::ostringstream out;
    const auto emit_risk = [&out](
        const std::string_view prefix,
        const RiskLimitConfig& risk) {
        out << prefix << "maximum_single_order_quantity="
            << risk.maximum_single_order_quantity << '\n'
            << prefix << "maximum_single_order_notional="
            << risk.maximum_single_order_notional << '\n'
            << prefix << "maximum_open_quantity="
            << risk.maximum_open_quantity << '\n'
            << prefix << "maximum_open_notional="
            << risk.maximum_open_notional << '\n'
            << prefix << "maximum_open_orders="
            << risk.maximum_open_orders << '\n'
            << prefix << "maximum_orders_per_second="
            << risk.maximum_orders_per_second << '\n'
            << prefix << "kill_switch_engaged="
            << bool_text(risk.kill_switch_engaged) << '\n';
    };
    out << "[device]\n"
        << "schema_version=" << config.schema_version << '\n'
        << "device_id=" << config.device_id << '\n'
        << "hostname=" << config.hostname << '\n'
        << "deployment_mode=" << to_string(config.deployment_mode) << "\n\n"
        << "[management]\n"
        << "interface=" << config.management.interface_name << '\n'
        << "listen_address=" << config.management.listen_address << '\n'
        << "listen_port=" << config.management.listen_port << '\n'
        << "require_mutual_tls=" << bool_text(config.management.require_mutual_tls) << '\n'
        << "server_certificate_reference=" << config.management.server_certificate_reference << '\n'
        << "private_key_reference=" << config.management.private_key_reference << '\n'
        << "client_ca_reference=" << config.management.client_ca_reference << '\n'
        << "aaa_provider=" << config.management.aaa_provider << '\n'
        << "remote_support_enabled=" << bool_text(config.management.remote_support_enabled) << "\n\n"
        << "[hardware]\n"
        << "profile_id=" << config.hardware.profile_id << '\n'
        << "realtime_cores=" << join_values(config.hardware.realtime_cores) << '\n'
        << "management_cores=" << join_values(config.hardware.management_cores) << '\n'
        << "data_interfaces=" << join_values(config.hardware.data_interfaces) << '\n'
        << "audit_interfaces=" << join_values(config.hardware.audit_interfaces) << '\n'
        << "huge_pages_2mb=" << config.hardware.huge_pages_2mb << '\n'
        << "journal_device=" << config.hardware.journal_device << "\n\n"
        << "[capacities]\n"
        << "parent_order_pool=" << config.capacities.parent_order_pool << '\n'
        << "child_order_pool=" << config.capacities.child_order_pool << '\n'
        << "request_history_pool=" << config.capacities.request_history_pool << '\n'
        << "ingress_ring_entries=" << config.capacities.ingress_ring_entries << '\n'
        << "market_data_ring_entries=" << config.capacities.market_data_ring_entries << '\n'
        << "route_command_ring_entries=" << config.capacities.route_command_ring_entries << '\n'
        << "execution_ring_entries=" << config.capacities.execution_ring_entries << '\n'
        << "journal_ring_entries=" << config.capacities.journal_ring_entries << "\n\n"
        << "[time]\n"
        << "ptp_interface=" << config.time.ptp_interface << '\n'
        << "maximum_offset_ns=" << config.time.maximum_offset_ns << '\n'
        << "require_lock_before_routing=" << bool_text(config.time.require_lock_before_routing) << "\n\n"
        << "[ha]\n"
        << "mode=" << to_string(config.ha.mode) << '\n';
    if (!config.ha.node_id.empty()) out << "node_id=" << config.ha.node_id << '\n';
    if (!config.ha.peer_endpoint.empty()) out << "peer_endpoint=" << config.ha.peer_endpoint << '\n';
    if (!config.ha.witness_endpoint.empty()) out << "witness_endpoint=" << config.ha.witness_endpoint << '\n';
    if (!config.ha.replication_interface.empty()) out << "replication_interface=" << config.ha.replication_interface << '\n';
    out << "election_timeout_ms=" << config.ha.election_timeout_ms << "\n\n"
        << "[telemetry]\n"
        << "metrics_listen_address=" << config.telemetry.metrics_listen_address << '\n'
        << "metrics_listen_port=" << config.telemetry.metrics_listen_port << '\n';
    if (!config.telemetry.syslog_endpoint.empty()) out << "syslog_endpoint=" << config.telemetry.syslog_endpoint << '\n';
    out << "snmp_enabled=" << bool_text(config.telemetry.snmp_enabled) << '\n'
        << "vendor_upload_enabled=" << bool_text(config.telemetry.vendor_upload_enabled) << "\n\n"
        << "[recorder]\n"
        << "enabled=" << bool_text(config.recorder.enabled) << '\n'
        << "storage_path=" << config.recorder.storage_path << '\n'
        << "audit_interface_name=" << config.recorder.audit_interface_name << '\n'
        << "audit_listen_address=" << config.recorder.audit_listen_address << '\n'
        << "audit_listen_port=" << config.recorder.audit_listen_port << '\n'
        << "audit_require_mutual_tls="
        << bool_text(config.recorder.audit_require_mutual_tls) << '\n'
        << "audit_server_certificate_reference="
        << config.recorder.audit_server_certificate_reference << '\n'
        << "audit_private_key_reference="
        << config.recorder.audit_private_key_reference << '\n'
        << "audit_client_ca_reference="
        << config.recorder.audit_client_ca_reference << '\n'
        << "retention_seconds=" << config.recorder.retention_seconds << '\n'
        << "qualified_maximum_bytes_per_second="
        << config.recorder.qualified_maximum_bytes_per_second << '\n'
        << "tier0_usable_bytes=" << config.recorder.tier0_usable_bytes << '\n'
        << "tier0_cancel_reserved_bytes="
        << config.recorder.tier0_cancel_reserved_bytes << '\n'
        << "tier0_reconciliation_reserved_bytes="
        << config.recorder.tier0_reconciliation_reserved_bytes << '\n'
        << "maximum_storage_bytes=" << config.recorder.maximum_storage_bytes << "\n\n"
        << "[update]\n"
        << "channel=" << config.update.channel << '\n'
        << "trust_root_reference=" << config.update.trust_root_reference << '\n'
        << "image_manifest_reference=" << config.update.image_manifest.manifest_reference << '\n'
        << "image_sha256=" << config.update.image_manifest.sha256 << '\n'
        << "image_signature_reference=" << config.update.image_manifest.signature_reference << '\n'
        << "automatic_download=" << bool_text(config.update.automatic_download) << '\n'
        << "automatic_activation=" << bool_text(config.update.automatic_activation) << "\n\n"
        << "[licensing]\n"
        << "entitlement_reference=" << config.licensing.entitlement_reference << '\n'
        << "expiry_grace_hours=" << config.licensing.expiry_grace_hours << "\n\n"
        << "[signed-manifest]\n"
        << "manifest_reference=" << config.signed_manifest.manifest_reference << '\n'
        << "sha256=" << config.signed_manifest.sha256 << '\n'
        << "signature_reference=" << config.signed_manifest.signature_reference << "\n\n";
    for (const auto& context : config.routing_contexts) {
        out << "[routing-context " << context.context_id << "]\n"
            << "account_context=" << context.account_context << '\n'
            << "routing_config_path=" << context.routing_config_path << '\n'
            << "journal_path=" << context.journal_path << '\n'
            << "checkpoint_path=" << context.checkpoint_path << '\n';
        emit_risk("context_", context.context_risk);
        out << "policy_generation=" << context.routing_generations.policy_generation << '\n'
            << "fee_generation=" << context.routing_generations.fee_generation << '\n'
            << "metric_generation=" << context.routing_generations.metric_generation << '\n'
            << "weight_generation=" << context.routing_generations.weight_generation << '\n'
            << "routing_manifest_reference="
            << context.routing_generations.manifest.manifest_reference << '\n'
            << "routing_sha256=" << context.routing_generations.manifest.sha256 << '\n'
            << "routing_signature_reference="
            << context.routing_generations.manifest.signature_reference << "\n\n";
        for (const auto& scoped : context.account_risk_limits) {
            out << "[account-risk " << context.context_id << ' '
                << scoped.scope_id << "]\n";
            emit_risk("", scoped.limits);
            out << '\n';
        }
        for (const auto& scoped : context.client_risk_limits) {
            out << "[client-risk " << context.context_id << ' '
                << scoped.scope_id << "]\n";
            emit_risk("", scoped.limits);
            out << '\n';
        }
        for (const auto& ingress : context.ingress_connectors) {
            out << "[ingress " << context.context_id << ' ' << ingress.connector_id << "]\n"
                << "protocol=" << to_string(ingress.protocol) << '\n'
                << "listen_address=" << ingress.listen_address << '\n'
                << "listen_port=" << ingress.listen_port << '\n'
                << "interface=" << ingress.interface_name << '\n'
                << "credential_reference=" << ingress.credential_reference << '\n';
            if (!ingress.sender_comp_id.empty()) out << "sender_comp_id=" << ingress.sender_comp_id << '\n';
            if (!ingress.target_comp_id.empty()) out << "target_comp_id=" << ingress.target_comp_id << '\n';
            out << "normal_credit_limit=" << ingress.normal_credit_limit << '\n'
                << "cancel_credit_limit=" << ingress.cancel_credit_limit << "\n\n";
        }
        for (const auto& venue : context.venues) {
            out << "[venue " << context.context_id << ' ' << venue.venue_id << "]\n"
                << "venue_name=" << venue.venue_name << '\n'
                << "connector_id=" << venue.connector_id << '\n'
                << "connector_version=" << venue.connector_version << '\n'
                << "connector_type=" << to_string(venue.connector_type) << '\n'
                << "feed_a_host=" << venue.feed_a.host << '\n'
                << "feed_a_port=" << venue.feed_a.port << '\n'
                << "feed_a_interface=" << venue.feed_a.interface_name << '\n'
                << "feed_b_host=" << venue.feed_b.host << '\n'
                << "feed_b_port=" << venue.feed_b.port << '\n'
                << "feed_b_interface=" << venue.feed_b.interface_name << '\n'
                << "recovery_host=" << venue.recovery.host << '\n'
                << "recovery_port=" << venue.recovery.port << '\n'
                << "recovery_interface=" << venue.recovery.interface_name << '\n'
                << "order_entry_host=" << venue.order_entry_host << '\n'
                << "order_entry_port=" << venue.order_entry_port << '\n'
                << "order_entry_interface=" << venue.order_entry_interface << '\n'
                << "quickfix_settings_reference=" << venue.quickfix_settings_reference << '\n'
                << "credential_reference=" << venue.credential_reference << '\n'
                << "dictionary_sha256=" << venue.dictionary_sha256 << '\n'
                << "connector_manifest_reference="
                << venue.connector_manifest.manifest_reference << '\n'
                << "connector_sha256=" << venue.connector_manifest.sha256 << '\n'
                << "connector_signature_reference="
                << venue.connector_manifest.signature_reference << '\n'
                << "supports_reconciliation=" << bool_text(venue.supports_reconciliation) << "\n\n";
        }
    }
    return out.str();
}

namespace {

[[nodiscard]] std::string yaml_quote(const std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2U);
    output.push_back('"');
    for (const char item : value) {
        switch (item) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output.push_back(item); break;
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::optional<std::string> yaml_unquote(
    const std::string_view encoded) {
    const auto value = trim(encoded);
    if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
        return std::nullopt;
    }
    std::string output;
    output.reserve(value.size() - 2U);
    for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
        const char item = value[index];
        if (item != '\\') {
            if (static_cast<unsigned char>(item) < 0x20U) return std::nullopt;
            output.push_back(item);
            continue;
        }
        if (++index + 1U >= value.size()) return std::nullopt;
        switch (value[index]) {
            case '\\': output.push_back('\\'); break;
            case '"': output.push_back('"'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            default: return std::nullopt;
        }
    }
    return output;
}

[[nodiscard]] std::optional<std::size_t> yaml_mapping_separator(
    const std::string_view line) noexcept {
    if (line.empty() || line.front() != '"') return std::nullopt;
    bool escaped = false;
    for (std::size_t index = 1U; index < line.size(); ++index) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (line[index] == '\\') {
            escaped = true;
        } else if (line[index] == '"') {
            std::size_t cursor = index + 1U;
            while (cursor < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[cursor])) != 0) {
                ++cursor;
            }
            if (cursor < line.size() && line[cursor] == ':') return cursor;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string YamlDeviceConfigCodec::encode(const DeviceConfigV2& config) const {
    LegacyIniMigrationCodec legacy;
    const auto ini = legacy.encode(config);
    std::ostringstream output;
    output << "---\nformat: \"SORX_DEVICE_CONFIG_V2\"\n";
    std::string section;
    std::size_t begin = 0U;
    while (begin <= ini.size()) {
        const auto end = ini.find('\n', begin);
        const auto line = trim(std::string_view{ini}.substr(
            begin, end == ini.npos ? ini.size() - begin : end - begin));
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            section = std::string{line.substr(1U, line.size() - 2U)};
        } else if (!line.empty()) {
            const auto equals = line.find('=');
            if (equals != line.npos) {
                output << yaml_quote(section + "/" +
                                     std::string{line.substr(0U, equals)})
                       << ": " << yaml_quote(line.substr(equals + 1U)) << '\n';
            }
        }
        if (end == ini.npos) break;
        begin = end + 1U;
    }
    return output.str();
}

ConfigDecodeResult YamlDeviceConfigCodec::decode(
    const std::string_view document) const {
    std::vector<ConfigIssue> issues;
    std::vector<std::string> section_order;
    std::map<std::string, std::map<std::string, RawField>> sections;
    bool format_seen = false;
    std::size_t line_number = 0U;
    std::size_t begin = 0U;
    while (begin <= document.size()) {
        const auto end = document.find('\n', begin);
        ++line_number;
        auto line = trim(document.substr(
            begin, end == document.npos ? document.size() - begin : end - begin));
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (!line.empty() && line != "---" && line.front() != '#') {
            if (line.starts_with("format:")) {
                if (format_seen) {
                    issue(issues, ConfigIssueCode::DuplicateField, "format",
                          "duplicate YAML format marker", line_number);
                } else {
                    const auto decoded = yaml_unquote(line.substr(7U));
                    if (!decoded || *decoded != "SORX_DEVICE_CONFIG_V2") {
                        issue(issues, ConfigIssueCode::InvalidValue, "format",
                              "expected SORX_DEVICE_CONFIG_V2", line_number);
                    }
                    format_seen = true;
                }
            } else {
                const auto separator = yaml_mapping_separator(line);
                if (!separator) {
                    issue(issues, ConfigIssueCode::Syntax, "",
                          "expected a quoted flat YAML key and quoted scalar value",
                          line_number);
                } else {
                    const auto key = yaml_unquote(line.substr(0U, *separator));
                    const auto value = yaml_unquote(line.substr(*separator + 1U));
                    if (!key || !value || value->find_first_of("\r\n") != value->npos) {
                        issue(issues, ConfigIssueCode::Syntax, "",
                              "invalid or multiline YAML scalar", line_number);
                    } else {
                        const auto slash = key->rfind('/');
                        if (slash == key->npos || slash == 0U ||
                            slash + 1U == key->size()) {
                            issue(issues, ConfigIssueCode::UnknownField, *key,
                                  "flat key must be section/field", line_number);
                        } else {
                            const auto section = key->substr(0U, slash);
                            const auto field = key->substr(slash + 1U);
                            if (!sections.contains(section)) {
                                section_order.push_back(section);
                            }
                            auto& fields = sections[section];
                            if (!fields.emplace(
                                    field, RawField{*value, line_number}).second) {
                                issue(issues, ConfigIssueCode::DuplicateField, *key,
                                      "duplicate YAML key", line_number);
                            }
                        }
                    }
                }
            }
        }
        if (end == document.npos) break;
        begin = end + 1U;
    }
    if (!format_seen) {
        issue(issues, ConfigIssueCode::MissingRequired, "format",
              "YAML format marker is required");
    }
    if (!issues.empty()) {
        return ConfigDecodeResult{std::nullopt, std::move(issues)};
    }
    std::ostringstream legacy_document;
    for (const auto& section : section_order) {
        legacy_document << '[' << section << "]\n";
        for (const auto& [field, value] : sections.at(section)) {
            legacy_document << field << '=' << value.value << '\n';
        }
        legacy_document << '\n';
    }
    LegacyIniMigrationCodec legacy;
    return legacy.decode(legacy_document.str());
}

std::string device_config_sha256(const DeviceConfigV2& config) {
    const YamlDeviceConfigCodec codec;
    return sha256(codec.encode(config));
}

ConfigStore::ConfigStore(
    DeviceConfigV2 initial,
    AuditMetadata bootstrap_metadata) {
    const auto validation = validate_device_config(initial);
    if (!validation.empty()) {
        throw std::invalid_argument(
            "initial DeviceConfigV2 is invalid: " + validation.front().message);
    }
    if (!valid_audit(bootstrap_metadata)) {
        throw std::invalid_argument("bootstrap audit metadata is incomplete");
    }
    running_ = VersionedDeviceConfig{1U, std::move(initial)};
    candidate_ = running_;
    startup_ = running_;
    history_.push_back(running_);
    append_audit(
        AuditOperation::Bootstrap,
        std::move(bootstrap_metadata),
        true,
        0U,
        1U,
        {},
        "initial running and startup configuration installed");
}

ConfigStoreResult ConfigStore::stage_candidate(
    DeviceConfigV2 candidate,
    AuditMetadata metadata) {
    const auto from = running_.revision;
    if (!valid_audit(metadata)) {
        append_audit(AuditOperation::StageCandidate, std::move(metadata), false,
                     from, from, {}, "audit metadata is incomplete");
        return ConfigStoreResult{
            ConfigStoreCode::InvalidAuditMetadata, from, {}, {},
            "audit metadata is incomplete"};
    }
    if (pending_commit_.has_value()) {
        append_audit(AuditOperation::StageCandidate, std::move(metadata), false,
                     from, from, {},
                     "cannot stage while a commit is awaiting confirmation");
        return ConfigStoreResult{
            ConfigStoreCode::PendingConfirmationExists, from, {}, {},
            "cannot stage while a commit is awaiting confirmation"};
    }
    auto validation = validate_device_config(candidate);
    if (!validation.empty()) {
        append_audit(AuditOperation::StageCandidate, std::move(metadata), false,
                     from, from, {}, "candidate validation failed");
        return ConfigStoreResult{
            ConfigStoreCode::ValidationFailed,
            from,
            {},
            std::move(validation),
            "candidate validation failed"};
    }
    candidate_ = VersionedDeviceConfig{next_revision_++, std::move(candidate)};
    candidate_dirty_ = true;
    auto diff = diff_device_config(running_.config, candidate_.config);
    append_audit(AuditOperation::StageCandidate, std::move(metadata), true,
                 from, candidate_.revision, diff, "candidate staged");
    return ConfigStoreResult{
        ConfigStoreCode::Success, candidate_.revision, std::move(diff), {},
        "candidate staged"};
}

ConfigStoreResult ConfigStore::commit_candidate(
    const CommitPolicy policy,
    AuditMetadata metadata) {
    if (!candidate_dirty_) {
        append_audit(AuditOperation::CommitCandidate, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "no candidate is staged");
        return ConfigStoreResult{
            ConfigStoreCode::CandidateNotStaged,
            running_.revision,
            {},
            {},
            "no candidate is staged"};
    }
    return commit_version(
        candidate_, candidate_.revision, policy, std::move(metadata),
        AuditOperation::CommitCandidate);
}

ConfigStoreResult ConfigStore::commit_version(
    VersionedDeviceConfig versioned,
    const std::uint64_t authorization_candidate_revision,
    const CommitPolicy policy,
    AuditMetadata metadata,
    const AuditOperation operation) {
    const auto from = running_.revision;
    if (pending_commit_.has_value()) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "an earlier commit is awaiting confirmation",
                     policy.secondary_authorization);
        return ConfigStoreResult{
            ConfigStoreCode::PendingConfirmationExists, from, {}, {},
            "an earlier commit is awaiting confirmation"};
    }
    if (!valid_audit(metadata)) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "audit metadata is incomplete");
        return ConfigStoreResult{
            ConfigStoreCode::InvalidAuditMetadata, from, {}, {},
            "audit metadata is incomplete"};
    }
    if (policy.expected_running_revision == 0U ||
        policy.expected_running_revision != running_.revision) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "an exact nonzero running revision is required",
                     policy.secondary_authorization);
        return ConfigStoreResult{
            ConfigStoreCode::RevisionConflict, from, {}, {},
            "an exact nonzero running revision is required"};
    }
    if (policy.expected_candidate_revision == 0U ||
        policy.expected_candidate_revision !=
            authorization_candidate_revision) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "candidate revision changed",
                     policy.secondary_authorization);
        return ConfigStoreResult{
            ConfigStoreCode::CandidateRevisionConflict, from, {}, {},
            "candidate revision changed"};
    }
    const std::string candidate_hash = device_config_sha256(versioned.config);
    if (!sha256_hex(policy.expected_candidate_sha256) ||
        policy.expected_candidate_sha256 != candidate_hash) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "candidate hash changed",
                     policy.secondary_authorization);
        return ConfigStoreResult{
            ConfigStoreCode::CandidateHashMismatch, from, {}, {},
            "candidate hash changed"};
    }
    auto validation = validate_device_config(versioned.config);
    if (!validation.empty()) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "configuration validation failed");
        return ConfigStoreResult{
            ConfigStoreCode::ValidationFailed, from, {},
            std::move(validation), "configuration validation failed"};
    }
    if (routing_generation_regressed(running_.config, versioned.config)) {
        append_audit(operation, std::move(metadata), false, from, from, {},
                     "routing policy, fee, metric, and weight generations are monotonic");
        return ConfigStoreResult{
            ConfigStoreCode::GenerationRegression, from, {}, {},
            "routing policy, fee, metric, and weight generations are monotonic"};
    }
    auto diff = diff_device_config(running_.config, versioned.config);
    if (needs_dual_authorization(diff)) {
        const auto& secondary = policy.secondary_authorization;
        const std::string diff_hash = config_diff_sha256(diff);
        if (!risk_authorizer(metadata) || !secondary.has_value() ||
            !secondary->authenticated ||
            !risk_authorizer(secondary->approver) ||
            secondary->approver.principal == metadata.principal ||
            secondary->approver.request_id == metadata.request_id ||
            secondary->action != operation ||
            secondary->running_revision != from ||
            secondary->candidate_revision !=
                authorization_candidate_revision ||
            !sha256_hex(secondary->candidate_sha256) ||
            secondary->candidate_sha256 != candidate_hash ||
            !sha256_hex(secondary->diff_sha256) ||
            secondary->diff_sha256 != diff_hash ||
            policy.monotonic_now_ns == 0U ||
            secondary->expires_at_monotonic_ns == 0U) {
            append_audit(
                operation, std::move(metadata), false, from, from, diff,
                "dual authorization is missing, unauthenticated, or not bound to this exact action, revision, hash, and diff",
                secondary);
            return ConfigStoreResult{
                ConfigStoreCode::DualAuthorizationRequired, from,
                std::move(diff), {},
                "dual authorization is missing, unauthenticated, or not bound to this exact action, revision, hash, and diff"};
        }
        if (policy.monotonic_now_ns >
            secondary->expires_at_monotonic_ns) {
            append_audit(
                operation, std::move(metadata), false, from, from, diff,
                "dual authorization has expired", secondary);
            return ConfigStoreResult{
                ConfigStoreCode::AuthorizationExpired, from,
                std::move(diff), {}, "dual authorization has expired"};
        }
        const bool replayed = std::any_of(
            audit_log_.begin(), audit_log_.end(),
            [&secondary](const AuditRecord& record) {
                return record.accepted &&
                    record.secondary_authorization.has_value() &&
                    record.secondary_authorization->approver.request_id ==
                        secondary->approver.request_id;
            });
        if (replayed) {
            append_audit(
                operation, std::move(metadata), false, from, from, diff,
                "dual authorization approval ID was already consumed",
                secondary);
            return ConfigStoreResult{
                ConfigStoreCode::AuthorizationReplay, from,
                std::move(diff), {},
                "dual authorization approval ID was already consumed"};
        }
    }
    const auto permission = policy_error(diff, policy);
    if (permission != ConfigStoreCode::Success) {
        append_audit(operation, std::move(metadata), false, from, from, diff,
                     "commit policy does not permit the required lifecycle action");
        return ConfigStoreResult{
            permission, from, std::move(diff), {},
            "commit policy does not permit the required lifecycle action"};
    }
    if (changes_management_plane(diff) &&
        policy.confirmation_timeout_ns == 0U) {
        append_audit(
            operation, std::move(metadata), false, from, from, diff,
            "management-plane changes require commit-confirmed rollback",
            policy.secondary_authorization);
        return ConfigStoreResult{
            ConfigStoreCode::InvalidCommitPolicy, from, std::move(diff), {},
            "management-plane changes require commit-confirmed rollback"};
    }
    if (policy.confirmation_timeout_ns != 0U &&
        (policy.monotonic_now_ns == 0U ||
         policy.confirmation_timeout_ns >
             std::numeric_limits<std::uint64_t>::max() -
                 policy.monotonic_now_ns)) {
        append_audit(operation, std::move(metadata), false, from, from, diff,
                     "commit-confirmed requires a valid monotonic start and non-overflowing timeout",
                     policy.secondary_authorization);
        return ConfigStoreResult{
            ConfigStoreCode::InvalidCommitPolicy, from, std::move(diff), {},
            "commit-confirmed requires a valid monotonic start and non-overflowing timeout"};
    }
    const auto rollback_version = running_;
    running_ = std::move(versioned);
    candidate_ = running_;
    candidate_dirty_ = false;
    history_.push_back(running_);
    if (policy.confirmation_timeout_ns != 0U) {
        pending_commit_ = PendingCommitState{
            PendingCommitStatus{
                running_.revision,
                rollback_version.revision,
                policy.monotonic_now_ns + policy.confirmation_timeout_ns,
                policy.promote_to_startup},
            rollback_version};
    } else if (policy.promote_to_startup) {
        startup_ = running_;
    }
    const auto accepted_revision = running_.revision;
    append_audit(operation, std::move(metadata), true, from,
                 accepted_revision, diff,
                 policy.confirmation_timeout_ns == 0U ?
                    "configuration committed" :
                    "configuration committed pending confirmation",
                 policy.secondary_authorization);
    return ConfigStoreResult{
        ConfigStoreCode::Success,
        accepted_revision,
        std::move(diff),
        {},
        policy.confirmation_timeout_ns == 0U ?
            "configuration committed" :
            "configuration committed pending confirmation"};
}

ConfigStoreResult ConfigStore::discard_candidate(AuditMetadata metadata) {
    const auto candidate_revision = candidate_.revision;
    if (!valid_audit(metadata)) {
        append_audit(AuditOperation::DiscardCandidate, std::move(metadata), false,
                     running_.revision, candidate_revision, {},
                     "audit metadata is incomplete");
        return ConfigStoreResult{
            ConfigStoreCode::InvalidAuditMetadata, running_.revision, {}, {},
            "audit metadata is incomplete"};
    }
    candidate_ = running_;
    candidate_dirty_ = false;
    append_audit(AuditOperation::DiscardCandidate, std::move(metadata), true,
                 candidate_revision, running_.revision, {}, "candidate discarded");
    return ConfigStoreResult{
        ConfigStoreCode::Success, running_.revision, {}, {},
        "candidate discarded"};
}

ConfigStoreResult ConfigStore::save_startup(AuditMetadata metadata) {
    if (pending_commit_.has_value()) {
        append_audit(AuditOperation::SaveStartup, std::move(metadata), false,
                     startup_.revision, startup_.revision, {},
                     "cannot save an unconfirmed running revision");
        return ConfigStoreResult{
            ConfigStoreCode::PendingConfirmationExists, startup_.revision,
            {}, {}, "cannot save an unconfirmed running revision"};
    }
    if (!valid_audit(metadata)) {
        append_audit(AuditOperation::SaveStartup, std::move(metadata), false,
                     startup_.revision, startup_.revision, {},
                     "audit metadata is incomplete");
        return ConfigStoreResult{
            ConfigStoreCode::InvalidAuditMetadata, startup_.revision, {}, {},
            "audit metadata is incomplete"};
    }
    const auto from = startup_.revision;
    startup_ = running_;
    append_audit(AuditOperation::SaveStartup, std::move(metadata), true,
                 from, startup_.revision, {}, "running saved as startup");
    return ConfigStoreResult{
        ConfigStoreCode::Success, startup_.revision, {}, {},
        "running saved as startup"};
}

ConfigStoreResult ConfigStore::rollback(
    const std::uint64_t target_revision,
    const CommitPolicy policy,
    AuditMetadata metadata) {
    const auto found = std::find_if(
        history_.begin(), history_.end(),
        [target_revision](const auto& item) {
            return item.revision == target_revision;
        });
    if (found == history_.end()) {
        append_audit(AuditOperation::Rollback, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "target revision is not retained");
        return ConfigStoreResult{
            ConfigStoreCode::RevisionNotFound, running_.revision, {}, {},
            "target revision is not retained"};
    }
    VersionedDeviceConfig rollback_version{
        next_revision_++, found->config};
    return commit_version(
        std::move(rollback_version), target_revision, policy,
        std::move(metadata),
        AuditOperation::Rollback);
}

ConfigStoreResult ConfigStore::confirm_commit(
    const std::uint64_t revision,
    const std::uint64_t monotonic_now_ns,
    AuditMetadata metadata) {
    if (!valid_audit(metadata)) {
        append_audit(AuditOperation::ConfirmCommit, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "audit metadata is incomplete");
        return ConfigStoreResult{
            ConfigStoreCode::InvalidAuditMetadata, running_.revision, {}, {},
            "audit metadata is incomplete"};
    }
    if (!pending_commit_) {
        append_audit(AuditOperation::ConfirmCommit, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "no commit is awaiting confirmation");
        return ConfigStoreResult{
            ConfigStoreCode::NoPendingConfirmation, running_.revision, {}, {},
            "no commit is awaiting confirmation"};
    }
    if (pending_commit_->status.revision != revision) {
        append_audit(AuditOperation::ConfirmCommit, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "confirmation revision does not match pending commit");
        return ConfigStoreResult{
            ConfigStoreCode::ConfirmationRevisionMismatch, running_.revision,
            {}, {}, "confirmation revision does not match pending commit"};
    }
    if (monotonic_now_ns > pending_commit_->status.deadline_monotonic_ns) {
        append_audit(AuditOperation::ConfirmCommit, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "confirmation deadline has expired");
        return ConfigStoreResult{
            ConfigStoreCode::ConfirmationExpired, running_.revision, {}, {},
            "confirmation deadline has expired"};
    }
    const auto promote =
        pending_commit_->status.promote_to_startup_on_confirmation;
    pending_commit_.reset();
    if (promote) startup_ = running_;
    append_audit(AuditOperation::ConfirmCommit, std::move(metadata), true,
                 running_.revision, running_.revision, {},
                 "pending commit confirmed");
    return ConfigStoreResult{
        ConfigStoreCode::Success, running_.revision, {}, {},
        "pending commit confirmed"};
}

ConfigStoreResult ConfigStore::rollback_unconfirmed(
    const std::uint64_t monotonic_now_ns,
    AuditMetadata metadata) {
    if (!pending_commit_) {
        if (!valid_audit(metadata)) {
            append_audit(
                AuditOperation::AutomaticRollback, std::move(metadata), false,
                running_.revision, running_.revision, {},
                "audit metadata is incomplete");
            return ConfigStoreResult{
                ConfigStoreCode::InvalidAuditMetadata, running_.revision, {}, {},
                "audit metadata is incomplete"};
        }
        append_audit(AuditOperation::AutomaticRollback, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "no commit is awaiting confirmation");
        return ConfigStoreResult{
            ConfigStoreCode::NoPendingConfirmation, running_.revision, {}, {},
            "no commit is awaiting confirmation"};
    }
    if (monotonic_now_ns <= pending_commit_->status.deadline_monotonic_ns) {
        if (!valid_audit(metadata)) {
            append_audit(
                AuditOperation::AutomaticRollback, std::move(metadata), false,
                running_.revision, running_.revision, {},
                "audit metadata is incomplete");
            return ConfigStoreResult{
                ConfigStoreCode::InvalidAuditMetadata, running_.revision, {}, {},
                "audit metadata is incomplete"};
        }
        append_audit(AuditOperation::AutomaticRollback, std::move(metadata), false,
                     running_.revision, running_.revision, {},
                     "confirmation deadline has not expired");
        return ConfigStoreResult{
            ConfigStoreCode::ConfirmationNotExpired, running_.revision, {}, {},
            "confirmation deadline has not expired"};
    }
    const bool supplied_metadata_invalid = !valid_audit(metadata);
    if (supplied_metadata_invalid) {
        metadata = AuditMetadata{
            "sorx-watchdog",
            "system",
            "internal-watchdog",
            "automatic-rollback-" + std::to_string(running_.revision),
            "expired commit rolled back after invalid caller audit metadata",
            monotonic_now_ns};
    }
    const auto from = running_.revision;
    VersionedDeviceConfig rollback_version{
        next_revision_++, pending_commit_->rollback_version.config};
    const auto diff = diff_device_config(
        running_.config, rollback_version.config);
    running_ = std::move(rollback_version);
    candidate_ = running_;
    candidate_dirty_ = false;
    history_.push_back(running_);
    pending_commit_.reset();
    append_audit(AuditOperation::AutomaticRollback, std::move(metadata), true,
                 from, running_.revision, diff,
                 supplied_metadata_invalid ?
                     "unconfirmed commit automatically rolled back; invalid caller audit metadata replaced by system identity" :
                     "unconfirmed commit automatically rolled back");
    return ConfigStoreResult{
        ConfigStoreCode::Success, running_.revision, diff, {},
        "unconfirmed commit automatically rolled back"};
}

std::optional<PendingCommitStatus> ConfigStore::pending_commit() const noexcept {
    if (!pending_commit_) return std::nullopt;
    return pending_commit_->status;
}

void ConfigStore::append_audit(
    const AuditOperation operation,
    AuditMetadata metadata,
    const bool accepted,
    const std::uint64_t from_revision,
    const std::uint64_t to_revision,
    ConfigDiff diff,
    std::string detail,
    std::optional<DualAuthorizationCapability> secondary_authorization) {
    audit_log_.push_back(AuditRecord{
        next_audit_sequence_++,
        operation,
        std::move(metadata),
        accepted,
        from_revision,
        to_revision,
        std::move(diff),
        std::move(detail),
        std::move(secondary_authorization),
    });
}

std::string_view to_string(const DeploymentMode value) noexcept {
    switch (value) {
        case DeploymentMode::Development: return "DEVELOPMENT";
        case DeploymentMode::Certification: return "CERTIFICATION";
        case DeploymentMode::Production: return "PRODUCTION";
    }
    return "UNKNOWN";
}

std::string_view to_string(const HaMode value) noexcept {
    switch (value) {
        case HaMode::Standalone: return "STANDALONE";
        case HaMode::ActiveStandbySynchronous: return "ACTIVE_STANDBY_SYNC";
        case HaMode::ActiveStandbyAsynchronous: return "ACTIVE_STANDBY_ASYNC";
    }
    return "UNKNOWN";
}

std::string_view to_string(const IngressProtocol value) noexcept {
    switch (value) {
        case IngressProtocol::SobpV1: return "SOBP_V1";
        case IngressProtocol::Fix44: return "FIX44";
    }
    return "UNKNOWN";
}

std::string_view to_string(const VenueConnectorType value) noexcept {
    switch (value) {
        case VenueConnectorType::NativeBinary: return "NATIVE_BINARY";
        case VenueConnectorType::Fix44: return "FIX44";
    }
    return "UNKNOWN";
}

std::string_view to_string(const ChangeClass value) noexcept {
    switch (value) {
        case ChangeClass::LiveAtomic: return "LIVE_ATOMIC";
        case ChangeClass::DualAuthorized: return "DUAL_AUTHORIZED";
        case ChangeClass::DrainReconcileRestart:
            return "DRAIN_RECONCILE_RESTART";
        case ChangeClass::MaintenanceReboot: return "MAINTENANCE_REBOOT";
    }
    return "UNKNOWN";
}

}  // namespace sorx::appliance
