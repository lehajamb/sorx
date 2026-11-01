#include "sorx/adapters/mockx_fix44.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace sorx::adapters::mockx {
namespace {

constexpr std::string_view kMockXFix44V1ProfileName{"MockXFix44V1"};

bool locked_v1_config(const MockXFix44Config& config) noexcept {
    return config.price_decimal_places == 2U &&
           config.quantity_decimal_places == 0U &&
           config.maximum_clordid_length == 32U &&
           config.clordid_prefix == "SX";
}

const std::string* find_field(
    const std::span<const FixField> fields,
    const int tag) noexcept {
    const auto found = std::find_if(
        fields.begin(),
        fields.end(),
        [tag](const FixField& field) { return field.tag == tag; });
    return found == fields.end() ? nullptr : &found->value;
}

std::string format_unsigned_decimal(
    const std::uint64_t units,
    const std::uint8_t decimal_places) {
    std::string digits = std::to_string(units);
    if (decimal_places == 0U) {
        return digits;
    }
    if (digits.size() <= decimal_places) {
        digits.insert(
            0U,
            static_cast<std::size_t>(decimal_places) + 1U - digits.size(),
            '0');
    }
    digits.insert(digits.size() - decimal_places, 1U, '.');
    return digits;
}

std::optional<std::uint64_t> parse_unsigned_decimal(
    const std::string_view text,
    const std::uint8_t decimal_places) noexcept {
    if (text.empty() || text.front() == '-') {
        return std::nullopt;
    }
    const std::size_t decimal = text.find('.');
    if (decimal != std::string_view::npos &&
        text.find('.', decimal + 1U) != std::string_view::npos) {
        return std::nullopt;
    }

    std::string digits;
    digits.reserve(text.size() + decimal_places);
    const std::string_view whole =
        decimal == std::string_view::npos ? text : text.substr(0U, decimal);
    std::string_view fraction =
        decimal == std::string_view::npos ? std::string_view{} :
                                            text.substr(decimal + 1U);
    if (whole.empty() || fraction.size() > decimal_places) {
        return std::nullopt;
    }
    if (!std::all_of(
            whole.begin(), whole.end(),
            [](const char value) {
                return std::isdigit(static_cast<unsigned char>(value)) != 0;
            }) ||
        !std::all_of(
            fraction.begin(), fraction.end(),
            [](const char value) {
                return std::isdigit(static_cast<unsigned char>(value)) != 0;
            })) {
        return std::nullopt;
    }

    digits.append(whole);
    digits.append(fraction);
    digits.append(
        static_cast<std::size_t>(decimal_places) - fraction.size(),
        '0');
    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        return 0U;
    }
    digits.erase(0U, first_nonzero);

    std::uint64_t value = 0U;
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Quantity> parse_quantity(
    const std::string* value,
    const std::uint8_t scale) noexcept {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto parsed = parse_unsigned_decimal(*value, scale);
    return parsed.has_value() ? std::optional<Quantity>{Quantity{*parsed}} :
                                std::nullopt;
}

std::optional<Price> parse_price(
    const std::string* value,
    const std::uint8_t scale) noexcept {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto parsed = parse_unsigned_decimal(*value, scale);
    if (!parsed.has_value() ||
        *parsed > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return Price{static_cast<std::int64_t>(*parsed)};
}

std::optional<ExecutionKind> execution_kind(const std::string_view value) noexcept {
    if (value == "0") {
        return ExecutionKind::Acknowledged;
    }
    if (value == "F") {
        return ExecutionKind::Trade;
    }
    if (value == "4") {
        return ExecutionKind::Canceled;
    }
    if (value == "8") {
        return ExecutionKind::Rejected;
    }
    return std::nullopt;
}

std::optional<NormalizedOrderStatus> order_status(
    const std::string_view value) noexcept {
    if (value == "0") {
        return NormalizedOrderStatus::Working;
    }
    if (value == "1") {
        return NormalizedOrderStatus::PartiallyFilled;
    }
    if (value == "2") {
        return NormalizedOrderStatus::Filled;
    }
    if (value == "4") {
        return NormalizedOrderStatus::Canceled;
    }
    if (value == "8") {
        return NormalizedOrderStatus::Rejected;
    }
    return std::nullopt;
}

MessageBuildResult build_error(std::string error) {
    return MessageBuildResult{false, {}, std::move(error)};
}

NormalizeResult normalize_error(std::string error) {
    return NormalizeResult{false, {}, std::move(error)};
}

bool resolve_new_order_units(
    const NewOrderIntent& intent,
    Quantity& venue_quantity,
    Price& venue_price) noexcept {
    const bool any_explicit =
        static_cast<bool>(intent.canonical_quantity) ||
        intent.canonical_limit_price.units != 0 ||
        static_cast<bool>(intent.venue_quantity) ||
        intent.venue_limit_price.units != 0;
    if (!any_explicit) {
        venue_quantity = intent.quantity;
        venue_price = intent.limit_price;
        return static_cast<bool>(venue_quantity) &&
               venue_price.units > 0;
    }

    if (!intent.canonical_quantity ||
        intent.canonical_limit_price.units <= 0 ||
        !intent.venue_quantity ||
        intent.venue_limit_price.units <= 0 ||
        (intent.quantity &&
         intent.quantity != intent.canonical_quantity) ||
        (intent.limit_price.units != 0 &&
         intent.limit_price != intent.canonical_limit_price)) {
        return false;
    }
    venue_quantity = intent.venue_quantity;
    venue_price = intent.venue_limit_price;
    return true;
}

bool resolve_cancel_units(
    const CancelOrderIntent& intent,
    Quantity& venue_quantity) noexcept {
    const bool any_explicit =
        static_cast<bool>(intent.canonical_original_quantity) ||
        static_cast<bool>(intent.venue_original_quantity);
    if (!any_explicit) {
        venue_quantity = intent.original_quantity;
        return static_cast<bool>(venue_quantity);
    }

    if (!intent.canonical_original_quantity ||
        !intent.venue_original_quantity ||
        (intent.original_quantity &&
         intent.original_quantity !=
             intent.canonical_original_quantity)) {
        return false;
    }
    venue_quantity = intent.venue_original_quantity;
    return true;
}

}  // namespace

struct MockXFix44WireIdentityFactory::Impl final {
    struct Entry final {
        WireProfileIdentity identity;
        MockXFix44Profile profile;

        Entry(
            WireProfileIdentity input_identity,
            MockXFix44Config input_config)
            : identity(std::move(input_identity)),
              profile(std::move(input_config)) {}
    };

    std::vector<WireProfileIdentity> identities;
    std::vector<Entry> entries;
};

MockXFix44WireIdentityFactory::MockXFix44WireIdentityFactory(
    std::vector<MockXFix44WireIdentityBinding> bindings) {
    std::sort(
        bindings.begin(),
        bindings.end(),
        [](const auto& left, const auto& right) {
            return left.identity.venue < right.identity.venue;
        });
    auto built = std::make_shared<Impl>();
    built->identities.reserve(bindings.size());
    built->entries.reserve(bindings.size());
    VenueId previous_venue{};
    for (auto& binding : bindings) {
        const auto& identity = binding.identity;
        if (!identity.venue || !identity.profile ||
            identity.profile_name != kMockXFix44V1ProfileName ||
            !locked_v1_config(binding.config) ||
            identity.session_identity.empty() ||
            identity.dictionary_identity.empty() ||
            identity.sender_comp_id.empty() ||
            identity.target_comp_id.empty() ||
            identity.venue == previous_venue) {
            throw std::invalid_argument(
                "invalid or duplicate MockX wire profile binding");
        }
        previous_venue = identity.venue;
        built->identities.push_back(identity);
        built->entries.emplace_back(
            std::move(binding.identity), std::move(binding.config));
    }
    if (built->entries.empty()) {
        throw std::invalid_argument(
            "MockX wire identity factory requires a binding");
    }
    impl_ = std::move(built);
}

std::span<const WireProfileIdentity>
MockXFix44WireIdentityFactory::profiles() const noexcept {
    return impl_->identities;
}

WireIdentityResult MockXFix44WireIdentityFactory::make(
    const WireIdentityRequest& request) const {
    WireIdentityResult result;
    result.profile = request.profile;
    if (!request.child || !request.parent ||
        request.parent_revision == 0U ||
        request.session_epoch == 0U ||
        (request.kind != DispatchAttemptKind::New &&
         request.kind != DispatchAttemptKind::Cancel)) {
        return result;
    }
    const auto found = std::find_if(
        impl_->entries.begin(),
        impl_->entries.end(),
        [&request](const Impl::Entry& entry) {
            return entry.identity == request.profile;
        });
    if (found == impl_->entries.end()) {
        return result;
    }
    try {
        result.wire_order_id = found->profile.make_wire_order_id(
            request.child,
            request.parent_revision,
            request.session_epoch,
            request.kind == DispatchAttemptKind::Cancel);
    } catch (const std::exception&) {
        result.wire_order_id.clear();
        return result;
    }
    result.profile = found->identity;
    result.ok = !result.wire_order_id.empty() &&
                result.wire_order_id.size() <=
                    kMaximumWireOrderIdLength;
    return result;
}

MockXFix44Profile::MockXFix44Profile(MockXFix44Config config)
    : config_(std::move(config)) {
    if (!locked_v1_config(config_)) {
        throw std::invalid_argument(
            "MockXFix44V1 requires locked wire configuration");
    }
}

const MockXFix44Config& MockXFix44Profile::config() const noexcept {
    return config_;
}

std::string MockXFix44Profile::make_wire_order_id(
    const ChildOrderId child,
    const std::uint64_t parent_revision,
    const std::uint64_t session_epoch,
    const bool cancel) const {
    std::string output = config_.clordid_prefix;
    output += cancel ? "C" : "N";
    output += std::to_string(session_epoch);
    output += "-";
    output += std::to_string(parent_revision);
    output += "-";
    output += std::to_string(child.value());
    if (!valid_wire_order_id(output)) {
        throw std::length_error("generated ClOrdID exceeds venue contract");
    }
    return output;
}

bool MockXFix44Profile::valid_wire_order_id(
    const std::string_view value) const noexcept {
    return !value.empty() &&
           value.size() <= config_.maximum_clordid_length &&
           std::all_of(
               value.begin(), value.end(),
               [](const char character) {
                   const auto item = static_cast<unsigned char>(character);
                   return item >= 0x21U && item <= 0x7eU &&
                          character != '=' && character != '\x01';
               });
}

MessageBuildResult MockXFix44Profile::make_new_order(
    const NewOrderIntent& intent) const {
    Quantity venue_quantity;
    Price venue_price;
    if (!resolve_new_order_units(intent, venue_quantity, venue_price) ||
        !intent.venue || !intent.parent || !intent.child || !intent.instrument ||
        (intent.side != Side::Buy && intent.side != Side::Sell) ||
        (intent.time_in_force != TimeInForce::ImmediateOrCancel &&
         intent.time_in_force != TimeInForce::GoodTillCancel) ||
        intent.session_epoch == 0U || intent.wire_symbol.empty() ||
        !valid_wire_order_id(intent.wire_order_id)) {
        return build_error("invalid new-order intent");
    }
    FixMessage message;
    message.message_type = "D";
    message.fields = {
        {11, intent.wire_order_id},
        {55, intent.wire_symbol},
        {54, intent.side == Side::Buy ? "1" : "2"},
        {38, format_unsigned_decimal(
                 venue_quantity.units, config_.quantity_decimal_places)},
        {40, "2"},
        {44, format_unsigned_decimal(
                 static_cast<std::uint64_t>(venue_price.units),
                 config_.price_decimal_places)},
        {59, intent.time_in_force == TimeInForce::ImmediateOrCancel ? "3" : "1"},
    };
    return MessageBuildResult{true, std::move(message), {}};
}

MessageBuildResult MockXFix44Profile::make_cancel(
    const CancelOrderIntent& intent) const {
    Quantity venue_quantity;
    if (!resolve_cancel_units(intent, venue_quantity) ||
        !intent.venue || !intent.parent || !intent.child ||
        (intent.side != Side::Buy && intent.side != Side::Sell) ||
        intent.session_epoch == 0U || intent.wire_symbol.empty() ||
        !valid_wire_order_id(intent.original_wire_order_id) ||
        !valid_wire_order_id(intent.cancel_wire_order_id)) {
        return build_error("invalid cancel intent");
    }
    FixMessage message;
    message.message_type = "F";
    message.fields = {
        {11, intent.cancel_wire_order_id},
        {41, intent.original_wire_order_id},
        {55, intent.wire_symbol},
        {54, intent.side == Side::Buy ? "1" : "2"},
        {38, format_unsigned_decimal(
                 venue_quantity.units,
                 config_.quantity_decimal_places)},
    };
    return MessageBuildResult{true, std::move(message), {}};
}

NormalizeResult MockXFix44Profile::normalize_execution_report(
    const VenueId venue,
    const std::uint64_t session_epoch,
    const std::span<const FixField> fields) const {
    const std::string* wire_id = find_field(fields, 11);
    const std::string* execution_id = find_field(fields, 17);
    const std::string* execution_type = find_field(fields, 150);
    const std::string* status_value = find_field(fields, 39);
    const auto kind =
        execution_type == nullptr ? std::nullopt : execution_kind(*execution_type);
    const auto status =
        status_value == nullptr ? std::nullopt : order_status(*status_value);
    const auto cumulative = parse_quantity(
        find_field(fields, 14), config_.quantity_decimal_places);
    const auto leaves = parse_quantity(
        find_field(fields, 151), config_.quantity_decimal_places);
    if (!venue || session_epoch == 0U || wire_id == nullptr ||
        execution_id == nullptr || execution_id->empty() ||
        !valid_wire_order_id(*wire_id) || !kind.has_value() ||
        !status.has_value() || !cumulative.has_value() || !leaves.has_value()) {
        return normalize_error("missing or invalid required execution-report field");
    }

    NormalizedExecutionReport report;
    report.venue = venue;
    report.session_epoch = session_epoch;
    report.wire_order_id = *wire_id;
    report.execution_id = *execution_id;
    report.durable_report_key = *execution_id;
    report.kind = *kind;
    report.status = *status;
    report.cumulative_quantity = *cumulative;
    report.leaves_quantity = *leaves;
    report.value_domain = ExecutionValueDomain::Venue;

    const std::string* last_quantity = find_field(fields, 32);
    const std::string* last_price = find_field(fields, 31);
    if (*kind == ExecutionKind::Trade) {
        report.last_quantity =
            parse_quantity(last_quantity, config_.quantity_decimal_places);
        report.last_price =
            parse_price(last_price, config_.price_decimal_places);
        if (!report.last_quantity.has_value() || !*report.last_quantity ||
            !report.last_price.has_value() || report.last_price->units <= 0) {
            return normalize_error("trade lacks exact positive LastQty/LastPx");
        }
    } else if (last_quantity != nullptr || last_price != nullptr) {
        return normalize_error("non-trade execution report carries trade fields");
    }

    if (const std::string* reason = find_field(fields, 58); reason != nullptr) {
        report.reject_reason = *reason;
    }
    return NormalizeResult{true, std::move(report), {}};
}

NormalizeResult MockXFix44Profile::normalize_cancel_reject(
    const VenueId venue,
    const std::uint64_t session_epoch,
    const std::uint64_t inbound_session_sequence,
    const std::span<const FixField> fields) const {
    const std::string* cancel_wire_id = find_field(fields, 11);
    const std::string* original_wire_id = find_field(fields, 41);
    if (!venue || session_epoch == 0U || inbound_session_sequence == 0U ||
        cancel_wire_id == nullptr || original_wire_id == nullptr ||
        !valid_wire_order_id(*cancel_wire_id) ||
        !valid_wire_order_id(*original_wire_id)) {
        return normalize_error("missing or invalid cancel-reject identity");
    }

    NormalizedExecutionReport report;
    report.venue = venue;
    report.session_epoch = session_epoch;
    report.wire_order_id = *original_wire_id;
    report.durable_report_key =
        "CXLREJ-" + std::to_string(session_epoch) + "-" +
        std::to_string(inbound_session_sequence);
    report.kind = ExecutionKind::CancelRejected;
    report.status = NormalizedOrderStatus::Working;
    report.value_domain = ExecutionValueDomain::Venue;
    if (const std::string* reason = find_field(fields, 58); reason != nullptr) {
        report.reject_reason = *reason;
    }
    return NormalizeResult{true, std::move(report), {}};
}

}  // namespace sorx::adapters::mockx





