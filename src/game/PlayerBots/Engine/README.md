# PartyBotEngine

Independent party-coordinated AI for the pinned Vanilla 1.12.1 / build 5875 vMaNGOS Core. The design borrows the Strategy / Trigger / Action / Value / Multiplier model from CMaNGOS PlayerBots and encounter-strategy ideas from AzerothCore, but all runtime behavior is implemented against the pinned vMaNGOS C++14 APIs.

## Current status (2026-11-20)

The project has completed most of the engine foundation and the first exclusive runtime/async-dispatch infrastructure. It has **not** reached playable New Engine combat.

Runtime authority is explicit:

- `PlayerBot.EngineAuthority = 0`: Legacy, the default and current safe baseline.
- `PlayerBot.EngineAuthority = 1`: Shadow observation; Legacy remains the only executor.
- `PlayerBot.EngineAuthority = 2`: NewEngine exclusive authority. Legacy class/role/combat policy is bypassed; absent Engine actions are deliberate no-ops, never a Legacy fallback.

Implemented foundations include value-only same-map party snapshots, per-bot value cache and trigger scheduling, coordinator reservations, immediate and pending queues, the NewEngine decision-cycle boundary, bounded synchronous ActionResult and Core Spell terminal listeners, typed AND/alternative dependencies, idempotency tombstones, continuation dispatch, queue starvation resistance and ready-backlog preservation under full-queue pressure (F28–F38).

F39 is currently an API-semantics audit: legacy PointMovementGenerator finalization callbacks also run on forced cleanup, so `MovementInform(POINT_MOTION_TYPE, id)` alone is not a successful-arrival signal. Typed movement terminal events still need implementation. Item-use terminal semantics are also unaudited.

**Critical limit:** NewEngine currently has no production class/role Core action adapters registered. The exclusive mode and generic dispatch mechanisms are verified, but meaningful NewEngine gameplay is not available. The default remains Legacy.

## Validation snapshot

- Full trusted push CI [#44716873212](https://github.com/tyrael9/core/actions/runs/44716873212) passed on source-code head `08d06610d2e042fc62963a9677d7ed481dc501ac`: pure PartyBotEngine tests, read-only report collector tests, Linux ARM64 Vanilla 5875 `mangosd` + `realmd` build, simulator harness checks, full fixture-backed Core simulator and final `Runtime PASS`. The run uploaded the simulator log artifact.
- Current branch head `e2ed0247e586b48d545e1eb189a8529d3a5b024b` contains documentation-only changes after that code head; it does not change the tested C++ source.
- Latest head docs-only push CI [#44718136178](https://github.com/tyrael9/core/actions/runs/44718136178) and PR CI [#44718146532](https://github.com/tyrael9/core/actions/runs/44718146532) passed the documentation path. Their ARM64 build and fixture runtime were intentionally skipped; do not interpret them as new full simulator runs.
- PR #1 remains Draft. No image was published and no Oracle deployment or live-realm test was performed by these CI runs.

## Phase status

- **Phases 0–1:** pinned baseline and lifecycle host are established; source/API audit is substantial but action-specific adapter audits continue.
- **Phase 2:** decision-engine foundation, cache/scheduler lifecycle hooks and queue primitives are implemented.
- **Phase 2.5:** extensive fixture-backed tests exercise real Core combat, spell, threat, Group, death/recovery, movement/follow and map-transfer behavior. This is test infrastructure, not a complete autonomous encounter simulator.
- **Phase 3:** encounter trace and local read-only report collector are implemented; detailed AI state, some trace coverage, cross-map closure and deployment-diagnostics integration remain open.
- **Phase 4:** F29–F38 establish exclusive NewEngine authority, decision-cycle dispatch and async dependency infrastructure. F39 is the movement-terminal audit. The production action executor and action adapters remain the major gate.
- **Phases 5–8:** generic behavior state machine, class/role library, full party coordination and dungeon/raid encounter strategies remain future work.

## Design documents

- `ARCHITECTURE.md`: runtime authority, responsibilities, strategy composition and execution boundary.
- `ROADMAP.md`: phases, milestones, open gates and acceptance criteria.
- `ACTION-QUEUE.md`: immediate/pending dispatch, continuations, prerequisites and bounded-queue semantics.
- `DECISION-TRACE.md`: encounter event schema, budgets and post-fight analysis contract.
- `VMANGOS-API-AUDIT.md`: pinned Core update/threading and action-adapter API audit.
- `CORE-COMBAT-SIMULATOR.md`: isolated fixture-backed simulator and its exact evidence boundaries.
- `TESTING.md`: CI paths, simulator execution, test-realm checks and validation vocabulary.
- `THREAT-BUDGETS.md`: Core-authoritative aggro semantics and intentionally fail-closed threat-impact estimation.
- `RESEARCH-CMANGOS.md` / `RESEARCH-AZEROTHCORE.md`: architectural research only; no foreign runtime dependency or WotLK data is imported.

## Pinned baseline

- Core: `0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`
- Deploy wrapper reference: `ebaa560951459419c35140e20d787ec5db2905b8`
- Client/data: Vanilla 1.12.1 / build 5875
- Language: C++14
- Target: Linux ARM64, Oracle A1 (1 OCPU / 6 GB)

Do not rebase onto moving upstream without auditing PartyBot, lifecycle and database changes.

## Build and test

Run the standalone C++14 decision-engine harness from the repository root:

```sh
bash src/game/PlayerBots/Engine/tests/run_tests.sh
```

The focused PartyBot Core workflow also runs report-collector tests and a full Linux ARM64 / Vanilla 5875 Core build. Trusted branch pushes additionally run the fixture-backed simulator when the extracted-data URL and checksum secrets are available. PR jobs deliberately do not receive those fixture secrets.

A CI pass is not an in-game test and does not authorize deployment.

## Operational boundaries

The simulator uses disposable `partybot_sim_*` database schemas and never connects to the Oracle realm. Repository development does not publish an image, restart a server, change deployment configuration or mutate live database volumes. Image publication and deployment remain separate, explicitly authorized operations.