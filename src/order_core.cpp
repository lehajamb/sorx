#include "sorx/order_core.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sorx {
namespace {

using Bytes = std::vector<std::byte>;

void put_u8(Bytes& output, const std::uint8_t value) {
    output.push_back(static_cast<std::byte>(value));
}

void put_u16(Bytes& output, const std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(value & 0xffU));
}

void put_u32(Bytes& output, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_u64(Bytes& output, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_i64(Bytes& output, const std::int64_t value) {
    put_u64(output, static_cast<std::uint64_t>(value));
}

void put_string(Bytes& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("deterministic record string is too large");
    }
    put_u32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(
        output.end(),
        reinterpret_cast<const std::byte*>(value.data()),
        reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

template <typename Enum>
void put_enum(Bytes& output, const Enum value) {
    put_u8(output, static_cast<std::uint8_t>(value));
}

void put_wire_profile(
    Bytes& output,
    const adapters::WireProfileIdentity& profile) {
    put_u32(output, profile.venue.value());
    put_u32(output, profile.profile.value());
    put_string(output, profile.profile_name);
    put_string(output, profile.session_identity);
    put_string(output, profile.dictionary_identity);
    put_string(output, profile.sender_comp_id);
    put_string(output, profile.target_comp_id);
}

class StateReader final {
public:
    explicit StateReader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    std::uint8_t u8() {
        require(1U);
        return std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }

    std::uint16_t u16() {
        require(2U);
        std::uint16_t value = 0U;
        for (std::size_t index = 0U; index < 2U; ++index) {
            value = static_cast<std::uint16_t>(
                (value << 8U) |
                std::to_integer<std::uint16_t>(bytes_[offset_ + index]));
        }
        offset_ += 2U;
        return value;
    }

    std::uint32_t u32() {
        require(4U);
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint32_t>(bytes_[offset_ + index]);
        }
        offset_ += 4U;
        return value;
    }

    std::uint64_t u64() {
        require(8U);
        std::uint64_t value = 0U;
        for (std::size_t index = 0U; index < 8U; ++index) {
            value = (value << 8U) |
                    std::to_integer<std::uint64_t>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return value;
    }

    std::int64_t i64() {
        return static_cast<std::int64_t>(u64());
    }

    std::string string() {
        const std::uint32_t size = u32();
        require(size);
        const char* begin =
            reinterpret_cast<const char*>(bytes_.data() + offset_);
        std::string output(begin, begin + size);
        offset_ += size;
        return output;
    }

    void require_end() const {
        if (offset_ != bytes_.size()) {
            throw std::runtime_error("trailing bytes in core state image");
        }
    }

private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - offset_) {
            throw std::runtime_error("truncated core state image");
        }
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

adapters::WireProfileIdentity read_wire_profile(StateReader& reader) {
    adapters::WireProfileIdentity profile;
    profile.venue = VenueId{reader.u32()};
    profile.profile = ProfileId{reader.u32()};
    profile.profile_name = reader.string();
    profile.session_identity = reader.string();
    profile.dictionary_identity = reader.string();
    profile.sender_comp_id = reader.string();
    profile.target_comp_id = reader.string();
    return profile;
}

void put_dispatch_attempt(
    Bytes& output,
    const DispatchAttemptSnapshot& attempt) {
    put_enum(output, attempt.kind);
    put_u64(output, attempt.attempt_id);
    put_u32(output, attempt.venue.value());
    put_u64(output, attempt.session_epoch);
    put_string(output, attempt.wire_order_id);
}

DispatchAttemptSnapshot read_dispatch_attempt(StateReader& reader) {
    DispatchAttemptSnapshot attempt;
    attempt.kind =
        static_cast<adapters::DispatchAttemptKind>(reader.u8());
    attempt.attempt_id = reader.u64();
    attempt.venue = VenueId{reader.u32()};
    attempt.session_epoch = reader.u64();
    attempt.wire_order_id = reader.string();
    return attempt;
}

Bytes encode_parent_order(const ParentOrder& order) {
    Bytes output;
    put_u64(output, order.id.value());
    put_u32(output, order.instrument.value());
    put_enum(output, order.side);
    put_u64(output, order.quantity.units);
    put_i64(output, order.limit_price.units);
    put_enum(output, order.intent);
    return output;
}

Bytes encode_parent_replace(const ParentReplace& replacement) {
    Bytes output;
    put_u64(output, replacement.id.value());
    put_u64(output, replacement.new_total_quantity.units);
    put_i64(output, replacement.new_limit_price.units);
    return output;
}

Bytes encode_route_decision(
    const ParentOrderId parent,
    const std::uint64_t revision,
    const RouteDecision& decision) {
    Bytes output;
    put_u64(output, parent.value());
    put_u64(output, revision);
    put_u64(output, decision.candidate_table_generation);
    put_u64(output, decision.risk_generation);
    put_u32(output, static_cast<std::uint32_t>(decision.candidate_pins.size()));
    for (const auto& pin : decision.candidate_pins) {
        put_u32(output, pin.venue.value());
        put_u64(output, pin.adjacency_generation);
        put_u8(output, pin.book_version.has_value() ? 1U : 0U);
        if (pin.book_version.has_value()) {
            put_u32(output, pin.book_version->venue.value());
            put_u32(output, pin.book_version->instrument.value());
            put_u64(output, pin.book_version->feed_session_epoch);
            put_u64(output, pin.book_version->recovery_generation);
            put_u64(output, pin.book_version->view_generation);
            put_u64(output, pin.book_version->last_applied_sequence);
            put_u64(output, pin.book_version->next_sequence);
        }
    }
    put_u64(output, decision.allocated.units);
    put_u64(output, decision.remainder.units);
    put_enum(output, decision.remainder_reason);
    put_enum(output, decision.reject_reason);
    put_u8(output, decision.observed_truncated_depth ? 1U : 0U);
    put_u32(output, static_cast<std::uint32_t>(decision.exclusions.size()));
    for (const auto& exclusion : decision.exclusions) {
        put_u32(output, exclusion.venue.value());
        put_enum(output, exclusion.reason);
    }
    put_u32(output, static_cast<std::uint32_t>(decision.allocations.size()));
    for (const auto& allocation : decision.allocations) {
        put_u32(output, allocation.venue.value());
        put_u64(output, allocation.quantity.units);
        put_i64(output, allocation.limit_price.units);
        put_u64(output, allocation.venue_quantity.units);
        put_i64(output, allocation.venue_limit_price.units);
        put_u32(output, allocation.allocation_order);
        put_u64(output, allocation.adjacency_generation);
        put_u64(output, allocation.book_version.feed_session_epoch);
        put_u64(output, allocation.book_version.recovery_generation);
        put_u64(output, allocation.book_version.view_generation);
        put_u64(output, allocation.book_version.last_applied_sequence);
        put_u64(output, allocation.book_version.next_sequence);
    }
    return output;
}

Bytes encode_child(
    const ChildOrderId child,
    const ParentOrderId parent,
    const std::uint64_t revision,
    const VenueId venue,
    const std::uint64_t session_epoch,
    const Quantity quantity,
    const Price price,
    const Quantity venue_quantity,
    const Price venue_price,
    const RationalConversion& price_conversion,
    const RationalConversion& quantity_conversion,
    const std::string& wire_order_id,
    const adapters::WireProfileIdentity& wire_profile,
    const std::string& cancel_wire_order_id,
    const ChildState state) {
    Bytes output;
    put_u64(output, child.value());
    put_u64(output, parent.value());
    put_u64(output, revision);
    put_u32(output, venue.value());
    put_u64(output, session_epoch);
    put_u64(output, quantity.units);
    put_i64(output, price.units);
    put_u64(output, venue_quantity.units);
    put_i64(output, venue_price.units);
    put_u64(output, price_conversion.numerator());
    put_u64(output, price_conversion.denominator());
    put_u64(output, quantity_conversion.numerator());
    put_u64(output, quantity_conversion.denominator());
    put_string(output, wire_order_id);
    put_wire_profile(output, wire_profile);
    put_string(output, cancel_wire_order_id);
    put_enum(output, state);
    return output;
}

Bytes encode_reservation(
    const ChildOrderId child,
    const VenueId venue,
    const InstrumentId instrument,
    const Quantity quantity) {
    Bytes output;
    put_u64(output, child.value());
    put_u32(output, venue.value());
    put_u32(output, instrument.value());
    put_u64(output, quantity.units);
    return output;
}

const std::string& execution_report_identity(
    const adapters::NormalizedExecutionReport& report) noexcept {
    return report.execution_id.empty() ?
               report.durable_report_key :
               report.execution_id;
}

void put_execution_report(
    Bytes& output,
    const adapters::NormalizedExecutionReport& report) {
    put_u32(output, report.venue.value());
    put_u64(output, report.session_epoch);
    put_string(output, report.wire_order_id);
    put_string(output, report.execution_id);
    put_string(output, report.durable_report_key);
    put_enum(output, report.kind);
    put_enum(output, report.status);
    put_u8(output, report.last_quantity.has_value() ? 1U : 0U);
    if (report.last_quantity.has_value()) {
        put_u64(output, report.last_quantity->units);
    }
    put_u8(output, report.last_price.has_value() ? 1U : 0U);
    if (report.last_price.has_value()) {
        put_i64(output, report.last_price->units);
    }
    put_u64(output, report.cumulative_quantity.units);
    put_u8(output, report.leaves_quantity.has_value() ? 1U : 0U);
    if (report.leaves_quantity.has_value()) {
        put_u64(output, report.leaves_quantity->units);
    }
    put_string(output, report.reject_reason);
    put_enum(output, report.value_domain);
}

adapters::NormalizedExecutionReport read_execution_report(
    StateReader& reader) {
    adapters::NormalizedExecutionReport report;
    report.venue = VenueId{reader.u32()};
    report.session_epoch = reader.u64();
    report.wire_order_id = reader.string();
    report.execution_id = reader.string();
    report.durable_report_key = reader.string();
    report.kind = static_cast<adapters::ExecutionKind>(reader.u8());
    report.status =
        static_cast<adapters::NormalizedOrderStatus>(reader.u8());

    const std::uint8_t has_last_quantity = reader.u8();
    if (has_last_quantity > 1U) {
        throw std::runtime_error(
            "invalid LastQty presence flag in core state");
    }
    if (has_last_quantity != 0U) {
        report.last_quantity = Quantity{reader.u64()};
    }

    const std::uint8_t has_last_price = reader.u8();
    if (has_last_price > 1U) {
        throw std::runtime_error(
            "invalid LastPx presence flag in core state");
    }
    if (has_last_price != 0U) {
        report.last_price = Price{reader.i64()};
    }

    report.cumulative_quantity = Quantity{reader.u64()};
    const std::uint8_t has_leaves_quantity = reader.u8();
    if (has_leaves_quantity > 1U) {
        throw std::runtime_error(
            "invalid LeavesQty presence flag in core state");
    }
    if (has_leaves_quantity != 0U) {
        report.leaves_quantity = Quantity{reader.u64()};
    }
    report.reject_reason = reader.string();
    report.value_domain =
        static_cast<adapters::ExecutionValueDomain>(reader.u8());
    return report;
}

Bytes encode_execution_report(
    const adapters::NormalizedExecutionReport& report) {
    Bytes output;
    put_execution_report(output, report);
    return output;
}

Bytes encode_execution_canonicalized(
    const ChildOrderId child,
    const RationalConversion& price_conversion,
    const RationalConversion& quantity_conversion,
    const adapters::NormalizedExecutionReport& raw,
    const adapters::NormalizedExecutionReport& canonical) {
    Bytes output;
    put_u64(output, child.value());
    put_u64(output, price_conversion.numerator());
    put_u64(output, price_conversion.denominator());
    put_u64(output, quantity_conversion.numerator());
    put_u64(output, quantity_conversion.denominator());
    put_execution_report(output, raw);
    put_execution_report(output, canonical);
    return output;
}

Bytes encode_dispatch_started(
    const ChildOrderId child,
    const DispatchAttemptSnapshot& attempt) {
    Bytes output;
    put_u64(output, child.value());
    put_dispatch_attempt(output, attempt);
    return output;
}

Bytes encode_dispatch_result(
    const adapters::LocalDispatchResult& dispatch) {
    Bytes output;
    put_u64(output, dispatch.child.value());
    put_enum(output, dispatch.kind);
    put_u64(output, dispatch.attempt_id);
    put_u32(output, dispatch.venue.value());
    put_u64(output, dispatch.session_epoch);
    put_string(output, dispatch.wire_order_id);
    put_enum(output, dispatch.state);
    put_string(output, dispatch.detail);
    return output;
}

DeterministicRecord make_record(
    const DeterministicRecordKind kind,
    const std::uint64_t sequence,
    Bytes payload) {
    return DeterministicRecord{kind, sequence, std::move(payload)};
}

bool parent_terminal(const ParentState state) noexcept {
    return state == ParentState::Filled || state == ParentState::Canceled ||
           state == ParentState::Rejected;
}

bool child_terminal(const ChildState state) noexcept {
    return state == ChildState::Filled || state == ChildState::Canceled ||
           state == ChildState::Rejected;
}

bool valid_side(const Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

bool valid_routing_intent(const RoutingIntent intent) noexcept {
    return intent == RoutingIntent::AggressiveIOC ||
           intent == RoutingIntent::PassiveGTC;
}

bool valid_order_entry_state(const OrderEntryState state) noexcept {
    return state == OrderEntryState::Disconnected ||
           state == OrderEntryState::Connecting ||
           state == OrderEntryState::Ready ||
           state == OrderEntryState::Degraded;
}

bool valid_dispatch_attempt_kind(
    const adapters::DispatchAttemptKind kind) noexcept {
    return kind == adapters::DispatchAttemptKind::New ||
           kind == adapters::DispatchAttemptKind::Cancel;
}

bool valid_local_dispatch_state(
    const adapters::LocalDispatchState state) noexcept {
    return state == adapters::LocalDispatchState::DefinitelyUnsent ||
           state == adapters::LocalDispatchState::AcceptedLocally ||
           state == adapters::LocalDispatchState::Unknown;
}

bool valid_execution_kind(
    const adapters::ExecutionKind kind) noexcept {
    return kind == adapters::ExecutionKind::Acknowledged ||
           kind == adapters::ExecutionKind::Trade ||
           kind == adapters::ExecutionKind::Canceled ||
           kind == adapters::ExecutionKind::Rejected ||
           kind == adapters::ExecutionKind::CancelRejected;
}

bool valid_normalized_order_status(
    const adapters::NormalizedOrderStatus status) noexcept {
    return status == adapters::NormalizedOrderStatus::Pending ||
           status == adapters::NormalizedOrderStatus::Working ||
           status == adapters::NormalizedOrderStatus::PartiallyFilled ||
           status == adapters::NormalizedOrderStatus::Filled ||
           status == adapters::NormalizedOrderStatus::Canceled ||
           status == adapters::NormalizedOrderStatus::Rejected;
}

bool valid_execution_value_domain(
    const adapters::ExecutionValueDomain domain) noexcept {
    return domain == adapters::ExecutionValueDomain::Canonical ||
           domain == adapters::ExecutionValueDomain::Venue;
}

bool valid_reconciliation_disposition(
    const ReconciliationDisposition disposition) noexcept {
    return disposition == ReconciliationDisposition::DefinitelyUnsent ||
           disposition == ReconciliationDisposition::Working ||
           disposition == ReconciliationDisposition::TerminalCanceled ||
           disposition == ReconciliationDisposition::TerminalFilled ||
           disposition == ReconciliationDisposition::TerminalRejected ||
           disposition == ReconciliationDisposition::Unresolved;
}

bool valid_parent_state(const ParentState state) noexcept {
    return state == ParentState::Routing ||
           state == ParentState::Active ||
           state == ParentState::CancelPending ||
           state == ParentState::ReplacePending ||
           state == ParentState::Suspended ||
           state == ParentState::Filled ||
           state == ParentState::Canceled ||
           state == ParentState::Rejected;
}

bool valid_child_state(const ChildState state) noexcept {
    return state == ChildState::Prepared ||
           state == ChildState::DispatchStarted ||
           state == ChildState::PendingAcknowledgement ||
           state == ChildState::Working ||
           state == ChildState::CancelPending ||
           state == ChildState::Filled ||
           state == ChildState::Canceled ||
           state == ChildState::Rejected ||
           state == ChildState::Ambiguous;
}

bool valid_wire_profile_identity(
    const adapters::WireProfileIdentity& profile) noexcept {
    return profile.venue && profile.profile &&
           !profile.profile_name.empty() &&
           !profile.session_identity.empty() &&
           !profile.dictionary_identity.empty() &&
           !profile.sender_comp_id.empty() &&
           !profile.target_comp_id.empty();
}

class DirectCoreTestWireIdentityFactory final
    : public adapters::WireIdentityFactory {
public:
    explicit DirectCoreTestWireIdentityFactory(
        const std::shared_ptr<const CandidateTable>& candidates) {
        std::set<VenueId> venues;
        if (candidates) {
            for (const auto& set : candidates->entries()) {
                for (const auto& candidate : set.candidates) {
                    venues.insert(candidate.venue);
                }
            }
        }
        profiles_.reserve(venues.size());
        for (const VenueId venue : venues) {
            profiles_.push_back(adapters::WireProfileIdentity{
                venue,
                ProfileId{venue.value()},
                "DIRECT_CORE_TEST_PROFILE",
                "DIRECT_CORE_TEST_SESSION_" +
                    std::to_string(venue.value()),
                "DIRECT_CORE_TEST_DICTIONARY",
                "DIRECT_CORE_TEST_SENDER",
                "DIRECT_CORE_TEST_TARGET_" +
                    std::to_string(venue.value()),
            });
        }
    }

    std::span<const adapters::WireProfileIdentity> profiles()
        const noexcept override {
        return profiles_;
    }

    adapters::WireIdentityResult make(
        const adapters::WireIdentityRequest& request) const override {
        adapters::WireIdentityResult result;
        result.profile = request.profile;
        const auto found = std::find(
            profiles_.begin(), profiles_.end(), request.profile);
        if (found == profiles_.end() || !request.child ||
            !request.parent || request.parent_revision == 0U ||
            request.session_epoch == 0U ||
            (request.kind != adapters::DispatchAttemptKind::New &&
             request.kind != adapters::DispatchAttemptKind::Cancel)) {
            return result;
        }
        result.wire_order_id =
            (request.kind == adapters::DispatchAttemptKind::New ?
                 "N" :
                 "C") +
            std::to_string(request.session_epoch) + "-" +
            std::to_string(request.parent_revision) + "-" +
            std::to_string(request.child.value());
        result.ok =
            result.wire_order_id.size() <=
            adapters::kMaximumWireOrderIdLength;
        return result;
    }

private:
    std::vector<adapters::WireProfileIdentity> profiles_;
};

}  // namespace

struct SorCore::Impl final {
    struct ParentRecord final {
        ParentOrder order;
        std::uint64_t revision{1U};
        Quantity requested_total{};
        Quantity cumulative_filled{};
        Quantity explicit_remainder{};
        ParentState state{ParentState::Routing};
        std::vector<ChildOrderId> children;
        std::optional<ParentReplace> pending_replace;
    };

    struct ChildRecord final {
        ChildOrderId id{};
        ParentOrderId parent{};
        InstrumentId instrument{};
        std::uint64_t parent_revision{1U};
        VenueId venue{};
        std::uint64_t session_epoch{1U};
        std::string wire_order_id;
        std::string wire_symbol;
        adapters::WireProfileIdentity wire_profile;
        Side side{Side::Buy};
        RoutingIntent intent{RoutingIntent::AggressiveIOC};
        Quantity ordered{};
        Quantity filled{};
        Price limit_price{};
        Quantity venue_ordered{};
        Price venue_limit_price{};
        RationalConversion price_conversion{};
        RationalConversion quantity_conversion{};
        ChildState state{ChildState::Prepared};
        Quantity risk_reservation{};
        std::optional<NormalizedOrderStatus> pending_terminal_status;
        Quantity pending_terminal_cumulative{};
        std::string last_cancel_wire_id;
        std::optional<DispatchAttemptSnapshot> active_dispatch_attempt;
        std::optional<DispatchAttemptSnapshot> last_dispatch_attempt;
        std::map<
            std::uint64_t,
            adapters::NormalizedExecutionReport> pending_trade_reports;
    };

    using RuntimeKey = std::pair<InstrumentId, VenueId>;
    using WireKey = std::tuple<VenueId, std::uint64_t, std::string>;
    using ReportKey = std::tuple<VenueId, std::uint64_t, std::string>;

    CoreConfiguration configuration;
    std::shared_ptr<const CandidateTable> candidates;
    std::shared_ptr<const adapters::WireIdentityFactory>
        wire_identity_factory;
    std::map<VenueId, adapters::WireProfileIdentity> wire_profiles;
    std::map<RuntimeKey, CandidateRuntimeView> runtime_views;
    std::map<VenueId, std::pair<std::uint64_t, OrderEntryState>> sessions;
    std::map<ParentOrderId, ParentRecord> parents;
    std::map<ChildOrderId, ChildRecord> children;
    std::map<WireKey, ChildOrderId> wire_to_child;
    std::map<ReportKey, bool> observed_reports;
    std::map<InstrumentId, Quantity> instrument_reservations;
    std::map<VenueId, Quantity> venue_reservations;
    std::uint64_t next_child_id{1U};
    std::uint64_t last_dispatch_attempt_id{};
    std::uint64_t last_ingress_sequence{};
    bool kill_switch{};
    bool routing_enabled{true};

    Impl(
        CoreConfiguration input_configuration,
        std::shared_ptr<const CandidateTable> input_candidates,
        std::shared_ptr<const adapters::WireIdentityFactory>
            input_wire_identity_factory)
        : configuration(std::move(input_configuration)),
          candidates(std::move(input_candidates)),
          wire_identity_factory(std::move(input_wire_identity_factory)) {
        if (!candidates || !wire_identity_factory ||
            configuration.maximum_aggressive_venues == 0U ||
            configuration.risk.generation == 0U ||
            configuration.risk.instruments.empty() ||
            configuration.risk.venues.empty()) {
            throw std::invalid_argument("incomplete SORX core configuration");
        }
        for (const auto& profile : wire_identity_factory->profiles()) {
            if (!valid_wire_profile_identity(profile) ||
                !wire_profiles.emplace(profile.venue, profile).second) {
                throw std::invalid_argument(
                    "invalid or duplicate core wire profile binding");
            }
        }
        for (const auto& set : candidates->entries()) {
            for (const auto& candidate : set.candidates) {
                if (!wire_profiles.contains(candidate.venue)) {
                    throw std::invalid_argument(
                        "candidate venue lacks wire profile binding");
                }
            }
        }
        for (const auto& limit : configuration.risk.instruments) {
            if (!limit.instrument || !limit.maximum_parent_quantity ||
                !limit.maximum_child_quantity ||
                limit.maximum_parent_notional.units == 0U ||
                limit.maximum_child_notional.units == 0U ||
                limit.minimum_price.units <= 0 ||
                limit.maximum_price < limit.minimum_price ||
                limit.maximum_open_parents == 0U ||
                limit.maximum_open_children == 0U ||
                !limit.maximum_gross_outstanding) {
                throw std::invalid_argument("invalid instrument risk limit");
            }
        }
        for (const auto& limit : configuration.risk.venues) {
            if (!limit.venue || !limit.maximum_outstanding) {
                throw std::invalid_argument("invalid venue risk limit");
            }
        }
    }

    const InstrumentRiskLimit* instrument_limit(
        const InstrumentId instrument) const noexcept {
        const auto found = std::find_if(
            configuration.risk.instruments.begin(),
            configuration.risk.instruments.end(),
            [instrument](const InstrumentRiskLimit& item) {
                return item.instrument == instrument;
            });
        return found == configuration.risk.instruments.end() ? nullptr : &*found;
    }

    const VenueRiskLimit* venue_limit(const VenueId venue) const noexcept {
        const auto found = std::find_if(
            configuration.risk.venues.begin(),
            configuration.risk.venues.end(),
            [venue](const VenueRiskLimit& item) { return item.venue == venue; });
        return found == configuration.risk.venues.end() ? nullptr : &*found;
    }

    std::uint64_t open_parents(const InstrumentId instrument) const noexcept {
        return static_cast<std::uint64_t>(std::count_if(
            parents.begin(),
            parents.end(),
            [instrument](const auto& item) {
                return item.second.order.instrument == instrument &&
                       !parent_terminal(item.second.state);
            }));
    }

    std::uint64_t open_children(const InstrumentId instrument) const noexcept {
        return static_cast<std::uint64_t>(std::count_if(
            children.begin(),
            children.end(),
            [instrument](const auto& item) {
                return item.second.instrument == instrument &&
                       !child_terminal(item.second.state);
            }));
    }

    CommandError validate_parent(
        const ParentOrder& order,
        const Quantity total_quantity,
        const bool replacement) const noexcept {
        if (!order.id || !order.instrument || !order.quantity ||
            !total_quantity || order.limit_price.units <= 0 ||
            !valid_side(order.side) ||
            !valid_routing_intent(order.intent)) {
            return CommandError::InvalidCommand;
        }
        if (!routing_enabled) {
            return CommandError::RoutingDisabled;
        }
        if (kill_switch) {
            return CommandError::KillSwitchActive;
        }
        const InstrumentRiskLimit* limit = instrument_limit(order.instrument);
        if (limit == nullptr || !limit->allowed) {
            return CommandError::InstrumentProhibited;
        }
        if (total_quantity > limit->maximum_parent_quantity) {
            return CommandError::ParentQuantityLimit;
        }
        const auto notional = checked_notional(order.limit_price, total_quantity);
        if (!notional.has_value() ||
            *notional > limit->maximum_parent_notional) {
            return CommandError::ParentNotionalLimit;
        }
        if (order.limit_price < limit->minimum_price ||
            order.limit_price > limit->maximum_price) {
            return CommandError::PriceCollar;
        }
        if (!replacement &&
            open_parents(order.instrument) >= limit->maximum_open_parents) {
            return CommandError::OpenParentLimit;
        }
        return CommandError::None;
    }

    std::vector<CandidateRuntimeView> candidate_runtime_views(
        const InstrumentId instrument) const {
        std::vector<CandidateRuntimeView> output;
        const CandidateSet* set = candidates->find(instrument);
        if (set == nullptr) {
            return output;
        }
        output.reserve(set->candidates.size());
        for (const auto& candidate : set->candidates) {
            const auto found = runtime_views.find({instrument, candidate.venue});
            CandidateRuntimeView view;
            if (found != runtime_views.end()) {
                view = found->second;
            } else {
                view.venue = candidate.venue;
            }
            if (const VenueRiskLimit* limit = venue_limit(candidate.venue);
                limit == nullptr || !limit->allowed) {
                view.risk_allowed = false;
            }
            output.push_back(std::move(view));
        }
        return output;
    }

    bool proposal_exceeds_risk(
        const ParentOrder& order,
        const std::vector<ChildAllocation>& allocations,
        VenueId& prohibited_venue) const noexcept {
        const InstrumentRiskLimit* instrument = instrument_limit(order.instrument);
        if (instrument == nullptr) {
            prohibited_venue = allocations.empty() ? VenueId{} :
                                                     allocations.front().venue;
            return true;
        }
        if (open_children(order.instrument) + allocations.size() >
            instrument->maximum_open_children) {
            prohibited_venue = allocations.empty() ? VenueId{} :
                                                     allocations.back().venue;
            return true;
        }

        Quantity instrument_total =
            instrument_reservations.contains(order.instrument) ?
                instrument_reservations.at(order.instrument) :
                Quantity{};
        std::map<VenueId, Quantity> proposed_by_venue;
        for (const auto& allocation : allocations) {
            if (allocation.quantity > instrument->maximum_child_quantity) {
                prohibited_venue = allocation.venue;
                return true;
            }
            const auto notional =
                checked_notional(allocation.limit_price, allocation.quantity);
            if (!notional.has_value() ||
                *notional > instrument->maximum_child_notional) {
                prohibited_venue = allocation.venue;
                return true;
            }
            const auto next_instrument =
                checked_add(instrument_total, allocation.quantity);
            if (!next_instrument.has_value() ||
                *next_instrument > instrument->maximum_gross_outstanding) {
                prohibited_venue = allocation.venue;
                return true;
            }
            instrument_total = *next_instrument;

            Quantity venue_total =
                venue_reservations.contains(allocation.venue) ?
                    venue_reservations.at(allocation.venue) :
                    Quantity{};
            if (proposed_by_venue.contains(allocation.venue)) {
                const auto combined =
                    checked_add(venue_total, proposed_by_venue.at(allocation.venue));
                if (!combined.has_value()) {
                    prohibited_venue = allocation.venue;
                    return true;
                }
                venue_total = *combined;
            }
            const auto next_venue =
                checked_add(venue_total, allocation.quantity);
            const VenueRiskLimit* venue = venue_limit(allocation.venue);
            if (!next_venue.has_value() || venue == nullptr || !venue->allowed ||
                *next_venue > venue->maximum_outstanding) {
                prohibited_venue = allocation.venue;
                return true;
            }
            proposed_by_venue[allocation.venue] =
                proposed_by_venue.contains(allocation.venue) ?
                    *checked_add(
                        proposed_by_venue.at(allocation.venue),
                        allocation.quantity) :
                    allocation.quantity;
        }
        return false;
    }

    RouteDecision plan_route(const ParentOrder& order) const {
        auto views = candidate_runtime_views(order.instrument);
        RouteDecision decision;
        for (std::size_t attempt = 0U; attempt <= views.size(); ++attempt) {
            decision =
                order.intent == RoutingIntent::AggressiveIOC ?
                    RoutingPolicy::aggressive_ioc(
                        order,
                        *candidates,
                        views,
                        configuration.maximum_aggressive_venues) :
                    RoutingPolicy::passive_gtc(order, *candidates, views);
            decision.risk_generation = configuration.risk.generation;
            if (!decision.accepted() || decision.allocations.empty()) {
                return decision;
            }
            VenueId prohibited;
            if (!proposal_exceeds_risk(order, decision.allocations, prohibited)) {
                return decision;
            }
            const auto found = std::find_if(
                views.begin(),
                views.end(),
                [prohibited](const CandidateRuntimeView& item) {
                    return item.venue == prohibited;
                });
            if (found == views.end() || !prohibited) {
                for (auto& view : views) {
                    view.risk_allowed = false;
                }
            } else {
                found->risk_allowed = false;
            }
        }
        return decision;
    }

    const CandidateHandle* candidate_handle(
        const InstrumentId instrument,
        const VenueId venue) const noexcept {
        const CandidateSet* set = candidates->find(instrument);
        if (set == nullptr) {
            return nullptr;
        }
        const auto found = std::find_if(
            set->candidates.begin(),
            set->candidates.end(),
            [venue](const CandidateHandle& candidate) {
                return candidate.venue == venue;
            });
        return found == set->candidates.end() ? nullptr : &*found;
    }

    std::uint64_t session_epoch(const VenueId venue) const noexcept {
        const auto found = sessions.find(venue);
        return found == sessions.end() || found->second.first == 0U ?
                   1U :
                   found->second.first;
    }

    void add_reservation(const ChildRecord& child) {
        const auto next_instrument = checked_add(
            instrument_reservations[child.instrument],
            child.risk_reservation);
        const auto next_venue = checked_add(
            venue_reservations[child.venue],
            child.risk_reservation);
        if (!next_instrument.has_value() || !next_venue.has_value()) {
            throw std::logic_error("validated risk reservation overflowed");
        }
        instrument_reservations[child.instrument] = *next_instrument;
        venue_reservations[child.venue] = *next_venue;
    }

    void release_reservation(
        ChildRecord& child,
        const std::uint64_t sequence,
        ApplyResult& result) {
        if (!child.risk_reservation) {
            return;
        }
        instrument_reservations[child.instrument] =
            *checked_subtract(
                instrument_reservations[child.instrument],
                child.risk_reservation);
        venue_reservations[child.venue] =
            *checked_subtract(
                venue_reservations[child.venue],
                child.risk_reservation);
        result.records.push_back(make_record(
            DeterministicRecordKind::RiskReservationReleased,
            sequence,
            encode_reservation(
                child.id,
                child.venue,
                child.instrument,
                child.risk_reservation)));
        child.risk_reservation = Quantity{};
    }

    const adapters::WireProfileIdentity* wire_profile(
        const VenueId venue) const noexcept {
        const auto found = wire_profiles.find(venue);
        return found == wire_profiles.end() ? nullptr : &found->second;
    }

    std::optional<std::string> generated_wire_identity(
        const ChildRecord& child,
        const adapters::DispatchAttemptKind kind) const noexcept {
        const auto* profile = wire_profile(child.venue);
        if (profile == nullptr) {
            return std::nullopt;
        }
        adapters::WireIdentityResult generated;
        try {
            generated = wire_identity_factory->make(
                adapters::WireIdentityRequest{
                    *profile,
                    kind,
                    child.id,
                    child.parent,
                    child.parent_revision,
                    child.session_epoch,
                });
        } catch (...) {
            return std::nullopt;
        }
        if (!generated.ok || generated.profile != *profile ||
            generated.wire_order_id.empty() ||
            generated.wire_order_id.size() >
                adapters::kMaximumWireOrderIdLength) {
            return std::nullopt;
        }
        return generated.wire_order_id;
    }

    bool wire_identity_in_use(const std::string& wire_order_id) const noexcept {
        return std::any_of(
            children.begin(),
            children.end(),
            [&wire_order_id](const auto& item) {
                return item.second.wire_order_id == wire_order_id ||
                       item.second.last_cancel_wire_id == wire_order_id;
            });
    }

    adapters::NewOrderIntent new_order_effect(const ChildRecord& child) const {
        const ParentRecord& parent = parents.at(child.parent);
        adapters::NewOrderIntent intent;
        intent.venue = child.venue;
        intent.parent = child.parent;
        intent.child = child.id;
        intent.instrument = child.instrument;
        intent.side = child.side;
        intent.quantity = child.ordered;
        intent.limit_price = child.limit_price;
        intent.canonical_quantity = child.ordered;
        intent.canonical_limit_price = child.limit_price;
        intent.venue_quantity = child.venue_ordered;
        intent.venue_limit_price = child.venue_limit_price;
        intent.time_in_force =
            child.intent == RoutingIntent::AggressiveIOC ?
                adapters::TimeInForce::ImmediateOrCancel :
                adapters::TimeInForce::GoodTillCancel;
        intent.parent_revision = child.parent_revision;
        intent.session_epoch = child.session_epoch;
        intent.wire_order_id = child.wire_order_id;
        intent.wire_symbol = child.wire_symbol;
        static_cast<void>(parent);
        return intent;
    }

    adapters::CancelOrderIntent cancel_effect(
        const ChildRecord& child) const {
        if (child.last_cancel_wire_id.empty()) {
            throw std::logic_error(
                "prepared child lacks durable cancel wire identity");
        }
        adapters::CancelOrderIntent intent;
        intent.venue = child.venue;
        intent.parent = child.parent;
        intent.child = child.id;
        intent.side = child.side;
        intent.original_quantity = child.ordered;
        intent.canonical_original_quantity = child.ordered;
        intent.venue_original_quantity = child.venue_ordered;
        intent.session_epoch = child.session_epoch;
        intent.original_wire_order_id = child.wire_order_id;
        intent.cancel_wire_order_id = child.last_cancel_wire_id;
        intent.wire_symbol = child.wire_symbol;
        return intent;
    }

    void append_parent_report(
        const ParentRecord& parent,
        const ParentReportKind kind,
        std::string reason,
        ApplyResult& result) const {
        result.reports.push_back(ParentReport{
            parent.order.id,
            kind,
            parent.state,
            parent.cumulative_filled,
            parent.explicit_remainder,
            std::move(reason),
        });
    }

    bool route_parent(
        ParentRecord& parent,
        const std::uint64_t sequence,
        ApplyResult& result) {
        const ParentState previous_state = parent.state;
        const Quantity previous_remainder = parent.explicit_remainder;
        const std::size_t previous_record_count = result.records.size();
        const std::size_t previous_effect_count = result.effects.size();
        const std::size_t previous_report_count = result.reports.size();
        const auto identity_failure = [&]() {
            parent.state = previous_state;
            parent.explicit_remainder = previous_remainder;
            result.records.resize(previous_record_count);
            result.effects.resize(previous_effect_count);
            result.reports.resize(previous_report_count);
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return false;
        };

        parent.state = ParentState::Routing;
        const RouteDecision decision = plan_route(parent.order);
        result.records.push_back(make_record(
            DeterministicRecordKind::RouteDecision,
            sequence,
            encode_route_decision(parent.order.id, parent.revision, decision)));

        if (!decision.accepted() || decision.allocations.empty()) {
            parent.explicit_remainder = decision.remainder;
            parent.state = ParentState::Rejected;
            append_parent_report(
                parent,
                ParentReportKind::Rejected,
                "route decision rejected",
                result);
            result.records.push_back(make_record(
                DeterministicRecordKind::ParentTerminal,
                sequence,
                encode_parent_order(parent.order)));
            return true;
        }

        std::vector<ChildRecord> prepared_children;
        prepared_children.reserve(decision.allocations.size());
        std::set<std::string> prepared_wire_identities;
        std::uint64_t prospective_child_id = next_child_id;
        for (const auto& allocation : decision.allocations) {
            if (prospective_child_id == 0U ||
                prospective_child_id ==
                    std::numeric_limits<std::uint64_t>::max()) {
                return identity_failure();
            }
            const CandidateHandle* handle =
                candidate_handle(parent.order.instrument, allocation.venue);
            const auto* profile = wire_profile(allocation.venue);
            if (handle == nullptr || profile == nullptr) {
                return identity_failure();
            }

            ChildRecord child;
            child.id = ChildOrderId{prospective_child_id++};
            child.parent = parent.order.id;
            child.instrument = parent.order.instrument;
            child.parent_revision = parent.revision;
            child.venue = allocation.venue;
            child.session_epoch = session_epoch(allocation.venue);
            child.wire_symbol = handle->wire_symbol;
            child.wire_profile = *profile;
            child.side = parent.order.side;
            child.intent = parent.order.intent;
            child.ordered = allocation.quantity;
            child.limit_price = allocation.limit_price;
            child.venue_ordered = allocation.venue_quantity;
            child.venue_limit_price = allocation.venue_limit_price;
            child.price_conversion = handle->price_conversion;
            child.quantity_conversion = handle->quantity_conversion;

            const auto new_identity = generated_wire_identity(
                child, adapters::DispatchAttemptKind::New);
            const auto cancel_identity = generated_wire_identity(
                child, adapters::DispatchAttemptKind::Cancel);
            if (!new_identity.has_value() ||
                !cancel_identity.has_value() ||
                *new_identity == *cancel_identity ||
                wire_identity_in_use(*new_identity) ||
                wire_identity_in_use(*cancel_identity) ||
                !prepared_wire_identities.insert(*new_identity).second ||
                !prepared_wire_identities.insert(*cancel_identity).second) {
                return identity_failure();
            }
            child.wire_order_id = *new_identity;
            child.last_cancel_wire_id = *cancel_identity;

            const auto converted_quantity =
                child.quantity_conversion.convert_exact(child.ordered);
            const auto converted_price =
                child.price_conversion.convert_exact(child.limit_price);
            if (!converted_quantity.has_value() ||
                !converted_price.has_value() ||
                *converted_quantity != child.venue_ordered ||
                *converted_price != child.venue_limit_price ||
                !child.venue_ordered ||
                child.venue_limit_price.units <= 0) {
                return identity_failure();
            }
            child.state = ChildState::Prepared;
            child.risk_reservation = allocation.quantity;
            prepared_children.push_back(std::move(child));
        }

        next_child_id = prospective_child_id;
        parent.explicit_remainder = decision.remainder;
        for (auto& child : prepared_children) {
            add_reservation(child);
            parent.children.push_back(child.id);
            const auto wire_inserted = wire_to_child.emplace(
                WireKey{
                    child.venue,
                    child.session_epoch,
                    child.wire_order_id,
                },
                child.id);
            const ChildOrderId child_id = child.id;
            const auto child_inserted =
                children.emplace(child.id, std::move(child));
            if (!wire_inserted.second || !child_inserted.second) {
                throw std::logic_error(
                    "preflighted child identity insertion failed");
            }
            ChildRecord& stored = child_inserted.first->second;

            result.records.push_back(make_record(
                DeterministicRecordKind::RiskReservationCreated,
                sequence,
                encode_reservation(
                    stored.id,
                    stored.venue,
                    stored.instrument,
                    stored.risk_reservation)));
            result.records.push_back(make_record(
                DeterministicRecordKind::ChildPrepared,
                sequence,
                encode_child(
                    stored.id,
                    stored.parent,
                    stored.parent_revision,
                    stored.venue,
                    stored.session_epoch,
                    stored.ordered,
                    stored.limit_price,
                    stored.venue_ordered,
                    stored.venue_limit_price,
                    stored.price_conversion,
                    stored.quantity_conversion,
                    stored.wire_order_id,
                    stored.wire_profile,
                    stored.last_cancel_wire_id,
                    stored.state)));
            result.effects.push_back(
                DispatchNewEffect{new_order_effect(stored)});
        }

        parent.state = ParentState::Active;
        append_parent_report(
            parent,
            ParentReportKind::RoutePlanned,
            decision.fully_allocated() ?
                "fully allocated" :
                "allocated with explicit remainder",
            result);
        return true;
    }

    bool all_children_terminal(const ParentRecord& parent) const noexcept {
        return std::all_of(
            parent.children.begin(),
            parent.children.end(),
            [this](const ChildOrderId id) {
                const auto found = children.find(id);
                return found != children.end() &&
                       child_terminal(found->second.state);
            });
    }

    void complete_replace(
        ParentRecord& parent,
        const std::uint64_t sequence,
        ApplyResult& result) {
        if (!parent.pending_replace.has_value()) {
            return;
        }
        const ParentReplace replacement = *parent.pending_replace;
        parent.pending_replace.reset();
        if (parent.cumulative_filled >= replacement.new_total_quantity) {
            parent.explicit_remainder = Quantity{};
            parent.state = ParentState::Filled;
            append_parent_report(
                parent,
                ParentReportKind::Filled,
                parent.cumulative_filled == replacement.new_total_quantity ?
                    "replace completed with no remaining quantity" :
                    "replace superseded by racing fills",
                result);
            result.records.push_back(make_record(
                DeterministicRecordKind::ParentTerminal,
                sequence,
                encode_parent_replace(replacement)));
            return;
        }

        ++parent.revision;
        parent.requested_total = replacement.new_total_quantity;
        parent.order.quantity =
            *checked_subtract(
                replacement.new_total_quantity,
                parent.cumulative_filled);
        parent.order.limit_price = replacement.new_limit_price;
        parent.explicit_remainder = Quantity{};
        append_parent_report(
            parent,
            ParentReportKind::Replaced,
            "old revision is terminal; routing replacement",
            result);
        route_parent(parent, sequence, result);
    }

    void maybe_complete_parent(
        ParentRecord& parent,
        const std::uint64_t sequence,
        ApplyResult& result) {
        if (!all_children_terminal(parent)) {
            return;
        }
        if (parent.pending_replace.has_value()) {
            complete_replace(parent, sequence, result);
            return;
        }
        if (parent.cumulative_filled >= parent.requested_total) {
            parent.state = ParentState::Filled;
            parent.explicit_remainder = Quantity{};
            append_parent_report(
                parent, ParentReportKind::Filled, "parent filled", result);
        } else if (parent.state == ParentState::CancelPending) {
            parent.state = ParentState::Canceled;
            append_parent_report(
                parent, ParentReportKind::Canceled, "parent canceled", result);
        } else if (parent.order.intent == RoutingIntent::AggressiveIOC) {
            parent.state = ParentState::Canceled;
            append_parent_report(
                parent,
                ParentReportKind::Canceled,
                "IOC children terminal with explicit remainder",
                result);
        } else {
            parent.state = ParentState::Canceled;
            append_parent_report(
                parent,
                ParentReportKind::Canceled,
                "passive child terminal",
                result);
        }
        result.records.push_back(make_record(
            DeterministicRecordKind::ParentTerminal,
            sequence,
            encode_parent_order(parent.order)));
    }

    void finalize_child(
        ChildRecord& child,
        const ChildState terminal_state,
        const std::uint64_t sequence,
        ApplyResult& result) {
        if (child_terminal(child.state)) {
            return;
        }
        if (terminal_state != ChildState::Filled) {
            const auto unfilled = checked_subtract(child.ordered, child.filled);
            ParentRecord& parent = parents.at(child.parent);
            const auto remainder =
                unfilled.has_value() ?
                    checked_add(parent.explicit_remainder, *unfilled) :
                    std::nullopt;
            if (!remainder.has_value() ||
                remainder->units >
                    parent.requested_total.units -
                        std::min(
                            parent.requested_total.units,
                            parent.cumulative_filled.units)) {
                throw std::logic_error(
                    "terminal child remainder violates parent conservation");
            }
            parent.explicit_remainder = *remainder;
        }
        child.active_dispatch_attempt.reset();
        child.state = terminal_state;
        child.pending_terminal_status.reset();
        child.pending_terminal_cumulative = Quantity{};
        release_reservation(child, sequence, result);
        result.records.push_back(make_record(
            DeterministicRecordKind::ChildTerminal,
            sequence,
            encode_child(
                child.id,
                child.parent,
                child.parent_revision,
                child.venue,
                child.session_epoch,
                child.ordered,
                child.limit_price,
                child.venue_ordered,
                child.venue_limit_price,
                child.price_conversion,
                child.quantity_conversion,
                child.wire_order_id,
                child.wire_profile,
                child.last_cancel_wire_id,
                child.state)));
        result.records.push_back(make_record(
            DeterministicRecordKind::ExecutionOutcome,
            sequence,
            encode_child(
                child.id,
                child.parent,
                child.parent_revision,
                child.venue,
                child.session_epoch,
                child.filled,
                child.limit_price,
                child.venue_ordered,
                child.venue_limit_price,
                child.price_conversion,
                child.quantity_conversion,
                child.wire_order_id,
                child.wire_profile,
                child.last_cancel_wire_id,
                child.state)));
        maybe_complete_parent(parents.at(child.parent), sequence, result);
    }

    void suspend_child(
        ChildRecord& child,
        const std::uint64_t sequence,
        std::string reason,
        ApplyResult& result) {
        if (child_terminal(child.state)) {
            return;
        }
        child.active_dispatch_attempt.reset();
        child.state = ChildState::Ambiguous;
        ParentRecord& parent = parents.at(child.parent);
        parent.state = ParentState::Suspended;
        append_parent_report(
            parent, ParentReportKind::Suspended, std::move(reason), result);
        result.records.push_back(make_record(
            DeterministicRecordKind::ExecutionOutcome,
            sequence,
            encode_child(
                child.id,
                child.parent,
                child.parent_revision,
                child.venue,
                child.session_epoch,
                child.filled,
                child.limit_price,
                child.venue_ordered,
                child.venue_limit_price,
                child.price_conversion,
                child.quantity_conversion,
                child.wire_order_id,
                child.wire_profile,
                child.last_cancel_wire_id,
                child.state)));
    }

    void request_cancels(
        ParentRecord& parent,
        const std::uint64_t sequence,
        ApplyResult& result) {
        const std::vector<ChildOrderId> existing_children = parent.children;
        for (const ChildOrderId id : existing_children) {
            ChildRecord& child = children.at(id);
            if (child_terminal(child.state) ||
                child.state == ChildState::CancelPending ||
                child.state == ChildState::Ambiguous) {
                continue;
            }

            // A queued new that has not begun dispatch is definitely local.
            // Terminate it now; a later stale egress item will fail its
            // DispatchStartedEvent identity/state validation and be dropped.
            if (child.state == ChildState::Prepared &&
                !child.active_dispatch_attempt.has_value()) {
                finalize_child(
                    child,
                    ChildState::Canceled,
                    sequence,
                    result);
                continue;
            }

            child.state = ChildState::CancelPending;
            if (!child.active_dispatch_attempt.has_value()) {
                result.effects.push_back(
                    DispatchCancelEffect{cancel_effect(child)});
            }
        }
    }

    ApplyResult apply_submit(
        const ParentSubmitEvent& event,
        const std::uint64_t sequence) {
        ApplyResult result;
        const ParentOrder& order = event.order;
        const auto existing = parents.find(order.id);
        if (existing != parents.end()) {
            if (existing->second.order == order) {
                result.idempotent_replay = true;
                return result;
            }
            result.command_accepted = false;
            result.command_error = CommandError::ConflictingParentId;
            return result;
        }
        result.command_error =
            validate_parent(order, order.quantity, false);
        if (result.command_error != CommandError::None) {
            result.command_accepted = false;
            return result;
        }

        result.records.push_back(make_record(
            DeterministicRecordKind::ParentSubmit,
            sequence,
            encode_parent_order(order)));
        ParentRecord parent;
        parent.order = order;
        parent.requested_total = order.quantity;
        parent.state = ParentState::Routing;
        const auto inserted = parents.emplace(order.id, std::move(parent));
        append_parent_report(
            inserted.first->second,
            ParentReportKind::Accepted,
            "parent command accepted",
            result);
        if (!route_parent(
                inserted.first->second, sequence, result)) {
            parents.erase(order.id);
            result.records.clear();
            result.effects.clear();
            result.reports.clear();
        }
        return result;
    }

    ApplyResult apply_cancel(
        const ParentCancelEvent& event,
        const std::uint64_t sequence) {
        ApplyResult result;
        const auto found = parents.find(event.parent);
        if (found == parents.end()) {
            result.command_accepted = false;
            result.command_error = CommandError::ParentNotFound;
            return result;
        }
        ParentRecord& parent = found->second;
        if (parent_terminal(parent.state)) {
            result.command_accepted = false;
            result.command_error = CommandError::ParentTerminal;
            return result;
        }
        result.records.push_back(make_record(
            DeterministicRecordKind::ParentCancel,
            sequence,
            [&] {
                Bytes payload;
                put_u64(payload, event.parent.value());
                return payload;
            }()));
        parent.state = ParentState::CancelPending;
        append_parent_report(
            parent,
            ParentReportKind::CancelPending,
            "cancel intent is durable",
            result);
        request_cancels(parent, sequence, result);
        if (!parent_terminal(parent.state) &&
            (all_children_terminal(parent) || parent.children.empty())) {
            parent.state = ParentState::Canceled;
            append_parent_report(
                parent,
                ParentReportKind::Canceled,
                "nothing active to cancel",
                result);
        }
        return result;
    }

    ApplyResult apply_replace(
        const ParentReplaceEvent& event,
        const std::uint64_t sequence) {
        ApplyResult result;
        const auto found = parents.find(event.replacement.id);
        if (found == parents.end()) {
            result.command_accepted = false;
            result.command_error = CommandError::ParentNotFound;
            return result;
        }
        ParentRecord& parent = found->second;
        if (parent_terminal(parent.state)) {
            result.command_accepted = false;
            result.command_error = CommandError::ParentTerminal;
            return result;
        }
        if (!event.replacement.new_total_quantity ||
            event.replacement.new_limit_price.units <= 0) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }
        if (event.replacement.new_total_quantity < parent.cumulative_filled) {
            result.command_accepted = false;
            result.command_error = CommandError::ReplacementBelowFilled;
            return result;
        }
        ParentOrder replacement_order = parent.order;
        replacement_order.quantity =
            event.replacement.new_total_quantity;
        replacement_order.limit_price =
            event.replacement.new_limit_price;
        result.command_error = validate_parent(
            replacement_order,
            event.replacement.new_total_quantity,
            true);
        if (result.command_error != CommandError::None) {
            result.command_accepted = false;
            return result;
        }

        result.records.push_back(make_record(
            DeterministicRecordKind::ParentReplace,
            sequence,
            encode_parent_replace(event.replacement)));
        parent.pending_replace = event.replacement;
        parent.state = ParentState::ReplacePending;
        append_parent_report(
            parent,
            ParentReportKind::ReplacePending,
            "replace intent is durable; canceling old revision",
            result);
        request_cancels(parent, sequence, result);
        if (all_children_terminal(parent) || parent.children.empty()) {
            complete_replace(parent, sequence, result);
        }
        return result;
    }

    bool dispatch_attempt_matches(
        const DispatchAttemptSnapshot& attempt,
        const adapters::LocalDispatchResult& result) const noexcept {
        return attempt.kind == result.kind &&
               attempt.attempt_id == result.attempt_id &&
               attempt.venue == result.venue &&
               attempt.session_epoch == result.session_epoch &&
               attempt.wire_order_id == result.wire_order_id;
    }

    ApplyResult apply_dispatch_started(
        const DispatchStartedEvent& event,
        const std::uint64_t sequence) {
        ApplyResult result;
        const auto found = children.find(event.child);
        if (found == children.end() ||
            !valid_dispatch_attempt_kind(event.kind) ||
            event.attempt_id == 0U ||
            event.venue == VenueId{} ||
            event.session_epoch == 0U ||
            event.wire_order_id.empty() ||
            last_dispatch_attempt_id ==
                std::numeric_limits<std::uint64_t>::max() ||
            event.attempt_id != last_dispatch_attempt_id + 1U) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }

        ChildRecord& child = found->second;
        const bool new_attempt =
            event.kind == adapters::DispatchAttemptKind::New;
        const std::string& expected_wire =
            new_attempt ? child.wire_order_id : child.last_cancel_wire_id;
        const bool valid_state =
            new_attempt ?
                child.state == ChildState::Prepared :
                child.state == ChildState::CancelPending;
        if (child_terminal(child.state) ||
            child.active_dispatch_attempt.has_value() ||
            !valid_state ||
            child.venue != event.venue ||
            child.session_epoch != event.session_epoch ||
            expected_wire.empty() ||
            expected_wire != event.wire_order_id) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }

        DispatchAttemptSnapshot attempt{
            event.kind,
            event.attempt_id,
            event.venue,
            event.session_epoch,
            event.wire_order_id,
        };
        last_dispatch_attempt_id = event.attempt_id;
        child.active_dispatch_attempt = attempt;
        child.last_dispatch_attempt = attempt;
        if (new_attempt) {
            child.state = ChildState::DispatchStarted;
        }
        result.records.push_back(make_record(
            DeterministicRecordKind::DispatchStarted,
            sequence,
            encode_dispatch_started(child.id, attempt)));
        return result;
    }

    ApplyResult apply_local_dispatch(
        const LocalDispatchEvent& event,
        const std::uint64_t sequence) {
        ApplyResult result;
        const auto found = children.find(event.result.child);
        if (found == children.end() ||
            !valid_dispatch_attempt_kind(event.result.kind) ||
            !valid_local_dispatch_state(event.result.state) ||
            event.result.attempt_id == 0U ||
            !event.result.venue ||
            event.result.session_epoch == 0U ||
            event.result.wire_order_id.empty()) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }

        ChildRecord& child = found->second;
        if (child_terminal(child.state) ||
            !child.active_dispatch_attempt.has_value() ||
            !dispatch_attempt_matches(
                *child.active_dispatch_attempt,
                event.result)) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }

        const adapters::DispatchAttemptKind attempt_kind =
            child.active_dispatch_attempt->kind;
        const bool new_attempt =
            attempt_kind == adapters::DispatchAttemptKind::New;
        const bool cancel_requested_during_new =
            new_attempt && child.state == ChildState::CancelPending;
        const bool valid_state =
            new_attempt ?
                (child.state == ChildState::DispatchStarted ||
                 child.state == ChildState::CancelPending) :
                child.state == ChildState::CancelPending;
        if (!valid_state) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }

        result.records.push_back(make_record(
            DeterministicRecordKind::DispatchResult,
            sequence,
            encode_dispatch_result(event.result)));
        child.active_dispatch_attempt.reset();

        if (event.result.state ==
            adapters::LocalDispatchState::Unknown) {
            suspend_child(
                child,
                sequence,
                event.result.detail.empty() ?
                    "dispatch outcome is ambiguous" :
                    event.result.detail,
                result);
            return result;
        }

        if (new_attempt) {
            if (event.result.state ==
                adapters::LocalDispatchState::DefinitelyUnsent) {
                if (cancel_requested_during_new) {
                    finalize_child(
                        child,
                        ChildState::Canceled,
                        sequence,
                        result);
                } else {
                    child.state = ChildState::Prepared;
                    result.effects.push_back(
                        DispatchNewEffect{new_order_effect(child)});
                }
            } else if (cancel_requested_during_new) {
                child.state = ChildState::CancelPending;
                result.effects.push_back(
                    DispatchCancelEffect{cancel_effect(child)});
            } else {
                child.state = ChildState::PendingAcknowledgement;
            }
            return result;
        }

        child.state = ChildState::CancelPending;
        if (event.result.state ==
            adapters::LocalDispatchState::DefinitelyUnsent) {
            result.effects.push_back(
                DispatchCancelEffect{cancel_effect(child)});
        }
        return result;
    }

    std::optional<adapters::NormalizedExecutionReport>
    canonicalize_execution_report(
        const ChildRecord& child,
        const adapters::NormalizedExecutionReport& raw,
        std::string& error) const noexcept {
        adapters::NormalizedExecutionReport canonical = raw;
        if (raw.value_domain == adapters::ExecutionValueDomain::Venue) {
            if (raw.cumulative_quantity > child.venue_ordered) {
                error = "venue cumulative quantity exceeds durable allocation";
                return std::nullopt;
            }
            if (raw.kind == adapters::ExecutionKind::Trade &&
                raw.leaves_quantity.has_value()) {
                const auto raw_total = checked_add(
                    raw.cumulative_quantity, *raw.leaves_quantity);
                if (!raw_total.has_value() ||
                    *raw_total != child.venue_ordered) {
                    error =
                        "venue execution report violates quantity conservation";
                    return std::nullopt;
                }
            }

            const auto inverse_quantity = RationalConversion::create(
                child.quantity_conversion.denominator(),
                child.quantity_conversion.numerator());
            const auto inverse_price = RationalConversion::create(
                child.price_conversion.denominator(),
                child.price_conversion.numerator());
            if (!inverse_quantity.has_value() ||
                !inverse_price.has_value()) {
                error = "invalid durable venue conversion identity";
                return std::nullopt;
            }

            const auto cumulative =
                inverse_quantity->convert_exact(raw.cumulative_quantity);
            if (!cumulative.has_value()) {
                error = "inexact venue CumQty conversion";
                return std::nullopt;
            }
            canonical.cumulative_quantity = *cumulative;

            if (raw.leaves_quantity.has_value()) {
                const auto leaves =
                    inverse_quantity->convert_exact(*raw.leaves_quantity);
                if (!leaves.has_value()) {
                    error = "inexact venue LeavesQty conversion";
                    return std::nullopt;
                }
                canonical.leaves_quantity = *leaves;
            }
            if (raw.last_quantity.has_value()) {
                const auto last_quantity =
                    inverse_quantity->convert_exact(*raw.last_quantity);
                if (!last_quantity.has_value()) {
                    error = "inexact venue LastQty conversion";
                    return std::nullopt;
                }
                canonical.last_quantity = *last_quantity;
            }
            if (raw.last_price.has_value()) {
                const auto last_price =
                    inverse_price->convert_exact(*raw.last_price);
                if (!last_price.has_value()) {
                    error = "inexact venue LastPx conversion";
                    return std::nullopt;
                }
                canonical.last_price = *last_price;
            }
            canonical.value_domain =
                adapters::ExecutionValueDomain::Canonical;
        }

        if (canonical.cumulative_quantity > child.ordered) {
            error = "canonical cumulative quantity exceeds child quantity";
            return std::nullopt;
        }
        if (canonical.kind == adapters::ExecutionKind::Trade &&
            canonical.leaves_quantity.has_value()) {
            const auto canonical_total = checked_add(
                canonical.cumulative_quantity,
                *canonical.leaves_quantity);
            if (!canonical_total.has_value() ||
                *canonical_total != child.ordered) {
                error =
                    "canonical execution report violates quantity conservation";
                return std::nullopt;
            }
        }
        return canonical;
    }

    bool valid_trade_report_for_child(
        const ChildRecord& child,
        const adapters::NormalizedExecutionReport& report) const noexcept {
        if (report.kind != adapters::ExecutionKind::Trade ||
            report.value_domain !=
                adapters::ExecutionValueDomain::Canonical ||
            !valid_normalized_order_status(report.status) ||
            report.venue != child.venue ||
            report.session_epoch != child.session_epoch ||
            report.wire_order_id != child.wire_order_id ||
            execution_report_identity(report).empty() ||
            !report.last_quantity.has_value() ||
            !*report.last_quantity ||
            !report.last_price.has_value() ||
            report.last_price->units <= 0 ||
            report.cumulative_quantity > child.ordered) {
            return false;
        }

        const bool price_violation =
            child.side == Side::Buy ?
                *report.last_price > child.limit_price :
                *report.last_price < child.limit_price;
        const auto prior_cumulative = checked_subtract(
            report.cumulative_quantity, *report.last_quantity);
        const auto conserved_quantity =
            report.leaves_quantity.has_value() ?
                checked_add(
                    report.cumulative_quantity,
                    *report.leaves_quantity) :
                std::optional<Quantity>{child.ordered};
        return !price_violation &&
               prior_cumulative.has_value() &&
               conserved_quantity.has_value() &&
               *conserved_quantity == child.ordered;
    }

    static bool equivalent_pending_trade(
        const adapters::NormalizedExecutionReport& left,
        const adapters::NormalizedExecutionReport& right) noexcept {
        return left.venue == right.venue &&
               left.session_epoch == right.session_epoch &&
               left.wire_order_id == right.wire_order_id &&
               left.kind == right.kind &&
               left.status == right.status &&
               left.last_quantity == right.last_quantity &&
               left.last_price == right.last_price &&
               left.cumulative_quantity == right.cumulative_quantity &&
               left.leaves_quantity == right.leaves_quantity &&
               left.reject_reason == right.reject_reason &&
               left.value_domain == right.value_domain;
    }

    bool pending_trade_chain_valid(
        const ChildRecord& child) const noexcept {
        Quantity lower_bound = child.filled;
        for (const auto& [cumulative_units, report] :
             child.pending_trade_reports) {
            if (!valid_trade_report_for_child(child, report) ||
                report.cumulative_quantity !=
                    Quantity{cumulative_units}) {
                return false;
            }
            const auto prior_cumulative = checked_subtract(
                report.cumulative_quantity,
                *report.last_quantity);
            if (!prior_cumulative.has_value() ||
                *prior_cumulative < lower_bound) {
                return false;
            }
            lower_bound = report.cumulative_quantity;
        }
        return true;
    }

    void apply_execution_status(
        ChildRecord& child,
        const adapters::NormalizedExecutionReport& report,
        const std::uint64_t sequence,
        ApplyResult& result) {
        if (report.cumulative_quantity > child.ordered) {
            suspend_child(
                child,
                sequence,
                "contradictory cumulative execution quantity",
                result);
            return;
        }

        const bool terminal_status =
            report.status == adapters::NormalizedOrderStatus::Filled ||
            report.status == adapters::NormalizedOrderStatus::Canceled ||
            report.status == adapters::NormalizedOrderStatus::Rejected;
        if (terminal_status &&
            (report.cumulative_quantity > child.filled ||
             !child.pending_trade_reports.empty())) {
            child.pending_terminal_status = report.status;
            child.pending_terminal_cumulative =
                report.cumulative_quantity;
            suspend_child(
                child,
                sequence,
                "terminal report awaits reordered fills",
                result);
            return;
        }

        const bool preserve_cancel_pending =
            child.state == ChildState::CancelPending;
        if (report.status == adapters::NormalizedOrderStatus::Filled) {
            if (child.filled != child.ordered) {
                suspend_child(
                    child,
                    sequence,
                    "filled status does not conserve child quantity",
                    result);
            } else {
                finalize_child(child, ChildState::Filled, sequence, result);
            }
        } else if (
            report.status == adapters::NormalizedOrderStatus::Canceled) {
            finalize_child(child, ChildState::Canceled, sequence, result);
        } else if (
            report.status == adapters::NormalizedOrderStatus::Rejected) {
            finalize_child(child, ChildState::Rejected, sequence, result);
        } else if (
            report.status == adapters::NormalizedOrderStatus::Working ||
            report.status ==
                adapters::NormalizedOrderStatus::PartiallyFilled) {
            child.state =
                preserve_cancel_pending ?
                    ChildState::CancelPending :
                    ChildState::Working;
        } else if (
            report.status == adapters::NormalizedOrderStatus::Pending &&
            child.state == ChildState::DispatchStarted) {
            child.state = ChildState::PendingAcknowledgement;
        }

        if (child.pending_terminal_status.has_value() &&
            child.pending_trade_reports.empty() &&
            child.filled == child.pending_terminal_cumulative) {
            const auto pending = *child.pending_terminal_status;
            if (pending == adapters::NormalizedOrderStatus::Filled &&
                child.filled == child.ordered) {
                finalize_child(child, ChildState::Filled, sequence, result);
            } else if (
                pending == adapters::NormalizedOrderStatus::Canceled) {
                finalize_child(child, ChildState::Canceled, sequence, result);
            } else if (
                pending == adapters::NormalizedOrderStatus::Rejected) {
                finalize_child(child, ChildState::Rejected, sequence, result);
            }
        }
    }

    void process_trade_report(
        ChildRecord& child,
        ParentRecord& parent,
        const adapters::NormalizedExecutionReport& report,
        const std::uint64_t sequence,
        ApplyResult& result) {
        if (!valid_trade_report_for_child(child, report)) {
            const bool price_violation =
                report.last_price.has_value() &&
                report.last_price->units > 0 &&
                (child.side == Side::Buy ?
                     *report.last_price > child.limit_price :
                     *report.last_price < child.limit_price);
            suspend_child(
                child,
                sequence,
                price_violation ?
                    "execution price violates child protection" :
                    "execution report violates quantity conservation",
                result);
            return;
        }

        const auto prior_cumulative = checked_subtract(
            report.cumulative_quantity, *report.last_quantity);
        if (!prior_cumulative.has_value()) {
            suspend_child(
                child,
                sequence,
                "execution report violates quantity conservation",
                result);
            return;
        }
        if (report.cumulative_quantity <= child.filled) {
            return;
        }
        if (*prior_cumulative < child.filled) {
            suspend_child(
                child,
                sequence,
                "execution report overlaps accounted quantity",
                result);
            return;
        }

        const std::uint64_t cumulative_units =
            report.cumulative_quantity.units;
        const auto existing =
            child.pending_trade_reports.find(cumulative_units);
        bool inserted = false;
        if (existing == child.pending_trade_reports.end()) {
            inserted = child.pending_trade_reports.emplace(
                cumulative_units, report).second;
        } else if (!equivalent_pending_trade(existing->second, report)) {
            suspend_child(
                child,
                sequence,
                "conflicting executions share CumQty",
                result);
            return;
        }

        if (!pending_trade_chain_valid(child)) {
            if (inserted) {
                child.pending_trade_reports.erase(cumulative_units);
            }
            suspend_child(
                child,
                sequence,
                "reordered executions overlap",
                result);
            return;
        }

        while (!child.pending_trade_reports.empty() &&
               !child_terminal(child.state)) {
            auto next = child.pending_trade_reports.begin();
            const adapters::NormalizedExecutionReport pending =
                next->second;
            const auto pending_prior = checked_subtract(
                pending.cumulative_quantity, *pending.last_quantity);
            if (!pending_prior.has_value()) {
                child.pending_trade_reports.erase(next);
                suspend_child(
                    child,
                    sequence,
                    "buffered execution is invalid",
                    result);
                return;
            }
            if (*pending_prior > child.filled) {
                break;
            }
            if (*pending_prior < child.filled) {
                child.pending_trade_reports.erase(next);
                suspend_child(
                    child,
                    sequence,
                    "buffered execution overlaps accounted quantity",
                    result);
                return;
            }

            const auto next_child =
                checked_add(child.filled, *pending.last_quantity);
            const auto next_parent = checked_add(
                parent.cumulative_filled, *pending.last_quantity);
            if (!next_child.has_value() ||
                *next_child != pending.cumulative_quantity ||
                *next_child > child.ordered ||
                !next_parent.has_value() ||
                *next_parent > parent.requested_total) {
                child.pending_trade_reports.erase(next);
                suspend_child(
                    child,
                    sequence,
                    "execution report violates quantity conservation",
                    result);
                return;
            }

            child.pending_trade_reports.erase(next);
            child.filled = *next_child;
            parent.cumulative_filled = *next_parent;
            append_parent_report(
                parent,
                child.filled == child.ordered ?
                    ParentReportKind::Filled :
                    ParentReportKind::PartiallyFilled,
                "unique execution applied",
                result);
            apply_execution_status(
                child, pending, sequence, result);
        }

        if (!child.pending_trade_reports.empty() &&
            !child_terminal(child.state) &&
            child.state != ChildState::Ambiguous) {
            suspend_child(
                child,
                sequence,
                "execution report awaits reordered predecessor",
                result);
        }
    }

    ApplyResult apply_execution(
        const ExecutionReportEvent& event,
        const std::uint64_t sequence) {
        ApplyResult result;
        if (!event.report.venue ||
            event.report.session_epoch == 0U ||
            event.report.wire_order_id.empty() ||
            !valid_execution_kind(event.report.kind) ||
            !valid_normalized_order_status(event.report.status) ||
            !valid_execution_value_domain(event.report.value_domain)) {
            result.command_accepted = false;
            result.command_error = CommandError::InvalidCommand;
            return result;
        }

        result.records.push_back(make_record(
            DeterministicRecordKind::ExecutionReportObserved,
            sequence,
            encode_execution_report(event.report)));

        const std::string& report_identity =
            execution_report_identity(event.report);
        if (report_identity.empty()) {
            return result;
        }
        const ReportKey report_key{
            event.report.venue,
            event.report.session_epoch,
            report_identity,
        };
        if (!observed_reports.emplace(report_key, true).second) {
            return result;
        }

        const auto wire = wire_to_child.find(WireKey{
            event.report.venue,
            event.report.session_epoch,
            event.report.wire_order_id,
        });
        if (wire == wire_to_child.end()) {
            return result;
        }
        ChildRecord& child = children.at(wire->second);
        ParentRecord& parent = parents.at(child.parent);
        if (child_terminal(child.state)) {
            return result;
        }

        std::string conversion_error;
        const auto canonical_report = canonicalize_execution_report(
            child, event.report, conversion_error);
        if (!canonical_report.has_value()) {
            suspend_child(
                child, sequence, std::move(conversion_error), result);
            return result;
        }
        result.records.push_back(make_record(
            DeterministicRecordKind::ExecutionCanonicalized,
            sequence,
            encode_execution_canonicalized(
                child.id,
                child.price_conversion,
                child.quantity_conversion,
                event.report,
                *canonical_report)));
        const adapters::NormalizedExecutionReport& report =
            *canonical_report;

        const bool cancel_waiting_on_new =
            child.state == ChildState::CancelPending &&
            child.active_dispatch_attempt.has_value() &&
            child.active_dispatch_attempt->kind ==
                adapters::DispatchAttemptKind::New;
        child.active_dispatch_attempt.reset();

        if (report.kind == adapters::ExecutionKind::CancelRejected) {
            if (child.state == ChildState::CancelPending) {
                child.state = ChildState::Working;
            }
            append_parent_report(
                parent,
                ParentReportKind::Rejected,
                report.reject_reason.empty() ?
                    "cancel rejected" :
                    report.reject_reason,
                result);
            return result;
        }

        if (report.kind == adapters::ExecutionKind::Trade) {
            process_trade_report(
                child, parent, report, sequence, result);
        } else {
            apply_execution_status(
                child, report, sequence, result);
        }

        if (cancel_waiting_on_new &&
            !child_terminal(child.state) &&
            child.state != ChildState::Ambiguous) {
            child.state = ChildState::CancelPending;
            result.effects.push_back(
                DispatchCancelEffect{cancel_effect(child)});
        }
        return result;
    }

    ApplyResult apply(const CoreEvent& event) {
        if (event.ingress_sequence == 0U ||
            event.ingress_sequence != last_ingress_sequence + 1U) {
            throw std::logic_error(
                "core ingress sequence must be positive and contiguous");
        }
        last_ingress_sequence = event.ingress_sequence;

        return std::visit(
            [this, sequence = event.ingress_sequence](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, ParentSubmitEvent>) {
                    return apply_submit(payload, sequence);
                } else if constexpr (std::is_same_v<T, ParentCancelEvent>) {
                    return apply_cancel(payload, sequence);
                } else if constexpr (std::is_same_v<T, ParentReplaceEvent>) {
                    return apply_replace(payload, sequence);
                } else if constexpr (
                    std::is_same_v<T, CandidateTablePublishedEvent>) {
                    ApplyResult result;
                    if (!payload.table) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }
                    candidates = payload.table;
                    Bytes bytes;
                    put_u64(bytes, candidates->generation());
                    put_u64(bytes, candidates->source_rib_revision());
                    result.records.push_back(make_record(
                        DeterministicRecordKind::CandidateTablePublished,
                        sequence,
                        std::move(bytes)));
                    return result;
                } else if constexpr (
                    std::is_same_v<T, RuntimeViewPublishedEvent>) {
                    ApplyResult result;
                    const RuntimeKey key{
                        payload.instrument,
                        payload.view.venue,
                    };
                    if (!payload.instrument ||
                        !payload.view.venue ||
                        payload.view.adjacency_generation == 0U ||
                        (payload.view.book.has_value() &&
                         (!payload.view.book->valid() ||
                          payload.view.book->venue != payload.view.venue ||
                          payload.view.book->instrument !=
                              payload.instrument))) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }

                    const auto existing = runtime_views.find(key);
                    if (existing != runtime_views.end() &&
                        (payload.view.adjacency_generation <
                             existing->second.adjacency_generation ||
                         (payload.view.book.has_value() &&
                          existing->second.book.has_value() &&
                          payload.view.book->version() <
                              existing->second.book->version()))) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }

                    CandidateRuntimeView merged = payload.view;
                    const auto session = sessions.find(payload.view.venue);
                    merged.order_entry_state =
                        session == sessions.end() ?
                            OrderEntryState::Disconnected :
                            session->second.second;
                    runtime_views[key] = merged;

                    Bytes bytes;
                    put_u32(bytes, payload.instrument.value());
                    put_u32(bytes, merged.venue.value());
                    put_u64(bytes, merged.adjacency_generation);
                    put_enum(bytes, merged.market_data_state);
                    put_enum(bytes, merged.order_entry_state);
                    put_enum(bytes, merged.egress_state);
                    put_enum(bytes, merged.instrument_state);
                    result.records.push_back(make_record(
                        DeterministicRecordKind::RuntimeViewPublished,
                        sequence,
                        std::move(bytes)));
                    return result;
                } else if constexpr (
                    std::is_same_v<T, OrderEntrySessionEvent>) {
                    ApplyResult result;
                    const auto existing = sessions.find(payload.venue);
                    if (!payload.venue ||
                        payload.session_epoch == 0U ||
                        !valid_order_entry_state(payload.state) ||
                        (existing != sessions.end() &&
                         payload.session_epoch < existing->second.first)) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }
                    for (const auto& [key, view] : runtime_views) {
                        if (key.second == payload.venue &&
                            view.adjacency_generation ==
                                std::numeric_limits<std::uint64_t>::max()) {
                            result.command_accepted = false;
                            result.command_error = CommandError::InvalidCommand;
                            return result;
                        }
                    }

                    sessions[payload.venue] =
                        {payload.session_epoch, payload.state};
                    for (auto& [key, view] : runtime_views) {
                        if (key.second == payload.venue) {
                            view.order_entry_state = payload.state;
                            ++view.adjacency_generation;
                        }
                    }
                    Bytes bytes;
                    put_u32(bytes, payload.venue.value());
                    put_u64(bytes, payload.session_epoch);
                    put_enum(bytes, payload.state);
                    result.records.push_back(make_record(
                        DeterministicRecordKind::OrderEntrySessionChanged,
                        sequence,
                        std::move(bytes)));
                    return result;
                } else if constexpr (
                    std::is_same_v<T, DispatchStartedEvent>) {
                    return apply_dispatch_started(payload, sequence);
                } else if constexpr (
                    std::is_same_v<T, LocalDispatchEvent>) {
                    return apply_local_dispatch(payload, sequence);
                } else if constexpr (
                    std::is_same_v<T, ExecutionReportEvent>) {
                    return apply_execution(payload, sequence);
                } else if constexpr (
                    std::is_same_v<T, DispatchTimeoutEvent>) {
                    ApplyResult result;
                    const auto found = children.find(payload.child);
                    if (found == children.end() ||
                        child_terminal(found->second.state) ||
                        (found->second.state != ChildState::DispatchStarted &&
                         found->second.state !=
                             ChildState::PendingAcknowledgement &&
                         found->second.state != ChildState::CancelPending)) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }
                    suspend_child(
                        found->second,
                        sequence,
                        payload.reason.empty() ?
                            "dispatch timed out" :
                            payload.reason,
                        result);
                    return result;
                } else if constexpr (std::is_same_v<T, KillSwitchEvent>) {
                    ApplyResult result;
                    kill_switch = payload.active;
                    Bytes bytes;
                    put_u8(bytes, kill_switch ? 1U : 0U);
                    put_string(bytes, payload.reason);
                    result.records.push_back(make_record(
                        DeterministicRecordKind::KillSwitchChanged,
                        sequence,
                        std::move(bytes)));
                    return result;
                } else if constexpr (std::is_same_v<T, RoutingGateEvent>) {
                    ApplyResult result;
                    routing_enabled = payload.enabled;
                    Bytes bytes;
                    put_u8(bytes, routing_enabled ? 1U : 0U);
                    result.records.push_back(make_record(
                        DeterministicRecordKind::RoutingGateChanged,
                        sequence,
                        std::move(bytes)));
                    return result;
                } else {
                    static_assert(std::is_same_v<T, ReconciliationEvent>);
                    ApplyResult result;
                    const auto found = children.find(payload.child);
                    if (found == children.end() ||
                        !valid_reconciliation_disposition(
                            payload.disposition) ||
                        child_terminal(found->second.state) ||
                        (found->second.state == ChildState::Prepared &&
                         payload.disposition !=
                             ReconciliationDisposition::DefinitelyUnsent)) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }
                    ChildRecord& child = found->second;
                    if (payload.disposition ==
                            ReconciliationDisposition::DefinitelyUnsent &&
                        child.state != ChildState::Prepared &&
                        child.state != ChildState::Ambiguous &&
                        child.state != ChildState::DispatchStarted) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }
                    if (payload.disposition ==
                            ReconciliationDisposition::DefinitelyUnsent &&
                        child.state != ChildState::Prepared &&
                        !child.last_dispatch_attempt.has_value()) {
                        result.command_accepted = false;
                        result.command_error = CommandError::InvalidCommand;
                        return result;
                    }

                    Bytes bytes;
                    put_u64(bytes, child.id.value());
                    put_enum(bytes, payload.disposition);
                    put_u64(bytes, payload.cumulative_quantity.units);
                    result.records.push_back(make_record(
                        DeterministicRecordKind::ReconciliationOutcome,
                        sequence,
                        std::move(bytes)));
                    switch (payload.disposition) {
                        case ReconciliationDisposition::DefinitelyUnsent:
                            child.active_dispatch_attempt.reset();
                            if (child.state == ChildState::Prepared ||
                                child.last_dispatch_attempt->kind ==
                                    adapters::DispatchAttemptKind::New) {
                                child.state = ChildState::Prepared;
                                result.effects.push_back(
                                    DispatchNewEffect{
                                        new_order_effect(child)});
                            } else {
                                child.state = ChildState::CancelPending;
                                result.effects.push_back(
                                    DispatchCancelEffect{
                                        cancel_effect(child)});
                            }
                            break;
                        case ReconciliationDisposition::Working:
                            child.active_dispatch_attempt.reset();
                            if (child.state == ChildState::CancelPending) {
                                result.effects.push_back(
                                    DispatchCancelEffect{
                                        cancel_effect(child)});
                            } else {
                                child.state = ChildState::Working;
                            }
                            break;
                        case ReconciliationDisposition::TerminalCanceled:
                            if (payload.cumulative_quantity == child.filled) {
                                finalize_child(
                                    child,
                                    ChildState::Canceled,
                                    sequence,
                                    result);
                            } else {
                                suspend_child(
                                    child,
                                    sequence,
                                    "reconciliation lacks prior fills",
                                    result);
                            }
                            break;
                        case ReconciliationDisposition::TerminalFilled:
                            if (payload.cumulative_quantity == child.ordered &&
                                child.filled == child.ordered) {
                                finalize_child(
                                    child,
                                    ChildState::Filled,
                                    sequence,
                                    result);
                            } else {
                                suspend_child(
                                    child,
                                    sequence,
                                    "filled reconciliation is incomplete",
                                    result);
                            }
                            break;
                        case ReconciliationDisposition::TerminalRejected:
                            finalize_child(
                                child,
                                ChildState::Rejected,
                                sequence,
                                result);
                            break;
                        case ReconciliationDisposition::Unresolved:
                            suspend_child(
                                child,
                                sequence,
                                "reconciliation unresolved",
                                result);
                            break;
                    }
                    return result;
                }
            },
            event.payload);
    }

    Bytes save_state() const {
        constexpr std::uint32_t state_magic = 0x53584353U;
        constexpr std::uint16_t state_version = 5U;
        Bytes output;
        put_u32(output, state_magic);
        put_u16(output, state_version);
        put_u64(output, last_ingress_sequence);
        put_u64(output, next_child_id);
        put_u64(output, last_dispatch_attempt_id);
        put_u64(output, candidates->generation());
        put_u8(output, kill_switch ? 1U : 0U);
        put_u8(output, routing_enabled ? 1U : 0U);

        put_u32(output, static_cast<std::uint32_t>(runtime_views.size()));
        for (const auto& [key, view] : runtime_views) {
            put_u32(output, key.first.value());
            put_u32(output, key.second.value());
            put_u32(output, view.venue.value());
            put_u64(output, view.adjacency_generation);
            put_enum(output, view.market_data_state);
            put_enum(output, view.order_entry_state);
            put_enum(output, view.egress_state);
            put_enum(output, view.instrument_state);
            put_u8(output, view.feed_sufficiently_fresh ? 1U : 0U);
            put_u8(output, view.conversion_compatible ? 1U : 0U);
            put_u8(output, view.risk_allowed ? 1U : 0U);
            put_u8(output, view.book.has_value() ? 1U : 0U);
            if (view.book.has_value()) {
                const md::BookView& book = *view.book;
                put_u32(output, book.venue.value());
                put_u32(output, book.channel.value());
                put_u32(output, book.profile.value());
                put_u32(output, book.instrument.value());
                put_u16(output, book.bid_count);
                put_u16(output, book.ask_count);
                for (std::size_t index = 0U; index < book.bid_count; ++index) {
                    put_i64(output, book.bids[index].price.units);
                    put_u64(output, book.bids[index].quantity.units);
                    put_u32(output, book.bids[index].order_count);
                }
                for (std::size_t index = 0U; index < book.ask_count; ++index) {
                    put_i64(output, book.asks[index].price.units);
                    put_u64(output, book.asks[index].quantity.units);
                    put_u32(output, book.asks[index].order_count);
                }
                put_u64(output, book.feed_session_epoch);
                put_u64(output, book.recovery_generation);
                put_u64(output, book.view_generation);
                put_u64(output, book.last_applied_sequence);
                put_u64(output, book.next_sequence);
                put_i64(output, book.exchange_timestamp_ns);
                put_u64(output, book.local_receive_time_ns);
                put_u64(output, book.local_publication_time_ns);
                put_enum(output, book.feed_state);
                put_enum(output, book.trading_state);
                put_u8(output, book.depth_truncated ? 1U : 0U);
            }
        }

        put_u32(output, static_cast<std::uint32_t>(sessions.size()));
        for (const auto& [venue, session] : sessions) {
            put_u32(output, venue.value());
            put_u64(output, session.first);
            put_enum(output, session.second);
        }

        put_u32(output, static_cast<std::uint32_t>(parents.size()));
        for (const auto& [id, parent] : parents) {
            static_cast<void>(id);
            const Bytes order = encode_parent_order(parent.order);
            output.insert(output.end(), order.begin(), order.end());
            put_u64(output, parent.revision);
            put_u64(output, parent.requested_total.units);
            put_u64(output, parent.cumulative_filled.units);
            put_u64(output, parent.explicit_remainder.units);
            put_enum(output, parent.state);
            put_u32(output, static_cast<std::uint32_t>(parent.children.size()));
            for (const ChildOrderId child : parent.children) {
                put_u64(output, child.value());
            }
            put_u8(output, parent.pending_replace.has_value() ? 1U : 0U);
            if (parent.pending_replace.has_value()) {
                const Bytes replacement =
                    encode_parent_replace(*parent.pending_replace);
                output.insert(
                    output.end(), replacement.begin(), replacement.end());
            }
        }

        put_u32(output, static_cast<std::uint32_t>(children.size()));
        for (const auto& [id, child] : children) {
            static_cast<void>(id);
            put_u64(output, child.id.value());
            put_u64(output, child.parent.value());
            put_u32(output, child.instrument.value());
            put_u64(output, child.parent_revision);
            put_u32(output, child.venue.value());
            put_u64(output, child.session_epoch);
            put_string(output, child.wire_order_id);
            put_string(output, child.wire_symbol);
            put_wire_profile(output, child.wire_profile);
            put_enum(output, child.side);
            put_enum(output, child.intent);
            put_u64(output, child.ordered.units);
            put_u64(output, child.filled.units);
            put_i64(output, child.limit_price.units);
            put_u64(output, child.venue_ordered.units);
            put_i64(output, child.venue_limit_price.units);
            put_u64(output, child.price_conversion.numerator());
            put_u64(output, child.price_conversion.denominator());
            put_u64(output, child.quantity_conversion.numerator());
            put_u64(output, child.quantity_conversion.denominator());
            put_enum(output, child.state);
            put_u64(output, child.risk_reservation.units);
            put_u8(output, child.pending_terminal_status.has_value() ? 1U : 0U);
            if (child.pending_terminal_status.has_value()) {
                put_enum(output, *child.pending_terminal_status);
            }
            put_u64(output, child.pending_terminal_cumulative.units);
            put_string(output, child.last_cancel_wire_id);
            put_u8(
                output,
                child.active_dispatch_attempt.has_value() ? 1U : 0U);
            if (child.active_dispatch_attempt.has_value()) {
                put_dispatch_attempt(
                    output, *child.active_dispatch_attempt);
            }
            put_u8(
                output,
                child.last_dispatch_attempt.has_value() ? 1U : 0U);
            if (child.last_dispatch_attempt.has_value()) {
                put_dispatch_attempt(
                    output, *child.last_dispatch_attempt);
            }
            if (child.pending_trade_reports.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error(
                    "too many pending trades in core state");
            }
            put_u32(
                output,
                static_cast<std::uint32_t>(
                    child.pending_trade_reports.size()));
            for (const auto& [cumulative, report] :
                 child.pending_trade_reports) {
                static_cast<void>(cumulative);
                put_execution_report(output, report);
            }
        }

        put_u32(output, static_cast<std::uint32_t>(observed_reports.size()));
        for (const auto& [key, observed] : observed_reports) {
            static_cast<void>(observed);
            put_u32(output, std::get<0>(key).value());
            put_u64(output, std::get<1>(key));
            put_string(output, std::get<2>(key));
        }
        return output;
    }

    void restore_state(const std::span<const std::byte> state) {
        constexpr std::uint32_t state_magic = 0x53584353U;
        constexpr std::uint16_t state_version = 5U;
        StateReader reader(state);
        if (reader.u32() != state_magic || reader.u16() != state_version) {
            throw std::runtime_error("incompatible core state image");
        }
        const std::uint64_t restored_last_sequence = reader.u64();
        const std::uint64_t restored_next_child = reader.u64();
        const std::uint64_t restored_last_dispatch_attempt = reader.u64();
        const std::uint64_t restored_candidate_generation = reader.u64();
        if (restored_candidate_generation != candidates->generation()) {
            throw std::runtime_error(
                "core state candidate-table generation mismatch");
        }
        const bool restored_kill_switch = reader.u8() != 0U;
        const bool restored_routing_enabled = reader.u8() != 0U;

        std::map<RuntimeKey, CandidateRuntimeView> restored_views;
        const std::uint32_t view_count = reader.u32();
        for (std::uint32_t view_index = 0U;
             view_index < view_count;
             ++view_index) {
            const InstrumentId instrument{reader.u32()};
            const VenueId key_venue{reader.u32()};
            CandidateRuntimeView view;
            view.venue = VenueId{reader.u32()};
            view.adjacency_generation = reader.u64();
            view.market_data_state =
                static_cast<MarketDataState>(reader.u8());
            view.order_entry_state =
                static_cast<OrderEntryState>(reader.u8());
            view.egress_state = static_cast<EgressState>(reader.u8());
            view.instrument_state =
                static_cast<InstrumentState>(reader.u8());
            view.feed_sufficiently_fresh = reader.u8() != 0U;
            view.conversion_compatible = reader.u8() != 0U;
            view.risk_allowed = reader.u8() != 0U;
            if (!instrument || !key_venue || view.venue != key_venue ||
                !valid_order_entry_state(view.order_entry_state)) {
                throw std::runtime_error(
                    "invalid runtime-view identity in core state");
            }
            if (reader.u8() != 0U) {
                md::BookView book;
                book.venue = VenueId{reader.u32()};
                book.channel = ChannelId{reader.u32()};
                book.profile = ProfileId{reader.u32()};
                book.instrument = InstrumentId{reader.u32()};
                book.bid_count = reader.u16();
                book.ask_count = reader.u16();
                if (book.bid_count > md::kMaxDepthLevels ||
                    book.ask_count > md::kMaxDepthLevels) {
                    throw std::runtime_error(
                        "invalid book depth in core state");
                }
                for (std::size_t index = 0U;
                     index < book.bid_count;
                     ++index) {
                    book.bids[index] = md::Level{
                        Price{reader.i64()},
                        Quantity{reader.u64()},
                        reader.u32(),
                    };
                }
                for (std::size_t index = 0U;
                     index < book.ask_count;
                     ++index) {
                    book.asks[index] = md::Level{
                        Price{reader.i64()},
                        Quantity{reader.u64()},
                        reader.u32(),
                    };
                }
                book.feed_session_epoch = reader.u64();
                book.recovery_generation = reader.u64();
                book.view_generation = reader.u64();
                book.last_applied_sequence = reader.u64();
                book.next_sequence = reader.u64();
                book.exchange_timestamp_ns = reader.i64();
                book.local_receive_time_ns = reader.u64();
                book.local_publication_time_ns = reader.u64();
                book.feed_state =
                    static_cast<MarketDataState>(reader.u8());
                book.trading_state =
                    static_cast<InstrumentState>(reader.u8());
                book.depth_truncated = reader.u8() != 0U;
                if (book.venue != view.venue ||
                    book.instrument != instrument || !book.valid()) {
                    throw std::runtime_error(
                        "invalid book identity in core state");
                }
                view.book = book;
            }
            if (!restored_views.emplace(
                    RuntimeKey{instrument, key_venue}, std::move(view))
                     .second) {
                throw std::runtime_error(
                    "duplicate runtime view in core state");
            }
        }

        std::map<VenueId, std::pair<std::uint64_t, OrderEntryState>>
            restored_sessions;
        const std::uint32_t session_count = reader.u32();
        for (std::uint32_t index = 0U; index < session_count; ++index) {
            const VenueId venue{reader.u32()};
            const std::uint64_t epoch = reader.u64();
            const auto state_value =
                static_cast<OrderEntryState>(reader.u8());
            if (!venue || epoch == 0U ||
                !valid_order_entry_state(state_value) ||
                !restored_sessions.emplace(
                    venue, std::pair{epoch, state_value}).second) {
                throw std::runtime_error(
                    "invalid session in core state");
            }
        }

        std::map<ParentOrderId, ParentRecord> restored_parents;
        const std::uint32_t parent_count = reader.u32();
        for (std::uint32_t index = 0U; index < parent_count; ++index) {
            ParentRecord parent;
            parent.order.id = ParentOrderId{reader.u64()};
            parent.order.instrument = InstrumentId{reader.u32()};
            parent.order.side = static_cast<Side>(reader.u8());
            parent.order.quantity = Quantity{reader.u64()};
            parent.order.limit_price = Price{reader.i64()};
            parent.order.intent =
                static_cast<RoutingIntent>(reader.u8());
            parent.revision = reader.u64();
            parent.requested_total = Quantity{reader.u64()};
            parent.cumulative_filled = Quantity{reader.u64()};
            parent.explicit_remainder = Quantity{reader.u64()};
            parent.state = static_cast<ParentState>(reader.u8());
            const std::uint32_t child_count = reader.u32();
            parent.children.reserve(child_count);
            for (std::uint32_t child_index = 0U;
                 child_index < child_count;
                 ++child_index) {
                parent.children.push_back(
                    ChildOrderId{reader.u64()});
            }
            if (reader.u8() != 0U) {
                ParentReplace replacement;
                replacement.id = ParentOrderId{reader.u64()};
                replacement.new_total_quantity =
                    Quantity{reader.u64()};
                replacement.new_limit_price = Price{reader.i64()};
                parent.pending_replace = replacement;
            }
            if (!parent.order.id || !parent.order.instrument ||
                !valid_side(parent.order.side) ||
                !valid_routing_intent(parent.order.intent) ||
                !valid_parent_state(parent.state) ||
                parent.revision == 0U ||
                !restored_parents.emplace(
                    parent.order.id, std::move(parent)).second) {
                throw std::runtime_error(
                    "invalid parent in core state");
            }
        }

        std::map<ChildOrderId, ChildRecord> restored_children;
        std::map<WireKey, ChildOrderId> restored_wire_map;
        std::set<std::string> restored_wire_identities;
        std::map<InstrumentId, Quantity> restored_instrument_reservations;
        std::map<VenueId, Quantity> restored_venue_reservations;
        std::uint64_t greatest_child_id = 0U;
        std::uint64_t greatest_dispatch_attempt_id = 0U;
        const std::uint32_t child_count = reader.u32();
        for (std::uint32_t index = 0U; index < child_count; ++index) {
            ChildRecord child;
            child.id = ChildOrderId{reader.u64()};
            child.parent = ParentOrderId{reader.u64()};
            child.instrument = InstrumentId{reader.u32()};
            child.parent_revision = reader.u64();
            child.venue = VenueId{reader.u32()};
            child.session_epoch = reader.u64();
            child.wire_order_id = reader.string();
            child.wire_symbol = reader.string();
            child.wire_profile = read_wire_profile(reader);
            child.side = static_cast<Side>(reader.u8());
            child.intent = static_cast<RoutingIntent>(reader.u8());
            child.ordered = Quantity{reader.u64()};
            child.filled = Quantity{reader.u64()};
            child.limit_price = Price{reader.i64()};
            child.venue_ordered = Quantity{reader.u64()};
            child.venue_limit_price = Price{reader.i64()};
            const std::uint64_t price_numerator = reader.u64();
            const std::uint64_t price_denominator = reader.u64();
            const std::uint64_t quantity_numerator = reader.u64();
            const std::uint64_t quantity_denominator = reader.u64();
            const auto price_conversion = RationalConversion::create(
                price_numerator, price_denominator);
            const auto quantity_conversion = RationalConversion::create(
                quantity_numerator, quantity_denominator);
            if (!price_conversion.has_value() ||
                !quantity_conversion.has_value()) {
                throw std::runtime_error(
                    "invalid child conversion in core state");
            }
            child.price_conversion = *price_conversion;
            child.quantity_conversion = *quantity_conversion;
            child.state = static_cast<ChildState>(reader.u8());
            child.risk_reservation = Quantity{reader.u64()};
            if (reader.u8() != 0U) {
                child.pending_terminal_status =
                    static_cast<NormalizedOrderStatus>(reader.u8());
            }
            child.pending_terminal_cumulative =
                Quantity{reader.u64()};
            child.last_cancel_wire_id = reader.string();
            if (reader.u8() != 0U) {
                child.active_dispatch_attempt =
                    read_dispatch_attempt(reader);
            }
            if (reader.u8() != 0U) {
                child.last_dispatch_attempt =
                    read_dispatch_attempt(reader);
            }
            const std::uint32_t pending_trade_count = reader.u32();
            if (pending_trade_count > child.ordered.units) {
                throw std::runtime_error(
                    "invalid pending trade count in core state");
            }
            for (std::uint32_t pending_index = 0U;
                 pending_index < pending_trade_count;
                 ++pending_index) {
                auto report = read_execution_report(reader);
                if (!child.pending_trade_reports.emplace(
                        report.cumulative_quantity.units,
                        std::move(report)).second) {
                    throw std::runtime_error(
                        "duplicate pending CumQty in core state");
                }
            }

            const auto valid_attempt =
                [&child, restored_last_dispatch_attempt](
                    const DispatchAttemptSnapshot& attempt) {
                    if (!valid_dispatch_attempt_kind(attempt.kind) ||
                        attempt.attempt_id == 0U ||
                        attempt.attempt_id >
                            restored_last_dispatch_attempt ||
                        attempt.venue != child.venue ||
                        attempt.session_epoch != child.session_epoch) {
                        return false;
                    }
                    const std::string& expected_wire =
                        attempt.kind ==
                                adapters::DispatchAttemptKind::New ?
                            child.wire_order_id :
                            child.last_cancel_wire_id;
                    return !expected_wire.empty() &&
                           attempt.wire_order_id == expected_wire;
                };
            const bool valid_active_state =
                !child.active_dispatch_attempt.has_value() ||
                (child.active_dispatch_attempt->kind ==
                         adapters::DispatchAttemptKind::New ?
                     (child.state == ChildState::DispatchStarted ||
                      child.state == ChildState::CancelPending) :
                     child.state == ChildState::CancelPending);
            const bool valid_pending_trades =
                pending_trade_chain_valid(child) &&
                (child.pending_trade_reports.empty() ||
                 (child.state == ChildState::Ambiguous &&
                  *checked_subtract(
                      child.pending_trade_reports.begin()
                          ->second.cumulative_quantity,
                      *child.pending_trade_reports.begin()
                           ->second.last_quantity) > child.filled));
            const auto converted_venue_ordered =
                child.quantity_conversion.convert_exact(child.ordered);
            const auto converted_venue_price =
                child.price_conversion.convert_exact(child.limit_price);
            const auto restored_profile = wire_profiles.find(child.venue);
            if (!child.id || !child.parent || !child.instrument ||
                !child.venue || child.session_epoch == 0U ||
                child.wire_order_id.empty() ||
                child.wire_order_id.size() >
                    adapters::kMaximumWireOrderIdLength ||
                child.last_cancel_wire_id.empty() ||
                child.last_cancel_wire_id.size() >
                    adapters::kMaximumWireOrderIdLength ||
                child.wire_order_id == child.last_cancel_wire_id ||
                restored_profile == wire_profiles.end() ||
                child.wire_profile != restored_profile->second ||
                !child.ordered || child.limit_price.units <= 0 ||
                !child.venue_ordered ||
                child.venue_limit_price.units <= 0 ||
                !converted_venue_ordered.has_value() ||
                !converted_venue_price.has_value() ||
                *converted_venue_ordered != child.venue_ordered ||
                *converted_venue_price != child.venue_limit_price ||
                child.filled > child.ordered ||
                !valid_side(child.side) ||
                !valid_routing_intent(child.intent) ||
                !valid_child_state(child.state) ||
                (child.pending_terminal_status.has_value() &&
                 !valid_normalized_order_status(
                     *child.pending_terminal_status)) ||
                (child.active_dispatch_attempt.has_value() &&
                 !valid_attempt(*child.active_dispatch_attempt)) ||
                (child.last_dispatch_attempt.has_value() &&
                 !valid_attempt(*child.last_dispatch_attempt)) ||
                (child.active_dispatch_attempt.has_value() &&
                 (!child.last_dispatch_attempt.has_value() ||
                  *child.active_dispatch_attempt !=
                      *child.last_dispatch_attempt)) ||
                !valid_active_state ||
                !valid_pending_trades ||
                (child.state == ChildState::DispatchStarted &&
                 (!child.active_dispatch_attempt.has_value() ||
                  child.active_dispatch_attempt->kind !=
                      adapters::DispatchAttemptKind::New)) ||
                (child_terminal(child.state) &&
                 child.active_dispatch_attempt.has_value()) ||
                !restored_parents.contains(child.parent)) {
                throw std::runtime_error(
                    "invalid child in core state");
            }
            greatest_child_id =
                std::max(greatest_child_id, child.id.value());
            if (child.last_dispatch_attempt.has_value()) {
                greatest_dispatch_attempt_id = std::max(
                    greatest_dispatch_attempt_id,
                    child.last_dispatch_attempt->attempt_id);
            }
            if (!restored_wire_identities
                     .insert(child.wire_order_id).second ||
                !restored_wire_identities
                     .insert(child.last_cancel_wire_id).second) {
                throw std::runtime_error(
                    "duplicate global wire identity in core state");
            }
            if (!restored_wire_map.emplace(
                    WireKey{
                        child.venue,
                        child.session_epoch,
                        child.wire_order_id,
                    },
                    child.id)
                     .second) {
                throw std::runtime_error(
                    "duplicate wire identity in core state");
            }
            if (child.risk_reservation) {
                const auto instrument_total = checked_add(
                    restored_instrument_reservations[child.instrument],
                    child.risk_reservation);
                const auto venue_total = checked_add(
                    restored_venue_reservations[child.venue],
                    child.risk_reservation);
                if (!instrument_total.has_value() ||
                    !venue_total.has_value()) {
                    throw std::runtime_error(
                        "risk reservation overflow in core state");
                }
                restored_instrument_reservations[child.instrument] =
                    *instrument_total;
                restored_venue_reservations[child.venue] =
                    *venue_total;
            }
            if (!restored_children.emplace(
                    child.id, std::move(child)).second) {
                throw std::runtime_error(
                    "duplicate child in core state");
            }
        }

        for (const auto& [id, parent] : restored_parents) {
            for (const ChildOrderId child_id : parent.children) {
                const auto child = restored_children.find(child_id);
                if (child == restored_children.end() ||
                    child->second.parent != id) {
                    throw std::runtime_error(
                        "parent/child ownership mismatch in core state");
                }
            }
        }

        std::map<ReportKey, bool> restored_reports;
        const std::uint32_t report_count = reader.u32();
        for (std::uint32_t index = 0U; index < report_count; ++index) {
            const ReportKey key{
                VenueId{reader.u32()},
                reader.u64(),
                reader.string(),
            };
            if (!std::get<0>(key) || std::get<1>(key) == 0U ||
                std::get<2>(key).empty() ||
                !restored_reports.emplace(key, true).second) {
                throw std::runtime_error(
                    "invalid report dedupe key in core state");
            }
        }
        for (const auto& [child_id, child] : restored_children) {
            static_cast<void>(child_id);
            for (const auto& [cumulative, report] :
                 child.pending_trade_reports) {
                static_cast<void>(cumulative);
                const std::string& identity =
                    execution_report_identity(report);
                if (identity.empty() ||
                    !restored_reports.contains(ReportKey{
                        report.venue,
                        report.session_epoch,
                        identity,
                    })) {
                    throw std::runtime_error(
                        "pending trade lacks dedupe identity in core state");
                }
            }
        }
        reader.require_end();
        if (restored_next_child == 0U ||
            restored_next_child <= greatest_child_id) {
            throw std::runtime_error(
                "invalid next child ID in core state");
        }
        if (restored_last_dispatch_attempt !=
            greatest_dispatch_attempt_id) {
            throw std::runtime_error(
                "invalid dispatch-attempt counter in core state");
        }

        runtime_views = std::move(restored_views);
        sessions = std::move(restored_sessions);
        parents = std::move(restored_parents);
        children = std::move(restored_children);
        wire_to_child = std::move(restored_wire_map);
        observed_reports = std::move(restored_reports);
        instrument_reservations =
            std::move(restored_instrument_reservations);
        venue_reservations = std::move(restored_venue_reservations);
        next_child_id = restored_next_child;
        last_dispatch_attempt_id =
            restored_last_dispatch_attempt;
        last_ingress_sequence = restored_last_sequence;
        kill_switch = restored_kill_switch;
        routing_enabled = restored_routing_enabled;
    }

    void begin_recovery() {
        routing_enabled = false;
        for (auto& [key, view] : runtime_views) {
            static_cast<void>(key);
            view.market_data_state = MarketDataState::Recovering;
            view.order_entry_state = OrderEntryState::Disconnected;
            view.feed_sufficiently_fresh = false;
            ++view.adjacency_generation;
            if (view.book.has_value()) {
                view.book->feed_state = MarketDataState::Recovering;
                ++view.book->view_generation;
            }
        }
        for (auto& [venue, session] : sessions) {
            static_cast<void>(venue);
            session.second = OrderEntryState::Disconnected;
        }
    }

    bool recovery_consistent() const {
        for (const auto& [id, child] : children) {
            static_cast<void>(id);
            if (child.state == ChildState::Prepared ||
                child.state == ChildState::DispatchStarted ||
                child.state == ChildState::PendingAcknowledgement ||
                child.state == ChildState::Ambiguous ||
                child.active_dispatch_attempt.has_value() ||
                child.pending_terminal_status.has_value() ||
                !child.pending_trade_reports.empty()) {
                return false;
            }
            if (!child_terminal(child.state) &&
                child.state != ChildState::Working &&
                child.state != ChildState::CancelPending) {
                return false;
            }
        }
        for (const auto& set : candidates->entries()) {
            for (const auto& candidate : set.candidates) {
                if (candidate.admin_state == AdminState::Down) {
                    continue;
                }
                const auto found =
                    runtime_views.find({set.instrument, candidate.venue});
                if (found == runtime_views.end()) {
                    return false;
                }
                const CandidateRuntimeView& view = found->second;
                if (!view.book.has_value() ||
                    !view.book->valid() ||
                    view.market_data_state != MarketDataState::Live ||
                    view.book->feed_state != MarketDataState::Live ||
                    !view.feed_sufficiently_fresh ||
                    view.instrument_state != InstrumentState::Trading ||
                    view.book->trading_state != InstrumentState::Trading ||
                    view.order_entry_state != OrderEntryState::Ready) {
                    return false;
                }
            }
        }
        return true;
    }

    RouteTrace trace(const ParentOrder& order) const {
        RouteTrace output;
        output.risk_generation = configuration.risk.generation;
        output.hard_rejection =
            validate_parent(order, order.quantity, false);
        output.hard_risk_accepted =
            output.hard_rejection == CommandError::None;
        if (output.hard_risk_accepted) {
            output.decision = plan_route(order);
        }
        return output;
    }

    SorSnapshot snapshot() const {
        SorSnapshot output;
        output.last_ingress_sequence = last_ingress_sequence;
        output.last_dispatch_attempt_id = last_dispatch_attempt_id;
        output.candidate_table_generation = candidates->generation();
        output.risk_generation = configuration.risk.generation;
        output.kill_switch = kill_switch;
        output.routing_enabled = routing_enabled;
        output.parents.reserve(parents.size());
        for (const auto& [id, parent] : parents) {
            static_cast<void>(id);
            output.parents.push_back(ParentSnapshot{
                parent.order,
                parent.revision,
                parent.requested_total,
                parent.cumulative_filled,
                parent.explicit_remainder,
                parent.state,
                parent.children,
                parent.pending_replace,
            });
        }
        output.children.reserve(children.size());
        for (const auto& [id, child] : children) {
            static_cast<void>(id);
            output.children.push_back(ChildSnapshot{
                child.id,
                child.parent,
                child.parent_revision,
                child.venue,
                child.session_epoch,
                child.wire_order_id,
                child.ordered,
                child.filled,
                child.limit_price,
                child.state,
                child.risk_reservation,
                child.active_dispatch_attempt,
                child.venue_ordered,
                child.venue_limit_price,
                child.price_conversion,
                child.quantity_conversion,
                child.wire_profile,
                child.last_cancel_wire_id,
            });
        }
        return output;
    }
};

SorCore::SorCore(
    CoreConfiguration configuration,
    std::shared_ptr<const CandidateTable> candidates)
    : impl_(std::make_unique<Impl>(
          std::move(configuration),
          candidates,
          std::make_shared<DirectCoreTestWireIdentityFactory>(
              candidates))) {}

SorCore::SorCore(
    CoreConfiguration configuration,
    std::shared_ptr<const CandidateTable> candidates,
    std::shared_ptr<const adapters::WireIdentityFactory>
        input_wire_identity_factory)
    : impl_(std::make_unique<Impl>(
          std::move(configuration),
          std::move(candidates),
          std::move(input_wire_identity_factory))) {}

SorCore::~SorCore() = default;
SorCore::SorCore(SorCore&&) noexcept = default;
SorCore& SorCore::operator=(SorCore&&) noexcept = default;

ApplyResult SorCore::apply(const CoreEvent& event) {
    return impl_->apply(event);
}

RouteTrace SorCore::trace(const ParentOrder& order) const {
    return impl_->trace(order);
}

SorSnapshot SorCore::snapshot() const {
    return impl_->snapshot();
}

std::vector<std::byte> SorCore::save_state() const {
    return impl_->save_state();
}

void SorCore::restore_state(const std::span<const std::byte> state) {
    impl_->restore_state(state);
}

void SorCore::begin_recovery() {
    impl_->begin_recovery();
}

bool SorCore::recovery_consistent() const {
    return impl_->recovery_consistent();
}

}  // namespace sorx

