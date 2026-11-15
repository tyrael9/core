# PartyBotEngine architecture

## 1. Goal

Implement a new, party-coordinated decision engine for the deployed Vanilla 1.12.1 (5875) vMaNGOS server. Preserve the existing PlayerBots runtime and execution APIs. Borrow the Strategy/Trigger/Action/Value/Multiplier decision model from CMaNGOS PlayerBots and encounter design from the active AzerothCore mod-playerbots project, but reimplement both against vMaNGOS.

The current PartyBotAI class-specific routines remain the Legacy implementation and fallback. The new engine is a separate decision authority; it is not a gradual renaming of the old rotations.

Research notes and source revisions:
- RESEARCH-CMANGOS.md
- RESEARCH-AZEROTHCORE.md

## 2. Layered architecture

    Existing PartyBot lifecycle / update host
                  |
           PartyCoordinator
          /       |        \
    PartySnapshot |   Shared reservations
          \       |        /
           PartyBotEngine (per bot)
                  |
       Strategies / Triggers / Values
                  |
        Candidate Actions + Multipliers
                  |
          Decision / Arbitration
                  |
        vMaNGOS Action Executor
                  |
       Existing Core spell/move APIs

Legacy PartyBotAI remains available beside this path until each behavior is validated.

### 2.1 Lifecycle host

The current PartyBotAI remains responsible for creation, update cadence, owner/group lifecycle, follow/teleport, resurrection, eating/drinking and other non-combat lifecycle behavior.

The new engine must not silently bypass these responsibilities. Integration begins at a narrow combat-decision seam. Do not replace the entire PartyBotAI update function.

### 2.2 PartyCoordinator

One coordinator is associated with the active party/group and its current map/instance context. It owns short-lived, GUID-based coordination state:

- selected primary target and optional secondary/add targets;
- main-tank and assist-tank assignments;
- crowd-control reservations (target, CC type, owner bot, expiry);
- healing reservations (target, estimated amount, healer, expiry);
- interrupt reservations (target, cast/spell, interrupter, expiry);
- dispel and resurrection assignments;
- encounter identity/phase and group recovery state.

Reservations are advisory and expire quickly. They are invalidated when the owner bot is dead, offline, out of the relevant map/instance, unable to act, or when the target is no longer valid. Explicit player commands and encounter safety rules take precedence over autonomous assignments.

The coordinator must not store raw Unit/Player pointers across updates. Store GUIDs and resolve them inside the current map/update context. Whether the coordinator can be shared directly across bot updates depends on the vMaNGOS map-thread audit; no cross-thread access is allowed without an explicit synchronization/ownership design.

### 2.3 PartySnapshot

Build a single read-only snapshot per coordinator update epoch, then expose a bot-specific view to each engine evaluation.

Initial snapshot fields:
- group identity and membership generation;
- bot GUID, class, level, role, alive/online state, health/mana, form, current cast and GCD/cooldown facts;
- each member's GUID, map/instance, alive/online state, health/mana, position/distance, role and current cast;
- hostile GUIDs attacking party members, current victim, health, distance, aura/control state and available threat facts;
- target validity, line of sight and spell/range facts required by active strategies;
- encounter identity, phase and relevant mechanic facts;
- CC marks and existing coordinator reservations.

Do not eagerly scan all nearby creatures or all auras. Snapshot fields are demand-driven by active strategy needs and cached for the update epoch. Expensive queries must be measured and bounded.

A snapshot is valid only for its update epoch. Raw pointers may be used transiently while building it, but the published snapshot should contain value data and GUIDs. Re-resolve GUIDs immediately before execution.

### 2.4 PartyBotEngine

Each bot has an engine instance that evaluates its own candidate actions against the shared party snapshot and coordinator state.

The engine owns:
- active strategies for class, role, combat state and encounter;
- trigger evaluation and trigger cadence;
- named values backed by the snapshot;
- action factories/registry;
- action candidate list and priority multipliers;
- decision trace and selected action result.

Strategies do not execute spells directly. They propose candidate actions with a target GUID, action parameters and priority. The executor performs final validation and invokes vMaNGOS.

### 2.5 Decision vocabulary

- Strategy: composable source of behavior (generic combat, class, role, party coordination, encounter).
- Trigger: a cached condition that proposes one or more actions.
- Action: one operation with explicit IsPossible, IsUseful and Execute/result phases.
- Value: a named, cached query over PartySnapshot/coordinator facts.
- Multiplier: a contextual priority adjustment or veto. Hard safety constraints remain explicit checks, not floating-point conventions.
- DecisionTrace: bounded explanation of candidates, rejection reasons, priorities, reservations and execution results.

CMaNGOS's prerequisite/alternative/continuer model is a useful optional action relationship. It should only be added where a concrete Vanilla behavior needs it; the first implementation should stay small.

## 3. Decision and execution rules

1. Build or reuse the current party snapshot.
2. Determine the current bot role and active encounter context.
3. Evaluate only active triggers whose cached dependencies changed or whose interval elapsed.
4. Generate candidate actions from generic, class, role and encounter strategies.
5. Reject impossible actions: unknown spell, cooldown/GCD, insufficient resource, wrong form/stance, invalid target, range/LOS failure, immunity, movement/cast restriction.
6. Reject useless actions: no meaningful health deficit, target already controlled, no threat to recover, no interrupt window, no mechanic requiring movement.
7. Apply party reservations and encounter safety constraints.
8. Compute deterministic priority and choose the best candidate.
9. Revalidate immediately before execution; execute through vMaNGOS.
10. Record success/failure and why alternatives were rejected.

A bot should execute at most one primary GCD action per decision cycle. Movement/positioning and target/facing operations are separate channels with explicit arbitration. A movement action must not repeatedly cancel another higher-priority movement action.

A failed action does not count as success merely because a target was found. Spell execution must preserve the actual vMaNGOS cast result.

## 4. Initial strategy composition

Every bot evaluates a composition rather than a monolithic rotation:

- Generic safety: stop attack/cast when required, avoid protected CC, escape dangerous mechanics.
- Class: abilities and class-specific constraints.
- Role: tank, healer, ranged DPS, melee DPS, support.
- Party: target assignment, healing/CC/interrupt reservations, pull and recovery state.
- Encounter: mechanic-specific constraints and phase behavior.

Encounter strategies can veto class actions or propose mechanic actions. For example, a Living Bomb-like mechanic may allow only the designated escape movement; an add damage shield may suppress attacks from non-tanks. The actual Vanilla mechanic and spell data must be independently verified before implementation.

## 5. First vertical slice

The first end-to-end slice should prove the architecture, not attempt every class:

1. A PartySnapshot with group member state and hostile attackers.
2. A PartyCoordinator with expiring target and heal reservations.
3. A PartyBotEngine with Strategy, Trigger, Action, Value and Multiplier interfaces.
4. A read-only Shadow mode that evaluates and traces decisions without executing them.
5. One real behavior migrated behind an explicit Strategy mode, initially healing triage/reservation or Warrior tank target/threat assignment.
6. Legacy fallback when the new engine is disabled, unavailable or rejects the feature.

The final choice between healing-first and tank-first is made after the vMaNGOS API audit and review of the current healer/Warrior execution seams. Do not begin a broad rewrite before this vertical slice is buildable and testable.

## 6. Rollout and fallback modes

The engine should support three conceptual modes:

- Legacy: current PartyBotAI class routines execute; default during development.
- Shadow: new engine evaluates and traces candidate decisions, but Legacy remains the only executor.
- Strategy: new engine owns the migrated decision domain; Legacy is not allowed to issue a competing action for that same domain.

Avoid two independent decision authorities issuing GCD spells in one update. Mode selection should be configurable and default to Legacy until the user has tested the new behavior. The exact configuration key and granularity will be chosen during implementation.

Feature migration is per decision domain, not necessarily per whole bot. For example, healer triage can move to Strategy mode while non-combat buffs remain in Legacy mode, provided the action domains do not overlap.

## 7. vMaNGOS API adapter audit

Before writing concrete actions, document the exact vMaNGOS methods and ownership rules for:

- PartyBotAI update and combat routine entry points;
- Group membership and leader/offline handling;
- Player/Unit GUID resolution and map/instance membership;
- threat list/victim/attacker enumeration;
- spell known/readiness/cooldown/GCD/resource checks;
- cast return codes and target validation;
- aura lookup and dispel eligibility;
- line of sight, range and movement;
- instance script state access.

Do not infer TrinityCore/AzerothCore method names. Do not introduce wrappers around APIs until the actual vMaNGOS declarations and call sites have been read.

## 8. Threading, lifetime and performance

The vMaNGOS threading model must be confirmed from this exact baseline before coordinator implementation. The design target is map-thread ownership: build, read and mutate a party's coordinator state only from the owning map/update context unless a proven cross-thread boundary requires synchronization.

No synchronous database operations belong in combat decision evaluation. No network or inference calls belong in the AI tick. Triggers must be cheap; group and nearby-unit scans should be centralized and cached. Snapshot and trace storage must be bounded.

## 9. Compatibility constraints

- Client/data: Vanilla 1.12.1, build 5875.
- Core base: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa.
- Language: C++14.
- Target deployment: Linux ARM64, Oracle A1, 1 OCPU / 6 GB.
- No external inference service.
- No DB schema changes for the first engine milestones.
- No changes to production deployment without explicit user approval.

Do not use C++17/20-only facilities from modern AzerothCore. Prefer explicit ownership and C++14-compatible value types.

## 10. Acceptance criteria

A milestone is complete only when:
- source and docs are committed together;
- the pinned Core builds on Linux ARM64 and supported CI targets;
- Legacy mode behavior remains unchanged;
- Shadow mode produces stable, bounded and explainable traces;
- Strategy mode has no duplicate GCD decisions with Legacy;
- positive and negative action paths are tested;
- party disconnect/reconnect and instance persistence remain intact;
- in-game testing is recorded separately from CI.

A CI pass is not an in-game test and is not deployment authorization.
