#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/order_core.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sorx;

RouteConfig route(const std::uint32_t venue, const std::uint32_t priority) {
    return RouteConfig{
        VenueId{venue},
        InstrumentId{7U},
        "VENUE_" + std::to_string(venue),
        "ALPHA",
        "ALPHA_" + std::to_string(venue),
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{100U},
        priority,
        priority,
        static_cast<std::uint64_t>(priority) * 100U,
        AdminState::Up,
    };
}

std::shared_ptr<const CandidateTable> make_table(const std::size_t venue_count) {
    std::vector<RouteConfig> routes;
    for (std::size_t index = 0U; index < venue_count; ++index) {
        routes.push_back(
            route(
                static_cast<std::uint32_t>(index + 1U),
                static_cast<std::uint32_t>(index)));
    }
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(std::move(routes), errors));
    FibCompiler compiler;
    return compiler.compile(rib);
}

CoreConfiguration configuration(const std::size_t maximum_venues) {
    CoreConfiguration config;
    config.maximum_aggressive_venues = maximum_venues;
    config.risk.generation = 11U;
    config.risk.instruments.push_back(InstrumentRiskLimit{
        InstrumentId{7U},
        true,
        Quantity{1'000U},
        Quantity{500U},
        Notional{1'000'000U},
        Notional{500'000U},
        Price{1U},
        Price{10'000U},
        100U,
        100U,
        Quantity{10'000U},
    });
    for (std::size_t index = 0U; index < maximum_venues; ++index) {
        config.risk.venues.push_back(VenueRiskLimit{
            VenueId{static_cast<std::uint32_t>(index + 1U)},
            true,
            Quantity{10'000U},
        });
    }
    return config;
}

md::BookView book(
    const std::uint32_t venue,
    const std::int64_t bid,
    const std::int64_t ask,
    const std::uint64_t depth) {
    md::BookView view;
    view.venue = VenueId{venue};
    view.channel = ChannelId{venue};
    view.profile = ProfileId{1U};
    view.instrument = InstrumentId{7U};
    view.feed_session_epoch = 3U;
    view.recovery_generation = 4U;
    view.view_generation = 100U + venue;
    view.last_applied_sequence = 20U;
    view.next_sequence = 21U;
    view.exchange_timestamp_ns = 100U;
    view.local_receive_time_ns = 200U;
    view.local_publication_time_ns = 201U;
    view.feed_state = MarketDataState::Live;
    view.trading_state = InstrumentState::Trading;
    assert(view.push_bid(md::Level{Price{bid}, Quantity{depth}, 1U}));
    assert(view.push_ask(md::Level{Price{ask}, Quantity{depth}, 1U}));
    assert(view.valid());
    return view;
}

CandidateRuntimeView runtime_view(md::BookView view) {
    return CandidateRuntimeView{
        view.venue,
        40U + view.venue.value(),
        std::move(view),
        MarketDataState::Live,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        true,
        true,
        true,
    };
}

void publish_view(
    SorCore& core,
    std::uint64_t& sequence,
    CandidateRuntimeView view,
    const bool publish_session = true) {
    if (publish_session) {
        const VenueId venue = view.venue;
        const auto session = core.apply(CoreEvent{
            ++sequence,
            OrderEntrySessionEvent{
                venue, 1U, OrderEntryState::Ready},
        });
        assert(session.command_accepted);
    }
    const auto result = core.apply(CoreEvent{
        ++sequence,
        RuntimeViewPublishedEvent{InstrumentId{7U}, std::move(view)},
    });
    assert(result.command_accepted);
}

adapters::NormalizedExecutionReport report(
    const ChildSnapshot& child,
    const std::string& execution_id,
    const adapters::ExecutionKind kind,
    const adapters::NormalizedOrderStatus status,
    const std::uint64_t last_quantity,
    const std::uint64_t cumulative_quantity,
    const std::uint64_t leaves_quantity,
    const std::int64_t last_price = 100) {
    adapters::NormalizedExecutionReport value;
    value.venue = child.venue;
    value.session_epoch = child.session_epoch;
    value.wire_order_id = child.wire_order_id;
    value.execution_id = execution_id;
    value.durable_report_key = execution_id;
    value.kind = kind;
    value.status = status;
    if (last_quantity != 0U) {
        value.last_quantity = Quantity{last_quantity};
        value.last_price = Price{last_price};
    }
    value.cumulative_quantity = Quantity{cumulative_quantity};
    value.leaves_quantity = Quantity{leaves_quantity};
    return value;
}

void aggressive_batch_and_ambiguity_test() {
    SorCore core(configuration(2U), make_table(2U));
    std::uint64_t sequence = 0U;
    publish_view(core, sequence, runtime_view(book(1U, 99, 100, 4U)));
    publish_view(core, sequence, runtime_view(book(2U, 98, 101, 8U)));

    const ParentOrder order{
        ParentOrderId{10U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{10U},
        Price{101U},
        RoutingIntent::AggressiveIOC,
    };
    const RouteTrace pinned = core.trace(order);
    assert(pinned.hard_risk_accepted);
    assert(pinned.decision.risk_generation == 11U);
    assert(pinned.decision.candidate_pins.size() == 2U);
    assert(pinned.decision.candidate_pins[0].adjacency_generation == 41U);
    assert(pinned.decision.candidate_pins[0].book_version.has_value());
    assert(pinned.decision.candidate_pins[1].adjacency_generation == 42U);
    assert(pinned.decision.candidate_pins[1].book_version.has_value());

    const auto submit = core.apply(
        CoreEvent{++sequence, ParentSubmitEvent{order}});
    assert(submit.command_accepted);
    assert(submit.effects.size() == 2U);
    assert(submit.records.front().kind ==
           DeterministicRecordKind::ParentSubmit);
    assert(submit.records[1].kind ==
           DeterministicRecordKind::RouteDecision);
    assert(submit.records.back().kind ==
           DeterministicRecordKind::ChildPrepared);

    auto state = core.snapshot();
    assert(state.parents.size() == 1U);
    assert(state.children.size() == 2U);
    assert(state.parents.front().explicit_remainder == Quantity{});
    assert(state.children[0].risk_reservation == state.children[0].ordered);
    assert(state.children[1].risk_reservation == state.children[1].ordered);

    const auto duplicate = core.apply(
        CoreEvent{++sequence, ParentSubmitEvent{order}});
    assert(duplicate.command_accepted);
    assert(duplicate.idempotent_replay);
    assert(duplicate.effects.empty());
    assert(core.snapshot().children.size() == 2U);

    const ChildSnapshot first = core.snapshot().children.front();
    const auto& first_intent =
        std::get<DispatchNewEffect>(submit.effects.front()).intent;
    const auto started = core.apply(CoreEvent{
        ++sequence,
        DispatchStartedEvent{
            first.id,
            adapters::DispatchAttemptKind::New,
            1U,
            first_intent.venue,
            first_intent.session_epoch,
            first_intent.wire_order_id,
        },
    });
    assert(started.command_accepted);

    adapters::LocalDispatchResult dispatch_result;
    dispatch_result.child = first.id;
    dispatch_result.state =
        adapters::LocalDispatchState::AcceptedLocally;
    dispatch_result.kind = adapters::DispatchAttemptKind::New;
    dispatch_result.attempt_id = 1U;
    dispatch_result.venue = first_intent.venue;
    dispatch_result.session_epoch = first_intent.session_epoch;
    dispatch_result.wire_order_id = first_intent.wire_order_id;
    const auto accepted = core.apply(CoreEvent{
        ++sequence,
        LocalDispatchEvent{std::move(dispatch_result)},
    });
    assert(accepted.command_accepted);
    assert(core.snapshot().children.front().state ==
           ChildState::PendingAcknowledgement);

    const auto timeout = core.apply(CoreEvent{
        ++sequence,
        DispatchTimeoutEvent{first.id, "FIX response timeout"},
    });
    assert(!timeout.reports.empty());
    state = core.snapshot();
    assert(state.children.front().state == ChildState::Ambiguous);
    assert(state.children.front().risk_reservation == first.ordered);
    assert(state.parents.front().state == ParentState::Suspended);
}

void reordered_and_duplicate_execution_test() {
    SorCore core(configuration(1U), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_view(core, sequence, runtime_view(book(1U, 99, 100, 10U)));
    const ParentOrder order{
        ParentOrderId{20U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{10U},
        Price{100U},
        RoutingIntent::AggressiveIOC,
    };
    assert(core.apply(CoreEvent{++sequence, ParentSubmitEvent{order}})
               .command_accepted);
    const ChildSnapshot child = core.snapshot().children.front();

    const auto second_fill = report(
        child,
        "EXEC-2",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::Filled,
        5U,
        10U,
        0U);
    assert(core.apply(
                   CoreEvent{
                       ++sequence,
                       ExecutionReportEvent{second_fill},
                   })
               .command_accepted);
    // CumQty=10 arrived before the missing CumQty=5 report. The core retains
    // it without advancing accounting until the monotonic predecessor arrives.
    assert(core.snapshot().children.front().filled == Quantity{});

    const auto first_fill = report(
        child,
        "EXEC-1",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        5U,
        5U,
        5U);
    assert(core.apply(
                   CoreEvent{
                       ++sequence,
                       ExecutionReportEvent{first_fill},
                   })
               .command_accepted);
    auto state = core.snapshot();
    assert(state.children.front().filled == Quantity{10U});
    assert(state.children.front().state == ChildState::Filled);
    assert(state.children.front().risk_reservation == Quantity{});
    assert(state.parents.front().cumulative_filled == Quantity{10U});
    assert(state.parents.front().state == ParentState::Filled);

    assert(core.apply(
                   CoreEvent{
                       ++sequence,
                       ExecutionReportEvent{second_fill},
                   })
               .command_accepted);
    state = core.snapshot();
    assert(state.parents.front().cumulative_filled == Quantity{10U});
}

void cancel_ignores_feed_and_replace_race_test() {
    SorCore core(configuration(1U), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_view(core, sequence, runtime_view(book(1U, 99, 100, 20U)));
    const ParentOrder order{
        ParentOrderId{30U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{10U},
        Price{99U},
        RoutingIntent::PassiveGTC,
    };
    const auto submit =
        core.apply(CoreEvent{++sequence, ParentSubmitEvent{order}});
    assert(submit.command_accepted);
    assert(submit.effects.size() == 1U);
    const ChildSnapshot old_child = core.snapshot().children.front();

    auto failed_feed = runtime_view(book(1U, 99, 100, 20U));
    failed_feed.market_data_state = MarketDataState::Failed;
    failed_feed.feed_sufficiently_fresh = false;
    ++failed_feed.adjacency_generation;
    failed_feed.book->feed_state = MarketDataState::Failed;
    ++failed_feed.book->view_generation;
    publish_view(core, sequence, std::move(failed_feed), false);

    const auto initial_fill = report(
        old_child,
        "FILL-1",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        2U,
        2U,
        8U,
        99);
    assert(core.apply(
                   CoreEvent{
                       ++sequence,
                       ExecutionReportEvent{initial_fill},
                   })
               .command_accepted);

    const auto replace_result = core.apply(CoreEvent{
        ++sequence,
        ParentReplaceEvent{
            ParentReplace{order.id, Quantity{8U}, Price{98U}}},
    });
    assert(replace_result.command_accepted);
    assert(replace_result.effects.size() == 1U);
    assert(std::holds_alternative<DispatchCancelEffect>(
        replace_result.effects.front()));

    const auto racing_fill = report(
        old_child,
        "FILL-2",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        4U,
        6U,
        4U,
        99);
    assert(core.apply(
                   CoreEvent{
                       ++sequence,
                       ExecutionReportEvent{racing_fill},
                   })
               .command_accepted);

    const auto terminal_cancel = report(
        old_child,
        "CANCEL-1",
        adapters::ExecutionKind::Canceled,
        adapters::NormalizedOrderStatus::Canceled,
        0U,
        6U,
        4U);
    const auto canceled = core.apply(
        CoreEvent{
            ++sequence,
            ExecutionReportEvent{terminal_cancel},
        });
    assert(canceled.command_accepted);

    const auto state = core.snapshot();
    assert(state.parents.front().cumulative_filled == Quantity{6U});
    assert(state.parents.front().revision == 2U);
    assert(state.parents.front().order.quantity == Quantity{2U});
    assert(state.children.size() == 1U);
    assert(state.parents.front().state == ParentState::Rejected);
    assert(state.parents.front().explicit_remainder == Quantity{2U});
}

void cancel_without_fib_lookup_test() {
    SorCore core(configuration(1U), make_table(1U));
    std::uint64_t sequence = 0U;
    publish_view(core, sequence, runtime_view(book(1U, 99, 100, 20U)));
    const ParentOrder order{
        ParentOrderId{40U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{5U},
        Price{99U},
        RoutingIntent::PassiveGTC,
    };
    const auto submitted =
        core.apply(CoreEvent{++sequence, ParentSubmitEvent{order}});
    assert(submitted.command_accepted);
    assert(submitted.effects.size() == 1U);
    const auto& intent =
        std::get<DispatchNewEffect>(submitted.effects.front()).intent;
    assert(core.apply(CoreEvent{
                    ++sequence,
                    DispatchStartedEvent{
                        intent.child,
                        adapters::DispatchAttemptKind::New,
                        1U,
                        intent.venue,
                        intent.session_epoch,
                        intent.wire_order_id,
                    }})
               .command_accepted);
    adapters::LocalDispatchResult accepted;
    accepted.child = intent.child;
    accepted.state = adapters::LocalDispatchState::AcceptedLocally;
    accepted.kind = adapters::DispatchAttemptKind::New;
    accepted.attempt_id = 1U;
    accepted.venue = intent.venue;
    accepted.session_epoch = intent.session_epoch;
    accepted.wire_order_id = intent.wire_order_id;
    assert(core.apply(CoreEvent{
                    ++sequence,
                    LocalDispatchEvent{std::move(accepted)}})
               .command_accepted);

    auto withdrawn = runtime_view(book(1U, 99, 100, 20U));
    withdrawn.market_data_state = MarketDataState::Failed;
    withdrawn.order_entry_state = OrderEntryState::Degraded;
    withdrawn.feed_sufficiently_fresh = false;
    ++withdrawn.adjacency_generation;
    withdrawn.book->feed_state = MarketDataState::Failed;
    ++withdrawn.book->view_generation;
    publish_view(core, sequence, std::move(withdrawn), false);

    const auto cancel =
        core.apply(CoreEvent{++sequence, ParentCancelEvent{order.id}});
    assert(cancel.command_accepted);
    assert(cancel.effects.size() == 1U);
    assert(std::holds_alternative<DispatchCancelEffect>(
        cancel.effects.front()));
}

}  // namespace

int main() {
    aggressive_batch_and_ambiguity_test();
    reordered_and_duplicate_execution_test();
    cancel_ignores_feed_and_replace_race_test();
    cancel_without_fib_lookup_test();
    return 0;
}














