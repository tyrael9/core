# PartyBot Engine — design and implementation plan

## 1. Purpose

Build a dependable five-player PartyBot group for the Vanilla 1.12.1 (5875) server. The target is a small group controlled by one human player and a few PartyBots, with coordinated healing, tanking, damage, crowd control and instance mechanics. Correctness and debuggability matter more than maximizing the number of bots.

This is an incremental redesign of the decision-making layer. It is not a rewrite of the vMaNGOS player/session/group runtime.

## 2. Compatibility baseline and hard constraints

- Core revision: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa
- Deploy wrapper revision: ebaa560951459419c35140e20d787ec5db2905b8
- Client variant: 5875 (Vanilla 1.12.1)
- Target language level: C++14
- Target deployment architecture: Linux ARM64 (Oracle A1)

Hard constraints:
1. Keep the 1.12.1 client protocol, data and spell behavior.
2. Keep the existing PartyBotAI lifecycle and vMaNGOS session/group APIs.
3. No external inference service, scripting runtime or network service for combat decisions.
4. No database tables or character/world schema changes for the initial engine.
5. Keep the old class-specific AI available as a fallback until each replacement behavior passes in-game tests.
6. Keep changes buildable with the Core's C++14 toolchain and supported ARM64 environment.
7. Never deploy or modify the production realm without explicit user approval.

Long-lived engine code belongs in vmangos/core, not in the deploy wrapper's small build/runtime patch directory.

## 3. Design principles

- Observe before changing decisions; every important rejection must have an explainable reason.
- One decision authority per action; avoid multiple class routines issuing conflicting GCD spells in one update.
- Coordinate at party scope; bots must use the same party situation and shared assignments.
- Separate decision from execution; strategies propose actions and vMaNGOS APIs execute them.
- Preserve lifecycle behavior: login/logout, resurrection, follow, teleport, eating/drinking, battleground and instance handling.
- Prefer deterministic rules, bounded traces and explicit test cases over opaque heuristics.
- Build a real independent engine; incremental patches to legacy class rotations are maintenance, not the main implementation.

## 4. Reference research

The engine model is based on the Strategy/Trigger/Action/Value/Multiplier architecture in cmangos/playerbots. The encounter design model is informed by the active mod-playerbots/mod-playerbots repository, especially its Molten Core raid strategies. These are architectural/behavioral references only; neither codebase is source-compatible with the deployed vMaNGOS baseline.

See:
- [RESEARCH-CMANGOS.md](RESEARCH-CMANGOS.md)
- [RESEARCH-AZEROTHCORE.md](RESEARCH-AZEROTHCORE.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)

The active AzerothCore module is mod-playerbots/mod-playerbots. The similarly named azerothcore/mod-playerbots repository is an old 2021 fork and is not the current upstream reference.

## 5. Target architecture

The architecture is specified in [ARCHITECTURE.md](ARCHITECTURE.md). Its main components are:

- PartyCoordinator: party-scoped target, tank, heal, CC, interrupt, dispel and recovery reservations.
- PartySnapshot: one read-only, bounded snapshot of party and relevant hostile state per update epoch.
- PartyBotEngine: per-bot Strategy/Trigger/Action/Value/Multiplier evaluation.
- vMaNGOS Action Executor: final revalidation and execution through existing Core APIs.
- Legacy PartyBotAI: retained as fallback; no two decision authorities may issue the same action domain.

Conceptually:

    PartyCoordinator + PartySnapshot
                 |
           PartyBotEngine
       Strategies / Triggers
       Values / Actions / Multipliers
                 |
        Candidate arbitration
                 |
        vMaNGOS action executor

The engine must use GUID-based references across updates, keep snapshots immutable, and resolve objects again before execution. The exact thread/ownership model must be confirmed from the pinned vMaNGOS source before coordinator implementation.

## 6. Decision pipeline

1. Build or reuse the current party snapshot.
2. Determine role, combat state and active encounter strategy.
3. Evaluate active triggers using cached facts.
4. Generate candidate actions.
5. Check IsPossible: spell known, cooldown/GCD, resources, form/stance, target validity, range, line of sight and Core cast restrictions.
6. Check IsUseful: meaningful heal deficit, threat to recover, interrupt window, CC requirement or encounter mechanic.
7. Apply party reservations and encounter safety constraints.
8. Calculate deterministic priority and select the best candidate.
9. Revalidate immediately before execution and call vMaNGOS.
10. Record the result and rejected alternatives in bounded DecisionTrace.

A normal decision cycle executes at most one primary GCD action. Movement and target/facing operations use separate, explicitly arbitrated channels. A failed cast is never reported as a successful action merely because a target was found.

## 7. Party and owner lifecycle

A temporary owner network disconnect is not an intentional group disband. When the original owner's GUID remains in the current group:
- retain PartyBots and their current instance during the reconnect grace period;
- do not request immediate bot removal;
- reset the timer on reconnect and restore owner leadership if necessary;
- preserve normal cleanup for explicit group leave/disband/removal;
- allow cleanup after the configured grace expires.

The initial default is 10 minutes, configurable with PlayerBot.OwnerOfflineGraceMs; 0 disables the grace period. This behavior is committed but still requires in-game validation on a disposable realm.

## 8. Diagnostics and operational controls

DecisionTrace is opt-in through PlayerBot.DecisionTrace = 1. Output is bounded per bot/update and does not write to the database. It records spell eligibility/cast outcomes, heal target rejection, party combat detection, role assignment, Warrior assist/taunt no-action cases, Mage AoE rejection and owner disconnect grace transitions.

Tracing is diagnostic evidence, not a substitute for tests.

## 9. Rollout and fallback

Three conceptual modes are required:
- Legacy: existing class routines execute; default during development.
- Shadow: new engine evaluates and traces decisions; Legacy remains the only executor.
- Strategy: new engine owns a migrated decision domain; Legacy cannot issue competing actions for that domain.

Migration is per decision domain, not necessarily per whole bot. The concrete config key and granularity will be decided during implementation. No Strategy mode becomes default before in-game validation.

## 10. Roadmap

See [ROADMAP.md](ROADMAP.md). The immediate next gate is a source-level audit of vMaNGOS update cadence, threading, object lifetime, group, threat, spell and movement APIs. The first engine implementation must follow that audit rather than assuming TrinityCore/AzerothCore method names.

Planned order:
1. independent engine foundation and shared snapshot/coordinator;
2. Shadow mode and deterministic tests;
3. one role vertical slice (healing triage/reservation or Warrior threat assignment, selected after API audit);
4. remaining five-player role coordination;
5. five-player encounter strategies;
6. Molten Core, Onyxia, Zul'Gurub, Blackwing Lair and AQ20, encounter by encounter.

## 11. Validation and CI status

The ARM64 full-Core workflow has passed for implementation head 9a3dae4de7e556858a2fdce13488d61a6c668f05. The existing broad CI Build workflow for that head was still running/queued at the time this document was updated; do not describe the full matrix as passed until its latest run completes successfully.

A successful CI build proves compilation only. It does not prove in-game behavior, instance persistence, database compatibility beyond compilation, or deployment readiness. Manual regression cases are in [TESTING.md](TESTING.md). Production deployment requires explicit user approval.

## 12. Current implementation status

Committed Phase 0 groundwork:
- bounded opt-in DecisionTrace;
- healer lifecycle entry when same-map party members are fighting;
- owner disconnect reconnect grace period;
- Warrior/Mage diagnostic instrumentation;
- pre-heal helper returns success only when the cast is accepted;
- ARM64 build workflow and manual regression checklist.

Not yet implemented:
- PartyCoordinator;
- PartySnapshot;
- independent Strategy/Trigger/Action/Value/Multiplier engine;
- Shadow/Strategy execution modes;
- migrated healer/tank decision domains;
- encounter strategies.

Do not describe the current code as a completed new engine. Update this section and ROADMAP.md with every implementation milestone.
