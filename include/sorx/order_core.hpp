#pragma once

#include "sorx/adapters/order_entry.hpp"
#include "sorx/route_rib.hpp"
#include "sorx/routing_policy.hpp"
#include "sorx/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace sorx {

enum class ParentState : std::uint8_t {
    Routing,
    Active,
    CancelPending,
    ReplacePending,
    Suspended,
    Filled,
    Canceled,
    Rejected,
};

enum class ChildState : std::uint8_t {
    Prepared,
    DispatchStarted,
    PendingAcknowledgement,
    Working,
    CancelPending,
    Filled,
    Canceled,
    Rejected,
    Ambiguous,
};

enum class CommandError : std::uint8_t {
    None,
    InvalidCommand,
    ConflictingParentId,
    ParentNotFound,
    ParentTerminal,
    ReplacementBelowFilled,
    KillSwitchActive,
    RoutingDisabled,
    InstrumentProhibited,
    ParentQuantityLimit,
    ParentNotionalLimit,
    PriceCollar,
    OpenParentLimit,
    JournalFailure,
    IngressSaturated,
    RecoveryInProgress,
};

struct InstrumentRiskLimit final {
    InstrumentId instrument{};
    bool allowed{true};
    Quantity maximum_parent_quantity{};
    Quantity maximum_child_quantity{};
    Notional maximum_parent_notional{};
    Notional maximum_child_notional{};
    Price minimum_price{};
    Price maximum_price{};
    std::uint64_t maximum_open_parents{};
    std::uint64_t maximum_open_children{};
    Quantity maximum_gross_outstanding{};
};

struct VenueRiskLimit final {
    VenueId venue{};
    bool allowed{true};
    Quantity maximum_outstanding{};
};

struct RiskConfiguration final {
    std::uint64_t generation{1U};
    std::vector<InstrumentRiskLimit> instruments;
    std::vector<VenueRiskLimit> venues;
};

struct CoreConfiguration final {
    RiskConfiguration risk;
    std::size_t maximum_aggressive_venues{1U};
};

struct ParentSubmitEvent final {
    ParentOrder order;
};

struct ParentCancelEvent final {
    ParentOrderId parent{};
};

struct ParentReplaceEvent final {
    ParentReplace replacement;
};

struct CandidateTablePublishedEvent final {
    std::shared_ptr<const CandidateTable> table;
};

struct RuntimeViewPublishedEvent final {
    InstrumentId instrument{};
    CandidateRuntimeView view;
};

struct OrderEntrySessionEvent final {
    VenueId venue{};
    std::uint64_t session_epoch{};
    OrderEntryState state{OrderEntryState::Disconnected};
};

struct DispatchStartedEvent final {
    ChildOrderId child{};
    adapters::DispatchAttemptKind kind{adapters::DispatchAttemptKind::New};
    std::uint64_t attempt_id{};
    VenueId venue{};
    std::uint64_t session_epoch{};
    std::string wire_order_id;
};

struct LocalDispatchEvent final {
    adapters::LocalDispatchResult result;
};

struct ExecutionReportEvent final {
    adapters::NormalizedExecutionReport report;
};

struct DispatchTimeoutEvent final {
    ChildOrderId child{};
    std::string reason;
};

struct KillSwitchEvent final {
    bool active{};
    std::string reason;
};

struct RoutingGateEvent final {
    bool enabled{};
};

enum class ReconciliationDisposition : std::uint8_t {
    DefinitelyUnsent,
    Working,
    TerminalCanceled,
    TerminalFilled,
    TerminalRejected,
    Unresolved,
};

struct ReconciliationEvent final {
    ChildOrderId child{};
    ReconciliationDisposition disposition{ReconciliationDisposition::Unresolved};
    Quantity cumulative_quantity{};
};

using CoreEventPayload = std::variant<
    ParentSubmitEvent,
    ParentCancelEvent,
    ParentReplaceEvent,
    CandidateTablePublishedEvent,
    RuntimeViewPublishedEvent,
    OrderEntrySessionEvent,
    DispatchStartedEvent,
    LocalDispatchEvent,
    ExecutionReportEvent,
    DispatchTimeoutEvent,
    KillSwitchEvent,
    RoutingGateEvent,
    ReconciliationEvent>;

struct CoreEvent final {
    std::uint64_t ingress_sequence{};
    CoreEventPayload payload;
};

enum class DeterministicRecordKind : std::uint16_t {
    ParentSubmit = 1U,
    ParentCancel = 2U,
    ParentReplace = 3U,
    RouteDecision = 4U,
    RiskReservationCreated = 5U,
    RiskReservationReleased = 6U,
    ChildPrepared = 7U,
    DispatchResult = 8U,
    ExecutionReportObserved = 9U,
    ExecutionOutcome = 10U,
    ParentTerminal = 11U,
    ChildTerminal = 12U,
    CandidateTablePublished = 13U,
    RuntimeViewPublished = 14U,
    OrderEntrySessionChanged = 15U,
    KillSwitchChanged = 16U,
    RoutingGateChanged = 17U,
    ReconciliationOutcome = 18U,
    DispatchStarted = 19U,
    ExecutionCanonicalized = 20U,
};

struct DeterministicRecord final {
    DeterministicRecordKind kind{DeterministicRecordKind::ParentSubmit};
    std::uint64_t ingress_sequence{};
    std::vector<std::byte> payload;
};

struct DispatchNewEffect final {
    adapters::NewOrderIntent intent;
};

struct DispatchCancelEffect final {
    adapters::CancelOrderIntent intent;
};

using CoreEffect = std::variant<DispatchNewEffect, DispatchCancelEffect>;

enum class ParentReportKind : std::uint8_t {
    Accepted,
    RoutePlanned,
    PartiallyFilled,
    Filled,
    CancelPending,
    Canceled,
    ReplacePending,
    Replaced,
    Rejected,
    Suspended,
};

struct ParentReport final {
    ParentOrderId parent{};
    ParentReportKind kind{ParentReportKind::Accepted};
    ParentState state{ParentState::Routing};
    Quantity cumulative_filled{};
    Quantity explicit_remainder{};
    std::string reason;
};

struct ApplyResult final {
    bool command_accepted{true};
    bool idempotent_replay{};
    CommandError command_error{CommandError::None};
    std::vector<DeterministicRecord> records;
    std::vector<CoreEffect> effects;
    std::vector<ParentReport> reports;
};

struct DispatchAttemptSnapshot final {
    adapters::DispatchAttemptKind kind{adapters::DispatchAttemptKind::New};
    std::uint64_t attempt_id{};
    VenueId venue{};
    std::uint64_t session_epoch{};
    std::string wire_order_id;

    auto operator<=>(const DispatchAttemptSnapshot&) const = default;
};

struct ChildSnapshot final {
    ChildOrderId id{};
    ParentOrderId parent{};
    std::uint64_t parent_revision{};
    VenueId venue{};
    std::uint64_t session_epoch{};
    std::string wire_order_id;
    Quantity ordered{};
    Quantity filled{};
    Price limit_price{};
    ChildState state{ChildState::Prepared};
    Quantity risk_reservation{};
    std::optional<DispatchAttemptSnapshot> active_dispatch_attempt;

    Quantity venue_ordered{};
    Price venue_limit_price{};
    // Durable canonical-to-venue conversion identities.
    RationalConversion price_conversion{};
    RationalConversion quantity_conversion{};

    adapters::WireProfileIdentity wire_profile;
    std::string cancel_wire_order_id;
};

struct ParentSnapshot final {
    ParentOrder order;
    std::uint64_t revision{};
    Quantity requested_total{};
    Quantity cumulative_filled{};
    Quantity explicit_remainder{};
    ParentState state{ParentState::Routing};
    std::vector<ChildOrderId> children;
    std::optional<ParentReplace> pending_replace;
};

struct SorSnapshot final {
    std::uint64_t last_ingress_sequence{};
    std::uint64_t last_dispatch_attempt_id{};
    std::uint64_t candidate_table_generation{};
    std::uint64_t risk_generation{};
    bool kill_switch{};
    bool routing_enabled{};
    std::vector<ParentSnapshot> parents;
    std::vector<ChildSnapshot> children;
};

struct RouteTrace final {
    bool hard_risk_accepted{};
    CommandError hard_rejection{CommandError::None};
    std::uint64_t risk_generation{};
    RouteDecision decision;
};

class SorCore final {
public:
    // Direct-core test compatibility only. Production construction must
    // inject the session/profile-resolved immutable factory below.
    SorCore(
        CoreConfiguration configuration,
        std::shared_ptr<const CandidateTable> candidates);
    SorCore(
        CoreConfiguration configuration,
        std::shared_ptr<const CandidateTable> candidates,
        std::shared_ptr<const adapters::WireIdentityFactory>
            wire_identity_factory);
    ~SorCore();

    SorCore(const SorCore&) = delete;
    SorCore& operator=(const SorCore&) = delete;
    SorCore(SorCore&&) noexcept;
    SorCore& operator=(SorCore&&) noexcept;

    [[nodiscard]] ApplyResult apply(const CoreEvent& event);
    [[nodiscard]] RouteTrace trace(const ParentOrder& order) const;
    [[nodiscard]] SorSnapshot snapshot() const;

    // Canonical deterministic state image used as the final record in every
    // committed runtime batch. It contains all coordinator ownership state,
    // dedupe keys, pinned runtime views, sessions, reservations, and ID counters.
    [[nodiscard]] std::vector<std::byte> save_state() const;
    void restore_state(std::span<const std::byte> state);
    void begin_recovery();

    // Recovery may enable new routing only when every uncertain dispatch has
    // been reconciled and every administratively active route has fresh LIVE
    // market data with consistent accounting.
    [[nodiscard]] bool recovery_consistent() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx

