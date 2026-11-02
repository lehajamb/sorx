# SORX

SORX is a deterministic C++20 smart-order-routing codebase with two explicit
execution tiers: the original embedded `SorCore`/`SorRuntime` reference model,
and a new bounded appliance-path foundation for a fixed-hardware order-router.
The appliance path adds fixed-capacity routing, market-data and TX primitives,
a crash-recording/token gate contract, a binary client protocol, versioned
configuration, audit/HA/readiness boundaries, and separately packaged process
shells.

This repository does **not** yet represent a venue-certified or latency-
qualified physical appliance. The volatile recorder and portable transports
are functional/fault-injection references; production sends reject them when
hardware persistence is required. The exact implemented boundary and remaining
release gates are documented in
[`docs/APPLIANCE_IMPLEMENTATION.md`](docs/APPLIANCE_IMPLEMENTATION.md) and
[`docs/HARDWARE_QUALIFICATION.md`](docs/HARDWARE_QUALIFICATION.md).

The checked-in MockX acceptance path is intentionally in-process and needs no
external service. QuickFIX is optional for the dependency-free build and is
enabled explicitly for the full adapter build.

## Interactive visual guide

Read the [published SORX documentation](https://lehajamb.github.io/sorx/).
GitHub Pages publishes the guide from `doc` whenever it changes on `main`.

Open [`doc/index.html`](doc/index.html) directly in a browser to explore how
SORX splits a 10-share order across venues, combines configured limits with
live liquidity, and accounts for fills, cancellations, and uncertain outcomes.
The guide separates current routing capabilities from broader adaptive SOR
features, with architecture and source details available on demand. The
guide is plain HTML/CSS/JavaScript and requires no build step; Google Fonts
are optional and system fallbacks keep it readable offline.

To serve it from the repository root instead:

```powershell
python -m http.server 8000 --bind 127.0.0.1 --directory .
```

Then open `http://127.0.0.1:8000/doc/`.

## Architecture and contracts

| Plane | Implemented responsibility |
| --- | --- |
| Management | Strict configuration, `RouteRib`, immutable compiled candidate tables, venue administration, kill switch, and hard risk limits. |
| Control | Per-venue feed health and owned `BookView` generations, FIX-session readiness/epochs, and bounded normal and cancel egress classes. |
| Forwarding | `SorCore::apply` as the deterministic state machine and `SorRuntime` as the journaled coordinator, queue owner, and side-effect boundary. |
| Adapters | Neutral `sorx::md::FeedEngine`, native MockX UDP/GLIMPSE market-data session, MockX FIX 4.4 profile, optional QuickFIX session, and one concrete per-venue runtime. Protocol and wire types are prohibited from the core by a test gate. |
| Appliance foundation | Bounded route and client/account risk kernels, fixed L3/A-B feed path, deterministic adaptive policy, schema-v3 one-batch dispatch, per-frame durable-effect-permit-gated TX, restart-safe native child ownership, v2 route/risk checkpoints, authenticated SOBP v1, strict FIX 4.4 translation, native OUCH/BOE codec foundations, DeviceConfig v2, recorder/audit/HA/readiness/telemetry contracts, and process/package shells. The RouteKernel's default construction policy requires adaptive evidence and a registered native wire-identity planner for every candidate venue; reference-generic identity and optional policy are explicit test/simulation choices. |

The principal behavioral contracts are:

- Prices and quantities are exact integers. Every configured venue conversion
  must be exact; floating-point routing arithmetic is not used.
- Aggressive orders are protected IOC orders. Allocation is a deterministic
  price/depth sweep with stable venue tie-breaking, configured venue/count
  caps, and at most one child per selected venue.
- Passive GTC orders must be non-marketable and route to one deterministically
  selected venue. Replace is cancel, terminal accounting, then reroute of a new
  parent revision.
- The coordinator is the sole owner of parent, child, fill, reservation,
  cancel, replace, and recovery state. Every input receives a monotonic core
  ingress sequence.
- A successful `CommandReceipt` means validation, ingress sequencing, and a
  durable journal commit completed. Venue acknowledgements and outcomes remain
  asynchronous.
- Once dispatch can no longer be proved absent, quantity is never routed
  elsewhere. An unresolved child becomes `AMBIGUOUS`; its reservation remains
  held and its parent remains suspended.
- Cancels use the child's recorded venue/session mapping and reserved cancel
  capacity. They do not perform a fresh route lookup and are not blocked by a
  failed market-data feed.
- Decisions pin the exact FIB, risk, adjacency, and book generations used.
  SORX does not claim an atomic cross-venue market snapshot.
- Production construction requires a profile-owned `WireIdentityFactory`. New
  and cancel `ClOrdID` values are generated before durable `ChildPrepared`,
  validated against the route-critical profile/session identity, and reused
  unchanged across retries and recovery.

The original reference design and acceptance contract is in
[`docs/SORX_TARGET_ARCHITECTURE.md`](docs/SORX_TARGET_ARCHITECTURE.md). The
appliance implementation boundary is tracked separately so portable test
backends cannot be mistaken for hardware qualification evidence.

## Build requirements

- CMake 3.20 or newer.
- A C++20 compiler. The project is continuously intended for current MSVC,
  GCC, and Clang.
- Threads, supplied by the platform toolchain.
- QuickFIX C++ only when `SORX_ENABLE_QUICKFIX=ON`.

All SORX targets compile with warnings as errors: `/W4 /WX /permissive-` on
MSVC and `-Wall -Wextra -Wpedantic -Werror` elsewhere.

### Dependency-free build

This builds the core, configuration, journal, runtime, neutral feed engine,
MockX profile, and their tests without QuickFIX or external MockX processes.

On Windows with Visual Studio:

```powershell
cmake -S . -B build/dependency-free -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=OFF -DSORX_ENABLE_INTEGRATION=OFF
cmake --build build/dependency-free --config Release --parallel
ctest --test-dir build/dependency-free -C Release --output-on-failure
```

On a single-config Linux toolchain:

```bash
cmake -S . -B build/dependency-free -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=OFF -DSORX_ENABLE_INTEGRATION=OFF
cmake --build build/dependency-free --parallel
ctest --test-dir build/dependency-free --output-on-failure
```

The in-process multi-venue integration suite is still dependency-free. Enable
it with the same commands plus `-DSORX_ENABLE_INTEGRATION=ON`.

`SORX_BUILD_APPLIANCE=ON` is the default and additionally builds `sorxd`,
`sorx-fixgw`, `sorx-auditd`, `sorx-mgmtd`, and `sorxctl`. Their current safe
shell supports offline validation of the DeviceConfig v2 YAML import/export
format:

```powershell
build\dependency-free\Release\sorxctl.exe --check-config configs\appliance\sorx-device-v2.yaml
```

`sorxctl` also supplies the explicit one-time migration path. It validates the
complete legacy appliance INI document, round-trips the resulting YAML and its
route-critical hash, and refuses to replace an existing output file:

```powershell
build\dependency-free\Release\sorxctl.exe --migrate-ini legacy-device.ini device-v2.yaml
```

The C++ `DeviceConfigV2` object graph is mirrored by the normative
`schemas/device_config_v2.proto` schema. A negative-drift test fails if their
messages, enums, fields, ordering, repetition or scalar mappings diverge. The
current contract contains 18 messages, four enums, and 132 fields:

```powershell
python tests\check_device_config_schema_contract.py --self-test
```

No generated protobuf runtime, `protoc` adapter or management RPC is linked
yet. Runtime activation is also intentionally unavailable until the mirrored
MRAM recorder, accelerated NIC/FPGA path, fixed Linux image, network
supervisors and venue-certified sessions are implemented and qualified. The
repository contains no measured latency or power-loss certificate.

Production `DeviceConfigV2` validation is deliberately narrower than the
general schema. It requires exactly one routing context, management/audit/data
interfaces that do not overlap, a hardware-declared dedicated audit interface,
management and audit mTLS with opaque TPM/TPM2/PKCS#11 references, at least 900
seconds of retention at a qualified rate of at least 500,000,000 bytes/second,
at least 4 GiB of usable Tier-0 capacity, nonzero cancel/reconciliation/normal
lanes, and storage of at least `retention * qualified_rate + Tier0`. These are
configuration admission checks, not evidence that a backend meets the stated
hardware guarantees.

Audit metadata does not copy a configured retention scalar. The reference
source defaults to unverified retention and reports zero seconds; a nonzero
claim requires authenticated capacity/rate/generation evidence and is rebound
to live recorder capacity before publication. The checked-in source accounts
only Tier-0 slots, so a production source that exposes the qualified Tier-1
retention window is still required for the 900-second product guarantee.

The current route-only and composite dispatch records are schema v3 and their
current checkpoints are v2. Composite recovery additionally accepts checkpoint
v1 and frozen dispatch schemas v1/v2 for migration. Those legacy records use
the replay-only `LegacyRangeV0` authorization envelope: they can reconstruct
state but can never mint a TX permit. Production composite checkpoint restore
defaults to `RequireExternalProvenance`; the checked-in CRC/SHA self-check mode
is an explicit reference-test option and no production TPM/signature verifier
is supplied here.

### Full QuickFIX and integration build

Install QuickFIX into any conventional prefix containing `include/quickfix`
and its library directory, then expose that prefix through the portable
`SORX_QUICKFIX_PREFIX` environment convention and CMake's standard
`CMAKE_PREFIX_PATH`. No developer-specific path is required.

PowerShell/MSVC:

```powershell
$env:SORX_QUICKFIX_PREFIX = 'C:\path\to\quickfix-prefix'
cmake -S . -B build/full-acceptance -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=ON -DSORX_ENABLE_INTEGRATION=ON "-DCMAKE_PREFIX_PATH=$env:SORX_QUICKFIX_PREFIX"
cmake --build build/full-acceptance --config Release --parallel
ctest --test-dir build/full-acceptance -C Release --output-on-failure
```

POSIX shell:

```bash
export SORX_QUICKFIX_PREFIX=/path/to/quickfix-prefix
cmake -S . -B build/full-acceptance -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=ON -DSORX_ENABLE_INTEGRATION=ON -DCMAKE_PREFIX_PATH="$SORX_QUICKFIX_PREFIX"
cmake --build build/full-acceptance --parallel
ctest --test-dir build/full-acceptance --output-on-failure
```

`FindQuickFIX.cmake` also accepts `QUICKFIX_ROOT` as a fallback. A vcpkg
toolchain is supported but is not the only discovery method. For example, add
`-DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"` to the
configure command after installing the `quickfix` port.

### MSVC presets

The checked-in presets use Visual Studio 18 2026 and x64. The full preset maps
`SORX_QUICKFIX_PREFIX` to both `CMAKE_PREFIX_PATH` and the module fallback
`QUICKFIX_ROOT`.

```powershell
cmake --preset msvc-dependency-free
cmake --build --preset msvc-dependency-free-build
ctest --preset msvc-dependency-free-test

$env:SORX_QUICKFIX_PREFIX = 'C:\path\to\quickfix-prefix'
cmake --preset msvc-full-acceptance
cmake --build --preset msvc-full-acceptance-build
ctest --preset msvc-full-acceptance-test
```

## Strict configuration

`ConfigurationLoader` accepts schema version 1 INI input with exactly four
case-sensitive section forms: `[sorx]`, `[instrument <id>]`, `[venue <id>]`,
and `[route <instrument-id> <venue-id>]`. Unknown sections or fields,
duplicates, missing required fields, invalid identities/references, overflows,
and inexact conversion ratios are errors; parsing never silently substitutes a
route-critical value. Whole-line comments begin with `#` or `;`.

The validated configuration produces a canonical SHA-256 route-critical hash.
It covers schema/account identity, instruments and risk limits, venue/session
and dictionary identities, capacities, routes, exact conversion ratios,
capabilities, limits, and deterministic priorities. Credential references are
operational locators and are deliberately excluded. The journal stores the
hash, schema, and account context and refuses incompatible recovery.

[`configs/multi_mockx/sorx.ini`](configs/multi_mockx/sorx.ini) is the complete
strict two-venue example used by acceptance tests and the demo.

## Deterministic multi-MockX demo

Build with `SORX_ENABLE_INTEGRATION=ON`, then run:

```powershell
build\full-acceptance\Release\sorx_multi_mockx_demo.exe configs\multi_mockx\sorx.ini
```

For a dependency-free integration build, substitute that build directory. The
expected transcript is:

```text
FEED venue=1 snapshot=FRESH state=LIVE ask=100x4
FEED venue=2 snapshot=FRESH state=LIVE ask=101x6
COMMAND parent=1 intent=IOC qty=10 limit=101 accepted
DECISION parent=1 venue=1 qty=4 limit=100
FIX venue=1 35=D|11=SXN1-1-1|55=ALPHA-A|54=1|38=4|40=2|44=1.00|59=3
DECISION parent=1 venue=2 qty=6 limit=101
FIX venue=2 35=D|11=SXN1-1-2|55=ALPHA-B|54=1|38=6|40=2|44=1.01|59=3
OUTCOME parent=1 state=FILLED filled=10 children=2 risk=0
CHECK deterministic_conservation=PASS
```

The executable returns nonzero if the transition sequence or conservation
check differs. It intentionally uses normalized in-process events so its
transcript is deterministic. Native socket/session behavior is covered by
`sorx_mockx_md_session_tests`, `sorx_quickfix_store_recovery_tests`, and
`sorx_mockx_venue_runtime_tests`; production venue certification remains
external environment work.

## Production venue sessions

`MockXVenueRuntime` is the concrete first-release owner instantiated once per
configured venue. It composes the native UDP/GLIMPSE market-data session and a
single resolved QuickFIX session, publishes immutable adjacency snapshots,
bridges callbacks through bounded queues, drains cancel/risk-off egress before
new traffic, and keeps `SorRuntime` recovery-only until feed, session, and
accounting readiness agree. Market-data callback loss withdraws health
immediately. Because MockX has no order-status query in this release, recovery
classifies only a durable `Prepared` child as definitely unsent; every other
nonterminal child remains unresolved with its reservation held until external
reconciliation.

The real QuickFIX store test persists sender/target sequence numbers and a
message through `FileStoreFactory`, destroys and reopens the store, then proves
the production adapter observes the restored values before logon. Missing or
unusable `FileStorePath` settings fail closed.

## Persistence and recovery semantics

Journal and checkpoint bytes use explicit canonical big-endian encoding, not
raw C++ object representations. The journal has a checksummed header and
checksummed logical batches with monotonic batch and record IDs. A batch is
visible only after its commit marker and an OS durable flush. Replay validates
complete batches, rejects incompatible headers or corruption in committed
history, and ignores an incomplete torn tail. A process-wide exclusive lock
prevents two owners of the same journal. Checkpoints use checksummed temporary
write, durable flush, and atomic replacement.

Startup acquires the journal lock, validates the route-critical hash, replays
durable state and ID/session counters, restores session stores, and remains in
recovery mode. Every nonterminal child must be reconciled. Definite terminal
outcomes are accounted normally; unresolved ownership becomes `AMBIGUOUS` and
retains risk. New routing is enabled only after reconciliation is complete and
feed/accounting gates are consistent. A route-critical change with outstanding
children fails startup instead of remapping them.

## MockX facade regression and preserved baselines

SORX is a sibling of MockX. Set a portable root variable before running the
facade regression from the MockX repository:

```powershell
$env:MOCKX_ROOT = (Resolve-Path '..\mockx')
cmake -S "$env:MOCKX_ROOT\examples\cpp_market_feed" -B "$env:MOCKX_ROOT\examples\cpp_market_feed\build-sorx" -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build "$env:MOCKX_ROOT\examples\cpp_market_feed\build-sorx" --config Release --parallel
ctest --test-dir "$env:MOCKX_ROOT\examples\cpp_market_feed\build-sorx" -C Release --output-on-failure
```

The neutral feed extraction preserves `mockx_feed_core` as the compatibility
facade. `examples/cpp_sor` source was not changed. These commands prove that
directly and record the tracked tree identity:

```powershell
git -C "$env:MOCKX_ROOT" diff --name-only -- examples/cpp_sor
git -C "$env:MOCKX_ROOT" rev-parse HEAD:examples/cpp_sor
```

The expected diff output is empty; the discovery tree was
`88e35d7a4344b88f8890aa5005903c794b903560`.

Three known MockX baseline issues are separate from SORX:

1. A fresh build of unchanged `examples/cpp_sor/src/config.cpp` currently
   fails MSVC C2065 at lines 55 and 96 because `venue_count` is undeclared
   (with a non-fatal `getenv` deprecation warning at line 69). Reproduce it
   with:

   ```powershell
   cmake -S "$env:MOCKX_ROOT\examples\cpp_sor" -B "$env:MOCKX_ROOT\examples\cpp_sor\build-sorx" -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON "-DCMAKE_PREFIX_PATH=$env:SORX_QUICKFIX_PREFIX"
   cmake --build "$env:MOCKX_ROOT\examples\cpp_sor\build-sorx" --config Release --parallel
   ```

   The freshly rebuilt `mockx_sor_core_tests` still passes 5/5, and the
   pre-existing `build-vcpkg/Release/mockx_sor_fix_tests.exe` passes 5/5.
2. The dirty MockX Docker configuration enables ITCH while an existing Python
   test expects it to be disabled.
3. A newly present `.itch.csv` fixture makes an existing sample-discovery test
   find two samples where it expects one.

Consequently the recorded full MockX Python baseline is `601 passed, 1
skipped, 2 failed`; those two failures predate and are outside the SORX change.
Re-run it from the MockX root after activating the repository's declared
Python environment:

```powershell
python -m compileall -q src tests examples
docker compose config --quiet
python -m pytest -q
```

The SORX CI workflow always starts from a clean checkout and separately gates
dependency-free and integration builds on Linux and Windows, plus the Linux
QuickFIX adapter against Ubuntu's packaged QuickFIX development library.
