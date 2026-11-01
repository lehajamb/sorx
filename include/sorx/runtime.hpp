#pragma once

#include "sorx/config.hpp"
#include "sorx/order_core.hpp"
#include "sorx/persistence/journal.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sorx {

enum class CommandStatus : std::uint8_t {
    Accepted,
    Rejected,
};

struct CommandReceipt final {
    CommandStatus status{CommandStatus::Rejected};
    std::uint64_t ingress_sequence{};
    std::uint64_t journal_record_id{};
    CommandError error{CommandError::None};
    bool idempotent_replay{};

    [[nodiscard]] bool accepted() const noexcept {
        return status == CommandStatus::Accepted;
    }
};

class IParentReportSink {
public:
    virtual ~IParentReportSink() = default;
    virtual void on_parent_report(const ParentReport& report) = 0;
};

struct RuntimeConfiguration final {
    config::ValidatedConfiguration validated;
    std::filesystem::path journal_path;
    std::filesystem::path checkpoint_path;
    std::size_t checkpoint_interval_batches{64U};
};

using AdapterEventPayload = std::variant<
    RuntimeViewPublishedEvent,
    OrderEntrySessionEvent,
    LocalDispatchEvent,
    ExecutionReportEvent,
    DispatchTimeoutEvent,
    ReconciliationEvent>;

class SorRuntime final {
public:
    SorRuntime(RuntimeConfiguration configuration) = delete;
    SorRuntime(
        RuntimeConfiguration configuration,
        std::shared_ptr<const adapters::WireIdentityFactory>
            wire_identity_factory);
    ~SorRuntime();

    SorRuntime(const SorRuntime&) = delete;
    SorRuntime& operator=(const SorRuntime&) = delete;
    SorRuntime(SorRuntime&&) noexcept;
    SorRuntime& operator=(SorRuntime&&) noexcept;

    [[nodiscard]] CommandReceipt submit(const ParentOrder& order);
    [[nodiscard]] CommandReceipt cancel(ParentOrderId parent);
    [[nodiscard]] CommandReceipt replace(const ParentReplace& replacement);

    [[nodiscard]] RouteTrace trace(const ParentOrder& order) const;
    [[nodiscard]] SorSnapshot inspect() const;

    // Reports are delivered by a dedicated worker. A referenced sink must
    // outlive this runtime or be detached with clear_report_sink() before its
    // destruction. Both methods are barriers with respect to prior reports.
    void set_report_sink(IParentReportSink& sink);
    void clear_report_sink();

    [[nodiscard]] CommandReceipt set_venue_admin(
        VenueId venue,
        AdminState state);
    [[nodiscard]] CommandReceipt set_kill_switch(
        bool active,
        std::string reason);

    // Adapters publish immutable protocol-neutral events. The event and every
    // resulting core transition are durable before effects or reports escape.
    [[nodiscard]] bool publish(AdapterEventPayload event);

    // A returned dispatch has a durable DispatchStarted record. Cancel/risk-off
    // traffic is always drained before new/replacement traffic for that venue.
    [[nodiscard]] std::optional<CoreEffect> poll_egress(VenueId venue);

    // Startup remains recovery-only until every caller-performed reconciliation
    // is complete and the accounting/market gates are known consistent.
    [[nodiscard]] bool complete_recovery();
    [[nodiscard]] bool recovery_pending() const;
    [[nodiscard]] bool failed() const;
    [[nodiscard]] std::uint64_t report_callback_failures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx

