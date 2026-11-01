# Embedded Configuration

The embedded `ConfigurationLoader` reads a strict schema-v1 INI document and produces a `ValidatedConfiguration`. That object supplies the runtime's route inventory, instrument and venue guardrails, queue capacities, profile identities, and route-critical hash.

Use the complete [two-venue fixture](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/configs/multi_mockx/sorx.ini) as the starting point. The appliance uses a different [Device Configuration and CLI](https://github.com/lehajamb/sorx/wiki/Device-Configuration-and-CLI) contract; its legacy INI migration does not consume this embedded fixture.

## Document shape

Only these four case-sensitive section forms are accepted:

| Section | Meaning | Required fields |
|---|---|---|
| `[sorx]` | One account/routing context | `schema_version`, `account_id`, `maximum_aggressive_venues`, `ingress_capacity`, `risk_generation` |
| `[instrument <id>]` | Canonical symbol and hard instrument limits | `symbol`, `allowed`, parent/child quantity and notional limits, price collar, open-order limits, gross outstanding limit |
| `[venue <id>]` | Venue identity, operational mappings, capacities, and risk | Name/admin/risk fields; four queue capacities; market-data and order-entry channel/profile/session identities; dictionary and FIX CompIDs |
| `[route <instrument-id> <venue-id>]` | One configured instrument-to-venue candidate | Wire symbol, price/quantity conversion ratios, supported intents, allocation cap, priorities, configured latency |

IDs are positive `uint32` values. A route must refer to a declared instrument and venue. Unknown or duplicate sections/fields, invalid identities, unmapped references, overflow, and incompatible conversions are errors. Do not rely on omitted fields receiving route-critical defaults. Whole-line comments begin with `#` or `;`; keys and enum spellings are case-sensitive.

The global portion of the fixture is:

```ini
[sorx]
schema_version=1
account_id=ACCOUNT-MULTI-MOCKX
maximum_aggressive_venues=2
ingress_capacity=1024
risk_generation=1
```

This is an excerpt, not a standalone runnable configuration: instrument, venue, and route sections are also required.

## Instrument and venue controls

| Group | Keys and interpretation |
|---|---|
| Instrument identity | `symbol`; `allowed=true` or `false` controls eligibility. |
| Quantity limits | `maximum_parent_quantity`, `maximum_child_quantity`, `maximum_gross_outstanding`; all use canonical quantity units. |
| Notional limits | `maximum_parent_notional`, `maximum_child_notional`; checked integer price-times-quantity values. |
| Price collar | `minimum_price`, `maximum_price`; positive canonical prices with minimum no greater than maximum. |
| Order counts | `maximum_open_parents`, `maximum_open_children`; outstanding coordinator-owned objects. |
| Venue controls | `name`, `admin_state=UP` or `DOWN`, `risk_allowed=true` or `false`, `maximum_outstanding`. |
| Venue capacity | `market_data_ingress_capacity`, `order_entry_ingress_capacity`, `cancel_egress_capacity`, `normal_egress_capacity`. Cancels have their own egress capacity. |
| Numeric profile mapping | `market_data_channel_id`, `market_data_profile_id`, `order_entry_profile_id`. |
| Named profile mapping | `market_data_profile`, `market_data_session`, `order_entry_profile`, `order_entry_session`, `dictionary_identity`, `sender_comp_id`, `target_comp_id`. |
| Optional credential locator | `credential_reference`; an operational reference, not a place to embed secret material. |

The fixture maps instrument 7 (`ALPHA`) to venues 1 and 2. Its MockX identities use `MockXItchV1`, `MockXFix44V1`, and `FIX44-MOCKX-2026`, with distinct per-venue session and CompID mappings. The supplied wire-identity factory must agree with these validated identities.

## Route policy and exact conversion

The venue-A route in the fixture is:

```ini
[route 7 1]
wire_symbol=ALPHA-A
price_numerator=1
price_denominator=1
quantity_numerator=1
quantity_denominator=1
aggressive_ioc=true
passive_gtc=true
allocation_cap=1000
aggressive_priority=1
passive_priority=2
configured_latency_ns=100
```

This is another excerpt. At least one intent must be supported. The route's allocation cap cannot exceed the instrument child quantity limit or venue outstanding limit. Lower numerical priority wins; passive selection additionally uses configured latency and venue ID as deterministic tie-breakers. Configured latency is policy input, not a measured latency guarantee.

Conversions map canonical units to venue units as `canonical * numerator / denominator`, using exact integer arithmetic. A ratio must be positive and representable; the loader normalizes it using the greatest common divisor. Fractional or overflowing results are rejected rather than rounded. Validation checks the configured price collar, quantity/risk limits, and allocation cap against each route's conversion. Runtime orders and report values still need their own exactness checks.

The identity ratio does not specify a FIX decimal rendering scale. The demo's MockX profile separately uses two price decimal places and zero quantity decimal places, so canonical price 100 is rendered as FIX `44=1.00`. Keep numeric conversion identity and protocol text formatting aligned; see [MockX and QuickFIX Integration](https://github.com/lehajamb/sorx/wiki/MockX-and-QuickFIX-Integration).

## Loading and diagnostics

Call `ConfigurationLoader::parse_ini(text)` or `load_ini_file(path)`. Both return a `ConfigurationResult`. Its Boolean conversion succeeds only when `configuration` exists and `errors` is empty. Each error carries a code, line, section, field, and message; surface those details to the operator before constructing the runtime.

Useful error classes include `UnknownField`, `MissingField`, `DuplicateIdentity`, `UnmappedReference`, `InexactConversion`, and `Overflow`. Fix the configuration itself instead of coercing a rejected value in the caller. The [Embedded Cpp API](https://github.com/lehajamb/sorx/wiki/Embedded-Cpp-API) page shows loading and runtime construction.

## Configuration identity and recovery

The validated model includes a canonical SHA-256 route-critical hash. It covers the schema/account, instruments and hard risk, venue/session/dictionary identities, capacities, routes, conversion ratios, capabilities, limits, and priorities. Canonical ordering makes equivalent section orderings hash alike. `credential_reference` is intentionally excluded.

The journal binds durable ownership to its configuration identity. Do not rewrite session mappings, units, or route-critical values to force an existing journal to open. An incompatible identity is rejected during recovery; changing credentials through the same operational locator has different semantics from changing routing/session identity.

`set_venue_admin` is a sequenced runtime administration operation. Editing an INI file is not a hot-reload API. See [Persistence and Recovery](https://github.com/lehajamb/sorx/wiki/Persistence-and-Recovery) for startup and reconciliation requirements.

## Source references

- [Validated types, errors, and loader API](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/config.hpp)
- [Field schema, semantic validation, and canonical hashing](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/config.cpp)
- [Strict parsing and canonical-hash tests](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/config_tests.cpp)
- [Exact numeric value types](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/types.hpp)
