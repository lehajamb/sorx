#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/order_core.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace sorx;

constexpr InstrumentId kInstrument{7U};

RouteConfig route(
    const std::uint32_t venue,
    const AdminState admin_state = AdminState::Up) {
    return RouteConfig{
        VenueId{venue},
        kInstrument,
        "VENUE_" + std::to_string(venue),
        "ALPHA",
        "ALPHA_" + std::to_string(venue),
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{1'000U},
        venue - 1U,
        venue - 1U,
        static_cast<std::uint64_t>(venue) * 100U,
        admin_state,
    };
}

std::shared_ptr<const CandidateTable> make_table(
    const std::size_t venue_count,
    const AdminState first_venue_state = AdminState::Up) {
    std::vector<RouteConfig> routes;
    routes.reserve(venue_count);
    for (std::size_t index = 0U; index < venue_count; ++index) {
        const auto venue = static_cast<std::uint32_t>(index + 1U);
        routes.push_back(route(
            venue,
            venue == 1U ? first_venue_state : AdminState::Up));
    }

    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(std::move(routes), errors));
    assert(errors.empty());
    FibCompiler compiler;
    return compiler.compile(rib);
}

CoreConfiguration configuration(const std::size_t venue_count) {
    CoreConfiguration value;
    value.maximum_aggressive_venues = venue_count;
    value.risk.generation = 91U;
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
        Quantity{1'000U},
    });
    value.risk.venues.reserve(venue_count);
    for (std::size_t index = 0U; index < venue_count; ++index) {
        value.risk.venues.push_back(VenueRiskLimit{
            VenueId{static_cast<std::uint32_t>(index + 1U)},
            true,
            Quantity{1'000U},
        });
    }
    return value;
}

InstrumentRiskLimit& instrument_limit(CoreConfiguration& value) {
    assert(value.risk.instruments.size() == 1U);
    return value.risk.instruments.front();
}

VenueRiskLimit& venue_limit(
    CoreConfiguration& value,
    const std::size_t index = 0U) {
    assert(index < value.risk.venues.size());
    return value.risk.venues[index];
}

md::BookView book(
    const std::uint32_t venue,
    const std::int64_t ask,
    const std::uint64_t depth) {
    md::BookView value;
    value.venue = VenueId{venue};
    value.channel = ChannelId{venue};
    value.profile = ProfileId{1U};
    value.instrument = kInstrument;
    value.feed_session_epoch = 3U;
    value.recovery_generation = 4U;
    value.view_generation = 100U + venue;
    value.last_applied_sequence = 20U;
    value.next_sequence = 21U;
    value.exchange_timestamp_ns = 100;
    value.local_receive_time_ns = 200U;
    value.local_publication_time_ns = 201U;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    assert(value.push_bid(md::Level{Price{ask - 1}, Quantity{depth}, 1U}));
    assert(value.push_ask(md::Level{Price{ask}, Quantity{depth}, 1U}));
    assert(value.valid());
    return value;
}

CandidateRuntimeView runtime_view(md::BookView value) {
    return CandidateRuntimeView{
        value.venue,
        40U + value.venue.value(),
        std::move(value),
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
    std::uint64_t& ingress_sequence,
    Event event) {
    return core.apply(CoreEvent{
        ++ingress_sequence,
        std::move(event),
    });
}

void publish_views(
    SorCore& core,
    std::uint64_t& ingress_sequence,
    const std::size_t venue_count,
    const std::uint64_t depth = 100U) {
    for (std::size_t index = 0U; index < venue_count; ++index) {
        const auto venue = static_cast<std::uint32_t>(index + 1U);
        const auto ask = static_cast<std::int64_t>(100U + index);
        const auto session = apply_event(
            core,
            ingress_sequence,
            OrderEntrySessionEvent{
                VenueId{venue}, 1U, OrderEntryState::Ready,
            });
        assert(session.command_accepted);
        const auto result = apply_event(
            core,
            ingress_sequence,
            RuntimeViewPublishedEvent{
                kInstrument,
                runtime_view(book(venue, ask, depth)),
            });
        assert(result.command_accepted);
    }
}

ParentOrder order(
    const std::uint64_t id,
    const std::uint64_t quantity,
    const std::int64_t limit_price = 100,
    const RoutingIntent intent = RoutingIntent::AggressiveIOC,
    const InstrumentId instrument = kInstrument) {
    return ParentOrder{
        ParentOrderId{id},
        instrument,
        Side::Buy,
        Quantity{quantity},
        Price{limit_price},
        intent,
    };
}

const ParentSnapshot& parent_snapshot(
    const SorSnapshot& state,
    const ParentOrderId parent) {
    const auto found = std::find_if(
        state.parents.begin(),
        state.parents.end(),
        [parent](const ParentSnapshot& item) {
            return item.order.id == parent;
        });
    assert(found != state.parents.end());
    return *found;
}

const ChildSnapshot& child_snapshot(
    const SorSnapshot& state,
    const ChildOrderId child) {
    const auto found = std::find_if(
        state.children.begin(),
        state.children.end(),
        [child](const ChildSnapshot& item) { return item.id == child; });
    assert(found != state.children.end());
    return *found;
}

Quantity total_reservation(
    const SorSnapshot& state,
    const std::optional<VenueId> venue = std::nullopt) {
    Quantity total;
    for (const auto& child : state.children) {
        if (venue.has_value() && child.venue != *venue) {
            continue;
        }
        const auto next = checked_add(total, child.risk_reservation);
        assert(next.has_value());
        total = *next;
    }
    return total;
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

bool has_exclusion(
    const RouteDecision& decision,
    const VenueId venue,
    const CandidateExclusionReason reason) {
    return std::any_of(
        decision.exclusions.begin(),
        decision.exclusions.end(),
        [venue, reason](const CandidateExclusion& exclusion) {
            return exclusion.venue == venue && exclusion.reason == reason;
        });
}

adapters::NormalizedExecutionReport execution_report(
    const ChildSnapshot& child,
    std::string execution_id,
    const adapters::ExecutionKind kind,
    const adapters::NormalizedOrderStatus status,
    const std::uint64_t last_quantity,
    const std::uint64_t cumulative_quantity,
    const std::uint64_t leaves_quantity) {
    adapters::NormalizedExecutionReport value;
    value.venue = child.venue;
    value.session_epoch = child.session_epoch;
    value.wire_order_id = child.wire_order_id;
    value.execution_id = std::move(execution_id);
    value.durable_report_key = value.execution_id;
    value.kind = kind;
    value.status = status;
    if (last_quantity != 0U) {
        value.last_quantity = Quantity{last_quantity};
        value.last_price = child.limit_price;
    }
    value.cumulative_quantity = Quantity{cumulative_quantity};
    value.leaves_quantity = Quantity{leaves_quantity};
    return value;
}

void expect_hard_rejection(
    CoreConfiguration value,
    const ParentOrder& parent,
    const CommandError expected) {
    SorCore core(std::move(value), make_table(1U));
    std::uint64_t sequence = 0U;
    const RouteTrace trace = core.trace(parent);
    assert(!trace.hard_risk_accepted);
    assert(trace.hard_rejection == expected);

    const auto result = apply_event(
        core,
        sequence,
        ParentSubmitEvent{parent});
    assert(!result.command_accepted);
    assert(result.command_error == expected);
    assert(result.effects.empty());
    assert(core.snapshot().parents.empty());
    assert(core.snapshot().children.empty());
}

void expect_route_rejection(
    SorCore& core,
    std::uint64_t& sequence,
    const ParentOrder& parent,
    const CandidateExclusionReason exclusion_reason,
    const std::size_t existing_children) {
    const RouteTrace trace = core.trace(parent);
    assert(trace.hard_risk_accepted);
    assert(trace.hard_rejection == CommandError::None);
    assert(!trace.decision.accepted());
    assert(trace.decision.allocations.empty());
    assert(trace.decision.remainder == parent.quantity);
    assert(has_exclusion(
        trace.decision,
        VenueId{1U},
        exclusion_reason));

    const auto result = apply_event(
        core,
        sequence,
        ParentSubmitEvent{parent});
    assert(result.command_accepted);
    assert(result.command_error == CommandError::None);
    assert(result.effects.empty());

    const auto state = core.snapshot();
    assert(state.children.size() == existing_children);
    const auto& stored_parent = parent_snapshot(state, parent.id);
    assert(stored_parent.state == ParentState::Rejected);
    assert(stored_parent.explicit_remainder == parent.quantity);
}

void hard_parent_guardrail_matrix_test() {
    {
        auto value = configuration(1U);
        instrument_limit(value).allowed = false;
        expect_hard_rejection(
            std::move(value),
            order(1U, 5U),
            CommandError::InstrumentProhibited);
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_parent_quantity = Quantity{4U};
        expect_hard_rejection(
            std::move(value),
            order(2U, 5U),
            CommandError::ParentQuantityLimit);
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_parent_notional = Notional{499U};
        expect_hard_rejection(
            std::move(value),
            order(3U, 5U),
            CommandError::ParentNotionalLimit);
    }
    {
        auto value = configuration(1U);
        auto& limit = instrument_limit(value);
        limit.maximum_parent_notional =
            Notional{std::numeric_limits<std::uint64_t>::max()};
        limit.maximum_price =
            Price{std::numeric_limits<std::int64_t>::max()};
        expect_hard_rejection(
            std::move(value),
            order(
                4U,
                3U,
                std::numeric_limits<std::int64_t>::max()),
            CommandError::ParentNotionalLimit);
    }
    {
        auto value = configuration(1U);
        auto& limit = instrument_limit(value);
        limit.minimum_price = Price{90};
        limit.maximum_price = Price{110};
        expect_hard_rejection(
            value,
            order(5U, 5U, 89),
            CommandError::PriceCollar);
        expect_hard_rejection(
            std::move(value),
            order(6U, 5U, 111),
            CommandError::PriceCollar);
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_open_parents = 1U;
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        assert(apply_event(
                   core,
                   sequence,
                   ParentSubmitEvent{order(7U, 5U)})
                   .command_accepted);
        const auto rejected = apply_event(
            core,
            sequence,
            ParentSubmitEvent{order(8U, 5U)});
        assert(!rejected.command_accepted);
        assert(rejected.command_error == CommandError::OpenParentLimit);
        assert(core.snapshot().parents.size() == 1U);
        assert(core.snapshot().children.size() == 1U);
    }
    {
        SorCore core(configuration(1U), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        const ParentOrder active = order(9U, 5U);
        assert(apply_event(
                   core,
                   sequence,
                   ParentSubmitEvent{active})
                   .command_accepted);
        const auto kill = apply_event(
            core,
            sequence,
            KillSwitchEvent{true, "operator risk-off"});
        assert(kill.command_accepted);

        const auto new_order = apply_event(
            core,
            sequence,
            ParentSubmitEvent{order(10U, 1U)});
        assert(!new_order.command_accepted);
        assert(new_order.command_error == CommandError::KillSwitchActive);

        const auto replace = apply_event(
            core,
            sequence,
            ParentReplaceEvent{
                ParentReplace{active.id, Quantity{6U}, Price{100}}});
        assert(!replace.command_accepted);
        assert(replace.command_error == CommandError::KillSwitchActive);

        const auto cancel = apply_event(
            core,
            sequence,
            ParentCancelEvent{active.id});
        assert(cancel.command_accepted);
        // The new was still Prepared, so cancellation is local: emitting a
        // venue cancel before the new could reorder it on the wire.
        assert(cancel.effects.empty());
        const auto canceled_state = core.snapshot();
        assert(child_snapshot(
                   canceled_state,
                   canceled_state.children.front().id)
                   .state == ChildState::Canceled);
        assert(total_reservation(canceled_state) == Quantity{});
    }
    {
        SorCore core(configuration(1U), make_table(1U));
        std::uint64_t sequence = 0U;
        assert(apply_event(
                   core,
                   sequence,
                   RoutingGateEvent{false})
                   .command_accepted);
        const auto result = apply_event(
            core,
            sequence,
            ParentSubmitEvent{order(11U, 1U)});
        assert(!result.command_accepted);
        assert(result.command_error == CommandError::RoutingDisabled);
    }
}

void route_level_guardrail_matrix_test() {
    {
        auto value = configuration(1U);
        venue_limit(value).allowed = false;
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        expect_route_rejection(
            core,
            sequence,
            order(20U, 5U),
            CandidateExclusionReason::RiskProhibited,
            0U);
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_child_quantity = Quantity{4U};
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        expect_route_rejection(
            core,
            sequence,
            order(21U, 5U),
            CandidateExclusionReason::RiskProhibited,
            0U);
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_child_notional = Notional{499U};
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        expect_route_rejection(
            core,
            sequence,
            order(22U, 5U),
            CandidateExclusionReason::RiskProhibited,
            0U);
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_open_children = 1U;
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        assert(apply_event(
                   core,
                   sequence,
                   ParentSubmitEvent{order(23U, 5U)})
                   .command_accepted);
        expect_route_rejection(
            core,
            sequence,
            order(24U, 5U),
            CandidateExclusionReason::RiskProhibited,
            1U);
        assert(total_reservation(core.snapshot()) == Quantity{5U});
    }
    {
        auto value = configuration(1U);
        instrument_limit(value).maximum_gross_outstanding = Quantity{10U};
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        assert(apply_event(
                   core,
                   sequence,
                   ParentSubmitEvent{order(25U, 6U)})
                   .command_accepted);
        expect_route_rejection(
            core,
            sequence,
            order(26U, 5U),
            CandidateExclusionReason::RiskProhibited,
            1U);
        assert(total_reservation(core.snapshot()) == Quantity{6U});
    }
    {
        auto value = configuration(1U);
        venue_limit(value).maximum_outstanding = Quantity{10U};
        SorCore core(std::move(value), make_table(1U));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        assert(apply_event(
                   core,
                   sequence,
                   ParentSubmitEvent{order(27U, 6U)})
                   .command_accepted);
        expect_route_rejection(
            core,
            sequence,
            order(28U, 5U),
            CandidateExclusionReason::RiskProhibited,
            1U);
        assert(total_reservation(
                   core.snapshot(),
                   VenueId{1U}) == Quantity{6U});
    }
    {
        SorCore core(
            configuration(1U),
            make_table(1U, AdminState::Down));
        std::uint64_t sequence = 0U;
        publish_views(core, sequence, 1U);
        expect_route_rejection(
            core,
            sequence,
            order(29U, 5U),
            CandidateExclusionReason::AdminDown,
            0U);
    }
}

bool definite_reject_release_and_remainder_test() {
    auto value = configuration(2U);
    instrument_limit(value).maximum_gross_outstanding = Quantity{5U};
    venue_limit(value, 0U).maximum_outstanding = Quantity{5U};
    venue_limit(value, 1U).maximum_outstanding = Quantity{5U};
    SorCore core(std::move(value), make_table(2U));
    std::uint64_t sequence = 0U;
    publish_views(core, sequence, 2U, 10U);

    const ParentOrder parent = order(40U, 5U, 101);
    const auto submit = apply_event(
        core,
        sequence,
        ParentSubmitEvent{parent});
    assert(submit.command_accepted);
    assert(submit.effects.size() == 1U);
    auto state = core.snapshot();
    assert(state.children.size() == 1U);
    const ChildSnapshot child = state.children.front();
    assert(child.venue == VenueId{1U});
    assert(child.risk_reservation == Quantity{5U});

    const auto reject = apply_event(
        core,
        sequence,
        ExecutionReportEvent{execution_report(
            child,
            "REJECT-1",
            adapters::ExecutionKind::Rejected,
            adapters::NormalizedOrderStatus::Rejected,
            0U,
            0U,
            5U)});
    assert(reject.command_accepted);
    assert(reject.effects.empty());
    assert(record_count(
               reject,
               DeterministicRecordKind::RiskReservationReleased) == 1U);

    state = core.snapshot();
    assert(state.children.size() == 1U);
    assert(state.children.front().state == ChildState::Rejected);
    assert(state.children.front().risk_reservation == Quantity{});
    assert(total_reservation(state) == Quantity{});
    const auto& stored_parent = parent_snapshot(state, parent.id);
    assert(stored_parent.state == ParentState::Canceled);
    assert(stored_parent.cumulative_filled == Quantity{});
    const bool explicit_remainder_conserved =
        stored_parent.explicit_remainder == parent.quantity;

    // A definite venue reject is terminal. It must not manufacture a second
    // child or reroute automatically, but the released capacity is reusable by
    // a later independent parent.
    assert(state.children.size() == 1U);
    const auto later = apply_event(
        core,
        sequence,
        ParentSubmitEvent{order(41U, 5U, 101)});
    assert(later.command_accepted);
    assert(later.effects.size() == 1U);
    assert(core.snapshot().children.size() == 2U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});
    return explicit_remainder_conserved;
}

void ambiguous_retention_and_same_wire_retry_test() {
    auto value = configuration(1U);
    instrument_limit(value).maximum_gross_outstanding = Quantity{5U};
    venue_limit(value).maximum_outstanding = Quantity{5U};
    SorCore core(std::move(value), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_views(core, sequence, 1U);

    const auto submitted = apply_event(
        core,
        sequence,
        ParentSubmitEvent{order(50U, 5U)});
    assert(submitted.command_accepted);
    assert(submitted.effects.size() == 1U);
    const ChildSnapshot child = core.snapshot().children.front();
    const auto& first_intent =
        std::get<DispatchNewEffect>(submitted.effects.front()).intent;

    assert(apply_event(
               core,
               sequence,
               DispatchStartedEvent{
                   child.id,
                   adapters::DispatchAttemptKind::New,
                   1U,
                   first_intent.venue,
                   first_intent.session_epoch,
                   first_intent.wire_order_id,
               })
               .command_accepted);
    adapters::LocalDispatchResult first_result;
    first_result.child = child.id;
    first_result.state =
        adapters::LocalDispatchState::DefinitelyUnsent;
    first_result.detail = "transport did not accept bytes";
    first_result.kind = adapters::DispatchAttemptKind::New;
    first_result.attempt_id = 1U;
    first_result.venue = first_intent.venue;
    first_result.session_epoch = first_intent.session_epoch;
    first_result.wire_order_id = first_intent.wire_order_id;
    const auto definitely_unsent = apply_event(
        core,
        sequence,
        LocalDispatchEvent{std::move(first_result)});
    assert(definitely_unsent.command_accepted);
    assert(definitely_unsent.effects.size() == 1U);
    assert(record_count(
               definitely_unsent,
               DeterministicRecordKind::RiskReservationReleased) == 0U);
    const auto& retry = std::get<DispatchNewEffect>(
        definitely_unsent.effects.front());
    assert(retry.intent.child == child.id);
    assert(retry.intent.wire_order_id == child.wire_order_id);
    assert(total_reservation(core.snapshot()) == Quantity{5U});

    assert(apply_event(
               core,
               sequence,
               DispatchStartedEvent{
                   child.id,
                   adapters::DispatchAttemptKind::New,
                   2U,
                   retry.intent.venue,
                   retry.intent.session_epoch,
                   retry.intent.wire_order_id,
               })
               .command_accepted);
    adapters::LocalDispatchResult retry_result;
    retry_result.child = child.id;
    retry_result.state =
        adapters::LocalDispatchState::AcceptedLocally;
    retry_result.kind = adapters::DispatchAttemptKind::New;
    retry_result.attempt_id = 2U;
    retry_result.venue = retry.intent.venue;
    retry_result.session_epoch = retry.intent.session_epoch;
    retry_result.wire_order_id = retry.intent.wire_order_id;
    assert(apply_event(
               core,
               sequence,
               LocalDispatchEvent{std::move(retry_result)})
               .command_accepted);
    const auto timeout = apply_event(
        core,
        sequence,
        DispatchTimeoutEvent{child.id, "no definitive response"});
    assert(timeout.command_accepted);
    assert(record_count(
               timeout,
               DeterministicRecordKind::RiskReservationReleased) == 0U);

    auto state = core.snapshot();
    assert(child_snapshot(state, child.id).state == ChildState::Ambiguous);
    assert(child_snapshot(state, child.id).risk_reservation == Quantity{5U});
    assert(parent_snapshot(state, ParentOrderId{50U}).state ==
           ParentState::Suspended);
    assert(total_reservation(state) == Quantity{5U});

    expect_route_rejection(
        core,
        sequence,
        order(51U, 1U),
        CandidateExclusionReason::RiskProhibited,
        1U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});

    const auto reconciled = apply_event(
        core,
        sequence,
        ReconciliationEvent{
            child.id,
            ReconciliationDisposition::TerminalRejected,
            Quantity{},
        });
    assert(reconciled.command_accepted);
    assert(record_count(
               reconciled,
               DeterministicRecordKind::RiskReservationReleased) == 1U);
    assert(total_reservation(core.snapshot()) == Quantity{});

    const auto later = apply_event(
        core,
        sequence,
        ParentSubmitEvent{order(52U, 5U)});
    assert(later.command_accepted);
    assert(later.effects.size() == 1U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});
}

void fill_terminal_release_and_duplicate_test() {
    auto value = configuration(1U);
    instrument_limit(value).maximum_gross_outstanding = Quantity{5U};
    venue_limit(value).maximum_outstanding = Quantity{5U};
    SorCore core(std::move(value), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_views(core, sequence, 1U);

    assert(apply_event(
               core,
               sequence,
               ParentSubmitEvent{order(60U, 5U)})
               .command_accepted);
    const ChildSnapshot child = core.snapshot().children.front();
    const auto fill = execution_report(
        child,
        "FILL-1",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::Filled,
        5U,
        5U,
        0U);
    const auto filled = apply_event(
        core,
        sequence,
        ExecutionReportEvent{fill});
    assert(filled.command_accepted);
    assert(record_count(
               filled,
               DeterministicRecordKind::RiskReservationReleased) == 1U);

    auto state = core.snapshot();
    assert(child_snapshot(state, child.id).state == ChildState::Filled);
    assert(child_snapshot(state, child.id).filled == Quantity{5U});
    assert(child_snapshot(state, child.id).risk_reservation == Quantity{});
    assert(parent_snapshot(state, ParentOrderId{60U}).state ==
           ParentState::Filled);
    assert(total_reservation(state) == Quantity{});

    const auto duplicate = apply_event(
        core,
        sequence,
        ExecutionReportEvent{fill});
    assert(duplicate.command_accepted);
    assert(record_count(
               duplicate,
               DeterministicRecordKind::RiskReservationReleased) == 0U);
    state = core.snapshot();
    assert(child_snapshot(state, child.id).filled == Quantity{5U});
    assert(total_reservation(state) == Quantity{});

    const auto later = apply_event(
        core,
        sequence,
        ParentSubmitEvent{order(61U, 5U)});
    assert(later.command_accepted);
    assert(later.effects.size() == 1U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});
}

void cancel_terminal_release_test() {
    auto value = configuration(1U);
    instrument_limit(value).maximum_gross_outstanding = Quantity{5U};
    venue_limit(value).maximum_outstanding = Quantity{5U};
    SorCore core(std::move(value), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_views(core, sequence, 1U);

    assert(apply_event(
               core,
               sequence,
               ParentSubmitEvent{order(70U, 5U)})
               .command_accepted);
    const ChildSnapshot child = core.snapshot().children.front();
    const auto canceled = apply_event(
        core,
        sequence,
        ExecutionReportEvent{execution_report(
            child,
            "CANCEL-1",
            adapters::ExecutionKind::Canceled,
            adapters::NormalizedOrderStatus::Canceled,
            0U,
            0U,
            0U)});
    assert(canceled.command_accepted);
    assert(record_count(
               canceled,
               DeterministicRecordKind::RiskReservationReleased) == 1U);
    const auto state = core.snapshot();
    assert(child_snapshot(state, child.id).state == ChildState::Canceled);
    assert(child_snapshot(state, child.id).risk_reservation == Quantity{});
    assert(total_reservation(state) == Quantity{});

    // A distinct acknowledgement may have been delayed behind the terminal
    // report. It is not a duplicate identity, but terminal state and released
    // accounting must still be monotonic.
    const auto late_acknowledgement = apply_event(
        core,
        sequence,
        ExecutionReportEvent{execution_report(
            child,
            "ACK-LATE",
            adapters::ExecutionKind::Acknowledged,
            adapters::NormalizedOrderStatus::Working,
            0U,
            0U,
            5U)});
    assert(late_acknowledgement.command_accepted);
    assert(record_count(
               late_acknowledgement,
               DeterministicRecordKind::RiskReservationReleased) == 0U);
    const auto after_late_acknowledgement = core.snapshot();
    assert(child_snapshot(after_late_acknowledgement, child.id).state ==
           ChildState::Canceled);
    assert(child_snapshot(after_late_acknowledgement, child.id)
               .risk_reservation == Quantity{});
    assert(parent_snapshot(
               after_late_acknowledgement,
               ParentOrderId{70U})
               .state == ParentState::Canceled);
    assert(total_reservation(after_late_acknowledgement) == Quantity{});

    const auto later = apply_event(
        core,
        sequence,
        ParentSubmitEvent{order(71U, 5U)});
    assert(later.command_accepted);
    assert(later.effects.size() == 1U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});
}

void reordered_terminal_and_duplicate_report_test() {
    auto value = configuration(1U);
    instrument_limit(value).maximum_gross_outstanding = Quantity{5U};
    venue_limit(value).maximum_outstanding = Quantity{5U};
    SorCore core(std::move(value), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_views(core, sequence, 1U);

    assert(apply_event(
               core,
               sequence,
               ParentSubmitEvent{order(80U, 5U)})
               .command_accepted);
    const ChildSnapshot child = core.snapshot().children.front();
    const auto terminal_first = execution_report(
        child,
        "CANCEL-FIRST",
        adapters::ExecutionKind::Canceled,
        adapters::NormalizedOrderStatus::Canceled,
        0U,
        2U,
        3U);
    const auto pending = apply_event(
        core,
        sequence,
        ExecutionReportEvent{terminal_first});
    assert(pending.command_accepted);
    assert(record_count(
               pending,
               DeterministicRecordKind::RiskReservationReleased) == 0U);
    auto state = core.snapshot();
    assert(child_snapshot(state, child.id).state == ChildState::Ambiguous);
    assert(child_snapshot(state, child.id).filled == Quantity{});
    assert(child_snapshot(state, child.id).risk_reservation == Quantity{5U});

    const auto duplicate_terminal = apply_event(
        core,
        sequence,
        ExecutionReportEvent{terminal_first});
    assert(duplicate_terminal.command_accepted);
    assert(record_count(
               duplicate_terminal,
               DeterministicRecordKind::RiskReservationReleased) == 0U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});

    const auto late_fill = execution_report(
        child,
        "FILL-LATE",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        2U,
        2U,
        3U);
    const auto resolved = apply_event(
        core,
        sequence,
        ExecutionReportEvent{late_fill});
    assert(resolved.command_accepted);
    assert(record_count(
               resolved,
               DeterministicRecordKind::RiskReservationReleased) == 1U);
    state = core.snapshot();
    assert(child_snapshot(state, child.id).state == ChildState::Canceled);
    assert(child_snapshot(state, child.id).filled == Quantity{2U});
    assert(child_snapshot(state, child.id).risk_reservation == Quantity{});
    assert(parent_snapshot(state, ParentOrderId{80U}).cumulative_filled ==
           Quantity{2U});
    assert(total_reservation(state) == Quantity{});

    const auto duplicate_fill = apply_event(
        core,
        sequence,
        ExecutionReportEvent{late_fill});
    assert(duplicate_fill.command_accepted);
    assert(record_count(
               duplicate_fill,
               DeterministicRecordKind::RiskReservationReleased) == 0U);
    assert(child_snapshot(core.snapshot(), child.id).filled == Quantity{2U});

    const auto later = apply_event(
        core,
        sequence,
        ParentSubmitEvent{order(81U, 5U)});
    assert(later.command_accepted);
    assert(later.effects.size() == 1U);
    assert(total_reservation(core.snapshot()) == Quantity{5U});
}

}  // namespace

int main() {
    hard_parent_guardrail_matrix_test();
    route_level_guardrail_matrix_test();
    const bool definite_remainder_conserved =
        definite_reject_release_and_remainder_test();
    ambiguous_retention_and_same_wire_retry_test();
    fill_terminal_release_and_duplicate_test();
    cancel_terminal_release_test();
    reordered_terminal_and_duplicate_report_test();

    // Keep this assertion last so every independent guardrail and reservation
    // case runs even when the core violates definite-reject conservation.
    assert(definite_remainder_conserved);
    return 0;
}






