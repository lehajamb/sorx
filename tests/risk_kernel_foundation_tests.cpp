#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/risk/risk_kernel.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <utility>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<bool> armed{false};
std::atomic<std::size_t> count{0U};

void record() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        count.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(const std::size_t size) {
    record();
    if (void* const result = std::malloc(size == 0U ? 1U : size)) {
        return result;
    }
    throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(
    const std::size_t size,
    const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* const result =
            _aligned_malloc(size == 0U ? 1U : size, alignment)) {
        return result;
    }
#else
    const std::size_t nonzero = size == 0U ? 1U : size;
    const std::size_t rounded =
        ((nonzero + alignment - 1U) / alignment) * alignment;
    if (void* const result = std::aligned_alloc(alignment, rounded)) {
        return result;
    }
#endif
    throw std::bad_alloc{};
}

void deallocate_aligned(void* const memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

class Scope final {
public:
    Scope() noexcept {
        count.store(0U, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    [[nodiscard]] std::size_t allocations() const noexcept {
        return count.load(std::memory_order_relaxed);
    }
};

}  // namespace allocation_probe

void* operator new(const std::size_t size) {
    return allocation_probe::allocate(size);
}
void* operator new[](const std::size_t size) {
    return allocation_probe::allocate(size);
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* const memory, const std::size_t) noexcept {
    std::free(memory);
}
void* operator new(
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](
    const std::size_t size,
    const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete(
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}
void operator delete[](
    void* const memory,
    const std::size_t,
    const std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(memory);
}

namespace {

using namespace sorx;
using namespace sorx::risk;

[[nodiscard]] PayloadDigest digest(const std::uint8_t seed) noexcept {
    PayloadDigest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(seed + index);
    }
    return result;
}

[[nodiscard]] RiskLimits limits() noexcept {
    return RiskLimits{Quantity{50U},
                      Notional{5'000U},
                      Quantity{100U},
                      Notional{10'000U},
                      2U,
                      Price{10},
                      Price{200}};
}

[[nodiscard]] RiskKernel make_kernel(
    const std::uint32_t normal_capacity = 100U,
    const std::uint32_t cancel_capacity = 20U,
    const std::size_t dedupe_capacity = 32U,
    const RiskLimits account_limits = limits(),
    const RiskLimits client_limits = limits()) {
    const std::array accounts{
        AccountRiskConfig{10U, RiskPermission::NewAndCancel, account_limits},
        AccountRiskConfig{20U, RiskPermission::Cancel, account_limits},
    };
    const std::array clients{
        ClientRiskConfig{1U,
                         10U,
                         RiskPermission::NewAndCancel,
                         client_limits,
                         {normal_capacity, 1U, 1'000U},
                         {cancel_capacity, 1U, 1'000U}},
        ClientRiskConfig{2U,
                         10U,
                         RiskPermission::Cancel,
                         client_limits,
                         {normal_capacity, 1U, 1'000U},
                         {cancel_capacity, 1U, 1'000U}},
        ClientRiskConfig{3U,
                         20U,
                         RiskPermission::NewAndCancel,
                         client_limits,
                         {normal_capacity, 1U, 1'000U},
                         {cancel_capacity, 1U, 1'000U}},
    };
    const std::array restricted{InstrumentId{9U}};
    return RiskKernel{
        RiskKernelConfig{7U,
                         11U,
                         {0x0123456789abcdefULL, 0xfedcba9876543210ULL},
                         8U,
                         4U,
                         8U,
                         dedupe_capacity,
                         4U},
        accounts, clients, restricted};
}

[[nodiscard]] RiskCommand new_order(
    const RequestId request,
    const std::uint64_t parent,
    const std::uint64_t quantity = 10U,
    const std::int64_t price = 100,
    const std::uint32_t instrument = 1U,
    const ClientId client = 1U,
    const AccountId account = 10U) noexcept {
    return RiskCommand{RiskCommandKind::NewOrder,
                       request,
                       digest(static_cast<std::uint8_t>(request)),
                       client,
                       account,
                       ParentOrderId{parent},
                       InstrumentId{instrument},
                       Side::Buy,
                       Quantity{quantity},
                       Price{price}};
}

[[nodiscard]] RiskCommand cancel(
    const RequestId request,
    const std::uint64_t parent,
    const ClientId client = 1U,
    const AccountId account = 10U,
    const std::uint32_t instrument = 1U) noexcept {
    return RiskCommand{RiskCommandKind::Cancel,
                       request,
                       digest(static_cast<std::uint8_t>(request)),
                       client,
                       account,
                       ParentOrderId{parent},
                       InstrumentId{instrument},
                       Side::Buy,
                       Quantity{},
                       Price{}};
}

[[nodiscard]] RiskPlanResult plan_commit(
    RiskKernel& kernel,
    const RiskCommand& command,
    const std::uint64_t time) {
    const RiskPlanResult planned = kernel.plan(command, time);
    assert(planned.disposition == RiskPlanDisposition::Planned);
    assert(kernel.commit(planned.delta) == RiskCommitStatus::Committed);
    return planned;
}

void test_plan_commit_and_accounting_conservation() {
    RiskKernel kernel = make_kernel();
    const RiskKernelMetadata before = kernel.metadata();
    const RiskPlanResult planned = kernel.plan(new_order(1U, 100U), 1U);
    assert(planned.allowed());
    assert(planned.delta.reservation_quantity == Quantity{10U});
    assert(planned.delta.reservation_notional == Notional{1'000U});
    assert(kernel.metadata().kernel_state_generation ==
           before.kernel_state_generation);
    assert(kernel.metadata().active_parents == 0U);

    assert(kernel.commit(planned.delta) == RiskCommitStatus::Committed);
    assert(kernel.client_counters(1U).open_quantity == Quantity{10U});
    assert(kernel.client_counters(1U).open_notional == Notional{1'000U});
    assert(kernel.account_counters(10U).open_orders == 1U);
    assert(kernel.active_parent(ParentOrderId{100U}).remaining_quantity ==
           Quantity{10U});

    assert(kernel.fill(ParentOrderId{100U}, Quantity{4U}) ==
           RiskAccountingStatus::Applied);
    assert(kernel.active_parent(ParentOrderId{100U}).remaining_quantity ==
           Quantity{6U});
    assert(kernel.client_counters(1U).open_notional == Notional{600U});
    assert(kernel.release(ParentOrderId{100U}, Quantity{2U}) ==
           RiskAccountingStatus::Applied);
    assert(kernel.client_counters(1U).open_quantity == Quantity{4U});
    assert(kernel.terminal(ParentOrderId{100U}) ==
           RiskAccountingStatus::Applied);
    assert(!kernel.active_parent(ParentOrderId{100U}).found);
    assert(kernel.client_counters(1U).open_quantity == Quantity{});
    assert(kernel.client_counters(1U).open_notional == Notional{});
    assert(kernel.client_counters(1U).open_orders == 0U);
    assert(kernel.account_counters(10U).open_orders == 0U);
}

void test_static_limits_permissions_restrictions_and_kill() {
    {
        RiskKernel kernel = make_kernel();
        assert(kernel.plan(new_order(1U, 1U, 10U, 201), 1U).verdict ==
               RiskVerdict::PriceOutsideCollar);
        assert(kernel.plan(new_order(2U, 2U, 51U, 50), 2U).verdict ==
               RiskVerdict::OrderQuantityExceeded);
        assert(kernel.plan(new_order(3U, 3U, 30U, 200), 3U).verdict ==
               RiskVerdict::OrderNotionalExceeded);
        assert(kernel.plan(new_order(4U, 4U, 1U, 100, 9U), 4U).verdict ==
               RiskVerdict::RestrictedInstrument);

        RiskCommand overflow = new_order(5U, 5U);
        overflow.quantity = Quantity{std::numeric_limits<std::uint64_t>::max()};
        overflow.limit_price = Price{std::numeric_limits<std::int64_t>::max()};
        assert(kernel.plan(overflow, 5U).verdict ==
               RiskVerdict::OrderNotionalExceeded);
    }
    {
        RiskKernel kernel = make_kernel();
        assert(kernel.plan(new_order(1U, 1U, 1U, 100, 1U, 2U, 10U), 1U)
                   .verdict == RiskVerdict::ClientPermissionDenied);
        assert(kernel.plan(new_order(2U, 2U, 1U, 100, 1U, 3U, 20U), 2U)
                   .verdict == RiskVerdict::AccountPermissionDenied);
        assert(kernel.plan(new_order(3U, 3U, 1U, 100, 1U, 1U, 20U), 3U)
                   .verdict == RiskVerdict::ClientAccountMismatch);
    }
    {
        RiskKernel kernel = make_kernel();
        assert(kernel.set_kill_switch(true));
        assert(kernel.plan(new_order(1U, 1U), 1U).verdict ==
               RiskVerdict::KillSwitchEngaged);
    }
}

void test_gross_limits_and_open_order_limit() {
    RiskKernel open_orders = make_kernel();
    assert(plan_commit(open_orders, new_order(1U, 1U), 1U).allowed());
    assert(plan_commit(open_orders, new_order(2U, 2U), 2U).allowed());
    assert(open_orders.plan(new_order(3U, 3U), 3U).verdict ==
           RiskVerdict::ClientOpenOrderLimitExceeded);

    RiskLimits tight = limits();
    tight.maximum_single_order_quantity = Quantity{10U};
    tight.maximum_single_order_notional = Notional{2'000U};
    tight.maximum_open_quantity = Quantity{15U};
    tight.maximum_open_notional = Notional{3'000U};
    tight.maximum_open_orders = 10U;
    RiskKernel gross = make_kernel(100U, 20U, 32U, tight, tight);
    assert(plan_commit(gross, new_order(1U, 1U, 10U, 100), 1U).allowed());
    assert(gross.plan(new_order(2U, 2U, 6U, 100), 2U).verdict ==
           RiskVerdict::ClientOpenQuantityExceeded);

    RiskLimits account_quantity = limits();
    account_quantity.maximum_single_order_quantity = Quantity{10U};
    account_quantity.maximum_single_order_notional = Notional{2'000U};
    account_quantity.maximum_open_quantity = Quantity{15U};
    account_quantity.maximum_open_notional = Notional{3'000U};
    account_quantity.maximum_open_orders = 10U;
    RiskLimits client_wide = limits();
    client_wide.maximum_open_orders = 10U;
    RiskKernel account_gross =
        make_kernel(100U, 20U, 32U, account_quantity, client_wide);
    assert(plan_commit(account_gross, new_order(1U, 1U, 10U, 100), 1U)
               .allowed());
    assert(account_gross.plan(new_order(2U, 2U, 6U, 100), 2U).verdict ==
           RiskVerdict::AccountOpenQuantityExceeded);

    RiskLimits account_notional = limits();
    account_notional.maximum_single_order_notional = Notional{1'500U};
    account_notional.maximum_open_notional = Notional{1'500U};
    account_notional.maximum_open_orders = 10U;
    RiskKernel account_credit =
        make_kernel(100U, 20U, 32U, account_notional, client_wide);
    assert(plan_commit(account_credit, new_order(1U, 1U, 10U, 100), 1U)
               .allowed());
    assert(account_credit.plan(new_order(2U, 2U, 5U, 101), 2U).verdict ==
           RiskVerdict::AccountOpenNotionalExceeded);

    RiskLimits account_orders = limits();
    account_orders.maximum_open_orders = 1U;
    RiskKernel account_order_limit =
        make_kernel(100U, 20U, 32U, account_orders, client_wide);
    assert(plan_commit(account_order_limit, new_order(1U, 1U), 1U)
               .allowed());
    assert(account_order_limit.plan(new_order(2U, 2U), 2U).verdict ==
           RiskVerdict::AccountOpenOrderLimitExceeded);
}

void test_dedupe_replay_conflict_capacity_and_forget() {
    RiskKernel kernel = make_kernel();
    const RiskCommand original = new_order(1U, 100U);
    assert(plan_commit(kernel, original, 1U).allowed());
    const RiskPlanResult replay = kernel.plan(original, 2U);
    assert(replay.disposition == RiskPlanDisposition::DuplicateReplay);
    assert(replay.verdict == RiskVerdict::Allowed);
    assert(kernel.commit(replay.delta) == RiskCommitStatus::ReplayNoop);
    assert(kernel.plan(original, 3U).disposition ==
           RiskPlanDisposition::DuplicateReplay);

    RiskCommand conflict = original;
    conflict.payload_digest[0] ^= std::byte{0x01U};
    assert(kernel.plan(conflict, 4U).disposition ==
           RiskPlanDisposition::DuplicateConflict);
    assert(kernel.forget_request(1U, 1U));
    assert(kernel.plan(original, 5U).verdict == RiskVerdict::DuplicateParent);

    RiskKernel bounded = make_kernel(100U, 20U, 1U);
    assert(plan_commit(bounded, new_order(1U, 1U), 1U).allowed());
    const RiskPlanResult full = bounded.plan(new_order(2U, 2U), 2U);
    assert(full.disposition == RiskPlanDisposition::CapacityExceeded);
    assert(full.verdict == RiskVerdict::DedupeCapacityExceeded);
}

void test_stale_and_forged_deltas() {
    RiskKernel kernel = make_kernel();
    RiskPlanResult forged = kernel.plan(new_order(1U, 1U), 1U);
    assert(forged.allowed());
    ++forged.delta.reservation_quantity.units;
    assert(kernel.commit(forged.delta) == RiskCommitStatus::ForgedDelta);

    RiskPlanResult wrong_risk = kernel.plan(new_order(2U, 2U), 2U);
    ++wrong_risk.delta.risk_generation;
    assert(kernel.commit(wrong_risk.delta) ==
           RiskCommitStatus::RiskGenerationMismatch);

    const RiskPlanResult first = kernel.plan(new_order(3U, 3U), 3U);
    const RiskPlanResult stale = kernel.plan(new_order(4U, 4U), 3U);
    assert(kernel.commit(first.delta) == RiskCommitStatus::Committed);
    assert(kernel.commit(stale.delta) == RiskCommitStatus::StaleDelta);
}

void test_cancel_reserve_progress_under_normal_saturation() {
    RiskKernel kernel = make_kernel(1U, 2U);
    assert(plan_commit(kernel, new_order(1U, 100U), 1U).allowed());
    const RiskPlanResult saturated = kernel.plan(new_order(2U, 101U), 2U);
    assert(saturated.verdict == RiskVerdict::NormalRateLimited);
    assert(kernel.commit(saturated.delta) == RiskCommitStatus::Committed);
    assert(kernel.client_counters(1U).normal_tokens == 0U);
    assert(kernel.client_counters(1U).cancel_tokens == 2U);

    const RiskPlanResult cancellation = kernel.plan(cancel(3U, 100U), 3U);
    assert(cancellation.allowed());
    assert(kernel.commit(cancellation.delta) == RiskCommitStatus::Committed);
    assert(kernel.client_counters(1U).cancel_tokens == 1U);

    assert(kernel.set_kill_switch(true));
    assert(kernel.plan(new_order(4U, 102U), 4U).verdict ==
           RiskVerdict::KillSwitchEngaged);
    const RiskPlanResult cancel_while_killed =
        kernel.plan(cancel(5U, 100U), 5U);
    assert(cancel_while_killed.allowed());
    assert(kernel.commit(cancel_while_killed.delta) ==
           RiskCommitStatus::Committed);
}

void test_zero_hot_allocations() {
    RiskKernel kernel = make_kernel();
    std::size_t allocations = 0U;
    {
        allocation_probe::Scope scope;
        const RiskPlanResult planned = kernel.plan(new_order(1U, 1U), 1U);
        assert(planned.allowed());
        assert(kernel.commit(planned.delta) == RiskCommitStatus::Committed);
        assert(kernel.client_counters(1U).open_orders == 1U);
        const RiskPlanResult cancellation = kernel.plan(cancel(2U, 1U), 2U);
        assert(cancellation.allowed());
        assert(kernel.commit(cancellation.delta) ==
               RiskCommitStatus::Committed);
        assert(kernel.fill(ParentOrderId{1U}, Quantity{4U}) ==
               RiskAccountingStatus::Applied);
        assert(kernel.terminal(ParentOrderId{1U}) ==
               RiskAccountingStatus::Applied);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
}

void test_atomic_recovery_snapshot_and_live_gate() {
    RiskKernel source = make_kernel();
    assert(plan_commit(source, new_order(1U, 100U), 10U).allowed());
    assert(plan_commit(source, new_order(2U, 101U, 5U, 120), 20U)
               .allowed());
    assert(source.fill(ParentOrderId{100U}, Quantity{4U}) ==
           RiskAccountingStatus::Applied);
    assert(source.set_kill_switch(true));

    std::array<RecoveredAccountRiskState, 4U> accounts{};
    std::array<RecoveredClientRiskState, 8U> clients{};
    std::array<RecoveredParentRiskState, 8U> parents{};
    std::array<RecoveredDedupeRiskState, 32U> dedupe{};
    const RiskRecoveryExportResult exported =
        source.export_recovery_snapshot(accounts, clients, parents, dedupe);
    assert(exported.exported());
    assert(exported.account_count == 2U);
    assert(exported.client_count == 3U);
    assert(exported.active_parent_count == 2U);
    assert(exported.dedupe_count == 2U);

    RiskKernel restored = make_kernel();
    assert(restored.begin_recovery());
    assert(restored.recovery_only());
    assert(restored.plan(new_order(9U, 900U), 30U).disposition ==
           RiskPlanDisposition::RecoveryOnly);
    assert(restored.commit(RiskDelta{}) == RiskCommitStatus::RecoveryOnly);
    assert(restored.fill(ParentOrderId{100U}, Quantity{1U}) ==
           RiskAccountingStatus::RecoveryOnly);

    const RiskRecoveryKernelState kernel_state =
        source.recovery_kernel_state();
    const ScopeRiskCounters before = restored.client_counters(1U);
    accounts[0U].open_orders += 1U;
    assert(restored.import_recovery_snapshot(
               kernel_state,
               std::span{accounts}.first(exported.account_count),
               std::span{clients}.first(exported.client_count),
               std::span{parents}.first(exported.active_parent_count),
               std::span{dedupe}.first(exported.dedupe_count)) ==
           RiskRecoveryImportStatus::CounterMismatch);
    assert(restored.client_counters(1U).open_orders == before.open_orders);
    accounts[0U].open_orders -= 1U;

    std::size_t allocations{};
    {
        allocation_probe::Scope scope;
        assert(restored.import_recovery_snapshot(
                   kernel_state,
                   std::span{accounts}.first(exported.account_count),
                   std::span{clients}.first(exported.client_count),
                   std::span{parents}.first(exported.active_parent_count),
                   std::span{dedupe}.first(exported.dedupe_count)) ==
               RiskRecoveryImportStatus::Imported);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    assert(restored.metadata().kill_switch);
    assert(restored.client_counters(1U).open_quantity == Quantity{11U});
    assert(restored.active_parent(ParentOrderId{100U}).remaining_quantity ==
           Quantity{6U});
    assert(restored.complete_recovery());
    assert(!restored.recovery_only());
    assert(restored.plan(new_order(1U, 100U), 30U).disposition ==
           RiskPlanDisposition::DuplicateReplay);
    assert(restored.fill(ParentOrderId{100U}, Quantity{2U}) ==
           RiskAccountingStatus::Applied);
    assert(restored.terminal(ParentOrderId{101U}) ==
           RiskAccountingStatus::Applied);
}

void test_authenticated_incremental_recovery_delta() {
    RiskKernel planner = make_kernel();
    const RiskPlanResult planned = planner.plan(new_order(1U, 100U), 10U);
    assert(planned.allowed());

    RiskKernel restored = make_kernel();
    assert(restored.begin_recovery());
    std::size_t allocations{};
    {
        allocation_probe::Scope scope;
        assert(restored.restore_recovered_delta(planned.delta) ==
               RiskRecoveryImportStatus::Imported);
        assert(restored.restore_recovered_delta(planned.delta) ==
               RiskRecoveryImportStatus::AlreadyImported);
        assert(restored.apply_recovered_release(
                   ParentOrderId{100U}, Quantity{3U}) ==
               RiskAccountingStatus::Applied);
        allocations = scope.allocations();
    }
    assert(allocations == 0U);
    RiskDelta forged = planned.delta;
    forged.command.payload_digest[0U] ^= std::byte{1U};
    assert(restored.restore_recovered_delta(forged) ==
           RiskRecoveryImportStatus::ForgedDelta);
    assert(restored.complete_recovery());
    assert(restored.client_counters(1U).open_quantity == Quantity{7U});
}

static_assert(noexcept(std::declval<const RiskKernel&>().plan(
    RiskCommand{}, 1U)));
static_assert(noexcept(std::declval<RiskKernel&>().commit(RiskDelta{})));
static_assert(noexcept(std::declval<RiskKernel&>().begin_recovery()));
static_assert(noexcept(std::declval<RiskKernel&>().restore_recovered_delta(
    RiskDelta{})));
static_assert(noexcept(std::declval<RiskKernel&>().apply_recovered_release(
    ParentOrderId{}, Quantity{})));
static_assert(noexcept(std::declval<const RiskKernel&>()
    .export_recovery_snapshot(
        std::span<RecoveredAccountRiskState>{},
        std::span<RecoveredClientRiskState>{},
        std::span<RecoveredParentRiskState>{},
        std::span<RecoveredDedupeRiskState>{})));

}  // namespace

int main() {
    test_plan_commit_and_accounting_conservation();
    test_static_limits_permissions_restrictions_and_kill();
    test_gross_limits_and_open_order_limit();
    test_dedupe_replay_conflict_capacity_and_forget();
    test_stale_and_forged_deltas();
    test_cancel_reserve_progress_under_normal_saturation();
    test_zero_hot_allocations();
    test_atomic_recovery_snapshot_and_live_gate();
    test_authenticated_incremental_recovery_delta();
    return 0;
}
