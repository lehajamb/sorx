# SORX fixed-image inputs

These files are image-build inputs, not a script that mutates an arbitrary
host. The production image builder renders CPU IDs, PCI identities, hugepage
counts, firmware digests, and service credentials from the signed hardware
profile selected by Phase 0.

The systemd files in the adjacent directory are supervision blueprints. The
currently built process shells validate configuration but deliberately reject
`--config` runtime activation, so an image build must not enable `sorx.target`
until the qualified backends and supervisors replace that fail-closed shell.

The checked-in command line assumes a provisional single-socket 16-physical-
core topology: CPUs 2–13 are isolated and CPUs 0–1/14–15 are housekeeping.
It must not be shipped unchanged on another topology. `sorxd` remains
recovery-only unless `sorx::platform::evaluate_readiness` accepts a complete
fact snapshot against the signed production profile.

Image qualification must additionally verify locked and prefaulted hugepages,
SMT state, performance governor and C-state bounds, IRQ placement, NUMA-local
NIC/recorder/NVMe memory, PTP/PPS lock, secure/measured boot, signed FPGA and
firmware identities, redundant power, and the recorded bare-metal latency and
power-pull evidence. These static files cannot establish any of those facts.
