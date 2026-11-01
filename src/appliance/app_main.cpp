#include "sorx/appliance/app_main.hpp"

#include "sorx/appliance/device_config.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace sorx::appliance {
namespace {

[[nodiscard]] std::string_view app_name(const AppKind kind) noexcept {
    switch (kind) {
        case AppKind::DataPlane: return "sorxd";
        case AppKind::FixGateway: return "sorx-fixgw";
        case AppKind::AuditDaemon: return "sorx-auditd";
        case AppKind::ManagementDaemon: return "sorx-mgmtd";
        case AppKind::ControlCli: return "sorxctl";
    }
    return "sorx";
}

void print_help(const AppKind kind) {
    const auto name = app_name(kind);
    std::cout
        << "usage: " << name << " [--help] [--version] [--check-config PATH]\n"
        << "\n"
        << "This is the safe appliance scaffold. --check-config parses and\n"
        << "validates canonical YAML DeviceConfigV2 without starting threads or opening sockets.\n"
        << "Runtime/network activation is intentionally unavailable until the\n"
        << "supervisor, authenticated control plane, and connector wiring land.\n";
    if (kind == AppKind::ControlCli) {
        std::cout
            << "\n"
            << "       sorxctl --migrate-ini INPUT.ini OUTPUT.yaml\n"
            << "\n"
            << "Migrate a complete legacy appliance INI document to canonical\n"
            << "DeviceConfigV2 YAML. Existing output files are never replaced.\n";
    }
}

void print_config_issues(
    const std::string& path,
    const std::vector<ConfigIssue>& issues) {
    for (const auto& issue : issues) {
        std::cerr << path;
        if (issue.line != 0U) std::cerr << ':' << issue.line;
        if (!issue.path.empty()) std::cerr << " [" << issue.path << ']';
        std::cerr << ": " << issue.message << '\n';
    }
}

[[nodiscard]] int check_config(
    const std::string_view name,
    const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << name << ": cannot open config: " << path << '\n';
        return 2;
    }
    const std::string document{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    YamlDeviceConfigCodec codec;
    auto decoded = codec.decode(document);
    if (!decoded) {
        print_config_issues(path, decoded.issues);
        return 2;
    }
    std::cout << name << ": valid DeviceConfigV2"
              << " device=" << decoded.config->device_id
              << " contexts=" << decoded.config->routing_contexts.size()
              << " ha=" << to_string(decoded.config->ha.mode) << '\n';
    return 0;
}

[[nodiscard]] int migrate_ini(
    const std::string& input_path,
    const std::string& output_path) {
    namespace fs = std::filesystem;
    if (input_path.empty() || output_path.empty()) {
        std::cerr << "sorxctl: input and output paths must be nonempty\n";
        return 2;
    }
    std::error_code error;
    if (fs::exists(output_path, error) || error) {
        std::cerr << "sorxctl: output already exists or cannot be inspected: "
                  << output_path << '\n';
        return 2;
    }
    if (input_path == output_path) {
        std::cerr << "sorxctl: input and output paths must differ\n";
        return 2;
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        std::cerr << "sorxctl: cannot open legacy config: " << input_path << '\n';
        return 2;
    }
    const std::string document{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};

    LegacyIniMigrationCodec legacy;
    auto decoded = legacy.decode(document);
    if (!decoded) {
        print_config_issues(input_path, decoded.issues);
        return 2;
    }

    YamlDeviceConfigCodec yaml;
    const auto migrated = yaml.encode(*decoded.config);
    const auto round_trip = yaml.decode(migrated);
    if (!round_trip ||
        *round_trip.config != *decoded.config ||
        route_critical_sha256(*round_trip.config) !=
            route_critical_sha256(*decoded.config)) {
        std::cerr << "sorxctl: internal migration verification failed\n";
        return 70;
    }

    const fs::path temporary = fs::path{output_path}.concat(".sorxctl.tmp");
    error.clear();
    if (fs::exists(temporary, error) || error) {
        std::cerr << "sorxctl: temporary output already exists or cannot be inspected: "
                  << temporary.string() << '\n';
        return 2;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(migrated.data(), static_cast<std::streamsize>(migrated.size()));
        output.flush();
        if (!output) {
            output.close();
            fs::remove(temporary, error);
            std::cerr << "sorxctl: cannot write migrated config: "
                      << temporary.string() << '\n';
            return 2;
        }
    }
    // Publish without a check-then-rename overwrite race. The temporary lives
    // beside the destination, so a hard link is same-filesystem and fails if
    // another writer created the destination after the exists() check.
    fs::create_hard_link(temporary, output_path, error);
    if (error) {
        std::error_code cleanup_error;
        fs::remove(temporary, cleanup_error);
        std::cerr << "sorxctl: cannot publish migrated config: "
                  << output_path << " (" << error.message() << ")\n";
        return 2;
    }
    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    if (cleanup_error) {
        std::cerr << "sorxctl: migrated output published, but temporary link "
                  << temporary.string() << " could not be removed ("
                  << cleanup_error.message() << ")\n";
    }
    std::cout << "sorxctl: migrated " << input_path << " -> " << output_path
              << " route-critical-sha256="
              << route_critical_sha256(*decoded.config) << '\n';
    return 0;
}

}  // namespace

int run_appliance_app(
    const AppKind kind,
    const int argc,
    const char* const argv[]) {
    const auto name = app_name(kind);
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        print_help(kind);
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << name << ' ' << appliance_scaffold_version << '\n';
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--check-config") {
        return check_config(name, argv[2]);
    }
    if (kind == AppKind::ControlCli && argc == 4 &&
        std::string_view{argv[1]} == "--migrate-ini") {
        return migrate_ini(argv[2], argv[3]);
    }
    if (argc > 1) {
        std::cerr << name << ": unsupported arguments\n";
        print_help(kind);
        return 64;
    }
    std::cerr
        << name
        << ": safe scaffold only; runtime activation is not implemented and no sockets were opened\n";
    return 78;
}

}  // namespace sorx::appliance
