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


## Core-backed headless combat simulator (in development)

The simulator design and pinned-Core API audit are in CORE-COMBAT-SIMULATOR.md. This is intended as a third test layer:

1. pure decision-engine tests (fast, isolated policy logic);
2. Core-backed headless scenarios (real vMaNGOS damage/threat/target/death behavior, no client);
3. live realm tests (networking, client, lifecycle and long-duration behavior).

The initial audit confirmed a useful seam: `MapManager::CreateTestMap` / `DeleteTestMap`, public `Map::Add` / `Map::Update`, `Creature::Create`, `Unit::Attack`, `Unit::DealDamage`, `Unit::AddThreat` and `ThreatManager`. However, test-map construction depends on initialized global Core services (DBC/ObjectMgr, terrain, persistent state, world configuration and related managers). A standalone object-only test is insufficient.

**Stage A code is committed but not runtime-verified.** The opt-in `PartyBot.Simulator.Enable = 1` mode runs a real Core map create/delete smoke test, then exits through the normal WorldRunnable shutdown path. It refuses to start unless `WorldDatabase.Info`, `CharacterDatabase.Info`, `LoginDatabase.Info` and `LogsDatabase.Info` all point to schema names beginning with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`). Set `PartyBot.Simulator.MapId` to a valid dungeon/raid map (default 33). Run `mangosd` with a disposable copy of all four DBs and the normal client data directory; it does not open the world, RA or SOAP listeners. A successful exit proves only map registration/deletion and cleanup—not combat. The startup audit found a candidate hook in `Master::Run`: immediately after `sWorld.SetInitialWorldSettings()`, before `WorldRunnable` and before the world listener is opened. Startup already performs DB writes and starts background workers, so simulator mode must use a dedicated disposable DB/configuration and reuse normal shutdown/cleanup; it cannot simply return early. `Map::Update(diff)` takes an explicit diff but still reads wall-clock/global services, so fixed diffs alone do not prove deterministic full-map ticks. The implementation must test or isolate those dependencies. First run this Stage A smoke test, then add real temporary Creature actors and prove damage, threat ordering, victim reselection and actor/map cleanup; only then connect the PartyBotEngine executor.

Do not treat the existing pure tests as Core combat coverage. Do not use mocked damage/threat or direct state mutation as proof of Core behavior. The simulator must be bounded, seedable and explicit about remaining nondeterminism. It must not publish or deploy images.
