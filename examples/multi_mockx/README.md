# Deterministic multi-MockX acceptance demo

This example runs two configured MockX venue profiles entirely in process. It
uses the production `sorx::md::FeedEngine`, `sorx::SorRuntime`, durable journal,
priority egress queues, and `sorx::adapters::mockx::MockXFix44Profile`. No
external service, socket, credential, or wall-clock timing is required.

From the repository root, configure and build:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DSORX_ENABLE_INTEGRATION=ON
cmake --build build --config Release --target sorx_multi_mockx_demo sorx_multi_mockx_tests
```

Run the demo with the checked-in strict configuration:

```powershell
build\Release\sorx_multi_mockx_demo.exe configs\multi_mockx\sorx.ini
```

Run the acceptance and core-boundary gates:

```powershell
ctest --test-dir build -C Release -R "sorx_multi_mockx_tests|sorx_core_protocol_boundary" --output-on-failure
```

The demo publishes independent fresh snapshots, stitches both feeds to `LIVE`,
routes one protected IOC across displayed depth on both venues, constructs the
exact normalized FIX `D` fields, applies local acceptance, acknowledgements,
and fills, then prints a concise deterministic transcript and conservation
result. It returns nonzero if any expected transition or accounting invariant
fails.

The broader `sorx_multi_mockx_tests` executable also covers isolated feed
withdrawal, sequence-gap recovery through a fresh snapshot, FIX session epoch
changes, passive GTC rest/fill/cancel, cancel-then-reroute replacement, normal
egress saturation with independent cancel priority, same-time report ordering,
duplicate execution-report deduplication, journal restart/reconciliation, and
no reroute or risk release for an ambiguous child.

## Deliberate boundary of this demo

This executable is deterministic profile acceptance, not a live network
certification. Its feed input is already normalized and its FIX output is a
field-value transcript. The separate `sorx_mockx_md_session_tests`,
`sorx_quickfix_store_recovery_tests`, and
`sorx_mockx_venue_runtime_tests` cover raw transport framing, socket/session
failure, persistent sequence-store reopen, and concrete per-venue ownership.
Credentials, venue-operated environments, and real exchange certification
remain external acceptance work.


