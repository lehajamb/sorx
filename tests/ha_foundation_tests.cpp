#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/ha/replication.hpp"

int main() {
    using namespace sorx::ha;

    const FenceLease first{7U, 10U, 3U, 1'000U};

    ReplicationGate local(7U, ReplicationPolicy::LocalDurable);
    assert(local.install_authenticated_lease(first));
    assert(local.note_local_durable(41U, 10U));
    assert(local.authorize(41U, 10U, 999U) == DispatchFenceError::None);
    assert(local.authorize(42U, 10U, 999U) ==
           DispatchFenceError::LocalRecordNotDurable);
    assert(local.authorize(41U, 9U, 999U) ==
           DispatchFenceError::StaleLeadershipEpoch);
    assert(local.authorize(41U, 10U, 1'000U) ==
           DispatchFenceError::LeaseExpired);
    assert(!local.install_authenticated_lease(FenceLease{8U, 11U, 4U, 2'000U}));
    assert(!local.install_authenticated_lease(FenceLease{7U, 9U, 4U, 2'000U}));

    ReplicationGate replay_gate(7U, ReplicationPolicy::LocalDurable);
    assert(replay_gate.install_authenticated_lease(first));
    assert(!replay_gate.install_authenticated_lease(first));
    assert(replay_gate.install_authenticated_lease(
        FenceLease{7U, 10U, 3U, 1'500U}));
    assert(!replay_gate.install_authenticated_lease(
        FenceLease{7U, 10U, 4U, 1'400U}));
    assert(!replay_gate.install_authenticated_lease(
        FenceLease{7U, 11U, 2U, 2'000U}));
    replay_gate.revoke();
    assert(!replay_gate.install_authenticated_lease(
        FenceLease{7U, 10U, 3U, 3'000U}));
    assert(replay_gate.install_authenticated_lease(
        FenceLease{7U, 11U, 4U, 3'000U}));

    ReplicationGate dual(7U, ReplicationPolicy::DualDurable);
    TakeoverGate takeover(dual);
    assert(takeover.begin_recovery(first));
    assert(!takeover.may_route_new());
    assert(takeover.begin_reconciliation());
    assert(!takeover.complete_reconciliation(false, true));
    assert(takeover.complete_reconciliation(true, true));
    assert(takeover.may_route_new());

    assert(dual.note_local_durable(51U, 10U));
    assert(dual.authorize(51U, 10U, 999U) ==
           DispatchFenceError::RemoteRecordNotDurable);
    assert(dual.note_remote_durable(51U, 10U));
    assert(dual.authorize(51U, 10U, 999U) == DispatchFenceError::None);
    assert(dual.install_authenticated_lease(
        FenceLease{7U, 10U, 3U, 1'500U}));
    assert(dual.local_durable_sequence() == 51U);
    assert(dual.remote_durable_sequence() == 51U);
    assert(dual.authorize(51U, 10U, 1'499U) == DispatchFenceError::None);

    const FenceLease next{7U, 11U, 4U, 2'000U};
    assert(dual.install_authenticated_lease(next));
    assert(dual.local_durable_sequence() == 0U);
    assert(dual.remote_durable_sequence() == 0U);
    assert(dual.authorize(51U, 10U, 1'500U) ==
           DispatchFenceError::StaleLeadershipEpoch);

    takeover.revoke();
    assert(!takeover.may_route_new());
    assert(dual.authorize(1U, 11U, 1'500U) == DispatchFenceError::NoLease);
    assert(!takeover.begin_recovery(FenceLease{7U, 11U, 4U, 3'000U}));
    assert(takeover.begin_recovery(FenceLease{7U, 12U, 5U, 3'000U}));
    assert(takeover.state() == TakeoverState::RecoveryOnly);

    return 0;
}
