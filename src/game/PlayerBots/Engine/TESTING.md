# PartyBot Core build, runtime and trace testing

## Automated compile CI

The focused workflow at .github/workflows/partybot-core-build.yml runs the standalone PartyBot engine tests and builds the pinned Vanilla 1.12.1 / build 5875 C++14 Core on Linux ARM64, matching the Oracle A1 target.

A successful run proves configure/compile/link in that CI environment. It does not prove runtime behavior, live snapshot correctness, instance persistence, database compatibility beyond compilation or successful operation on the Oracle realm. It is not permission to deploy.

## Publishing and deployment

The ARM64 image workflow is maintained at .github/workflows/publish-partybot-image.yml. Always use the exact tag and Core SHA from the successful workflow run; do not infer a tag from an earlier run or from this document.

Publishing an image does not deploy it. Deployment is a separate user-authorized operation through the helper in tyrael9/vmangos-deploy/scripts/partybot-deploy.sh. Do not replace database images or touch database volumes. Players should log out before a server-image replacement because it disconnects active clients.

No CI task, documentation edit or image publish grants permission to restart or modify the Oracle server.

## Lifecycle regression — Legacy host

Owner-disconnect persistence was runtime-tested on the Oracle test realm on 2026-11-15:
- owner reconnect within the grace period preserved party membership, bot positions and instance state;
- intentional owner leave removed the bots;
- expiry beyond the grace period cleaned up the bots.

This validates the existing lifecycle-host/Legacy path only. Keep it as a regression test when modifying PartyBotAI host integration.

## Current Shadow smoke test (diagnostics only)

The current implementation has PlayerBot.EngineShadow and PlayerBot.DecisionTrace flags. This is an observe-only test, not New Engine execution:

1. On a disposable test realm, set PlayerBot.EngineShadow = 1.
2. Set PlayerBot.DecisionTrace = 1.
3. Restart the game server only after the intended test image has been separately approved/deployed.
4. Form a party and keep the PartyBots on the same map/instance.
5. Inspect [PartyBotShadow] and [PartyBotShadowDecision] lines for group/map/instance identity, member/hostile counts, role, strategy, target GUID, priority and selection status.
6. Remember that the old PartyBot still performs all actual combat behavior after the Shadow evaluation. These logs cannot prove an action was cast or a target was changed by the Engine.

The current Shadow trace is limited to a summary and at most three candidate details per bot per ten seconds. It is only sufficient for snapshot/candidate smoke tests, not full boss-fight diagnosis.

## Required New Engine encounter trace

Before New Engine mode is used for meaningful combat tests, implement the event contract in DECISION-TRACE.md. Required minimum evidence:
- encounter start/end, encounter ID and sequence number;
- party roster with class/role and same-map availability;
- per-bot state heartbeat during combat (health, mana, state, current cast and victim);
- trigger activation/inactivation with reason;
- candidate action, priority, channel, Possible/Useful outcome and rejection reason;
- arbitration outcome and competing-action/reservation explanation;
- action attempt and authoritative Core execution result, recorded separately;
- target changes and reason;
- threat samples when the vMaNGOS API exposes valid comparable values;
- casts/interruption/failure, health changes and death events;
- trace truncation or collection errors explicitly marked.

Do not silently suppress action results, death events or encounter-end summaries to satisfy normal sampling limits. Trace must be bounded and must not perform synchronous DB/network work on map threads.

## Post-fight collection workflow

Target experience:
1. Finish the Boss encounter.
2. Run one read-only diagnostics command on the Oracle host.
3. Attach the encounter report and raw matching log lines in chat.
4. Analysis classifies failures as perception, policy, coordination, eligibility, execution, encounter-model or trace-gap issues.

The collector should accept an encounter ID and also support a time-window fallback. It must not restart containers, change configuration, run SQL, access/mutate database volumes or deploy images.

The deployment repository currently has a Shadow diagnostics script. It must be extended only after the structured Engine trace schema is implemented; until then, the existing script is not a full encounter-report collector.

## Pure decision-engine tests

Run from the repository root:

    bash src/game/PlayerBots/Engine/tests/run_tests.sh

Current test coverage includes deterministic priority ordering, one action per channel, Possible/Useful rejection, missing action registration, reservation conflicts/expiry, non-finite priority rejection, snapshot cache freshness, registry identity and role-aware candidate generation.

These tests cover pure decision logic only. They do not validate live PartyBotAI integration, map-thread safety or vMaNGOS action execution.

## New Engine acceptance tests (planned)

- Values are cached, invalidated and refreshed at the expected epoch/cadence.
- Trigger intervals avoid repeated expensive scans while state transitions invalidate promptly.
- Action prerequisites, alternatives and continuers produce deterministic bounded queues.
- Every executor adapter reports the actual Core outcome (accepted, failed, interrupted, impossible or skipped).
- New Engine mode never calls Legacy class/role combat or out-of-combat policy.
- Missing behavior produces an explicit no-op and trace, never a Legacy fallback.
- Encounter traces can be parsed into one report and retain high-value events at event-budget limits.
- Coordinator reservations and arbitration behave correctly with staggered bot update ticks.
- Owner reconnect/instance persistence regression remains intact.

## Validation vocabulary

- Committed: source and documentation are present in the branch.
- CI passed: the latest relevant workflow run completed successfully.
- In-game tested: a scenario was exercised on a test realm.
- Deployed: the user explicitly approved and performed the intended server update.

A CI pass is never an in-game test and never authorizes deployment.


## Core-backed headless combat simulator

The simulator design and pinned-Core API audit are in CORE-COMBAT-SIMULATOR.md. This is intended as a third test layer:

1. pure decision-engine tests (fast, isolated policy logic);
2. Core-backed headless scenarios (real vMaNGOS damage/threat/target/death behavior, no client);
3. live realm tests (networking, client, lifecycle and long-duration behavior).

The initial audit confirmed a useful seam: `MapManager::CreateTestMap` / `DeleteTestMap`, public `Map::Add` / `Map::Update`, `Creature::Create`, `Unit::Attack`, `Unit::DealDamage`, `Unit::AddThreat` and `ThreatManager`. However, test-map construction depends on initialized global Core services (DBC/ObjectMgr, terrain, persistent state, world configuration and related managers). A standalone object-only test is insufficient.

The opt-in `PartyBot.Simulator.Enable = 1` mode runs a real Core map create/register/delete lifecycle check plus a Stage B actor/update/combat scenario, then exits through the normal WorldRunnable shutdown path. It refuses to start unless `WorldDatabase.Info`, `CharacterDatabase.Info`, `LoginDatabase.Info` and `LogsDatabase.Info` all point to schema names beginning with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`). Simulator mode also skips PID-file creation, realm-online/offline updates, account cleanup, mass-mail processing and all client-facing listeners. Set `PartyBot.Simulator.MapId` to a valid dungeon/raid map (default 33). Run `mangosd` with a disposable copy of all four DBs and the normal client data directory; it does not open the world, RA or SOAP listeners. A successful exit requires map registration/cleanup and the Stage B Core actor/update/damage/threat/target-switch/cleanup assertions. The startup audit found a candidate hook in `Master::Run`: immediately after `sWorld.SetInitialWorldSettings()`, before `WorldRunnable` and before the world listener is opened. Startup already performs DB writes and starts background workers, so simulator mode must use a dedicated disposable DB/configuration and reuse normal shutdown/cleanup; it cannot simply return early. `Map::Update(diff)` takes an explicit diff but still reads wall-clock/global services, so fixed diffs alone do not prove deterministic full-map ticks. The implementation must test or isolate those dependencies. The fixture-backed runtime test has now exercised the temporary Creature scenario; see the current status below. Do not connect the PartyBotEngine executor until the wrapper reports a clean end-to-end pass.


The Stage B source audit has now verified the temporary-Creature lifecycle contract:
- Create actors only from loaded `CreatureInfo` templates and explicit test positions.
- Call `Creature::AIM_Initialize()` before replacing the template AI. It initializes the `MotionMaster` as well as selecting AI. `Creature::SetAI` is a raw pointer setter; delete the prior AI explicitly before installing the simulator-owned probe AI.
- Do not tick the map before Stage B actor setup; the first meaningful map tick occurs after all simulator actors have valid movement generators.
- In an empty headless instance, ordinary non-active Creatures are not guaranteed to receive `Creature::Update`: `Map::UpdateCells` visits cells around players and active non-player objects. Mark simulator actors active before `Map::Add`, and assert the custom AI update counter advances after a tick. Respect the configured wall-clock cell-update cadence; do not count an empty map tick as an actor update.
- Check template-driven spawn spells: `Map::Add` invokes `Creature::CastSpawnSpell()` after insertion.
- Detach temporary actors with `Map::Remove(creature, false)`, then explicitly destroy them. Never use `remove=true` for these actors because that path can save respawn time.
- Remove/destroy every actor before `MapManager::DeleteTestMap`.

The first Core-backed scenario must report separate assertions for actor insertion/lookup, real `Unit::DealDamage` health/death effects, real threat references, `ThreatManager::getHostileTarget` selection and switching, custom `CreatureAI::UpdateAI` callback delivery, and cleanup. Calling a Core method directly is valid evidence for that method's semantics, but must not be presented as proof that a full autonomous combat loop or map tick executed it.


### Stage B Core Creature damage/threat smoke test (Core runtime assertions passed; runner marker fix pending)

The opt-in simulator now attempts a three-Creature scenario on its isolated test map. It chooses an ordinary non-NPC, non-civilian, non-pet normal-rank template with no immunities or spawn/script side effects, plus a mutually hostile faction-template pair from the loaded DB/DBC data. It creates a threat owner plus two hostile actors and verifies each actor is registered by `Map::GetCreature`. Each actor first runs `Creature::AIM_Initialize()` and then replaces the template AI with a probe AI before insertion. The actors are active objects; bounded real map ticks must increment all three AI update counters.

The damage sequence uses real `Unit::DealDamage`: target A deals 100, target B deals 105, then B deals 10 more to the threat owner. The assertions require health loss, damage-generated threat references, target A retained below the 110% melee threshold, and target B selected after crossing it. Logs include exact health/damage/threat values, selected GUIDs, AI update counts and cleanup results. The actors are detached with `Map::Remove(actor, false)`, their threat references are cleared, and map lookup must return null before explicit destruction.

This test uses Creature actors, not Player actors, and does not yet exercise spell casts, healing, movement, PartyBotEngine decisions/execution or encounter scripts. It is a smoke test of real Core damage/threat/update paths only. The bounded tick loop uses wall-clock pacing and is not yet a deterministic replay facility. The implementation passed full Linux ARM64 / Vanilla 5875 compile/link CI; this is build evidence only. The optional GitHub Actions runtime runner is implemented in `tools/partybot-simulator/run-ci.sh`. It runs only on trusted branch pushes and only performs runtime work when both Actions secrets are configured: `PARTYBOT_SIMULATOR_DATA_URL` (HTTPS Google Drive share URL to a tar.gz containing the full extracted-data tree: `5875/dbc`, `maps`, `vmaps`, `mmaps`) and `PARTYBOT_SIMULATOR_DATA_SHA256`. The runner validates those directories, starts the deployment's public vMangos database image, clones all four schemas to the required `partybot_sim_*` prefix, runs `mangosd` against those clones and uploads console/file logs for 14 days. It does not access the Oracle realm. If the secrets are not configured, the workflow explicitly skips runtime execution.

The first configured-fixture run exposed and fixed the empty-`MotionMaster` lifecycle bug. The next run confirmed the fix and passed the actual Core scenario: AI update counters `1/1/1`, health reduction, damage-generated threat, 110% target switch, three successful actor cleanups and map deletion. `mangosd` exited successfully. The workflow was marked failed only because the shell runner still matched the old final PASS string containing `ticked`; the Core log now correctly emits a final PASS line without that word. The runner now checks the actual combat, actor-cleanup and map-lifecycle PASS lines. A fresh end-to-end CI run is required to validate this harness correction.

The Core Creature update/damage/threat scenario has passed on disposable schemas and real 5875 data. Keep the overall CI gate open until the corrected shell marker checks also pass end-to-end.

Do not treat the existing pure tests as Core combat coverage. Do not use mocked damage/threat or direct state mutation as proof of Core behavior. The simulator must be bounded, seedable and explicit about remaining nondeterminism. It must not publish or deploy images.


## Threat-impact reservation coverage

The standalone C++14 suite in src/game/PlayerBots/Engine/tests/run_tests.sh now covers the P7.3c coordinator contract without constructing live Core objects: action fan-out across hostiles and actual threat owners, idempotent row refresh, aggregate pending threat, action completion cleanup, bot-owner cleanup when threat is attributed to a pet GUID, wrap-safe deadline expiry, invalid/expired input rejection, and the bounded 128-row fail-closed capacity. This validates only the reservation data structure and lifecycle; P7.3d still needs to connect these values to action eligibility and prove the result against real Core-generated threat.


## Threat budget policy and Engine admission

The standalone suite now exercises the P7.3d policy path as well as the P7.3c reservation container. Complete estimates are aggregated per hostile and actual threat recipient; admission subtracts existing in-flight reservations and higher-priority candidates from the same report. Tests cover multi-hostile fan-out, unknown and pet-only recipients, current-victim handling, stale epochs, execution-time reservation, failed-action rollback and no-selection when projected threat exceeds headroom. Positive complete estimates require a finite bounded reservation duration. This remains pure-policy coverage: Core runtime validation must still compare the decision to live ThreatManager values and prove no Core threat/victim mutation.


## Core-backed threat budget admission scenario

The combined production five-Player Group/two-add simulator fixture now includes a P7.3d probe in addition to the standalone policy tests. It creates a real small assist-threat reference through Core, reads the actual hostile/member entries with PartySnapshotBuilder, checks the 110% melee and 130% ranged projections, and runs an Engine candidate through rejection/selection. The accepted candidate creates a coordinator reservation; before any real damage is applied, the test asserts that neither Core ThreatManager values nor current victims have changed. It then applies the exact spell-less direct-damage input through Player::DealDamage, checks the real threat delta against the Core ThreatCalcHelper estimate and checks that Core still selects the Tank. Finally it releases the reservation. This source is build-validated in PR CI #44234198163; a push-only full-fixture runtime run is still required before this counts as runtime evidence.
