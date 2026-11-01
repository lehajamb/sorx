# Routing and Risk

Embedded SORX makes one deterministic decision from an immutable candidate table and per-venue runtime views. It uses exact canonical integer prices and quantities, configured venue eligibility, displayed depth, and explicit risk reservations. The appliance has a separate bounded route/risk kernel and adaptive-policy foundation, described in [Appliance Execution Foundation](https://github.com/lehajamb/sorx/wiki/Appliance-Execution-Foundation).

## Inputs and eligibility

The management-side `RouteRib` expresses configured intent; `FibCompiler` produces a generation-stamped `CandidateTable`. Feed and session updates supply `CandidateRuntimeView` values. A decision records the candidate-table and risk generation, plus each candidate's adjacency generation and available `BookVersion`.

Those pins identify the exact views used. They do not make independently received venue books one atomic cross-venue market snapshot.

| Gate | Required condition |
|---|---|
| Administration and capability | Venue is up; the selected intent is supported. |
| Book identity and validity | A valid owned book matches the instrument and venue. |
| Market state | Both the view and book are `Live`, sufficiently fresh, and `Trading`. |
| Order-entry state | Session is `Ready`; egress is `Ready`. |
| Conversion | Parent protection and selected allocation values convert exactly. |
| Risk | Venue/runtime risk eligibility and coordinator hard/aggregate limits permit the proposal. |

`RouteDecision.exclusions` records a reason per excluded venue. Examples include stale books, unavailable order entry, congestion, conversion incompatibility, and risk prohibition. Eligibility for new orders is separate from the ability to attempt cancellation of an already owned child.

## Aggressive IOC

`AggressiveIOC` always carries a protection price. A buy consumes asks at or below that price; a sell consumes bids at or above it.

1. Filter candidates and pin their versions.
2. Compare the best remaining executable displayed level across eligible venues: best price first, then lower `aggressive_priority`, then lower venue ID.
3. Allocate within displayed depth, parent remaining quantity, per-venue allocation caps, and `maximum_aggressive_venues`.
4. Aggregate retained allocations into at most one child per selected venue. Each child uses that venue's worst retained level as its limit.
5. Record the unallocated quantity and a reason explicitly.

For the two-venue example, a buy of 10 at protection 101 sees venue A offering 4 at 100 and venue B offering 6 at 101. The result is A: 4 at 100, B: 6 at 101, remainder zero. This is allocation against the observed displayed books; the later venue reports decide whether the orders fill.

The policy never invents hidden liquidity. Truncated depth remains explicitly unknown. If a selected venue's accumulated quantity is not exactly convertible, the policy trims its worst retained slices to a convertible quantity and reports the residual. It does not silently round a venue quantity or run a second sweep to refill that trimmed residual.

| Remainder reason | Interpretation |
|---|---|
| `ParentLimitReached` | Further displayed prices breach the protection price. |
| `InsufficientDisplayedDepth` | Eligible displayed depth is exhausted. |
| `TruncatedDepthUnknown` | The observed depth is incomplete and some quantity remains. |
| `VenueAllocationCaps` | Configured per-venue limits constrained allocation. |
| `MaximumVenueCount` | The decision cannot select another venue. |
| `InexactConversion` | Available values cannot all be represented exactly in venue units. |
| `NoEligibleCandidate` | No candidate passes the necessary gates. |

The result carries one primary remainder reason even when multiple constraints apply; inspect exclusions and version pins alongside it. An accepted IOC route can have a nonzero remainder. SORX does not automatically chase or re-route unfilled quantity after dispatch.

## Passive GTC

`PassiveGTC` requires the full quantity to fit one selected venue. Before selecting a passive route, the policy rejects a buy that would cross any commonly eligible venue's best ask, or a sell that would cross its best bid. This marketability check occurs before filtering for passive capability and full allocation capacity; selecting a different venue does not hide an observed eligible cross.

From the remaining candidates, require passive support, an allocation cap covering the full quantity, and exact quantity conversion. Rank by:

1. Lower `passive_priority`.
2. Lower `configured_latency_ns`.
3. Lower venue ID.

The child uses the parent's limit, with zero remainder on acceptance. If no venue can accept the full quantity, the route is rejected. The configured latency field is a stable preference, not a live measurement or execution guarantee.

## Hard limits and reservations

The coordinator validates positive IDs/quantity/price, recognized side/intent, routing and kill-switch gates, allowed instrument, parent quantity/notional, price collar, and open-parent count before routing. Checked arithmetic rejects overflow.

After forming a proposal, it checks child quantity/notional, open-child count, instrument gross outstanding quantity, and per-venue outstanding limits. An offending venue can be marked risk-ineligible and the decision replanned before any child is prepared. This bounded pre-dispatch policy work is different from retrying quantity at another venue after ownership becomes uncertain.

Accepted child allocations create reservations in the same logical transition as `ChildPrepared`. In this embedded implementation, the original child allocation remains reserved through partial fills; the reservation is released when the child reaches a definite terminal state. An ambiguous child retains its reservation. The coordinator therefore continues accounting conservatively for orders whose venue outcome cannot be proved.

The embedded kill switch blocks new and replacement command validation. Setting it does not automatically cancel every active child; cancellation remains an explicit parent operation. Feed withdrawal and venue administration also do not erase existing ownership. See [Order Lifecycle](https://github.com/lehajamb/sorx/wiki/Order-Lifecycle).

## Diagnose a decision

Use `SorRuntime::trace(parent)` to inspect `hard_risk_accepted`, `hard_rejection`, and the policy result without submitting an order. A successful trace is an observation, not a capacity reservation or promise that a later submit sees unchanged inputs. Use `inspect()` and parent reports to correlate actual durable children and reservations.

For interactive examples, open the [route decision lab](https://lehajamb.github.io/sorx/#routing). For configured limits and unit mappings, see [Embedded Configuration](https://github.com/lehajamb/sorx/wiki/Embedded-Configuration).

## Source references

- [Policy inputs, decisions, and reason enums](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/routing_policy.hpp)
- [Sweep, exact trimming, and passive selection implementation](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/routing_policy.cpp)
- [Hard limits, proposal checks, reservations, and trace](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/src/order_core.cpp)
- [Routing scenarios](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/routing_tests.cpp) and [risk conservation tests](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/risk_conservation_tests.cpp)
