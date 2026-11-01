# SORX implementation plan

This plan is the execution ledger for docs/SORX_TARGET_ARCHITECTURE.md. The
architecture document is the authoritative technical specification. Checkboxes
are completed only when the named evidence exists and has been rerun.

## 1. Repository and baseline record

- Primary repository: C:\Users\lehaj\dev\mockx_rebirth_july_2026\sorx
- Secondary compatibility repository:
  C:\Users\lehaj\dev\mockx_rebirth_july_2026\mockx
- Specification copy: docs/SORX_TARGET_ARCHITECTURE.md
- The specification copy was compared line by line with the supplied source:
  322 lines in each file and identical content.
- Toolchain observed: CMake 4.4.2, Visual Studio 18 2026, C++20.
- Vcpkg toolchain:
  C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/vcpkg/scripts/buildsystems/vcpkg.cmake
- MockX branch at discovery: feature/coinapi_codex.
- MockX HEAD at discovery: eecebc0a.
- Existing MockX user work is intentionally preserved. Discovery found 23
  staged paths and 556,204 inserted lines outside the C++ feed/SOR work.
- examples/cpp_sor was clean at discovery. Its HEAD tree was
  88e35d7a4344b88f8890aa5005903c794b903560 and the tracked working manifest
  SHA-256 was fb2e6c0fef7e3d1408f3ee4efc1c7037252c5f8d61bbb96d88ff06a70d73c4d2.
- Existing native baseline:
  - mockx_feed_tests: 6 of 6 groups passed.
  - mockx_sor_core_tests: 5 of 5 cases passed.
  - mockx_sor_fix_tests: 5 of 5 cases passed.
- Existing full Python baseline under the MockX Conda environment:
  601 passed, 1 skipped, 2 failed.
  - The dirty Docker configuration enables ITCH while one existing test
    expects it disabled.
  - A new .itch.csv file makes a repository sample-discovery test find two
    CSV files rather than one.
  These failures predate SORX changes and their files will not be overwritten.

## 2. Locked implementation decisions

- SORX is an embedded C++20 library with typed northbound and administrative
  APIs. It has no HTTP service, CLI management shell, route discovery, LPM,
  multi-hop routing, or internal microservices.
- The deterministic core is standard-library-only. It links no sockets,
  threads, QuickFIX, MockX, Mold, Soup, GLIMPSE, ITCH, or other wire types.
- All prices, quantities, conversions, limits, and notionals use checked
  integer arithmetic. Configuration rejects inexact conversions and overflow.
- One OrderCoordinator is the sole owner of mutable order, fill, risk, ID,
  cancel, replace, ambiguity, and recovery state.
- SorCore::apply(event) is synchronous and deterministic. Runtime code supplies
  clocks, threads, bounded queues, persistence, profiles, and networking.
- RouteRib is the sole configured route inventory. FibCompiler publishes
  immutable CandidateTable generations only for configuration, administrative,
  or instrument-directory changes.
- Transient feed, adjacency, and egress state never rebuilds the FIB.
- Cross-thread market data is an owned fixed-capacity sorx::md::BookView.
  Borrowed books are confined to the legacy MockX compatibility facade.
- A neutral sorx_md_core component is extracted beneath
  mockx/examples/cpp_market_feed. The target name mockx_feed_core, public
  mockx_feed include paths, namespace, and legacy APIs remain compatible.
- SORX production code consumes only sorx::md market-data types.
- MockX ITCH/GLIMPSE and MockX FIX 4.4 are the first protocol profiles. Wire
  types remain in adapters. QuickFIX support is optional for the dependency-free
  build and required by the full acceptance configuration.
- Each configured venue has immutable versioned adjacency and two bounded
  egress classes. Cancel and risk-off capacity is independent from new and
  replacement traffic. Drops are always explicit.
- Accepted public commands receive an ingress sequence and durable journal
  record before CommandReceipt success. Success never implies routing, dispatch,
  or acknowledgement.
- All children, logical and wire IDs, risk reservations, and ChildPrepared
  records for one route decision are committed in one batch before dispatch.
- The journal uses canonical field encoding, checksums, monotonic record IDs,
  batch commits, an exclusive process lock, and checksummed atomic checkpoints.
- Startup is single-active recovery: lock, validate route-critical hash, replay,
  restore IDs/risk/session epochs, restore order-entry sequence stores, start
  recovery-only, reconcile every nonterminal child, then enable routing only
  when market data and accounting are consistent.
- There is no active/standby HA, execution algorithm, broad portfolio risk,
  automatic liquidity chasing, passive rebalancing, or initial native replace.

## 3. Target layout and ownership

### Primary SORX repository

- CMakeLists.txt and CMakePresets.json:
  dependency-free and full-acceptance build graphs.
- include/sorx:
  public API, strong IDs, exact values, reports, inspection, configuration,
  normalized events/effects, management state, market-data values, and adapter
  interfaces.
- src/core:
  routing policy, risk, coordinator, invariant checking, and SorCore.
- src/management:
  RouteRib, FibCompiler, immutable CandidateTable, and operational views.
- src/persistence:
  canonical journal codec, CRC, journal batches, process locking, checkpoints,
  and replay.
- src/runtime:
  SorRuntime, venue runtimes, coordinator loop, bounded queues, reserved
  priority egress, recovery, and deterministic transcripts.
- src/adapters/mockx:
  MockX market-data profile/recovery and MockX FIX 4.4 profile. Protocol headers
  and QuickFIX objects cannot cross this boundary.
- configs and examples/multi_mockx:
  strict multi-venue configuration and an end-to-end demonstration.
- tests:
  dependency-free unit/property/crash tests, protocol conformance tests,
  transcript replay, and multi-MockX integration tests.

### Secondary MockX repository

- Only mockx/examples/cpp_market_feed may be changed for the neutral extraction.
- The existing mockx_feed_core target and public compatibility facade remain.
- mockx/examples/cpp_sor must remain byte-for-byte unchanged.
- Existing ring, wire decoder, L3 book, snapshot stitch, continuity, CLI, and
  live-smoke tests remain regression gates.

## 4. Milestones

### M0 - discovery, specification, and baseline

- [x] Read the goal objective before continuing.
- [x] Read the supplied architecture document completely.
- [x] Copy the architecture document into docs without changing its contents.
- [x] Read MockX AGENTS.md and the checked-in graft graph references.
- [x] Read the relevant MockX market-data, GLIMPSE, Mold/Soup, FIX, scheduler,
  cpp_market_feed, cpp_sor, test, and build sources before implementation.
- [x] Record the existing native and full-Python baselines.
- [x] Record the dirty-worktree boundary and cpp_sor integrity hashes.
- [x] Reconfirm repository status immediately before the first edit in MockX.

### M1 - scaffold, exact values, configuration, RIB, and FIB

- [x] Add warning-clean C++20 CMake targets and dependency-free presets.
- [x] Implement protocol-neutral strong IDs and checked exact arithmetic.
- [x] Implement strict typed configuration and canonical route-critical hash.
- [x] Implement RouteRib validation and immutable CandidateTable compilation.
- [x] Model the fixed admin, MD, OE, egress, and instrument state dimensions.
- [x] Test overflow, exact conversion, duplicate IDs/symbols, invalid limits,
  stable hashes, FIB generation, and the prohibition on liveness recompiles.

### M2 - neutral market data and MockX compatibility

- [x] Add owned fixed-capacity BookView with all required identity, depth,
  epoch, sequence, generation, timestamp, state, and truncation fields.
- [x] Separate deterministic FeedEngine from sockets and threads.
- [x] Preserve direct UDP to bounded SPSC storage, strict framing, single-writer
  L3, integer prices, GLIMPSE/live stitching, and fail-closed continuity.
- [x] Make recovery a profile strategy; MockX uses a fresh GLIMPSE snapshot.
- [x] Validate-and-ignore only catalogued neutral ITCH messages. Withdraw health
  on unknown types, wrong lengths, ring loss, malformed framing, or gaps.
- [x] Publish a non-LIVE generation before any later view after invalidation.
- [x] Preserve the mockx_feed_core target and all cpp_sor-facing APIs.
- [x] Prove SORX headers depend on sorx::md and not mockx::feed.
- [x] Rerun all existing cpp_market_feed and cpp_sor native tests.

### M3 - pure policies, risk, coordinator, and lifecycle

- [x] Implement hard risk checks and durable reservation accounting.
- [x] Implement AggressiveIOC k-way merge using canonical price, configured
  venue priority, then stable venue ID.
- [x] Enforce parent limit, displayed depth, caps, max venue count, parent
  quantity, one child per venue, and each venue's worst allocated level.
- [x] Continue levels only on already selected venues after the venue maximum.
- [x] Treat unseen truncated depth as unknown and record explicit remainder.
- [x] Pin FIB, adjacency, risk, and every book version. Permit one pre-persist
  replan and abort cleanly on a second version change.
- [x] Implement PassiveGTC as a full-quantity, exactly-one-venue,
  non-marketable placement ranked by passive priority, configured latency, then
  stable venue ID.
- [x] Implement direct child cancellation without FIB or feed eligibility.
- [x] Implement cancel-then-terminal-then-reroute parent replacement, including
  fills racing with cancel and new-total-quantity semantics.
- [x] Implement protocol-neutral logical IDs and durable wire-ID mapping.
- [x] Deduplicate normalized execution reports by venue, session epoch, ExecID.
- [x] Implement definite reject, definitely-unsent retry, local acceptance,
  timeout/disconnect ambiguity, suspension, and no post-dispatch reallocation.
- [x] Test allocation and risk conservation with duplicate/reordered reports,
  partial fills, cancel/fill races, reject, ambiguity, kill switch, and shutdown.

### M4 - journal, replay, checkpoints, and recovery

- [x] Implement append-only checksummed canonical journal batches.
- [x] Flush commits before successful receipts or external effects.
- [x] Implement exclusive process locking on Windows and POSIX.
- [x] Implement periodic checksummed checkpoint temp-write, flush, and atomic
  replace without release-one journal compaction.
- [x] Journal commands, decisions/exclusions, risk, IDs, ChildPrepared,
  dispatch state, execution reports/outcomes, terminal state, and admin events.
- [x] Replay complete batches identically, ignore a torn trailing batch, and
  fail closed on middle corruption or incompatible schema/config.
- [x] Implement the mandated startup order and nonterminal reconciliation.
- [x] Cover the complete crash matrix and repeated replay without duplicate IDs.

### M5 - runtime, queues, profiles, and operations

- [x] Add one VenueRuntime per configured venue.
- [x] Add bounded ingress and per-venue egress with independent cancel reserve.
- [x] Drain cancel/risk-off before new/replacement and make all saturation
  outcomes explicit.
- [x] Implement immutable adjacency publication and computed operational views.
- [x] Ensure management mutations are ordered journaled coordinator events.
- [x] Implement SorRuntime submit, cancel, replace, trace, inspect, report sink,
  admin state, kill switch, startup, and shutdown behavior.
- [x] Implement MockXFix44Profile outside the core with session-level profile
  dispatch and persistent venue-valid ClOrdID generation.
- [x] Normalize FIX execution/cancel-reject messages without leaking QuickFIX.
- [x] Verify feed failure never blocks cancellation.

### M6 - deterministic transcript and multi-MockX acceptance

- [x] Record explicitly ordered normalized network transcripts.
- [x] Replay transcripts byte-for-byte for decisions, accounting, reports, and
  final state.
- [x] Add an N-venue MockX configuration and documented demo command.
- [x] Test venue withdrawal isolation, ITCH gap/fresh recovery, FIX disconnect
  and reconnect, passive rest/fill/cancel/replace, egress saturation/cancel
  priority, same-time events, duplicate reports, and venue restart.

### M7 - regression, boundary gates, and handoff

- [x] Scan and link-check that the SORX core contains no MockX, QuickFIX, Mold,
  Soup, GLIMPSE, ITCH, FIX wire, networking, or thread dependencies.
- [x] Rerun the SORX test suite from a clean build.
- [x] Rerun MockX cpp_market_feed tests.
- [x] Rerun MockX cpp_sor tests and prove its tree/manifest is unchanged.
- [x] Rerun MockX compileall, Docker Compose validation, and full Python tests.
- [x] Classify any preserved pre-existing regression separately with evidence.
- [x] Document clean-checkout configure, build, test, and demo commands.
- [x] Reconcile every checkbox and acceptance row with final evidence.

## 5. Acceptance evidence matrix

| Specification area | Required evidence |
| --- | --- |
| Three-plane ownership | Type/dependency boundary tests plus coordinator-only mutation tests |
| RIB/FIB semantics | Validation, immutable generation, and no-liveness-recompile tests |
| Fixed state dimensions | Compile-time/public enum checks and eligibility matrix tests |
| Aggressive IOC | Sweep, deterministic tie, caps, limits, truncation, max-venue, one-child-per-venue, and replan tests |
| Passive GTC | Non-marketable, exact full quantity, single venue, deterministic ranking, and no-chasing tests |
| Cancel/replace | Direct recorded-child cancel and cancel-terminal-reroute race tests |
| Risk | Guardrail matrix, checked notional, reservation, release, and conservation properties |
| Ambiguity | Timeout/disconnect suspension, retained risk, and no-reallocation tests |
| Execution reports | Venue/epoch/ExecID dedupe, reorder, cancel/fill, reject, and conservation tests |
| Owned market data | Fixed-capacity value/lifetime tests and generation pinning |
| Feed continuity | Ring loss, gap, malformed/unknown, GLIMPSE stitch, and immediate health-withdraw tests |
| Egress priority | Saturation, reserved cancel capacity, ordered draining, and explicit-drop tests |
| Persistence | Checksums, flush boundary, lock, checkpoint, replay, corruption, and crash matrix |
| Startup recovery | Fixed startup-order, reconciliation, ambiguity, config-hash, and enable-gate tests |
| Determinism | Explicitly ordered core tests and byte-identical transcript replay |
| MockX FIX 4.4 | D/F construction, 8/9 normalization, exact values, durable IDs, and reconnect tests |
| Multi-MockX | N venues, isolated withdrawal, gap/recovery, FIX lifecycle, saturation, duplicates, restart |
| Regression | Existing feed and cpp_sor suites, unchanged cpp_sor hash, Python suite classification |
| Protocol boundary | Source/include scan and target dependency/link graph checks |

## 6. Conservative resolutions for specification gaps

- Aggressive insufficient liquidity allocates only eligible displayed quantity
  and records an explicit remainder; it never infers truncated depth or chases.
- A second pinned-generation change before ChildPrepared ends the decision with
  no dispatch.
- Passive marketability is checked against every eligible decision-snapshot
  venue. A passive parent is never silently resized.
- Multi-child aggressive preparation, IDs, and reservations are one durable
  batch and dispatch follows deterministic allocation order.
- Replacement recomputes remaining quantity after old children become terminal.
  It creates no child when filled quantity equals or exceeds the requested new
  total and reports the race explicitly.
- A duplicate parent ID is idempotent only when its canonical command is exactly
  identical; conflicting reuse is rejected.
- Kill switch and venue shutdown block new/replacement routing and retain the
  cancel path. Explicit risk-off cancellation is an administrative operation;
  feed failure alone never prevents cancel attempts.
- Ambiguous children retain their leaves and reservation. Unrelated routing may
  resume only when aggregate accounting is provably consistent with ambiguity.
- Route-critical hashing includes account, identities, mappings, exact
  conversions, capabilities, priorities, risk/route limits, profile/dictionary
  identity, session identity, and queue-capacity semantics.
- Serialization uses stable ordering and canonical field encoding. Pointers,
  unordered iteration, random IDs, and processing durations never enter
  deterministic records.
- Wide integer operations are checked explicitly and exact conversions reject
  nonzero remainder.
- Stable FIB/book handles retain immutable generation ownership and never dangle.

## 7. Verification commands

The final README will keep these commands current and executable.

Configure and build the dependency-free SORX suite:

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

Configure SORX with the sibling MockX source and full profiles:

    cmake -S . -B build-acceptance -G "Visual Studio 18 2026" -A x64 -DSORX_MOCKX_ROOT=C:/Users/lehaj/dev/mockx_rebirth_july_2026/mockx -DSORX_ENABLE_QUICKFIX=ON -DSORX_ENABLE_INTEGRATION=ON
    cmake --build build-acceptance --config Release
    ctest --test-dir build-acceptance -C Release --output-on-failure

Run the multi-venue demonstration:

    build-acceptance/Release/sorx_multi_mockx_demo.exe configs/multi_mockx/sorx.ini

Rebuild and test the MockX feed facade:

    cmake -S examples/cpp_market_feed -B examples/cpp_market_feed/build-sorx -G "Visual Studio 18 2026" -A x64
    cmake --build examples/cpp_market_feed/build-sorx --config Release
    ctest --test-dir examples/cpp_market_feed/build-sorx -C Release --output-on-failure

Rebuild and test unchanged cpp_sor:

    cmake -S examples/cpp_sor -B examples/cpp_sor/build-sorx -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE="C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
    cmake --build examples/cpp_sor/build-sorx --config Release
    ctest --test-dir examples/cpp_sor/build-sorx -C Release --output-on-failure

Run MockX repository gates from its activated Conda environment:

    python -m compileall -q src tests examples
    docker compose config
    python -m pytest -q

## 8. Final verified evidence (2026-08-15)

- The copied specification is byte-for-byte identical to the attachment:
  SHA-256 `B71837CCD0395E787B48233A97BC1A959B4F75BB445A4DBC2BB9C2E334593456`,
  16,076 bytes, 322 lines.
- A fresh dependency-free Visual Studio build compiled warning-clean and passed
  22/22 CTests in Debug and 22/22 in Release. Release tests explicitly undefine
  `NDEBUG`.
- A fresh full QuickFIX/integration build compiled warning-clean and passed
  26/26 CTests in Debug and 26/26 in Release, including native UDP/GLIMPSE,
  real QuickFIX FileStore reopen, per-venue orchestration, crash matrix,
  deterministic transcript, multi-MockX, protocol boundary, and clean installed
  consumer.
- The Release demo emitted the documented two-venue 4+6 sweep with
  `SXN1-1-1` and `SXN1-1-2`, finished FILLED=10/risk=0, and printed
  `CHECK deterministic_conservation=PASS`.
- Standalone MockX `cpp_market_feed` Release CTest passed 2/2. The
  `examples/cpp_sor` diff is empty and its tracked tree remains
  `88e35d7a4344b88f8890aa5005903c794b903560`; its freshly built core and
  preserved FIX executables each pass 5/5.
- MockX `compileall` and `docker compose config --quiet` pass. Its full
  Python suite reproduces exactly `601 passed, 1 skipped, 2 failed`: the dirty
  Docker ITCH enablement conflicts with an old expectation, and a second
  `.itch.csv` fixture conflicts with a one-sample assertion.
- The unchanged `cpp_sor` full source build still stops at pre-existing
  `src/config.cpp` C2065 errors for undeclared `venue_count` (lines 55 and
  96). This is separately documented and does not affect the passing facade,
  core, or preserved FIX regression evidence.
- Genuine external work is limited to production venue certification and
  operator reconciliation of nonterminal MockX children for which this
  first-release dialect has no order-status query. Such children fail closed as
  unresolved/AMBIGUOUS with reservation retained.

The final report must include actual command outcomes, remaining environmental
limitations, and the cpp_sor integrity proof. It must not claim completion from
unchecked boxes or inferred results.


