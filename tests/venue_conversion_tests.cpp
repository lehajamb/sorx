#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/order_core.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;

constexpr VenueId kVenue{1U};
constexpr InstrumentId kInstrument{7U};
constexpr std::uint64_t kSessionEpoch = 9U;

RouteConfig nonidentity_route() {
    const auto conversion = RationalConversion::create(2U, 3U);
    assert(conversion.has_value());
    return RouteConfig{
        kVenue,
        kInstrument,
        "VENUE_1",
        "ALPHA",
        "ALPHA_1",
        *conversion,
        *conversion,
        VenueCapabilities{true, true},
        Quantity{300U},
        0U,
        0U,
        100U,
        AdminState::Up,
    };
}

std::shared_ptr<const CandidateTable> make_table() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(
        std::vector<RouteConfig>{nonidentity_route()}, errors));
    assert(errors.empty());
    FibCompiler compiler;
    return compiler.compile(rib);
}

CoreConfiguration configuration() {
    CoreConfiguration value;
    value.maximum_aggressive_venues = 1U;
    value.risk.generation = 21U;
    value.risk.instruments.push_back(InstrumentRiskLimit{
        kInstrument,
        true,
        Quantity{1'000U},
        Quantity{1'000U},
        Notional{1'000'000U},
        Notional{1'000'000U},
        Price{1U},
        Price{10'000U},
        100U,
        100U,
        Quantity{10'000U},
    });
    value.risk.venues.push_back(VenueRiskLimit{
        kVenue,
        true,
        Quantity{10'000U},
    });
    return value;
}

md::BookView book() {
    md::BookView value;
    value.venue = kVenue;
    value.channel = ChannelId{1U};
    value.profile = ProfileId{1U};
    value.instrument = kInstrument;
    value.feed_session_epoch = 3U;
    value.recovery_generation = 4U;
    value.view_generation = 100U;
    value.last_applied_sequence = 20U;
    value.next_sequence = 21U;
    value.exchange_timestamp_ns = 100;
    value.local_receive_time_ns = 200U;
    value.local_publication_time_ns = 201U;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    assert(value.push_bid(md::Level{Price{147}, Quantity{30U}, 1U}));
    assert(value.push_ask(md::Level{Price{150}, Quantity{30U}, 1U}));
    assert(value.valid());
    return value;
}

CandidateRuntimeView runtime_view() {
    return CandidateRuntimeView{
        kVenue,
        100U,
        book(),
        MarketDataState::Live,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        true,
        true,
        true,
    };
}

template <typename Event>
ApplyResult apply_event(
    SorCore& core,
    std::uint64_t& sequence,
    Event event) {
    return core.apply(CoreEvent{++sequence, std::move(event)});
}

void publish_ready(SorCore& core, std::uint64_t& sequence) {
    assert(apply_event(
               core,
               sequence,
               OrderEntrySessionEvent{
                   kVenue,
                   kSessionEpoch,
                   OrderEntryState::Ready,
               })
               .command_accepted);
    assert(apply_event(
               core,
               sequence,
               RuntimeViewPublishedEvent{
                   kInstrument,
                   runtime_view(),
               })
               .command_accepted);
}

ParentOrder parent_order(const std::uint64_t id) {
    return ParentOrder{
        ParentOrderId{id},
        kInstrument,
        Side::Buy,
        Quantity{3U},
        Price{150},
        RoutingIntent::AggressiveIOC,
    };
}

struct Submitted final {
    ChildSnapshot child;
    NewOrderIntent intent;
};

Submitted submit_one(
    SorCore& core,
    std::uint64_t& sequence,
    const std::uint64_t parent_id) {
    const auto submitted = apply_event(
        core,
        sequence,
        ParentSubmitEvent{parent_order(parent_id)});
    assert(submitted.command_accepted);
    assert(submitted.effects.size() == 1U);
    const auto* effect =
        std::get_if<DispatchNewEffect>(&submitted.effects.front());
    assert(effect != nullptr);
    const SorSnapshot state = core.snapshot();
    assert(state.children.size() == 1U);
    return Submitted{state.children.front(), effect->intent};
}

void accept_new(
    SorCore& core,
    std::uint64_t& sequence,
    const Submitted& submitted,
    const std::uint64_t attempt_id) {
    const DispatchStartedEvent started{
        submitted.child.id,
        DispatchAttemptKind::New,
        attempt_id,
        submitted.child.venue,
        submitted.child.session_epoch,
        submitted.child.wire_order_id,
    };
    assert(apply_event(core, sequence, started).command_accepted);

    LocalDispatchResult local;
    local.child = submitted.child.id;
    local.state = LocalDispatchState::AcceptedLocally;
    local.kind = DispatchAttemptKind::New;
    local.attempt_id = attempt_id;
    local.venue = submitted.child.venue;
    local.session_epoch = submitted.child.session_epoch;
    local.wire_order_id = submitted.child.wire_order_id;
    assert(apply_event(
               core, sequence, LocalDispatchEvent{std::move(local)})
               .command_accepted);
}

const std::string& field(const FixMessage& message, const int tag) {
    const auto found = std::find_if(
        message.fields.begin(),
        message.fields.end(),
        [tag](const FixField& item) { return item.tag == tag; });
    if (found == message.fields.end()) {
        throw std::runtime_error("missing FIX field");
    }
    return found->value;
}

std::size_t record_count(
    const ApplyResult& result,
    const DeterministicRecordKind kind) {
    return static_cast<std::size_t>(std::count_if(
        result.records.begin(),
        result.records.end(),
        [kind](const DeterministicRecord& record) {
            return record.kind == kind;
        }));
}

NormalizeResult venue_trade(
    const MockXFix44Profile& profile,
    const NewOrderIntent& intent,
    const std::string& execution_id,
    const std::string& cumulative,
    const std::string& leaves,
    const std::string& last_quantity) {
    const std::vector<FixField> fields{
        {11, intent.wire_order_id},
        {17, execution_id},
        {150, "F"},
        {39, cumulative == "2" ? "2" : "1"},
        {14, cumulative},
        {151, leaves},
        {32, last_quantity},
        {31, "1.00"},
    };
    return profile.normalize_execution_report(
        intent.venue, intent.session_epoch, fields);
}

void outbound_restore_and_fill_test() {
    const auto table = make_table();
    SorCore core(configuration(), table);
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);
    const Submitted submitted = submit_one(core, sequence, 1U);

    const auto conversion = RationalConversion::create(2U, 3U);
    assert(conversion.has_value());
    assert(submitted.child.ordered == Quantity{3U});
    assert(submitted.child.limit_price == Price{150});
    assert(submitted.child.venue_ordered == Quantity{2U});
    assert(submitted.child.venue_limit_price == Price{100});
    assert(submitted.child.quantity_conversion == *conversion);
    assert(submitted.child.price_conversion == *conversion);

    assert(submitted.intent.quantity == Quantity{3U});
    assert(submitted.intent.limit_price == Price{150});
    assert(submitted.intent.canonical_quantity == Quantity{3U});
    assert(submitted.intent.canonical_limit_price == Price{150});
    assert(submitted.intent.venue_quantity == Quantity{2U});
    assert(submitted.intent.venue_limit_price == Price{100});

    MockXFix44Profile profile(MockXFix44Config{2U, 0U, 32U, "SX"});
    const auto wire_new = profile.make_new_order(submitted.intent);
    assert(wire_new.ok);
    assert(field(wire_new.message, 38) == "2");
    assert(field(wire_new.message, 44) == "1.00");

    NewOrderIntent missing_venue = submitted.intent;
    missing_venue.venue_quantity = Quantity{};
    assert(!profile.make_new_order(missing_venue).ok);
    NewOrderIntent conflicting_legacy = submitted.intent;
    conflicting_legacy.quantity = Quantity{4U};
    assert(!profile.make_new_order(conflicting_legacy).ok);

    accept_new(core, sequence, submitted, 1U);
    const std::vector<std::byte> before_fill = core.save_state();
    SorCore restored(configuration(), table);
    restored.restore_state(before_fill);
    sequence = restored.snapshot().last_ingress_sequence;
    const ChildSnapshot durable_child = restored.snapshot().children.front();
    assert(durable_child.venue_ordered == Quantity{2U});
    assert(durable_child.venue_limit_price == Price{100});
    assert(durable_child.quantity_conversion == *conversion);
    assert(durable_child.price_conversion == *conversion);

    const auto normalized = venue_trade(
        profile, submitted.intent, "VENUE-FILL-1", "2", "0", "2");
    assert(normalized.ok);
    assert(normalized.report.value_domain == ExecutionValueDomain::Venue);
    const auto filled = apply_event(
        restored,
        sequence,
        ExecutionReportEvent{normalized.report});
    assert(filled.command_accepted);
    assert(record_count(
               filled,
               DeterministicRecordKind::ExecutionCanonicalized) == 1U);
    SorSnapshot state = restored.snapshot();
    assert(state.children.front().filled == Quantity{3U});
    assert(state.children.front().state == ChildState::Filled);
    assert(state.children.front().risk_reservation == Quantity{});
    assert(state.parents.front().cumulative_filled == Quantity{3U});
    assert(state.parents.front().state == ParentState::Filled);

    const std::vector<std::byte> after_fill = restored.save_state();
    SorCore replayed(configuration(), table);
    replayed.restore_state(after_fill);
    state = replayed.snapshot();
    assert(state.children.front().filled == Quantity{3U});
    assert(state.children.front().state == ChildState::Filled);
    assert(state.children.front().venue_ordered == Quantity{2U});
    assert(state.children.front().quantity_conversion == *conversion);
}

void cancel_uses_venue_original_quantity_test() {
    SorCore core(configuration(), make_table());
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);
    const Submitted submitted = submit_one(core, sequence, 2U);
    accept_new(core, sequence, submitted, 1U);

    const auto canceled = apply_event(
        core,
        sequence,
        ParentCancelEvent{ParentOrderId{2U}});
    assert(canceled.command_accepted);
    assert(canceled.effects.size() == 1U);
    const auto* effect =
        std::get_if<DispatchCancelEffect>(&canceled.effects.front());
    assert(effect != nullptr);
    assert(effect->intent.original_quantity == Quantity{3U});
    assert(effect->intent.canonical_original_quantity == Quantity{3U});
    assert(effect->intent.venue_original_quantity == Quantity{2U});

    MockXFix44Profile profile(MockXFix44Config{2U, 0U, 32U, "SX"});
    const auto wire_cancel = profile.make_cancel(effect->intent);
    assert(wire_cancel.ok);
    assert(field(wire_cancel.message, 38) == "2");

    CancelOrderIntent missing_venue = effect->intent;
    missing_venue.venue_original_quantity = Quantity{};
    assert(!profile.make_cancel(missing_venue).ok);
    CancelOrderIntent conflicting_legacy = effect->intent;
    conflicting_legacy.original_quantity = Quantity{4U};
    assert(!profile.make_cancel(conflicting_legacy).ok);
}

void inexact_venue_execution_fails_closed_test() {
    SorCore core(configuration(), make_table());
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);
    const Submitted submitted = submit_one(core, sequence, 3U);
    accept_new(core, sequence, submitted, 1U);

    MockXFix44Profile profile(MockXFix44Config{2U, 0U, 32U, "SX"});
    const auto normalized = venue_trade(
        profile, submitted.intent, "VENUE-INEXACT-1", "1", "1", "1");
    assert(normalized.ok);
    assert(normalized.report.value_domain == ExecutionValueDomain::Venue);
    const auto result = apply_event(
        core,
        sequence,
        ExecutionReportEvent{normalized.report});
    assert(result.command_accepted);
    assert(record_count(
               result,
               DeterministicRecordKind::ExecutionReportObserved) == 1U);
    assert(record_count(
               result,
               DeterministicRecordKind::ExecutionCanonicalized) == 0U);
    const SorSnapshot state = core.snapshot();
    assert(state.children.front().state == ChildState::Ambiguous);
    assert(state.children.front().filled == Quantity{});
    assert(state.children.front().risk_reservation == Quantity{3U});
    assert(state.parents.front().state == ParentState::Suspended);
    assert(state.parents.front().cumulative_filled == Quantity{});
}

}  // namespace

int main() {
    outbound_restore_and_fill_test();
    cancel_uses_venue_original_quantity_test();
    inexact_venue_execution_fails_closed_test();
    return 0;
}


