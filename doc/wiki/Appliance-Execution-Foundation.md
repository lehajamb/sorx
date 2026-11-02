# Appliance execution foundation

Use this page when integrating or reviewing the bounded appliance libraries.
They implement routing, risk, dispatch and recovery contracts, but the shipped
process shells do not yet compose them into an operational appliance.

The embedded `SorCore`/`SorRuntime` remains the behavioral reference. Its
coordinator, journal and adapter queues are a separate execution path; they
are not inserted into the proposed appliance hot path. For that model, start
with [Embedded C++ API](https://github.com/lehajamb/sorx/wiki/Embedded-Cpp-API).

## Prepare dependencies before routing

The appliance path uses fixed capacities and a single coordinator owner.
Provision its state pools, book views, risk configuration, venue registry,
encoders, recorder and queues before admitting commands.

| Component | Integration contract |
| --- | --- |
| `dataplane::RouteKernel` | Plans against pinned books, credits and generations; commits the bounded route/ownership delta separately. |
| `risk::RiskKernel` | Plans client/account risk without mutation, binds the delta to generations, and maintains reservations and bounded deduplication. |
| `fastpath::RiskDispatchPipeline` | Composes risk, route planning, one durable batch, commits and native TX queueing. |
| `fastpath::VenueTxQueues` | One coordinator producer and one TX-owner consumer; separate normal and cancel rings. |
| Recorder and TX gate | Authorize an exact recorded effect before it can reach the transport. |

`RouteKernel` defaults to `AdaptivePolicyMode::Required` and
`WireIdentityMode::RequireRegistered`. Each candidate venue needs a registered
identity planner. Startup rejects incomplete planner coverage. Optional
adaptive policy and generic reference identities are explicit simulation/test
choices, not production defaults.

Adaptive policy evaluates integer-valued, effective-dated evidence. Protected
quotes, permissions, health, capability, risk, throttles and credits constrain
eligibility before deterministic ranking. Decisions retain the policy, fee,
metric, weight and compliance generations used.

## Follow the dispatch order

```text
pin books, credits and generations
  -> plan client/account risk and route ownership
  -> encode exact venue frames; preflight queue capacity
  -> append one atomic durable batch
  -> validate durable token, durability domain and fence
  -> commit bounded risk and route state
  -> derive one exact-effect permit per frame
  -> enqueue frame plus permit for the venue TX owner
  -> revalidate the permit at the transport boundary
```

The current route-only and composite dispatch records use schema **v3**.
The composite batch includes the command, authenticated risk delta, risk
projections, routing inputs, adaptive evidence, native frames and their
external-effect authorization list. An authorization binds the venue,
child/attempt, session ID and epoch, ownership generation, dispatch class,
frame size and SHA-256.

Inspect `RiskDispatchResult`, including its status and recovery requirement.
`Dispatched` means the pipeline completed its commits and queueing; it does
not mean the venue acknowledged or filled the order.

## Handle failures without losing ownership

Before durable authorization, rejected planning, encoding or capacity checks
leave active risk and TX queues unchanged. A recorder that reports a commit
without enough token material still creates a recovery obligation; absence
of a usable token is not proof that no durable record exists.

After durable authorization, a failed risk/route commit or queue invariant
latches recovery-only state. The durable batch is authoritative. Do not retry
through another venue, attempt an in-process rollback, or clear the failure
flag to resume routing. Preserve the recovery requirement and reconstruct
ownership through durable replay and reconciliation.

The default required durability domain is `HardwarePersistent`. The checked-in
volatile recorder reports `VolatileSoftwareReference`; production gates reject
it. Portable tests select reference policies explicitly.

## Cancel, replace and recover

Cancels reuse the recorded venue, native session/wire identity and ownership
generation. They receive a new attempt ID and a cancel-lane durable batch
before transition and queueing. Normal-lane saturation must not consume the
separate cancel reservation. Duplicate cancels are idempotent; mass cancel is
cursor-bounded. Replacement waits for terminal or reconciled ownership.

Current checkpoints are **v2** and preserve pending-cancel identity. The
composite recovery ledger also reads checkpoint v1 and frozen dispatch v1/v2
records for migration. Historical `LegacyRangeV0` authorization envelopes are
replay-only; only `ExactEffectsV1` can produce an effect permit. Recovery
ledgers expose no TX or authorization interface.

Production composite-checkpoint restoration requires external provenance by
default. The signed-manifest/TPM/HSM verifier is not supplied. CRC/SHA-only
self-check is a reference-test option, not a production trust substitute.

## Verify an integration

Use the dispatch, risk-dispatch, cancel, native-codec and recovery CTest
families described in [Testing and troubleshooting](https://github.com/lehajamb/sorx/wiki/Testing-and-Troubleshooting).
Exercise rejected generations, queue exhaustion, recorder failures,
post-durable failure, ambiguous ownership, restart and pending cancels.
Passing portable tests does not establish the hardware guarantees in
[Operations and qualification](https://github.com/lehajamb/sorx/wiki/Operations-and-Qualification).

## Source baseline

- [Implementation boundary and missing backends](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/APPLIANCE_IMPLEMENTATION.md)
- [RouteKernel construction and planning contracts](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/dataplane/route_kernel.hpp)
- [Composite dispatch results and recovery requirements](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/fastpath/risk_dispatch_pipeline.hpp)
- [Exact-effect permits and recorder contracts](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/recorder/flight_recorder.hpp)
- [Composite recovery and checkpoint trust policy](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/recovery/risk_dispatch_recovery.hpp)
