#pragma once

#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/order_core.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace sorx::adapters::quickfix {

struct QuickFixSessionIdentity final {
    std::string begin_string{"FIX.4.4"};
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string session_qualifier;

    friend bool operator==(
        const QuickFixSessionIdentity&,
        const QuickFixSessionIdentity&) = default;
};

struct QuickFixSessionConfig final {
    VenueId venue{};
    std::uint64_t session_epoch{};
    std::filesystem::path settings_file;
    QuickFixSessionIdentity identity;
    mockx::MockXFix44Config profile;
};

class IQuickFixSessionSink {
public:
    virtual ~IQuickFixSessionSink() = default;

    // These are bounded, nonblocking publication attempts. Returning false
    // means the event could not be retained and degrades the session.
    [[nodiscard]] virtual bool try_publish(OrderEntrySessionEvent event) = 0;
    [[nodiscard]] virtual bool try_publish(LocalDispatchResult result) = 0;
    [[nodiscard]] virtual bool try_publish(
        NormalizedExecutionReport report) = 0;
};

struct QuickFixTransportSendResult final {
    LocalDispatchState state{LocalDispatchState::Unknown};
    std::string detail;
};

// A protocol-neutral seam used by offline tests and alternate transports.
// Production construction owns the native initiator and does not use this.
struct QuickFixTestTransport final {
    std::function<bool(std::string& error)> start;
    std::function<void()> stop;
    std::function<QuickFixTransportSendResult(
        const mockx::FixMessage& message)> send;
    std::function<std::string()> utc_timestamp;
};

enum class TransportSessionSignal : std::uint8_t {
    Created,
    LoggedOn,
    LoggedOut,
};

class QuickFixSessionAdapter final {
public:
    QuickFixSessionAdapter(
        QuickFixSessionConfig config,
        IQuickFixSessionSink& sink);
    QuickFixSessionAdapter(
        QuickFixSessionConfig config,
        IQuickFixSessionSink& sink,
        QuickFixTestTransport test_transport);
    ~QuickFixSessionAdapter();

    QuickFixSessionAdapter(const QuickFixSessionAdapter&) = delete;
    QuickFixSessionAdapter& operator=(const QuickFixSessionAdapter&) = delete;
    QuickFixSessionAdapter(QuickFixSessionAdapter&&) noexcept;
    QuickFixSessionAdapter& operator=(QuickFixSessionAdapter&&) noexcept;

    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] LocalDispatchResult send_new(const NewOrderIntent& intent);
    [[nodiscard]] LocalDispatchResult send_cancel(
        const CancelOrderIntent& intent);

    // Production transport callbacks and offline tests enter through
    // the same protocol-neutral seams.
    [[nodiscard]] bool ingest_transport_session(
        TransportSessionSignal signal,
        const QuickFixSessionIdentity& identity);
    [[nodiscard]] bool ingest_transport_application(
        const QuickFixSessionIdentity& identity,
        std::string_view message_type,
        std::uint64_t inbound_session_sequence,
        std::span<const mockx::FixField> fields);

    [[nodiscard]] OrderEntryState state() const noexcept;
    [[nodiscard]] std::uint64_t callback_failures() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] const QuickFixSessionConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::adapters::quickfix


