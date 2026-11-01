# Testing and Troubleshooting

Validate the execution tier you are changing. Embedded tests establish deterministic behavior and durable ownership; appliance tests establish bounded software contracts and fail-closed reference behavior. Neither replaces venue certification or hardware qualification.

## Run the portable software checks

After [Getting Started](https://github.com/lehajamb/sorx/wiki/Getting-Started):

```powershell
ctest --test-dir build-wiki -C Release --output-on-failure
.\build-wiki\Release\sorx_multi_mockx_demo.exe configs\multi_mockx\sorx.ini
.\build-wiki\Release\sorxctl.exe --check-config configs\appliance\sorx-device-v2.yaml
python tests\check_device_config_schema_contract.py --self-test
node doc/.interaction-smoke.cjs
```

Run the Node command from the repository root. It checks routing presets, lifecycle paths, feed recovery, crash points, keyboard plane navigation, reset cancellation and recovery outcomes. It is separate from CTest. The schema checker compares C++ and protobuf definitions and runs negative-drift checks; generated protobuf runtime integration is not present.

For a focused rerun, CTest accepts a name regex:

```powershell
ctest --test-dir build-wiki -C Release -R 'sorx_multi_mockx_tests|sorx_core_protocol_boundary|sorx_install_consumer_smoke' --output-on-failure
```

Use the complete suite after behavior changes. `ctest -N --test-dir build-wiki -C Release` lists registered tests; missing Python or disabled optional targets can change the count. Build failures are not test passes, and a zero-test selection is not coverage.

## Evidence for the pinned source

The initial [GitHub CI run](https://github.com/lehajamb/sorx/actions/runs/41337726992) exercised source `3234de9`:

| Configuration | Result |
|---|---|
| Windows, dependency-free | 46 tests passed |
| Windows, in-process integration | 47 tests passed |
| Linux, dependency-free | Build failed before CTest |
| Linux, in-process integration | Build failed before CTest |
| Linux, QuickFIX + integration | Build failed before CTest |

The shared first Linux error is at [session_auth.cpp:258](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/protocol/session_auth.cpp#L258): partial `AuthVerifyResult` aggregate initialization triggers GCC's missing-field-initializers warning under `-Werror`. Disabling appliance executables does not remove that library. Preserve this caveat until a later source revision has passing evidence.

The August 15 implementation report's 22/26-test counts describe earlier work. Do not substitute those numbers for current CI. Wiki publication validation is recorded in its documentation PR; configuration-validation success remains distinct from operational readiness.

## Coverage map

| Contract | Relevant tests or evidence |
|---|---|
| Exact allocation, passive selection and exclusions | `routing_tests`, `allocation_property_tests`, `venue_conversion_tests` |
| Parent/child ownership, replacement and conservation | `order_core_tests`, `dispatch_lifecycle_tests`, `risk_conservation_tests` |
| Journal, restart, dedupe and ambiguous outcomes | `journal_tests`, `crash_matrix_tests`, `recovery_state_tests`, `wire_identity_tests` |
| Queue ownership and report delivery | `runtime_tests`, `runtime_queue_tests`, `runtime_concurrency_tests` |
| Feed continuity and MockX adapters | `feed_engine_tests`, `mockx_md_tests`, `mockx_md_session_tests`; optional QuickFIX/session tests |
| Appliance atomic dispatch and authorization | `dispatch_pipeline_tests`, `risk_dispatch_pipeline_tests`, `cancel_pipeline_tests`, `fastpath_contract_tests` |
| Appliance restoration and risk | `fast_recovery_tests`, `risk_dispatch_recovery_tests`, `risk_kernel_foundation_tests` |
| Native messages, authentication and configuration | `native_protocol_codec_tests`, `native_marketdata_codec_tests`, `session_auth_tests`, `appliance_cli_tests`, schema checker |
| Packaging and dependency boundaries | `sorx_install_consumer_smoke`, `sorx_core_protocol_boundary` |

Most compiled CTest names prefix the source basename with `sorx_`. The build definitions are the authoritative test registry. Assertions are kept active in optimized test configurations.

## Common symptoms

| Symptom | Check and next action |
|---|---|
| CMake cannot find the requested Visual Studio generator | Use a CMake release supporting the installed toolchain and a fresh build directory. The presets require VS 2026. |
| QuickFIX not found | Leave it off for the in-process demo, or provide the matching C++ installation/toolchain as described in Build and Installation. |
| Constructing `SorRuntime` with configuration alone fails | Supply a profile-owned `WireIdentityFactory`; the one-argument constructor is deleted. |
| Configuration parse fails | Inspect unknown/duplicate keys, identity references, required fields, exact conversions and the correct schema family. |
| A receipt is accepted but there is no fill | A receipt is durable command acceptance. Check event publication, per-venue egress draining, dispatch results and asynchronous reports. |
| Routing stays in recovery mode | Reconcile all nonterminal ownership, then verify feed/session/accounting readiness before `complete_recovery()`. Do not discard the journal to bypass ownership. |
| A child is `AMBIGUOUS` | Preserve its reservation. Use externally established reconciliation; a timeout does not prove the original order was absent. |
| Feed is no longer `LIVE` | Inspect continuity, freshness and session generations; restore from a proven snapshot cut. Known-child cancels use their recorded mapping. |
| Appliance configuration passes but `--config` activation fails | This is the implemented safe-shell boundary. Offline validation does not provide supervisors or qualified backends. |
| Production token/readiness gate rejects a test backend | Volatile durability and incomplete hardware facts cannot authorize production TX. Use the reference test mode only for software tests. |

For hardware measurements, power-loss testing and release criteria, use [Operations and Qualification](https://github.com/lehajamb/sorx/wiki/Operations-and-Qualification). For documentation updates, use [Contributing and Wiki Maintenance](https://github.com/lehajamb/sorx/wiki/Contributing-and-Wiki-Maintenance).

Sources: [test registry](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/CMakeLists.txt), [CI matrix](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/.github/workflows/ci.yml), [schema checker](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/check_device_config_schema_contract.py), [guide smoke test](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/doc/.interaction-smoke.cjs), and [historical implementation report](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/IMPLEMENTATION_REPORT.md).
