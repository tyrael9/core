# PartyBotEngine

Independent party-coordinated AI for the pinned Vanilla 1.12.1 / build 5875 vMaNGOS Core. The design borrows the Strategy / Trigger / Action / Value / Multiplier model from CMaNGOS PlayerBots and encounter-strategy ideas from AzerothCore, but all runtime behavior is implemented against the pinned vMaNGOS C++14 APIs.

## Current status (2026-11-20)

The project has completed most of the engine foundation and the first exclusive runtime/async-dispatch infrastructure. It has **not** reached playable New Engine combat.

Runtime authority is explicit:

- `PlayerBot.EngineAuthority = 0`: Legacy, the default and current safe baseline.
- `PlayerBot.EngineAuthority = 1`: Shadow observation; Legacy remains the only executor.
- `PlayerBot.EngineAuthority = 2`: NewEngine exclusive authority. Legacy class/role/combat policy is bypassed; absent Engine actions are deliberate no-ops, never a Legacy fallback.

Implemented foundations include value-only same-map party snapshots, per-bot value cache and trigger scheduling, coordinator reservations, immediate and pending queues, the NewEngine decision-cycle boundary, bounded synchronous ActionResult and Core Spell terminal listeners, typed AND/alternative dependencies, idempotency tombstones, continuation dispatch, queue starvation resistance and ready-backlog preservation under full-queue pressure (F28–F38). F39 adds a typed, action/encounter-correlated Player point-movement terminal source with destination verification and continuation integration. F40 audits item-use fan-out/consumption semantics and adds the initial real Core potion fixture. F41 adds a final CoreAction execution wrapper and map-thread actor/target/map/instance resolver; all four Engine-created DecisionContext copies preserve that resolver. F42 implements bounded item-use root/child aggregation, independent Core charge/count mutation events, ItemUseTerminal listeners/continuations and a synthetic two-ON_USE-slot Core simulator fixture.

These foundations do not constitute production movement policy or class/role adapters. The CoreAction base is an audited dispatch gate, not yet a complete executor library or a substitute for action-specific final range/LOS/resource/cooldown checks.

**Critical limit:** NewEngine currently has no production class/role Core action adapters registered. The exclusive mode and generic dispatch mechanisms are verified, but meaningful NewEngine gameplay is not available. The default remains Legacy.

## Validation snapshot

- PR CI [#44833258687](https://github.com/tyrael9/core/actions/runs/44833258687) passed changed-path classification, pure PartyBotEngine tests, report-collector tests, CMake configure, full Linux ARM64 Vanilla 5875 `mangosd` + `realmd` build and simulator-runner syntax validation. The private fixture runtime was skipped on this PR-triggered job by design.
- PR CI [#44838640743](https://github.com/tyrael9/core/actions/runs/44838640743) and trusted push CI [#44838633233](https://github.com/tyrael9/core/actions/runs/44838633233) were launched for head `2010ae74620de27b790bdda436031bdee16af4fc`. The trusted run passed pure Engine tests, report collector and configure and was building the ARM64 Core before the fixture-backed runtime; that runtime uses the corrected F42 marker gate. PR runtime is fixture-secret-isolated by design.
- PR #1 remains Draft on the pinned Vanilla 5875 validation base. No image was published and no Oracle deployment or live-realm test was performed.

## Phase status

- **Phases 0–1:** pinned baseline and lifecycle host are established; source/API audit is substantial but action-specific adapter audits continue.
- **Phase 2:** decision-engine foundation, cache/scheduler lifecycle hooks and queue primitives are implemented.
- **Phase 2.5:** extensive fixture-backed tests exercise real Core combat, spell, threat, Group, death/recovery, movement/follow and map-transfer behavior. This is test infrastructure, not a complete autonomous encounter simulator.
- **Phase 3:** encounter trace and local read-only report collector are implemented; detailed AI state, some trace coverage, cross-map closure and deployment-diagnostics integration remain open.
- **Phase 4:** F29–F42 establish exclusive NewEngine authority, decision-cycle dispatch, typed async dependencies, item-use root/child/mutation aggregation and the CoreAction/map-thread resolution boundary. F42's two-slot Core runtime result awaits completion of trusted Push CI [#44838633233](https://github.com/tyrael9/core/actions/runs/44838633233); safe delayed/cancel-before-and-after-mutation Core fixtures remain open. F43 production-executor hardening and audited generic adapters are next. Class/role action adapters remain future work.
- **Phases 5–8:** generic behavior state machine, class/role library, full party coordination and dungeon/raid encounter strategies remain future work.

## Design documents

- `ARCHITECTURE.md`: runtime authority, responsibilities, strategy composition and execution boundary.
- `ROADMAP.md`: phases, milestones, open gates and acceptance criteria.
- `ACTION-QUEUE.md`: immediate/pending dispatch, continuations, prerequisites and bounded-queue semantics.
- `DECISION-TRACE.md`: encounter event schema, budgets and post-fight analysis contract.
- `VMANGOS-API-AUDIT.md`: pinned Core update/threading and action-adapter API audit.
- `CORE-COMBAT-SIMULATOR.md`: isolated fixture-backed simulator and its exact evidence boundaries.
- `ITEM-USE-AGGREGATE.md`: F40 Core API audit and F42's implemented multi-spell item-use root/child/mutation contract, tests and remaining fixture gaps.
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