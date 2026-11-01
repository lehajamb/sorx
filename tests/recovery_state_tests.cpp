#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/order_core.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sorx;

std::shared_ptr<const CandidateTable> table() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(
        {
            RouteConfig{
                VenueId{1U},
                InstrumentId{7U},
                "VENUE_1",
                "ALPHA",
                "ALPHA_1",
                *RationalConversion::create(1U, 1U),
                *RationalConversion::create(1U, 1U),
                VenueCapabilities{true, true},
                Quantity{100U},
                1U,
                1U,
                10U,
                AdminState::Up,
            },
        },
        errors));
    FibCompiler compiler;
    return compiler.compile(rib);
}

CoreConfiguration configuration() {
    CoreConfiguration result;
    result.maximum_aggressive_venues = 1U;
    result.risk.generation = 9U;
    result.risk.instruments.push_back(InstrumentRiskLimit{
        InstrumentId{7U},
        true,
        Quantity{100U},
        Quantity{100U},
        Notional{100'000U},
        Notional{100'000U},
        Price{1U},
        Price{1'000U},
        10U,
        10U,
        Quantity{1'000U},
    });
    result.risk.venues.push_back(
        VenueRiskLimit{VenueId{1U}, true, Quantity{1'000U}});
    return result;
}

CandidateRuntimeView live_view() {
    md::BookView book;
    book.venue = VenueId{1U};
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = InstrumentId{7U};
    book.feed_session_epoch = 1U;
    book.recovery_generation = 1U;
    book.view_generation = 1U;
    book.last_applied_sequence = 3U;
    book.next_sequence = 4U;
    book.local_receive_time_ns = 10U;
    book.local_publication_time_ns = 11U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.push_bid(
        md::Level{Price{99U}, Quantity{20U}, 1U}));
    assert(book.push_ask(
        md::Level{Price{100U}, Quantity{20U}, 1U}));
    return CandidateRuntimeView{
        VenueId{1U},
        2U,
        book,
        MarketDataState::Live,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        true,
        true,
        true,
    };
}

void assert_same_records(
    const ApplyResult& lhs,
    const ApplyResult& rhs) {
    assert(lhs.command_accepted == rhs.command_accepted);
    assert(lhs.command_error == rhs.command_error);
    assert(lhs.records.size() == rhs.records.size());
    for (std::size_t index = 0U; index < lhs.records.size(); ++index) {
        assert(lhs.records[index].kind == rhs.records[index].kind);
        assert(lhs.records[index].ingress_sequence ==
               rhs.records[index].ingress_sequence);
        assert(lhs.records[index].payload ==
               rhs.records[index].payload);
    }
}

}  // namespace

int main() {
    const auto candidates = table();
    SorCore original(configuration(), candidates);
    std::uint64_t sequence = 0U;
    assert(original
               .apply(CoreEvent{
                   ++sequence,
                   OrderEntrySessionEvent{
                       VenueId{1U}, 1U, OrderEntryState::Ready},
               })
               .command_accepted);
    assert(original
               .apply(CoreEvent{
                   ++sequence,
                   RuntimeViewPublishedEvent{
                       InstrumentId{7U}, live_view()},
               })
               .command_accepted);
    const ParentOrder order{
        ParentOrderId{77U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{10U},
        Price{100U},
        RoutingIntent::AggressiveIOC,
    };
    assert(original
               .apply(CoreEvent{
                   ++sequence, ParentSubmitEvent{order}})
               .command_accepted);
    const ChildSnapshot child = original.snapshot().children.front();

    adapters::NormalizedExecutionReport partial;
    partial.venue = child.venue;
    partial.session_epoch = child.session_epoch;
    partial.wire_order_id = child.wire_order_id;
    partial.execution_id = "EXEC-PARTIAL";
    partial.durable_report_key = partial.execution_id;
    partial.kind = adapters::ExecutionKind::Trade;
    partial.status =
        adapters::NormalizedOrderStatus::PartiallyFilled;
    partial.last_quantity = Quantity{3U};
    partial.last_price = Price{100U};
    partial.cumulative_quantity = Quantity{3U};
    partial.leaves_quantity = Quantity{7U};
    assert(original
               .apply(CoreEvent{
                   ++sequence,
                   ExecutionReportEvent{partial},
               })
               .command_accepted);

    const auto saved = original.save_state();
    SorCore restored(configuration(), table());
    restored.restore_state(saved);
    assert(restored.save_state() == saved);

    const auto duplicate_original = original.apply(CoreEvent{
        ++sequence,
        ExecutionReportEvent{partial},
    });
    const auto duplicate_restored = restored.apply(CoreEvent{
        sequence,
        ExecutionReportEvent{partial},
    });
    assert_same_records(duplicate_original, duplicate_restored);
    assert(original.save_state() == restored.save_state());

    const auto cancel_original = original.apply(
        CoreEvent{++sequence, ParentCancelEvent{order.id}});
    const auto cancel_restored = restored.apply(
        CoreEvent{sequence, ParentCancelEvent{order.id}});
    assert_same_records(cancel_original, cancel_restored);
    assert(cancel_original.effects.size() ==
           cancel_restored.effects.size());
    assert(original.save_state() == restored.save_state());

    return 0;
}


