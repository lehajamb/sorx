#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/adapters/quickfix_session.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;
using namespace sorx::adapters::quickfix;

struct CapturingSink final : IQuickFixSessionSink {
    bool accept_session{true};
    bool accept_dispatch{true};
    bool accept_execution{true};
    std::vector<OrderEntrySessionEvent> sessions;
    std::vector<LocalDispatchResult> dispatches;
    std::vector<NormalizedExecutionReport> executions;

    bool try_publish(OrderEntrySessionEvent event) override {
        if (!accept_session) {
            return false;
        }
        sessions.push_back(std::move(event));
        return true;
    }

    bool try_publish(LocalDispatchResult result) override {
        if (!accept_dispatch) {
            return false;
        }
        dispatches.push_back(std::move(result));
        return true;
    }

    bool try_publish(NormalizedExecutionReport report) override {
        if (!accept_execution) {
            return false;
        }
        executions.push_back(std::move(report));
        return true;
    }
};

struct TransportCapture final {
    bool start_result{true};
    bool started{};
    std::size_t stop_calls{};
    QuickFixTransportSendResult next_send{
        LocalDispatchState::AcceptedLocally,
        "accepted by offline transport",
    };
    std::vector<FixMessage> messages;

    [[nodiscard]] QuickFixTestTransport seam() {
        return QuickFixTestTransport{
            [this](std::string& error) {
                started = true;
                if (!start_result) {
                    error = "offline start refused";
                }
                return start_result;
            },
            [this] { ++stop_calls; },
            [this](const FixMessage& message) {
                messages.push_back(message);
                return next_send;
            },
            [] { return std::string{"20260815-12:34:56.789"}; },
        };
    }
};

[[nodiscard]] QuickFixSessionConfig config(
    const std::uint32_t venue = 1,
    const std::uint64_t epoch = 42) {
    return QuickFixSessionConfig{
        VenueId{venue},
        epoch,
        "offline-quickfix-session.cfg",
        QuickFixSessionIdentity{
            "FIX.4.4",
            "SORX_" + std::to_string(venue),
            "MOCKX_" + std::to_string(venue),
            "PRIMARY",
        },
        MockXFix44Config{2U, 0U, 32U, "SX"},
    };
}

[[nodiscard]] NewOrderIntent new_order(
    const QuickFixSessionConfig& session) {
    NewOrderIntent intent;
    intent.venue = session.venue;
    intent.parent = ParentOrderId{7};
    intent.child = ChildOrderId{9};
    intent.instrument = InstrumentId{3};
    intent.side = Side::Buy;
    intent.quantity = Quantity{25};
    intent.limit_price = Price{123456};
    intent.time_in_force = TimeInForce::ImmediateOrCancel;
    intent.parent_revision = 2;
    intent.session_epoch = session.session_epoch;
    intent.wire_order_id = "SXN42-2-9";
    intent.wire_symbol = "ALPHA";
    intent.dispatch_attempt_id = 11U;
    return intent;
}

[[nodiscard]] CancelOrderIntent cancel_order(
    const QuickFixSessionConfig& session) {
    CancelOrderIntent intent;
    intent.venue = session.venue;
    intent.parent = ParentOrderId{7};
    intent.child = ChildOrderId{9};
    intent.side = Side::Buy;
    intent.original_quantity = Quantity{25};
    intent.session_epoch = session.session_epoch;
    intent.original_wire_order_id = "SXN42-2-9";
    intent.cancel_wire_order_id = "SXC42-2-9";
    intent.wire_symbol = "ALPHA";
    intent.dispatch_attempt_id = 12U;
    return intent;
}

[[nodiscard]] const std::string& field(
    const FixMessage& message,
    const int tag) {
    for (const auto& candidate : message.fields) {
        if (candidate.tag == tag) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing FIX field");
}

void make_ready(
    QuickFixSessionAdapter& adapter,
    const QuickFixSessionConfig& session) {
    assert(adapter.start());
    assert(adapter.state() == OrderEntryState::Connecting);
    assert(adapter.ingest_transport_session(
        TransportSessionSignal::Created,
        session.identity));
    assert(adapter.ingest_transport_session(
        TransportSessionSignal::LoggedOn,
        session.identity));
    assert(adapter.state() == OrderEntryState::Ready);
}

[[nodiscard]] std::vector<FixField> trade_fields(
    const std::string& execution_id = "EXEC-1") {
    return {
        {11, "SXN42-2-9"},
        {17, execution_id},
        {150, "F"},
        {39, "1"},
        {14, "5"},
        {151, "20"},
        {32, "5"},
        {31, "1234.56"},
    };
}

void translation_and_dispatch_tests() {
    CapturingSink sink;
    TransportCapture transport;
    const auto session = config();
    QuickFixSessionAdapter adapter(session, sink, transport.seam());

    const auto before_ready = adapter.send_new(new_order(session));
    assert(before_ready.state == LocalDispatchState::DefinitelyUnsent);
    assert(before_ready.kind == DispatchAttemptKind::New);
    assert(before_ready.attempt_id == 11U);
    assert(before_ready.venue == session.venue);
    assert(before_ready.session_epoch == session.session_epoch);
    assert(before_ready.wire_order_id == "SXN42-2-9");
    assert(transport.messages.empty());
    assert(sink.dispatches.size() == 1);

    make_ready(adapter, session);
    assert(transport.started);
    assert(adapter.config().session_epoch == 42);
    assert(adapter.config().identity == session.identity);

    const auto sent = adapter.send_new(new_order(session));
    assert(sent.state == LocalDispatchState::AcceptedLocally);
    assert(sent.kind == DispatchAttemptKind::New);
    assert(sent.attempt_id == 11U);
    assert(sent.wire_order_id == "SXN42-2-9");
    assert(transport.messages.size() == 1);
    const auto& message = transport.messages.back();
    assert(message.message_type == "D");
    assert(field(message, 11) == "SXN42-2-9");
    assert(field(message, 38) == "25");
    assert(field(message, 44) == "1234.56");
    assert(field(message, 59) == "3");
    assert(field(message, 60) == "20260815-12:34:56.789");

    const auto canceled = adapter.send_cancel(cancel_order(session));
    assert(canceled.state == LocalDispatchState::AcceptedLocally);
    assert(canceled.kind == DispatchAttemptKind::Cancel);
    assert(canceled.attempt_id == 12U);
    assert(canceled.wire_order_id == "SXC42-2-9");
    assert(transport.messages.size() == 2);
    const auto& cancel = transport.messages.back();
    assert(cancel.message_type == "F");
    assert(field(cancel, 11) == "SXC42-2-9");
    assert(field(cancel, 41) == "SXN42-2-9");
    assert(field(cancel, 60) == "20260815-12:34:56.789");
    assert(sink.dispatches.size() == 3);

    adapter.stop();
    assert(adapter.state() == OrderEntryState::Disconnected);
    assert(transport.stop_calls == 1);
    assert(sink.sessions.back().state == OrderEntryState::Disconnected);
}

void inbound_normalization_and_dedupe_tests() {
    CapturingSink sink;
    TransportCapture transport;
    const auto session = config();
    QuickFixSessionAdapter adapter(session, sink, transport.seam());
    make_ready(adapter, session);

    const auto trade = trade_fields();
    assert(adapter.ingest_transport_application(
        session.identity,
        "8",
        77,
        trade));
    assert(sink.executions.size() == 1);
    assert(sink.executions[0].venue == VenueId{1});
    assert(sink.executions[0].session_epoch == 42);
    assert(sink.executions[0].execution_id == "EXEC-1");
    assert(sink.executions[0].kind == ExecutionKind::Trade);
    assert(sink.executions[0].last_quantity == Quantity{5});
    assert(sink.executions[0].last_price == Price{123456});

    assert(adapter.ingest_transport_application(
        session.identity,
        "8",
        78,
        trade));
    assert(sink.executions.size() == 1);

    const std::vector<FixField> cancel_reject{
        {11, "SXC42-2-9"},
        {41, "SXN42-2-9"},
        {58, "too late"},
    };
    assert(adapter.ingest_transport_application(
        session.identity,
        "9",
        88,
        cancel_reject));
    assert(sink.executions.size() == 2);
    assert(sink.executions[1].kind == ExecutionKind::CancelRejected);
    assert(sink.executions[1].wire_order_id == "SXN42-2-9");
    assert(sink.executions[1].durable_report_key == "CXLREJ-42-88");
}

void session_resolution_and_failure_tests() {
    CapturingSink sink;
    TransportCapture transport;
    const auto session = config();
    QuickFixSessionAdapter adapter(session, sink, transport.seam());
    make_ready(adapter, session);

    auto wrong = session.identity;
    wrong.target_comp_id = "OTHER";
    assert(!adapter.ingest_transport_session(
        TransportSessionSignal::LoggedOn,
        wrong));
    assert(adapter.state() == OrderEntryState::Degraded);
    assert(adapter.last_error().find("unknown") != std::string::npos);

    bool threw = false;
    try {
        auto invalid = session;
        invalid.session_epoch = 0;
        QuickFixSessionAdapter rejected(invalid, sink, transport.seam());
        static_cast<void>(rejected);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    CapturingSink start_sink;
    TransportCapture failed_transport;
    failed_transport.start_result = false;
    QuickFixSessionAdapter failed(session, start_sink, failed_transport.seam());
    assert(!failed.start());
    assert(failed.state() == OrderEntryState::Disconnected);
    assert(failed.last_error() == "offline start refused");
    assert(start_sink.sessions.front().state == OrderEntryState::Connecting);
    assert(start_sink.sessions.back().state == OrderEntryState::Disconnected);
}

void bounded_callback_failure_tests() {
    const auto session = config();

    CapturingSink report_sink;
    TransportCapture report_transport;
    QuickFixSessionAdapter report_adapter(
        session,
        report_sink,
        report_transport.seam());
    make_ready(report_adapter, session);
    report_sink.accept_execution = false;
    const auto report = trade_fields("EXEC-RETRY");
    assert(!report_adapter.ingest_transport_application(
        session.identity,
        "8",
        90,
        report));
    assert(report_adapter.state() == OrderEntryState::Degraded);
    assert(report_adapter.callback_failures() == 1);
    assert(report_adapter.last_error().find("capacity") != std::string::npos);
    assert(report_sink.executions.empty());

    report_sink.accept_execution = true;
    assert(report_adapter.ingest_transport_application(
        session.identity,
        "8",
        91,
        report));
    assert(report_sink.executions.size() == 1);

    CapturingSink dispatch_sink;
    TransportCapture dispatch_transport;
    QuickFixSessionAdapter dispatch_adapter(
        session,
        dispatch_sink,
        dispatch_transport.seam());
    make_ready(dispatch_adapter, session);
    dispatch_sink.accept_dispatch = false;
    const auto result = dispatch_adapter.send_new(new_order(session));
    assert(result.state == LocalDispatchState::AcceptedLocally);
    assert(dispatch_transport.messages.size() == 1);
    assert(dispatch_adapter.state() == OrderEntryState::Degraded);
    assert(dispatch_adapter.callback_failures() == 1);

    CapturingSink session_sink;
    session_sink.accept_session = false;
    TransportCapture session_transport;
    QuickFixSessionAdapter session_adapter(
        session,
        session_sink,
        session_transport.seam());
    assert(!session_adapter.start());
    assert(!session_transport.started);
    assert(session_adapter.state() == OrderEntryState::Degraded);
    assert(session_adapter.callback_failures() == 1);
}

void malformed_message_tests() {
    CapturingSink sink;
    TransportCapture transport;
    const auto session = config();
    QuickFixSessionAdapter adapter(session, sink, transport.seam());
    make_ready(adapter, session);

    auto malformed = trade_fields();
    malformed.erase(malformed.begin() + 5);
    assert(!adapter.ingest_transport_application(
        session.identity,
        "8",
        100,
        malformed));
    assert(adapter.state() == OrderEntryState::Degraded);
    assert(sink.executions.empty());

    assert(!adapter.ingest_transport_application(
        session.identity,
        "Z",
        101,
        {}));
    assert(adapter.last_error().find("unsupported") != std::string::npos);
}

}  // namespace

int main() {
    translation_and_dispatch_tests();
    inbound_normalization_and_dedupe_tests();
    session_resolution_and_failure_tests();
    bounded_callback_failure_tests();
    malformed_message_tests();
    return 0;
}




