#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/routing_policy.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sorx;

class DeterministicGenerator final {
public:
    std::uint64_t next() noexcept {
        state_ = state_ * 6'364'136'223'846'793'005ULL +
                 1'442'695'040'888'963'407ULL;
        return state_;
    }

    std::uint64_t between(
        const std::uint64_t minimum,
        const std::uint64_t maximum) noexcept {
        return minimum + next() % (maximum - minimum + 1U);
    }

private:
    std::uint64_t state_{0x534f52582d50524fULL};
};

RouteConfig route(
    const std::uint32_t venue,
    const std::uint64_t cap) {
    return RouteConfig{
        VenueId{venue},
        InstrumentId{7U},
        "VENUE_" + std::to_string(venue),
        "ALPHA",
        "ALPHA_" + std::to_string(venue),
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{cap},
        venue % 2U,
        venue % 3U,
        static_cast<std::uint64_t>(venue) * 10U,
        AdminState::Up,
    };
}

md::BookView book(
    const std::uint32_t venue,
    const std::uint64_t first_depth,
    const std::uint64_t second_depth,
    const bool truncated) {
    md::BookView value;
    value.venue = VenueId{venue};
    value.channel = ChannelId{venue};
    value.profile = ProfileId{1U};
    value.instrument = InstrumentId{7U};
    value.feed_session_epoch = 1U;
    value.recovery_generation = 1U;
    value.view_generation = venue;
    value.last_applied_sequence = 10U;
    value.next_sequence = 11U;
    value.local_receive_time_ns = 100U;
    value.local_publication_time_ns = 101U;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    assert(value.push_bid(md::Level{
        Price{100 - static_cast<std::int64_t>(venue)},
        Quantity{first_depth},
        1U,
    }));
    assert(value.push_bid(md::Level{
        Price{90 - static_cast<std::int64_t>(venue)},
        Quantity{second_depth},
        1U,
    }));
    assert(value.push_ask(md::Level{
        Price{100 + static_cast<std::int64_t>(venue)},
        Quantity{first_depth},
        1U,
    }));
    assert(value.push_ask(md::Level{
        Price{110 + static_cast<std::int64_t>(venue)},
        Quantity{second_depth},
        1U,
    }));
    value.depth_truncated = truncated;
    assert(value.valid());
    return value;
}

CandidateRuntimeView runtime(md::BookView value) {
    return CandidateRuntimeView{
        value.venue,
        value.view_generation,
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

void assert_same_allocations(
    const RouteDecision& lhs,
    const RouteDecision& rhs) {
    assert(lhs.candidate_table_generation ==
           rhs.candidate_table_generation);
    assert(lhs.allocated == rhs.allocated);
    assert(lhs.remainder == rhs.remainder);
    assert(lhs.remainder_reason == rhs.remainder_reason);
    assert(lhs.reject_reason == rhs.reject_reason);
    assert(lhs.allocations.size() == rhs.allocations.size());
    for (std::size_t index = 0U;
         index < lhs.allocations.size();
         ++index) {
        assert(lhs.allocations[index] == rhs.allocations[index]);
    }
}

}  // namespace

int main() {
    DeterministicGenerator random;
    for (std::size_t scenario = 0U; scenario < 500U; ++scenario) {
        std::vector<RouteConfig> routes;
        std::vector<CandidateRuntimeView> views;
        std::vector<std::uint64_t> caps;
        for (std::uint32_t venue = 1U; venue <= 4U; ++venue) {
            const std::uint64_t cap = random.between(1U, 60U);
            caps.push_back(cap);
            routes.push_back(route(venue, cap));
            views.push_back(runtime(book(
                venue,
                random.between(1U, 40U),
                random.between(1U, 40U),
                random.between(0U, 3U) == 0U)));
        }

        RouteRib rib;
        std::vector<RouteValidationError> errors;
        assert(rib.replace_all(std::move(routes), errors));
        FibCompiler compiler;
        const auto candidates = compiler.compile(rib);

        const Side side =
            random.between(0U, 1U) == 0U ?
                Side::Buy :
                Side::Sell;
        const ParentOrder parent{
            ParentOrderId{static_cast<std::uint64_t>(scenario + 1U)},
            InstrumentId{7U},
            side,
            Quantity{random.between(1U, 150U)},
            side == Side::Buy ? Price{115U} : Price{85U},
            RoutingIntent::AggressiveIOC,
        };
        const std::size_t maximum_venues =
            static_cast<std::size_t>(random.between(1U, 4U));
        const RouteDecision decision =
            RoutingPolicy::aggressive_ioc(
                parent, *candidates, views, maximum_venues);
        const RouteDecision repeated =
            RoutingPolicy::aggressive_ioc(
                parent, *candidates, views, maximum_venues);
        assert_same_allocations(decision, repeated);
        assert(decision.accepted());

        std::uint64_t sum = 0U;
        std::set<VenueId> selected;
        for (const auto& allocation : decision.allocations) {
            assert(selected.insert(allocation.venue).second);
            assert(allocation.quantity.units != 0U);
            assert(allocation.quantity.units <=
                   caps[allocation.venue.value() - 1U]);
            assert(allocation.quantity == allocation.venue_quantity);
            if (side == Side::Buy) {
                assert(allocation.limit_price <= parent.limit_price);
            } else {
                assert(allocation.limit_price >= parent.limit_price);
            }
            sum += allocation.quantity.units;
        }
        assert(selected.size() <= maximum_venues);
        assert(sum == decision.allocated.units);
        assert(sum <= parent.quantity.units);
        assert(decision.allocated.units + decision.remainder.units ==
               parent.quantity.units);
    }
    return 0;
}
