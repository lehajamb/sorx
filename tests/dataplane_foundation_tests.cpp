#include "sorx/dataplane/book_slot.hpp"
#include "sorx/dataplane/route_kernel.hpp"
#include "sorx/dataplane/spsc_ring.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::dataplane;

constexpr VenueId kVenueOne{1U};
constexpr VenueId kVenueTwo{2U};
constexpr InstrumentId kInstrument{11U};

[[nodiscard]] WireIdentityPlanStatus constant_native_identity(
    const WireIdentityPlanRequest& request,
    InlineWireIdentity& output) noexcept {
    if (!request.venue || !request.child_order_id ||
        request.dispatch_attempt_id == 0U ||
        request.venue_session_id == 0U ||
        request.venue_session_epoch == 0U) {
        return WireIdentityPlanStatus::InvalidInput;
    }
    output = {};
    output.size = 18U;
    output.bytes[3] = std::byte{1U};
    std::fill_n(output.bytes.begin() + 4U, 14U, std::byte{'A'});
    return WireIdentityPlanStatus::Planned;
}

std::uint64_t unstable_identity_calls{};

[[nodiscard]] WireIdentityPlanStatus unstable_native_identity(
    const WireIdentityPlanRequest& request,
    InlineWireIdentity& output) noexcept {
    const WireIdentityPlanStatus status = constant_native_identity(request, output);
    if (status == WireIdentityPlanStatus::Planned) {
        output.bytes[17U] = (++unstable_identity_calls & 1U) != 0U
            ? std::byte{'A'}
            : std::byte{'B'};
    }
    return status;
}

[[nodiscard]] RouteConfig route(
    const VenueId venue,
    const std::uint32_t priority) {
    RouteConfig output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.venue_name = venue == kVenueOne ? "VENUE_ONE" : "VENUE_TWO";
    output.canonical_symbol = "TEST";
    output.wire_symbol = venue == kVenueOne ? "TEST1" : "TEST2";
    output.capabilities = VenueCapabilities{true, true};
    output.allocation_cap = Quantity{100U};
    output.aggressive_priority = priority;
    output.passive_priority = priority;
    output.configured_latency_ns = 100U + priority;
    return output;
}

[[nodiscard]] std::shared_ptr<const CandidateTable> candidate_table() {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all(
        std::vector<RouteConfig>{route(kVenueOne, 1U), route(kVenueTwo, 2U)},
        errors));
    assert(errors.empty());
    FibCompiler compiler;
    return compiler.compile(rib);
}

[[nodiscard]] CoreConfiguration configuration() {
    CoreConfiguration output;
    output.maximum_aggressive_venues = 2U;
    output.risk.generation = 7U;
    InstrumentRiskLimit instrument;
    instrument.instrument = kInstrument;
    instrument.maximum_parent_quantity = Quantity{1'000U};
    instrument.maximum_child_quantity = Quantity{1'000U};
    instrument.maximum_parent_notional = Notional{1'000'000'000U};
    instrument.maximum_child_notional = Notional{1'000'000'000U};
    instrument.minimum_price = Price{1};
    instrument.maximum_price = Price{1'000'000};
    instrument.maximum_open_parents = 100U;
    instrument.maximum_open_children = 100U;
    instrument.maximum_gross_outstanding = Quantity{10'000U};
    output.risk.instruments.push_back(instrument);
    output.risk.venues.push_back(VenueRiskLimit{
        kVenueOne, true, Quantity{10'000U}});
    output.risk.venues.push_back(VenueRiskLimit{
        kVenueTwo, true, Quantity{10'000U}});
    return output;
}

[[nodiscard]] BookSlotView live_view(
    const VenueId venue,
    const Price ask,
    const Quantity ask_quantity,
    const std::uint64_t view_generation = 1U) {
    md::BookView book;
    book.venue = venue;
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = kInstrument;
    assert(book.push_bid(md::Level{
        Price{ask.units - 2}, Quantity{10U}, 1U}));
    assert(book.push_ask(md::Level{ask, ask_quantity, 1U}));
    book.feed_session_epoch = 3U;
    book.recovery_generation = 1U;
    book.view_generation = view_generation;
    book.last_applied_sequence = view_generation;
    book.next_sequence = view_generation + 1U;
    book.exchange_timestamp_ns = 900;
    book.local_receive_time_ns = 1'000U;
    book.local_publication_time_ns = 1'010U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.valid());

    BookSlotView output;
    output.venue = venue;
    output.instrument = kInstrument;
    output.adjacency_generation = 9U;
    output.book = book;
    output.market_data_state = MarketDataState::Live;
    output.order_entry_state = OrderEntryState::Ready;
    output.egress_state = EgressState::Ready;
    output.instrument_state = InstrumentState::Trading;
    output.feed_epoch = book.feed_session_epoch;
    output.source_health_generation = 5U;
    output.monotonic_receive_time_ns = 1'000U;
    output.monotonic_publication_time_ns = 1'010U;
    output.maximum_quote_age_ns = 100U;
    output.tx_credit_generation = 4U;
    output.throttle_generation = 8U;
    output.normal_tx_credits = 1U;
    output.cancel_reserved_tx_credits = 2U;
    output.throttle_credits = 4U;
    output.has_book = true;
    output.source_healthy = true;
    output.conversion_compatible = true;
    output.risk_allowed = true;
    assert(output.valid());
    return output;
}

void test_spsc_ring() {
    SpscRing<std::uint64_t, 8U> ring;
    for (std::uint64_t value = 0U; value < ring.capacity(); ++value) {
        assert(ring.try_push(value));
    }
    assert(!ring.try_push(99U));
    for (std::uint64_t expected = 0U; expected < ring.capacity(); ++expected) {
        std::uint64_t actual = std::numeric_limits<std::uint64_t>::max();
        assert(ring.try_pop(actual));
        assert(actual == expected);
    }
    std::uint64_t unused{};
    assert(!ring.try_pop(unused));

    constexpr std::uint64_t kMessages = 100'000U;
    SpscRing<std::uint64_t, 1024U> concurrent;
    std::thread producer([&concurrent] {
        for (std::uint64_t value = 0U; value < kMessages; ++value) {
            while (!concurrent.try_push(value)) {
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&concurrent] {
        for (std::uint64_t expected = 0U; expected < kMessages; ++expected) {
            std::uint64_t actual{};
            while (!concurrent.try_pop(actual)) {
                std::this_thread::yield();
            }
            assert(actual == expected);
        }
    });
    producer.join();
    consumer.join();
    assert(concurrent.empty_approx());
}

void test_book_publisher() {
    BookSlotPublisher publisher;
    BookSlotView output;
    assert(!publisher.try_read(output));
    const BookSlotView first = live_view(kVenueOne, Price{100}, Quantity{5U});
    assert(publisher.try_publish(first) == BookPublishStatus::Published);
    std::uint64_t generation{};
    assert(publisher.try_read(output, &generation));
    assert(generation == 1U);
    assert(output.book.version() == first.book.version());

    constexpr std::uint64_t kPublications = 20'000U;
    std::atomic<bool> complete{};
    std::thread writer([&publisher, &first, &complete] {
        for (std::uint64_t value = 2U; value <= kPublications; ++value) {
            BookSlotView next = first;
            next.book.view_generation = value;
            next.book.last_applied_sequence = value;
            next.book.next_sequence = value + 1U;
            next.book.asks[0].price = Price{99 + static_cast<std::int64_t>(value)};
            next.book.bids[0].price = Price{98 + static_cast<std::int64_t>(value)};
            while (publisher.try_publish(next) == BookPublishStatus::ReaderBusy) {
                std::this_thread::yield();
            }
        }
        complete.store(true, std::memory_order_release);
    });
    std::thread reader([&publisher, &complete] {
        while (!complete.load(std::memory_order_acquire)) {
            BookSlotView snapshot;
            if (publisher.try_read(snapshot)) {
                assert(snapshot.valid());
                assert(snapshot.book.asks[0].price.units ==
                       99 + static_cast<std::int64_t>(
                           snapshot.book.view_generation));
            }
        }
    });
    writer.join();
    reader.join();
    assert(publisher.generation() == kPublications);
}

void test_route_kernel() {
    const auto candidates = candidate_table();
    RouteKernel kernel(
        configuration(),
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::ReferenceGeneric);
    std::array<BookSlotView, 2U> views{
        live_view(kVenueOne, Price{100}, Quantity{4U}),
        live_view(kVenueTwo, Price{101}, Quantity{6U}),
    };
    const ParentOrder parent{
        ParentOrderId{1U},
        kInstrument,
        Side::Buy,
        Quantity{10U},
        Price{101},
        RoutingIntent::AggressiveIOC,
    };

    const PlanResult planned = kernel.plan(parent, views, 1'050U);
    assert(planned.accepted());
    assert(planned.plan.allocation_count == 2U);
    assert(planned.plan.allocated == Quantity{10U});
    assert(planned.plan.allocations[0].venue == kVenueOne);
    assert(planned.plan.allocations[0].quantity == Quantity{4U});
    assert(planned.plan.allocations[1].venue == kVenueTwo);
    assert(planned.plan.allocations[1].quantity == Quantity{6U});
    assert(planned.plan.planned_children[0].child_order_id == ChildOrderId{1U});
    assert(planned.plan.planned_children[1].child_order_id == ChildOrderId{2U});
    assert(planned.plan.planned_children[0].dispatch_attempt_id == 1U);
    assert(planned.plan.planned_children[1].dispatch_attempt_id == 2U);
    assert(planned.plan.planned_children[0].wire_identity.size ==
           kInlineWireIdentityBytes);
    assert(planned.plan.planned_children[0].wire_identity !=
           planned.plan.planned_children[1].wire_identity);
    assert(planned.plan.planned_children[0].normal_tx_credit_delta == 1U);
    assert(planned.plan.planned_children[0].cancel_tx_credit_delta == 0U);

    RoutePlan duplicate_ids = planned.plan;
    duplicate_ids.planned_children[1] = duplicate_ids.planned_children[0];
    assert(kernel.commit(duplicate_ids, views, 1'060U).status ==
           CommitStatus::WireIdentityMismatch);

    const CommitResult committed = kernel.commit(planned.plan, views, 1'060U);
    assert(committed.committed());
    assert(kernel.active_parent_count() == 1U);
    const InstrumentRiskCounters instrument =
        kernel.instrument_counters(kInstrument);
    assert(instrument.found);
    assert(instrument.gross_outstanding == Quantity{10U});
    assert(instrument.open_parents == 1U);
    assert(instrument.open_children == 2U);
    assert(kernel.venue_counters(kVenueOne).outstanding == Quantity{4U});
    assert(kernel.venue_counters(kVenueTwo).outstanding == Quantity{6U});

    assert(kernel.commit(planned.plan, views, 1'061U).status ==
           CommitStatus::StalePlan);
    assert(kernel.release_child(parent.id, kVenueOne) == ReleaseStatus::Released);
    assert(kernel.release_child(parent.id, kVenueTwo) == ReleaseStatus::Released);
    assert(kernel.active_parent_count() == 0U);
    assert(kernel.instrument_counters(kInstrument).gross_outstanding ==
           Quantity{});

    ParentOrder capacity_blocked = parent;
    capacity_blocked.id = ParentOrderId{99U};
    assert(kernel.plan(capacity_blocked, views, 1'080U).status ==
           PlanStatus::RouteRejected);
    for (BookSlotView& view : views) {
        ++view.tx_credit_generation;
    }

    ParentOrder second = parent;
    second.id = ParentOrderId{2U};
    const PlanResult second_plan = kernel.plan(second, views, 1'080U);
    assert(second_plan.accepted());
    assert(second_plan.plan.planned_children[0].child_order_id == ChildOrderId{3U});
    assert(second_plan.plan.planned_children[0].dispatch_attempt_id == 3U);
    auto changed_views = views;
    changed_views[0].book.view_generation = 2U;
    changed_views[0].book.last_applied_sequence = 2U;
    changed_views[0].book.next_sequence = 3U;
    assert(changed_views[0].valid());
    assert(kernel.commit(second_plan.plan, changed_views, 1'090U).status ==
           CommitStatus::StaleMarketView);

    ParentOrder stale = parent;
    stale.id = ParentOrderId{3U};
    const PlanResult stale_plan = kernel.plan(stale, views, 1'200U);
    assert(stale_plan.status == PlanStatus::RouteRejected);

    auto cancel_only = views;
    for (BookSlotView& view : cancel_only) {
        view.monotonic_receive_time_ns = 2'000U;
        view.monotonic_publication_time_ns = 2'001U;
        view.tx_credit_generation = 6U;
        view.normal_tx_credits = 0U;
        view.cancel_reserved_tx_credits = 100U;
        view.throttle_generation = 9U;
        view.throttle_credits = 10U;
        assert(view.valid());
    }
    ParentOrder cancel_capacity = parent;
    cancel_capacity.id = ParentOrderId{4U};
    assert(kernel.plan(cancel_capacity, cancel_only, 2'010U).status ==
           PlanStatus::RouteRejected);
}

void test_required_wire_identity_and_collision_gate() {
    const auto candidates = candidate_table();
    std::array<BookSlotView, 2U> views{
        live_view(kVenueOne, Price{100}, Quantity{10U}),
        live_view(kVenueTwo, Price{101}, Quantity{10U}),
    };
    for (BookSlotView& view : views) {
        view.normal_tx_credits = 10U;
        view.throttle_credits = 10U;
    }
    bool missing_rejected{};
    try {
        RouteKernel missing(configuration(), *candidates);
        static_cast<void>(missing);
    } catch (const std::invalid_argument&) {
        missing_rejected = true;
    }
    assert(missing_rejected);

    const std::array incomplete_bindings{
        WireIdentityPlannerBinding{
            kVenueOne, 7U, 9U, &constant_native_identity},
    };
    bool incomplete_rejected{};
    try {
        RouteKernel incomplete(
            configuration(),
            *candidates,
            AdaptivePolicyMode::Optional,
            WireIdentityMode::RequireRegistered,
            incomplete_bindings);
        static_cast<void>(incomplete);
    } catch (const std::invalid_argument&) {
        incomplete_rejected = true;
    }
    assert(incomplete_rejected);
    const ParentOrder first{
        ParentOrderId{10U},
        kInstrument,
        Side::Buy,
        Quantity{1U},
        Price{101},
        RoutingIntent::AggressiveIOC,
    };
    const std::array bindings{
        WireIdentityPlannerBinding{
            kVenueOne, 7U, 9U, &constant_native_identity},
        WireIdentityPlannerBinding{
            kVenueTwo, 8U, 9U, &constant_native_identity},
    };
    RouteKernel kernel(
        configuration(),
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::RequireRegistered,
        bindings);
    const PlanResult planned = kernel.plan(first, views, 1'050U);
    assert(planned.accepted());
    assert(planned.plan.planned_children[0].wire_identity.size == 18U);
    assert(kernel.commit(planned.plan, views, 1'051U).committed());

    ParentOrder second = first;
    second.id = ParentOrderId{11U};
    const PlanResult collision = kernel.plan(second, views, 1'052U);
    assert(collision.status == PlanStatus::WireIdentityRejected);
    assert(collision.wire_identity_status ==
           WireIdentityPlanStatus::InvalidOutput);
    assert(kernel.release_child(first.id, kVenueOne) == ReleaseStatus::Released);
    assert(kernel.plan(second, views, 1'053U).accepted());

    unstable_identity_calls = 0U;
    const std::array unstable_bindings{
        WireIdentityPlannerBinding{
            kVenueOne, 7U, 9U, &unstable_native_identity},
        WireIdentityPlannerBinding{
            kVenueTwo, 8U, 9U, &unstable_native_identity},
    };
    RouteKernel unstable(
        configuration(),
        *candidates,
        AdaptivePolicyMode::Optional,
        WireIdentityMode::RequireRegistered,
        unstable_bindings);
    const PlanResult unstable_plan = unstable.plan(first, views, 1'060U);
    assert(unstable_plan.accepted());
    assert(unstable.commit(unstable_plan.plan, views, 1'061U).status ==
           CommitStatus::WireIdentityMismatch);
}

}  // namespace

int main() {
    test_spsc_ring();
    test_book_publisher();
    test_route_kernel();
    test_required_wire_identity_and_collision_gate();
    return 0;
}
