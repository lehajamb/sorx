#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/config.hpp"
#include "sorx/md/feed_engine.hpp"
#include "sorx/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;

constexpr InstrumentId instrument{7U};
constexpr VenueId venue_one{1U};
constexpr VenueId venue_two{2U};

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

class DemoJournal final {
public:
    DemoJournal() {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("sorx-multi-mockx-demo-" + std::to_string(stamp) + ".journal");
        erase();
    }

    DemoJournal(const DemoJournal&) = delete;
    DemoJournal& operator=(const DemoJournal&) = delete;

    ~DemoJournal() {
        erase();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    void erase() noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".lock", ignored);
    }

    std::filesystem::path path_;
};

[[nodiscard]] const config::VenueConfiguration& configured_venue(
    const config::ValidatedConfiguration& configuration,
    const VenueId venue) {
    const auto found = std::find_if(
        configuration.venues.begin(),
        configuration.venues.end(),
        [venue](const auto& item) { return item.id == venue; });
    require(found != configuration.venues.end(), "configured venue is missing");
    return *found;
}

[[nodiscard]] RuntimeConfiguration runtime_configuration(
    const config::ValidatedConfiguration& validated,
    const std::filesystem::path& journal) {
    RuntimeConfiguration result;
    result.validated = validated;
    result.journal_path = journal;
    return result;
}

[[nodiscard]] std::shared_ptr<const WireIdentityFactory>
mockx_wire_factory(const config::ValidatedConfiguration& validated) {
    std::vector<MockXFix44WireIdentityBinding> bindings;
    bindings.reserve(validated.venues.size());
    for (const auto& venue : validated.venues) {
        bindings.push_back(MockXFix44WireIdentityBinding{
            WireProfileIdentity{
                venue.id,
                venue.order_entry_profile_id,
                venue.order_entry_profile,
                venue.order_entry_session_identity,
                venue.dictionary_identity,
                venue.sender_comp_id,
                venue.target_comp_id,
            },
            MockXFix44Config{2U, 0U, 32U, "SX"},
        });
    }
    return std::make_shared<MockXFix44WireIdentityFactory>(
        std::move(bindings));
}

[[nodiscard]] md::EngineUpdate make_live_feed(
    md::FeedEngine& feed,
    const VenueId venue,
    const Price bid,
    const Quantity bid_quantity,
    const Price ask,
    const Quantity ask_quantity,
    const std::uint64_t receive_time) {
    const std::uint64_t reference_base =
        static_cast<std::uint64_t>(venue.value()) * 10'000U;
    const auto exchange_time = static_cast<std::int64_t>(receive_time);
    const std::vector<md::FeedEvent> events{
        md::InstrumentDirectoryEvent{
            instrument.value(), instrument, exchange_time},
        md::TradingStateEvent{
            instrument.value(),
            instrument,
            InstrumentState::Trading,
            exchange_time + 1},
        md::AddOrderEvent{
            reference_base + 1U,
            instrument.value(),
            instrument,
            Side::Buy,
            bid_quantity,
            bid,
            exchange_time + 2},
        md::AddOrderEvent{
            reference_base + 2U,
            instrument.value(),
            instrument,
            Side::Sell,
            ask_quantity,
            ask,
            exchange_time + 3},
    };
    const auto snapshot = feed.apply_snapshot(md::SnapshotImage{
        events,
        10U,
        receive_time,
        receive_time + 1U,
        1'000U + venue.value(),
        10U,
    });
    require(
        snapshot.state == MarketDataState::Stitching,
        "fresh snapshot did not enter STITCHING");
    const auto live = feed.complete_stitch(receive_time + 2U);
    require(live.healthy(), "snapshot stitch did not enter LIVE");
    require(live.publications.size() == 1U, "feed did not publish one book");
    return live;
}

[[nodiscard]] CandidateRuntimeView runtime_view(const md::BookView& book) {
    return CandidateRuntimeView{
        book.venue,
        book.view_generation,
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

[[nodiscard]] std::string render_fix(const FixMessage& message) {
    std::string result = "35=" + message.message_type;
    for (const auto& field : message.fields) {
        result += "|" + std::to_string(field.tag) + "=" + field.value;
    }
    return result;
}

[[nodiscard]] NormalizedExecutionReport acknowledgement(
    const MockXFix44Profile& profile,
    const NewOrderIntent& intent,
    const std::string& execution_id) {
    const std::vector<FixField> fields{
        {11, intent.wire_order_id},
        {17, execution_id},
        {150, "0"},
        {39, "0"},
        {14, "0"},
        {151, std::to_string(intent.quantity.units)},
    };
    const auto normalized = profile.normalize_execution_report(
        intent.venue, intent.session_epoch, fields);
    require(normalized.ok, "MockX acknowledgement did not normalize");
    return normalized.report;
}

[[nodiscard]] NormalizedExecutionReport fill(
    const MockXFix44Profile& profile,
    const NewOrderIntent& intent,
    const std::string& execution_id,
    const std::string_view price) {
    const std::string quantity = std::to_string(intent.quantity.units);
    const std::vector<FixField> fields{
        {11, intent.wire_order_id},
        {17, execution_id},
        {150, "F"},
        {39, "2"},
        {14, quantity},
        {151, "0"},
        {32, quantity},
        {31, std::string{price}},
    };
    const auto normalized = profile.normalize_execution_report(
        intent.venue, intent.session_epoch, fields);
    require(normalized.ok, "MockX fill did not normalize");
    return normalized.report;
}

[[nodiscard]] const ParentSnapshot& parent(
    const SorSnapshot& snapshot,
    const ParentOrderId id) {
    const auto found = std::find_if(
        snapshot.parents.begin(),
        snapshot.parents.end(),
        [id](const ParentSnapshot& item) { return item.order.id == id; });
    require(found != snapshot.parents.end(), "parent is missing from snapshot");
    return *found;
}

int run(const std::filesystem::path& configuration_path) {
    const auto parsed = config::ConfigurationLoader::load_ini_file(
        configuration_path);
    if (!parsed) {
        for (const auto& error : parsed.errors) {
            std::cerr << "config line " << error.line << ": "
                      << error.message << '\n';
        }
        return 2;
    }
    const auto& validated = *parsed.configuration;
    require(validated.venues.size() == 2U, "demo requires exactly two venues");
    require(validated.routes.size() == 2U, "demo requires exactly two routes");

    const auto& first_config = configured_venue(validated, venue_one);
    const auto& second_config = configured_venue(validated, venue_two);
    md::FeedEngine first_feed{md::FeedEngineConfig{
        first_config.id,
        first_config.market_data_channel_id,
        first_config.market_data_profile_id,
        md::kMaxDepthLevels,
        md::RecoveryStrategy::FreshSnapshot,
    }};
    md::FeedEngine second_feed{md::FeedEngineConfig{
        second_config.id,
        second_config.market_data_channel_id,
        second_config.market_data_profile_id,
        md::kMaxDepthLevels,
        md::RecoveryStrategy::FreshSnapshot,
    }};
    const auto first_live = make_live_feed(
        first_feed,
        venue_one,
        Price{99},
        Quantity{20U},
        Price{100},
        Quantity{4U},
        100U);
    const auto second_live = make_live_feed(
        second_feed,
        venue_two,
        Price{98},
        Quantity{20U},
        Price{101},
        Quantity{6U},
        100U);

    DemoJournal files;
    SorRuntime runtime{
        runtime_configuration(validated, files.path()),
        mockx_wire_factory(validated)};
    require(runtime.publish(OrderEntrySessionEvent{
        venue_one, 1U, OrderEntryState::Ready}), "venue 1 FIX session failed");
    require(runtime.publish(OrderEntrySessionEvent{
        venue_two, 1U, OrderEntryState::Ready}), "venue 2 FIX session failed");
    require(runtime.publish(RuntimeViewPublishedEvent{
        instrument, runtime_view(first_live.publications.front())}),
        "venue 1 book publication failed");
    require(runtime.publish(RuntimeViewPublishedEvent{
        instrument, runtime_view(second_live.publications.front())}),
        "venue 2 book publication failed");
    require(runtime.complete_recovery(), "runtime recovery gate did not open");

    std::cout << "FEED venue=1 snapshot=FRESH state=LIVE ask=100x4\n";
    std::cout << "FEED venue=2 snapshot=FRESH state=LIVE ask=101x6\n";

    constexpr ParentOrderId parent_id{1U};
    const ParentOrder order{
        parent_id,
        instrument,
        Side::Buy,
        Quantity{10U},
        Price{101},
        RoutingIntent::AggressiveIOC,
    };
    const auto receipt = runtime.submit(order);
    require(receipt.accepted(), "parent command was rejected");
    std::cout << "COMMAND parent=1 intent=IOC qty=10 limit=101 accepted\n";

    MockXFix44Profile profile{MockXFix44Config{2U, 0U, 32U, "SX"}};
    for (const VenueId venue : {venue_one, venue_two}) {
        const auto effect = runtime.poll_egress(venue);
        require(effect.has_value(), "expected venue dispatch is missing");
        require(
            std::holds_alternative<DispatchNewEffect>(*effect),
            "expected a new-order dispatch");
        const NewOrderIntent intent =
            std::get<DispatchNewEffect>(*effect).intent;
        const auto built = profile.make_new_order(intent);
        require(built.ok, "MockX FIX D construction failed");
        std::cout << "DECISION parent=1 venue=" << venue.value()
                  << " qty=" << intent.quantity.units
                  << " limit=" << intent.limit_price.units << '\n';
        std::cout << "FIX venue=" << venue.value() << " "
                  << render_fix(built.message) << '\n';

        LocalDispatchResult accepted;
        accepted.child = intent.child;
        accepted.state = LocalDispatchState::AcceptedLocally;
        accepted.detail = "accepted by in-process MockX";
        accepted.kind = DispatchAttemptKind::New;
        accepted.attempt_id = intent.dispatch_attempt_id;
        accepted.venue = intent.venue;
        accepted.session_epoch = intent.session_epoch;
        accepted.wire_order_id = intent.wire_order_id;
        require(runtime.publish(
            LocalDispatchEvent{std::move(accepted)}),
            "local FIX acceptance failed");
        require(runtime.publish(ExecutionReportEvent{acknowledgement(
            profile,
            intent,
            "ACK-" + std::to_string(venue.value()))}),
            "acknowledgement publication failed");
        const std::string_view price =
            venue == venue_one ? std::string_view{"1.00"} :
                                 std::string_view{"1.01"};
        require(runtime.publish(ExecutionReportEvent{fill(
            profile,
            intent,
            "FILL-" + std::to_string(venue.value()),
            price)}), "fill publication failed");
    }

    const auto snapshot = runtime.inspect();
    const auto& completed = parent(snapshot, parent_id);
    require(completed.state == ParentState::Filled, "parent did not fill");
    require(
        completed.cumulative_filled == Quantity{10U},
        "parent fill conservation failed");
    require(snapshot.children.size() == 2U, "expected exactly two children");
    std::uint64_t total_filled = 0U;
    for (const auto& child : snapshot.children) {
        total_filled += child.filled.units;
        require(
            child.risk_reservation == Quantity{},
            "terminal child retained risk");
    }
    require(total_filled == 10U, "child fill conservation failed");
    std::cout << "OUTCOME parent=1 state=FILLED filled=10 children=2 risk=0\n";
    std::cout << "CHECK deterministic_conservation=PASS\n";
    return 0;
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
        if (argc > 2) {
            std::cerr << "usage: sorx_multi_mockx_demo [config.ini]\n";
            return 2;
        }
        const std::filesystem::path configuration = argc == 2 ?
            std::filesystem::path{argv[1]} :
            std::filesystem::path{"configs/multi_mockx/sorx.ini"};
        return run(configuration);
    } catch (const std::exception& error) {
        std::cerr << "multi-MockX demo failed: " << error.what() << '\n';
        return 1;
    }
}
