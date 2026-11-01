#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/adapters/mockx_md.hpp"
#include "sorx/adapters/mockx_md_session.hpp"
#include "sorx/adapters/order_entry.hpp"
#include "sorx/adapters/quickfix_session.hpp"
#include "sorx/book_view.hpp"
#include "sorx/config.hpp"
#include "sorx/order_core.hpp"
#include "sorx/route_rib.hpp"
#include "sorx/routing_policy.hpp"
#include "sorx/runtime.hpp"
#include "sorx/runtime_queues.hpp"
#include "sorx/transcript.hpp"
#include "sorx/types.hpp"
#if defined(SORX_PUBLIC_VALIDATION_HAS_MD_CORE)
#include "sorx/md/feed_engine.hpp"
#endif

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <vector>

#if defined(NDEBUG)
#error "SORX CTest executables must keep assert enabled in optimized builds"
#endif

namespace {

template <typename Enum, typename... Rest>
consteval bool contiguous_from(
    const std::uint64_t first,
    const Enum head,
    const Rest... rest) {
    static_assert(std::is_enum_v<Enum>);
    static_assert((std::is_same_v<Enum, Rest> && ...));
    const Enum values[]{head, rest...};
    for (std::size_t index = 0U; index < std::size(values); ++index) {
        using Underlying = std::underlying_type_t<Enum>;
        const auto value = static_cast<std::uint64_t>(
            static_cast<Underlying>(values[index]));
        if (value != first + index) {
            return false;
        }
    }
    return true;
}

static_assert(contiguous_from(0U, sorx::Side::Buy, sorx::Side::Sell));
static_assert(contiguous_from(
    0U,
    sorx::RoutingIntent::AggressiveIOC,
    sorx::RoutingIntent::PassiveGTC));
static_assert(contiguous_from(0U, sorx::AdminState::Up, sorx::AdminState::Down));
static_assert(contiguous_from(
    0U,
    sorx::MarketDataState::Snapshotting,
    sorx::MarketDataState::Stitching,
    sorx::MarketDataState::Live,
    sorx::MarketDataState::Recovering,
    sorx::MarketDataState::Failed));
static_assert(contiguous_from(
    0U,
    sorx::OrderEntryState::Disconnected,
    sorx::OrderEntryState::Connecting,
    sorx::OrderEntryState::Ready,
    sorx::OrderEntryState::Degraded));
static_assert(contiguous_from(
    0U, sorx::EgressState::Ready, sorx::EgressState::Congested));
static_assert(contiguous_from(
    0U,
    sorx::InstrumentState::Unknown,
    sorx::InstrumentState::Trading,
    sorx::InstrumentState::Halted,
    sorx::InstrumentState::Closed));

static_assert(contiguous_from(
    0U,
    sorx::adapters::TimeInForce::ImmediateOrCancel,
    sorx::adapters::TimeInForce::GoodTillCancel));
static_assert(contiguous_from(
    0U,
    sorx::adapters::DispatchAttemptKind::New,
    sorx::adapters::DispatchAttemptKind::Cancel));
static_assert(contiguous_from(
    0U,
    sorx::adapters::ExecutionValueDomain::Canonical,
    sorx::adapters::ExecutionValueDomain::Venue));
static_assert(contiguous_from(
    0U,
    sorx::adapters::ExecutionKind::Acknowledged,
    sorx::adapters::ExecutionKind::Trade,
    sorx::adapters::ExecutionKind::Canceled,
    sorx::adapters::ExecutionKind::Rejected,
    sorx::adapters::ExecutionKind::CancelRejected));
static_assert(contiguous_from(
    0U,
    sorx::adapters::NormalizedOrderStatus::Pending,
    sorx::adapters::NormalizedOrderStatus::Working,
    sorx::adapters::NormalizedOrderStatus::PartiallyFilled,
    sorx::adapters::NormalizedOrderStatus::Filled,
    sorx::adapters::NormalizedOrderStatus::Canceled,
    sorx::adapters::NormalizedOrderStatus::Rejected));
static_assert(contiguous_from(
    0U,
    sorx::adapters::LocalDispatchState::DefinitelyUnsent,
    sorx::adapters::LocalDispatchState::AcceptedLocally,
    sorx::adapters::LocalDispatchState::Unknown));
static_assert(contiguous_from(
    0U,
    sorx::adapters::MockXEnqueueResult::Accepted,
    sorx::adapters::MockXEnqueueResult::QueueFull,
    sorx::adapters::MockXEnqueueResult::InputTooLarge,
    sorx::adapters::MockXEnqueueResult::InvalidTimestamp));
static_assert(contiguous_from(
    0U,
    sorx::adapters::MockXMdSessionState::Stopped,
    sorx::adapters::MockXMdSessionState::Starting,
    sorx::adapters::MockXMdSessionState::WaitingForLiveIdentity,
    sorx::adapters::MockXMdSessionState::Snapshotting,
    sorx::adapters::MockXMdSessionState::Stitching,
    sorx::adapters::MockXMdSessionState::Live,
    sorx::adapters::MockXMdSessionState::Recovering,
    sorx::adapters::MockXMdSessionState::Stopping,
    sorx::adapters::MockXMdSessionState::Failed));
static_assert(contiguous_from(
    0U,
    sorx::adapters::MockXMdSessionError::None,
    sorx::adapters::MockXMdSessionError::UdpTransportFailure,
    sorx::adapters::MockXMdSessionError::GlimpseTransportFailure,
    sorx::adapters::MockXMdSessionError::GlimpseAuthenticationRejected,
    sorx::adapters::MockXMdSessionError::GlimpseProtocolFailure,
    sorx::adapters::MockXMdSessionError::RawQueueContinuityLoss,
    sorx::adapters::MockXMdSessionError::AdapterQueueContinuityLoss,
    sorx::adapters::MockXMdSessionError::PublicationSinkFull,
    sorx::adapters::MockXMdSessionError::ClockFailure));

static_assert(contiguous_from(
    0U,
    sorx::ParentState::Routing,
    sorx::ParentState::Active,
    sorx::ParentState::CancelPending,
    sorx::ParentState::ReplacePending,
    sorx::ParentState::Suspended,
    sorx::ParentState::Filled,
    sorx::ParentState::Canceled,
    sorx::ParentState::Rejected));
static_assert(contiguous_from(
    0U,
    sorx::ChildState::Prepared,
    sorx::ChildState::DispatchStarted,
    sorx::ChildState::PendingAcknowledgement,
    sorx::ChildState::Working,
    sorx::ChildState::CancelPending,
    sorx::ChildState::Filled,
    sorx::ChildState::Canceled,
    sorx::ChildState::Rejected,
    sorx::ChildState::Ambiguous));
static_assert(contiguous_from(
    0U,
    sorx::CommandError::None,
    sorx::CommandError::InvalidCommand,
    sorx::CommandError::ConflictingParentId,
    sorx::CommandError::ParentNotFound,
    sorx::CommandError::ParentTerminal,
    sorx::CommandError::ReplacementBelowFilled,
    sorx::CommandError::KillSwitchActive,
    sorx::CommandError::RoutingDisabled,
    sorx::CommandError::InstrumentProhibited,
    sorx::CommandError::ParentQuantityLimit,
    sorx::CommandError::ParentNotionalLimit,
    sorx::CommandError::PriceCollar,
    sorx::CommandError::OpenParentLimit,
    sorx::CommandError::JournalFailure,
    sorx::CommandError::IngressSaturated,
    sorx::CommandError::RecoveryInProgress));
static_assert(contiguous_from(
    0U,
    sorx::ReconciliationDisposition::DefinitelyUnsent,
    sorx::ReconciliationDisposition::Working,
    sorx::ReconciliationDisposition::TerminalCanceled,
    sorx::ReconciliationDisposition::TerminalFilled,
    sorx::ReconciliationDisposition::TerminalRejected,
    sorx::ReconciliationDisposition::Unresolved));
static_assert(contiguous_from(
    1U,
    sorx::DeterministicRecordKind::ParentSubmit,
    sorx::DeterministicRecordKind::ParentCancel,
    sorx::DeterministicRecordKind::ParentReplace,
    sorx::DeterministicRecordKind::RouteDecision,
    sorx::DeterministicRecordKind::RiskReservationCreated,
    sorx::DeterministicRecordKind::RiskReservationReleased,
    sorx::DeterministicRecordKind::ChildPrepared,
    sorx::DeterministicRecordKind::DispatchResult,
    sorx::DeterministicRecordKind::ExecutionReportObserved,
    sorx::DeterministicRecordKind::ExecutionOutcome,
    sorx::DeterministicRecordKind::ParentTerminal,
    sorx::DeterministicRecordKind::ChildTerminal,
    sorx::DeterministicRecordKind::CandidateTablePublished,
    sorx::DeterministicRecordKind::RuntimeViewPublished,
    sorx::DeterministicRecordKind::OrderEntrySessionChanged,
    sorx::DeterministicRecordKind::KillSwitchChanged,
    sorx::DeterministicRecordKind::RoutingGateChanged,
    sorx::DeterministicRecordKind::ReconciliationOutcome,
    sorx::DeterministicRecordKind::DispatchStarted,
    sorx::DeterministicRecordKind::ExecutionCanonicalized));
static_assert(contiguous_from(
    1U,
    sorx::CoreTranscriptEventKind::ParentSubmit,
    sorx::CoreTranscriptEventKind::ParentCancel,
    sorx::CoreTranscriptEventKind::ParentReplace,
    sorx::CoreTranscriptEventKind::CandidateTablePublished,
    sorx::CoreTranscriptEventKind::RuntimeViewPublished,
    sorx::CoreTranscriptEventKind::OrderEntrySession,
    sorx::CoreTranscriptEventKind::DispatchStarted,
    sorx::CoreTranscriptEventKind::LocalDispatch,
    sorx::CoreTranscriptEventKind::ExecutionReport,
    sorx::CoreTranscriptEventKind::DispatchTimeout,
    sorx::CoreTranscriptEventKind::KillSwitch,
    sorx::CoreTranscriptEventKind::RoutingGate,
    sorx::CoreTranscriptEventKind::Reconciliation));
static_assert(contiguous_from(
    0U,
    sorx::ParentReportKind::Accepted,
    sorx::ParentReportKind::RoutePlanned,
    sorx::ParentReportKind::PartiallyFilled,
    sorx::ParentReportKind::Filled,
    sorx::ParentReportKind::CancelPending,
    sorx::ParentReportKind::Canceled,
    sorx::ParentReportKind::ReplacePending,
    sorx::ParentReportKind::Replaced,
    sorx::ParentReportKind::Rejected,
    sorx::ParentReportKind::Suspended));

static_assert(contiguous_from(
    0U,
    sorx::CandidateExclusionReason::AdminDown,
    sorx::CandidateExclusionReason::UnsupportedIntent,
    sorx::CandidateExclusionReason::MissingRuntimeState,
    sorx::CandidateExclusionReason::MissingBook,
    sorx::CandidateExclusionReason::InvalidBook,
    sorx::CandidateExclusionReason::BookIdentityMismatch,
    sorx::CandidateExclusionReason::MarketDataNotLive,
    sorx::CandidateExclusionReason::StaleBook,
    sorx::CandidateExclusionReason::InstrumentNotTrading,
    sorx::CandidateExclusionReason::OrderEntryNotReady,
    sorx::CandidateExclusionReason::EgressCongested,
    sorx::CandidateExclusionReason::ConversionIncompatible,
    sorx::CandidateExclusionReason::InexactPriceConversion,
    sorx::CandidateExclusionReason::InexactQuantityConversion,
    sorx::CandidateExclusionReason::RiskProhibited,
    sorx::CandidateExclusionReason::AllocationCapTooSmall,
    sorx::CandidateExclusionReason::NoExecutableDepth,
    sorx::CandidateExclusionReason::MaximumVenueCount));
static_assert(contiguous_from(
    0U,
    sorx::RouteRejectReason::None,
    sorx::RouteRejectReason::InvalidParent,
    sorx::RouteRejectReason::IntentMismatch,
    sorx::RouteRejectReason::InstrumentNotConfigured,
    sorx::RouteRejectReason::NoEligibleCandidate,
    sorx::RouteRejectReason::NoExecutableLiquidity,
    sorx::RouteRejectReason::PassiveMarketable,
    sorx::RouteRejectReason::PassiveFullQuantityUnavailable));
static_assert(contiguous_from(
    0U,
    sorx::RemainderReason::None,
    sorx::RemainderReason::NoEligibleCandidate,
    sorx::RemainderReason::ParentLimitReached,
    sorx::RemainderReason::InsufficientDisplayedDepth,
    sorx::RemainderReason::TruncatedDepthUnknown,
    sorx::RemainderReason::VenueAllocationCaps,
    sorx::RemainderReason::MaximumVenueCount,
    sorx::RemainderReason::InexactConversion,
    sorx::RemainderReason::PassiveFullQuantityUnavailable));
static_assert(contiguous_from(
    0U,
    sorx::RouteValidationCode::EmptyRib,
    sorx::RouteValidationCode::InvalidVenueId,
    sorx::RouteValidationCode::InvalidInstrumentId,
    sorx::RouteValidationCode::InvalidAdminState,
    sorx::RouteValidationCode::EmptyVenueName,
    sorx::RouteValidationCode::EmptyCanonicalSymbol,
    sorx::RouteValidationCode::EmptyWireSymbol,
    sorx::RouteValidationCode::NoRoutingCapability,
    sorx::RouteValidationCode::ZeroAllocationCap,
    sorx::RouteValidationCode::DuplicateRoute,
    sorx::RouteValidationCode::VenueNameConflict,
    sorx::RouteValidationCode::VenueAdminConflict,
    sorx::RouteValidationCode::InstrumentSymbolConflict,
    sorx::RouteValidationCode::CanonicalSymbolConflict,
    sorx::RouteValidationCode::DuplicateWireSymbol));
static_assert(contiguous_from(
    0U,
    sorx::config::ConfigurationErrorCode::Io,
    sorx::config::ConfigurationErrorCode::Syntax,
    sorx::config::ConfigurationErrorCode::UnknownSection,
    sorx::config::ConfigurationErrorCode::DuplicateSection,
    sorx::config::ConfigurationErrorCode::DuplicateField,
    sorx::config::ConfigurationErrorCode::UnknownField,
    sorx::config::ConfigurationErrorCode::MissingField,
    sorx::config::ConfigurationErrorCode::InvalidValue,
    sorx::config::ConfigurationErrorCode::DuplicateIdentity,
    sorx::config::ConfigurationErrorCode::UnmappedReference,
    sorx::config::ConfigurationErrorCode::InexactConversion,
    sorx::config::ConfigurationErrorCode::Overflow,
    sorx::config::ConfigurationErrorCode::RouteValidation));
static_assert(contiguous_from(
    0U,
    sorx::CommandStatus::Accepted,
    sorx::CommandStatus::Rejected));
static_assert(contiguous_from(
    0U,
    sorx::runtime::EgressClass::cancel_or_risk_off,
    sorx::runtime::EgressClass::new_or_replacement));
static_assert(contiguous_from(
    0U,
    sorx::adapters::quickfix::TransportSessionSignal::Created,
    sorx::adapters::quickfix::TransportSessionSignal::LoggedOn,
    sorx::adapters::quickfix::TransportSessionSignal::LoggedOut));

#if defined(SORX_PUBLIC_VALIDATION_HAS_MD_CORE)
static_assert(contiguous_from(0U, sorx::md::RecoveryStrategy::FreshSnapshot));
static_assert(contiguous_from(
    0U,
    sorx::md::RecoveryAction::None,
    sorx::md::RecoveryAction::RequestFreshSnapshot,
    sorx::md::RecoveryAction::Stop));
static_assert(contiguous_from(
    0U,
    sorx::md::FeedError::None,
    sorx::md::FeedError::InvalidState,
    sorx::md::FeedError::ClockRegression,
    sorx::md::FeedError::MalformedFrame,
    sorx::md::FeedError::SequenceGap,
    sorx::md::FeedError::NewSession,
    sorx::md::FeedError::RingContinuityLoss,
    sorx::md::FeedError::TruncatedDatagram,
    sorx::md::FeedError::InvalidSnapshot,
    sorx::md::FeedError::InvalidEvent,
    sorx::md::FeedError::BufferCapacityExceeded,
    sorx::md::FeedError::UnsupportedMessage,
    sorx::md::FeedError::TransportFailure,
    sorx::md::FeedError::EndOfSession));
#endif

sorx::RouteConfig valid_route() {
    sorx::RouteConfig route;
    route.venue = sorx::VenueId{1U};
    route.instrument = sorx::InstrumentId{7U};
    route.venue_name = "VENUE";
    route.canonical_symbol = "ALPHA";
    route.wire_symbol = "ALPHA-X";
    route.capabilities.aggressive_ioc = true;
    route.capabilities.passive_gtc = true;
    route.allocation_cap = sorx::Quantity{100U};
    route.aggressive_priority = 1U;
    route.passive_priority = 1U;
    route.configured_latency_ns = 1U;
    route.admin_state = sorx::AdminState::Up;
    return route;
}

void exact_value_helpers_fail_closed() {
    constexpr auto exact = sorx::RationalConversion::create(4U, 2U);
    static_assert(exact.has_value());
    static_assert(exact->numerator() == 2U);
    static_assert(exact->denominator() == 1U);
    static_assert(!sorx::RationalConversion::create(0U, 1U).has_value());
    static_assert(!sorx::RationalConversion::create(1U, 0U).has_value());
    static_assert(!sorx::checked_subtract(
        sorx::Quantity{1U}, sorx::Quantity{2U}).has_value());
    static_assert(!sorx::checked_notional(
        sorx::Price{-1}, sorx::Quantity{1U}).has_value());
}

void route_and_policy_reject_invalid_enums() {
    const auto invalid_admin = static_cast<sorx::AdminState>(0xffU);
    auto route = valid_route();
    route.admin_state = invalid_admin;
    sorx::RouteRib invalid_rib;
    std::vector<sorx::RouteValidationError> errors;
    assert(!invalid_rib.replace_all({route}, errors));
    assert(errors.size() == 1U);
    assert(errors.front().code == sorx::RouteValidationCode::InvalidAdminState);

    sorx::RouteRib rib;
    errors.clear();
    assert(rib.replace_all({valid_route()}, errors));
    assert(errors.empty());
    assert(!rib.set_admin(sorx::VenueId{1U}, invalid_admin));
    assert(rib.routes().front().admin_state == sorx::AdminState::Up);

    sorx::FibCompiler compiler;
    const auto candidates = compiler.compile(rib);
    sorx::ParentOrder parent;
    parent.id = sorx::ParentOrderId{1U};
    parent.instrument = sorx::InstrumentId{7U};
    parent.side = static_cast<sorx::Side>(0xffU);
    parent.quantity = sorx::Quantity{1U};
    parent.limit_price = sorx::Price{100};
    parent.intent = sorx::RoutingIntent::AggressiveIOC;
    const auto decision = sorx::RoutingPolicy::aggressive_ioc(
        parent, *candidates, {}, 1U);
    assert(decision.reject_reason == sorx::RouteRejectReason::InvalidParent);
    assert(!decision.accepted());
}

void book_view_rejects_invalid_enums() {
    sorx::md::BookView view;
    view.venue = sorx::VenueId{1U};
    view.channel = sorx::ChannelId{1U};
    view.profile = sorx::ProfileId{1U};
    view.instrument = sorx::InstrumentId{7U};
    view.feed_session_epoch = 1U;
    view.recovery_generation = 1U;
    view.view_generation = 1U;
    view.last_applied_sequence = 1U;
    view.next_sequence = 2U;
    view.feed_state = sorx::MarketDataState::Live;
    view.trading_state = sorx::InstrumentState::Trading;
    assert(view.valid());

    auto invalid_feed = view;
    invalid_feed.feed_state = static_cast<sorx::MarketDataState>(0xffU);
    assert(!invalid_feed.valid());

    auto invalid_instrument = view;
    invalid_instrument.trading_state =
        static_cast<sorx::InstrumentState>(0xffU);
    assert(!invalid_instrument.valid());
}

void mockx_profile_rejects_invalid_enums() {
    const sorx::adapters::mockx::MockXFix44Profile profile(
        sorx::adapters::mockx::MockXFix44Config{2U, 0U, 32U, "SX"});

    sorx::adapters::NewOrderIntent order;
    order.venue = sorx::VenueId{1U};
    order.parent = sorx::ParentOrderId{1U};
    order.child = sorx::ChildOrderId{1U};
    order.instrument = sorx::InstrumentId{7U};
    order.side = sorx::Side::Buy;
    order.quantity = sorx::Quantity{2U};
    order.limit_price = sorx::Price{100};
    order.time_in_force = sorx::adapters::TimeInForce::ImmediateOrCancel;
    order.session_epoch = 1U;
    order.wire_order_id = "SXN1-1-1";
    order.wire_symbol = "ALPHA-X";
    assert(profile.make_new_order(order).ok);

    auto invalid_side = order;
    invalid_side.side = static_cast<sorx::Side>(0xffU);
    assert(!profile.make_new_order(invalid_side).ok);

    auto invalid_tif = order;
    invalid_tif.time_in_force =
        static_cast<sorx::adapters::TimeInForce>(0xffU);
    assert(!profile.make_new_order(invalid_tif).ok);

    sorx::adapters::CancelOrderIntent cancel;
    cancel.venue = order.venue;
    cancel.parent = order.parent;
    cancel.child = order.child;
    cancel.side = sorx::Side::Buy;
    cancel.original_quantity = order.quantity;
    cancel.session_epoch = order.session_epoch;
    cancel.original_wire_order_id = order.wire_order_id;
    cancel.cancel_wire_order_id = "PVC1-1-1";
    cancel.wire_symbol = order.wire_symbol;
    assert(profile.make_cancel(cancel).ok);
    cancel.side = static_cast<sorx::Side>(0xffU);
    assert(!profile.make_cancel(cancel).ok);
}

#if defined(SORX_PUBLIC_VALIDATION_HAS_MD_CORE)
void feed_engine_rejects_invalid_strategy() {
    sorx::md::FeedEngineConfig config;
    config.venue = sorx::VenueId{1U};
    config.channel = sorx::ChannelId{1U};
    config.profile = sorx::ProfileId{1U};
    config.recovery_strategy =
        static_cast<sorx::md::RecoveryStrategy>(0xffU);
    bool threw = false;
    try {
        const sorx::md::FeedEngine engine(config);
        static_cast<void>(engine);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}
#endif

}  // namespace

int main() {
    exact_value_helpers_fail_closed();
    route_and_policy_reject_invalid_enums();
    book_view_rejects_invalid_enums();
    mockx_profile_rejects_invalid_enums();
#if defined(SORX_PUBLIC_VALIDATION_HAS_MD_CORE)
    feed_engine_rejects_invalid_strategy();
#endif
    return 0;
}




