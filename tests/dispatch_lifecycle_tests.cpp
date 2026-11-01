#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/order_core.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace sorx;

constexpr InstrumentId kInstrument{7U};
constexpr VenueId kVenue{1U};
constexpr std::uint64_t kSessionEpoch = 9U;

RouteConfig route() {
    return RouteConfig{
        kVenue,
        kInstrument,
        "VENUE_1",
        "ALPHA",
        "ALPHA_1",
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{1'000U},
        0U,
        0U,
        100U,
        AdminState::Up,
    };
}

std::shared_ptr<const CandidateTable> make_table() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(std::vector<RouteConfig>{route()}, errors));
    assert(errors.empty());
    FibCompiler compiler;
    return compiler.compile(rib);
}

CoreConfiguration configuration() {
    CoreConfiguration value;
    value.maximum_aggressive_venues = 1U;
    value.risk.generation = 17U;
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
    value.risk.venues.push_back(VenueRiskLimit{
        kVenue,
        true,
        Quantity{1'000U},
    });
    return value;
}

md::BookView book(
    const std::uint64_t view_generation = 100U,
    const std::int64_t bid = 99,
    const std::int64_t ask = 100) {
    md::BookView value;
    value.venue = kVenue;
    value.channel = ChannelId{1U};
    value.profile = ProfileId{1U};
    value.instrument = kInstrument;
    value.feed_session_epoch = 3U;
    value.recovery_generation = 4U;
    value.view_generation = view_generation;
    value.last_applied_sequence = view_generation;
    value.next_sequence = view_generation + 1U;
    value.exchange_timestamp_ns = 100;
    value.local_receive_time_ns = 200U;
    value.local_publication_time_ns = 201U;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    assert(value.push_bid(md::Level{Price{bid}, Quantity{100U}, 1U}));
    assert(value.push_ask(md::Level{Price{ask}, Quantity{100U}, 1U}));
    assert(value.valid());
    return value;
}

CandidateRuntimeView runtime_view(
    md::BookView value,
    const std::uint64_t adjacency_generation,
    const OrderEntryState embedded_order_entry = OrderEntryState::Ready) {
    return CandidateRuntimeView{
        value.venue,
        adjacency_generation,
        std::move(value),
        MarketDataState::Live,
        embedded_order_entry,
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

void publish_ready(
    SorCore& core,
    std::uint64_t& sequence,
    const std::uint64_t epoch = kSessionEpoch) {
    assert(apply_event(
               core,
               sequence,
               OrderEntrySessionEvent{
                   kVenue,
                   epoch,
                   OrderEntryState::Ready,
               })
               .command_accepted);
    assert(apply_event(
               core,
               sequence,
               RuntimeViewPublishedEvent{
                   kInstrument,
                   runtime_view(book(), 100U),
               })
               .command_accepted);
}

ParentOrder order(
    const std::uint64_t id,
    const Side side = Side::Buy) {
    return ParentOrder{
        ParentOrderId{id},
        kInstrument,
        side,
        Quantity{5U},
        Price{side == Side::Buy ? 100 : 99},
        RoutingIntent::AggressiveIOC,
    };
}

const ChildSnapshot& find_child(
    const SorSnapshot& state,
    const ChildOrderId child) {
    const auto found = std::find_if(
        state.children.begin(),
        state.children.end(),
        [child](const ChildSnapshot& item) { return item.id == child; });
    assert(found != state.children.end());
    return *found;
}

const ParentSnapshot& find_parent(
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
    const CandidateExclusionReason reason) {
    return std::any_of(
        decision.exclusions.begin(),
        decision.exclusions.end(),
        [reason](const CandidateExclusion& exclusion) {
            return exclusion.venue == kVenue &&
                   exclusion.reason == reason;
        });
}

struct SubmittedChild final {
    ChildSnapshot child;
    adapters::NewOrderIntent intent;
};

SubmittedChild submit_child(
    SorCore& core,
    std::uint64_t& sequence,
    const ParentOrder& parent) {
    const auto submitted = apply_event(
        core,
        sequence,
        ParentSubmitEvent{parent});
    assert(submitted.command_accepted);
    assert(submitted.effects.size() == 1U);
    const auto& effect = std::get<DispatchNewEffect>(
        submitted.effects.front());
    assert(effect.intent.dispatch_attempt_id == 0U);
    const auto state = core.snapshot();
    assert(state.children.size() == 1U);
    return SubmittedChild{state.children.front(), effect.intent};
}

DispatchStartedEvent started_event(
    const ChildSnapshot& child,
    const adapters::DispatchAttemptKind kind,
    const std::uint64_t attempt_id,
    std::string wire_order_id) {
    return DispatchStartedEvent{
        child.id,
        kind,
        attempt_id,
        child.venue,
        child.session_epoch,
        std::move(wire_order_id),
    };
}

adapters::LocalDispatchResult dispatch_result(
    const DispatchStartedEvent& started,
    const adapters::LocalDispatchState state,
    std::string detail = {}) {
    adapters::LocalDispatchResult result;
    result.child = started.child;
    result.state = state;
    result.detail = std::move(detail);
    result.kind = started.kind;
    result.attempt_id = started.attempt_id;
    result.venue = started.venue;
    result.session_epoch = started.session_epoch;
    result.wire_order_id = started.wire_order_id;
    return result;
}

adapters::NormalizedExecutionReport execution_report(
    const ChildSnapshot& child,
    std::string execution_id,
    const adapters::ExecutionKind kind,
    const adapters::NormalizedOrderStatus status,
    const std::uint64_t cumulative,
    const std::uint64_t leaves) {
    adapters::NormalizedExecutionReport report;
    report.venue = child.venue;
    report.session_epoch = child.session_epoch;
    report.wire_order_id = child.wire_order_id;
    report.execution_id = std::move(execution_id);
    report.durable_report_key = report.execution_id;
    report.kind = kind;
    report.status = status;
    report.cumulative_quantity = Quantity{cumulative};
    report.leaves_quantity = Quantity{leaves};
    return report;
}

void assert_active_attempt(
    const ChildSnapshot& child,
    const DispatchStartedEvent& started) {
    assert(child.active_dispatch_attempt.has_value());
    assert(child.active_dispatch_attempt->kind == started.kind);
    assert(child.active_dispatch_attempt->attempt_id == started.attempt_id);
    assert(child.active_dispatch_attempt->venue == started.venue);
    assert(child.active_dispatch_attempt->session_epoch ==
           started.session_epoch);
    assert(child.active_dispatch_attempt->wire_order_id ==
           started.wire_order_id);
}

void new_dispatch_retry_and_persistence_test() {
    const auto table = make_table();
    SorCore core(configuration(), table);
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);
    const SubmittedChild submitted = submit_child(core, sequence, order(1U));
    assert(submitted.child.state == ChildState::Prepared);
    assert(!submitted.child.active_dispatch_attempt.has_value());
    assert(core.snapshot().last_dispatch_attempt_id == 0U);

    const auto skipped_id = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::New,
        2U,
        submitted.child.wire_order_id);
    const auto skipped = apply_event(core, sequence, skipped_id);
    assert(!skipped.command_accepted);
    assert(skipped.command_error == CommandError::InvalidCommand);
    assert(core.snapshot().last_dispatch_attempt_id == 0U);

    auto wrong_identity = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::New,
        1U,
        "WRONG-WIRE-ID");
    const auto wrong_start = apply_event(core, sequence, wrong_identity);
    assert(!wrong_start.command_accepted);
    assert(core.snapshot().last_dispatch_attempt_id == 0U);

    const auto first = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::New,
        1U,
        submitted.child.wire_order_id);
    const auto started = apply_event(core, sequence, first);
    assert(started.command_accepted);
    assert(record_count(
               started,
               DeterministicRecordKind::DispatchStarted) == 1U);
    auto state = core.snapshot();
    assert(state.last_dispatch_attempt_id == 1U);
    assert(find_child(state, submitted.child.id).state ==
           ChildState::DispatchStarted);
    assert_active_attempt(find_child(state, submitted.child.id), first);

    // DispatchStarted is part of the core state image, not an out-of-band
    // runtime marker.
    const auto saved = core.save_state();
    SorCore recovered(configuration(), table);
    recovered.restore_state(saved);
    state = recovered.snapshot();
    sequence = state.last_ingress_sequence;
    assert(state.last_dispatch_attempt_id == 1U);
    assert_active_attempt(find_child(state, submitted.child.id), first);

    auto mismatch = dispatch_result(
        first,
        adapters::LocalDispatchState::AcceptedLocally);
    mismatch.session_epoch = first.session_epoch + 1U;
    const auto wrong_result = apply_event(
        recovered,
        sequence,
        LocalDispatchEvent{mismatch});
    assert(!wrong_result.command_accepted);
    assert(wrong_result.command_error == CommandError::InvalidCommand);
    assert_active_attempt(
        find_child(recovered.snapshot(), submitted.child.id),
        first);

    const auto definitely_unsent = apply_event(
        recovered,
        sequence,
        LocalDispatchEvent{dispatch_result(
            first,
            adapters::LocalDispatchState::DefinitelyUnsent)});
    assert(definitely_unsent.command_accepted);
    assert(record_count(
               definitely_unsent,
               DeterministicRecordKind::DispatchResult) == 1U);
    assert(definitely_unsent.effects.size() == 1U);
    const auto& retry = std::get<DispatchNewEffect>(
        definitely_unsent.effects.front());
    assert(retry.intent.wire_order_id == submitted.child.wire_order_id);
    assert(retry.intent.dispatch_attempt_id == 0U);
    state = recovered.snapshot();
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Prepared);
    assert(!find_child(state, submitted.child.id)
                .active_dispatch_attempt.has_value());

    const auto stale = apply_event(
        recovered,
        sequence,
        LocalDispatchEvent{dispatch_result(
            first,
            adapters::LocalDispatchState::AcceptedLocally)});
    assert(!stale.command_accepted);
    assert(find_child(recovered.snapshot(), submitted.child.id).state ==
           ChildState::Prepared);

    const auto second = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::New,
        2U,
        submitted.child.wire_order_id);
    assert(apply_event(recovered, sequence, second).command_accepted);
    const auto accepted = apply_event(
        recovered,
        sequence,
        LocalDispatchEvent{dispatch_result(
            second,
            adapters::LocalDispatchState::AcceptedLocally)});
    assert(accepted.command_accepted);
    assert(accepted.effects.empty());
    state = recovered.snapshot();
    assert(state.last_dispatch_attempt_id == 2U);
    assert(find_child(state, submitted.child.id).state ==
           ChildState::PendingAcknowledgement);
    assert(!find_child(state, submitted.child.id)
                .active_dispatch_attempt.has_value());

    const auto duplicate = apply_event(
        recovered,
        sequence,
        LocalDispatchEvent{dispatch_result(
            second,
            adapters::LocalDispatchState::AcceptedLocally)});
    assert(!duplicate.command_accepted);
    assert(find_child(recovered.snapshot(), submitted.child.id).state ==
           ChildState::PendingAcknowledgement);
}

void cancel_dispatch_retry_and_terminal_test() {
    SorCore core(configuration(), make_table());
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);
    const SubmittedChild submitted = submit_child(core, sequence, order(10U));
    const auto new_started = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::New,
        1U,
        submitted.child.wire_order_id);
    assert(apply_event(core, sequence, new_started).command_accepted);
    assert(apply_event(
               core,
               sequence,
               LocalDispatchEvent{dispatch_result(
                   new_started,
                   adapters::LocalDispatchState::AcceptedLocally)})
               .command_accepted);

    const auto cancel_command = apply_event(
        core,
        sequence,
        ParentCancelEvent{ParentOrderId{10U}});
    assert(cancel_command.command_accepted);
    assert(cancel_command.effects.size() == 1U);
    const auto cancel_intent = std::get<DispatchCancelEffect>(
        cancel_command.effects.front()).intent;
    assert(cancel_intent.dispatch_attempt_id == 0U);
    assert(!cancel_intent.cancel_wire_order_id.empty());

    const auto cancel_first = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::Cancel,
        2U,
        cancel_intent.cancel_wire_order_id);
    const auto cancel_started = apply_event(core, sequence, cancel_first);
    assert(cancel_started.command_accepted);
    assert(find_child(core.snapshot(), submitted.child.id).state ==
           ChildState::CancelPending);
    assert_active_attempt(
        find_child(core.snapshot(), submitted.child.id),
        cancel_first);

    auto wrong_wire = dispatch_result(
        cancel_first,
        adapters::LocalDispatchState::DefinitelyUnsent);
    wrong_wire.wire_order_id = submitted.child.wire_order_id;
    assert(!apply_event(
                core,
                sequence,
                LocalDispatchEvent{wrong_wire})
                .command_accepted);

    const auto cancel_unsent = apply_event(
        core,
        sequence,
        LocalDispatchEvent{dispatch_result(
            cancel_first,
            adapters::LocalDispatchState::DefinitelyUnsent)});
    assert(cancel_unsent.command_accepted);
    assert(cancel_unsent.effects.size() == 1U);
    const auto& cancel_retry = std::get<DispatchCancelEffect>(
        cancel_unsent.effects.front());
    assert(cancel_retry.intent.cancel_wire_order_id ==
           cancel_intent.cancel_wire_order_id);
    assert(cancel_retry.intent.dispatch_attempt_id == 0U);
    assert(find_child(core.snapshot(), submitted.child.id).state ==
           ChildState::CancelPending);

    assert(!apply_event(
                core,
                sequence,
                LocalDispatchEvent{dispatch_result(
                    cancel_first,
                    adapters::LocalDispatchState::AcceptedLocally)})
                .command_accepted);

    const auto cancel_second = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::Cancel,
        3U,
        cancel_intent.cancel_wire_order_id);
    assert(apply_event(core, sequence, cancel_second).command_accepted);
    const auto cancel_accepted = apply_event(
        core,
        sequence,
        LocalDispatchEvent{dispatch_result(
            cancel_second,
            adapters::LocalDispatchState::AcceptedLocally)});
    assert(cancel_accepted.command_accepted);
    assert(cancel_accepted.effects.empty());
    assert(find_child(core.snapshot(), submitted.child.id).state ==
           ChildState::CancelPending);

    const auto still_working = apply_event(
        core,
        sequence,
        ReconciliationEvent{
            submitted.child.id,
            ReconciliationDisposition::Working,
            Quantity{},
        });
    assert(still_working.command_accepted);
    assert(still_working.effects.size() == 1U);
    const auto& recovered_cancel = std::get<DispatchCancelEffect>(
        still_working.effects.front());
    assert(recovered_cancel.intent.cancel_wire_order_id ==
           cancel_intent.cancel_wire_order_id);
    assert(recovered_cancel.intent.original_wire_order_id ==
           cancel_intent.original_wire_order_id);
    assert(recovered_cancel.intent.dispatch_attempt_id == 0U);
    assert(find_child(core.snapshot(), submitted.child.id).state ==
           ChildState::CancelPending);

    const auto canceled = execution_report(
        submitted.child,
        "CANCEL-TERMINAL",
        adapters::ExecutionKind::Canceled,
        adapters::NormalizedOrderStatus::Canceled,
        0U,
        0U);
    assert(apply_event(
               core,
               sequence,
               ExecutionReportEvent{canceled})
               .command_accepted);
    auto state = core.snapshot();
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Canceled);
    assert(find_child(state, submitted.child.id).risk_reservation ==
           Quantity{});
    assert(find_parent(state, ParentOrderId{10U}).state ==
           ParentState::Canceled);

    // Neither stale local results, stale starts, nor reconciliation may
    // resurrect terminal state or accounting.
    assert(!apply_event(
                core,
                sequence,
                LocalDispatchEvent{dispatch_result(
                    cancel_second,
                    adapters::LocalDispatchState::DefinitelyUnsent)})
                .command_accepted);
    const auto stale_start = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::Cancel,
        4U,
        cancel_intent.cancel_wire_order_id);
    assert(!apply_event(core, sequence, stale_start).command_accepted);
    assert(!apply_event(
                core,
                sequence,
                ReconciliationEvent{
                    submitted.child.id,
                    ReconciliationDisposition::Working,
                    Quantity{},
                })
                .command_accepted);
    state = core.snapshot();
    assert(state.last_dispatch_attempt_id == 3U);
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Canceled);
    assert(find_child(state, submitted.child.id).risk_reservation ==
           Quantity{});
}

void unknown_and_typed_reconciliation_test() {
    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        publish_ready(core, sequence);
        const SubmittedChild submitted =
            submit_child(core, sequence, order(19U));
        const auto reconciled = apply_event(
            core,
            sequence,
            ReconciliationEvent{
                submitted.child.id,
                ReconciliationDisposition::DefinitelyUnsent,
                Quantity{},
            });
        assert(reconciled.command_accepted);
        assert(reconciled.effects.size() == 1U);
        const auto& retry = std::get<DispatchNewEffect>(
            reconciled.effects.front());
        assert(retry.intent.wire_order_id ==
               submitted.child.wire_order_id);
        const auto state = core.snapshot();
        assert(state.last_dispatch_attempt_id == 0U);
        assert(find_child(state, submitted.child.id).state ==
               ChildState::Prepared);
    }

    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        publish_ready(core, sequence);
        const SubmittedChild submitted =
            submit_child(core, sequence, order(20U));
        const auto started = started_event(
            submitted.child,
            adapters::DispatchAttemptKind::New,
            1U,
            submitted.child.wire_order_id);
        assert(apply_event(core, sequence, started).command_accepted);
        const auto unknown = apply_event(
            core,
            sequence,
            LocalDispatchEvent{dispatch_result(
                started,
                adapters::LocalDispatchState::Unknown,
                "socket outcome unknown")});
        assert(unknown.command_accepted);
        auto state = core.snapshot();
        assert(find_child(state, submitted.child.id).state ==
               ChildState::Ambiguous);
        assert(find_child(state, submitted.child.id).risk_reservation ==
               Quantity{5U});
        assert(find_parent(state, ParentOrderId{20U}).state ==
               ParentState::Suspended);
        assert(!find_child(state, submitted.child.id)
                    .active_dispatch_attempt.has_value());

        assert(!apply_event(
                    core,
                    sequence,
                    LocalDispatchEvent{dispatch_result(
                        started,
                        adapters::LocalDispatchState::AcceptedLocally)})
                    .command_accepted);
        const auto reconciled = apply_event(
            core,
            sequence,
            ReconciliationEvent{
                submitted.child.id,
                ReconciliationDisposition::DefinitelyUnsent,
                Quantity{},
            });
        assert(reconciled.command_accepted);
        assert(reconciled.effects.size() == 1U);
        const auto& retry = std::get<DispatchNewEffect>(
            reconciled.effects.front());
        assert(retry.intent.wire_order_id ==
               submitted.child.wire_order_id);
        assert(find_child(core.snapshot(), submitted.child.id).state ==
               ChildState::Prepared);

        assert(!apply_event(
                    core,
                    sequence,
                    ReconciliationEvent{
                        submitted.child.id,
                        ReconciliationDisposition::Working,
                        Quantity{},
                    })
                    .command_accepted);
    }

    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        publish_ready(core, sequence);
        const SubmittedChild submitted =
            submit_child(core, sequence, order(21U));
        const auto new_started = started_event(
            submitted.child,
            adapters::DispatchAttemptKind::New,
            1U,
            submitted.child.wire_order_id);
        assert(apply_event(core, sequence, new_started).command_accepted);
        assert(apply_event(
                   core,
                   sequence,
                   LocalDispatchEvent{dispatch_result(
                       new_started,
                       adapters::LocalDispatchState::AcceptedLocally)})
                   .command_accepted);
        const auto cancel = apply_event(
            core,
            sequence,
            ParentCancelEvent{ParentOrderId{21U}});
        const auto cancel_intent = std::get<DispatchCancelEffect>(
            cancel.effects.front()).intent;
        const auto cancel_started = started_event(
            submitted.child,
            adapters::DispatchAttemptKind::Cancel,
            2U,
            cancel_intent.cancel_wire_order_id);
        assert(apply_event(core, sequence, cancel_started).command_accepted);
        assert(apply_event(
                   core,
                   sequence,
                   LocalDispatchEvent{dispatch_result(
                       cancel_started,
                       adapters::LocalDispatchState::Unknown)})
                   .command_accepted);
        assert(find_child(core.snapshot(), submitted.child.id).state ==
               ChildState::Ambiguous);

        const auto reconciled = apply_event(
            core,
            sequence,
            ReconciliationEvent{
                submitted.child.id,
                ReconciliationDisposition::DefinitelyUnsent,
                Quantity{},
            });
        assert(reconciled.command_accepted);
        assert(reconciled.effects.size() == 1U);
        const auto& retry = std::get<DispatchCancelEffect>(
            reconciled.effects.front());
        assert(retry.intent.cancel_wire_order_id ==
               cancel_intent.cancel_wire_order_id);
        assert(find_child(core.snapshot(), submitted.child.id).state ==
               ChildState::CancelPending);
    }
}

void cancel_before_and_during_new_dispatch_test() {
    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        publish_ready(core, sequence);
        const SubmittedChild submitted =
            submit_child(core, sequence, order(30U));
        const auto canceled = apply_event(
            core,
            sequence,
            ParentCancelEvent{ParentOrderId{30U}});
        assert(canceled.command_accepted);
        assert(canceled.effects.empty());
        const auto state = core.snapshot();
        assert(find_child(state, submitted.child.id).state ==
               ChildState::Canceled);
        assert(find_child(state, submitted.child.id).risk_reservation ==
               Quantity{});

        // The already-queued new effect is now stale and cannot start.
        const auto stale_new = started_event(
            submitted.child,
            adapters::DispatchAttemptKind::New,
            1U,
            submitted.child.wire_order_id);
        assert(!apply_event(core, sequence, stale_new).command_accepted);
        assert(core.snapshot().last_dispatch_attempt_id == 0U);
    }

    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        publish_ready(core, sequence);
        const SubmittedChild submitted =
            submit_child(core, sequence, order(31U));
        const auto started = started_event(
            submitted.child,
            adapters::DispatchAttemptKind::New,
            1U,
            submitted.child.wire_order_id);
        assert(apply_event(core, sequence, started).command_accepted);
        const auto cancel = apply_event(
            core,
            sequence,
            ParentCancelEvent{ParentOrderId{31U}});
        assert(cancel.command_accepted);
        assert(cancel.effects.empty());
        assert(find_child(core.snapshot(), submitted.child.id).state ==
               ChildState::CancelPending);
        assert_active_attempt(
            find_child(core.snapshot(), submitted.child.id),
            started);

        const auto new_accepted = apply_event(
            core,
            sequence,
            LocalDispatchEvent{dispatch_result(
                started,
                adapters::LocalDispatchState::AcceptedLocally)});
        assert(new_accepted.command_accepted);
        assert(new_accepted.effects.size() == 1U);
        assert(std::holds_alternative<DispatchCancelEffect>(
            new_accepted.effects.front()));
        assert(find_child(core.snapshot(), submitted.child.id).state ==
               ChildState::CancelPending);
    }

    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        publish_ready(core, sequence);
        const SubmittedChild submitted =
            submit_child(core, sequence, order(32U));
        const auto started = started_event(
            submitted.child,
            adapters::DispatchAttemptKind::New,
            1U,
            submitted.child.wire_order_id);
        assert(apply_event(core, sequence, started).command_accepted);
        assert(apply_event(
                   core,
                   sequence,
                   ParentCancelEvent{ParentOrderId{32U}})
                   .command_accepted);
        const auto definitely_unsent = apply_event(
            core,
            sequence,
            LocalDispatchEvent{dispatch_result(
                started,
                adapters::LocalDispatchState::DefinitelyUnsent)});
        assert(definitely_unsent.command_accepted);
        assert(definitely_unsent.effects.empty());
        const auto state = core.snapshot();
        assert(find_child(state, submitted.child.id).state ==
               ChildState::Canceled);
        assert(find_child(state, submitted.child.id).risk_reservation ==
               Quantity{});
    }
}

void session_and_runtime_version_monotonicity_test() {
    SorCore core(configuration(), make_table());
    std::uint64_t sequence = 0U;

    // Without authoritative session state, an embedded Ready value is ignored.
    assert(apply_event(
               core,
               sequence,
               RuntimeViewPublishedEvent{
                   kInstrument,
                   runtime_view(book(10U), 10U),
               })
               .command_accepted);
    auto trace = core.trace(order(40U));
    assert(trace.hard_risk_accepted);
    assert(!trace.decision.accepted());
    assert(has_exclusion(
        trace.decision,
        CandidateExclusionReason::OrderEntryNotReady));

    assert(apply_event(
               core,
               sequence,
               OrderEntrySessionEvent{
                   kVenue,
                   2U,
                   OrderEntryState::Ready,
               })
               .command_accepted);
    assert(apply_event(
               core,
               sequence,
               RuntimeViewPublishedEvent{
                   kInstrument,
                   runtime_view(book(12U), 12U),
               })
               .command_accepted);
    trace = core.trace(order(41U));
    assert(trace.decision.accepted());

    assert(apply_event(
               core,
               sequence,
               OrderEntrySessionEvent{
                   kVenue,
                   2U,
                   OrderEntryState::Disconnected,
               })
               .command_accepted);

    // The session event advanced the stored adjacency to 13. The old market
    // snapshot cannot overwrite that boundary.
    const auto stale_view = apply_event(
        core,
        sequence,
        RuntimeViewPublishedEvent{
            kInstrument,
            runtime_view(book(12U), 12U),
        });
    assert(!stale_view.command_accepted);
    assert(stale_view.command_error == CommandError::InvalidCommand);

    // Even genuinely newer market data cannot restore its embedded Ready
    // value after the authoritative session disconnected.
    assert(apply_event(
               core,
               sequence,
               RuntimeViewPublishedEvent{
                   kInstrument,
                   runtime_view(book(14U), 14U),
               })
               .command_accepted);
    trace = core.trace(order(42U));
    assert(!trace.decision.accepted());
    assert(has_exclusion(
        trace.decision,
        CandidateExclusionReason::OrderEntryNotReady));

    const auto regressed_epoch = apply_event(
        core,
        sequence,
        OrderEntrySessionEvent{
            kVenue,
            1U,
            OrderEntryState::Ready,
        });
    assert(!regressed_epoch.command_accepted);
    assert(regressed_epoch.command_error == CommandError::InvalidCommand);

    const auto invalid_state = apply_event(
        core,
        sequence,
        OrderEntrySessionEvent{
            kVenue,
            3U,
            static_cast<OrderEntryState>(255U),
        });
    assert(!invalid_state.command_accepted);

    assert(apply_event(
               core,
               sequence,
               OrderEntrySessionEvent{
                   kVenue,
                   3U,
                   OrderEntryState::Ready,
               })
               .command_accepted);
    trace = core.trace(order(43U));
    assert(trace.decision.accepted());

    // A higher adjacency cannot smuggle in a lower owned book generation.
    const auto lower_book = apply_event(
        core,
        sequence,
        RuntimeViewPublishedEvent{
            kInstrument,
            runtime_view(book(13U), 16U),
        });
    assert(!lower_book.command_accepted);
    assert(lower_book.command_error == CommandError::InvalidCommand);
    assert(core.trace(order(44U)).decision.accepted());
}

void fail_closed_public_enum_test() {
    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        ParentOrder invalid = order(50U);
        invalid.side = static_cast<Side>(255U);
        const auto result = apply_event(
            core,
            sequence,
            ParentSubmitEvent{invalid});
        assert(!result.command_accepted);
        assert(result.command_error == CommandError::InvalidCommand);
        assert(core.snapshot().parents.empty());
    }
    {
        SorCore core(configuration(), make_table());
        std::uint64_t sequence = 0U;
        ParentOrder invalid = order(51U);
        invalid.intent = static_cast<RoutingIntent>(255U);
        const auto result = apply_event(
            core,
            sequence,
            ParentSubmitEvent{invalid});
        assert(!result.command_accepted);
        assert(result.command_error == CommandError::InvalidCommand);
        assert(core.snapshot().parents.empty());
    }
}

void expect_invalid_trade(
    const Side side,
    const Price last_price,
    const Quantity cumulative,
    const Quantity leaves) {
    SorCore core(configuration(), make_table());
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);
    const SubmittedChild submitted =
        submit_child(core, sequence, order(60U, side));
    const auto started = started_event(
        submitted.child,
        adapters::DispatchAttemptKind::New,
        1U,
        submitted.child.wire_order_id);
    assert(apply_event(core, sequence, started).command_accepted);
    assert(apply_event(
               core,
               sequence,
               LocalDispatchEvent{dispatch_result(
                   started,
                   adapters::LocalDispatchState::AcceptedLocally)})
               .command_accepted);

    auto report = execution_report(
        submitted.child,
        "INVALID-TRADE",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        cumulative.units,
        leaves.units);
    report.last_quantity = Quantity{1U};
    report.last_price = last_price;
    const auto result = apply_event(
        core,
        sequence,
        ExecutionReportEvent{report});
    assert(result.command_accepted);
    const auto state = core.snapshot();
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Ambiguous);
    assert(find_child(state, submitted.child.id).filled == Quantity{});
    assert(find_child(state, submitted.child.id).risk_reservation ==
           Quantity{5U});
    assert(find_parent(state, ParentOrderId{60U}).cumulative_filled ==
           Quantity{});
    assert(find_parent(state, ParentOrderId{60U}).state ==
           ParentState::Suspended);
}

void reordered_trade_persistence_test() {
    const auto table = make_table();
    SorCore core(configuration(), table);
    std::uint64_t sequence = 0U;
    publish_ready(core, sequence);

    ParentOrder parent_order = order(70U);
    parent_order.quantity = Quantity{15U};
    const SubmittedChild submitted =
        submit_child(core, sequence, parent_order);

    const auto make_trade =
        [&submitted](
            std::string execution_id,
            const adapters::NormalizedOrderStatus status,
            const std::uint64_t cumulative,
            const std::uint64_t leaves) {
            auto report = execution_report(
                submitted.child,
                std::move(execution_id),
                adapters::ExecutionKind::Trade,
                status,
                cumulative,
                leaves);
            report.last_quantity = Quantity{5U};
            report.last_price = Price{100};
            return report;
        };

    const auto third = make_trade(
        "REORDER-3",
        adapters::NormalizedOrderStatus::Filled,
        15U,
        0U);
    const auto second = make_trade(
        "REORDER-2",
        adapters::NormalizedOrderStatus::PartiallyFilled,
        10U,
        5U);
    const auto first = make_trade(
        "REORDER-1",
        adapters::NormalizedOrderStatus::PartiallyFilled,
        5U,
        10U);

    assert(apply_event(
               core,
               sequence,
               ExecutionReportEvent{third})
               .command_accepted);
    assert(apply_event(
               core,
               sequence,
               ExecutionReportEvent{second})
               .command_accepted);
    auto state = core.snapshot();
    assert(find_child(state, submitted.child.id).filled == Quantity{});
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Ambiguous);
    assert(find_child(state, submitted.child.id).risk_reservation ==
           Quantity{15U});
    assert(find_parent(state, parent_order.id).cumulative_filled ==
           Quantity{});

    const auto pending_image = core.save_state();
    SorCore restored(configuration(), table);
    restored.restore_state(pending_image);
    assert(!restored.recovery_consistent());
    state = restored.snapshot();
    assert(find_child(state, submitted.child.id).filled == Quantity{});
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Ambiguous);

    // Dedupe state and the buffered accounting payload both survive restore.
    assert(apply_event(
               restored,
               sequence,
               ExecutionReportEvent{third})
               .command_accepted);
    assert(apply_event(
               restored,
               sequence,
               ExecutionReportEvent{second})
               .command_accepted);
    assert(find_child(restored.snapshot(), submitted.child.id).filled ==
           Quantity{});

    // The exact predecessor unlocks the entire deterministic cumulative chain.
    assert(apply_event(
               restored,
               sequence,
               ExecutionReportEvent{first})
               .command_accepted);
    state = restored.snapshot();
    assert(find_child(state, submitted.child.id).filled == Quantity{15U});
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Filled);
    assert(find_child(state, submitted.child.id).risk_reservation ==
           Quantity{});
    assert(find_parent(state, parent_order.id).cumulative_filled ==
           Quantity{15U});
    assert(find_parent(state, parent_order.id).state ==
           ParentState::Filled);
    assert(find_parent(state, parent_order.id).explicit_remainder ==
           Quantity{});

    for (const auto& duplicate : {first, second, third}) {
        assert(apply_event(
                   restored,
                   sequence,
                   ExecutionReportEvent{duplicate})
                   .command_accepted);
    }
    state = restored.snapshot();
    assert(find_child(state, submitted.child.id).filled == Quantity{15U});
    assert(find_parent(state, parent_order.id).cumulative_filled ==
           Quantity{15U});

    const auto terminal_image = restored.save_state();
    SorCore terminal_restored(configuration(), table);
    terminal_restored.restore_state(terminal_image);
    state = terminal_restored.snapshot();
    assert(find_child(state, submitted.child.id).filled == Quantity{15U});
    assert(find_child(state, submitted.child.id).state ==
           ChildState::Filled);
    assert(find_parent(state, parent_order.id).cumulative_filled ==
           Quantity{15U});
    assert(find_parent(state, parent_order.id).state ==
           ParentState::Filled);
}

void trade_protection_and_conservation_test() {
    expect_invalid_trade(
        Side::Buy,
        Price{101},
        Quantity{1U},
        Quantity{4U});
    expect_invalid_trade(
        Side::Sell,
        Price{98},
        Quantity{1U},
        Quantity{4U});
    expect_invalid_trade(
        Side::Buy,
        Price{100},
        Quantity{2U},
        Quantity{3U});
    expect_invalid_trade(
        Side::Buy,
        Price{100},
        Quantity{1U},
        Quantity{3U});
}

}  // namespace

int main() {
    new_dispatch_retry_and_persistence_test();
    cancel_dispatch_retry_and_terminal_test();
    unknown_and_typed_reconciliation_test();
    cancel_before_and_during_new_dispatch_test();
    session_and_runtime_version_monotonicity_test();
    fail_closed_public_enum_test();
    reordered_trade_persistence_test();
    trade_protection_and_conservation_test();
    return 0;
}






