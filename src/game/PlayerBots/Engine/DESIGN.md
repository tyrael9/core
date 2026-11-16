# PartyBot Engine — design and implementation plan

## 1. Purpose

Build a dependable five-player PartyBot group for the Vanilla 1.12.1 (5875) server. The target is a small group controlled by one human player and a few PartyBots, with coordinated healing, tanking, damage, crowd control and instance mechanics. Correctness and debuggability matter more than maximizing the number of bots.

This is an incremental redesign of the decision-making layer, not a rewrite of the vMaNGOS player/session/group runtime.

## 2. Compatibility baseline

- Core revision: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa
- Deploy wrapper revision: ebaa560951459419c35140e20d787ec5db2905b8
- Client: 5875 (Vanilla 1.12.1)
- Language: C++14
- Target: Linux ARM64 (Oracle A1)

No external inference service, scripting runtime, DB schema changes or production deployment is part of the initial engine milestones. Keep existing PartyBotAI lifecycle and class-specific behavior available as fallback. Never deploy or modify the production realm without explicit approval.

## 3. Design principles

- Observe before changing decisions; every important rejection must have an explainable reason.
- One decision authority per action; avoid multiple class routines issuing conflicting GCD spells.
- Coordinate at party scope, using same-map shared state and explicit reservations.
- Separate decision from execution; strategies propose actions and vMaNGOS APIs execute them.
- Preserve lifecycle behavior: login/logout, resurrection, follow, teleport, eating/drinking, battleground and instance handling.
- Prefer deterministic rules, bounded traces and explicit tests over opaque heuristics.
- Build an independent engine; incremental patches to legacy rotations are maintenance, not the main implementation.

## 4. Reference research

The Strategy/Trigger/Action/Value/Multiplier model is researched in cmangos/playerbots. Encounter design is researched in the active mod-playerbots/mod-playerbots project, especially its Molten Core strategies. Both are architectural/behavioral references only; neither codebase is source-compatible with the deployed vMaNGOS baseline.

See RESEARCH-CMANGOS.md, RESEARCH-AZEROTHCORE.md, ARCHITECTURE.md and VMANGOS-API-AUDIT.md.

## 5. Target architecture

The target components are:
- PartyCoordinator: map/instance-scoped target, tank, heal, CC, interrupt, dispel and recovery reservations.
- PartySnapshot: bounded read-only view of party and relevant hostile state.
- PartyBotEngine: per-bot Strategy/Trigger/Action/Value/Multiplier evaluation.
- vMaNGOS Action Executor: final revalidation and execution through existing Core APIs.
- Legacy PartyBotAI: retained as fallback; no two decision authorities may issue the same action domain.

The exact interfaces, data ownership and rollout modes are specified in ARCHITECTURE.md.

## 6. Decision pipeline

1. Build or reuse the current same-map party snapshot.
2. Determine role, combat state and active encounter strategy.
3. Evaluate active triggers using cached facts.
4. Generate candidate actions.
5. Check IsPossible: spell known, cooldown/GCD, resources, form/stance, target validity, range, line of sight and Core cast restrictions.
6. Check IsUseful: meaningful heal deficit, threat to recover, interrupt window, CC requirement or encounter mechanic.
7. Apply party reservations and encounter safety constraints.
8. Calculate deterministic priority and select the best candidate.
9. Revalidate immediately before execution and call vMaNGOS.
10. Record result and rejected alternatives in bounded DecisionTrace.

A normal decision cycle executes at most one primary GCD action. Movement and target/facing operations use separate, explicitly arbitrated channels. A failed cast is never reported as successful merely because a target was found.

## 7. Owner lifecycle

A temporary owner disconnect is not an intentional group disband. When the original owner's GUID remains in the current group, retain PartyBots during the reconnect grace period, reset the timer on reconnect, restore owner leadership if needed, preserve explicit leave/disband cleanup and allow removal after grace expiry.

The initial default is 10 minutes, configurable with PlayerBot.OwnerOfflineGraceMs; 0 disables it. This behavior is committed but still requires in-game validation on a disposable realm.

## 8. Diagnostics

DecisionTrace is opt-in through PlayerBot.DecisionTrace = 1. Output is bounded per bot/update and does not write to the database. It records spell eligibility/cast outcomes, heal target rejection, party combat detection, role assignment, Warrior assist/taunt no-action cases, Mage AoE rejection and owner disconnect grace transitions.

Tracing is diagnostic evidence, not a substitute for tests.

## 9. Rollout

- Legacy: existing class routines execute; default during development.
- Shadow: new engine evaluates and traces candidates; Legacy remains the only executor.
- Strategy: new engine owns a migrated decision domain; Legacy cannot issue competing actions for that domain.

Migration is per decision domain. No Strategy mode becomes default before in-game validation.

## 10. Roadmap and next gate

See ROADMAP.md. The next gate is to finish the exact PlayerAI update call chain, coordinator ownership/lifetime and build/test harness audit. The first independent engine implementation follows that audit and starts in Shadow mode.

Planned order:
1. C++14 engine foundation and same-map snapshot/coordinator;
2. Shadow mode and deterministic tests;
3. one role vertical slice (healing triage/reservation or Warrior threat assignment, selected after audit);
4. remaining five-player role coordination;
5. five-player encounter strategies;
6. Molten Core, Onyxia, Zul'Gurub, Blackwing Lair and AQ20, encounter by encounter.

## 11. CI and validation

For head 9a3dae4de7e556858a2fdce13488d61a6c668f05, the dedicated Linux ARM64 full-Core build passed. The broad CI Build matrix had passed its Ubuntu GCC jobs and macOS/Windows Visual Studio jobs, while Ubuntu Clang and Windows MinGW jobs were still running in the latest observed attempts. Do not report the full matrix as passed until all jobs in the latest run complete successfully.

A CI pass proves compilation only. It does not prove in-game behavior, instance persistence, database compatibility beyond compilation or deployment readiness. Manual tests are in TESTING.md. Production deployment requires explicit approval.

## 12. Current implementation status

Committed Phase 0 groundwork:
- bounded opt-in DecisionTrace;
- healer lifecycle entry when same-map party members are fighting;
- owner disconnect reconnect grace period;
- Warrior/Mage diagnostic instrumentation;
- pre-heal helper returns success only when the cast is accepted;
- ARM64 build workflow and manual regression checklist.

Committed research and design:
- CMaNGOS strategy engine analysis;
- active AzerothCore raid strategy analysis;
- PartyBotEngine architecture and roadmap;
- first vMaNGOS API/update-context audit.

Not yet implemented:
- PartyCoordinator;
- PartySnapshot;
- independent Strategy/Trigger/Action/Value/Multiplier engine;
- Shadow/Strategy execution modes;
- migrated healer/tank decision domains;
- encounter strategies.

Do not describe current code as a completed new engine. Update this section and ROADMAP.md with every implementation milestone.


## 13. Independent engine foundation status

The pure C++14 foundation is now committed: value-only PartySnapshot types, PartyCoordinator reservation primitives, Strategy/Trigger/Action/Value/Multiplier interfaces, candidate evaluation and channel arbitration, plus standalone deterministic tests. Evaluate is read-only; ExecuteSelected is a separate explicit method.

No PartyBotAI runtime integration exists yet. The current code does not build a live snapshot, create map-scoped coordinator shards or execute vMaNGOS actions. The packet-processing audit confirms Group reads are safe in map context; the builder will use GetMemberSlots() GUIDs and current-map resolution.


## 14. Live snapshot/coordinator primitives

The current foundation now includes:
- value-only PartySnapshot data structures;
- a live builder that copies Group::GetMemberSlots() GUIDs and resolves only through the current Map;
- member health/resources/position/victim facts, hostile attackers and threat-list snapshots;
- weakly indexed coordinator shards keyed by group/map/instance, with per-bot shared ownership intended;
- bounded snapshot caching and monotonically advancing snapshot epochs;
- reservation expiry, conflict handling and additive multi-healer reservations;
- deterministic tests for registry identity and cache age.

The builder/registry are compiled but are not yet wired into PartyBotAI. No runtime Shadow hook or vMaNGOS action adapter is active yet.
