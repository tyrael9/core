# PartyBotEngine

Independent party-coordinated AI for the pinned Vanilla 1.12.1 / build 5875 vMaNGOS Core. The design borrows the Strategy / Trigger / Action / Value / Multiplier model from CMaNGOS PlayerBots and encounter-strategy ideas from AzerothCore, but all runtime behavior is implemented against the pinned vMaNGOS C++14 APIs.

## Current status (2026-11-21)

The project has completed most of the engine foundation and the first exclusive runtime/async-dispatch infrastructure. It has **not** reached playable New Engine combat.

Runtime authority is explicit:

- `PlayerBot.EngineAuthority = 0`: Legacy, the default and current safe baseline.
- `PlayerBot.EngineAuthority = 1`: Shadow observation; Legacy remains the only executor.
- `PlayerBot.EngineAuthority = 2`: NewEngine exclusive authority. Legacy class/role/combat policy is bypassed; absent Engine actions are deliberate no-ops, never a Legacy fallback.

Implemented foundations include value-only same-map party snapshots, per-bot value cache and trigger scheduling, coordinator reservations, immediate and pending queues, the NewEngine decision-cycle boundary, bounded synchronous ActionResult and Core Spell terminal listeners, typed AND/alternative dependencies, idempotency tombstones, continuation dispatch, queue starvation resistance and ready-backlog preservation under full-queue pressure (F28–F38). F39 adds a typed, action/encounter-correlated Player point-movement terminal source with destination verification and continuation integration. F40 audits item-use fan-out/consumption semantics and adds the initial real Core potion fixture. F41 adds a final CoreAction execution wrapper and map-thread actor/target/map/instance resolver; all four Engine-created DecisionContext copies preserve that resolver. F42 implements bounded item-use root/child aggregation, independent Core charge/count mutation events, ItemUseTerminal listeners/continuations, a synthetic two-ON_USE-slot fixture and a real delayed Brimstone/Fireball ON_USE cancellation fixture covering both sides of the Core charge mutation. Trusted push #44852141749 reaches final Runtime PASS. F43 adds mandatory action-specific Core preflight, structured execution reasons, SelectHostileTargetAction (live hostile accept / stale friendly reject), ClearIdleTargetAction (idle-only stale selection cleanup), StopUnattackableTargetAction (stale victim rejection and AttackStop-only cleanup) and FollowPartyMemberAction (native MoveFollow, GUID-only current-follow snapshot, same-target no-op, live Group revalidation and safe retarget) and StopFollowingAction (live follow-target identity preflight, `MovementExpired(false)` lifetime-safe expiry, and stale-request preservation of unrelated Point movement). The fifth adapter's trusted runtime validation is pending.

These foundations do not constitute production movement policy or class/role adapters. The five generic adapters are implemented and tested but remain unregistered; they are not a complete executor library or proof of playable combat AI.

**Critical limit:** no generic, class or role action is currently registered in the production NewEngine registry. The five reusable generic adapters are available for deliberate future registration after wider action matrices are established. NewEngine remains a deliberate no-op with no Legacy fallback; the default remains Legacy.

## Validation snapshot

- F42 PR CI [#44839755150](https://github.com/tyrael9/core/actions/runs/44839755150) passed pure Engine tests, report collector, configure, full ARM64 Core build and harness syntax. Trusted push [#44839748022](https://github.com/tyrael9/core/actions/runs/44839748022) additionally ran the full fixture-backed two-ON_USE-slot aggregate and reached final Runtime PASS, closing the initial root/child/mutation aggregation slice. The final delayed ON_USE cancellation-before/after-charge-mutation coverage and full F42 closure are validated by [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749) below.
- F43 first generic action PR [#44845475962](https://github.com/tyrael9/core/actions/runs/44845475962) and trusted runtime [#44845469020](https://github.com/tyrael9/core/actions/runs/44845469020) passed.
- F43 two-action head [#44847028717](https://github.com/tyrael9/core/actions/runs/44847028717) passed PR tests/build; trusted push [#44847023073](https://github.com/tyrael9/core/actions/runs/44847023073) passed full Core simulator, including idle selection cleanup, hostile acceptance, stale-friendly rejection and cleanup.
- F43 three-action head [#44849541737](https://github.com/tyrael9/core/actions/runs/44849541737) passed PR tests/build; trusted push [#44849535626](https://github.com/tyrael9/core/actions/runs/44849535626) passed full Core simulator with `registered=1 attackableNotProposed=1 staleSelected=1 staleRejected=1 liveUnattackable=1 selected=1 stoppedOnly=1` and final Runtime PASS.
- F42 delayed-use/cancellation closure / head `dfdfdf08628d64517f0e1b25e17e6029acd31c44`: PR [#44852146750](https://github.com/tyrael9/core/actions/runs/44852146750) and trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749) passed. The trusted runtime proves pre-mutation cancel (`-5>-5`), post-mutation cancel (`-5>-4`), Interrupted child terminals, mutation-before-effects ordering, no delayed target effect, and final Runtime PASS. Artifact: [partybot-simulator-dfdfdf0](https://github.com/tyrael9/core/actions/runs/44852141749/artifacts/14685618923).
- Fourth generic action / head `f73001882d3bdde5695e481e7af06e6964faba52`: PR [#44860033826](https://github.com/tyrael9/core/actions/runs/44860033826) and trusted push [#44860028963](https://github.com/tyrael9/core/actions/runs/44860028963) passed. Follow fixture marker: `registered=1 firstStarted=1 followTargetCaptured=1 sameTargetNotRestarted=1 retargetRegistered=1 retargeted=1 firstLeaderRemoved=1 staleSelected=1 staleGroupRejected=1`; F24 trace scoping and final Runtime PASS also passed. Artifact: [partybot-simulator-f730018](https://github.com/tyrael9/core/actions/runs/44860028963/artifacts/14689004218).
- PR #1 remains Draft on the pinned Vanilla 5875 validation base. No image was published and no Oracle deployment or live-realm test was performed.

## Phase status

- **Phases 0–1:** pinned baseline and lifecycle host are established; source/API audit is substantial but action-specific adapter audits continue.
- **Phase 2:** decision-engine foundation, cache/scheduler lifecycle hooks and queue primitives are implemented.
- **Phase 2.5:** extensive fixture-backed tests exercise real Core combat, spell, threat, Group, death/recovery, movement/follow and map-transfer behavior. This is test infrastructure, not a complete autonomous encounter simulator.
- **Phase 3:** encounter trace and local read-only report collector are implemented; detailed AI state, some trace coverage, cross-map closure and deployment-diagnostics integration remain open.
- **Phase 4:** F29–F42 provide exclusive NewEngine authority, dispatch, async dependencies, item-use aggregation and map-thread Core resolution. F42 is fully CI-closed, including real-Core delayed ON_USE cancellation before and after the actual charge mutation, by trusted runtime [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749). A separate count-based consumable/reagent fixture remains additional coverage, not a blocker for the current F42 scope. F43 now has mandatory action-specific preflight/structured results and five tested generic adapters, validated by PR [#44860033826](https://github.com/tyrael9/core/actions/runs/44860033826) and trusted runtime [#44860028963](https://github.com/tyrael9/core/actions/runs/44860028963). All four actions are intentionally unregistered; class/role actions remain future work.
- **Phases 5–8:** generic behavior state machine, class/role library, full party coordination and dungeon/raid encounter strategies remain future work.

## Design documents

- `ARCHITECTURE.md`: runtime authority, responsibilities, strategy composition and execution boundary.
- `ROADMAP.md`: phases, milestones, open gates and acceptance criteria.
- `ACTION-QUEUE.md`: immediate/pending dispatch, continuations, prerequisites and bounded-queue semantics.
- `DECISION-TRACE.md`: encounter event schema, budgets and post-fight analysis contract.
- `VMANGOS-API-AUDIT.md`: pinned Core update/threading and action-adapter API audit.
- `CORE-COMBAT-SIMULATOR.md`: isolated fixture-backed simulator and its exact evidence boundaries.
- `ITEM-USE-AGGREGATE.md`: F40 Core API audit and F42's implemented, runtime-validated multi-spell root/child/mutation contract, including delayed cancellation before/after charge mutation.
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