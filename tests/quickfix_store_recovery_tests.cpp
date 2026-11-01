#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "sorx/adapters/quickfix_session.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <quickfix/FileStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/SessionSettings.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace sorx;
using namespace sorx::adapters;
using namespace sorx::adapters::mockx;
using namespace sorx::adapters::quickfix;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        const auto tick = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = base /
                ("sorx-quickfix-recovery-" + std::to_string(tick) + "-" +
                 std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error &&
                error != std::make_error_code(
                             std::errc::file_exists)) {
                throw std::runtime_error(
                    "failed to create a temporary QuickFIX test directory");
            }
        }
        throw std::runtime_error(
            "could not allocate a unique QuickFIX test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Holding a loopback port bound but not listening makes connection attempts
// fail locally while preventing another process from racing to accept a FIX
// logon on that port. No external endpoint is needed by this test.
class ReservedLoopbackPort final {
public:
    ReservedLoopbackPort() {
#ifdef _WIN32
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        winsock_started_ = true;
#endif

        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == invalid_socket()) {
            cleanup();
            throw std::runtime_error("could not reserve a loopback socket");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0U);
        if (::bind(
                socket_,
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) != 0) {
            cleanup();
            throw std::runtime_error("could not bind a loopback socket");
        }

#ifdef _WIN32
        int address_size = static_cast<int>(sizeof(address));
#else
        socklen_t address_size = static_cast<socklen_t>(sizeof(address));
#endif
        if (::getsockname(
                socket_,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) != 0) {
            cleanup();
            throw std::runtime_error("could not inspect a loopback socket");
        }
        port_ = ntohs(address.sin_port);
        if (port_ == 0U) {
            cleanup();
            throw std::runtime_error("the operating system returned port zero");
        }
    }

    ~ReservedLoopbackPort() {
        cleanup();
    }

    ReservedLoopbackPort(const ReservedLoopbackPort&) = delete;
    ReservedLoopbackPort& operator=(const ReservedLoopbackPort&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;

    [[nodiscard]] static constexpr Socket invalid_socket() noexcept {
        return INVALID_SOCKET;
    }
#else
    using Socket = int;

    [[nodiscard]] static constexpr Socket invalid_socket() noexcept {
        return -1;
    }
#endif

    void cleanup() noexcept {
        if (socket_ != invalid_socket()) {
#ifdef _WIN32
            static_cast<void>(::closesocket(socket_));
#else
            static_cast<void>(::close(socket_));
#endif
            socket_ = invalid_socket();
        }
#ifdef _WIN32
        if (winsock_started_) {
            static_cast<void>(::WSACleanup());
            winsock_started_ = false;
        }
#endif
    }

    Socket socket_{invalid_socket()};
    std::uint16_t port_{};
#ifdef _WIN32
    bool winsock_started_{};
#endif
};

struct CapturingSink final : IQuickFixSessionSink {
    bool try_publish(OrderEntrySessionEvent event) override {
        std::lock_guard lock(mutex);
        sessions.push_back(std::move(event));
        return true;
    }

    bool try_publish(LocalDispatchResult result) override {
        std::lock_guard lock(mutex);
        dispatches.push_back(std::move(result));
        return true;
    }

    bool try_publish(NormalizedExecutionReport report) override {
        std::lock_guard lock(mutex);
        executions.push_back(std::move(report));
        return true;
    }

    std::mutex mutex;
    std::vector<OrderEntrySessionEvent> sessions;
    std::vector<LocalDispatchResult> dispatches;
    std::vector<NormalizedExecutionReport> executions;
};

[[nodiscard]] QuickFixSessionIdentity identity(
    const std::string& suffix) {
    return QuickFixSessionIdentity{
        "FIX.4.4",
        "SORX_RECOVERY_" + suffix,
        "MOCKX_RECOVERY_" + suffix,
        "",
    };
}

[[nodiscard]] FIX::SessionID native_identity(
    const QuickFixSessionIdentity& value) {
    return FIX::SessionID{
        value.begin_string,
        value.sender_comp_id,
        value.target_comp_id,
        value.session_qualifier,
    };
}

void write_settings(
    const std::filesystem::path& file,
    const QuickFixSessionIdentity& session,
    const std::uint16_t port,
    const std::optional<std::filesystem::path>& store_path) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create QuickFIX test settings");
    }

    output <<
        "[DEFAULT]\n"
        "ConnectionType=initiator\n"
        "ReconnectInterval=1\n"
        "StartTime=00:00:00\n"
        "EndTime=00:00:00\n"
        "UseDataDictionary=N\n"
        "HeartBtInt=30\n"
        "ResetOnLogon=N\n"
        "ResetOnLogout=N\n"
        "ResetOnDisconnect=N\n"
        "RefreshOnLogon=Y\n";
    if (store_path) {
        output << "FileStorePath=" << store_path->generic_string() << '\n';
    }
    output <<
        "\n[SESSION]\n"
        "BeginString=" << session.begin_string << '\n'
           << "SenderCompID=" << session.sender_comp_id << '\n'
           << "TargetCompID=" << session.target_comp_id << '\n';
    if (!session.session_qualifier.empty()) {
        output << "SessionQualifier=" << session.session_qualifier << '\n';
    }
    output <<
        "SocketConnectHost=127.0.0.1\n"
        "SocketConnectPort=" << port << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("could not finish QuickFIX test settings");
    }
}

[[nodiscard]] QuickFixSessionConfig adapter_config(
    const std::filesystem::path& settings_file,
    const QuickFixSessionIdentity& session) {
    return QuickFixSessionConfig{
        VenueId{7},
        91U,
        settings_file,
        session,
        MockXFix44Config{2U, 0U, 32U, "SX"},
    };
}

[[nodiscard]] NewOrderIntent pre_logon_order(
    const QuickFixSessionConfig& config) {
    NewOrderIntent intent;
    intent.venue = config.venue;
    intent.parent = ParentOrderId{1};
    intent.child = ChildOrderId{2};
    intent.instrument = InstrumentId{3};
    intent.side = Side::Buy;
    intent.quantity = Quantity{5};
    intent.limit_price = Price{10000};
    intent.time_in_force = TimeInForce::ImmediateOrCancel;
    intent.session_epoch = config.session_epoch;
    intent.wire_order_id = "SXN91-1-2";
    intent.wire_symbol = "ALPHA";
    intent.dispatch_attempt_id = 1U;
    return intent;
}

void persistent_file_store_round_trip_and_adapter_restore_test(
    const std::filesystem::path& root) {
    const ReservedLoopbackPort reserved_port;
    const auto session = identity("ROUND_TRIP");
    const auto session_id = native_identity(session);
    const auto store_path = root / "persistent-store";
    const auto settings_file = root / "persistent.cfg";
    assert(std::filesystem::create_directory(store_path));
    write_settings(
        settings_file,
        session,
        reserved_port.port(),
        store_path);

    constexpr FIX::SEQNUM sender_sequence = 17;
    constexpr FIX::SEQNUM target_sequence = 29;
    constexpr FIX::SEQNUM stored_message_sequence = 16;
    const std::string stored_message =
        "8=FIX.4.4\x01"
        "9=12\x01"
        "35=D\x01"
        "11=RECOVERED\x01";

    {
        const FIX::SessionSettings settings(settings_file.string());
        assert(settings.size() == 1U);
        assert(settings.has(session_id));
        FIX::FileStoreFactory factory(settings);
        FIX::MessageStore* const store =
            factory.create(FIX::UtcTimeStamp::now(), session_id);
        assert(store != nullptr);
        store->setNextSenderMsgSeqNum(sender_sequence);
        store->setNextTargetMsgSeqNum(target_sequence);
        assert(store->set(stored_message_sequence, stored_message));
        factory.destroy(store);
    }

    {
        const FIX::SessionSettings reopened_settings(settings_file.string());
        FIX::FileStoreFactory reopened_factory(reopened_settings);
        FIX::MessageStore* const reopened =
            reopened_factory.create(FIX::UtcTimeStamp::now(), session_id);
        assert(reopened != nullptr);
        assert(reopened->getNextSenderMsgSeqNum() == sender_sequence);
        assert(reopened->getNextTargetMsgSeqNum() == target_sequence);
        std::vector<std::string> recovered_messages;
        reopened->get(
            stored_message_sequence,
            stored_message_sequence,
            recovered_messages);
        assert(recovered_messages.size() == 1U);
        assert(recovered_messages.front() == stored_message);
        reopened_factory.destroy(reopened);
    }

    CapturingSink sink;
    const auto config = adapter_config(settings_file, session);
    QuickFixSessionAdapter adapter(config, sink);
    assert(FIX::Session::lookupSession(session_id) == nullptr);

    const auto before_start = adapter.send_new(pre_logon_order(config));
    assert(before_start.state == LocalDispatchState::DefinitelyUnsent);
    assert(before_start.detail.find("not ready") != std::string::npos);

    // Production start constructs FileStoreFactory and the native Session.
    // The loopback port cannot accept a connection, so these persisted values
    // are observable before logon and before any application order can route.
    assert(adapter.start());
    FIX::Session* const native_session =
        FIX::Session::lookupSession(session_id);
    assert(native_session != nullptr);
    assert(!native_session->isLoggedOn());
    assert(adapter.state() != OrderEntryState::Ready);
    assert(native_session->getStore()->getNextSenderMsgSeqNum() ==
           sender_sequence);
    assert(native_session->getStore()->getNextTargetMsgSeqNum() ==
           target_sequence);
    std::vector<std::string> adapter_recovered_messages;
    native_session->getStore()->get(
        stored_message_sequence,
        stored_message_sequence,
        adapter_recovered_messages);
    assert(adapter_recovered_messages.size() == 1U);
    assert(adapter_recovered_messages.front() == stored_message);

    const auto before_logon = adapter.send_new(pre_logon_order(config));
    assert(before_logon.state == LocalDispatchState::DefinitelyUnsent);
    assert(before_logon.detail.find("not ready") != std::string::npos);
    assert(native_session->getStore()->getNextSenderMsgSeqNum() ==
           sender_sequence);
    assert(native_session->getStore()->getNextTargetMsgSeqNum() ==
           target_sequence);

    adapter.stop();
    assert(FIX::Session::lookupSession(session_id) == nullptr);
}

void missing_store_path_is_rejected_test(
    const std::filesystem::path& root) {
    const auto session = identity("MISSING_PATH");
    const auto settings_file = root / "missing-store.cfg";
    write_settings(settings_file, session, 1U, std::nullopt);

    CapturingSink sink;
    QuickFixSessionAdapter adapter(
        adapter_config(settings_file, session),
        sink);
    assert(!adapter.start());
    assert(adapter.state() == OrderEntryState::Disconnected);
    assert(adapter.last_error().find("persistent FileStorePath") !=
           std::string::npos);
    assert(FIX::Session::lookupSession(native_identity(session)) == nullptr);
}

void incompatible_store_path_is_rejected_test(
    const std::filesystem::path& root) {
    const auto session = identity("INCOMPATIBLE_PATH");
    const auto settings_file = root / "incompatible-store.cfg";
    const auto regular_file = root / "not-a-directory";
    {
        std::ofstream output(regular_file, std::ios::binary | std::ios::trunc);
        output << "this path cannot contain QuickFIX store files";
    }
    assert(std::filesystem::is_regular_file(regular_file));
    write_settings(settings_file, session, 1U, regular_file);

    CapturingSink sink;
    QuickFixSessionAdapter adapter(
        adapter_config(settings_file, session),
        sink);
    assert(!adapter.start());
    assert(adapter.state() == OrderEntryState::Disconnected);
    assert(!adapter.last_error().empty());
    assert(FIX::Session::lookupSession(native_identity(session)) == nullptr);
}

}  // namespace

int main() {
    TemporaryDirectory temporary;
    persistent_file_store_round_trip_and_adapter_restore_test(
        temporary.path());
    missing_store_path_is_rejected_test(temporary.path());
    incompatible_store_path_is_rejected_test(temporary.path());
    return 0;
}












