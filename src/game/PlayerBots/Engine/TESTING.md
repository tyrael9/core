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

The first configured-fixture run exposed and fixed the empty-`MotionMaster` lifecycle bug. The next run confirmed the fix and passed the actual Core scenario: AI update counters `1/1/1`, health reduction, damage-generated threat, 110% target switch, three successful actor cleanups and map deletion. `mangosd` exited successfully. The workflow was marked failed only because the shell runner still matched the old final PASS string containing `ticked`; the runner was corrected to check the actual combat, actor-cleanup and map-lifecycle PASS lines. The later full fixture-backed push run `#44238072768` on `d23d865` passed the corrected runner and the complete simulator, including all P7.3d budget/race assertions.

The Core Creature update/damage/threat scenario and the complete Stage A-F12 simulator have passed on disposable schemas and real 5875 data in push run `#44238072768`; the final `Runtime PASS` marker was present and the runner completed successfully.

Do not treat the existing pure tests as Core combat coverage. Do not use mocked damage/threat or direct state mutation as proof of Core behavior. The simulator must be bounded, seedable and explicit about remaining nondeterminism. It must not publish or deploy images.


## Threat-impact reservation coverage

The standalone C++14 suite in src/game/PlayerBots/Engine/tests/run_tests.sh now covers the P7.3c coordinator contract without constructing live Core objects: action fan-out across hostiles and actual threat owners, idempotent row refresh, aggregate pending threat, action completion cleanup, bot-owner cleanup when threat is attributed to a pet GUID, wrap-safe deadline expiry, invalid/expired input rejection, and the bounded 128-row fail-closed capacity. This validates only the reservation data structure and lifecycle; P7.3d still needs to connect these values to action eligibility and prove the result against real Core-generated threat.


## Threat budget policy and Engine admission

The standalone suite exercises the P7.3d policy path as well as the P7.3c reservation container. Complete estimates are aggregated per hostile and actual threat recipient; admission subtracts existing in-flight reservations and higher-priority candidates from the same report. Tests cover multi-hostile fan-out, unresolved/duplicate/offline companion and missing-owner cases, primary Pet GUID threshold projection, Pet-targeted reservations with Player-owner cleanup, current-victim handling, stale epochs, execution-time reservation, failed-action rollback and no-selection when projected threat exceeds headroom. Positive complete estimates require a finite bounded reservation duration. Full-fixture generic admission/race coverage passed in push CI #44238072768; direct-heal assist upper-bound coverage passed in #44242344188; primary controlled Pet identity/budget/runtime cleanup passed in #44255823652.


## Core-backed threat budget admission scenario

The combined production five-Player Group/two-add simulator fixture includes the P7.3d budget/race checks and direct-heal-assist component validation. The generic path creates real assist-threat references through Core, reads hostile/member entries with PartySnapshotBuilder, checks 110% melee and 130% ranged projections, and evaluates over-budget and safe Engine candidates. It verifies no ThreatManager/current-victim mutation before damage, compares the Core damage delta with ThreatCalcHelper, tests execution-time reservation races and releases the action reservation. A second snapshot is built immediately before a real non-triggered Player Lesser Heal; the test estimates its direct-heal assist fan-out using current missing health, then compares each hostile's actual ThreatManager delta to its per-hostile upper bound. Push `#44242344188` passed with `upper=85.8/85.8`, `delta=14.5/14.5`. The helper also now enumerates a positive flat `spell_threat` row for the same healed target when Core's effect mask permits it, but this fixture's Lesser Heal has no nonzero flat entry; push `#44251387021` validates the effect-mask integration and full runtime regression only, not a nonzero flat-threat example. Both trusted pushes reached the full simulator `Runtime PASS` and uploaded logs.


Additional P7.3d regressions cover the gap between selection and execution: a competing bot reservation inserted after Evaluate must cause ExecuteSelected to revalidate and refuse execution. The bounded-capacity test starts with 127 unrelated rows and attempts a two-recipient reservation; the first partial row must be rolled back when the second cannot fit, leaving all 127 original rows intact.


## Primary controlled Pet threat snapshot and budget scenario

The production snapshot builder records the online primary Pet returned by each same-map Group Player as a separate companion value. The actual threat owner remains the Pet GUID. Its owner Player GUID is checked against the Group roster and is used only for logical-action cleanup; it is never substituted as the ThreatManager recipient.

The pure suite covers a valid Pet threat row using the Pet's own online/accessible/melee-reachability values and the Core 110%/130% threshold, plus duplicate companion rows, missing/offline owner, offline Pet, Pet current victim, Pet GUID reservation and Player owner ReleaseOwner cleanup.

The full F9 Core fixture creates a real temporary Pet(SUMMON_PET) owned by the Healer, registers it through the Core Pet/Map lifecycle, inserts a 30-point threat reference through Core, and rebuilds the normal five-Player Group snapshot. Assertions require petCreated=1, petSnapshot=1, petBudget=1, petThreat=30, petHeadroom=80, and petMelee=1; Pet is then detached after its HostileReference is removed and without saving or unsummoning persistent pet data. Trusted push CI #44255823652 on 295368c passed against the full Vanilla 5875 fixture, reached the simulator-wide Runtime PASS, and uploaded artifact partybot-simulator-295368c68574cd8f5e3c0e6de8e36aa21a5017e0.

Scope is limited to the primary controlled Pet. Guardians, totems, temporary summoned creatures and charmed units remain Unknown until equivalent owner/companion snapshot support exists.

## Running the full fixture-backed simulator

The PartyBot Core build workflow keeps Google Drive fixture secrets out of pull-request jobs. On this development branch, the full runtime, secret-presence checks and log-artifact upload are push-only. The workflow also declares workflow_dispatch for future use, but GitHub only exposes that manual trigger after the workflow file exists on the repository default branch; this Draft PR has not added it there. The job reports only whether the URL/checksum secrets are present, never their values; it downloads the archive, verifies SHA-256, runs disposable schemas and uploads simulator logs. It has no image-publish or deployment step.

## Encounter trace lifecycle and heartbeat coverage

The pure C++14 suite covers the shared encounter lifecycle and initial roster heartbeat contract without Core object pointers. It verifies process-unique encounter start, party-member/companion combat detection, duplicate start suppression, quiet-settle end, explicit close, uint32 clock wrap, and monotonic sequence allocation. Heartbeat regressions verify that a map/instance coordinator emits once for all bots, rejects invalid intervals, enforces the configured cadence, shares the encounter sequence, resets its gate for a new encounter and remains wrap-safe. The runtime Shadow integration logs one bounded `party_snapshot` line per active encounter interval; it caps members/companions/hostiles at 5/5/8 and includes explicit omitted counts plus a payload-truncated flag.

The heartbeat is an observational trace only. It does not execute actions, alter Core threat/victim state, retain Core pointers, or make the existing Shadow evaluation authoritative. Full bot-state, trigger/action lifecycle, threat samples, and post-fight collection remain separate Phase 3 work.

## Encounter-correlated Shadow decision trace coverage

During an active encounter, the read-only Shadow evaluation emits one bounded decision summary and at most three prioritized candidate records inside the existing per-bot ten-second detail window. Each record receives its sequence from the shared PartyCoordinator, so party heartbeat, summaries and candidates can be ordered under one encounter ID. Candidate fields include snapshot epoch, role, strategy/action, spell ID, action channel, target GUID, finite-priority validity, status and a token-safe reason. Correlated decision records are separately bounded by the ten-second Shadow window (one summary and at most three candidates) and therefore are not silently dropped by the ordinary per-tick legacy diagnostic quota. The event source is `PartyBotEngine::Evaluate` only; it does not call `ExecuteSelected` and is not evidence that a spell/movement action was attempted or accepted.

These records currently rely on the existing pure evaluation/cadence tests plus ARM64 compile/runtime regression. A dedicated log-format parser test and authoritative action-result correlation remain open.

