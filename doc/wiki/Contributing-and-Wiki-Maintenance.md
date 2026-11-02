# Contributing and Wiki Maintenance

Begin from the documented revision and identify which execution tier your change affects. The original embedded model and the appliance foundation share behavioral goals, but use different ownership, persistence and deployment boundaries.

## Find the implementation

| Area | Main repository locations |
|---|---|
| Embedded API, deterministic state and route policy | `include/sorx/runtime.hpp`, `order_core.hpp`, `routing_policy.hpp`; corresponding `src` files |
| Embedded configuration and persistence | `src/config.cpp`, `src/persistence/journal.cpp`, `configs/multi_mockx/sorx.ini` |
| Neutral feeds and first venue adapters | `src/feed_engine.cpp`, `src/adapters` |
| Bounded execution and recovery | `src/dataplane`, `src/risk`, `src/policy`, `src/fastpath`, `src/recovery` |
| Protocol and recorder foundations | `src/protocol`, `src/connectors`, `src/fixgw`, `src/recorder` |
| Appliance management and operational contracts | `src/appliance`, `src/audit`, `src/ha`, `src/platform`, `include/sorx/telemetry` |
| Software acceptance | `tests`, the CTest registry, and `cmake/SorxInstallConsumerTest.cmake` |
| Deployment inputs and qualification | `deploy`, `schemas`, and the appliance implementation/qualification documents |

## Preserve the contracts

Keep protocol/wire types outside the embedded core. Route using exact units and deterministic ordering. Preserve quantity ownership across every cancel, fill, replacement and ambiguous dispatch path. Separate the decision, durable acceptance and external effect boundaries.

Appliance changes also need bounded-capacity and no-allocation evidence for the tested hot path, exact-frame authorization, generation/fencing checks, and recovery-only behavior after post-durable failures. A test-only volatile recorder cannot establish production durability. Couple changes to the relevant tests in [Testing and Troubleshooting](https://github.com/lehajamb/sorx/wiki/Testing-and-Troubleshooting).

State what a test proves and what still requires external evidence. Codec conformance fixtures, in-process profile acceptance, native socket/session tests and exchange certification are different evidence levels.

## Update the wiki

Canonical editable Markdown lives in `doc/wiki`. Until the initial documentation PR is merged, use the [documentation branch](https://github.com/lehajamb/sorx/tree/codex/wiki-docs/doc/wiki). The separate GitHub wiki contains published copies. Publication is manual; editing a repository page does not automatically update the live wiki, and editing the live wiki does not update the repository.

1. Select the published source revision being documented and inspect its implementation and tests.
2. Edit the relevant pages in a documentation branch. Preserve filenames where possible because they determine public URLs.
3. Update Home, sidebar and cross-links together when adding or moving a topic.
4. When advancing the documented source, update the pinned source URLs, Home revision and footer revision/review date together.
5. Validate links, anchors, examples, fenced blocks and diagrams. Record actual commands, outcomes and skipped checks.
6. Open a documentation-only PR. Publish the reviewed Markdown through the separate wiki checkout, preserving independent changes and history.
7. Compare published files with repository files and verify representative rendered pages.

Detailed maintainer commands are in [wiki-maintenance.md](https://github.com/lehajamb/sorx/blob/codex/wiki-docs/doc/wiki-maintenance.md). Its branch link is intentional while the PR remains open; change documentation-source links to `main` when merging and retiring the branch.

Use absolute wiki URLs for navigation so links work from both GitHub's repository viewer and the wiki. Repository code links are pinned to the documented source commit. Supporting source material is authoritative for its own revision; historical plans and reports must retain their dates and status.

## Keep documentation surfaces clear

| Surface | Purpose and update path |
|---|---|
| README | Project entry point, build overview and links to the wiki and visual guide |
| GitHub wiki | Task guides, integration contracts, operational boundaries and source map; manually published Markdown |
| GitHub Pages | Existing interactive embedded-model visual guide; deployed by its own workflow from `main` |
| `docs` | Architecture, implementation boundary and hardware qualification records |

Keep interactive HTML/JavaScript on Pages. GitHub wiki pages use Markdown, tables, code fences and Mermaid. A documentation PR must not silently change APIs, weaken tests or implement missing behavior merely to make an example work.

Sources: [build and test contracts](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/CMakeLists.txt), [protocol-boundary gate](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/core_protocol_boundary.cmake), [appliance boundary](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/APPLIANCE_IMPLEMENTATION.md), and [qualification requirements](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/HARDWARE_QUALIFICATION.md).
