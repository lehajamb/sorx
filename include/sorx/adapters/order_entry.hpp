#pragma once

#include "sorx/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace sorx::adapters {

enum class TimeInForce : std::uint8_t {
    ImmediateOrCancel,
    GoodTillCancel,
};

enum class DispatchAttemptKind : std::uint8_t {
    New,
    Cancel,
};

inline constexpr std::size_t kMaximumWireOrderIdLength = 128U;

struct WireProfileIdentity final {
    VenueId venue{};
    ProfileId profile{};
    std::string profile_name;
    std::string session_identity;
    std::string dictionary_identity;
    std::string sender_comp_id;
    std::string target_comp_id;

    friend bool operator==(
        const WireProfileIdentity&,
        const WireProfileIdentity&) = default;
};

struct WireIdentityRequest final {
    WireProfileIdentity profile;
    DispatchAttemptKind kind{DispatchAttemptKind::New};
    ChildOrderId child{};
    ParentOrderId parent{};
    std::uint64_t parent_revision{1U};
    std::uint64_t session_epoch{1U};
};

struct WireIdentityResult final {
    bool ok{};
    WireProfileIdentity profile;
    std::string wire_order_id;
};

class WireIdentityFactory {
public:
    virtual ~WireIdentityFactory() = default;

    // The returned immutable profile set must remain valid for this factory's
    // lifetime. Production runtime construction pins it to validated config.
    [[nodiscard]] virtual std::span<const WireProfileIdentity> profiles()
        const noexcept = 0;
    [[nodiscard]] virtual WireIdentityResult make(
        const WireIdentityRequest& request) const = 0;
};

struct NewOrderIntent final {
    VenueId venue{};
    ParentOrderId parent{};
    ChildOrderId child{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price limit_price{};
    TimeInForce time_in_force{TimeInForce::ImmediateOrCancel};
    std::uint64_t parent_revision{1U};
    std::uint64_t session_epoch{1U};
    std::string wire_order_id;
    std::string wire_symbol;

    // Zero while queued. The runtime assigns this only after the matching
    // DispatchStartedEvent has been committed durably by the core.
    std::uint64_t dispatch_attempt_id{};

    // Explicit unit contract. The legacy quantity/limit_price fields above are
    // canonical mirrors and must agree whenever both are populated.
    Quantity canonical_quantity{};
    Price canonical_limit_price{};
    Quantity venue_quantity{};
    Price venue_limit_price{};
};

struct CancelOrderIntent final {
    VenueId venue{};
    ParentOrderId parent{};
    ChildOrderId child{};
    Side side{Side::Buy};
    Quantity original_quantity{};
    std::uint64_t session_epoch{1U};
    std::string original_wire_order_id;
    std::string cancel_wire_order_id;
    std::string wire_symbol;

    // Zero while queued. See NewOrderIntent::dispatch_attempt_id.
    std::uint64_t dispatch_attempt_id{};

    // Explicit unit contract. original_quantity remains the canonical mirror.
    Quantity canonical_original_quantity{};
    Quantity venue_original_quantity{};
};

enum class ExecutionKind : std::uint8_t {
    Acknowledged,
    Trade,
    Canceled,
    Rejected,
    CancelRejected,
};

enum class NormalizedOrderStatus : std::uint8_t {
    Pending,
    Working,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected,
};

enum class ExecutionValueDomain : std::uint8_t {
    Canonical,
    Venue,
};

struct NormalizedExecutionReport final {
    VenueId venue{};
    std::uint64_t session_epoch{};
    std::string wire_order_id;
    std::string execution_id;
    std::string durable_report_key;
    ExecutionKind kind{ExecutionKind::Acknowledged};
    NormalizedOrderStatus status{NormalizedOrderStatus::Pending};
    std::optional<Quantity> last_quantity;
    std::optional<Price> last_price;
    Quantity cumulative_quantity{};
    std::optional<Quantity> leaves_quantity;
    std::string reject_reason;

    // Labels every numeric quantity/price above. Protocol profiles report
    // Venue; the core converts once to Canonical before any accounting.
    ExecutionValueDomain value_domain{ExecutionValueDomain::Canonical};
};

enum class LocalDispatchState : std::uint8_t {
    DefinitelyUnsent,
    AcceptedLocally,
    Unknown,
};

struct LocalDispatchResult final {
    // Keep the original result fields first so protocol adapters can migrate
    // incrementally; the core rejects default/zero attempt metadata.
    ChildOrderId child{};
    LocalDispatchState state{LocalDispatchState::Unknown};
    std::string detail;

    DispatchAttemptKind kind{DispatchAttemptKind::New};
    std::uint64_t attempt_id{};
    VenueId venue{};
    std::uint64_t session_epoch{};
    std::string wire_order_id;
};

}  // namespace sorx::adapters

namespace sorx {

// This protocol-neutral status is used by the deterministic coordinator. The
// adapter namespace remains the owner of the normalized order-entry contract.
using NormalizedOrderStatus = adapters::NormalizedOrderStatus;

}  // namespace sorx

