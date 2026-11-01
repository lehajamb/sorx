# Operations and qualification

Use this page to distinguish the operational contracts available for testing
from the evidence and services required for a production appliance. The
repository contains no qualified hardware image, venue certification,
measured latency SLA or destructive power-loss certificate.

For the embedded library's journal and reconciliation behavior, use
[Persistence and recovery](https://github.com/lehajamb/sorx/wiki/Persistence-and-Recovery).
The appliance foundation has different recorder, readiness and HA boundaries.

## What can be operated today

The five process shells provide help, version and offline DeviceConfig
validation; `sorxctl` also migrates legacy appliance INI. They refuse runtime
activation. Follow
[Device configuration and CLI](https://github.com/lehajamb/sorx/wiki/Device-Configuration-and-CLI)
to exercise those supported commands.

`deploy/linux` and the systemd files are image-build inputs and supervision
blueprints. Do not enable `sorx.target` as a deployment recipe for this
baseline. The provisional CPU layout assumes one socket and 16 physical
cores, isolating CPUs 2–13; a different topology requires its own signed
profile, assignments and qualification.

## Evaluate readiness from evidence

`platform::evaluate_readiness` evaluates a fact snapshot against a signed
profile. An integrator must obtain trustworthy facts and signature evidence;
the pure evaluator does not discover or configure the machine itself.

| Area | Required facts include |
| --- | --- |
| CPU and memory | Topology, NUMA locality, isolation, SMT policy, locked/prefaulted hugepages and memlock. |
| NIC and timing | Firmware identity, queue capacity, kernel bypass, hardware timestamps and PTP lock/offset. |
| Recorder and storage | Hardware persistence, healthy mirrors, capacity, reserved lanes, qualified rate/latency, peer-to-peer operation and PLP NVMe. |
| Boot and power | Secure/measured boot, TPM/PCR policy, signed firmware/bitstream, governor and C-state bounds. |

Production ignores certification relaxation masks and fails on every
readiness finding. A `VolatileSoftwareReference` recorder cannot satisfy
`HardwarePersistent`. Supplying optimistic configuration values is not a
substitute for measured, authenticated facts.

## Observe without blocking the coordinator

The telemetry library publishes complete snapshots for management readers.
Readers copy a stable buffer rather than synchronously calling or walking
coordinator state. Snapshot fields include readiness, routing/kill-switch
state, queue depths/high-water marks, credits, feed gaps/age, risk use,
Tier-0 occupancy, Tier-1 lag, PTP, NIC drops, temperature and latency buckets.

These types and publication primitives do not establish a running metrics
server. Telemetry ports in DeviceConfig are configuration fields; the current
process shells do not open them. Software histograms also do not replace the
external hardware timestamps required for the latency gate.

## Preserve recorder and audit continuity

Stop new/replacement admission before Tier 0 can overwrite data not yet
durable in Tier 1. Keep cancellation and reconciliation reserves available.
In Tier-1-protected mode, audit-client acknowledgements report consumer
progress; they do not block Tier-0 recycling after Tier-1 durability or extend
the retention guarantee.

The reference audit collector rejects skipped, reordered and hash-discontinuous
records. Resume from the sink's highest contiguous sequence/hash and
acknowledge only committed contiguous ranges. A failed continuity check must
be investigated instead of advancing the consumer cursor past the gap.

Audit metadata defaults to unverified retention and zero seconds. A nonzero
claim needs authenticated capacity/rate/generation evidence that remains
consistent with live recorder capacity. The checked-in source accounts only
for Tier 0; it cannot demonstrate the required production Tier-1 window.
The qualified archive source and mTLS audit server remain unimplemented.

## Fence leadership and reconcile before takeover

`ReplicationGate` distinguishes local-durable from dual-durable policy.
It validates authenticated lease state, monotonic leadership/witness tuples,
expiry and the required durability watermarks. Revocation removes authority
and durable watermarks but retains the authenticated anti-replay floor.

`TakeoverGate` progresses through recovery and reconciliation before permitting
new routing. Accounting must be consistent and venue sessions recovered.
Witness transport, replication transport, live session fencing and takeover
supervisors are missing; the gate is not a deployed HA service.

## Qualification gates still to satisfy

The following numbers are acceptance requirements from the qualification
document, not results achieved by the checked-in implementation.

| Gate | Required evidence |
| --- | --- |
| Recorder | Mirrored usable Tier 0 of at least 4 GiB; at least 500 MB/s and one million records/s; p99.999 token completion at most 2 microseconds; qualified DMA/MMIO and destructive power-loss recovery. |
| Retention | At least 900 seconds at the qualified rate: 450 GB at 500 MB/s before filesystem, manifest, Tier-0 and reserve overhead. |
| Latency | External hardware RX last-bit to native TX first-bit p99.99 below 10 microseconds at full feed load; 250,000 commands/s sustained and a 1,000,000/s burst for 100 ms. |
| Bounded operation | A 72-hour full-load soak, no hot-path allocation after READY, stable occupancy, explicit full behavior and cancel progress under saturation. |
| Protocols and HA | Exchange UAT/certification, malformed/gap/reset recovery, drop-copy reconciliation, fencing, partitions, witness loss and takeover during dispatch phases. |
| Appliance image | Fixed-image provisioning, signed update/rollback, secure boot, thermal/power/component faults and support procedures. |

A signed release manifest must bind every gate to the same hardware,
firmware, bitstream, kernel and SORX build. Route-critical changes invalidate
the prior latency certificate until requalification. Shared CI runners supply
functional correctness evidence only.

The remaining implementation work includes MRAM/NIC/FPGA backends, complete
network supervisors, production crypto and checkpoint-provenance providers,
durable management/audit services and the fixed Linux image. Track these
against the implementation boundary rather than treating passing portable
tests as permission to activate production traffic.

## Source baseline

- [Hardware measurement boundary and release-gate matrix](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/HARDWARE_QUALIFICATION.md)
- [Implemented operational contracts and missing services](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/APPLIANCE_IMPLEMENTATION.md)
- [Readiness profile, facts and evaluation interface](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/platform/readiness.hpp)
- [Leadership, durability and takeover gates](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/ha/replication.hpp)
- [Linux image-input and systemd boundary](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/deploy/linux/README.md)
