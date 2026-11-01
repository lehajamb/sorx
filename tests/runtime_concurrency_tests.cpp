#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace sorx;

class RuntimeTestWireIdentityFactory final
    : public adapters::WireIdentityFactory {
public:
    RuntimeTestWireIdentityFactory() {
        profiles_.push_back(adapters::WireProfileIdentity{
            VenueId{1U},
            ProfileId{2U},
            "MockXFIX44",
            "OE-SESSION-1",
            "FIX44.xml",
            "SORX",
            "MOCKX",
        });
    }

    std::span<const adapters::WireProfileIdentity> profiles()
        const noexcept override {
        return profiles_;
    }

    adapters::WireIdentityResult make(
        const adapters::WireIdentityRequest& request) const override {
        adapters::WireIdentityResult result;
        result.profile = request.profile;
        if (request.profile != profiles_.front() ||
            !request.child || !request.parent ||
            request.parent_revision == 0U ||
            request.session_epoch == 0U) {
            return result;
        }
        result.wire_order_id =
            (request.kind == adapters::DispatchAttemptKind::New ?
                 "TEST-N" :
                 "TEST-C") +
            std::to_string(request.session_epoch) + "-" +
            std::to_string(request.parent_revision) + "-" +
            std::to_string(request.child.value());
        result.ok = true;
        return result;
    }

private:
    std::vector<adapters::WireProfileIdentity> profiles_;
};

std::shared_ptr<const adapters::WireIdentityFactory>
runtime_test_wire_factory() {
    return std::make_shared<RuntimeTestWireIdentityFactory>();
}

std::filesystem::path temporary_journal() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("sorx-runtime-concurrency-" + suffix + ".journal");
}

void clean_runtime_files(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".lock", ignored);
    std::filesystem::remove(path.string() + ".checkpoint", ignored);
    std::filesystem::remove(path.string() + ".checkpoint.tmp", ignored);
}

RuntimeConfiguration configuration(const std::filesystem::path& path) {
    config::ValidatedConfiguration validated;
    validated.schema_version = 1U;
    validated.account_id = "runtime-concurrency-test";
    validated.ingress_capacity = 1U;
    validated.core.maximum_aggressive_venues = 1U;
    validated.core.risk.generation = 1U;

    const InstrumentRiskLimit instrument_limit{
        InstrumentId{7U},
        true,
        Quantity{100U},
        Quantity{100U},
        Notional{10'000'000U},
        Notional{10'000'000U},
        Price{1U},
        Price{1'000U},
        1'000U,
        1'000U,
        Quantity{100'000U},
    };
    const VenueRiskLimit venue_limit{
        VenueId{1U}, true, Quantity{100'000U}};
    validated.core.risk.instruments.push_back(instrument_limit);
    validated.core.risk.venues.push_back(venue_limit);
    validated.instruments.push_back(config::InstrumentConfiguration{
        InstrumentId{7U}, "ALPHA", instrument_limit});
    validated.venues.push_back(config::VenueConfiguration{
        VenueId{1U},
        "VENUE_1",
        AdminState::Up,
        venue_limit,
        1U,
        1U,
        256U,
        256U,
        ChannelId{1U},
        ProfileId{1U},
        ProfileId{2U},
        "MockXITCH",
        "MD-SESSION-1",
        "MockXFIX44",
        "OE-SESSION-1",
        "FIX44.xml",
        "SORX",
        "MOCKX",
        std::nullopt,
    });
    validated.routes.push_back(RouteConfig{
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
    });
    validated.route_critical_hash =
        config::compute_route_critical_hash(validated);
    validated.route_critical_hash_hex =
        config::sha256_hex(validated.route_critical_hash);

    RuntimeConfiguration result;
    result.validated = std::move(validated);
    result.journal_path = path;
    result.checkpoint_interval_batches = 8U;
    return result;
}

CandidateRuntimeView runtime_view(
    const std::uint64_t generation,
    const MarketDataState health = MarketDataState::Live) {
    md::BookView book;
    book.venue = VenueId{1U};
    book.channel = ChannelId{1U};
    book.profile = ProfileId{1U};
    book.instrument = InstrumentId{7U};
    book.feed_session_epoch = 1U;
    book.recovery_generation = generation;
    book.view_generation = generation;
    book.last_applied_sequence = generation * 10U;
    book.next_sequence = book.last_applied_sequence + 1U;
    book.local_receive_time_ns = generation * 100U;
    book.local_publication_time_ns = generation * 100U + 1U;
    book.feed_state = health;
    book.trading_state = InstrumentState::Trading;
    assert(book.push_bid(md::Level{
        Price{99U}, Quantity{100'000U}, 1U}));
    assert(book.push_ask(md::Level{
        Price{100U}, Quantity{100'000U}, 1U}));
    assert(book.valid());
    return CandidateRuntimeView{
        VenueId{1U},
        generation,
        book,
        health,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        health == MarketDataState::Live,
        true,
        true,
    };
}

ParentOrder order(const std::uint64_t id) {
    return ParentOrder{
        ParentOrderId{id},
        InstrumentId{7U},
        Side::Buy,
        Quantity{1U},
        Price{100U},
        RoutingIntent::AggressiveIOC,
    };
}

struct BlockingSink final : IParentReportSink {
    std::promise<void> first_callback;
    std::shared_future<void> release;
    std::atomic<bool> announced{};
    std::mutex mutex;
    std::vector<std::thread::id> callback_threads;

    explicit BlockingSink(std::shared_future<void> release_signal)
        : release(std::move(release_signal)) {}

    void on_parent_report(const ParentReport&) override {
        {
            std::scoped_lock lock(mutex);
            callback_threads.push_back(std::this_thread::get_id());
        }
        if (!announced.exchange(true)) {
            first_callback.set_value();
        }
        release.wait();
    }
};

struct ThrowingSink final : IParentReportSink {
    std::atomic<std::uint64_t> calls{};
    std::mutex mutex;
    std::vector<std::thread::id> callback_threads;

    void on_parent_report(const ParentReport&) override {
        calls.fetch_add(1U, std::memory_order_relaxed);
        {
            std::scoped_lock lock(mutex);
            callback_threads.push_back(std::this_thread::get_id());
        }
        throw std::runtime_error("intentional report callback failure");
    }
};

struct RecordingSink final : IParentReportSink {
    std::mutex mutex;
    std::vector<ParentOrderId> parents;

    void on_parent_report(const ParentReport& report) override {
        std::scoped_lock lock(mutex);
        parents.push_back(report.parent);
    }
};

}  // namespace

int main() {
    const std::filesystem::path path = temporary_journal();
    clean_runtime_files(path);
    const std::thread::id caller_thread = std::this_thread::get_id();

    {
        SorRuntime runtime(configuration(path), runtime_test_wire_factory());
        assert(runtime.recovery_pending());
        assert(runtime.publish(OrderEntrySessionEvent{
            VenueId{1U}, 1U, OrderEntryState::Ready}));
        assert(runtime.publish(RuntimeViewPublishedEvent{
            InstrumentId{7U}, runtime_view(1U)}));
        assert(runtime.complete_recovery());

        // A blocked callback does not hold or run on the coordinator: both
        // command completion and inspection proceed while delivery is blocked.
        std::promise<void> release_callback;
        BlockingSink blocking(release_callback.get_future().share());
        auto callback_entered = blocking.first_callback.get_future();
        runtime.set_report_sink(blocking);
        auto submitted = std::async(std::launch::async, [&runtime] {
            return runtime.submit(order(1U));
        });
        assert(callback_entered.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
        assert(submitted.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
        assert(submitted.get().accepted());
        auto inspected = std::async(std::launch::async, [&runtime] {
            return runtime.inspect();
        });
        assert(inspected.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
        assert(!inspected.get().parents.empty());
        release_callback.set_value();
        {
            std::scoped_lock lock(blocking.mutex);
            assert(!blocking.callback_threads.empty());
            for (const auto callback_thread : blocking.callback_threads) {
                assert(callback_thread != caller_thread);
            }
        }
        runtime.clear_report_sink();

        // Callback exceptions cannot escape a committed command or fail-stop
        // the coordinator, and are observable after the delivery barrier.
        ThrowingSink throwing;
        runtime.set_report_sink(throwing);
        const std::uint64_t failures_before =
            runtime.report_callback_failures();
        assert(runtime.submit(order(2U)).accepted());
        runtime.clear_report_sink();
        assert(throwing.calls.load(std::memory_order_relaxed) != 0U);
        assert(runtime.report_callback_failures() > failures_before);
        assert(!runtime.failed());
        {
            std::scoped_lock lock(throwing.mutex);
            assert(!throwing.callback_threads.empty());
            for (const auto callback_thread : throwing.callback_threads) {
                assert(callback_thread != caller_thread);
            }
        }

        // The report worker preserves coordinator batch order.
        RecordingSink recording;
        runtime.set_report_sink(recording);
        assert(runtime.submit(order(3U)).accepted());
        assert(runtime.submit(order(4U)).accepted());
        runtime.clear_report_sink();
        {
            std::scoped_lock lock(recording.mutex);
            assert(!recording.parents.empty());
            const auto first_four = std::find(
                recording.parents.begin(),
                recording.parents.end(),
                ParentOrderId{4U});
            assert(first_four != recording.parents.end());
            assert(std::all_of(
                recording.parents.begin(),
                first_four,
                [](const ParentOrderId parent) {
                    return parent == ParentOrderId{3U};
                }));
        }

        // Simultaneous callers are serialized by one coordinator. With one
        // normal ingress slot, excess work is rejected explicitly rather than
        // dropped, while accepted sequences remain unique and durable.
        constexpr std::size_t caller_count = 64U;
        std::latch ready{caller_count};
        std::latch start{1U};
        std::vector<CommandReceipt> receipts(caller_count);
        std::vector<std::thread> callers;
        callers.reserve(caller_count);
        for (std::size_t index = 0U; index < caller_count; ++index) {
            callers.emplace_back([&, index] {
                ready.count_down();
                start.wait();
                receipts[index] = runtime.submit(order(1'000U + index));
            });
        }
        ready.wait();
        start.count_down();

        // A non-LIVE update uses reserved capacity and blocks for admission;
        // it cannot be lost even while the ordinary lane is saturated.
        assert(runtime.publish(RuntimeViewPublishedEvent{
            InstrumentId{7U},
            runtime_view(2U, MarketDataState::Failed)}));
        for (auto& caller : callers) {
            caller.join();
        }

        std::size_t accepted_count = 0U;
        std::size_t saturated_count = 0U;
        std::vector<std::uint64_t> sequences;
        for (const auto& receipt : receipts) {
            if (receipt.accepted()) {
                ++accepted_count;
                sequences.push_back(receipt.ingress_sequence);
            } else if (receipt.error == CommandError::IngressSaturated) {
                ++saturated_count;
                assert(receipt.ingress_sequence == 0U);
            }
        }
        assert(accepted_count != 0U);
        assert(saturated_count != 0U);
        std::sort(sequences.begin(), sequences.end());
        assert(std::adjacent_find(sequences.begin(), sequences.end()) ==
               sequences.end());

        const RouteTrace failed_trace = runtime.trace(order(9'000U));
        assert(!failed_trace.decision.accepted());
        assert(std::any_of(
            failed_trace.decision.exclusions.begin(),
            failed_trace.decision.exclusions.end(),
            [](const CandidateExclusion& exclusion) {
                return exclusion.reason ==
                       CandidateExclusionReason::MarketDataNotLive;
            }));

        assert(runtime.publish(RuntimeViewPublishedEvent{
            InstrumentId{7U}, runtime_view(3U)}));
        assert(!runtime.failed());

        // Detaching is a barrier: this referenced sink can now be destroyed,
        // and later reports are safely processed with no callback target.
        {
            RecordingSink temporary;
            runtime.set_report_sink(temporary);
            assert(runtime.submit(order(10'000U)).accepted());
            runtime.clear_report_sink();
        }
        static_cast<void>(runtime.submit(order(10'001U)));
    }

    clean_runtime_files(path);
    return 0;
}

