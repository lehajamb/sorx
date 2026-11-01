# Device configuration and CLI

Use the appliance CLI to validate a DeviceConfig v2 document offline or to
migrate a complete legacy appliance INI file. Validation starts no runtime
threads or sockets and does not activate routing.

DeviceConfig is separate from the embedded routing INI described in
[Embedded configuration](https://github.com/lehajamb/sorx/wiki/Embedded-Configuration).
In particular, `configs/multi_mockx/sorx.ini` is not a legacy appliance file
and is not input for `sorxctl --migrate-ini`.

## Validate the checked-in example

The commands below assume a Windows Release build in `build-wiki`
with the default `SORX_BUILD_APPLIANCE=ON`. See
[Build and installation](https://github.com/lehajamb/sorx/wiki/Build-and-Installation)
for the build procedure and platform differences.

```powershell
build-wiki\Release\sorxctl.exe --help
build-wiki\Release\sorxctl.exe --version
build-wiki\Release\sorxctl.exe --check-config configs\appliance\sorx-device-v2.yaml
```

The sample validation prints:

```text
sorxctl: valid DeviceConfigV2 device=SORX-TEST-1 contexts=1 ha=STANDALONE
```

This proves that the document satisfies software admission rules. It does
not verify its referenced certificates, hardware, remote endpoints, storage
performance or production qualification evidence.

## Preserve the canonical flat YAML format

The codec uses quoted path keys and quoted scalar values, with an explicit
format marker. The following excerpt illustrates the syntax; use the full
checked-in example as the starting document because this excerpt is incomplete.

```yaml
---
format: "SORX_DEVICE_CONFIG_V2"
"device/schema_version": "2"
"device/device_id": "SORX-TEST-1"
"device/deployment_mode": "PRODUCTION"
"management/interface": "mgmt0"
"management/require_mutual_tls": "true"
"management/private_key_reference": "PKCS11:MGMT-KEY"
"recorder/retention_seconds": "900"
"routing-context primary/account_context": "ACCOUNT-A"
```

Do not convert these entries into nested YAML objects or replace opaque key
references with secret material. Configuration admission checks cover the
complete document, including capacities, identities and cross-references.
Diagnostics identify the file and, when available, the line and field path.

Production admission requires exactly one routing context, distinct
management/audit/data interfaces, a dedicated audit interface, management and
audit mTLS, and opaque TPM/TPM2/PKCS#11 references. Recorder budgets require at
least 900 seconds at 500,000,000 bytes/second, at least 4 GiB usable Tier 0,
nonzero normal/cancel/reconciliation capacity, and total storage covering
retention times qualified rate plus Tier 0. These are requirements to check,
not measured properties of the sample device.

## Migrate a legacy appliance document

Only run this recipe when `legacy-device.ini` is a complete legacy appliance
configuration and the output path does not exist:

```powershell
build-wiki\Release\sorxctl.exe --migrate-ini legacy-device.ini device-v2.yaml
build-wiki\Release\sorxctl.exe --check-config device-v2.yaml
```

Migration validates the legacy input, encodes YAML, decodes it again, and
checks object equality and the route-critical SHA-256 before publication.
It refuses an existing destination or temporary output. A second invocation
must not overwrite the first result. Retain the printed route-critical hash
with the configuration review; migration does not commit or activate it.

## Understand the available commands

All five binaries (`sorxd`, `sorx-fixgw`, `sorx-auditd`, `sorx-mgmtd`,
`sorxctl`) accept `--help`, `--version` and `--check-config PATH`.
Only `sorxctl` accepts `--migrate-ini INPUT OUTPUT`.

| Exit code | Meaning in the current shell |
| --- | --- |
| `0` | Help/version, successful validation or successful migration. |
| `2` | Configuration, input/output or migration-publication error. |
| `64` | Unsupported arguments, including runtime `--config` activation. |
| `70` | Internal migration round-trip verification failed. |
| `78` | No-argument startup refused because runtime activation is unavailable. |

## Configuration store and schema boundary

The C++ configuration store binds a commit to the running revision, candidate
revision and complete candidate SHA-256. Dual approvals additionally bind the
action, diff, expiry and approver request ID and are single-use. This library
contract does not imply an available authenticated management server or CLI
commit command.

`schemas/device_config_v2.proto` mirrors the C++ graph: 18 messages, four enums
and 132 fields at this source baseline. Run the drift check from the repository
root:

```powershell
python tests\check_device_config_schema_contract.py --self-test
```

No generated protobuf runtime, `protoc`-backed codec or management RPC adapter
is linked. The network supervisors, durable configuration service and
production secret providers remain qualification work; see
[Operations and qualification](https://github.com/lehajamb/sorx/wiki/Operations-and-Qualification).

## Source baseline

- [Complete canonical DeviceConfig v2 sample](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/configs/appliance/sorx-device-v2.yaml)
- [CLI parsing, validation and migration](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/appliance/app_main.cpp)
- [DeviceConfig types, codecs and configuration store](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/appliance/device_config.hpp)
- [Normative protobuf schema](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/schemas/device_config_v2.proto)
- [Schema drift and negative-control checker](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/check_device_config_schema_contract.py)
