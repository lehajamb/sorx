# SORX implementation report

Date: 2026-08-15

## Outcome

The C++20 SORX project implements the first-release target architecture as an
embedded, deterministic, multi-venue smart-order router. The authoritative
specification is preserved byte-for-byte in `docs/SORX_TARGET_ARCHITECTURE.md`.
All 65 acceptance-plan checklist items are reconciled in `PLAN.md`.

## Architecture delivered

- Management plane: strict typed INI configuration, exact route-critical
  SHA-256 hashing, authoritative `RouteRib`, immutable `CandidateTable`
  generations, administration, kill switch, and hard risk configuration.
- Control plane: one concrete `MockXVenueRuntime` per configured venue, native
  UDP/GLIMPSE market data, one resolved QuickFIX session, immutable adjacency
  snapshots, bounded callback ingress, and independent cancel/new egress.
- Forwarding plane: synchronous deterministic `SorCore::apply`, one
  coordinator owner for all order/accounting state, exact integer conversions,
  profile-owned durable wire identities, and journal-before-side-effect runtime
  orchestration.
- Recovery: checksummed append-only batches, durable flush, exclusive process
  lock, atomic checkpoints, canonical state replay, QuickFIX FileStore restore,
  conservative child reconciliation, and fail-closed ambiguity.
- Verification: deterministic normalized transcript codec/replay, complete
  crash matrix, allocation/risk properties, native session loopback tests,
  multi-MockX integration, protocol-boundary link/source checks, and an
  installed-package consumer.

## Final command evidence

| Gate | Result |
| --- | --- |
| Fresh dependency-free Debug CTest | 22/22 passed |
| Fresh dependency-free Release CTest | 22/22 passed |
| Fresh full QuickFIX/integration Debug CTest | 26/26 passed |
| Fresh full QuickFIX/integration Release CTest | 26/26 passed |
| Strengthened installed consumer, Debug/Release | Passed; consumes native MD and venue-runtime targets |
| Release multi-MockX demo | 4+6 sweep, FILLED=10, risk=0, conservation PASS |
| MockX `cpp_market_feed` Release CTest | 2/2 passed |
| Unchanged `cpp_sor` native core executable | 5/5 passed |
| Preserved `cpp_sor` FIX executable | 5/5 passed |
| MockX Python `compileall` | Passed |
| MockX `docker compose config --quiet` | Passed |
| Full MockX Python pytest | 601 passed, 1 skipped, 2 pre-existing failures |

The exact portable configure, build, test, demo, MockX regression, and preset
commands are in `README.md` and `PLAN.md`.

## Integrity evidence

- Specification copy: SHA-256
  `B71837CCD0395E787B48233A97BC1A959B4F75BB445A4DBC2BB9C2E334593456`,
  16,076 bytes, 322 lines, identical to the supplied source.
- `mockx/examples/cpp_sor` has no source diff and retains tracked tree
  `88e35d7a4344b88f8890aa5005903c794b903560`.
- The SORX core protocol-boundary CTest passes; no MockX, QuickFIX, ITCH,
  GLIMPSE, Mold, Soup, or socket types are linked into or included by the
  deterministic routing core.
- All compiled targets use C++20 and strict warning-as-error settings. Release
  tests explicitly undefine `NDEBUG` so assertions remain active.

## Genuine limitations and external blockers

- Production exchange certification, credentials, and venue-operated network
  environments are external acceptance work. Nasdaq production profiles are
  intentionally later work per the specification.
- MockX provides no order-status query in this first-release dialect. Only a
  durable `Prepared` child can therefore be reconciled as definitely unsent;
  every other nonterminal child remains unresolved/AMBIGUOUS with its risk
  reservation retained until an operator supplies external reconciliation.
- The dirty sibling MockX worktree has two unrelated Python failures: Docker
  enables ITCH while an old test expects it disabled, and a second `.itch.csv`
  fixture violates a one-sample assertion. The exact reproduced result is
  `601 passed, 1 skipped, 2 failed`.
- A full fresh build of unchanged `mockx/examples/cpp_sor` is blocked by its
  pre-existing `src/config.cpp` use of undeclared `venue_count` at lines 55 and
  96. Its rebuilt core tests and preserved FIX tests both pass 5/5, and its
  source tree is unchanged.
