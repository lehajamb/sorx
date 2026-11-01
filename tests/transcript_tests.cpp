#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/transcript.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sorx;

std::vector<std::byte> bytes(const std::initializer_list<unsigned> values) {
    std::vector<std::byte> output;
    output.reserve(values.size());
    for (const unsigned value : values) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

RouteConfig route(const AdminState admin_state) {
    return RouteConfig{
        VenueId{1U},
        InstrumentId{7U},
        "MOCKX_ONE",
        "ALPHA",
        "ALPHA-X1",
        admin_state == AdminState::Down ?
            *RationalConversion::create(2U, 3U) :
            *RationalConversion::create(1U, 1U),
        admin_state == AdminState::Down ?
            *RationalConversion::create(3U, 5U) :
            *RationalConversion::create(1U, 1U),
        VenueCapabilities{true, true},
        Quantity{100U},
        1U,
        1U,
        50U,
        admin_state,
    };
}

std::shared_ptr<const CandidateTable> table(
    const std::uint64_t generation,
    const std::uint64_t revision,
    const AdminState admin_state = AdminState::Up) {
    RouteRib rib;
    std::vector<RouteValidationError> errors;
    assert(rib.replace_all({route(admin_state)}, errors));
    assert(rib.restore_revision(revision));
    FibCompiler compiler;
    return compiler.compile_at_generation(rib, generation);
}

CoreConfiguration configuration() {
    CoreConfiguration config;
    config.maximum_aggressive_venues = 1U;
    config.risk.generation = 17U;
    config.risk.instruments.push_back(InstrumentRiskLimit{
        InstrumentId{7U},
        true,
        Quantity{1'000U},
        Quantity{1'000U},
        Notional{10'000'000U},
        Notional{10'000'000U},
        Price{1},
        Price{1'000},
        10U,
        10U,
        Quantity{1'000U},
    });
    config.risk.venues.push_back(VenueRiskLimit{
        VenueId{1U}, true, Quantity{1'000U}});
    return config;
}

md::BookView book(const std::uint64_t view_generation) {
    md::BookView value;
    value.venue = VenueId{1U};
    value.channel = ChannelId{1U};
    value.profile = ProfileId{1U};
    value.instrument = InstrumentId{7U};
    value.feed_session_epoch = 3U;
    value.recovery_generation = 4U;
    value.view_generation = view_generation;
    value.last_applied_sequence = 40U + view_generation;
    value.next_sequence = value.last_applied_sequence + 1U;
    // The two views used below intentionally share all three timestamps. Their
    // ingress sequence, not a wall clock, is the deterministic tie-breaker.
    value.exchange_timestamp_ns = 500;
    value.local_receive_time_ns = 600U;
    value.local_publication_time_ns = 601U;
    value.feed_state = MarketDataState::Live;
    value.trading_state = InstrumentState::Trading;
    assert(value.push_bid(md::Level{Price{99}, Quantity{20U}, 2U}));
    assert(value.push_ask(md::Level{Price{100}, Quantity{20U}, 3U}));
    assert(value.valid());
    return value;
}

CandidateRuntimeView runtime_view(
    const std::uint64_t adjacency_generation,
    const std::uint64_t view_generation) {
    return CandidateRuntimeView{
        VenueId{1U},
        adjacency_generation,
        book(view_generation),
        MarketDataState::Live,
        OrderEntryState::Ready,
        EgressState::Ready,
        InstrumentState::Trading,
        true,
        true,
        true,
    };
}

ApplyResult append(
    SorCore& core,
    std::vector<CoreEvent>& transcript,
    std::uint64_t& sequence,
    CoreEventPayload payload) {
    transcript.push_back(CoreEvent{++sequence, std::move(payload)});
    return core.apply(transcript.back());
}

adapters::NormalizedExecutionReport execution_report(
    const ChildSnapshot& child,
    std::string execution_id,
    const adapters::ExecutionKind kind,
    const adapters::NormalizedOrderStatus status,
    const std::uint64_t last_quantity,
    const std::uint64_t cumulative_quantity,
    const std::uint64_t leaves_quantity) {
    adapters::NormalizedExecutionReport report;
    report.venue = child.venue;
    report.session_epoch = child.session_epoch;
    report.wire_order_id = child.wire_order_id;
    report.execution_id = std::move(execution_id);
    report.durable_report_key = report.execution_id;
    report.kind = kind;
    report.status = status;
    if (last_quantity != 0U) {
        report.last_quantity = Quantity{last_quantity};
        report.last_price = Price{100};
    }
    report.cumulative_quantity = Quantity{cumulative_quantity};
    report.leaves_quantity = Quantity{leaves_quantity};
    report.value_domain = adapters::ExecutionValueDomain::Canonical;
    return report;
}

bool contains_record(
    const ApplyResult& result,
    const DeterministicRecordKind kind) {
    return std::any_of(
        result.records.begin(),
        result.records.end(),
        [kind](const DeterministicRecord& record) {
            return record.kind == kind;
        });
}

void generic_codec_compatibility_test() {
    const std::vector<TranscriptEvent> input{
        {1U, 7U, bytes({1U, 2U})},
        {2U, 9U, bytes({3U})},
    };
    const auto encoded = TranscriptCodec::encode(input);
    assert(TranscriptCodec::decode(encoded) == input);

    const auto replay_once = TranscriptReplay::run(
        input,
        [](const TranscriptEvent& event) {
            auto output = event.payload;
            output.push_back(static_cast<std::byte>(event.kind));
            return output;
        });
    const auto replay_twice = TranscriptReplay::run(
        TranscriptCodec::decode(encoded),
        [](const TranscriptEvent& event) {
            auto output = event.payload;
            output.push_back(static_cast<std::byte>(event.kind));
            return output;
        });
    assert(replay_once == replay_twice);

    bool ordering_failed = false;
    try {
        static_cast<void>(TranscriptCodec::encode(
            std::vector<TranscriptEvent>{{2U, 1U, {}}, {2U, 2U, {}}}));
    } catch (const std::invalid_argument&) {
        ordering_failed = true;
    }
    assert(ordering_failed);
}

void deterministic_core_replay_test() {
    const auto initial_table = table(1U, 1U);
    SorCore recorder(configuration(), initial_table);
    std::vector<CoreEvent> events;
    std::uint64_t sequence = 0U;

    assert(append(
               recorder,
               events,
               sequence,
               OrderEntrySessionEvent{
                   VenueId{1U}, 1U, OrderEntryState::Ready})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               RuntimeViewPublishedEvent{
                   InstrumentId{7U}, runtime_view(10U, 100U)})
               .command_accepted);

    // Management events and route-admin publications are normalized inputs,
    // and therefore have deterministic positions in the same transcript.
    assert(append(
               recorder,
               events,
               sequence,
               KillSwitchEvent{true, "operator hold"})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               KillSwitchEvent{false, "operator release"})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               RoutingGateEvent{false})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               RoutingGateEvent{true})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               CandidateTablePublishedEvent{
                   table(2U, 2U, AdminState::Down)})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               CandidateTablePublishedEvent{
                   table(3U, 3U, AdminState::Up)})
               .command_accepted);

    assert(append(
               recorder,
               events,
               sequence,
               OrderEntrySessionEvent{
                   VenueId{1U}, 1U, OrderEntryState::Disconnected})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               OrderEntrySessionEvent{
                   VenueId{1U}, 2U, OrderEntryState::Ready})
               .command_accepted);
    assert(append(
               recorder,
               events,
               sequence,
               RuntimeViewPublishedEvent{
                   InstrumentId{7U}, runtime_view(13U, 101U)})
               .command_accepted);

    const ParentOrder parent{
        ParentOrderId{70U},
        InstrumentId{7U},
        Side::Buy,
        Quantity{15U},
        Price{100},
        RoutingIntent::AggressiveIOC,
    };
    const ApplyResult submitted = append(
        recorder, events, sequence, ParentSubmitEvent{parent});
    assert(submitted.command_accepted);
    assert(submitted.effects.size() == 1U);
    assert(!submitted.reports.empty());
    assert(contains_record(submitted, DeterministicRecordKind::RouteDecision));
    assert(contains_record(
        submitted, DeterministicRecordKind::RiskReservationCreated));

    const ChildSnapshot child = recorder.snapshot().children.front();
    const auto& intent =
        std::get<DispatchNewEffect>(submitted.effects.front()).intent;
    const DispatchStartedEvent started{
        child.id,
        adapters::DispatchAttemptKind::New,
        1U,
        intent.venue,
        intent.session_epoch,
        intent.wire_order_id,
    };
    assert(append(recorder, events, sequence, started).command_accepted);

    adapters::LocalDispatchResult local;
    local.child = child.id;
    local.state = adapters::LocalDispatchState::AcceptedLocally;
    local.detail = "queued by normalized session";
    local.kind = adapters::DispatchAttemptKind::New;
    local.attempt_id = 1U;
    local.venue = intent.venue;
    local.session_epoch = intent.session_epoch;
    local.wire_order_id = intent.wire_order_id;
    assert(append(
               recorder, events, sequence, LocalDispatchEvent{local})
               .command_accepted);

    const auto acknowledged = execution_report(
        child,
        "ACK-1",
        adapters::ExecutionKind::Acknowledged,
        adapters::NormalizedOrderStatus::Working,
        0U,
        0U,
        15U);
    assert(append(
               recorder,
               events,
               sequence,
               ExecutionReportEvent{acknowledged})
               .command_accepted);

    const auto third = execution_report(
        child,
        "TRADE-3",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::Filled,
        5U,
        15U,
        0U);
    const auto second = execution_report(
        child,
        "TRADE-2",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        5U,
        10U,
        5U);
    const auto first = execution_report(
        child,
        "TRADE-1",
        adapters::ExecutionKind::Trade,
        adapters::NormalizedOrderStatus::PartiallyFilled,
        5U,
        5U,
        10U);

    assert(append(
               recorder, events, sequence, ExecutionReportEvent{third})
               .command_accepted);
    assert(append(
               recorder, events, sequence, ExecutionReportEvent{second})
               .command_accepted);
    assert(append(
               recorder, events, sequence, ExecutionReportEvent{third})
               .command_accepted);
    assert(recorder.snapshot().parents.front().cumulative_filled == Quantity{});

    const ApplyResult drained = append(
        recorder, events, sequence, ExecutionReportEvent{first});
    assert(drained.command_accepted);
    assert(!drained.reports.empty());
    assert(contains_record(
        drained, DeterministicRecordKind::RiskReservationReleased));
    assert(recorder.snapshot().parents.front().cumulative_filled ==
           Quantity{15U});
    assert(recorder.snapshot().parents.front().state == ParentState::Filled);

    // Cover the remaining normalized variants. Terminal/no-op outcomes are
    // still part of the deterministic output and must replay identically.
    static_cast<void>(append(
        recorder,
        events,
        sequence,
        ParentReplaceEvent{ParentReplace{parent.id, Quantity{16U}, Price{100}}}));
    static_cast<void>(append(
        recorder, events, sequence, ParentCancelEvent{parent.id}));
    static_cast<void>(append(
        recorder,
        events,
        sequence,
        DispatchTimeoutEvent{child.id, "late timeout"}));
    static_cast<void>(append(
        recorder,
        events,
        sequence,
        ReconciliationEvent{
            child.id, ReconciliationDisposition::TerminalFilled, Quantity{15U}}));

    const std::vector<std::byte> recorded_final_state = recorder.save_state();
    const std::vector<std::byte> encoded =
        CoreEventTranscriptCodec::encode(events);
    const std::vector<CoreEvent> decoded =
        CoreEventTranscriptCodec::decode(encoded);
    assert(decoded.size() == events.size());
    assert(CoreEventTranscriptCodec::encode(decoded) == encoded);

    const auto& first_view =
        std::get<RuntimeViewPublishedEvent>(decoded[1U].payload).view;
    const auto& second_view =
        std::get<RuntimeViewPublishedEvent>(decoded[10U].payload).view;
    assert(first_view.book->exchange_timestamp_ns ==
           second_view.book->exchange_timestamp_ns);
    assert(decoded[1U].ingress_sequence < decoded[10U].ingress_sequence);
    assert(second_view.book->view_generation == 101U);

    SorCore replay_one(configuration(), initial_table);
    SorCore replay_two(configuration(), initial_table);
    const CoreReplayImage image_one =
        CoreTranscriptReplay::run_encoded(replay_one, encoded);
    const CoreReplayImage image_two =
        CoreTranscriptReplay::run(replay_two, decoded);
    assert(image_one == image_two);
    assert(image_one.final_state == recorded_final_state);
    assert(replay_one.save_state() == replay_two.save_state());
    assert(replay_one.snapshot().parents.front().cumulative_filled ==
           Quantity{15U});
    assert(replay_one.snapshot().parents.front().explicit_remainder ==
           Quantity{});

    const auto outcomes = TranscriptCodec::decode(image_one.outcome_transcript);
    assert(outcomes.size() == events.size());
    assert(std::all_of(
        outcomes.begin(),
        outcomes.end(),
        [](const TranscriptEvent& outcome) {
            return !outcome.payload.empty();
        }));
}

void fail_closed_test() {
    const auto initial_table = table(1U, 1U);
    const std::vector<CoreEvent> input{
        CoreEvent{
            1U,
            OrderEntrySessionEvent{
                VenueId{1U}, 1U, OrderEntryState::Ready}},
    };
    const auto encoded = CoreEventTranscriptCodec::encode(input);

    auto corrupted = encoded;
    corrupted.back() ^= std::byte{1U};
    SorCore corruption_target(configuration(), initial_table);
    const auto before_corruption = corruption_target.save_state();
    bool checksum_failed = false;
    try {
        static_cast<void>(
            CoreTranscriptReplay::run_encoded(corruption_target, corrupted));
    } catch (const std::runtime_error&) {
        checksum_failed = true;
    }
    assert(checksum_failed);
    assert(corruption_target.save_state() == before_corruption);

    const auto unknown = TranscriptCodec::encode(
        std::vector<TranscriptEvent>{{1U, 65'535U, {}}});
    SorCore unknown_target(configuration(), initial_table);
    const auto before_unknown = unknown_target.save_state();
    bool unknown_failed = false;
    try {
        static_cast<void>(
            CoreTranscriptReplay::run_encoded(unknown_target, unknown));
    } catch (const std::runtime_error&) {
        unknown_failed = true;
    }
    assert(unknown_failed);
    assert(unknown_target.save_state() == before_unknown);

    auto invalid_frames = TranscriptCodec::decode(encoded);
    invalid_frames.front().payload.back() = std::byte{0xffU};
    const auto invalid_enum = TranscriptCodec::encode(invalid_frames);
    bool enum_failed = false;
    try {
        static_cast<void>(CoreEventTranscriptCodec::decode(invalid_enum));
    } catch (const std::runtime_error&) {
        enum_failed = true;
    }
    assert(enum_failed);

    SorCore gap_target(configuration(), initial_table);
    const auto before_gap = gap_target.save_state();
    bool gap_failed = false;
    try {
        static_cast<void>(CoreTranscriptReplay::run(
            gap_target,
            std::vector<CoreEvent>{
                CoreEvent{2U, KillSwitchEvent{true, "sequence gap"}},
            }));
    } catch (const std::invalid_argument&) {
        gap_failed = true;
    }
    assert(gap_failed);
    assert(gap_target.save_state() == before_gap);
}

}  // namespace

int main() {
    generic_codec_compatibility_test();
    deterministic_core_replay_test();
    fail_closed_test();
    return 0;
}






