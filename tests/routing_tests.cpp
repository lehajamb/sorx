#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/book_view.hpp"
#include "sorx/route_rib.hpp"
#include "sorx/routing_policy.hpp"
#include "sorx/types.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using sorx::AdminState;
using sorx::CandidateExclusionReason;
using sorx::CandidateRuntimeView;
using sorx::EgressState;
using sorx::InstrumentId;
using sorx::InstrumentState;
using sorx::MarketDataState;
using sorx::OrderEntryState;
using sorx::ParentOrder;
using sorx::ParentOrderId;
using sorx::Price;
using sorx::Quantity;
using sorx::RouteConfig;
using sorx::RouteDecision;
using sorx::RouteRib;
using sorx::RoutingIntent;
using sorx::RoutingPolicy;
using sorx::Side;
using sorx::VenueCapabilities;
using sorx::VenueId;

[[nodiscard]] RouteConfig route(
    const std::uint32_t venue,
    const std::uint32_t instrument = 7,
    const std::uint32_t aggressive_priority = 0,
    const std::uint32_t passive_priority = 0,
    const std::uint64_t latency = 0,
    const std::uint64_t cap = 1'000) {
    return RouteConfig{
        VenueId{venue},
        InstrumentId{instrument},
        "VENUE_" + std::to_string(venue),
        "ALPHA",
        "ALPHA_" + std::to_string(venue),
        *sorx::RationalConversion::create(1, 1),
        *sorx::RationalConversion::create(1, 1),
        VenueCapabilities{true, true},
        Quantity{cap},
        aggressive_priority,
        passive_priority,
        latency,
        AdminState::Up,
    };
}

[[nodiscard]] sorx::md::BookView book(
    const std::uint32_t venue,
    const std::initializer_list<std::pair<std::int64_t, std::uint64_t>> bids,
    const std::initializer_list<std::pair<std::int64_t, std::uint64_t>> asks,
    const bool truncated = false,
    const std::uint32_t instrument = 7) {
    sorx::md::BookView value;
    value.venue = VenueId{venue};
    value.channel = sorx::ChannelId{venue};
    value.profile = sorx::ProfileId{1};
    value.instrument = InstrumentId{instrument};
    value.feed_session_epoch = 11;
    value.recovery_generation = 12;
    value.view_generation = 100 + venue;
    value.last_applied_sequence = 700;
    value.next_sequence = 701;
    value.exchange_timestamp_ns = 1'000;
    value.local_receive_time_ns = 2'000;
    value.local_publication_time_ns = 2'001;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    for (const auto [price, quantity] : bids) {
        const bool inserted = value.push_bid(
            sorx::md::Level{Price{price}, Quantity{quantity}, 1});
        assert(inserted);
    }
    for (const auto [price, quantity] : asks) {
        const bool inserted = value.push_ask(
            sorx::md::Level{Price{price}, Quantity{quantity}, 1});
        assert(inserted);
    }
    value.depth_truncated = truncated;
    assert(value.valid());
    return value;
}

[[nodiscard]] CandidateRuntimeView runtime(sorx::md::BookView value) {
    return CandidateRuntimeView{
        value.venue,
        500 + value.venue.value(),
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

[[nodiscard]] std::shared_ptr<const sorx::CandidateTable> table(
    std::vector<RouteConfig> routes) {
    RouteRib rib;
    std::vector<sorx::RouteValidationError> errors;
    assert(rib.replace_all(std::move(routes), errors));
    assert(errors.empty());
    sorx::FibCompiler compiler;
    return compiler.compile(rib);
}

[[nodiscard]] ParentOrder parent(
    const Side side,
    const std::uint64_t quantity,
    const std::int64_t limit,
    const RoutingIntent intent = RoutingIntent::AggressiveIOC) {
    return ParentOrder{
        ParentOrderId{42},
        InstrumentId{7},
        side,
        Quantity{quantity},
        Price{limit},
        intent,
    };
}

[[nodiscard]] bool excluded_for(
    const RouteDecision& decision,
    const VenueId venue,
    const CandidateExclusionReason reason) {
    for (const auto& exclusion : decision.exclusions) {
        if (exclusion.venue == venue && exclusion.reason == reason) {
            return true;
        }
    }
    return false;
}

void exact_value_tests() {
    static_assert(!std::is_same_v<VenueId, InstrumentId>);
    static_assert(sizeof(sorx::AdminState) == 1);
    static_assert(sizeof(sorx::MarketDataState) == 1);

    const auto half = sorx::RationalConversion::create(2, 4);
    assert(half);
    assert(half->numerator() == 1);
    assert(half->denominator() == 2);
    assert(half->convert_exact(Price{8}) == Price{4});
    assert(!half->convert_exact(Price{7}));
    assert(half->convert_exact(Price{-8}) == Price{-4});
    assert(half->convert_exact(Quantity{8}) == Quantity{4});
    assert(!half->convert_exact(Quantity{7}));
    assert(!sorx::RationalConversion::create(0, 1));
    assert(!sorx::RationalConversion::create(1, 0));

    assert(sorx::checked_notional(Price{125}, Quantity{8}) == sorx::Notional{1'000});
    assert(!sorx::checked_notional(Price{-1}, Quantity{8}));
    assert(!sorx::checked_notional(
        Price{std::numeric_limits<std::int64_t>::max()},
        Quantity{3}));
    assert(!sorx::checked_add(
        Quantity{std::numeric_limits<std::uint64_t>::max()},
        Quantity{1}));
    assert(sorx::checked_subtract(Quantity{9}, Quantity{4}) == Quantity{5});
    assert(!sorx::checked_subtract(Quantity{4}, Quantity{9}));
}

void owned_book_view_tests() {
    auto original = book(1, {{101, 3}, {100, 4}}, {{102, 5}, {103, 6}});
    auto copy = original;
    original.bids[0].quantity = Quantity{99};
    assert(copy.bids[0].quantity == Quantity{3});
    assert(copy.version().view_generation == 101);

    sorx::md::BookView full;
    full.venue = VenueId{1};
    full.channel = sorx::ChannelId{1};
    full.profile = sorx::ProfileId{1};
    full.instrument = InstrumentId{7};
    full.feed_session_epoch = 1;
    full.recovery_generation = 1;
    full.view_generation = 1;
    full.next_sequence = 1;
    full.feed_state = MarketDataState::Live;
    full.trading_state = InstrumentState::Trading;
    for (std::size_t index = 0; index < sorx::md::kMaxDepthLevels; ++index) {
        assert(full.push_bid(sorx::md::Level{
            Price{1'000 - static_cast<std::int64_t>(index)},
            Quantity{1},
            1,
        }));
    }
    assert(!full.push_bid(sorx::md::Level{Price{1}, Quantity{1}, 1}));
    assert(full.depth_truncated);
    assert(full.valid());
}

void rib_and_fib_tests() {
    RouteRib rib;
    std::vector<sorx::RouteValidationError> errors;
    auto first_route = route(2);
    auto second_route = route(1);
    assert(rib.replace_all({first_route, second_route}, errors));
    assert(rib.revision() == 1);
    assert(rib.routes()[0].venue == VenueId{1});

    sorx::FibCompiler compiler;
    const auto first = compiler.compile(rib);
    const auto unchanged = compiler.compile(rib);
    assert(first.get() == unchanged.get());
    assert(first->generation() == 1);
    assert(first->source_rib_revision() == rib.revision());
    assert(first->find(InstrumentId{7}) != nullptr);
    assert(first->find(InstrumentId{99}) == nullptr);

    assert(rib.set_admin(VenueId{2}, AdminState::Down));
    const auto changed = compiler.compile(rib);
    assert(changed.get() != first.get());
    assert(changed->generation() == 2);
    assert(changed->find(InstrumentId{7})->candidates[1].admin_state == AdminState::Down);
    assert(compiler.compile(rib).get() == changed.get());

    RouteRib invalid;
    auto duplicate = route(1);
    assert(!invalid.replace_all({duplicate, duplicate}, errors));
    assert(!errors.empty());
    assert(invalid.routes().empty());

    auto symbol_conflict = route(2, 8);
    symbol_conflict.canonical_symbol = "ALPHA";
    assert(!invalid.replace_all({route(1), symbol_conflict}, errors));
    assert(!errors.empty());

    auto zero_cap = route(1);
    zero_cap.allocation_cap = Quantity{};
    assert(!invalid.replace_all({zero_cap}, errors));
}

void aggressive_sweep_and_tie_tests() {
    const auto candidates = table({
        route(1, 7, 2),
        route(2, 7, 1),
        route(3, 7, 0),
    });
    const std::vector views{
        runtime(book(1, {}, {{100, 4}, {102, 4}})),
        runtime(book(2, {}, {{100, 5}, {103, 5}})),
        runtime(book(3, {}, {{101, 2}})),
    };

    const auto decision = RoutingPolicy::aggressive_ioc(
        parent(Side::Buy, 12, 102),
        *candidates,
        views,
        3);
    assert(decision.accepted());
    assert(decision.fully_allocated());
    assert(decision.allocations.size() == 3);
    assert(decision.allocations[0].venue == VenueId{2});
    assert(decision.allocations[0].quantity == Quantity{5});
    assert(decision.allocations[0].limit_price == Price{100});
    assert(decision.allocations[1].venue == VenueId{1});
    assert(decision.allocations[1].quantity == Quantity{5});
    assert(decision.allocations[1].limit_price == Price{102});
    assert(decision.allocations[2].venue == VenueId{3});
    assert(decision.allocations[2].quantity == Quantity{2});
    assert(decision.allocations[0].book_version.view_generation == 102);
    assert(decision.risk_generation == 0U);
    assert(decision.candidate_pins.size() == 3U);
    assert(decision.candidate_pins[0].venue == VenueId{1});
    assert(decision.candidate_pins[0].adjacency_generation == 501U);
    assert(decision.candidate_pins[0].book_version.has_value());
    assert(decision.candidate_pins[0].book_version->view_generation == 101U);
    assert(decision.candidate_pins[1].venue == VenueId{2});
    assert(decision.candidate_pins[1].book_version->view_generation == 102U);
    assert(decision.candidate_pins[2].venue == VenueId{3});
    assert(decision.candidate_pins[2].book_version->view_generation == 103U);

    const auto sell_candidates = table({route(1), route(2)});
    const std::vector sell_views{
        runtime(book(1, {{101, 1}}, {})),
        runtime(book(2, {{102, 1}}, {})),
    };
    const auto sell = RoutingPolicy::aggressive_ioc(
        parent(Side::Sell, 2, 100),
        *sell_candidates,
        sell_views,
        2);
    assert(sell.fully_allocated());
    assert(sell.allocations[0].venue == VenueId{2});
    assert(sell.allocations[1].venue == VenueId{1});
}

void aggressive_constraints_tests() {
    const auto max_candidates = table({route(1), route(2)});
    const std::vector max_views{
        runtime(book(1, {}, {{100, 1}, {103, 4}})),
        runtime(book(2, {}, {{101, 10}})),
    };
    const auto maximum = RoutingPolicy::aggressive_ioc(
        parent(Side::Buy, 5, 103),
        *max_candidates,
        max_views,
        1);
    assert(maximum.fully_allocated());
    assert(maximum.allocations.size() == 1);
    assert(maximum.allocations[0].venue == VenueId{1});
    assert(maximum.allocations[0].quantity == Quantity{5});
    assert(maximum.allocations[0].limit_price == Price{103});
    assert(excluded_for(
        maximum,
        VenueId{2},
        CandidateExclusionReason::MaximumVenueCount));

    auto capped_route = route(1);
    capped_route.allocation_cap = Quantity{2};
    const auto capped_candidates = table({capped_route});
    const std::vector capped_views{runtime(book(1, {}, {{100, 5}}))};
    const auto capped = RoutingPolicy::aggressive_ioc(
        parent(Side::Buy, 5, 100),
        *capped_candidates,
        capped_views,
        1);
    assert(capped.allocated == Quantity{2});
    assert(capped.remainder == Quantity{3});
    assert(capped.remainder_reason == sorx::RemainderReason::VenueAllocationCaps);

    const auto truncated_candidates = table({route(1)});
    const std::vector truncated_views{
        runtime(book(1, {}, {{100, 2}}, true)),
    };
    const auto truncated = RoutingPolicy::aggressive_ioc(
        parent(Side::Buy, 5, 100),
        *truncated_candidates,
        truncated_views,
        1);
    assert(truncated.allocated == Quantity{2});
    assert(truncated.remainder == Quantity{3});
    assert(truncated.observed_truncated_depth);
    assert(truncated.remainder_reason == sorx::RemainderReason::TruncatedDepthUnknown);

    auto unavailable_view = runtime(book(1, {}, {{100, 5}}));
    unavailable_view.feed_sufficiently_fresh = false;
    const std::vector unavailable_views{unavailable_view};
    const auto unavailable = RoutingPolicy::aggressive_ioc(
        parent(Side::Buy, 5, 100),
        *truncated_candidates,
        unavailable_views,
        1);
    assert(!unavailable.accepted());
    assert(unavailable.reject_reason == sorx::RouteRejectReason::NoEligibleCandidate);
    assert(excluded_for(
        unavailable,
        VenueId{1},
        CandidateExclusionReason::StaleBook));
}

void aggressive_exact_conversion_test() {
    auto converted = route(1);
    converted.quantity_conversion = *sorx::RationalConversion::create(2, 3);
    const auto candidates = table({converted});
    const std::vector views{runtime(book(1, {}, {{100, 5}}))};
    const auto decision = RoutingPolicy::aggressive_ioc(
        parent(Side::Buy, 5, 100),
        *candidates,
        views,
        1);
    assert(decision.accepted());
    assert(decision.allocated == Quantity{3});
    assert(decision.remainder == Quantity{2});
    assert(decision.allocations[0].venue_quantity == Quantity{2});
    assert(decision.remainder_reason == sorx::RemainderReason::InexactConversion);
}

void passive_tests() {
    const auto candidates = table({
        route(1, 7, 0, 2, 5),
        route(2, 7, 0, 1, 50),
        route(3, 7, 0, 1, 10),
    });
    const std::vector views{
        runtime(book(1, {{99, 1}}, {{101, 1}})),
        runtime(book(2, {{99, 1}}, {{101, 1}})),
        runtime(book(3, {{99, 1}}, {{101, 1}})),
    };
    const auto decision = RoutingPolicy::passive_gtc(
        parent(Side::Buy, 20, 100, RoutingIntent::PassiveGTC),
        *candidates,
        views);
    assert(decision.accepted());
    assert(decision.fully_allocated());
    assert(decision.allocations.size() == 1);
    assert(decision.allocations[0].venue == VenueId{3});
    assert(decision.allocations[0].quantity == Quantity{20});
    assert(decision.allocations[0].limit_price == Price{100});

    auto non_passive = route(1);
    non_passive.capabilities.passive_gtc = false;
    const auto market_candidates = table({non_passive, route(2)});
    const std::vector market_views{
        runtime(book(1, {}, {{99, 1}})),
        runtime(book(2, {}, {{101, 1}})),
    };
    const auto marketable = RoutingPolicy::passive_gtc(
        parent(Side::Buy, 5, 100, RoutingIntent::PassiveGTC),
        *market_candidates,
        market_views);
    assert(!marketable.accepted());
    assert(marketable.reject_reason == sorx::RouteRejectReason::PassiveMarketable);
    assert(marketable.allocations.empty());

    auto small_one = route(1);
    auto small_two = route(2);
    small_one.allocation_cap = Quantity{2};
    small_two.allocation_cap = Quantity{4};
    const auto small_candidates = table({small_one, small_two});
    const std::vector small_views{
        runtime(book(1, {}, {{101, 1}})),
        runtime(book(2, {}, {{101, 1}})),
    };
    const auto too_large = RoutingPolicy::passive_gtc(
        parent(Side::Buy, 5, 100, RoutingIntent::PassiveGTC),
        *small_candidates,
        small_views);
    assert(!too_large.accepted());
    assert(too_large.reject_reason ==
           sorx::RouteRejectReason::PassiveFullQuantityUnavailable);
    assert(too_large.allocations.empty());
}

}  // namespace

int main() {
    exact_value_tests();
    owned_book_view_tests();
    rib_and_fib_tests();
    aggressive_sweep_and_tie_tests();
    aggressive_constraints_tests();
    aggressive_exact_conversion_test();
    passive_tests();
    return 0;
}


