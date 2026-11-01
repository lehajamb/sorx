# MockX and QuickFIX Integration

SORX provides a deterministic in-process MockX example and separate native market-data and QuickFIX adapters. The example is the starting point for understanding routing. Native adapters add sockets, callbacks, session identity, and persistent FIX sequence stores; they still require application-owned wiring and venue acceptance work.

## Choose the integration boundary

| Boundary | What it exercises | Requirements |
|---|---|---|
| `sorx_multi_mockx_demo` | Two normalized feeds, `SorRuntime`, durable order ownership, normalized FIX fields, fills, and conservation | `SORX_ENABLE_INTEGRATION=ON`; no external service or QuickFIX installation |
| `MockXMdAdapter` | MockX MoldUDP64/ITCH and GLIMPSE/SoupBinTCP decoding through a bounded raw-input seam | Bytes supplied by a caller; no socket ownership in this class |
| `MockXMdSession` | UDP receive, multicast/unicast selection, GLIMPSE connection/login/retry, and feed orchestration | Configured network endpoints and publication sink |
| `QuickFixSessionAdapter` | One exact FIX session, normalized callbacks, new/cancel dispatch results, persistent native store | `SORX_ENABLE_QUICKFIX=ON` and a discoverable QuickFIX C++ installation |
| `MockXVenueRuntime` | One venue's market-data session, QuickFIX adapter, bounded callback queues, adjacency, egress, and recovery gating | QuickFIX-enabled build and an already constructed `SorRuntime` |

The demo's FIX output is a field-value transcript, not a transmitted FIX session. The [Getting Started](https://github.com/lehajamb/sorx/wiki/Getting-Started) result demonstrates deterministic profile behavior; it does not prove remote server connectivity or certification.

## Build the optional QuickFIX layer

Follow [Build and Installation](https://github.com/lehajamb/sorx/wiki/Build-and-Installation) to install and locate a compatible QuickFIX C++ package. With `SORX_QUICKFIX_PREFIX` set to that installation prefix, use a separate build tree:

```powershell
cmake -S . -B build-wiki-quickfix -A x64 -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=ON -DSORX_ENABLE_INTEGRATION=ON "-DCMAKE_PREFIX_PATH=$env:SORX_QUICKFIX_PREFIX"
cmake --build build-wiki-quickfix --config Release --parallel
ctest --test-dir build-wiki-quickfix -C Release -R "^sorx_(quickfix_session|quickfix_store_recovery|mockx_venue_runtime)_tests$" --output-on-failure
```

These commands are optional; the ordinary `build-wiki` quickstart has QuickFIX disabled. The tests using `QuickFixTestTransport` retain the concrete adapter's normalization and bounded sink behavior but do not substitute for the real `FileStoreFactory` reopen test.

## Wire one venue runtime

The intended ownership sequence is:

1. Load validated embedded configuration and construct `SorRuntime` with a profile-owned `WireIdentityFactory`. Construction completes journal/checkpoint replay and leaves new routing closed.
2. Construct one `MockXVenueRuntime` per configured venue, passing a reference to that runtime and matching market-data/order-entry identities.
3. Start each venue runtime once. It owns its worker, native market-data session, and concrete QuickFIX adapter.
4. Allow the bounded event paths to publish books, session state, and dispatch outcomes. Use the versioned `adjacency_view()` for control-plane inspection.
5. Stop and destroy all venue runtimes before destroying the referenced `SorRuntime`.

The instance is one-shot. A failed `start()` is a closed startup gate, not permission to continue dispatch. Offline injection methods are restricted to their explicit test seams so a second producer cannot enter a native production callback path.

`MockXVenueAdjacencyView` combines venue identity, generation, market-data/order-entry/egress states, readiness flags, and candidate views. Its generation advances when relevant book or health state changes. Runtime state names distinguish `RecoveryOnly`, `Operational`, `Degraded`, and `Failed`; check `last_error()`, `last_error_detail()`, and loss/publication counters when diagnosing a transition.

## Configure market-data transport

`MockXMdSessionConfig` composes adapter bindings and network endpoints. The adapter maps each MockX stock locate/symbol to a SORX instrument. The UDP endpoint defaults to bind address `0.0.0.0` and port `18000`; an empty multicast group selects unicast. The GLIMPSE endpoint defaults to `127.0.0.1:18001` and username `GLIMP`, with the password supplied by the application.

These are constructor defaults, not discovery or a complete live-venue deployment. Set them to the intended MockX environment and provision credentials outside committed examples. Transport queues, raw-input sizes, Soup buffering, snapshot limits, heartbeat timeouts, and retry/backoff limits are explicit configuration fields.

Transport threads feed bounded queues; the coordinator thread alone mutates the feed engine. `MockXMdPublicationSink::try_publish()` must return immediately. Returning false means the event was not retained and fails the session closed. The provided `BoundedMockXMdPublicationSink` is SPSC: keep exactly one consumer for `try_pop()`.

## Preserve FIX session identity and storage

`QuickFixSessionIdentity` consists of `BeginString`, sender/target CompIDs, and optional session qualifier. The production settings file must resolve exactly one session matching that identity. Native startup requires a nonempty, usable `FileStorePath` and constructs `FileStoreFactory` before the initiator can log on. Missing or unusable persistent storage rejects startup.

Keep the settings identity, SORX venue/session epoch, profile identity, and configured dictionary identity consistent. Do not rebuild child identifiers at dispatch time: the core has already recorded profile-owned wire identities before releasing effects. Local send acceptance and a venue execution report remain distinct events; [Order Lifecycle](https://github.com/lehajamb/sorx/wiki/Order-Lifecycle) describes the ownership consequences.

Native QuickFIX and offline test callbacks enter the same identity checks and normalization seam. Sink methods are bounded, nonblocking publication attempts. Lost order-entry events degrade the session; callback overload must not silently leave it ready.

## Recovery and egress

`MockXVenueRuntime` drains cancel/risk-off traffic before new/replacement traffic for its venue. A feed failure withdraws eligibility for new price-sensitive routing; it does not require a new route lookup to cancel a recorded child.

MockX has no order-status query in this release. Its conservative reconciliation helper classifies only `Prepared` as definitely unsent. Every other nonterminal state, including a formerly working or cancel-pending child, is unresolved and retains its reservation. Restoring a live feed or logging on again does not resolve that ownership.

Automatic recovery completion is enabled by default, but the venue runtime does not ask `SorRuntime` to reopen routing until every configured instrument has a live book and the exact FIX session is ready. The runtime then enforces its own child-reconciliation and accounting gates. See [Persistence and Recovery](https://github.com/lehajamb/sorx/wiki/Persistence-and-Recovery).

## Focused diagnostics

| Symptom | Inspect |
|---|---|
| Startup fails | Exact session selection, `FileStorePath`, configured identities, and `last_error_detail()` |
| Feed never reaches live | Snapshot session/cut, buffered sequences, GLIMPSE authentication, and raw/adapter queue-loss counters |
| Session degrades under load | Bounded callback capacity, consumer progress, and retained-event failure counters |
| Recovery remains closed after logon | Unresolved children, per-instrument live/fresh books, session epoch, and accounting consistency |
| Demo passes but a remote venue fails | Native framing/session tests and the actual server configuration; the demo sends no network traffic |

Native Nasdaq/Cboe codec foundations belong to the separate [Protocol and Connector Foundations](https://github.com/lehajamb/sorx/wiki/Protocol-and-Connector-Foundations) page. They are not additional ready-to-run `MockXVenueRuntime` replacements.

## Source references

- [Venue ownership, lifetime, adjacency, and recovery API](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/adapters/mockx_venue_runtime.hpp)
- [Native market-data session configuration and sink contracts](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/adapters/mockx_md_session.hpp)
- [QuickFIX startup, identity checks, and dispatch implementation](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/adapters/quickfix_session.cpp)
- [Persistent sequence/message-store reopen tests](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/quickfix_store_recovery_tests.cpp)
- [Venue lifecycle, cancellation priority, and callback-loss tests](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/mockx_venue_runtime_tests.cpp)
