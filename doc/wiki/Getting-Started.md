# Getting Started

This walkthrough builds the embedded reference model and runs one protected IOC order across two in-process MockX venue profiles. It needs no external exchange, socket service, credentials or QuickFIX installation. The appliance libraries and offline command shells also build, but are a separate execution tier.

## Prerequisites

Use Windows with Visual Studio or Build Tools containing the C++ x64 tools and Windows SDK, CMake 3.20 or newer, and Git. The selected CMake version must support your Visual Studio generator; the checked-in presets specifically name Visual Studio 18 2026. Python 3 enables the configuration-schema test. Node.js is needed only for the interactive guide's separate smoke test.

The commands below are PowerShell commands from the repository root. The pinned revision's Linux build has a known compiler failure; see [Build and Installation](https://github.com/lehajamb/sorx/wiki/Build-and-Installation).

## Get the documented source

```powershell
git clone https://github.com/lehajamb/sorx.git
cd sorx
git checkout 3234de923d6497b9a119c80485a717ac24d4e6fe
```

The checkout intentionally selects the revision documented here. Use a development branch when making your own changes.

## Configure, build and test

```powershell
cmake -S . -B build-wiki -A x64 -DBUILD_TESTING=ON -DSORX_ENABLE_QUICKFIX=OFF -DSORX_ENABLE_INTEGRATION=ON -DSORX_BUILD_APPLIANCE=ON
cmake --build build-wiki --config Release --parallel 4
ctest --test-dir build-wiki -C Release --output-on-failure
```

Use a fresh build directory if switching generators. `--parallel 4` bounds build concurrency on a development machine. Release tests retain their assertions; the project explicitly undefines `NDEBUG` for its test executables.

`SORX_ENABLE_INTEGRATION=ON` adds the deterministic example and its test. It does not enable a networked deployment. With Python available, the Windows integration suite has 47 tests at this revision, including the installed-package consumer check.

## Run the two-venue example

```powershell
.\build-wiki\Release\sorx_multi_mockx_demo.exe configs\multi_mockx\sorx.ini
```

Expected transcript:

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

The parent requests ten units with protection 101. Venue A supplies four at 100; venue B supplies the remaining six at 101. The profile renders those exact canonical prices as venue FIX decimals. The final outcome accounts for all ten units and releases the risk reservation.

The demo verifies its expected transitions and returns nonzero on a mismatch. Its FIX lines are normalized field-value transcripts, not complete network messages. It feeds normalized events into the real feed engine and runtime, then supplies local acceptance, acknowledgement and fill events in deterministic order.

## Check an appliance configuration offline

```powershell
.\build-wiki\Release\sorxctl.exe --check-config configs\appliance\sorx-device-v2.yaml
python tests\check_device_config_schema_contract.py --self-test
```

The first command checks the appliance DeviceConfig v2 example; the second verifies the C++/protobuf schema contract and its negative checks. Passing them does not establish hardware readiness or start order routing. The embedded demo's INI and the appliance YAML are distinct configuration systems.

## Explore the behavior

Open the [visual guide](https://lehajamb.github.io/sorx/) to step through routing, order ownership and recovery. For a local copy, open `doc/index.html`, or serve the repository:

```powershell
python -m http.server 8765 --bind 127.0.0.1 --directory .
```

Browse `http://127.0.0.1:8765/doc/index.html`; stop the local server with Ctrl+C. Its optional Google Fonts have system-font fallbacks.

Continue with [Embedded Cpp API](https://github.com/lehajamb/sorx/wiki/Embedded-Cpp-API) to integrate a host application, [Embedded Configuration](https://github.com/lehajamb/sorx/wiki/Embedded-Configuration) to understand the fixture, and [MockX and QuickFIX Integration](https://github.com/lehajamb/sorx/wiki/MockX-and-QuickFIX-Integration) for the native-session boundary.

Sources: [demo](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/examples/multi_mockx/main.cpp), [example contract](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/examples/multi_mockx/README.md), [fixture](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/configs/multi_mockx/sorx.ini), and [build definitions](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/CMakeLists.txt).
