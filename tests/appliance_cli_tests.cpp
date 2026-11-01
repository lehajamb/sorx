#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/appliance/app_main.hpp"
#include "sorx/appliance/device_config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef SORX_SOURCE_DIR
#error "SORX_SOURCE_DIR must identify the source tree"
#endif

namespace {

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

void write_file(
    const std::filesystem::path& path,
    const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    assert(output);
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace sorx::appliance;

    const fs::path sample =
        fs::path{SORX_SOURCE_DIR} / "configs" / "appliance" /
        "sorx-device-v2.yaml";
    YamlDeviceConfigCodec yaml;
    const auto source = yaml.decode(read_file(sample));
    assert(source);

    LegacyIniMigrationCodec legacy;
    const auto temporary_root = fs::current_path();
    const auto input = temporary_root / "sorxctl-migration-test-input.ini";
    const auto output = temporary_root / "sorxctl-migration-test-output.yaml";
    const fs::path staging = output.string() + ".sorxctl.tmp";
    static_cast<void>(fs::remove(input));
    static_cast<void>(fs::remove(output));
    static_cast<void>(fs::remove(staging));
    write_file(input, legacy.encode(*source.config));

    const auto input_text = input.string();
    const auto output_text = output.string();
    const char* const migrate_arguments[]{
        "sorxctl",
        "--migrate-ini",
        input_text.c_str(),
        output_text.c_str(),
    };
    assert(run_appliance_app(
               AppKind::ControlCli,
               4,
               migrate_arguments) == 0);
    assert(fs::exists(output));
    assert(!fs::exists(staging));

    const auto migrated_document = read_file(output);
    const auto migrated = yaml.decode(migrated_document);
    assert(migrated);
    assert(*migrated.config == *source.config);
    assert(route_critical_sha256(*migrated.config) ==
           route_critical_sha256(*source.config));

    // A second invocation must fail without changing the published output.
    assert(run_appliance_app(
               AppKind::ControlCli,
               4,
               migrate_arguments) == 2);
    assert(read_file(output) == migrated_document);

    static_cast<void>(fs::remove(input));
    static_cast<void>(fs::remove(output));
    static_cast<void>(fs::remove(staging));
    return 0;
}
