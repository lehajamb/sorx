#pragma once

#include <cstdint>

namespace sorx::ha {

enum class ReplicationPolicy : std::uint8_t {
    LocalDurable,
    DualDurable,
};

struct FenceLease final {
    std::uint64_t leader_node_id{};
    std::uint64_t leadership_epoch{};
    std::uint64_t witness_term{};
    std::uint64_t expires_at_monotonic_ns{};

    [[nodiscard]] constexpr bool well_formed() const noexcept {
        return leader_node_id != 0U && leadership_epoch != 0U &&
            witness_term != 0U && expires_at_monotonic_ns != 0U;
    }
};

enum class DispatchFenceError : std::uint8_t {
    None,
    NoLease,
    WrongLeader,
    LeaseExpired,
    StaleLeadershipEpoch,
    LocalRecordNotDurable,
    RemoteRecordNotDurable,
};

// Single-owner, allocation-free dispatch gate. Cryptographic verification and
// witness transport belong to the HA backend; this class enforces the state
// and monotonicity invariants after a lease has been authenticated.
class ReplicationGate final {
public:
    ReplicationGate(
        std::uint64_t local_node_id,
        ReplicationPolicy policy) noexcept;

    [[nodiscard]] bool install_authenticated_lease(
        const FenceLease& lease) noexcept;
    void revoke() noexcept;

    [[nodiscard]] bool note_local_durable(
        std::uint64_t record_sequence,
        std::uint64_t leadership_epoch) noexcept;
    [[nodiscard]] bool note_remote_durable(
        std::uint64_t record_sequence,
        std::uint64_t leadership_epoch) noexcept;

    [[nodiscard]] DispatchFenceError authorize(
        std::uint64_t record_sequence,
        std::uint64_t leadership_epoch,
        std::uint64_t now_monotonic_ns) const noexcept;

    [[nodiscard]] ReplicationPolicy policy() const noexcept;
    [[nodiscard]] FenceLease lease() const noexcept;
    [[nodiscard]] std::uint64_t local_durable_sequence() const noexcept;
    [[nodiscard]] std::uint64_t remote_durable_sequence() const noexcept;

private:
    std::uint64_t local_node_id_{};
    ReplicationPolicy policy_{ReplicationPolicy::LocalDurable};
    FenceLease lease_{};
    // Authenticated anti-replay floor. Revocation clears authority and durable
    // watermarks but deliberately does not erase this tuple.
    std::uint64_t maximum_authenticated_epoch_{};
    std::uint64_t maximum_authenticated_witness_term_{};
    std::uint64_t local_durable_sequence_{};
    std::uint64_t remote_durable_sequence_{};
};

enum class TakeoverState : std::uint8_t {
    Standby,
    RecoveryOnly,
    Reconciling,
    Ready,
    Revoked,
};

class TakeoverGate final {
public:
    explicit TakeoverGate(ReplicationGate& replication) noexcept;

    [[nodiscard]] bool begin_recovery(
        const FenceLease& authenticated_lease) noexcept;
    [[nodiscard]] bool begin_reconciliation() noexcept;
    [[nodiscard]] bool complete_reconciliation(
        bool accounting_consistent,
        bool venue_sessions_recovered) noexcept;
    void revoke() noexcept;

    [[nodiscard]] TakeoverState state() const noexcept;
    [[nodiscard]] bool may_route_new() const noexcept;

private:
    ReplicationGate* replication_{};
    TakeoverState state_{TakeoverState::Standby};
};

}  // namespace sorx::ha
