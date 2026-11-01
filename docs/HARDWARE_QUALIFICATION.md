# SORX hardware qualification gate

The software implementation does not claim that ordinary files, simulated
memory, or a development NIC provide crash-survivable sub-10-microsecond
routing. A hardware profile becomes production-qualified only after the tests
below pass on the exact CPU, firmware, NIC, recorder, storage, kernel, BIOS,
bitstream, and chassis revision that will ship.

The repository currently has a volatile software recorder and portable
transports, not the MRAM/NIC/FPGA dataplane, fixed Linux image or network
supervisors. Its venue codecs are uncertified, its DeviceConfig protobuf has no
generated runtime/`protoc` adapter, and it contains no measured SLA evidence.

## Measurement boundary

The binary-path latency measurement begins at the hardware RX timestamp for
the last bit of a complete, authenticated order command and ends at the
hardware TX timestamp for the first bit of the first native venue order. It
includes parsing, pre-trade risk, book pinning, routing, wire-ID creation,
adaptive-policy evaluation, Tier-0 durable commit, encoding, token/fence
validation, per-frame effect-permit narrowing, and the TX doorbell. Cable
propagation, the client stack, and venue processing are excluded.

At full configured Nasdaq and Cboe feed load, the profile must sustain 250,000
commands per second and a 1,000,000-command-per-second burst lasting 100 ms.
The p99.99 latency must remain below 10 microseconds with no unexplained loss.

## Recorder go/no-go

The Tier-0 recorder must demonstrate all of the following before venue TX can
be enabled:

- mirrored persistent capacity of at least 4 GiB;
- at least 500 MB/s and one million committed records per second;
- p99.999 durable-token completion no greater than 2 microseconds;
- measured peer-to-peer NIC/recorder operation using the production DMA and
  MMIO path, not a host-memory substitute;
- recovery after host reset and cold power removal at every write boundary;
- detection of a missing mirror, torn header, torn payload, torn commit marker,
  sequence discontinuity, CRC mismatch, and authentication-chain mismatch;
- hardware enforcement that a venue frame cannot pass unless its compact
  effect permit matches one exact entry in the retained master token's
  authorization list.

The appliance must stop admitting new and replacement orders before Tier-0
would overwrite data not yet durable in Tier 1. Cancellation and reconciliation
use separately reserved capacity.

Tier 1 must sustain the recorder qualification rate and retain at least 900
seconds of continued recording. At 500 MB/s this is 450 GB before filesystem,
manifest, and reserve overhead. Audit-client acknowledgements are observable
progress only: they neither extend the retention guarantee nor block Tier-0
recycling after Tier-1 durability.

The audit service may publish a nonzero retention value only from authenticated
capacity/rate/generation evidence that remains consistent with the live
recorder. The checked-in adapter deliberately rebinds that evidence and reports
zero when it is absent or exceeds available capacity, but it accounts Tier 0
only. Qualification therefore also requires the production Tier-1 audit source
that exposes the actual rolling window; a configured 900-second scalar is not
evidence.

## Platform reproducibility

Qualification output records the complete hardware and software identity,
including serial numbers, firmware hashes, FPGA bitstream hash, kernel command
line, CPU microcode, BIOS settings, core and IRQ assignment, PTP configuration,
thermal state, compiler version, and binary build ID. A change to any
route-critical item invalidates the latency certificate until the suite is
rerun.

At every boot, the signed qualification facts are supplied to
`sorx::platform::evaluate_readiness`. Production remains recovery-only unless
the complete snapshot matches the signed profile, including recorder
bandwidth/rate/p99.999 latency, peer-to-peer and power-loss qualification.

Shared CI runners remain functional correctness gates only. Latency and
power-loss results are accepted exclusively from the controlled bare-metal
qualification rack and an independent timestamping traffic generator.

## Portable software prerequisites

The portable suite now exercises schema-v3 route and composite risk dispatch,
v2 checkpoints, adaptive evidence, and exact native session/wire ownership
through cancel, replay, checkpoint and restart. It also freezes the composite
v1/v2 migration decoders and v1 checkpoint reader. Those legacy batches use a
replay-only range envelope and cannot produce the compact exact-effect permit
required by the final TX gate. The composite recovery ledger restores
client/account risk and route state together and has no TX interface.

Production composite-checkpoint restore defaults to externally verified
provenance. The repository provides the verifier interface and fail-closed
policy, but not the signed-manifest/TPM/HSM implementation needed to satisfy
that policy. The CRC/SHA-only self-check option is limited to reference tests
and is not qualification evidence.

Management-side tests also cover lease anti-replay across revocation, exact
candidate/revision/hash-bound single-use approvals, verified-logon session-key
establishment, contiguous audit hash chains, and non-relaxable production
readiness findings. `DeviceConfigV2` has a normative 18-message/four-enum/
132-field protobuf mirror with a negative drift gate, but no generated codec
or RPC runtime. Production config admission requires one routing context,
separate management/audit/data interfaces, mTLS/opaque secret references, and
the checked 900-second, 500-MB/s, 4-GiB Tier-0 and Tier-1 storage budgets. These
are software prerequisites only; none substitute for a release-gate result
below.

## Release-gate matrix

No single passing benchmark promotes an image. A signed release manifest must
reference evidence for every gate below, produced from the same BOM, firmware,
bitstream, kernel and SORX build identity.

| Gate | Required production evidence |
| --- | --- |
| Hardware feasibility | The recorder and peer-to-peer measurements above, including destructive power-pull recovery and the hardware assertion that a frame without its exact master-token-derived effect permit cannot reach the venue MAC. Failure blocks the sub-10-microsecond claim; it is not converted into a weaker software-durability claim. |
| Behavioral equivalence | Fixed-seed and generated event corpora run through the golden `SorCore` policy and the bounded kernel. Shared semantics must match ordered allocations, conversions, exclusions, risk deltas, adaptive evidence, wire identities and terminal conservation. Native session/ownership identity and pending-cancel state must remain exact through replay and v2 checkpoint restart. Fast-only quote-age/credit/compliance gates must have explicit expected differences. |
| Latency SLA | External RX/TX hardware timestamps at full qualified Nasdaq/Cboe market-data load, 250,000 commands/s sustained and 1,000,000/s for 100 ms. Binary last-bit-to-first-bit p99.99 must be below 10 microseconds, with separately reported cancel and FIX distributions, zero unexplained loss and no coordinated-omission correction hidden from the report. |
| Recorder integrity | Power removal/crash at every header, payload, mirror and commit-marker boundary; corrupt records; one-bank failure; NVMe stall; rollover; reboot/replay; audit disconnect beyond retention; signed-root verification; and proof that new/replace stops before protected capacity is exhausted while cancel/reconciliation reserves remain usable. |
| Bounded operation | A 72-hour full-load soak with allocation interception after `READY`, stable RSS/table occupancy, explicit bounded-full behavior, no lifetime-history latency trend, safe segment recycling, and demonstrated cancel progress while normal ingress/TX/recorder lanes are saturated. |
| Protocol certification | Line-rate capture replay plus malformed/gap/reorder/rollover/session-reset tests, A/B arbitration and snapshot stitching, drop-copy reconciliation, and the applicable Nasdaq/Cboe UAT/certification artefacts. Checked-in codec unit tests are not certification evidence. |
| Appliance qualification | Secure/measured boot and PCR policy, signed update with A/B rollback, offline provisioning/factory reset, redundant-power and thermal tests, disk/card/NIC/PTP failures, support-bundle redaction, fixed-image/network-supervisor recovery, and validation that readiness remains fail-closed after hotplug or reboot. |
| HA qualification | Process and power loss, recorder loss, asymmetric partition, witness loss, split-brain attempts, replication lag, takeover during every dispatch phase, venue reconciliation before reopening, and rolling upgrade in both `local-durable` and separately measured `dual-durable` modes. |

The repository's CTest suite supplies deterministic, corruption, allocation and
state-machine evidence for the portable software contracts. It deliberately
does not emit a production certificate, and its in-memory recorder, portable
transport and simulated HA peer cannot satisfy any hardware row above.
