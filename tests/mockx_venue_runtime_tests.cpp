#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/adapters/mockx_venue_runtime.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;
using namespace sorx::adapters::quickfix;
using namespace std::chrono_literals;

[[nodiscard]] std::uint64_t process_identity() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

class TemporaryJournal final {
public:
    explicit TemporaryJournal(const std::string& label) {
        static std::atomic<std::uint64_t> next{};
        path_ = std::filesystem::temp_directory_path() /
                ("sorx-mockx-venue-" + label + "-p" +
                 std::to_string(process_identity()) + "-" +
                 std::to_string(next.fetch_add(1U)) + ".journal");
        remove();
    }

    ~TemporaryJournal() {
        remove();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    void remove() noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".lock", ignored);
        std::filesystem::remove(path_.string() + ".checkpoint", ignored);
        std::filesystem::remove(
            path_.string() + ".checkpoint.tmp", ignored);
    }

    std::filesystem::path path_;
};

template <typename Predicate>
void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

[[nodiscard]] QuickFixSessionConfig quickfix_config() {
    return QuickFixSessionConfig{
        VenueId{1U},
        41U,
        "offline-mockx-venue.cfg",
        QuickFixSessionIdentity{
            "FIX.4.4", "SORX", "MOCKX", "PRIMARY"},
        MockXFix44Config{2U, 0U, 32U, "SX"},
    };
}

[[nodiscard]] MockXMdSessionConfig market_data_config() {
    MockXMdSessionConfig value;
    value.adapter.feed.venue = VenueId{1U};
    value.adapter.feed.channel = ChannelId{1U};
    value.adapter.feed.profile = ProfileId{1U};
    value.adapter.feed.publication_depth = 8U;
    value.adapter.instruments.push_back(MockXInstrumentBinding{
        7U, "ALPHA", InstrumentId{7U}});
    value.transport_queue_capacity = 8U;
    value.coordinator_drain_batch = 8U;
    return value;
}

[[nodiscard]] RuntimeConfiguration runtime_config(
    const std::filesystem::path& journal) {
    config::ValidatedConfiguration validated;
    validated.schema_version = 1U;
    validated.account_id = "mockx-venue-runtime-test";
    validated.ingress_capacity = 128U;
    validated.core.maximum_aggressive_venues = 1U;
    validated.core.risk.generation = 1U;

    const InstrumentRiskLimit instrument_limit{
        InstrumentId{7U},
        true,
        Quantity{1'000U},
        Quantity{1'000U},
        Notional{10'000'000U},
        Notional{10'000'000U},
        Price{1U},
        Price{10'000U},
        100U,
        100U,
        Quantity{10'000U},
    };
    const VenueRiskLimit venue_limit{
        VenueId{1U}, true, Quantity{10'000U}};
    validated.core.risk.instruments.push_back(instrument_limit);
    validated.core.risk.venues.push_back(venue_limit);
    validated.instruments.push_back(config::InstrumentConfiguration{
        InstrumentId{7U}, "ALPHA", instrument_limit});
    validated.venues.push_back(config::VenueConfiguration{
        VenueId{1U},
        "MOCKX",
        AdminState::Up,
        venue_limit,
        32U,
        32U,
        16U,
        16U,
        ChannelId{1U},
        ProfileId{1U},
        ProfileId{2U},
        "MockXITCH",
        "MD-SESSION-1",
        "MockXFix44V1",
        "OE-SESSION-1",
        "FIX44.xml",
        "SORX",
        "MOCKX",
        std::nullopt,
    });
    validated.routes.push_back(RouteConfig{
        VenueId{1U},
        InstrumentId{7U},
        "MOCKX",
        "ALPHA",
        "ALPHA",
        *RationalConversion::create(1U, 1U),
        *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{1'000U},
        1U,
        1U,
        1U,
        AdminState::Up,
    });
    validated.route_critical_hash =
        config::compute_route_critical_hash(validated);
    validated.route_critical_hash_hex =
        config::sha256_hex(validated.route_critical_hash);

    RuntimeConfiguration output;
    output.validated = std::move(validated);
    output.journal_path = journal;
    output.checkpoint_interval_batches = 1U;
    return output;
}

[[nodiscard]] std::shared_ptr<const WireIdentityFactory>
wire_identity_factory() {
    WireProfileIdentity identity;
    identity.venue = VenueId{1U};
    identity.profile = ProfileId{2U};
    identity.profile_name = "MockXFix44V1";
    identity.session_identity = "OE-SESSION-1";
    identity.dictionary_identity = "FIX44.xml";
    identity.sender_comp_id = "SORX";
    identity.target_comp_id = "MOCKX";
    return std::make_shared<MockXFix44WireIdentityFactory>(
        std::vector<MockXFix44WireIdentityBinding>{
            MockXFix44WireIdentityBinding{
                std::move(identity),
                MockXFix44Config{2U, 0U, 32U, "SX"},
            },
        });
}

[[nodiscard]] MockXVenueRuntimeConfig venue_config(
    const std::size_t market_data_capacity = 64U) {
    MockXVenueRuntimeConfig output;
    output.market_data = market_data_config();
    output.order_entry = quickfix_config();
    output.market_data_callback_capacity = market_data_capacity;
    output.order_entry_callback_capacity = 64U;
    output.local_dispatch_callback_capacity = 4U;
    output.worker_drain_batch = 16U;
    output.worker_idle_wait = 1ms;
    return output;
}

[[nodiscard]] md::BookView live_book(
    const std::uint64_t generation = 1U) {
    md::BookView book;
    book.venue = VenueId{1U};
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = InstrumentId{7U};
    book.feed_session_epoch = 1U;
    book.recovery_generation = 1U;
    book.view_generation = generation;
    book.last_applied_sequence = generation * 10U;
    book.next_sequence = book.last_applied_sequence + 1U;
    book.local_receive_time_ns = generation * 100U;
    book.local_publication_time_ns = generation * 100U + 1U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.push_bid(md::Level{Price{99U}, Quantity{100U}, 1U}));
    assert(book.push_ask(md::Level{Price{100U}, Quantity{100U}, 1U}));
    assert(book.valid());
    return book;
}

[[nodiscard]] MockXMdPublication live_publication(
    const std::uint64_t generation = 1U) {
    md::EngineUpdate update;
    update.state = MarketDataState::Live;
    update.state_changed = true;
    update.publication_generation = generation;
    update.publications.push_back(live_book(generation));
    return MockXMdPublication{generation, std::move(update)};
}

[[nodiscard]] const std::string& field(
    const FixMessage& message,
    const int tag) {
    for (const auto& item : message.fields) {
        if (item.tag == tag) {
            return item.value;
        }
    }
    assert(false);
    return message.message_type;
}

struct OfflineTransport final {
    SorRuntime* runtime{};
    MockXVenueRuntime* venue_runtime{};
    std::atomic<bool>* permit{};
    bool reentrant_ack{};
    std::atomic<bool> start_observed_recovery{};
    std::atomic<bool> start_observed_empty_egress{};
    std::atomic<std::uint64_t> inbound_sequence{100U};
    std::atomic<std::uint64_t> execution_sequence{};
    mutable std::mutex mutex;
    std::vector<FixMessage> messages;

    [[nodiscard]] QuickFixTestTransport seam() {
        return QuickFixTestTransport{
            [this](std::string&) {
                start_observed_recovery.store(
                    runtime != nullptr && runtime->recovery_pending(),
                    std::memory_order_release);
                start_observed_empty_egress.store(
                    runtime != nullptr &&
                        !runtime->poll_egress(VenueId{1U}).has_value(),
                    std::memory_order_release);
                return true;
            },
            [] {},
            [this](const FixMessage& message) {
                {
                    std::lock_guard lock(mutex);
                    messages.push_back(message);
                }
                if (permit != nullptr && message.message_type == "D") {
                    permit->store(false, std::memory_order_release);
                }
                if (reentrant_ack && message.message_type == "D") {
                    assert(venue_runtime != nullptr);
                    const std::string execution_id =
                        "ACK-" + std::to_string(
                            execution_sequence.fetch_add(1U));
                    const std::vector<FixField> acknowledgement{
                        {11, field(message, 11)},
                        {17, execution_id},
                        {150, "0"},
                        {39, "0"},
                        {14, "0"},
                        {151, field(message, 38)},
                    };
                    assert(venue_runtime->ingest_fix_application(
                        quickfix_config().identity,
                        "8",
                        inbound_sequence.fetch_add(1U),
                        acknowledgement));
                }
                return QuickFixTransportSendResult{
                    LocalDispatchState::AcceptedLocally,
                    "offline transport accepted message",
                };
            },
            [] { return std::string{"20260815-12:34:56.789"}; },
        };
    }

    [[nodiscard]] std::vector<FixMessage> snapshot() const {
        std::lock_guard lock(mutex);
        return messages;
    }
};

void make_ready(
    SorRuntime& runtime,
    MockXVenueRuntime& venue_runtime) {
    assert(venue_runtime.start());
    const auto session = quickfix_config();
    assert(venue_runtime.ingest_fix_session(
        TransportSessionSignal::Created,
        session.identity));
    assert(venue_runtime.ingest_fix_session(
        TransportSessionSignal::LoggedOn,
        session.identity));
    assert(venue_runtime.try_publish_market_data(live_publication()));
    wait_until([&] { return !runtime.recovery_pending(); });
    wait_until([&] {
        return venue_runtime.state() ==
               MockXVenueRuntimeState::Operational;
    });
    const auto adjacency = venue_runtime.adjacency_view();
    assert(adjacency);
    assert(adjacency->generation != 0U);
    assert(adjacency->feed_ready);
    assert(adjacency->order_entry_ready);
    assert(!adjacency->failed_closed);
    assert(adjacency->candidates.size() == 1U);
    assert(adjacency->candidates.front().adjacency_generation ==
           adjacency->generation);
}

void process_scoped_temporary_journal_regression_test() {
    TemporaryJournal first{"path-identity"};
    TemporaryJournal second{"path-identity"};
    assert(first.path() != second.path());
    const std::string process_marker =
        "-p" + std::to_string(process_identity()) + "-";
    assert(first.path().filename().string().find(process_marker) !=
           std::string::npos);
    assert(second.path().filename().string().find(process_marker) !=
           std::string::npos);
}

void startup_reentrancy_and_dispatch_metadata_test() {
    TemporaryJournal journal{"reentrant"};
    SorRuntime runtime(runtime_config(journal.path()),
                       wire_identity_factory());
    OfflineTransport transport;
    transport.runtime = &runtime;
    transport.reentrant_ack = true;
    MockXVenueTestSeams seams;
    seams.order_entry_transport = transport.seam();
    seams.disable_native_market_data = true;
    MockXVenueRuntime venue_runtime(
        runtime, venue_config(), std::move(seams));
    transport.venue_runtime = &venue_runtime;

    assert(runtime.recovery_pending());
    assert(!venue_runtime.attempt_complete_recovery());
    make_ready(runtime, venue_runtime);
    assert(transport.start_observed_recovery.load(
        std::memory_order_acquire));
    assert(transport.start_observed_empty_egress.load(
        std::memory_order_acquire));

    const ParentOrder order{
        ParentOrderId{1U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{5U},
        Price{100U},
        RoutingIntent::AggressiveIOC,
    };
    assert(runtime.submit(order).accepted());
    wait_until([&] {
        const auto state = runtime.inspect();
        return state.children.size() == 1U &&
               state.children.front().state == ChildState::Working;
    });
    const auto sent = transport.snapshot();
    assert(sent.size() == 1U);
    assert(sent.front().message_type == "D");
    assert(field(sent.front(), 11).starts_with("SXN41-1-"));
    assert(field(sent.front(), 38) == "5");
    assert(field(sent.front(), 44) == "1.00");
    assert(!venue_runtime.failed());

    const std::vector<FixField> fill{
        {11, field(sent.front(), 11)},
        {17, "FILL-1"},
        {150, "F"},
        {39, "2"},
        {14, "5"},
        {151, "0"},
        {32, "5"},
        {31, "1.00"},
    };
    assert(venue_runtime.ingest_fix_application(
        quickfix_config().identity,
        "8",
        500U,
        fill));
    wait_until([&] {
        const auto state = runtime.inspect();
        return state.parents.front().state == ParentState::Filled &&
               state.children.front().state == ChildState::Filled &&
               state.children.front().risk_reservation == Quantity{};
    });
    const auto counters = venue_runtime.counters();
    assert(counters.new_dispatches == 1U);
    assert(counters.runtime_events_published >= 6U);
    assert(counters.recovery_completions == 1U);
    venue_runtime.stop();
    assert(!venue_runtime.running());
}

void cancel_before_new_exact_order_test() {
    TemporaryJournal journal{"cancel-priority"};
    SorRuntime runtime(runtime_config(journal.path()),
                       wire_identity_factory());
    std::atomic<bool> permit{true};
    OfflineTransport transport;
    transport.runtime = &runtime;
    transport.permit = &permit;
    MockXVenueTestSeams seams;
    seams.order_entry_transport = transport.seam();
    seams.disable_native_market_data = true;
    seams.permit_egress = [&permit] {
        return permit.load(std::memory_order_acquire);
    };
    MockXVenueRuntime venue_runtime(
        runtime, venue_config(), std::move(seams));
    transport.venue_runtime = &venue_runtime;
    make_ready(runtime, venue_runtime);

    const ParentOrder first{
        ParentOrderId{10U}, InstrumentId{7U}, Side::Buy,
        Quantity{5U}, Price{100U}, RoutingIntent::AggressiveIOC};
    const ParentOrder second{
        ParentOrderId{11U}, InstrumentId{7U}, Side::Buy,
        Quantity{6U}, Price{100U}, RoutingIntent::AggressiveIOC};
    assert(runtime.submit(first).accepted());
    wait_until([&] { return transport.snapshot().size() == 1U; });
    wait_until([&] {
        return runtime.inspect().children.front().state ==
               ChildState::PendingAcknowledgement;
    });

    const auto first_message = transport.snapshot().front();
    const std::vector<FixField> acknowledgement{
        {11, field(first_message, 11)},
        {17, "ACK-FIRST"},
        {150, "0"},
        {39, "0"},
        {14, "0"},
        {151, "5"},
    };
    assert(venue_runtime.ingest_fix_application(
        quickfix_config().identity, "8", 601U, acknowledgement));
    wait_until([&] {
        return runtime.inspect().children.front().state ==
               ChildState::Working;
    });

    assert(runtime.submit(second).accepted());
    assert(runtime.cancel(first.id).accepted());
    permit.store(true, std::memory_order_release);
    wait_until([&] { return transport.snapshot().size() == 3U; });
    const auto ordered = transport.snapshot();
    assert(ordered[0].message_type == "D");
    assert(ordered[1].message_type == "F");
    assert(ordered[2].message_type == "D");
    assert(field(ordered[1], 41) == field(ordered[0], 11));
    assert(field(ordered[2], 11) != field(ordered[0], 11));
    const auto counters = venue_runtime.counters();
    assert(counters.cancel_dispatches == 1U);
    assert(counters.new_dispatches == 2U);
    venue_runtime.stop();
}

void conservative_recovery_helper_test() {
    ChildSnapshot prepared;
    prepared.id = ChildOrderId{1U};
    prepared.state = ChildState::Prepared;
    prepared.filled = Quantity{};
    const auto definitely_unsent =
        MockXVenueRuntime::conservative_reconciliation(prepared);
    assert(definitely_unsent.child == prepared.id);
    assert(definitely_unsent.disposition ==
           ReconciliationDisposition::DefinitelyUnsent);

    ChildSnapshot working;
    working.id = ChildOrderId{2U};
    working.state = ChildState::Working;
    working.filled = Quantity{3U};
    working.risk_reservation = Quantity{7U};
    const auto unresolved =
        MockXVenueRuntime::conservative_reconciliation(working);
    assert(unresolved.child == working.id);
    assert(unresolved.disposition ==
           ReconciliationDisposition::Unresolved);
    assert(unresolved.cumulative_quantity == Quantity{3U});

    working.state = ChildState::Ambiguous;
    assert(MockXVenueRuntime::conservative_reconciliation(working)
               .disposition == ReconciliationDisposition::Unresolved);
    working.state = ChildState::CancelPending;
    assert(MockXVenueRuntime::conservative_reconciliation(working)
               .disposition == ReconciliationDisposition::Unresolved);
}

void bounded_callback_loss_fails_closed_test() {
    TemporaryJournal journal{"bounded-loss"};
    SorRuntime runtime(runtime_config(journal.path()),
                       wire_identity_factory());
    OfflineTransport transport;
    transport.runtime = &runtime;
    MockXVenueTestSeams seams;
    seams.order_entry_transport = transport.seam();
    seams.disable_native_market_data = true;
    MockXVenueRuntime venue_runtime(
        runtime, venue_config(1U), std::move(seams));
    transport.venue_runtime = &venue_runtime;
    assert(venue_runtime.start());

    bool observed_full = false;
    for (std::uint64_t sequence = 1U;
         sequence <= 100'000U && !observed_full; ++sequence) {
        MockXMdHealthTransition transition;
        transition.previous = MockXMdSessionState::Snapshotting;
        transition.current = MockXMdSessionState::Snapshotting;
        transition.generation = sequence;
        transition.monotonic_time_ns = sequence;
        observed_full = !venue_runtime.try_publish_market_data(
            MockXMdPublication{sequence, transition});
    }
    assert(observed_full);
    wait_until([&] { return venue_runtime.failed(); });
    wait_until([&] {
        const auto view = venue_runtime.adjacency_view();
        return view && view->failed_closed && !view->feed_ready &&
               view->market_data_state == MarketDataState::Failed;
    });
    assert(venue_runtime.last_error() ==
           MockXVenueRuntimeError::MarketDataCallbackQueueFull);
    assert(venue_runtime.counters().market_data_callback_losses != 0U);
    assert(runtime.recovery_pending());
    venue_runtime.stop();
}

}  // namespace

int main() {
    process_scoped_temporary_journal_regression_test();
    startup_reentrancy_and_dispatch_metadata_test();
    cancel_before_new_exact_order_test();
    conservative_recovery_helper_test();
    bounded_callback_loss_fails_closed_test();
    return 0;
}




