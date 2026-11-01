# Embedded Cpp API

The embedded API lets a strategy own a deterministic routing library inside its process. `SorCore` applies ordered protocol-neutral events; `SorRuntime` adds journal durability, serialized ownership, bounded queues, and report delivery. This API is separate from the [appliance execution foundation](https://github.com/lehajamb/sorx/wiki/Appliance-Execution-Foundation).

Start with the runnable [Getting Started](https://github.com/lehajamb/sorx/wiki/Getting-Started) demo. It supplies feed views, dispatch outcomes, and venue reports in process, so the complete ownership contract can be inspected without running an exchange.

## Choose the integration boundary

| Surface | Use it for | Caller responsibility |
|---|---|---|
| `SorCore::apply(CoreEvent)` | Deterministic state-machine tests and explicitly owned orchestration | Supply ordered events; persist records and govern effects yourself. `apply` alone does not flush a journal or send to a venue. |
| `SorRuntime` | Embedded routing with the supplied durable coordinator | Supply validated configuration, durable paths, a wire-identity factory, and adapter events. |
| `RoutingPolicy` | Pure route-policy evaluation | Supply a candidate table and immutable per-venue runtime views; this does not create reservations or children. |

`SorRuntime` serializes its core operations on a coordinator worker. Its public methods are typed C++ calls, not HTTP endpoints. FIX and feed wire objects stay in the adapter boundary.

## Construct a runtime with profile-owned identities

The one-argument `SorRuntime(RuntimeConfiguration)` constructor is deleted. Supply a `shared_ptr<const WireIdentityFactory>` whose immutable profile identities match the validated venue configuration. Production direct-core construction also requires an explicit factory; the two-argument `SorCore` overload exists for test compatibility.

This complete helper opens a runtime for the checked-in MockX INI schema. It constructs the same identity bindings and decimal settings as the acceptance demo. It does not start network sessions or open the recovery gate.

```cpp
#include "sorx/adapters/mockx_fix44.hpp"
#include "sorx/config.hpp"
#include "sorx/runtime.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

std::unique_ptr<sorx::SorRuntime> open_mockx_runtime(
    const std::filesystem::path& ini,
    const std::filesystem::path& journal) {
    auto loaded = sorx::config::ConfigurationLoader::load_ini_file(ini);
    if (!loaded) {
        throw std::runtime_error("invalid SORX configuration");
    }
    using namespace sorx::adapters;
    using namespace sorx::adapters::mockx;
    std::vector<MockXFix44WireIdentityBinding> bindings;
    for (const auto& venue : loaded.configuration->venues) {
        bindings.push_back({
            WireProfileIdentity{
                venue.id, venue.order_entry_profile_id,
                venue.order_entry_profile, venue.order_entry_session_identity,
                venue.dictionary_identity, venue.sender_comp_id,
                venue.target_comp_id},
            MockXFix44Config{2U, 0U, 32U, "SX"}});
    }
    auto factory = std::make_shared<MockXFix44WireIdentityFactory>(
        std::move(bindings));
    sorx::RuntimeConfiguration configuration;
    configuration.validated = std::move(*loaded.configuration);
    configuration.journal_path = journal;
    return std::make_unique<sorx::SorRuntime>(
        std::move(configuration), std::move(factory));
}
```

Link the containing target to `sorx::runtime` and `sorx::order_entry_profiles`. The latter supplies the MockX profile and does not require QuickFIX. Inspect `loaded.errors` in an application to report precise configuration diagnostics; the helper deliberately uses one generic exception. Choose an application-owned journal path and retain it across restarts.

Construction pins the configured profile/session/dictionary identities. Wire IDs for new and cancel intents are created before durable child preparation, validated, persisted, and reused across retries. An adapter must not invent a replacement identity after receiving an effect.

## Commands and observations

| Operation | Result and meaning |
|---|---|
| `submit(ParentOrder)` | Returns a `CommandReceipt`; a successful receipt means the command was durably accepted. |
| `cancel(ParentOrderId)` | Records cancellation intent for the existing parent's children. Completion remains asynchronous. |
| `replace(ParentReplace)` | Records a new total quantity and limit; cancels the old revision before routing its replacement. |
| `trace(ParentOrder)` | Returns current hard-risk checks, version pins, exclusions, allocations, and remainder without submitting the parent. |
| `inspect()` | Returns an owned snapshot of parents, children, generations, reservations, and routing/kill-switch state. |
| `set_venue_admin`, `set_kill_switch` | Apply sequenced, journaled administrative changes. |

A ready runtime can receive the demo's canonical order through this helper:

```cpp
#include "sorx/runtime.hpp"

sorx::CommandReceipt submit_demo_parent(sorx::SorRuntime& runtime) {
    return runtime.submit(sorx::ParentOrder{
        sorx::ParentOrderId{1U}, sorx::InstrumentId{7U},
        sorx::Side::Buy, sorx::Quantity{10U}, sorx::Price{101},
        sorx::RoutingIntent::AggressiveIOC});
}
```

The caller must first publish valid live market/session views and complete recovery. The values above are canonical integer units for the checked-in fixture. Reusing parent ID 1 with an identical stored order reports `idempotent_replay`; a conflicting order with that ID is rejected. Use distinct IDs for distinct parent commands.

Check `receipt.accepted()` and `receipt.error`. Retain `ingress_sequence` and `journal_record_id` for correlation. Command acceptance does not guarantee an executable route: a durable route rejection can follow accepted parent validation. Follow parent reports or `inspect()` for the actual order state.

## Adapter and reporting contract

1. Publish immutable `AdapterEventPayload` values for runtime views, session state, local dispatch results, executions, timeouts, and reconciliation. Check the returned Boolean; rejection or capacity exhaustion must not be silently treated as successful publication.
2. Poll `poll_egress(venue)` for `DispatchNewEffect` or `DispatchCancelEffect`. A returned effect already has a durable `DispatchStarted` record and a dispatch attempt ID. Cancel/risk-off traffic drains before normal traffic for that venue.
3. Preserve venue, session epoch, wire ID, attempt kind, and attempt ID when publishing the corresponding `LocalDispatchResult`. Local acceptance is distinct from a venue acknowledgement.
4. Normalize execution quantities into a labeled `ExecutionValueDomain`; venue-domain reports are converted once using the child's durable conversion identity before accounting.

`IParentReportSink` callbacks run on a dedicated report worker. A referenced sink must outlive the runtime or be detached with `clear_report_sink()` before destruction. Sink changes are barriers for prior reports. Monitor `report_callback_failures()`; report callbacks do not replace durable inspection/recovery state.

Startup remains recovery-only until reconciliation and accounting/market gates agree. Use `recovery_pending()`, `complete_recovery()`, and `failed()` rather than assuming construction proves readiness. See [Persistence and Recovery](https://github.com/lehajamb/sorx/wiki/Persistence-and-Recovery) and [Order Lifecycle](https://github.com/lehajamb/sorx/wiki/Order-Lifecycle).

## Source references

- [Runtime public contract and coordinator](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/runtime.hpp), [implementation](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/runtime.cpp)
- [Core events, effects, reports, and snapshots](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/order_core.hpp)
- [Runnable integration and factory setup](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/examples/multi_mockx/main.cpp)
- [Wire-identity construction, retry, and restore tests](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/wire_identity_tests.cpp)
