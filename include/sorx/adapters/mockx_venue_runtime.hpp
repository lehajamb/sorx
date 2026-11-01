#pragma once

#include "sorx/adapters/mockx_md_session.hpp"
#include "sorx/adapters/quickfix_session.hpp"
#include "sorx/runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sorx::adapters {

enum class MockXVenueRuntimeState : std::uint8_t {
    Stopped,
    Starting,
    RecoveryOnly,
    Operational,
    Degraded,
    Stopping,
    Failed,
};

enum class MockXVenueRuntimeError : std::uint8_t {
    None,
    InvalidLifecycle,
    MarketDataCallbackQueueFull,
    OrderEntryCallbackQueueFull,
    MarketDataSessionFailure,
    OrderEntrySessionFailure,
    RuntimePublicationRejected,
    LocalDispatchPublicationLost,
    WorkerFailure,
};

struct MockXVenueRuntimeCounters final {
    std::uint64_t market_data_callbacks{};
    std::uint64_t order_entry_callbacks{};
    std::uint64_t market_data_callback_losses{};
    std::uint64_t order_entry_callback_losses{};
    std::uint64_t runtime_events_published{};
    std::uint64_t runtime_publication_failures{};
    std::uint64_t new_dispatches{};
    std::uint64_t cancel_dispatches{};
    std::uint64_t recovery_attempts{};
    std::uint64_t recovery_completions{};
};

// One immutable, versioned control-plane image for a configured venue. Every
// candidate carries the same adjacency generation, which advances whenever
// market-data health, a book, order-entry health, or egress health changes.
struct MockXVenueAdjacencyView final {
    VenueId venue{};
    std::uint64_t generation{};
    MarketDataState market_data_state{MarketDataState::Snapshotting};
    OrderEntryState order_entry_state{OrderEntryState::Disconnected};
    EgressState egress_state{EgressState::Ready};
    bool feed_ready{};
    bool order_entry_ready{};
    bool failed_closed{};
    std::vector<CandidateRuntimeView> candidates;
};

struct MockXVenueRuntimeConfig final {
    MockXMdSessionConfig market_data;
    quickfix::QuickFixSessionConfig order_entry;
    std::size_t market_data_callback_capacity{1'024U};
    std::size_t order_entry_callback_capacity{1'024U};
    std::size_t local_dispatch_callback_capacity{8U};
    std::size_t worker_drain_batch{256U};
    std::chrono::milliseconds worker_idle_wait{1};
    bool automatically_complete_recovery{true};
};

// Offline construction still owns the same concrete QuickFixSessionAdapter,
// but supplies its protocol-neutral transport seam. Native market-data may be
// disabled so deterministic tests can inject already-decoded publications.
struct MockXVenueTestSeams final {
    std::optional<quickfix::QuickFixTestTransport> order_entry_transport;
    bool disable_native_market_data{};
    std::function<bool()> permit_egress;
};

class MockXVenueRuntime final {
public:
    // runtime must already have completed journal/checkpoint replay. The venue
    // runtime deliberately accepts a reference instead of constructing it and
    // must be stopped/destroyed before that referenced runtime. Production start
    // constructs QuickFIX's persistent FileStoreFactory before the initiator can
    // log on, while SorRuntime remains recovery-only.
    MockXVenueRuntime(
        SorRuntime& runtime,
        MockXVenueRuntimeConfig config);
    MockXVenueRuntime(
        SorRuntime& runtime,
        MockXVenueRuntimeConfig config,
        MockXVenueTestSeams test_seams);
    ~MockXVenueRuntime();

    MockXVenueRuntime(const MockXVenueRuntime&) = delete;
    MockXVenueRuntime& operator=(const MockXVenueRuntime&) = delete;
    MockXVenueRuntime(MockXVenueRuntime&&) = delete;
    MockXVenueRuntime& operator=(MockXVenueRuntime&&) = delete;

    // An instance is one-shot. false means startup failed closed.
    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] MockXVenueRuntimeState state() const noexcept;
    [[nodiscard]] MockXVenueRuntimeError last_error() const noexcept;
    [[nodiscard]] std::string last_error_detail() const;
    [[nodiscard]] MockXVenueRuntimeCounters counters() const noexcept;
    [[nodiscard]] std::shared_ptr<const MockXVenueAdjacencyView>
    adjacency_view() const noexcept;
    [[nodiscard]] std::uint16_t local_udp_port() const noexcept;

    // Deterministic/offline market-data seam. It is disabled for production
    // construction so a second producer cannot violate callback ordering.
    [[nodiscard]] bool try_publish_market_data(
        MockXMdPublication publication) noexcept;

    // Offline FIX callbacks enter the concrete session adapter and therefore
    // exercise exactly the same normalization and bounded sink path as native
    // QuickFIX callbacks.
    [[nodiscard]] bool ingest_fix_session(
        quickfix::TransportSessionSignal signal,
        const quickfix::QuickFixSessionIdentity& identity);
    [[nodiscard]] bool ingest_fix_application(
        const quickfix::QuickFixSessionIdentity& identity,
        std::string_view message_type,
        std::uint64_t inbound_session_sequence,
        std::span<const mockx::FixField> fields);

    // With no venue order-status query, only Prepared is provably unsent.
    // Every other nonterminal state is unresolved and retains its reservation.
    [[nodiscard]] static ReconciliationEvent conservative_reconciliation(
        const ChildSnapshot& child) noexcept;
    [[nodiscard]] bool reconcile_recovered_children();

    // Never asks SorRuntime to open routing until every configured instrument
    // has a LIVE book and this exact QuickFIX session is Ready.
    [[nodiscard]] bool attempt_complete_recovery();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::adapters


