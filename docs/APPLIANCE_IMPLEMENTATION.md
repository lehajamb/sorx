# SORX appliance implementation boundary

This tree now contains a bounded production-path **software foundation** next
to the original `SorCore`/`SorRuntime` reference implementation. It is not a
finished, venue-certified appliance and it makes no measured latency or
power-loss claim. The physical recorder, accelerated NIC/FPGA backends,
exchange sessions, OS image, security services, and bare-metal qualification
remain release gates.

## Implemented foundation

| Area | Implemented contract |
| --- | --- |
| Dataplane | Fixed-capacity route plans, deterministic child/attempt IDs, startup-sized open-addressed active state, O(1) risk counters, objective quote age, versioned credits/throttles, and no heap use in tested `plan`/`commit` calls. Registered per-venue planners bind each child to an exact native wire identity and venue session ID/epoch; an active index rejects identity reuse within that venue session. RouteKernel defaults to `AdaptivePolicyMode::Required` and `WireIdentityMode::RequireRegistered`, and startup rejects incomplete planner coverage. Optional policy and reference-generic identity must be selected explicitly for tests/simulation. |
| Pre-trade risk | Non-mutating, generation-pinned client/account plans with authenticated deltas; quantity/notional/collar/permission/restricted-symbol/open-order controls; bounded dedupe; independent normal/cancel token buckets; O(1) reservation/fill/release counters; and bounded recovery import/export. Partial IOC remainder release is projected into the durable record before mutation. |
| Dispatch | The route-only and composite client/account pipelines use explicit canonical schema v3 records. A composite batch contains the command, authenticated risk delta, full/final risk projections, pinned route inputs and adaptive evidence, exact native frames, session/wire ownership and a fixed external-effect authorization list. Each entry binds venue, child/attempt, session ID/epoch, ownership generation, dispatch class, exact frame size and SHA-256. Risk/route commits and TX enqueue occur only after a validated master token; every post-durable failure latches recovery-only state. |
| TX boundary | The master `DurableToken` narrows one committed list entry into a compact `DurableEffectPermit` carrying batch/root identity, ordinal and that exact effect. Per-venue normal/cancel SPSC rings carry frame plus permit; the cancel-first pinned owner and recorder-backed gate reject a missing, stale, tampered, mismatched or reused permit before transport. |
| Cancel/replace | A separately reserved cancel pipeline reads the RouteKernel's exact parent/child/venue, session ID/epoch, wire identity, ownership generation and last attempt. Native OUCH/BOE cancel encoders reuse that identity; a new attempt, cancel-lane batch and token are required before state transition and enqueue. Duplicate requests are idempotent, mass cancel is cursor-bounded, replacement is gated on terminal/reconciled ownership, and v2 checkpoints preserve pending-cancel identity across restart. |
| Market data | Startup-sized intrusive L3 order/level storage, open-addressed order index, bounded transactional undo, changed-only book slices, direct registered-buffer decode, and two-phase A/B sequence/gap/heartbeat/age arbitration. Native ITCH 5.0/MoldUDP64 and EDGX PITCH decoders emit one source-sequence-ordered action stream, use transactional directory/locate updates, normalize rollovers, and expose bounded recovery request/response seams. Market data is not journaled synchronously and the codecs remain uncertified. |
| Recorder model | Fixed 4-KiB/32-record batches, mirrored commit ordering, CRC32C, SHA-256 batch/chain hashes, completion-gated master tokens, fixed external-effect lists and compact per-effect permits, reserved normal/cancel/reconciliation lanes, exact replay, and Tier-1 recycling gates. The Tier-1 reference archive enforces contiguous segment IDs and nonoverlapping contiguous batch ranges plus previous/final-root continuity. `ExactEffectsV1` is the only envelope that can mint a permit or authorize TX. Historical `LegacyRangeV0` bytes retain their original hash format for replay, but are deliberately incapable of authorizing an external effect. |
| Fast recovery | The route ledger verifies current v3 canonical batches. The composite risk/route ledger also decodes frozen v1/v2 dispatch schemas for migration, then checks current v3 exact-frame authorizations. Both verify durable-token/hash-chain continuity, durability domain and generation pins. They restore account/client risk, route ownership, exact native session/wire identity, pending cancels, dedupe, monotonic IDs and consumed credits; apply fill/cancel/terminal/reconciliation accounting; and emit caller-buffer v2 checkpoints. Composite checkpoint v1 remains readable, but v2 is current. Production composite restore defaults to an external-provenance verifier; CRC/SHA-only self-check is explicitly reference-only. Neither ledger has a TX or authorization interface. |
| Audit | The recorder-backed source verifies token/batch integrity. It defaults to unverified/zero retention; a nonzero value requires authenticated capacity/rate/generation evidence and is rebound to live recorder capacity using worst-case 4-KiB slot accounting. The segmented reference collector rejects skipped, out-of-order or hash-discontinuous records, resumes only from the sink's highest contiguous sequence/hash, and acknowledges only a committed contiguous range. Acknowledgement remains metadata and does not gate Tier-0 recycling in Tier-1-protected mode. The checked-in source exposes Tier-0 capacity only, and the protobuf defines transport messages, but a qualified Tier-1 source and mTLS server are absent. |
| Client protocol | Fixed big-endian SOBP v1 messages, caller-owned encode buffers, strict decode/CRC/semantic checks, heartbeat state, idempotent request cache, and independent normal/cancel credits. A `SessionAuthenticator` can be created only from a single-use verified-logon capability; its fixed envelope binds session, epoch, direction, key generation and sequence. Authenticated credit exhaustion consumes the sequence consistently. Production MAC/KDF/HSM providers remain an integration gate. |
| FIX compatibility | A separate, fixed-buffer FIX 4.4 translator accepts the bounded D/F/G/q subset, validates framing/checksums/identities, derives stable request/parent identities, emits the common SOBP descriptors and encodes bounded outbound acknowledgements/reports. Socket, logon, mTLS and certification services remain outside this library slice. |
| Venue protocols | Allocation-free Nasdaq OUCH 5.0/Soup and Cboe EDGX BOE codec foundations use caller-owned buffers, exact integer fields, registered route-time identity planners and native new/cancel encoders. An end-to-end test covers native dispatch, cancel, replay, checkpoint, restart and retry with unchanged original identity. The codecs remain explicitly uncertified; session networking, recovery/drop-copy and exchange UAT are release gates. |
| Adaptive policy | An effective-dated, fixed-capacity integer evaluator is integrated into RouteKernel planning. It applies protected-quote, permission, health, capability, risk, throttle and credit gates, then deterministically ranks aggressive/passive venues using pinned fee, metric, weight, policy and compliance generations. Schema v3 records the overlay order, rank, score inputs and exact exclusion evidence. No online ML or floating-point arithmetic enters the decision. |
| Configuration | `DeviceConfigV2`, strict YAML import/export and `sorxctl --migrate-ini` are implemented. The store binds commits to the exact running revision, candidate revision and full candidate SHA-256; dual approvals also bind action, diff, expiry and approver request ID and are single-use. Production requires one routing context, physically distinct management/audit/data interfaces, audit mTLS, opaque TPM/TPM2/PKCS#11 references, a 900-second/500-MB/s recorder budget, at least 4 GiB of Tier 0 with nonzero normal/cancel/reconciliation lanes, and storage covering the checked retention budget plus Tier 0. The normative protobuf exactly mirrors the C++ graph (18 messages, four enums, 132 fields), and a negative-drift test checks fields, ordering, cardinality, scalar mappings and enums. |
| HA | The bounded gate enforces local- versus dual-durable authorization, monotonic leadership/witness tuples, lease expiry and fencing. Revocation clears authority but retains the authenticated anti-replay floor; takeover remains recovery-only until accounting and venue-session reconciliation complete. Witness/replication transports are absent. |
| Platform | Pure fail-closed readiness evaluation covers signed profile identity, CPU/NUMA/isolation, firmware, NIC queues/kernel bypass, PTP, hugepages/memlock, recorder qualification, PLP NVMe, secure/measured boot, TPM/PCR, governor and C-state facts. Production ignores certification relaxation masks and always requires hardware-persistent recorder facts plus cancel/reconciliation reserves. |
| Telemetry | Lock-free-copy snapshot publication for queue watermarks, feed state, risk use, recorder/NVMe lag, PTP, NIC drops, CPU/thermal state, and fixed latency histograms; management readers never synchronously inspect the coordinator. |
| Packaging shell | Five separately buildable command shells, systemd/image inputs, a signed-profile hardware manifest, YAML example, SBE/protobuf schemas, and install/export targets. These are offline/safe shells, not network supervisors. |

The critical new-order path is deliberately ordered as follows:

```text
pin versioned books/credits
        -> plan route + risk + child/attempt/session/wire ownership
        -> encode every exact venue frame and preflight TX capacity
        -> append one atomic durable batch
        -> validate local (and optional remote) durable token + fence
        -> commit bounded kernel delta
        -> narrow one per-frame durable effect permit
        -> enqueue frame + permit directly to the venue TX owner
        -> permit-gated transport/FPGA assertion
```

Any failure before the recorder token leaves active risk and TX queues
unchanged. Any failure after durable authorization is fail-closed: no frame is
sent, and recovery must resolve the recorded authorization before routing can
resume.

## Explicitly not implemented or qualified

The following work cannot be represented honestly by an in-memory Windows
test backend and still blocks a saleable production image:

- a qualified mirrored STT-MRAM PCIe board, real persistence barriers,
  hardware CRC/hash/token validation, and the FPGA “no TX without token”
  or matching per-effect permit assertion;
- ef_vi/TCPDirect/Onload or peer-to-peer DMA drivers, registered-buffer RX/TX,
  queue/core pinning, and a signed X3522 bitstream;
- complete networked and certified Nasdaq ITCH/MoldUDP64/GLIMPSE/OUCH and Cboe
  PITCH/Gap/Spin/BOE sessions, SIP/protected-quote ingestion, drop copy, and
  venue UAT/certification (the checked-in native codecs and recovery message
  seams are deliberately uncertified foundations);
- durable SOBP/FIX and venue session-store provisioning, live session recovery,
  drop-copy reconciliation, and the network supervisors which bind those
  services to the checked-in recovery-only ledgers;
- a mirrored direct-I/O PLP-NVMe segment backend, signed segment roots, and the
  mTLS audit network server (the checked-in Tier-1 sink and checkpoints are
  bounded in-memory/reference implementations; the Tier-0 audit adapter cannot
  advertise 900 seconds at the qualification rate, and no production Tier-1
  source supplies authenticated live capacity/rate evidence);
- the networked FIX gateway, authenticated management API, external AAA,
  production SOBP MAC/KDF and TPM/PKCS#11 secret/signature providers, durable
  configuration/audit database, licensing, support bundles, and the A/B
  update/rollback agent;
- a production `IRiskDispatchCheckpointProvenanceVerifier` backed by a signed
  checkpoint manifest and qualified TPM/HSM trust policy (production recovery
  fails closed without it; CRC/SHA-only self-check is a reference-test mode);
- generated protobuf classes, a `protoc`-backed `IDeviceConfigCodec`, and a
  management RPC adapter (the schema/drift gate exists, but no protobuf runtime
  is linked);
- HA replication transport, witness service, venue-session fencing, and live
  takeover reconciliation;
- the fixed Linux installer/image build, factory provisioning, thermal/power/
  component-failure qualification, venue certification, and regulatory
  operating procedures;
- bare-metal hardware-timestamped latency, sustained/burst load, 72-hour soak,
  destructive power-loss, or exchange-certification evidence. No SLA is
  claimed from the portable tests.

The in-memory recorder deliberately reports
`VolatileSoftwareReference`. A production token gate configured to require
`HardwarePersistent` rejects it, so the reference backend cannot accidentally
enable a production send.

## Build and verification

The dependency-free appliance foundation is enabled by default:

```bash
cmake -S . -B build/appliance \
  -DBUILD_TESTING=ON \
  -DSORX_BUILD_APPLIANCE=ON \
  -DSORX_ENABLE_QUICKFIX=OFF \
  -DSORX_ENABLE_INTEGRATION=OFF
cmake --build build/appliance
ctest --test-dir build/appliance --output-on-failure
```

The five binaries currently provide identity/help and offline configuration
validation only. In particular, `sorxd` is a fail-closed shell, not a composed
binary-ingress/market-data/route/recorder/venue-session supervisor. All five
intentionally refuse runtime activation until real supervisors, authenticated
control services, and qualified hardware/connector backends are wired.

See [HARDWARE_QUALIFICATION.md](HARDWARE_QUALIFICATION.md) for the exact
measurement boundary and go/no-go criteria. The original runtime and journal
remain available as the golden behavioral/recovery model and are not placed on
the proposed appliance hot path.
