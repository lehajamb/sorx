#pragma once

#include "sorx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sorx::risk {

using ClientId = std::uint64_t;
using AccountId = std::uint64_t;
using RequestId = std::uint64_t;
using PayloadDigest = std::array<std::byte, 16U>;

enum class RiskPermission : std::uint8_t {
    None = 0U,
    NewOrder = 1U,
    Cancel = 2U,
    NewAndCancel = 3U,
};

[[nodiscard]] constexpr bool permits(
    const RiskPermission permissions,
    const RiskPermission requested) noexcept {
    return (static_cast<std::uint8_t>(permissions) &
            static_cast<std::uint8_t>(requested)) ==
           static_cast<std::uint8_t>(requested);
}

struct RiskLimits final {
    Quantity maximum_single_order_quantity{};
    Notional maximum_single_order_notional{};
    Quantity maximum_open_quantity{};
    Notional maximum_open_notional{};
    std::uint32_t maximum_open_orders{};
    Price minimum_price{};
    Price maximum_price{};
};

struct TokenBucketConfig final {
    std::uint32_t capacity{};
    std::uint32_t refill_tokens{};
    std::uint64_t refill_interval_ns{};
};

struct AccountRiskConfig final {
    AccountId account_id{};
    RiskPermission permissions{RiskPermission::None};
    RiskLimits limits;
};

struct ClientRiskConfig final {
    ClientId client_id{};
    AccountId account_id{};
    RiskPermission permissions{RiskPermission::None};
    RiskLimits limits;
    TokenBucketConfig normal_rate;
    TokenBucketConfig cancel_rate;
};

struct RiskKernelConfig final {
    std::uint64_t risk_generation{};
    std::uint64_t configuration_generation{};
    std::array<std::uint64_t, 2U> delta_authentication_key{};
    std::size_t client_capacity{};
    std::size_t account_capacity{};
    std::size_t active_parent_capacity{};
    std::size_t dedupe_capacity{};
    std::size_t restricted_instrument_capacity{};
};

enum class RiskCommandKind : std::uint8_t {
    NewOrder,
    Cancel,
};

struct RiskCommand final {
    RiskCommandKind kind{RiskCommandKind::NewOrder};
    RequestId request_id{};
    PayloadDigest payload_digest{};
    ClientId client_id{};
    AccountId account_id{};
    ParentOrderId parent_order_id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price limit_price{};
};

enum class RiskVerdict : std::uint8_t {
    Allowed,
    InvalidCommand,
    ClockRegression,
    UnknownClient,
    UnknownAccount,
    ClientAccountMismatch,
    ClientPermissionDenied,
    AccountPermissionDenied,
    KillSwitchEngaged,
    RestrictedInstrument,
    PriceOutsideCollar,
    OrderQuantityExceeded,
    OrderNotionalExceeded,
    ClientOpenQuantityExceeded,
    ClientOpenNotionalExceeded,
    ClientOpenOrderLimitExceeded,
    AccountOpenQuantityExceeded,
    AccountOpenNotionalExceeded,
    AccountOpenOrderLimitExceeded,
    NormalRateLimited,
    CancelRateLimited,
    DuplicateParent,
    ParentNotFound,
    ParentOwnershipMismatch,
    ActiveParentCapacityExceeded,
    DedupeCapacityExceeded,
    RecoveryOnly,
};

enum class RiskPlanDisposition : std::uint8_t {
    Planned,
    DuplicateReplay,
    DuplicateConflict,
    Invalid,
    CapacityExceeded,
    RecoveryOnly,
};

struct RiskDelta final {
    RiskPlanDisposition disposition{RiskPlanDisposition::Invalid};
    RiskVerdict verdict{RiskVerdict::InvalidCommand};
    RiskCommand command;
    std::uint64_t command_fingerprint{};
    std::uint64_t risk_generation{};
    std::uint64_t configuration_generation{};
    std::uint64_t kernel_state_generation{};
    std::uint64_t monotonic_time_ns{};
    Quantity reservation_quantity{};
    Notional reservation_notional{};
    std::uint32_t normal_tokens_after{};
    std::uint32_t cancel_tokens_after{};
    std::uint64_t normal_refill_time_after{};
    std::uint64_t cancel_refill_time_after{};
    std::uint64_t authentication_tag{};

    [[nodiscard]] bool committable() const noexcept {
        return disposition == RiskPlanDisposition::Planned &&
               authentication_tag != 0U;
    }
};

struct RiskPlanResult final {
    RiskPlanDisposition disposition{RiskPlanDisposition::Invalid};
    RiskVerdict verdict{RiskVerdict::InvalidCommand};
    RiskDelta delta;

    [[nodiscard]] bool allowed() const noexcept {
        return verdict == RiskVerdict::Allowed &&
               (disposition == RiskPlanDisposition::Planned ||
                disposition == RiskPlanDisposition::DuplicateReplay);
    }
};

enum class RiskCommitStatus : std::uint8_t {
    Committed,
    ReplayNoop,
    NotCommittable,
    ForgedDelta,
    RiskGenerationMismatch,
    ConfigurationGenerationMismatch,
    StaleDelta,
    DedupeConflict,
    CapacityExceeded,
    InvariantViolation,
    GenerationExhausted,
    RecoveryOnly,
};

enum class RiskAccountingStatus : std::uint8_t {
    Applied,
    ParentNotFound,
    InvalidQuantity,
    QuantityExceedsReservation,
    InvariantViolation,
    GenerationExhausted,
    RecoveryOnly,
};

struct ScopeRiskCounters final {
    bool found{};
    Quantity open_quantity{};
    Notional open_notional{};
    std::uint32_t open_orders{};
    std::uint32_t normal_tokens{};
    std::uint32_t cancel_tokens{};
};

struct ActiveParentRisk final {
    bool found{};
    ClientId client_id{};
    AccountId account_id{};
    InstrumentId instrument{};
    Quantity remaining_quantity{};
    Notional remaining_notional{};
    Price limit_price{};
};

struct RiskKernelMetadata final {
    std::uint64_t risk_generation{};
    std::uint64_t configuration_generation{};
    std::uint64_t kernel_state_generation{};
    std::uint64_t last_committed_monotonic_time_ns{};
    std::size_t active_parents{};
    std::size_t dedupe_entries{};
    std::size_t dedupe_tombstones{};
    bool kill_switch{};
    bool recovery_only{};
};

// Compact dynamic state exported to a caller-owned checkpoint buffer. Static
// permissions, limits and token-bucket configurations remain owned by the
// signed RiskKernelConfig and are verified by identity during restore.
struct RecoveredAccountRiskState final {
    AccountId account_id{};
    Quantity open_quantity{};
    Notional open_notional{};
    std::uint32_t open_orders{};
};

struct RecoveredClientRiskState final {
    ClientId client_id{};
    AccountId account_id{};
    Quantity open_quantity{};
    Notional open_notional{};
    std::uint32_t open_orders{};
    std::uint32_t normal_tokens{};
    std::uint32_t cancel_tokens{};
    std::uint64_t normal_refill_time_ns{};
    std::uint64_t cancel_refill_time_ns{};
};

struct RecoveredParentRiskState final {
    ParentOrderId parent_order_id{};
    ClientId client_id{};
    AccountId account_id{};
    InstrumentId instrument{};
    Quantity remaining_quantity{};
    Notional remaining_notional{};
    Price limit_price{};
};

struct RecoveredDedupeRiskState final {
    ClientId client_id{};
    RequestId request_id{};
    std::uint64_t command_fingerprint{};
    RiskVerdict verdict{RiskVerdict::InvalidCommand};
    Quantity reservation_quantity{};
    Notional reservation_notional{};
};

struct RiskRecoveryKernelState final {
    std::uint64_t risk_generation{};
    std::uint64_t configuration_generation{};
    std::uint64_t kernel_state_generation{};
    std::uint64_t last_committed_monotonic_time_ns{};
    bool kill_switch{};
};

enum class RiskRecoveryImportStatus : std::uint8_t {
    Imported,
    AlreadyImported,
    NotRecoveryOnly,
    InvalidState,
    ConfigurationMismatch,
    StateGenerationMismatch,
    IdentityMismatch,
    DuplicateIdentity,
    CapacityExceeded,
    CounterMismatch,
    TokenStateInvalid,
    DedupeConflict,
    ForgedDelta,
    InvariantViolation,
    GenerationExhausted,
};

enum class RiskRecoveryExportStatus : std::uint8_t {
    Exported,
    CapacityExceeded,
    InvalidBuffer,
};

struct RiskRecoveryExportResult final {
    RiskRecoveryExportStatus status{RiskRecoveryExportStatus::InvalidBuffer};
    std::size_t account_count{};
    std::size_t client_count{};
    std::size_t active_parent_count{};
    std::size_t dedupe_count{};

    [[nodiscard]] bool exported() const noexcept {
        return status == RiskRecoveryExportStatus::Exported;
    }
};

// Single-writer kernel. Construction is a cold control-plane operation that
// allocates and copies configuration. All methods below are bounded, noexcept,
// and allocation-free after construction.
class RiskKernel final {
public:
    RiskKernel(
        RiskKernelConfig config,
        std::span<const AccountRiskConfig> accounts,
        std::span<const ClientRiskConfig> clients,
        std::span<const InstrumentId> restricted_instruments);
    ~RiskKernel();

    RiskKernel(const RiskKernel&) = delete;
    RiskKernel& operator=(const RiskKernel&) = delete;
    RiskKernel(RiskKernel&&) noexcept;
    RiskKernel& operator=(RiskKernel&&) noexcept;

    [[nodiscard]] RiskPlanResult plan(
        const RiskCommand& command,
        std::uint64_t monotonic_time_ns) const noexcept;
    [[nodiscard]] RiskCommitStatus commit(const RiskDelta& delta) noexcept;

    [[nodiscard]] RiskAccountingStatus release(
        ParentOrderId parent,
        Quantity quantity) noexcept;
    [[nodiscard]] RiskAccountingStatus fill(
        ParentOrderId parent,
        Quantity quantity) noexcept;
    [[nodiscard]] RiskAccountingStatus terminal(
        ParentOrderId parent) noexcept;

    // Recovery is permitted only on a freshly constructed kernel. Live
    // planning, commit and accounting calls fail closed while recovery_only()
    // is true. A snapshot import is validated in full before any live dynamic
    // state is changed; incremental durable deltas are then applied exactly
    // once with their original authenticated state generation.
    [[nodiscard]] bool begin_recovery() noexcept;
    [[nodiscard]] RiskRecoveryImportStatus import_recovery_snapshot(
        const RiskRecoveryKernelState& state,
        std::span<const RecoveredAccountRiskState> accounts,
        std::span<const RecoveredClientRiskState> clients,
        std::span<const RecoveredParentRiskState> active_parents,
        std::span<const RecoveredDedupeRiskState> dedupe_entries) noexcept;
    [[nodiscard]] RiskRecoveryImportStatus restore_recovered_delta(
        const RiskDelta& delta) noexcept;
    [[nodiscard]] RiskAccountingStatus apply_recovered_release(
        ParentOrderId parent,
        Quantity quantity) noexcept;
    [[nodiscard]] RiskAccountingStatus apply_recovered_fill(
        ParentOrderId parent,
        Quantity quantity) noexcept;
    [[nodiscard]] RiskAccountingStatus apply_recovered_terminal(
        ParentOrderId parent) noexcept;
    [[nodiscard]] bool complete_recovery() noexcept;
    [[nodiscard]] bool recovery_only() const noexcept;

    [[nodiscard]] RiskRecoveryKernelState recovery_kernel_state()
        const noexcept;
    [[nodiscard]] RiskRecoveryExportResult export_recovery_snapshot(
        std::span<RecoveredAccountRiskState> accounts,
        std::span<RecoveredClientRiskState> clients,
        std::span<RecoveredParentRiskState> active_parents,
        std::span<RecoveredDedupeRiskState> dedupe_entries) const noexcept;

    [[nodiscard]] bool set_kill_switch(bool engaged) noexcept;
    [[nodiscard]] bool forget_request(
        ClientId client,
        RequestId request) noexcept;

    [[nodiscard]] ScopeRiskCounters client_counters(
        ClientId client) const noexcept;
    [[nodiscard]] ScopeRiskCounters account_counters(
        AccountId account) const noexcept;
    [[nodiscard]] ActiveParentRisk active_parent(
        ParentOrderId parent) const noexcept;
    [[nodiscard]] RiskKernelMetadata metadata() const noexcept;

private:
    [[nodiscard]] RiskCommitStatus commit_impl(
        const RiskDelta& delta) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sorx::risk
