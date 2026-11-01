# Build and Installation

SORX builds static C++20 libraries with CMake and optionally adds its acceptance demo, QuickFIX adapter and appliance command shells. Begin with the tested Windows commands in [Getting Started](https://github.com/lehajamb/sorx/wiki/Getting-Started).

## Requirements and switches

CMake requires at least 3.20; use a version that supports the selected compiler generator. A C++20 compiler and platform thread support are required. Python 3 is discovered optionally for schema validation. Node.js is separate from the C++ build and is used for the visual guide smoke test.

| CMake option | Default | Effect |
|---|---|---|
| `BUILD_TESTING` | `ON` through CTest | Build and register tests; the Python schema check additionally needs a discovered interpreter. |
| `SORX_ENABLE_QUICKFIX` | `OFF` | Require QuickFIX C++ and build the session adapter and concrete MockX venue runtime. |
| `SORX_ENABLE_INTEGRATION` | `OFF` | Add the in-process multi-MockX demo and acceptance test. No external services are required. |
| `SORX_BUILD_APPLIANCE` | `ON` | Build five offline appliance command shells. Appliance foundation libraries are declared independently of this switch. |

All SORX targets enable warnings as errors: `/W4 /WX /permissive-` on MSVC and `-Wall -Wextra -Wpedantic -Werror` elsewhere. Turning off the appliance executables does not remove `sorx_protocol` or work around the pinned Linux compilation failure.

## Windows configurations

The wiki uses `build-wiki` for the dependency-free integration build, with executables under `build-wiki\Release`. For a core test configuration without the example, use a separate directory and set `SORX_ENABLE_INTEGRATION=OFF`.

The checked-in presets require the **Visual Studio 18 2026** generator:

```powershell
cmake --preset msvc-dependency-free
cmake --build --preset msvc-dependency-free-build
ctest --preset msvc-dependency-free-test
```

Those presets use `build/dependency-free-msvc` and disable integration and QuickFIX. They differ from the wiki walkthrough intentionally. Other supported Visual Studio installations can use explicit configuration commands with `-A x64`, letting CMake choose its default installed generator.

## Optional QuickFIX

Install a compatible QuickFIX C++ build for the same architecture/configuration as the consumer. `CMAKE_PREFIX_PATH`, a package-manager toolchain, or `QUICKFIX_ROOT` can supply its location. Configure a separate tree; substitute your actual installation prefix:

```powershell
$quickfixPrefix = 'C:\path\to\quickfix-install'
cmake -S . -B build-wiki-quickfix -A x64 -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=ON -DSORX_ENABLE_INTEGRATION=ON "-DCMAKE_PREFIX_PATH=$quickfixPrefix" "-DQUICKFIX_ROOT=$quickfixPrefix"
cmake --build build-wiki-quickfix --config Release --parallel 4
ctest --test-dir build-wiki-quickfix -C Release --output-on-failure
```

Discovery first tries a `quickfix` CMake config package, recognizes its supported target spellings, and then falls back to the bundled `FindQuickFIX.cmake`. That module looks for `quickfix/Application.h` and the library, with pkg-config hints when available. Missing QuickFIX is a configure error when explicitly enabled.

The full-acceptance presets instead read `SORX_QUICKFIX_PREFIX` from the environment. See [MockX and QuickFIX Integration](https://github.com/lehajamb/sorx/wiki/MockX-and-QuickFIX-Integration) for session identity and durable store requirements.

## Linux status at the documented revision

The intended single-config commands are:

```bash
cmake -S . -B build-wiki-linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=OFF -DSORX_ENABLE_INTEGRATION=ON
cmake --build build-wiki-linux --parallel 4
ctest --test-dir build-wiki-linux --output-on-failure
```

These are **not a passing Linux quickstart for commit `3234de9`**. Its three Linux CI jobs fail while compiling `src/protocol/session_auth.cpp`: error returns such as `AuthVerifyResult{initialization_error_}` omit `message`, and GCC raises `-Werror=missing-field-initializers`. The tests are consequently not reached. Record that baseline when assessing a later fix; this wiki change does not alter compiler flags or application source.

## Install and consume the package

After the Windows walkthrough succeeds, install to a local prefix:

```powershell
cmake --install build-wiki --config Release --prefix build-wiki/install
```

The prefix contains headers, static libraries, CMake package files, schemas, deployment inputs, appliance configuration and selected documentation. Enabled appliance executables are installed into `bin`; installation does not activate services or qualify the host.

A consuming project's `CMakeLists.txt` can use:

```cmake
cmake_minimum_required(VERSION 3.20)
project(sorx_consumer LANGUAGES CXX)
find_package(sorx 0.2 CONFIG REQUIRED)
add_executable(sorx_consumer main.cpp)
target_link_libraries(sorx_consumer PRIVATE sorx::runtime sorx::order_entry_profiles)
```

Supply that installed prefix through `CMAKE_PREFIX_PATH` when configuring the consumer. `main.cpp` is your hosting application; [Embedded Cpp API](https://github.com/lehajamb/sorx/wiki/Embedded-Cpp-API) explains its responsibilities. Imported targets propagate public include paths and C++20 requirements.

| Target family | Use |
|---|---|
| `sorx::core`, `sorx::config`, `sorx::persistence`, `sorx::runtime` | Embedded routing, configuration, journal and coordinator |
| `sorx::order_entry_profiles`, `sorx::md_core`, `sorx::mockx_md_adapter` | MockX identities/FIX fields, neutral feeds and native MockX feed adapter |
| `sorx::quickfix_adapter`, `sorx::mockx_venue_runtime` | Present when QuickFIX and the corresponding adapters are built |
| `sorx::dataplane`, `sorx::risk`, `sorx::fastpath`, `sorx::risk_fastpath`, `sorx::recovery` | Bounded appliance route/risk/dispatch/recovery contracts |
| Other exported foundation targets | Link the protocol, recorder, market-data, configuration, audit, HA, telemetry or platform components your integration actually uses. |

The `sorx_install_consumer_smoke` CTest test installs the package into an isolated build subdirectory, configures a separate consumer and runs it. It also exercises optional exports when those features are built. A package built with QuickFIX requires that dependency to remain discoverable to its consumers.

Sources: [targets and installation](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/CMakeLists.txt), [presets](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/CMakePresets.json), [QuickFIX finder](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/cmake/FindQuickFIX.cmake), [installed package config](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/cmake/sorxConfig.cmake.in), and [consumer test](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/cmake/SorxInstallConsumerTest.cmake).
