#include "sorx/adapters/quickfix_session.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <quickfix/Application.h>
#include <quickfix/Exceptions.h>
#include <quickfix/Fields.h>
#include <quickfix/FileStore.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sorx::adapters::quickfix {
namespace {

[[nodiscard]] bool printable_identity(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 255U &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte >= 0x21U && byte <= 0x7eU &&
                      character != '=' && character != '\x01';
           });
}

void validate_config(const QuickFixSessionConfig& config) {
    if (!config.venue || config.session_epoch == 0U ||
        config.settings_file.empty() ||
        config.identity.begin_string != "FIX.4.4" ||
        !printable_identity(config.identity.sender_comp_id) ||
        !printable_identity(config.identity.target_comp_id) ||
        (!config.identity.session_qualifier.empty() &&
         !printable_identity(config.identity.session_qualifier))) {
        throw std::invalid_argument("invalid QuickFIX session configuration");
    }
}

[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &seconds) != 0) {
        throw std::runtime_error("failed to convert FIX TransactTime to UTC");
    }
#else
    if (gmtime_r(&seconds, &utc) == nullptr) {
        throw std::runtime_error("failed to convert FIX TransactTime to UTC");
    }
#endif
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::array<char, 32> buffer{};
    const auto written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d%02d%02d-%02d:%02d:%02d.%03lld",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        static_cast<long long>(milliseconds.count()));
    if (written <= 0 ||
        static_cast<std::size_t>(written) >= buffer.size()) {
        throw std::runtime_error("failed to format FIX TransactTime");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] bool valid_utc_timestamp(const std::string_view value) noexcept {
    if (value.size() != 21U || value[8] != '-' || value[11] != ':' ||
        value[14] != ':' || value[17] != '.') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 11U || index == 14U || index == 17U) {
            continue;
        }
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QuickFixSessionIdentity neutral_identity(
    const FIX::SessionID& session_id) {
    return QuickFixSessionIdentity{
        session_id.getBeginString().getValue(),
        session_id.getSenderCompID().getValue(),
        session_id.getTargetCompID().getValue(),
        session_id.getSessionQualifier(),
    };
}

[[nodiscard]] FIX::SessionID native_identity(
    const QuickFixSessionIdentity& identity) {
    return FIX::SessionID{
        identity.begin_string,
        identity.sender_comp_id,
        identity.target_comp_id,
        identity.session_qualifier,
    };
}

[[nodiscard]] std::uint64_t header_sequence(
    const FIX::Message& message) noexcept {
    try {
        if (!message.getHeader().isSetField(34)) {
            return 0;
        }
        const auto& text = message.getHeader().getField(34);
        std::uint64_t sequence = 0;
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), sequence);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size()) {
            return 0;
        }
        return sequence;
    } catch (...) {
        return 0;
    }
}

[[nodiscard]] std::vector<mockx::FixField> extract_fields(
    const FIX::Message& message,
    const std::span<const int> tags) {
    std::vector<mockx::FixField> fields;
    fields.reserve(tags.size());
    for (const auto tag : tags) {
        if (message.isSetField(tag)) {
            fields.push_back(mockx::FixField{tag, message.getField(tag)});
        }
    }
    return fields;
}

[[nodiscard]] FIX::Message native_message(
    const mockx::FixMessage& message) {
    if (message.message_type.empty()) {
        throw std::invalid_argument("FIX message type is empty");
    }
    FIX::Message output;
    output.getHeader().setField(FIX::MsgType{message.message_type});
    std::set<int> tags;
    for (const auto& field : message.fields) {
        if (field.tag <= 0 || !tags.insert(field.tag).second ||
            field.value.find('\x01') != std::string::npos) {
            throw std::invalid_argument("invalid or duplicate FIX message field");
        }
        output.setField(FIX::StringField{field.tag, field.value});
    }
    return output;
}

[[nodiscard]] mockx::FixMessage with_transact_time(
    mockx::FixMessage message,
    std::string timestamp) {
    if (!valid_utc_timestamp(timestamp)) {
        throw std::invalid_argument("invalid FIX TransactTime");
    }
    const auto duplicate = std::find_if(
        message.fields.begin(),
        message.fields.end(),
        [](const mockx::FixField& field) { return field.tag == 60; });
    if (duplicate != message.fields.end()) {
        throw std::invalid_argument("FIX profile emitted duplicate TransactTime");
    }
    message.fields.push_back(mockx::FixField{60, std::move(timestamp)});
    return message;
}

}  // namespace

struct QuickFixSessionAdapter::Impl final : FIX::Application {
    QuickFixSessionConfig config;
    IQuickFixSessionSink& sink;
    mockx::MockXFix44Profile profile;
    std::optional<QuickFixTestTransport> test_transport;

    std::unique_ptr<FIX::SessionSettings> settings;
    std::unique_ptr<FIX::FileStoreFactory> store_factory;
    std::unique_ptr<FIX::SocketInitiator> initiator;
    std::optional<FIX::SessionID> session_id;

    std::atomic<OrderEntryState> state{OrderEntryState::Disconnected};
    std::atomic<std::uint64_t> callback_failure_count{};
    std::atomic<bool> started{};
    std::atomic<bool> stopping{};
    mutable std::mutex error_mutex;
    std::string error;
    std::mutex dedupe_mutex;
    std::set<std::tuple<VenueId, std::uint64_t, std::string>> seen_execution_ids;

    Impl(
        QuickFixSessionConfig input_config,
        IQuickFixSessionSink& input_sink,
        std::optional<QuickFixTestTransport> input_test_transport)
        : config(std::move(input_config)),
          sink(input_sink),
          profile(config.profile),
          test_transport(std::move(input_test_transport)) {
        validate_config(config);
        if (test_transport &&
            (!test_transport->start || !test_transport->stop ||
             !test_transport->send || !test_transport->utc_timestamp)) {
            throw std::invalid_argument("incomplete QuickFIX test transport");
        }
    }

    ~Impl() override {
        shutdown_transport();
    }

    void set_error(std::string detail) noexcept {
        try {
            std::lock_guard lock(error_mutex);
            error = std::move(detail);
        } catch (...) {
        }
    }

    [[nodiscard]] std::string error_copy() const {
        std::lock_guard lock(error_mutex);
        return error;
    }

    void callback_failed(const std::string_view kind) noexcept {
        callback_failure_count.fetch_add(1U, std::memory_order_relaxed);
        state.store(OrderEntryState::Degraded, std::memory_order_release);
        set_error(std::string{kind} + " callback capacity exhausted");
    }

    [[nodiscard]] bool publish_session(const OrderEntryState next) noexcept {
        state.store(next, std::memory_order_release);
        try {
            if (sink.try_publish(OrderEntrySessionEvent{
                    config.venue,
                    config.session_epoch,
                    next,
                })) {
                return true;
            }
        } catch (...) {
        }
        callback_failed("order-entry session");
        return false;
    }

    [[nodiscard]] bool publish_dispatch(LocalDispatchResult result) noexcept {
        try {
            if (sink.try_publish(std::move(result))) {
                return true;
            }
        } catch (...) {
        }
        callback_failed("local dispatch");
        return false;
    }

    [[nodiscard]] bool publish_execution(
        NormalizedExecutionReport report) noexcept {
        try {
            if (sink.try_publish(std::move(report))) {
                return true;
            }
        } catch (...) {
        }
        callback_failed("execution report");
        return false;
    }

    void degrade(std::string detail) noexcept {
        set_error(std::move(detail));
        state.store(OrderEntryState::Degraded, std::memory_order_release);
        try {
            if (!sink.try_publish(OrderEntrySessionEvent{
                    config.venue,
                    config.session_epoch,
                    OrderEntryState::Degraded,
                })) {
                callback_failed("degraded session");
            }
        } catch (...) {
            callback_failed("degraded session");
        }
    }

    [[nodiscard]] bool identity_matches(
        const QuickFixSessionIdentity& identity) const noexcept {
        return identity == config.identity;
    }

    [[nodiscard]] bool production_start(std::string& detail) noexcept {
        try {
            settings = std::make_unique<FIX::SessionSettings>(
                config.settings_file.string());
            const auto sessions = settings->getSessions();
            const auto expected = native_identity(config.identity);
            if (settings->size() != 1U || sessions.size() != 1U ||
                !sessions.contains(expected)) {
                throw std::runtime_error(
                    "QuickFIX settings must resolve exactly the configured session");
            }
            const auto& dictionary = settings->get(expected);
            if (!dictionary.has(FIX::FILE_STORE_PATH) ||
                dictionary.getString(FIX::FILE_STORE_PATH).empty()) {
                throw std::runtime_error(
                    "QuickFIX session requires persistent FileStorePath");
            }
            session_id = expected;
            store_factory =
                std::make_unique<FIX::FileStoreFactory>(*settings);
            initiator = std::make_unique<FIX::SocketInitiator>(
                *this,
                *store_factory,
                *settings);
            initiator->start();
            return true;
        } catch (const std::exception& exception) {
            detail = exception.what();
        } catch (...) {
            detail = "unknown QuickFIX startup failure";
        }
        if (initiator) {
            try {
                initiator->stop(true);
            } catch (...) {
            }
        }
        initiator.reset();
        store_factory.reset();
        settings.reset();
        session_id.reset();
        return false;
    }

    [[nodiscard]] bool start_transport(std::string& detail) noexcept {
        if (test_transport) {
            try {
                return test_transport->start(detail);
            } catch (const std::exception& exception) {
                detail = exception.what();
                return false;
            } catch (...) {
                detail = "unknown test transport startup failure";
                return false;
            }
        }
        return production_start(detail);
    }

    void shutdown_transport() noexcept {
        const bool was_started = started.load(std::memory_order_acquire);
        if (!was_started && !initiator) {
            store_factory.reset();
            settings.reset();
            session_id.reset();
            return;
        }
        stopping.store(true, std::memory_order_release);
        if (test_transport) {
            try {
                test_transport->stop();
            } catch (...) {
            }
        } else if (initiator) {
            try {
                initiator->stop(true);
            } catch (...) {
            }
        }
        initiator.reset();
        store_factory.reset();
        settings.reset();
        session_id.reset();
        started.store(false, std::memory_order_release);
        stopping.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool start_adapter() {
        bool expected = false;
        if (!started.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return state.load(std::memory_order_acquire) !=
                   OrderEntryState::Disconnected;
        }
        if (!publish_session(OrderEntryState::Connecting)) {
            started.store(false, std::memory_order_release);
            return false;
        }

        std::string detail;
        if (!start_transport(detail)) {
            if (test_transport) {
                try {
                    test_transport->stop();
                } catch (...) {
                }
            }
            started.store(false, std::memory_order_release);
            set_error(detail.empty() ? "QuickFIX transport start failed" : detail);
            static_cast<void>(publish_session(OrderEntryState::Disconnected));
            return false;
        }
        if (state.load(std::memory_order_acquire) ==
            OrderEntryState::Degraded) {
            shutdown_transport();
            return false;
        }
        return true;
    }

    void stop_adapter() noexcept {
        shutdown_transport();
        static_cast<void>(publish_session(OrderEntryState::Disconnected));
    }

    [[nodiscard]] std::string current_timestamp() {
        return test_transport ? test_transport->utc_timestamp() : utc_timestamp();
    }

    [[nodiscard]] QuickFixTransportSendResult production_send(
        const mockx::FixMessage& message) noexcept {
        if (!session_id) {
            return QuickFixTransportSendResult{
                LocalDispatchState::DefinitelyUnsent,
                "QuickFIX session has not been resolved",
            };
        }
        try {
            auto native = native_message(message);
            if (FIX::Session::sendToTarget(native, *session_id)) {
                return QuickFixTransportSendResult{
                    LocalDispatchState::AcceptedLocally,
                    "QuickFIX accepted the message locally",
                };
            }
            return QuickFixTransportSendResult{
                LocalDispatchState::DefinitelyUnsent,
                "QuickFIX refused the message before local acceptance",
            };
        } catch (const FIX::SessionNotFound& exception) {
            return QuickFixTransportSendResult{
                LocalDispatchState::DefinitelyUnsent,
                exception.what(),
            };
        } catch (const std::exception& exception) {
            return QuickFixTransportSendResult{
                LocalDispatchState::Unknown,
                exception.what(),
            };
        } catch (...) {
            return QuickFixTransportSendResult{
                LocalDispatchState::Unknown,
                "unknown QuickFIX send failure",
            };
        }
    }

    [[nodiscard]] QuickFixTransportSendResult transport_send(
        const mockx::FixMessage& message) noexcept {
        if (test_transport) {
            try {
                return test_transport->send(message);
            } catch (const std::exception& exception) {
                return QuickFixTransportSendResult{
                    LocalDispatchState::Unknown,
                    exception.what(),
                };
            } catch (...) {
                return QuickFixTransportSendResult{
                    LocalDispatchState::Unknown,
                    "unknown test transport send failure",
                };
            }
        }
        return production_send(message);
    }

    template <typename Intent>
    [[nodiscard]] LocalDispatchResult dispatch_result(
        const Intent& intent,
        const LocalDispatchState state_value,
        std::string detail) const {
        constexpr bool is_new =
            std::is_same_v<std::remove_cvref_t<Intent>, NewOrderIntent>;
        const std::string& wire_order_id =
            [&]() -> const std::string& {
                if constexpr (is_new) {
                    return intent.wire_order_id;
                } else {
                    return intent.cancel_wire_order_id;
                }
            }();

        LocalDispatchResult result;
        result.child = intent.child;
        result.state = state_value;
        result.detail = std::move(detail);
        result.kind = is_new ?
            DispatchAttemptKind::New :
            DispatchAttemptKind::Cancel;
        result.attempt_id = intent.dispatch_attempt_id;
        result.venue = intent.venue;
        result.session_epoch = intent.session_epoch;
        result.wire_order_id = wire_order_id;
        return result;
    }

    template <typename Intent, typename Builder>
    [[nodiscard]] LocalDispatchResult send_intent(
        const Intent& intent,
        Builder&& builder) {
        const auto definitely_unsent =
            [this, &intent](std::string detail) {
                LocalDispatchResult result = dispatch_result(
                    intent,
                    LocalDispatchState::DefinitelyUnsent,
                    std::move(detail));
                static_cast<void>(publish_dispatch(result));
                return result;
            };

        if (intent.venue != config.venue ||
            intent.session_epoch != config.session_epoch) {
            return definitely_unsent(
                "intent does not match the configured venue/session epoch");
        }
        if (intent.dispatch_attempt_id == 0U) {
            return definitely_unsent(
                "intent has no durable dispatch-attempt identity");
        }
        if (!started.load(std::memory_order_acquire) ||
            state.load(std::memory_order_acquire) != OrderEntryState::Ready) {
            return definitely_unsent("QuickFIX session is not ready");
        }

        const auto built = std::forward<Builder>(builder)(intent);
        if (!built.ok) {
            return definitely_unsent(built.error);
        }

        mockx::FixMessage message;
        try {
            message = with_transact_time(
                built.message,
                current_timestamp());
        } catch (const std::exception& exception) {
            return definitely_unsent(exception.what());
        } catch (...) {
            return definitely_unsent("failed to create FIX TransactTime");
        }

        auto transport_result = transport_send(message);
        LocalDispatchResult result = dispatch_result(
            intent,
            transport_result.state,
            std::move(transport_result.detail));
        static_cast<void>(publish_dispatch(result));
        return result;
    }

    [[nodiscard]] LocalDispatchResult send_new(
        const NewOrderIntent& intent) {
        return send_intent(
            intent,
            [this](const NewOrderIntent& value) {
                return profile.make_new_order(value);
            });
    }

    [[nodiscard]] LocalDispatchResult send_cancel(
        const CancelOrderIntent& intent) {
        return send_intent(
            intent,
            [this](const CancelOrderIntent& value) {
                return profile.make_cancel(value);
            });
    }

    [[nodiscard]] bool ingest_session(
        const TransportSessionSignal signal,
        const QuickFixSessionIdentity& identity) {
        if (!identity_matches(identity)) {
            degrade("transport event belongs to an unknown QuickFIX session");
            return false;
        }
        if (!started.load(std::memory_order_acquire)) {
            degrade("transport event arrived while QuickFIX adapter is stopped");
            return false;
        }

        switch (signal) {
            case TransportSessionSignal::Created:
                return publish_session(OrderEntryState::Connecting);
            case TransportSessionSignal::LoggedOn:
                return publish_session(OrderEntryState::Ready);
            case TransportSessionSignal::LoggedOut:
                if (stopping.load(std::memory_order_acquire)) {
                    return true;
                }
                return publish_session(OrderEntryState::Disconnected);
        }
        degrade("unknown QuickFIX transport session signal");
        return false;
    }

    [[nodiscard]] bool ingest_application(
        const QuickFixSessionIdentity& identity,
        const std::string_view message_type,
        const std::uint64_t inbound_session_sequence,
        const std::span<const mockx::FixField> fields) {
        if (!identity_matches(identity)) {
            degrade("application message belongs to an unknown QuickFIX session");
            return false;
        }
        if (!started.load(std::memory_order_acquire)) {
            degrade("application message arrived while QuickFIX adapter is stopped");
            return false;
        }
        if (message_type == "8") {
            auto normalized = profile.normalize_execution_report(
                config.venue,
                config.session_epoch,
                fields);
            if (!normalized.ok) {
                degrade("invalid FIX execution report: " + normalized.error);
                return false;
            }

            const auto key = std::tuple{
                normalized.report.venue,
                normalized.report.session_epoch,
                normalized.report.execution_id,
            };
            std::lock_guard lock(dedupe_mutex);
            if (seen_execution_ids.contains(key)) {
                return true;
            }
            if (!publish_execution(normalized.report)) {
                return false;
            }
            seen_execution_ids.insert(key);
            return true;
        }
        if (message_type == "9") {
            auto normalized = profile.normalize_cancel_reject(
                config.venue,
                config.session_epoch,
                inbound_session_sequence,
                fields);
            if (!normalized.ok) {
                degrade("invalid FIX cancel reject: " + normalized.error);
                return false;
            }
            return publish_execution(std::move(normalized.report));
        }

        degrade("unsupported FIX application message type " +
                std::string{message_type});
        return false;
    }

    void onCreate(const FIX::SessionID& native) override {
        static_cast<void>(ingest_session(
            TransportSessionSignal::Created,
            neutral_identity(native)));
    }

    void onLogon(const FIX::SessionID& native) override {
        static_cast<void>(ingest_session(
            TransportSessionSignal::LoggedOn,
            neutral_identity(native)));
    }

    void onLogout(const FIX::SessionID& native) override {
        static_cast<void>(ingest_session(
            TransportSessionSignal::LoggedOut,
            neutral_identity(native)));
    }

    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}

    void toApp(FIX::Message&, const FIX::SessionID& native) override {
        if (!identity_matches(neutral_identity(native))) {
            degrade("outbound application message belongs to an unknown session");
        }
    }

    void fromAdmin(
        const FIX::Message& message,
        const FIX::SessionID& native) override {
        try {
            if (!identity_matches(neutral_identity(native))) {
                degrade("admin message belongs to an unknown QuickFIX session");
                return;
            }
            if (message.getHeader().getField(35) == "3") {
                degrade("QuickFIX session reject received");
            }
        } catch (const std::exception& exception) {
            degrade(std::string{"invalid QuickFIX admin message: "} +
                    exception.what());
        } catch (...) {
            degrade("invalid QuickFIX admin message");
        }
    }

    void fromApp(
        const FIX::Message& message,
        const FIX::SessionID& native) override {
        try {
            const auto type = message.getHeader().getField(35);
            static constexpr std::array execution_tags{
                11, 17, 150, 39, 14, 151, 32, 31, 58,
            };
            static constexpr std::array cancel_reject_tags{
                11, 41, 58,
            };
            const auto tags = type == "8"
                                  ? std::span<const int>{execution_tags}
                                  : std::span<const int>{cancel_reject_tags};
            const auto fields = extract_fields(message, tags);
            static_cast<void>(ingest_application(
                neutral_identity(native),
                type,
                header_sequence(message),
                fields));
        } catch (const std::exception& exception) {
            degrade(std::string{"invalid QuickFIX application message: "} +
                    exception.what());
        } catch (...) {
            degrade("invalid QuickFIX application message");
        }
    }
};

QuickFixSessionAdapter::QuickFixSessionAdapter(
    QuickFixSessionConfig config,
    IQuickFixSessionSink& sink)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          sink,
          std::nullopt)) {}

QuickFixSessionAdapter::QuickFixSessionAdapter(
    QuickFixSessionConfig config,
    IQuickFixSessionSink& sink,
    QuickFixTestTransport test_transport)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          sink,
          std::move(test_transport))) {}

QuickFixSessionAdapter::~QuickFixSessionAdapter() = default;
QuickFixSessionAdapter::QuickFixSessionAdapter(
    QuickFixSessionAdapter&&) noexcept = default;
QuickFixSessionAdapter& QuickFixSessionAdapter::operator=(
    QuickFixSessionAdapter&&) noexcept = default;

bool QuickFixSessionAdapter::start() {
    return impl_->start_adapter();
}

void QuickFixSessionAdapter::stop() noexcept {
    impl_->stop_adapter();
}

LocalDispatchResult QuickFixSessionAdapter::send_new(
    const NewOrderIntent& intent) {
    return impl_->send_new(intent);
}

LocalDispatchResult QuickFixSessionAdapter::send_cancel(
    const CancelOrderIntent& intent) {
    return impl_->send_cancel(intent);
}

bool QuickFixSessionAdapter::ingest_transport_session(
    const TransportSessionSignal signal,
    const QuickFixSessionIdentity& identity) {
    return impl_->ingest_session(signal, identity);
}

bool QuickFixSessionAdapter::ingest_transport_application(
    const QuickFixSessionIdentity& identity,
    const std::string_view message_type,
    const std::uint64_t inbound_session_sequence,
    const std::span<const mockx::FixField> fields) {
    return impl_->ingest_application(
        identity,
        message_type,
        inbound_session_sequence,
        fields);
}

OrderEntryState QuickFixSessionAdapter::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}

std::uint64_t QuickFixSessionAdapter::callback_failures() const noexcept {
    return impl_->callback_failure_count.load(std::memory_order_acquire);
}

std::string QuickFixSessionAdapter::last_error() const {
    return impl_->error_copy();
}

const QuickFixSessionConfig& QuickFixSessionAdapter::config() const noexcept {
    return impl_->config;
}

}  // namespace sorx::adapters::quickfix






