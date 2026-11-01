#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/runtime.hpp"

#include <filesystem>
#include <string>
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
    const auto base = std::filesystem::temp_directory_path();
    const auto path = base / "sorx-runtime-test.journal";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".lock", ignored);
    std::filesystem::remove(
        path.string() + ".checkpoint", ignored);
    std::filesystem::remove(path.string() + ".checkpoint", ignored);
    std::filesystem::remove(
        path.string() + ".checkpoint.tmp", ignored);
    return path;
}

RuntimeConfiguration configuration(const std::filesystem::path& journal_path) {
    config::ValidatedConfiguration validated;
    validated.schema_version = 1U;
    validated.account_id = "runtime-test";
    validated.ingress_capacity = 16U;
    validated.core.maximum_aggressive_venues = 1U;
    validated.core.risk.generation = 1U;

    const InstrumentRiskLimit instrument_limit{
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
    };
    const VenueRiskLimit venue_limit{
        VenueId{1U}, true, Quantity{1'000U}};
    validated.core.risk.instruments.push_back(instrument_limit);
    validated.core.risk.venues.push_back(venue_limit);
    validated.instruments.push_back(config::InstrumentConfiguration{
        InstrumentId{7U}, "ALPHA", instrument_limit});
    validated.venues.push_back(config::VenueConfiguration{
        VenueId{1U},
        "VENUE_1",
        AdminState::Up,
        venue_limit,
        8U,
        8U,
        1U,
        1U,
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
    result.journal_path = journal_path;
    result.checkpoint_interval_batches = 1U;
    return result;
}

CandidateRuntimeView live_view() {
    static std::uint64_t fixture_generation = 0U;
    const std::uint64_t generation = ++fixture_generation;
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
    book.local_receive_time_ns = 10U;
    book.local_publication_time_ns = 11U;
    book.feed_state = MarketDataState::Live;
    book.trading_state = InstrumentState::Trading;
    assert(book.push_bid(md::Level{Price{99U}, Quantity{10U}, 1U}));
    assert(book.push_ask(md::Level{Price{100U}, Quantity{10U}, 1U}));
    assert(book.valid());
    return CandidateRuntimeView{
        VenueId{1U},
        generation * 10U,
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

struct Sink final : IParentReportSink {
    std::vector<ParentReport> reports;

    void on_parent_report(const ParentReport& report) override {
        reports.push_back(report);
    }
};

void publish_live(SorRuntime& runtime) {
    assert(runtime.publish(OrderEntrySessionEvent{
        VenueId{1U}, 1U, OrderEntryState::Ready}));
    assert(runtime.publish(RuntimeViewPublishedEvent{
        InstrumentId{7U}, live_view()}));
}

adapters::LocalDispatchResult accepted_new(
    const adapters::NewOrderIntent& intent) {
    adapters::LocalDispatchResult result;
    result.child = intent.child;
    result.state = adapters::LocalDispatchState::AcceptedLocally;
    result.kind = adapters::DispatchAttemptKind::New;
    result.attempt_id = intent.dispatch_attempt_id;
    result.venue = intent.venue;
    result.session_epoch = intent.session_epoch;
    result.wire_order_id = intent.wire_order_id;
    return result;
}

}  // namespace

int main() {
    const auto path = temporary_journal();
    {
        SorRuntime runtime(configuration(path), runtime_test_wire_factory());
        Sink sink;
        runtime.set_report_sink(sink);
        assert(runtime.recovery_pending());

        const ParentOrder order{
            ParentOrderId{1U},
            InstrumentId{7U},
            Side::Buy,
            Quantity{5U},
            Price{100U},
            RoutingIntent::AggressiveIOC,
        };
        const auto gated = runtime.submit(order);
        assert(!gated.accepted());
        assert(gated.error == CommandError::RecoveryInProgress);

        publish_live(runtime);
        assert(runtime.complete_recovery());

        const auto receipt = runtime.submit(order);
        assert(receipt.accepted());
        assert(receipt.ingress_sequence != 0U);
        assert(receipt.journal_record_id != 0U);
        runtime.clear_report_sink();
        assert(!sink.reports.empty());

        const auto effect = runtime.poll_egress(VenueId{1U});
        assert(effect.has_value());
        assert(std::holds_alternative<DispatchNewEffect>(*effect));
        const auto& dispatched_new =
            std::get<DispatchNewEffect>(*effect).intent;
        assert(dispatched_new.dispatch_attempt_id != 0U);

        auto state = runtime.inspect();
        const ChildSnapshot child = state.children.front();
        assert(runtime.publish(LocalDispatchEvent{
            accepted_new(dispatched_new)}));

        auto failed = live_view();
        failed.market_data_state = MarketDataState::Failed;
        failed.feed_sufficiently_fresh = false;
        failed.book->feed_state = MarketDataState::Failed;
        assert(runtime.publish(RuntimeViewPublishedEvent{
            InstrumentId{7U}, std::move(failed)}));

        const auto cancel = runtime.cancel(order.id);
        assert(cancel.accepted());
        const auto cancel_effect = runtime.poll_egress(VenueId{1U});
        assert(cancel_effect.has_value());
        assert(std::holds_alternative<DispatchCancelEffect>(*cancel_effect));
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".lock", ignored);
    std::filesystem::remove(
        path.string() + ".checkpoint", ignored);

    const auto restart_path =
        std::filesystem::temp_directory_path() /
        "sorx-runtime-restart-test.journal";
    std::filesystem::remove(restart_path, ignored);
    std::filesystem::remove(
        restart_path.string() + ".lock", ignored);
    std::filesystem::remove(
        restart_path.string() + ".checkpoint", ignored);
    {
        SorRuntime seeded(
            configuration(restart_path), runtime_test_wire_factory());
        publish_live(seeded);
        assert(seeded.complete_recovery());
        const ParentOrder prepared_order{
            ParentOrderId{10U},
            InstrumentId{7U},
            Side::Buy,
            Quantity{5U},
            Price{100U},
            RoutingIntent::AggressiveIOC,
        };
        assert(seeded.submit(prepared_order).accepted());
        assert(seeded.inspect().children.front().state ==
               ChildState::Prepared);
    }

    ChildSnapshot prepared_child;
    {
        SorRuntime recovered(
            configuration(restart_path), runtime_test_wire_factory());
        assert(recovered.recovery_pending());
        auto state = recovered.inspect();
        assert(state.parents.size() == 1U);
        assert(state.children.size() == 1U);
        prepared_child = state.children.front();
        assert(prepared_child.state == ChildState::Prepared);
        assert(!recovered.complete_recovery());

        publish_live(recovered);
        assert(recovered.publish(ReconciliationEvent{
            prepared_child.id,
            ReconciliationDisposition::DefinitelyUnsent,
            Quantity{},
        }));
        const auto retry = recovered.poll_egress(VenueId{1U});
        assert(retry.has_value());
        const auto& retry_new = std::get<DispatchNewEffect>(*retry);
        assert(retry_new.intent.child == prepared_child.id);
        assert(retry_new.intent.wire_order_id ==
               prepared_child.wire_order_id);

        assert(retry_new.intent.dispatch_attempt_id != 0U);
        assert(recovered.publish(LocalDispatchEvent{
            accepted_new(retry_new.intent)}));
        adapters::NormalizedExecutionReport acknowledgement;
        acknowledgement.venue = prepared_child.venue;
        acknowledgement.session_epoch = prepared_child.session_epoch;
        acknowledgement.wire_order_id = prepared_child.wire_order_id;
        acknowledgement.execution_id = "RECOVERY-ACK";
        acknowledgement.durable_report_key = "RECOVERY-ACK";
        acknowledgement.kind = adapters::ExecutionKind::Acknowledged;
        acknowledgement.status =
            adapters::NormalizedOrderStatus::Working;
        acknowledgement.cumulative_quantity = Quantity{};
        acknowledgement.leaves_quantity = prepared_child.ordered;
        assert(recovered.publish(
            ExecutionReportEvent{acknowledgement}));
        assert(recovered.complete_recovery());

        const ParentOrder second{
            ParentOrderId{11U},
            InstrumentId{7U},
            Side::Buy,
            Quantity{3U},
            Price{100U},
            RoutingIntent::AggressiveIOC,
        };
        assert(recovered.submit(second).accepted());
        state = recovered.inspect();
        assert(state.children.size() == 2U);
        assert(state.children.back().id.value() >
               prepared_child.id.value());
        const auto uncertain =
            recovered.poll_egress(VenueId{1U});
        assert(uncertain.has_value());
        assert(std::get<DispatchNewEffect>(*uncertain)
                   .intent.child == state.children.back().id);
    }

    {
        SorRuntime recovered_after_dispatch(
            configuration(restart_path), runtime_test_wire_factory());
        assert(recovered_after_dispatch.recovery_pending());
        const auto state = recovered_after_dispatch.inspect();
        assert(state.children.size() == 2U);
        const auto& uncertain = state.children.back();
        assert(uncertain.state == ChildState::Ambiguous);
        assert(uncertain.risk_reservation == uncertain.ordered);
        assert(state.parents.back().state == ParentState::Suspended);
        assert(!recovered_after_dispatch.complete_recovery());
    }

    std::filesystem::remove(restart_path, ignored);
    std::filesystem::remove(
        restart_path.string() + ".lock", ignored);
    std::filesystem::remove(
        restart_path.string() + ".checkpoint", ignored);
    return 0;
}

