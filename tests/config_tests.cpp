#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/config.hpp"
#include "sorx/route_rib.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sorx::AdminState;
using sorx::InstrumentId;
using sorx::Quantity;
using sorx::RouteRib;
using sorx::VenueId;
using sorx::config::ConfigurationErrorCode;
using sorx::config::ConfigurationLoader;
using sorx::config::ConfigurationResult;
using sorx::config::ValidatedConfiguration;

[[nodiscard]] std::string sorx_section() {
    return R"ini([sorx]
schema_version=1
account_id=ACCOUNT-ALPHA
maximum_aggressive_venues=2
ingress_capacity=1024
risk_generation=7

)ini";
}

[[nodiscard]] std::string instrument_section() {
    return R"ini([instrument 7]
symbol=ALPHA
allowed=true
maximum_parent_quantity=1000
maximum_child_quantity=500
maximum_parent_notional=10000000
maximum_child_notional=5000000
minimum_price=10
maximum_price=1000
maximum_open_parents=20
maximum_open_children=40
maximum_gross_outstanding=5000

)ini";
}

[[nodiscard]] std::string venue_one_section(const std::string_view credential) {
    return std::string{R"ini([venue 1]
name=MOCKX-A
admin_state=UP
risk_allowed=true
maximum_outstanding=2000
market_data_ingress_capacity=256
order_entry_ingress_capacity=128
cancel_egress_capacity=32
normal_egress_capacity=64
market_data_channel_id=101
market_data_profile_id=201
order_entry_profile_id=301
market_data_profile=MockXItchV1
market_data_session=MD-A
order_entry_profile=MockXFix44V1
order_entry_session=FIX-A
dictionary_identity=FIX44-MOCKX-2026
sender_comp_id=SORX-A
target_comp_id=MOCKX-A
credential_reference=)ini"} + std::string{credential} + "\n\n";
}

[[nodiscard]] std::string venue_two_section() {
    return R"ini([venue 2]
name=MOCKX-B
admin_state=UP
risk_allowed=true
maximum_outstanding=2000
market_data_ingress_capacity=512
order_entry_ingress_capacity=256
cancel_egress_capacity=48
normal_egress_capacity=96
market_data_channel_id=102
market_data_profile_id=201
order_entry_profile_id=301
market_data_profile=MockXItchV1
market_data_session=MD-B
order_entry_profile=MockXFix44V1
order_entry_session=FIX-B
dictionary_identity=FIX44-MOCKX-2026
sender_comp_id=SORX-B
target_comp_id=MOCKX-B

)ini";
}

[[nodiscard]] std::string route_one_section() {
    return R"ini([route 7 1]
wire_symbol=ALPHA-A
price_numerator=1
price_denominator=1
quantity_numerator=1
quantity_denominator=1
aggressive_ioc=true
passive_gtc=true
allocation_cap=500
aggressive_priority=1
passive_priority=2
configured_latency_ns=100

)ini";
}

[[nodiscard]] std::string route_two_section() {
    return R"ini([route 7 2]
wire_symbol=ALPHA-B
price_numerator=1
price_denominator=1
quantity_numerator=1
quantity_denominator=1
aggressive_ioc=true
passive_gtc=true
allocation_cap=500
aggressive_priority=2
passive_priority=1
configured_latency_ns=200
)ini";
}

[[nodiscard]] std::string valid_ini(
    const std::string_view credential = "ENV:SORX_MOCKX_A_PASSWORD") {
    return sorx_section() + instrument_section() +
           venue_one_section(credential) + venue_two_section() +
           route_one_section() + route_two_section();
}

[[nodiscard]] std::string reordered_ini() {
    const std::string reordered_sorx = R"ini(; comments and section order are non-semantic
[sorx]
risk_generation=7
ingress_capacity=1024
maximum_aggressive_venues=2
account_id=ACCOUNT-ALPHA
schema_version=1

)ini";
    return std::string{"# route declarations may precede definitions\n"} +
           route_two_section() + venue_two_section() + route_one_section() +
           instrument_section() + reordered_sorx +
           venue_one_section("ENV:SORX_MOCKX_A_PASSWORD");
}

[[nodiscard]] std::string replace_once(
    std::string input,
    const std::string_view before,
    const std::string_view after) {
    const auto position = input.find(before);
    assert(position != std::string::npos);
    input.replace(position, before.size(), after);
    return input;
}

[[nodiscard]] bool has_error(
    const ConfigurationResult& result,
    const ConfigurationErrorCode code) {
    return std::any_of(
        result.errors.begin(),
        result.errors.end(),
        [code](const auto& error) { return error.code == code; });
}

void expect_failure(
    const std::string& text,
    const ConfigurationErrorCode code) {
    const auto result = ConfigurationLoader::parse_ini(text);
    assert(!result);
    assert(!result.configuration);
    assert(has_error(result, code));
}

[[nodiscard]] std::string hash_of(const ValidatedConfiguration& configuration) {
    return sorx::config::sha256_hex(
        sorx::config::compute_route_critical_hash(configuration));
}

void valid_parse_and_model_tests() {
    const auto parsed = ConfigurationLoader::parse_ini(valid_ini());
    assert(parsed);
    assert(parsed.errors.empty());
    const auto& configuration = *parsed.configuration;
    assert(configuration.schema_version == 1);
    assert(configuration.account_id == "ACCOUNT-ALPHA");
    assert(configuration.ingress_capacity == 1024);
    assert(configuration.core.maximum_aggressive_venues == 2);
    assert(configuration.core.risk.generation == 7);
    assert(configuration.instruments.size() == 1);
    assert(configuration.venues.size() == 2);
    assert(configuration.routes.size() == 2);
    assert(configuration.instruments[0].id == InstrumentId{7});
    assert(configuration.instruments[0].canonical_symbol == "ALPHA");
    assert(configuration.venues[0].id == VenueId{1});
    assert(configuration.venues[0].admin_state == AdminState::Up);
    assert(configuration.venues[0].cancel_egress_capacity == 32);
    assert(configuration.venues[0].normal_egress_capacity == 64);
    assert(configuration.venues[0].risk.maximum_outstanding == Quantity{2000});
    assert(configuration.venues[0].credential_reference ==
           "ENV:SORX_MOCKX_A_PASSWORD");
    assert(configuration.routes[0].instrument == InstrumentId{7});
    assert(configuration.routes[0].venue == VenueId{1});
    assert(configuration.routes[0].wire_symbol == "ALPHA-A");
    assert(configuration.route_critical_hash_hex.size() == 64);
    assert(configuration.route_critical_hash_hex ==
           hash_of(configuration));

    RouteRib rib;
    std::vector<sorx::RouteValidationError> errors;
    assert(rib.replace_all(configuration.routes, errors));
    assert(errors.empty());
    sorx::FibCompiler compiler;
    const auto candidates = compiler.compile(rib);
    assert(candidates->find(InstrumentId{7}) != nullptr);
    assert(candidates->find(InstrumentId{7})->candidates.size() == 2);

    const auto missing_file = ConfigurationLoader::load_ini_file(
        std::filesystem::path{"definitely-does-not-exist-sorx-config.ini"});
    assert(!missing_file);
    assert(has_error(missing_file, ConfigurationErrorCode::Io));
}

void canonical_hash_tests() {
    const auto ordered = ConfigurationLoader::parse_ini(valid_ini());
    const auto reordered = ConfigurationLoader::parse_ini(reordered_ini());
    const auto other_credential =
        ConfigurationLoader::parse_ini(valid_ini("ENV:ROTATED_SECRET"));
    assert(ordered);
    assert(reordered);
    assert(other_credential);
    assert(ordered.configuration->route_critical_hash ==
           reordered.configuration->route_critical_hash);
    assert(ordered.configuration->route_critical_hash ==
           other_credential.configuration->route_critical_hash);

    ValidatedConfiguration empty;
    assert(hash_of(empty) ==
           "66563055ee10f18c7158419c01f51d35d627fdcac857cf4b11e984d884a94b4e");

    const auto& baseline = *ordered.configuration;
    const auto baseline_hash = hash_of(baseline);
    auto assert_sensitive = [&baseline_hash](ValidatedConfiguration changed) {
        assert(hash_of(changed) != baseline_hash);
    };

    auto changed = baseline;
    changed.account_id = "ACCOUNT-BETA";
    assert_sensitive(changed);

    changed = baseline;
    changed.core.maximum_aggressive_venues = 1;
    assert_sensitive(changed);

    changed = baseline;
    changed.instruments[0].risk.maximum_parent_quantity = Quantity{998};
    assert_sensitive(changed);

    changed = baseline;
    changed.venues[0].admin_state = AdminState::Down;
    assert_sensitive(changed);

    changed = baseline;
    changed.venues[0].cancel_egress_capacity = 33;
    assert_sensitive(changed);

    changed = baseline;
    changed.venues[0].dictionary_identity = "FIX44-MOCKX-2027";
    assert_sensitive(changed);

    changed = baseline;
    changed.venues[0].order_entry_session_identity = "FIX-A-NEW";
    assert_sensitive(changed);

    changed = baseline;
    changed.routes[0].wire_symbol = "ALPHA-A2";
    assert_sensitive(changed);

    changed = baseline;
    changed.routes[0].allocation_cap = Quantity{499};
    assert_sensitive(changed);

    changed = baseline;
    changed.routes[0].configured_latency_ns = 101;
    assert_sensitive(changed);

    changed = baseline;
    changed.routes[0].price_conversion =
        *sorx::RationalConversion::create(2, 1);
    assert_sensitive(changed);

    changed = baseline;
    std::reverse(changed.venues.begin(), changed.venues.end());
    std::reverse(changed.routes.begin(), changed.routes.end());
    assert(hash_of(changed) == baseline_hash);

    changed = baseline;
    changed.venues[0].credential_reference = "ANOTHER_SECRET_REFERENCE";
    assert(hash_of(changed) == baseline_hash);
}

void structural_rejection_tests() {
    expect_failure(
        replace_once(
            valid_ini(),
            "account_id=ACCOUNT-ALPHA",
            "account_id=ACCOUNT-ALPHA\naccount_id=SECOND"),
        ConfigurationErrorCode::DuplicateField);
    expect_failure(
        replace_once(
            valid_ini(),
            "risk_generation=7",
            "risk_generation=7\nmystery_field=1"),
        ConfigurationErrorCode::UnknownField);
    expect_failure(
        replace_once(
            valid_ini(),
            "dictionary_identity=FIX44-MOCKX-2026\nsender_comp_id=SORX-A",
            "sender_comp_id=SORX-A"),
        ConfigurationErrorCode::MissingField);
    expect_failure(
        valid_ini() + "\n[venue 1]\n",
        ConfigurationErrorCode::DuplicateSection);
    expect_failure(
        valid_ini() + "\n[network]\nvalue=1\n",
        ConfigurationErrorCode::UnknownSection);
    expect_failure(
        replace_once(valid_ini(), "risk_allowed=true", "risk_allowed=TRUE"),
        ConfigurationErrorCode::InvalidValue);
    expect_failure(
        replace_once(valid_ini(), "ingress_capacity=1024", "ingress_capacity=0"),
        ConfigurationErrorCode::InvalidValue);
}

void semantic_rejection_tests() {
    expect_failure(
        replace_once(valid_ini(), "[route 7 2]", "[route 7 99]"),
        ConfigurationErrorCode::UnmappedReference);
    expect_failure(
        replace_once(
            valid_ini(),
            "price_denominator=1",
            "price_denominator=3"),
        ConfigurationErrorCode::InexactConversion);
    expect_failure(
        replace_once(
            valid_ini(),
            "quantity_denominator=1",
            "quantity_denominator=3"),
        ConfigurationErrorCode::InexactConversion);
    expect_failure(
        replace_once(
            valid_ini(),
            "maximum_parent_quantity=1000",
            "maximum_parent_quantity=18446744073709551616"),
        ConfigurationErrorCode::Overflow);
    expect_failure(
        replace_once(
            valid_ini(),
            "maximum_price=1000",
            "maximum_price=9223372036854775807"),
        ConfigurationErrorCode::Overflow);
    expect_failure(
        replace_once(valid_ini(), "order_entry_session=FIX-B", "order_entry_session=FIX-A"),
        ConfigurationErrorCode::DuplicateIdentity);
    expect_failure(
        replace_once(
            valid_ini(),
            "aggressive_ioc=true\npassive_gtc=true",
            "aggressive_ioc=false\npassive_gtc=false"),
        ConfigurationErrorCode::InvalidValue);
    expect_failure(
        replace_once(valid_ini(), "allocation_cap=500", "allocation_cap=501"),
        ConfigurationErrorCode::InvalidValue);
}

}  // namespace

int main() {
    valid_parse_and_model_tests();
    canonical_hash_tests();
    structural_rejection_tests();
    semantic_rejection_tests();
    return 0;
}
