#include "sorx/route_rib.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sorx {
namespace {

[[nodiscard]] bool is_blank(const std::string& value) {
    return value.empty() || std::all_of(
        value.begin(),
        value.end(),
        [](const unsigned char character) { return std::isspace(character) != 0; });
}

}  // namespace

bool RouteRib::replace_all(
    std::vector<RouteConfig> routes,
    std::vector<RouteValidationError>& errors) {
    errors.clear();
    const auto add_error = [&errors](
                               const RouteValidationCode code,
                               const RouteConfig& route,
                               std::string message) {
        errors.push_back(RouteValidationError{
            code,
            route.venue,
            route.instrument,
            std::move(message),
        });
    };

    if (routes.empty()) {
        errors.push_back(RouteValidationError{
            RouteValidationCode::EmptyRib,
            VenueId{},
            InstrumentId{},
            "route inventory must contain at least one direct venue route",
        });
        return false;
    }

    std::map<VenueId, std::pair<std::string, AdminState>> venues;
    std::map<InstrumentId, std::string> instruments;
    std::map<std::string, InstrumentId> canonical_symbols;
    std::set<std::pair<InstrumentId, VenueId>> route_keys;
    std::set<std::pair<VenueId, std::string>> wire_symbols;

    for (const auto& route : routes) {
        if (!route.venue) {
            add_error(RouteValidationCode::InvalidVenueId, route, "venue ID zero is reserved");
        }
        if (!route.instrument) {
            add_error(
                RouteValidationCode::InvalidInstrumentId,
                route,
                "instrument ID zero is reserved");
        }
        if (route.admin_state != AdminState::Up &&
            route.admin_state != AdminState::Down) {
            add_error(
                RouteValidationCode::InvalidAdminState,
                route,
                "administrative state is invalid");
        }
        if (is_blank(route.venue_name)) {
            add_error(RouteValidationCode::EmptyVenueName, route, "venue name is required");
        }
        if (is_blank(route.canonical_symbol)) {
            add_error(
                RouteValidationCode::EmptyCanonicalSymbol,
                route,
                "canonical symbol is required");
        }
        if (is_blank(route.wire_symbol)) {
            add_error(RouteValidationCode::EmptyWireSymbol, route, "wire symbol is required");
        }
        if (!route.capabilities.aggressive_ioc && !route.capabilities.passive_gtc) {
            add_error(
                RouteValidationCode::NoRoutingCapability,
                route,
                "route must support at least one routing intent");
        }
        if (!route.allocation_cap) {
            add_error(
                RouteValidationCode::ZeroAllocationCap,
                route,
                "venue allocation cap must be positive");
        }

        if (!route_keys.emplace(route.instrument, route.venue).second) {
            add_error(
                RouteValidationCode::DuplicateRoute,
                route,
                "instrument and venue route is duplicated");
        }

        const auto [venue_it, venue_inserted] = venues.emplace(
            route.venue,
            std::pair{route.venue_name, route.admin_state});
        if (!venue_inserted) {
            if (venue_it->second.first != route.venue_name) {
                add_error(
                    RouteValidationCode::VenueNameConflict,
                    route,
                    "one venue ID maps to multiple venue names");
            }
            if (venue_it->second.second != route.admin_state) {
                add_error(
                    RouteValidationCode::VenueAdminConflict,
                    route,
                    "one venue ID has inconsistent administrative state");
            }
        }

        const auto [instrument_it, instrument_inserted] = instruments.emplace(
            route.instrument,
            route.canonical_symbol);
        if (!instrument_inserted && instrument_it->second != route.canonical_symbol) {
            add_error(
                RouteValidationCode::InstrumentSymbolConflict,
                route,
                "one instrument ID maps to multiple canonical symbols");
        }

        const auto [symbol_it, symbol_inserted] = canonical_symbols.emplace(
            route.canonical_symbol,
            route.instrument);
        if (!symbol_inserted && symbol_it->second != route.instrument) {
            add_error(
                RouteValidationCode::CanonicalSymbolConflict,
                route,
                "one canonical symbol maps to multiple instrument IDs");
        }

        if (!wire_symbols.emplace(route.venue, route.wire_symbol).second) {
            add_error(
                RouteValidationCode::DuplicateWireSymbol,
                route,
                "wire symbol is duplicated within a venue");
        }
    }

    if (!errors.empty()) {
        return false;
    }

    std::sort(
        routes.begin(),
        routes.end(),
        [](const RouteConfig& lhs, const RouteConfig& rhs) {
            return std::tie(lhs.instrument, lhs.venue, lhs.wire_symbol) <
                   std::tie(rhs.instrument, rhs.venue, rhs.wire_symbol);
        });

    if (routes == routes_) {
        return true;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        errors.push_back(RouteValidationError{
            RouteValidationCode::EmptyRib,
            VenueId{},
            InstrumentId{},
            "RIB revision counter exhausted",
        });
        return false;
    }

    routes_ = std::move(routes);
    ++revision_;
    return true;
}

bool RouteRib::set_admin(const VenueId venue, const AdminState state) noexcept {
    if (state != AdminState::Up && state != AdminState::Down) {
        return false;
    }
    const auto found = std::find_if(
        routes_.begin(),
        routes_.end(),
        [venue](const RouteConfig& route) { return route.venue == venue; });
    if (found == routes_.end()) {
        return false;
    }

    const bool changed = std::any_of(
        routes_.begin(),
        routes_.end(),
        [venue, state](const RouteConfig& route) {
            return route.venue == venue && route.admin_state != state;
        });
    if (!changed) {
        return true;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    for (auto& route : routes_) {
        if (route.venue == venue) {
            route.admin_state = state;
        }
    }
    ++revision_;
    return true;
}

bool RouteRib::restore_revision(const std::uint64_t revision) noexcept {
    if (routes_.empty() || revision == 0U) {
        return false;
    }
    revision_ = revision;
    return true;
}

const CandidateSet* CandidateTable::find(const InstrumentId instrument) const noexcept {
    const auto found = std::lower_bound(
        entries_.begin(),
        entries_.end(),
        instrument,
        [](const CandidateSet& entry, const InstrumentId value) {
            return entry.instrument < value;
        });
    if (found == entries_.end() || found->instrument != instrument) {
        return nullptr;
    }
    return &*found;
}

std::shared_ptr<const CandidateTable> FibCompiler::compile(const RouteRib& rib) {
    if (current_ && current_->source_rib_revision() == rib.revision()) {
        return current_;
    }
    if (next_generation_ == 0) {
        throw std::overflow_error("CandidateTable generation counter exhausted");
    }

    std::vector<CandidateSet> entries;
    for (const auto& route : rib.routes()) {
        if (entries.empty() || entries.back().instrument != route.instrument) {
            entries.push_back(CandidateSet{
                route.instrument,
                route.canonical_symbol,
                {},
            });
        }
        entries.back().candidates.push_back(CandidateHandle{
            route.venue,
            route.venue_name,
            route.wire_symbol,
            route.price_conversion,
            route.quantity_conversion,
            route.capabilities,
            route.allocation_cap,
            route.aggressive_priority,
            route.passive_priority,
            route.configured_latency_ns,
            route.admin_state,
        });
    }

    const auto generation = next_generation_++;
    current_ = std::shared_ptr<const CandidateTable>{
        new CandidateTable{generation, rib.revision(), std::move(entries)}};
    return current_;
}

std::shared_ptr<const CandidateTable> FibCompiler::compile_at_generation(
    const RouteRib& rib,
    const std::uint64_t generation) {
    if (generation == 0U ||
        generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument(
            "restored CandidateTable generation is invalid");
    }
    current_.reset();
    next_generation_ = generation;
    return compile(rib);
}

}  // namespace sorx




