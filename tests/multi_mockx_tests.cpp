#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/config.hpp"
#include "sorx/md/feed_engine.hpp"
#include "sorx/runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;

constexpr InstrumentId instrument{7U};
constexpr VenueId venue_one{1U};
constexpr VenueId venue_two{2U};

class TemporaryJournal final {
public:
    explicit TemporaryJournal(const std::string_view scenario) {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("sorx-multi-mockx-" + std::string{scenario} + "-" +
             std::to_string(stamp) + "-" +
             std::to_string(sequence.fetch_add(1U)) + ".journal");
        erase();
    }

    TemporaryJournal(const TemporaryJournal&) = delete;
    TemporaryJournal& operator=(const TemporaryJournal&) = delete;

    ~TemporaryJournal() {
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

[[nodiscard]] std::filesystem::path acceptance_configuration_path() {
    std::filesystem::path source{__FILE__};
    if (source.is_relative()) {
        source = std::filesystem::absolute(source);
    }
    const auto beside_source = source.parent_path().parent_path() /
        "configs" / "multi_mockx" / "sorx.ini";
    if (std::filesystem::exists(beside_source)) {
        return beside_source;
    }
    const auto below_working_directory = std::filesystem::current_path() /
        "configs" / "multi_mockx" / "sorx.ini";
    if (std::filesystem::exists(below_working_directory)) {
        return below_working_directory;
    }
    throw std::runtime_error("cannot locate configs/multi_mockx/sorx.ini");
}

[[nodiscard]] config::ValidatedConfiguration load_acceptance_configuration() {
    const auto parsed = config::ConfigurationLoader::load_ini_file(
        acceptance_configuration_path());
    assert(parsed);
    assert(parsed.errors.empty());
    assert(parsed.configuration.has_value());
    assert(parsed.configuration->venues.size() == 2U);
    assert(parsed.configuration->routes.size() == 2U);
    assert(parsed.configuration->core.maximum_aggressive_venues == 2U);
    assert(parsed.configuration->venues[0].id == venue_one);
    assert(parsed.configuration->venues[1].id == venue_two);
    return *parsed.configuration;
}

[[nodiscard]] RuntimeConfiguration runtime_configuration(
    const config::ValidatedConfiguration& validated,
    const std::filesystem::path& journal,
    const std::optional<std::size_t> normal_capacity = std::nullopt) {
    auto effective = validated;
    if (normal_capacity.has_value()) {
        for (auto& venue : effective.venues) {
            venue.normal_egress_capacity = *normal_capacity;
        }
        effective.route_critical_hash =
            config::compute_route_critical_hash(effective);
        effective.route_critical_hash_hex =
            config::sha256_hex(effective.route_critical_hash);
    }

    RuntimeConfiguration result;
    result.validated = std::move(effective);
    result.journal_path = journal;
    return result;
}

[[nodiscard]] const config::VenueConfiguration& configured_venue(
    const config::ValidatedConfiguration& validated,
    const VenueId venue) {
    const auto found = std::find_if(
        validated.venues.begin(),
        validated.venues.end(),
        [venue](const auto& item) { return item.id == venue; });
    assert(found != validated.venues.end());
    return *found;
}

std::shared_ptr<const WireIdentityFactory> mockx_wire_factory(
    const config::ValidatedConfiguration& validated) {
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

struct FeedFixture final {
    VenueId venue{};
    md::FeedEngine engine;

    FeedFixture(
        const config::ValidatedConfiguration& validated,
        const VenueId venue_id)
        : venue(venue_id),
          engine([&] {
              const auto& configured = configured_venue(validated, venue_id);
              return md::FeedEngineConfig{
                  configured.id,
                  configured.market_data_channel_id,
                  configured.market_data_profile_id,
                  md::kMaxDepthLevels,
                  md::RecoveryStrategy::FreshSnapshot,
              };
          }()) {}

    [[nodiscard]] md::EngineUpdate snapshot(
        const Price bid,
        const Quantity bid_quantity,
        const Price ask,
        const Quantity ask_quantity,
        const std::uint64_t next_sequence,
        const std::uint64_t receive_time) {
        const std::uint64_t reference_base =
            static_cast<std::uint64_t>(venue.value()) * 10'000U +
            engine.recovery_generation() * 100U;
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
        return engine.apply_snapshot(md::SnapshotImage{
            events,
            next_sequence,
            receive_time,
            receive_time + 1U,
            1'000U + venue.value(),
            next_sequence,
        });
    }

    [[nodiscard]] md::EngineUpdate make_live(
        const Price bid,
        const Quantity bid_quantity,
        const Price ask,
        const Quantity ask_quantity,
        const std::uint64_t next_sequence,
        const std::uint64_t receive_time) {
        const auto snap = snapshot(
            bid,
            bid_quantity,
            ask,
            ask_quantity,
            next_sequence,
            receive_time);
        assert(snap.state == MarketDataState::Stitching);
        assert(snap.publications.size() == 1U);
        const auto live = engine.complete_stitch(receive_time + 2U);
        assert(live.healthy());
        assert(live.publications.size() == 1U);
        return live;
    }

    [[nodiscard]] md::EngineUpdate inject_gap(
        const std::uint64_t session_key,
        const std::uint64_t receive_time) {
        const std::vector<md::FeedEvent> event{
            md::KnownBookNeutralEvent{static_cast<std::int64_t>(receive_time)}};
        const auto update = engine.apply_packet(md::PacketFrame{
            session_key,
            engine.next_sequence() + 1U,
            1U,
            event,
            receive_time,
            receive_time + 1U,
        });
        assert(update.state == MarketDataState::Recovering);
        assert(update.error == md::FeedError::SequenceGap);
        assert(update.recovery == md::RecoveryAction::RequestFreshSnapshot);
        assert(update.state_changed);
        assert(update.publications.size() == 1U);
        assert(update.publications.front().feed_state ==
               MarketDataState::Recovering);
        return update;
    }
};

[[nodiscard]] CandidateRuntimeView runtime_view(
    md::BookView book,
    const OrderEntryState order_entry = OrderEntryState::Ready) {
    const bool live = book.feed_state == MarketDataState::Live;
    return CandidateRuntimeView{
        book.venue,
        book.view_generation,
        std::move(book),
        live ? MarketDataState::Live : MarketDataState::Recovering,
        order_entry,
        EgressState::Ready,
        InstrumentState::Trading,
        live,
        true,
        true,
    };
}

void publish_book(
    SorRuntime& runtime,
    const md::EngineUpdate& update,
    const OrderEntryState order_entry = OrderEntryState::Ready,
    const std::uint64_t restored_generation = 0U) {
    assert(update.publications.size() == 1U);
    md::BookView book = update.publications.front();
    if (restored_generation != 0U) {
        book.recovery_generation = restored_generation;
        book.view_generation = restored_generation;
    }
    assert(runtime.publish(RuntimeViewPublishedEvent{
        instrument,
        runtime_view(std::move(book), order_entry),
    }));
}

void prime_runtime(
    SorRuntime& runtime,
    const md::EngineUpdate& first,
    const md::EngineUpdate& second,
    const std::uint64_t first_session_epoch = 1U,
    const std::uint64_t second_session_epoch = 1U) {
    assert(runtime.publish(OrderEntrySessionEvent{
        venue_one, first_session_epoch, OrderEntryState::Ready}));
    assert(runtime.publish(OrderEntrySessionEvent{
        venue_two, second_session_epoch, OrderEntryState::Ready}));
    publish_book(runtime, first);
    publish_book(runtime, second);
    assert(runtime.complete_recovery());
    assert(!runtime.recovery_pending());
}

struct ReportSink final : IParentReportSink {
    std::vector<ParentReport> reports;

    void on_parent_report(const ParentReport& report) override {
        reports.push_back(report);
    }
};

[[nodiscard]] const ParentSnapshot& find_parent(
    const SorSnapshot& snapshot,
    const ParentOrderId parent) {
    const auto found = std::find_if(
        snapshot.parents.begin(),
        snapshot.parents.end(),
        [parent](const ParentSnapshot& item) {
            return item.order.id == parent;
        });
    assert(found != snapshot.parents.end());
    return *found;
}

[[nodiscard]] std::vector<const ChildSnapshot*> children_for(
    const SorSnapshot& snapshot,
    const ParentOrderId parent) {
    std::vector<const ChildSnapshot*> result;
    for (const auto& child : snapshot.children) {
        if (child.parent == parent) {
            result.push_back(&child);
        }
    }
    return result;
}

[[nodiscard]] const ChildSnapshot& child_at_venue(
    const SorSnapshot& snapshot,
    const ParentOrderId parent,
    const VenueId venue) {
    const auto children = children_for(snapshot, parent);
    const auto found = std::find_if(
        children.begin(),
        children.end(),
        [venue](const ChildSnapshot* item) {
            return item->venue == venue;
        });
    assert(found != children.end());
    return **found;
}

[[nodiscard]] NewOrderIntent poll_new(
    SorRuntime& runtime,
    const VenueId venue) {
    const auto effect = runtime.poll_egress(venue);
    assert(effect.has_value());
    assert(std::holds_alternative<DispatchNewEffect>(*effect));
    return std::get<DispatchNewEffect>(*effect).intent;
}

[[nodiscard]] CancelOrderIntent poll_cancel(
    SorRuntime& runtime,
    const VenueId venue) {
    const auto effect = runtime.poll_egress(venue);
    assert(effect.has_value());
    assert(std::holds_alternative<DispatchCancelEffect>(*effect));
    return std::get<DispatchCancelEffect>(*effect).intent;
}

void expect_exact_new_message(
    const MockXFix44Profile& profile,
    const NewOrderIntent& intent,
    const std::string_view expected_wire_id,
    const std::string_view expected_symbol,
    const std::string_view expected_quantity,
    const std::string_view expected_price,
    const std::string_view expected_tif) {
    assert(intent.wire_order_id == expected_wire_id);
    assert(intent.wire_symbol == expected_symbol);
    const auto built = profile.make_new_order(intent);
    assert(built.ok);
    FixMessage expected;
    expected.message_type = "D";
    expected.fields = {
        {11, std::string{expected_wire_id}},
        {55, std::string{expected_symbol}},
        {54, intent.side == Side::Buy ? "1" : "2"},
        {38, std::string{expected_quantity}},
        {40, "2"},
        {44, std::string{expected_price}},
        {59, std::string{expected_tif}},
    };
    assert(built.message == expected);
}

void expect_exact_cancel_message(
    const MockXFix44Profile& profile,
    const CancelOrderIntent& intent) {
    const auto built = profile.make_cancel(intent);
    assert(built.ok);
    FixMessage expected;
    expected.message_type = "F";
    expected.fields = {
        {11, intent.cancel_wire_order_id},
        {41, intent.original_wire_order_id},
        {55, intent.wire_symbol},
        {54, intent.side == Side::Buy ? "1" : "2"},
        {38, std::to_string(intent.original_quantity.units)},
    };
    assert(built.message == expected);
}

[[nodiscard]] NormalizedExecutionReport normalize_acknowledgement(
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
    assert(normalized.ok);
    assert(normalized.report.kind == ExecutionKind::Acknowledged);
    return normalized.report;
}

[[nodiscard]] NormalizedExecutionReport normalize_trade(
    const MockXFix44Profile& profile,
    const NewOrderIntent& intent,
    const std::string& execution_id,
    const Quantity last,
    const Quantity cumulative,
    const Quantity leaves,
    const std::string_view price,
    const bool filled) {
    const std::vector<FixField> fields{
        {11, intent.wire_order_id},
        {17, execution_id},
        {150, "F"},
        {39, filled ? "2" : "1"},
        {14, std::to_string(cumulative.units)},
        {151, std::to_string(leaves.units)},
        {32, std::to_string(last.units)},
        {31, std::string{price}},
    };
    const auto normalized = profile.normalize_execution_report(
        intent.venue, intent.session_epoch, fields);
    assert(normalized.ok);
    assert(normalized.report.kind == ExecutionKind::Trade);
    return normalized.report;
}

[[nodiscard]] NormalizedExecutionReport normalize_canceled(
    const MockXFix44Profile& profile,
    const NewOrderIntent& original,
    const std::string& execution_id,
    const Quantity cumulative,
    const Quantity leaves) {
    const std::vector<FixField> fields{
        {11, original.wire_order_id},
        {17, execution_id},
        {150, "4"},
        {39, "4"},
        {14, std::to_string(cumulative.units)},
        {151, std::to_string(leaves.units)},
    };
    const auto normalized = profile.normalize_execution_report(
        original.venue, original.session_epoch, fields);
    assert(normalized.ok);
    assert(normalized.report.kind == ExecutionKind::Canceled);
    return normalized.report;
}

void accept_locally(
    SorRuntime& runtime,
    const NewOrderIntent& intent) {
    LocalDispatchResult accepted;
    accepted.child = intent.child;
    accepted.state = LocalDispatchState::AcceptedLocally;
    accepted.detail = "accepted by deterministic MockX session";
    accepted.kind = DispatchAttemptKind::New;
    accepted.attempt_id = intent.dispatch_attempt_id;
    accepted.venue = intent.venue;
    accepted.session_epoch = intent.session_epoch;
    accepted.wire_order_id = intent.wire_order_id;
    assert(runtime.publish(LocalDispatchEvent{std::move(accepted)}));
}

void accept_and_acknowledge(
    SorRuntime& runtime,
    const MockXFix44Profile& profile,
    const NewOrderIntent& intent,
    const std::string& execution_id) {
    accept_locally(runtime, intent);
    assert(runtime.publish(ExecutionReportEvent{
        normalize_acknowledgement(profile, intent, execution_id)}));
}

[[nodiscard]] ParentOrder aggressive_order(
    const std::uint64_t id,
    const std::uint64_t quantity,
    const std::int64_t limit) {
    return ParentOrder{
        ParentOrderId{id},
        instrument,
        Side::Buy,
        Quantity{quantity},
        Price{limit},
        RoutingIntent::AggressiveIOC,
    };
}

[[nodiscard]] ParentOrder passive_order(
    const std::uint64_t id,
    const std::uint64_t quantity,
    const std::int64_t limit) {
    return ParentOrder{
        ParentOrderId{id},
        instrument,
        Side::Buy,
        Quantity{quantity},
        Price{limit},
        RoutingIntent::PassiveGTC,
    };
}

void cross_venue_ioc_and_feed_isolation(
    const config::ValidatedConfiguration& validated) {
    TemporaryJournal journal{"cross-venue"};
    FeedFixture first{validated, venue_one};
    FeedFixture second{validated, venue_two};
    const auto first_live = first.make_live(
        Price{99}, Quantity{20}, Price{100}, Quantity{4}, 10U, 100U);
    const auto second_live = second.make_live(
        Price{98}, Quantity{20}, Price{101}, Quantity{8}, 10U, 100U);

    SorRuntime runtime{
        runtime_configuration(validated, journal.path()),
        mockx_wire_factory(validated)};
    ReportSink sink;
    runtime.set_report_sink(sink);
    prime_runtime(runtime, first_live, second_live);

    constexpr ParentOrderId parent_id{1U};
    const auto receipt = runtime.submit(aggressive_order(1U, 10U, 101));
    assert(receipt.accepted());
    auto snapshot = runtime.inspect();
    const auto children = children_for(snapshot, parent_id);
    assert(children.size() == 2U);
    const auto& first_child = child_at_venue(snapshot, parent_id, venue_one);
    const auto& second_child = child_at_venue(snapshot, parent_id, venue_two);
    assert(first_child.ordered == Quantity{4U});
    assert(first_child.limit_price == Price{100});
    assert(second_child.ordered == Quantity{6U});
    assert(second_child.limit_price == Price{101});
    assert(first_child.risk_reservation == first_child.ordered);
    assert(second_child.risk_reservation == second_child.ordered);

    MockXFix44Profile profile{MockXFix44Config{2U, 0U, 32U, "SX"}};
    const NewOrderIntent first_new = poll_new(runtime, venue_one);
    const NewOrderIntent second_new = poll_new(runtime, venue_two);
    expect_exact_new_message(
        profile, first_new, "SXN1-1-1", "ALPHA-A", "4", "1.00", "3");
    expect_exact_new_message(
        profile, second_new, "SXN1-1-2", "ALPHA-B", "6", "1.01", "3");
    accept_and_acknowledge(runtime, profile, first_new, "V1-ACK-1");
    accept_and_acknowledge(runtime, profile, second_new, "V2-ACK-1");

    struct TimedReport final {
        std::uint64_t logical_time_ns{};
        VenueId venue{};
        NormalizedExecutionReport report;
    };
    std::vector<TimedReport> same_time{
        TimedReport{
            1'000U,
            venue_two,
            normalize_trade(
                profile,
                second_new,
                "V2-FILL-1",
                Quantity{6U},
                Quantity{6U},
                Quantity{},
                "1.01",
                true)},
        TimedReport{
            1'000U,
            venue_one,
            normalize_trade(
                profile,
                first_new,
                "V1-FILL-1",
                Quantity{4U},
                Quantity{4U},
                Quantity{},
                "1.00",
                true)},
    };
    std::stable_sort(
        same_time.begin(),
        same_time.end(),
        [](const TimedReport& left, const TimedReport& right) {
            return std::tuple{
                       left.logical_time_ns,
                       left.venue.value(),
                       left.report.execution_id} <
                   std::tuple{
                       right.logical_time_ns,
                       right.venue.value(),
                       right.report.execution_id};
        });
    assert(same_time[0].venue == venue_one);
    assert(same_time[1].venue == venue_two);
    for (const auto& item : same_time) {
        assert(runtime.publish(ExecutionReportEvent{item.report}));
    }
    // The exact same venue/epoch/ExecID is durable but accounting-idempotent.
    assert(runtime.publish(ExecutionReportEvent{same_time.front().report}));

    snapshot = runtime.inspect();
    const auto& completed = find_parent(snapshot, parent_id);
    assert(completed.state == ParentState::Filled);
    assert(completed.cumulative_filled == Quantity{10U});
    const auto completed_children = children_for(snapshot, parent_id);
    std::uint64_t total_ordered = 0U;
    std::uint64_t total_filled = 0U;
    std::uint64_t total_reserved = 0U;
    for (const ChildSnapshot* child : completed_children) {
        total_ordered += child->ordered.units;
        total_filled += child->filled.units;
        total_reserved += child->risk_reservation.units;
        assert(child->state == ChildState::Filled);
    }
    assert(total_ordered == 10U);
    assert(total_filled == 10U);
    assert(total_reserved == 0U);
    assert(children_for(snapshot, parent_id).size() == 2U);
    runtime.clear_report_sink();
    assert(!sink.reports.empty());

    const auto second_generation = second.engine.publication_generation();
    const auto withdrawn = first.inject_gap(1'001U, 200U);
    publish_book(runtime, withdrawn);
    assert(first.engine.state() == MarketDataState::Recovering);
    assert(second.engine.state() == MarketDataState::Live);
    assert(second.engine.publication_generation() == second_generation);

    const auto isolated_trace = runtime.trace(aggressive_order(2U, 3U, 101));
    assert(isolated_trace.hard_risk_accepted);
    assert(isolated_trace.decision.allocations.size() == 1U);
    assert(isolated_trace.decision.allocations.front().venue == venue_two);
    assert(runtime.submit(aggressive_order(2U, 3U, 101)).accepted());
    snapshot = runtime.inspect();
    const auto isolated_children = children_for(snapshot, ParentOrderId{2U});
    assert(isolated_children.size() == 1U);
    assert(isolated_children.front()->venue == venue_two);
    (void)poll_new(runtime, venue_two);

    const auto recovered = first.make_live(
        Price{99}, Quantity{20}, Price{100}, Quantity{4}, 20U, 300U);
    publish_book(runtime, recovered);
    assert(first.engine.state() == MarketDataState::Live);
    const auto restored_trace = runtime.trace(aggressive_order(3U, 5U, 101));
    assert(restored_trace.decision.allocations.size() == 2U);
    assert(restored_trace.decision.allocations[0].venue == venue_one);
    assert(restored_trace.decision.allocations[1].venue == venue_two);
}

void passive_sessions_replace_and_egress_priority(
    const config::ValidatedConfiguration& validated) {
    TemporaryJournal journal{"passive"};
    FeedFixture first{validated, venue_one};
    FeedFixture second{validated, venue_two};
    const auto first_live = first.make_live(
        Price{97}, Quantity{20}, Price{100}, Quantity{20}, 10U, 100U);
    const auto second_live = second.make_live(
        Price{96}, Quantity{20}, Price{101}, Quantity{20}, 10U, 100U);
    SorRuntime runtime{
        runtime_configuration(validated, journal.path(), std::size_t{1U}),
        mockx_wire_factory(validated)};
    prime_runtime(runtime, first_live, second_live);
    MockXFix44Profile profile{MockXFix44Config{2U, 0U, 32U, "SX"}};

    assert(runtime.publish(OrderEntrySessionEvent{
        venue_two, 1U, OrderEntryState::Disconnected}));
    const auto disconnected_trace = runtime.trace(passive_order(10U, 5U, 99));
    assert(disconnected_trace.decision.allocations.size() == 1U);
    assert(disconnected_trace.decision.allocations.front().venue == venue_one);
    assert(runtime.publish(OrderEntrySessionEvent{
        venue_two, 2U, OrderEntryState::Ready}));
    const auto reconnected_trace = runtime.trace(passive_order(10U, 5U, 99));
    assert(reconnected_trace.decision.allocations.size() == 1U);
    assert(reconnected_trace.decision.allocations.front().venue == venue_two);

    constexpr ParentOrderId resting_parent{10U};
    assert(runtime.submit(passive_order(10U, 5U, 99)).accepted());
    const NewOrderIntent resting_new = poll_new(runtime, venue_two);
    assert(resting_new.session_epoch == 2U);
    expect_exact_new_message(
        profile, resting_new, "SXN2-1-1", "ALPHA-B", "5", "0.99", "1");
    accept_and_acknowledge(runtime, profile, resting_new, "REST-ACK");
    auto snapshot = runtime.inspect();
    assert(child_at_venue(snapshot, resting_parent, venue_two).state ==
           ChildState::Working);

    const auto partial = normalize_trade(
        profile,
        resting_new,
        "REST-FILL-1",
        Quantity{2U},
        Quantity{2U},
        Quantity{3U},
        "0.99",
        false);
    assert(runtime.publish(ExecutionReportEvent{partial}));
    assert(runtime.publish(ExecutionReportEvent{partial}));
    snapshot = runtime.inspect();
    assert(find_parent(snapshot, resting_parent).cumulative_filled == Quantity{2U});
    assert(children_for(snapshot, resting_parent).size() == 1U);

    assert(runtime.cancel(resting_parent).accepted());
    const CancelOrderIntent resting_cancel = poll_cancel(runtime, venue_two);
    expect_exact_cancel_message(profile, resting_cancel);
    assert(resting_cancel.original_wire_order_id == "SXN2-1-1");
    LocalDispatchResult cancel_accepted;
    cancel_accepted.child = resting_cancel.child;
    cancel_accepted.state = LocalDispatchState::AcceptedLocally;
    cancel_accepted.detail = "cancel accepted locally";
    cancel_accepted.kind = DispatchAttemptKind::Cancel;
    cancel_accepted.attempt_id = resting_cancel.dispatch_attempt_id;
    cancel_accepted.venue = resting_cancel.venue;
    cancel_accepted.session_epoch = resting_cancel.session_epoch;
    cancel_accepted.wire_order_id =
        resting_cancel.cancel_wire_order_id;
    assert(runtime.publish(
        LocalDispatchEvent{std::move(cancel_accepted)}));
    assert(runtime.publish(ExecutionReportEvent{normalize_canceled(
        profile,
        resting_new,
        "REST-CANCELED",
        Quantity{2U},
        Quantity{3U})}));
    snapshot = runtime.inspect();
    const auto& canceled_parent = find_parent(snapshot, resting_parent);
    const auto& canceled_child = child_at_venue(
        snapshot, resting_parent, venue_two);
    assert(canceled_parent.state == ParentState::Canceled);
    assert(canceled_parent.cumulative_filled == Quantity{2U});
    assert(canceled_child.ordered == Quantity{5U});
    assert(canceled_child.filled == Quantity{2U});
    assert(canceled_child.risk_reservation == Quantity{});

    constexpr ParentOrderId replaced_parent{20U};
    assert(runtime.submit(passive_order(20U, 6U, 99)).accepted());
    const NewOrderIntent old_revision = poll_new(runtime, venue_two);
    assert(old_revision.wire_order_id == "SXN2-1-2");
    accept_and_acknowledge(runtime, profile, old_revision, "REPLACE-OLD-ACK");
    assert(runtime.replace(ParentReplace{
        replaced_parent, Quantity{8U}, Price{98}}).accepted());
    const CancelOrderIntent replace_cancel = poll_cancel(runtime, venue_two);
    expect_exact_cancel_message(profile, replace_cancel);
    assert(replace_cancel.original_wire_order_id == "SXN2-1-2");
    assert(runtime.publish(ExecutionReportEvent{normalize_canceled(
        profile,
        old_revision,
        "REPLACE-OLD-CANCELED",
        Quantity{},
        Quantity{6U})}));

    snapshot = runtime.inspect();
    const auto& replacement_parent = find_parent(snapshot, replaced_parent);
    assert(replacement_parent.revision == 2U);
    assert(replacement_parent.requested_total == Quantity{8U});
    assert(replacement_parent.order.quantity == Quantity{8U});
    assert(replacement_parent.state == ParentState::Active);
    const auto replacement_children = children_for(snapshot, replaced_parent);
    assert(replacement_children.size() == 2U);
    assert(replacement_children[0]->state == ChildState::Canceled);
    assert(replacement_children[1]->state == ChildState::Prepared);
    const NewOrderIntent new_revision = poll_new(runtime, venue_two);
    expect_exact_new_message(
        profile, new_revision, "SXN2-2-3", "ALPHA-B", "8", "0.98", "1");
    accept_and_acknowledge(runtime, profile, new_revision, "REPLACE-NEW-ACK");
    assert(runtime.publish(ExecutionReportEvent{normalize_trade(
        profile,
        new_revision,
        "REPLACE-NEW-FILL",
        Quantity{8U},
        Quantity{8U},
        Quantity{},
        "0.98",
        true)}));
    snapshot = runtime.inspect();
    assert(find_parent(snapshot, replaced_parent).state == ParentState::Filled);

    constexpr ParentOrderId working_parent{30U};
    constexpr ParentOrderId queued_parent{31U};
    assert(runtime.submit(passive_order(30U, 3U, 99)).accepted());
    const NewOrderIntent working = poll_new(runtime, venue_two);
    accept_and_acknowledge(runtime, profile, working, "PRIORITY-WORKING-ACK");
    assert(runtime.submit(passive_order(31U, 2U, 99)).accepted());
    // The one-slot normal queue is now full, but the independent cancel class
    // remains available and must drain first.
    assert(runtime.cancel(working_parent).accepted());
    const CancelOrderIntent prioritized_cancel = poll_cancel(runtime, venue_two);
    assert(prioritized_cancel.parent == working_parent);
    const NewOrderIntent queued_new = poll_new(runtime, venue_two);
    assert(queued_new.parent == queued_parent);
    assert(!runtime.failed());
    assert(!runtime.poll_egress(venue_one).has_value());
}

void restart_reconciliation_and_ambiguity(
    const config::ValidatedConfiguration& validated) {
    TemporaryJournal journal{"reconcile"};
    FeedFixture first{validated, venue_one};
    FeedFixture second{validated, venue_two};
    const auto first_live = first.make_live(
        Price{99}, Quantity{20}, Price{100}, Quantity{10}, 10U, 100U);
    const auto second_live = second.make_live(
        Price{98}, Quantity{20}, Price{101}, Quantity{10}, 10U, 100U);
    MockXFix44Profile profile{MockXFix44Config{2U, 0U, 32U, "SX"}};

    ChildOrderId prepared_child{};
    std::string prepared_wire_id;
    {
        SorRuntime seeded{
            runtime_configuration(validated, journal.path()),
            mockx_wire_factory(validated)};
        prime_runtime(seeded, first_live, second_live);
        assert(seeded.submit(aggressive_order(100U, 3U, 100)).accepted());
        const auto snapshot = seeded.inspect();
        const auto children = children_for(snapshot, ParentOrderId{100U});
        assert(children.size() == 1U);
        prepared_child = children.front()->id;
        prepared_wire_id = children.front()->wire_order_id;
        assert(children.front()->state == ChildState::Prepared);
    }

    ChildOrderId ambiguous_child{};
    std::string ambiguous_wire_id;
    {
        SorRuntime recovered{
            runtime_configuration(validated, journal.path()),
            mockx_wire_factory(validated)};
        assert(recovered.recovery_pending());
        assert(recovered.publish(OrderEntrySessionEvent{
            venue_one, 1U, OrderEntryState::Ready}));
        assert(recovered.publish(OrderEntrySessionEvent{
            venue_two, 1U, OrderEntryState::Ready}));
        publish_book(
            recovered, first_live, OrderEntryState::Ready, 101U);
        publish_book(
            recovered, second_live, OrderEntryState::Ready, 102U);
        assert(!recovered.complete_recovery());
        assert(recovered.publish(ReconciliationEvent{
            prepared_child,
            ReconciliationDisposition::DefinitelyUnsent,
            Quantity{},
        }));
        const NewOrderIntent retry = poll_new(recovered, venue_one);
        assert(retry.child == prepared_child);
        assert(retry.wire_order_id == prepared_wire_id);
        accept_and_acknowledge(recovered, profile, retry, "RECOVERY-ACK");
        assert(recovered.complete_recovery());

        assert(recovered.submit(aggressive_order(101U, 2U, 100)).accepted());
        const NewOrderIntent uncertain = poll_new(recovered, venue_one);
        // Local transport acceptance followed by disconnect without a
        // definitive venue execution report is ambiguous.
        accept_locally(recovered, uncertain);
        ambiguous_child = uncertain.child;
        ambiguous_wire_id = uncertain.wire_order_id;
        assert(recovered.publish(OrderEntrySessionEvent{
            venue_one, 2U, OrderEntryState::Disconnected}));
        assert(recovered.publish(DispatchTimeoutEvent{
            ambiguous_child,
            "FIX disconnected after local acceptance",
        }));

        const auto snapshot = recovered.inspect();
        const auto ambiguous_children = children_for(
            snapshot, ParentOrderId{101U});
        assert(ambiguous_children.size() == 1U);
        assert(ambiguous_children.front()->state == ChildState::Ambiguous);
        assert(ambiguous_children.front()->risk_reservation ==
               ambiguous_children.front()->ordered);
        assert(find_parent(snapshot, ParentOrderId{101U}).state ==
               ParentState::Suspended);
        assert(!recovered.poll_egress(venue_two).has_value());
        assert(snapshot.children.size() == 2U);
    }

    {
        SorRuntime restarted{
            runtime_configuration(validated, journal.path()),
            mockx_wire_factory(validated)};
        assert(restarted.recovery_pending());
        assert(restarted.publish(OrderEntrySessionEvent{
            venue_one, 3U, OrderEntryState::Ready}));
        assert(restarted.publish(OrderEntrySessionEvent{
            venue_two, 2U, OrderEntryState::Ready}));
        publish_book(
            restarted, first_live, OrderEntryState::Ready, 201U);
        publish_book(
            restarted, second_live, OrderEntryState::Ready, 202U);
        assert(restarted.publish(ReconciliationEvent{
            prepared_child,
            ReconciliationDisposition::Working,
            Quantity{},
        }));
        assert(restarted.publish(ReconciliationEvent{
            ambiguous_child,
            ReconciliationDisposition::Unresolved,
            Quantity{},
        }));
        assert(!restarted.complete_recovery());
        const auto snapshot = restarted.inspect();
        const auto ambiguous_children = children_for(
            snapshot, ParentOrderId{101U});
        assert(ambiguous_children.size() == 1U);
        assert(ambiguous_children.front()->wire_order_id == ambiguous_wire_id);
        assert(ambiguous_children.front()->state == ChildState::Ambiguous);
        assert(ambiguous_children.front()->risk_reservation ==
               ambiguous_children.front()->ordered);
        assert(find_parent(snapshot, ParentOrderId{101U}).cumulative_filled ==
               Quantity{});
        assert(snapshot.children.size() == 2U);
        assert(!restarted.poll_egress(venue_one).has_value());
        assert(!restarted.poll_egress(venue_two).has_value());
    }
}

}  // namespace

int main() {
    const auto validated = load_acceptance_configuration();
    cross_venue_ioc_and_feed_isolation(validated);
    passive_sessions_replace_and_egress_priority(validated);
    restart_reconciliation_and_ambiguity(validated);
    return 0;
}

