#pragma once

#include "sorx/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sorx {

struct VenueCapabilities final {
    bool aggressive_ioc{};
    bool passive_gtc{};

    constexpr auto operator<=>(const VenueCapabilities&) const noexcept = default;
};

struct RouteConfig final {
    VenueId venue{};
    InstrumentId instrument{};
    std::string venue_name;
    std::string canonical_symbol;
    std::string wire_symbol;
    RationalConversion price_conversion{};
    RationalConversion quantity_conversion{};
    VenueCapabilities capabilities{};
    Quantity allocation_cap{};
    std::uint32_t aggressive_priority{};
    std::uint32_t passive_priority{};
    std::uint64_t configured_latency_ns{};
    AdminState admin_state{AdminState::Up};

    auto operator<=>(const RouteConfig&) const = default;
};

enum class RouteValidationCode : std::uint8_t {
    EmptyRib,
    InvalidVenueId,
    InvalidInstrumentId,
    InvalidAdminState,
    EmptyVenueName,
    EmptyCanonicalSymbol,
    EmptyWireSymbol,
    NoRoutingCapability,
    ZeroAllocationCap,
    DuplicateRoute,
    VenueNameConflict,
    VenueAdminConflict,
    InstrumentSymbolConflict,
    CanonicalSymbolConflict,
    DuplicateWireSymbol,
};

struct RouteValidationError final {
    RouteValidationCode code{RouteValidationCode::EmptyRib};
    VenueId venue{};
    InstrumentId instrument{};
    std::string message;
};

class RouteRib final {
public:
    RouteRib() = default;

    [[nodiscard]] bool replace_all(
        std::vector<RouteConfig> routes,
        std::vector<RouteValidationError>& errors);

    [[nodiscard]] bool set_admin(VenueId venue, AdminState state) noexcept;
    [[nodiscard]] bool restore_revision(std::uint64_t revision) noexcept;
    [[nodiscard]] std::span<const RouteConfig> routes() const noexcept { return routes_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    std::vector<RouteConfig> routes_;
    std::uint64_t revision_{};
};

struct CandidateHandle final {
    VenueId venue{};
    std::string venue_name;
    std::string wire_symbol;
    RationalConversion price_conversion{};
    RationalConversion quantity_conversion{};
    VenueCapabilities capabilities{};
    Quantity allocation_cap{};
    std::uint32_t aggressive_priority{};
    std::uint32_t passive_priority{};
    std::uint64_t configured_latency_ns{};
    AdminState admin_state{AdminState::Up};

    auto operator<=>(const CandidateHandle&) const = default;
};

struct CandidateSet final {
    InstrumentId instrument{};
    std::string canonical_symbol;
    std::vector<CandidateHandle> candidates;
};

class CandidateTable final {
public:
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t source_rib_revision() const noexcept {
        return source_rib_revision_;
    }
    [[nodiscard]] std::span<const CandidateSet> entries() const noexcept { return entries_; }
    [[nodiscard]] const CandidateSet* find(InstrumentId instrument) const noexcept;

private:
    friend class FibCompiler;

    CandidateTable(
        std::uint64_t generation,
        std::uint64_t source_rib_revision,
        std::vector<CandidateSet> entries)
        : generation_(generation),
          source_rib_revision_(source_rib_revision),
          entries_(std::move(entries)) {}

    std::uint64_t generation_{};
    std::uint64_t source_rib_revision_{};
    std::vector<CandidateSet> entries_;
};

class FibCompiler final {
public:
    [[nodiscard]] std::shared_ptr<const CandidateTable> compile(const RouteRib& rib);
    [[nodiscard]] std::shared_ptr<const CandidateTable> compile_at_generation(
        const RouteRib& rib,
        std::uint64_t generation);

private:
    std::uint64_t next_generation_{1};
    std::shared_ptr<const CandidateTable> current_;
};

}  // namespace sorx




