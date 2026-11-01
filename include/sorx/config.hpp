#pragma once

#include "sorx/order_core.hpp"
#include "sorx/route_rib.hpp"
#include "sorx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sorx::config {

using Sha256Digest = std::array<std::uint8_t, 32>;

struct InstrumentConfiguration final {
    InstrumentId id{};
    std::string canonical_symbol;
    InstrumentRiskLimit risk;
};

struct VenueConfiguration final {
    VenueId id{};
    std::string name;
    AdminState admin_state{AdminState::Up};
    VenueRiskLimit risk;

    std::size_t market_data_ingress_capacity{};
    std::size_t order_entry_ingress_capacity{};
    std::size_t cancel_egress_capacity{};
    std::size_t normal_egress_capacity{};

    ChannelId market_data_channel_id{};
    ProfileId market_data_profile_id{};
    ProfileId order_entry_profile_id{};
    std::string market_data_profile;
    std::string market_data_session_identity;
    std::string order_entry_profile;
    std::string order_entry_session_identity;
    std::string dictionary_identity;
    std::string sender_comp_id;
    std::string target_comp_id;

    // A credential locator is operational input, not route-critical identity.
    // It is deliberately omitted from the canonical digest.
    std::optional<std::string> credential_reference;
};

struct ValidatedConfiguration final {
    std::uint32_t schema_version{};
    std::string account_id;
    std::size_t ingress_capacity{};
    CoreConfiguration core;
    std::vector<InstrumentConfiguration> instruments;
    std::vector<VenueConfiguration> venues;
    std::vector<RouteConfig> routes;
    Sha256Digest route_critical_hash{};
    std::string route_critical_hash_hex;
};

enum class ConfigurationErrorCode : std::uint8_t {
    Io,
    Syntax,
    UnknownSection,
    DuplicateSection,
    DuplicateField,
    UnknownField,
    MissingField,
    InvalidValue,
    DuplicateIdentity,
    UnmappedReference,
    InexactConversion,
    Overflow,
    RouteValidation,
};

struct ConfigurationError final {
    ConfigurationErrorCode code{ConfigurationErrorCode::Syntax};
    std::size_t line{};
    std::string section;
    std::string field;
    std::string message;
};

struct ConfigurationResult final {
    std::optional<ValidatedConfiguration> configuration;
    std::vector<ConfigurationError> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return configuration.has_value() && errors.empty();
    }
};

class ConfigurationLoader final {
public:
    // Strict schema:
    //   [sorx]
    //   [instrument <positive uint32 ID>]
    //   [venue <positive uint32 ID>]
    //   [route <instrument ID> <venue ID>]
    // Keys and enum spellings are case-sensitive. Unknown and duplicate input
    // is rejected; comments are whole lines beginning with '#' or ';'.
    [[nodiscard]] static ConfigurationResult parse_ini(std::string_view text);
    [[nodiscard]] static ConfigurationResult load_ini_file(
        const std::filesystem::path& path);
};

[[nodiscard]] Sha256Digest compute_route_critical_hash(
    const ValidatedConfiguration& configuration);
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);

}  // namespace sorx::config
