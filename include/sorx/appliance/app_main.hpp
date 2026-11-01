#pragma once

#include <cstdint>
#include <string_view>

namespace sorx::appliance {

enum class AppKind : std::uint8_t {
    DataPlane,
    FixGateway,
    AuditDaemon,
    ManagementDaemon,
    ControlCli,
};

inline constexpr std::string_view appliance_scaffold_version{"0.2.0"};

// Safe common entry point for the initial appliance binaries. It provides
// offline config validation and identity/help output only; it never opens a
// socket or claims the forwarding plane is operational.
[[nodiscard]] int run_appliance_app(
    AppKind kind,
    int argc,
    const char* const argv[]);

}  // namespace sorx::appliance
