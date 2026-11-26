# PartyBotEngine architecture

## 1. Goal and design authority

Build a complete, independent, party-coordinated AI for the deployed Vanilla 1.12.1 (client build 5875) vMaNGOS Core. Reimplement the decision architecture researched from CMaNGOS PlayerBots and the encounter-strategy ideas researched from active AzerothCore mod-playerbots against the actual vMaNGOS C++14 APIs.

The goal is not to incrementally patch or rename the existing class rotations. In the target runtime, PartyBotEngine is the sole authority for every AI decision it owns. It composes generic, class, role, party and encounter strategies into one decision loop.

Research and API boundaries:
- RESEARCH-CMANGOS.md
- RESEARCH-AZEROTHCORE.md
- VMANGOS-API-AUDIT.md
- DECISION-TRACE.md

## 2. Runtime authority: no hybrid AI

The runtime exposes two execution-authority modes plus a separate observe-only diagnostics mode.

### Legacy mode

The existing PartyBot implementation runs as it does today. This is retained for explicit rollback and controlled baseline comparison. It is not the target architecture.

### New Engine mode

PartyBotEngine owns all AI policy and action selection for the bot. No legacy class rotation, legacy healer routine, legacy tank/assist decision, legacy CC/dispell policy or legacy target selection may run as a fallback when the new engine has no candidate or cannot execute an action.

If a behavior is not implemented, the new engine may wait, defend through an implemented generic safety action, or report a deliberate no-op. It must not silently delegate to Legacy. This is an intentional development trade-off: incomplete behavior and poor combat performance are acceptable on the test realm; memory corruption, unsafe cross-thread access, unbounded work, server crashes and database/lifecycle corruption are not.

### Observe-only diagnostics

The current Shadow hook is a temporary development instrument. It may remain available to evaluate snapshots and policies without execution, or to compare a proposed decision against a live encounter. It is not a third mixed-execution mode and is not the end-state. Observe-only evaluation must never be mistaken for New Engine execution.

Runtime authority is implemented through `PlayerBot.EngineAuthority`: Legacy (0, default), Shadow (1, read-only Engine evaluation with Legacy execution), and NewEngine (2, exclusive Engine authority). NewEngine returns before Legacy follow/heal/combat/class policy. Unknown values fail safe to Legacy. Missing Engine behavior is a deliberate no-op, never a Legacy fallback.

## 3. Responsibility boundaries

### 3.1 PartyBotAI as the Core lifecycle host

PartyBotAI remains the vMaNGOS integration and lifetime host. It may handle:
- bot creation/removal and owner/group lifecycle;
- owner disconnect grace and reconnect persistence;
- safe map-thread update entry and map/instance transitions;
- initialization and cleanup of Core-owned resources;
- dispatching the current tick to the selected AI runtime.

It must not retain class-specific combat policy in the New Engine path. In particular, New Engine mode must not fall through to UpdateInCombatAI_*, old heal selection, old tank assist/taunt, old CC selection or old class-specific out-of-combat rotations.

The engine's eventual full behavior scope includes combat and non-combat decisions: following/formation, idle, pull/engage, class buffs, food/drink/rest, target selection, healing, threat, crowd control, interrupts, dispels, resurrection, recovery/regroup and encounter responses. Lifecycle mechanics such as preserving a bot across owner disconnect remain host responsibilities; deciding what the bot should do while following, resting or fighting belongs to the engine.

### 3.2 Shared Core primitives are allowed

New Engine may reuse audited vMaNGOS data and execution primitives: Player/Unit/Group/Map access, SpellStore, spell cast APIs, MotionMaster, threat manager, target validation, map relocation and existing low-level packet/core operations.

Reusing a low-level API or a safe utility is not reusing Legacy AI policy. Do not call a legacy method merely because it combines a useful Core operation with an old policy decision; extract or wrap the low-level operation behind an Engine-owned adapter after auditing its side effects.

## 4. Layered architecture

    PartyBotAI lifecycle / map-thread host
                  |
       PartyCoordinatorRegistry
                  |
       PartyCoordinator shard
          /       |         \
    PartySnapshot |   Reservations
          \       |         /
           PartyBotEngine (per bot)
                  |
       State + Strategy Composition
                  |
       Cached Values / Trigger Scheduler
                  |
       Trigger Events -> Action Queue
                  |
    Prerequisites / Alternatives / Continuers
                  |
     Possible / Useful / Multipliers / Arbitration
                  |
       New vMaNGOS Action Executor
                  |
       Authoritative execution result
                  |
       State/value invalidation + decision trace

One party snapshot/coordinator shard is shared by same-map bots. Each bot owns its active strategy composition and decision queue. Cross-map members are not dereferenced through retained Core pointers.

## 5. Strategy composition

A bot's active strategy set is composed from independent dimensions rather than selecting one AI implementation by role:

1. State strategies: dead, idle, follow, preparing, resting, pulling, combat, recovery and regroup.
2. Generic strategies: self-preservation, target validity, range, line of sight, resource safety and movement constraints.
3. Class strategies: class spell book, class resources, forms/stances, damage/healing/utility actions and class-specific constraints.
4. Role strategies: tank, healer, ranged DPS, melee DPS and support priorities.
5. Party strategies: shared target assignment, threat recovery, healing/CC/interrupt/dispel/resurrection reservations and formation.
6. Encounter strategies: boss/add identity, phases, abilities, debuffs, positioning and encounter-specific vetoes/actions.

Strategies may add, modify or veto candidates. A role is context and policy composition, not a separate executor or a migration boundary. A Priest Healer still has one queue and one final arbitration step for its generic, Priest, healer, party and encounter actions.

## 6. Decision cycle

The target decision loop follows the CMaNGOS Strategy/Trigger/Action/Value/Multiplier model, with vMaNGOS-owned value types and execution adapters.

1. Update context: obtain the current PartySnapshot, bot state and encounter state; refresh only invalidated or expired cached values.
2. Select state and strategies: determine the bot's state and active state/class/role/party/encounter strategies.
3. Evaluate triggers: run due triggers according to their check cadence; triggers should consume cached values and remain cheap.
4. Create events and actions: active triggers enqueue named actions with relevance, target and parameters.
5. Expand action nodes: prerequisites, alternatives and continuers express dependencies and fallback behavior explicitly.
6. Check Possible: spell known, target valid, alive/attackable, range, LOS, form/stance, cooldown, GCD, mana/resource, movement and Core restrictions.
7. Check Useful: evaluate whether the action advances the current goal; account for missing health, incoming heals/HoTs, threat risk, CC, interrupt windows, encounter mechanics and resource cost.
8. Apply multipliers and vetoes: adjust relevance for context; hard safety constraints remain explicit eligibility checks, not soft scores.
9. Coordinate and arbitrate: acquire/check reservations and select compatible actions across GCD, targeting, movement and utility channels. Party-wide actions must not be decided from isolated per-bot assumptions.
10. Revalidate and execute: resolve GUIDs on the current map and revalidate immediately before calling the vMaNGOS executor.
11. Consume result: distinguish accepted, failed, impossible, interrupted, cancelled and skipped results. Invalidate affected cached values and schedule next actions when appropriate.
12. Trace: record why the decision was made and what the Core actually did.

A candidate selection is not an execution. A successful target lookup is not a successful cast. Every action result must have an explicit outcome.

## 7. Action queue and ActionNode semantics

The current candidate vector and one-winner-per-channel arbitration are only the first skeleton. The complete engine should support:
- named actions with parameters and stable identity;
- relevance/priority and deterministic tie breaking;
- per-action Possible and Useful checks;
- prerequisites that must run before the requested action;
- alternatives when the original action cannot run;
- continuers after an action succeeds;
- action result listeners and event/result overrides where needed;
- duplicate/loop suppression and bounded queue processing;
- independent arbitration for GCD, targeting, movement and utility actions.

Do not implement the full CMaNGOS surface mechanically. Add a semantic only when a concrete Vanilla behavior needs it, while preserving the same clear decision lifecycle.

Current implementation slices:
- `ActionQueue` is a per-dispatch, report-index queue. It is bounded to 32 entries, orders by priority with stable FIFO ties and is drained by `ExecuteSelected` with an eight-action ceiling. It does not retain work across updates.
- `PendingActionQueue` is bounded (32 entries), Engine-owned and called from `ExecuteDecisionCycle` under NewEngine authority. It copies value-only candidate/owner data, uses action-specific bounded TTLs and stable FIFO identity, revalidates Possible/Useful/threat/multipliers/channels/context on every dequeue, and is cleared by encounter/party/map/instance/lifecycle reset.
- F30–F38 add immediate-plus-pending dispatch with one shared eight-action ceiling, channel and cumulative threat arbitration, 3-second fairness promotion, bounded Core Spell/ActionResult listeners, typed AND/alternative dependencies, continuation gating, wrap-safe expiry, idempotency tombstones and a separate ready backlog when the pending queue is full. Children always return through ordinary pending dispatch; Core callbacks never execute them directly.
- Typed movement/item terminal sources and production Core action adapters remain open. A queued candidate is never permission to retain or dereference a Core object.


## 8. Values and trigger scheduling

Value is the shared, named interface to decision facts. Values should be typed and cached for a bounded epoch or duration. Values must read from PartySnapshot/coordinator/Core adapters; they must not retain unsafe Core pointers across updates.

Trigger definitions include name, cadence/check interval, active predicate and action/event handlers. Triggers that depend on the same party facts must reuse the same cached value/snapshot rather than rescan the group or hostile set for each bot/trigger. State transitions and explicit invalidation can force refresh before the normal interval.

Implemented cache/scheduler foundation:
- `CachedValue<T>` is a named typed adapter over a per-bot, map-thread-confined `ValueCache`. Cache keys include C++ value type and snapshot epoch; entries expire at a wrap-safe TTL deadline and may be invalidated by name or all at once. Zero/ambiguous TTL metadata bypasses storage. The cache holds at most 128 entries and computes without caching when full rather than evicting live facts.
- `Trigger::GetCheckIntervalMs()` defaults to zero, preserving every-update evaluation. Nonzero intervals are gated by a bounded per-bot `TriggerScheduler`; explicit trigger/all invalidation can force an early poll. If the scheduler table is full, an untracked trigger is evaluated every update rather than starved.
- The Engine attaches its cache to evaluation and execution contexts. Existing Shadow triggers remain every-tick until a concrete strategy opts into a slower cadence. PartyBotAI explicitly invalidates all decision facts and trigger schedules on Core death/resurrection, spell prepare/finish, successful target switches, encounter start/end, owner reconnect/removal and party/map/instance coordinator changes. Coordinator/mode teardown clears the scheduler table; state events force an early next poll. Future state-machine transitions must add their own invalidation calls; the scheduler never guesses that a state change occurred.

## 9. PartySnapshot and coordinator

### 9.1 Snapshot contents

Build a read-only same-map snapshot containing, on demand:
- group/map/instance identity, roster generation and snapshot epoch;
- bot and party-member GUID, class, role, level, online/alive/combat state, health/mana, form/stance, position, target and cast state;
- hostile GUIDs attacking party members or currently engaged by them, current victim, health, threat references and relevant aura/control state;
- spell facts needed by active actions: known spell, GCD/cooldown, resource, range and LOS checks where audited;
- party assignments, CC marks, encounter identity/phase and active reservations.

Snapshots contain GUIDs and scalar facts, not live Player/Unit pointers. Resolve GUIDs on the current map immediately before execution. Unknown facts remain explicitly unknown; do not guess role, threat, distance, spell state or encounter phase.

### 9.2 Coordinator ownership

Coordinator shards are keyed by (groupId, mapId, instanceId). The registry is weakly indexed; PartyBotAI instances retain shared ownership of their current shard. Shard mutation is confined to its owning map update context. Cross-map transitions do not mutate a shard from the wrong map thread; old reservations expire naturally.

The existing audits of Group roster reads, packet processing, Map resolution, attackers and threat APIs remain the source of truth. Extend VMANGOS-API-AUDIT.md for each new adapter before relying on it.

## 10. New vMaNGOS Action Executor

The executor is the only boundary allowed to produce in-game side effects for New Engine decisions. It owns audited adapters for:
- cast spell / item / pet action;
- select target / assist / stop attack;
- attack start / taunt / threat recovery;
- movement / follow / formation / disengage;
- crowd control / interrupt / dispel / heal / resurrection;
- food, drink, rest, buff and non-combat utility.

Each adapter must return a structured result with the Core result code and a stable reason. It re-resolves target GUIDs, validates map/instance, alive/hostile/friendly state, range/LOS, spell state and resource conditions immediately before execution. It must not report success when a cast was only queued or a target was only found.

Executor operations must remain map-thread safe. No synchronous SQL, networking or external inference belongs on the AI update path.

## 11. Full behavior state model

The engine must eventually own behavior across the full bot session, not only combat rotations:
- Idle / follow / formation.
- Party preparation, class buffs and group utility.
- Food, drink, rest and resource recovery.
- Pull/engage, target acquisition and assist policies.
- Generic self-preservation, threat response and movement safety.
- Active combat and role/class actions.
- CC, interrupt, dispel, resurrection and emergency response.
- Combat end, recovery, regroup and wipe handling.
- Death/release/resurrection integration with existing lifecycle facilities.
- Owner disconnect and reconnect are lifecycle-host behavior, not a combat strategy.

The state machine must have explicit transition conditions, timeouts and trace events. Existing lifecycle persistence code remains separately tested; New Engine behavior must not weaken it.

## 12. Encounter strategies

Encounter behavior is a composable strategy layer, not hard-coded exceptions inside class rotations. Each supported encounter should document:
- Vanilla creature entries and spell IDs;
- phases and transition conditions;
- tank/add assignments and target priorities;
- auras/debuffs and response windows;
- movement/position constraints;
- interrupts, dispels, CC and immunities;
- recovery and wipe conditions;
- vMaNGOS script deviations verified in this exact build.

AzerothCore WotLK spell/encounter data is design reference only. Every behavior must be validated against Vanilla 1.12.1 data and the deployed Core.

## 13. Decision and encounter trace

Post-fight logs are a required development capability. Each encounter gets an ID and ordered event sequence. The trace records party/bot state, trigger evaluation, candidate eligibility and rejection, arbitration, selected action, actual Core execution result, target changes, threat samples, reservations, health/death events and encounter boundaries.

The trace must distinguish proposed, selected, attempted and accepted actions. It must be bounded and rate-limited without silently dropping action results, deaths or encounter-end summaries. Use structured single-line key/value events so the deployment diagnostics collector can produce an attachable post-fight report.

See DECISION-TRACE.md for the implemented encounter event schema and bounded sampling rules. The existing trace is useful for diagnosis but is not lossless: richer AI/boss state, broader arbitration detail, ordinary-member per-hit health deltas, complete cross-map closure and deployment-diagnostics integration remain open.

## 14. Threading, lifetime and performance

MapManager schedules instance maps as separate update tasks; Player::Update invokes its AI within map update context. Same-map bots can share a coordinator shard, while different maps may update in parallel.

- Keep shard mutation inside its owning map update context.
- Never retain Core object pointers in snapshots or cross-map state.
- Resolve GUIDs against the current Map just before execution.
- Avoid repeated group/hostile scans inside triggers.
- Keep trigger evaluation O(1) where possible by consuming cached values.
- Bound queues, caches, event buffers and logs.
- No database/network/inference work on map threads.
- Measure update cost on the 1 OCPU / 6 GB ARM64 target before enabling expensive trace levels.

## 15. Compatibility and operational constraints

- Client/data: Vanilla 1.12.1 / build 5875.
- Core base: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa.
- Language: C++14.
- Target: Linux ARM64, Oracle A1 (1 OCPU / 6 GB).
- No external inference service.
- No database schema changes unless separately justified and approved.
- No production deployment or server-side changes without explicit user approval.

## 16. Acceptance criteria

A New Engine milestone is complete when:
- source and design/testing documents are committed together;
- standalone tests cover positive, negative, stale-data and failure paths;
- the pinned Linux ARM64 / Vanilla 5875 full Core build passes;
- New Engine mode has one decision authority and no legacy policy fallback;
- the executor reports authoritative Core outcomes;
- trace records can reconstruct why an action was chosen and what actually happened;
- party/map threading and resource bounds are tested;
- owner reconnect/instance persistence regression remains intact;
- in-game failures are recorded and diagnosed separately from CI.

Experimental combat quality may be poor while features are being built. Runtime safety, thread safety, bounded work and lifecycle/data integrity remain hard requirements. A CI pass is not an in-game test or deployment authorization.

## 17. Current implementation status (2026-11-26)

Implemented and supported by pure tests and/or fixture-backed Core runtime, as stated in ROADMAP.md:

- C++14 Strategy/Trigger/Action/Value/Multiplier engine, typed per-bot ValueCache and TriggerScheduler, deterministic evaluation/arbitration, bounded immediate/pending dispatch, explicit continuations/dependencies and authoritative Core action results.
- GUID/scalar-only PartySnapshot, same-map five-Player Group/hostile/threat builder, shared map/instance PartyCoordinator, reservations, stable formation slots and value-only death/resurrection/cast/movement/item lifecycle events.
- Exclusive NewEngine authority with no Legacy policy fallback. The only production NewEngine behavior currently registered is narrow out-of-combat Group-leader Follow plus StopFollowing recovery; Legacy remains the default runtime mode.
- Core-backed simulator coverage for follow/formation, leader handoff, five-Player regroup, wipe/death/resurrection, generic action preflight, movement/item-use lifecycle, trace budgets and Priest direct-heal forecast/source audits.
- Priest Phase 6 has a substantial test-only vertical slice: target/rank selection, cross-Bot Heal reservations, direct-heal/HoT/absorb snapshots, bounded timeline, Core heal/damage ordering, proc/target-trigger paths, a representative nested child chain, pushback and interruption invalidation. F54k random HoT bounds are trusted-runtime validated; F54l adds value-only school-absorb pools and known-school damage matching.

Still gated:

- `PriestDirectHealAction` and the Priest Healing Strategy are not registered in production. Its whole-action threat estimate remains fail-closed/Unknown; the test-only execution fixture is not a production policy.
- F54l school-absorb prediction, deeper child-chain closure (F54m), complete Core ordering and the whole-action helpful-threat/proc/recipient envelope (F54n) must each close independently. F54k random HoT bounds have trusted-runtime evidence but remain diagnostic/test-only.
- Druid, Shaman, Paladin, Warrior, Mage, Warlock, Hunter and Rogue class/role strategies are not implemented as production policies.
- Broader party-wide target/threat/CC/interrupt/dispel coordination and Vanilla dungeon/raid encounter strategies remain future phases.
- The trace/reporting pipeline is bounded and useful; richer AI/boss state, broader arbitration detail, ordinary-member per-hit health deltas, cross-map closure and deployment-diagnostics integration remain explicit gaps.

NewEngine is an exclusive development mode, not a complete/playable combat AI. Missing behavior is a deliberate no-op, never Legacy fallback. CI and trusted simulator fixtures validate only the paths they execute; in-game quality and deployment remain separate gates.

## 18. Immediate next implementation sequence

1. F54k is trusted-runtime validated: keep the guaranteed floor in triage and both reachable bounds in the diagnostic timeline.
2. F54l: capture bounded school-specific School Absorb pools and apply them only to known-school predicted damage; exclude Mana Shield, charged Aura paths, scripted Aura paths, unknown schools and ambiguous order.
3. F54m: extend nested child/proc closure beyond the representative three-cast chain, with explicit depth/event limits and per-hostile threat parity.
4. F54n: close supported Core ordering plus helpful-threat, proc, script and recipient paths. Unknown remains a hard veto.
5. Register `PriestDirectHealAction` and the Priest Healing Strategy only after the full action threat estimate is Complete or NoThreat for the exact supported spell shape.
6. Extend the same action/trace/runtime contracts across remaining classes, then party coordination and encounter-specific Vanilla strategies.

No merge, image publication, production deployment or live-realm change is implied by these milestones.
