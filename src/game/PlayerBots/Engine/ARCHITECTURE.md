# PartyBotEngine architecture

## 1. Goal

Implement a new, party-coordinated decision engine for the deployed Vanilla 1.12.1 (5875) vMaNGOS server. Preserve the existing PlayerBots runtime and execution APIs. Borrow the Strategy/Trigger/Action/Value/Multiplier decision model from CMaNGOS PlayerBots and encounter design from active AzerothCore mod-playerbots, but reimplement both against vMaNGOS.

Research:
- RESEARCH-CMANGOS.md
- RESEARCH-AZEROTHCORE.md
- VMANGOS-API-AUDIT.md

The current PartyBotAI class-specific routines remain the Legacy implementation and fallback. The new engine is a separate decision authority, not a gradual renaming of old rotations.

## 2. Layered architecture

    Existing PartyBot lifecycle / update host
                  |
           PartyCoordinator shard
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

PartyBotAI::UpdateAI remains responsible for initialization, owner/group lifecycle, resurrection, follow/teleport, food/drink, battleground and other non-combat lifecycle behavior.

The narrow combat seam is PartyBotAI::UpdateInCombatAI(), called after lifecycle and target/follow work when the bot is in combat or a healer is supporting same-map party combat. Existing UpdateInCombatAI performs tank assist/taunt, CC and dispels before the per-class combat switch. Each migrated domain must explicitly replace or retain its Legacy behavior so two decision authorities never issue competing GCD actions.

Do not replace the entire PartyBotAI update lifecycle.

### 2.2 Coordinator identity and ownership

Coordinator shards are keyed by group ID, map ID and instance ID. Normal Group::Create assigns a runtime/database ID through ObjectMgr::GenerateGroupId(); battleground groups use group ID 0 and should remain on Legacy behavior unless a separate BG coordinator is designed.

The registry is process-local and weakly indexed: PartyBotAI instances retain shared ownership of their current shard, while the registry maps the scalar key to a weak_ptr under a short mutex. The shard is mutated only in its owning map update context. Expired weak entries can be pruned without retaining Group, Map, Player or Unit pointers.

The packet-processing audit confirms Group roster reads are permitted in map context; group roster writes are serialized in world packet processing before map tasks. The live builder copies member GUIDs from Group::GetMemberSlots() and resolves them only through the current Map. It does not iterate cross-map GroupReference Player pointers.

### 2.3 PartySnapshot

Build a read-only snapshot for a same-map coordinator shard and expose a bot-specific view to each engine evaluation.

Initial fields:
- group ID and membership generation;
- bot GUID, class, level, role, alive/online state, health/mana, form, current cast and GCD/cooldown facts;
- member GUID, map/instance, alive/online state, health/mana, position/distance, role and current cast;
- hostile GUIDs attacking party members, current victim, health, distance, aura/control state and available threat facts;
- target validity, line of sight and spell/range facts required by active strategies;
- encounter identity/phase and relevant mechanic facts;
- CC marks and existing reservations.

Verified APIs include Group::GetId, GetFirstMember()/GroupReference::getSource, IsMember, GetLeaderGuid, GetTargetWithIcon, Map::GetUnit, Unit::GetAttackers, GetVictim, GetThreatManager and ThreatManager::getThreatList. See VMANGOS-API-AUDIT.md.

Snapshots contain GUIDs and scalar facts, not pointers retained across updates. Resolve GUIDs on the current map immediately before execution. Do not eagerly scan all nearby creatures or auras; fields are demand-driven and cached for a bounded epoch.

### 2.4 PartyBotEngine

Each bot has an engine instance that evaluates candidate actions against the shared same-map snapshot and coordinator state.

The engine owns active strategies for class, role, combat state and encounter; trigger evaluation/cadence; named values backed by the snapshot; action factories/registry; candidate list and priority multipliers; decision trace and selected action result.

Strategies propose candidate actions with target GUID, parameters and priority. The executor performs final validation and invokes vMaNGOS.

### 2.5 Decision vocabulary

- Strategy: composable behavior source (generic combat, class, role, party coordination, encounter).
- Trigger: cached condition that proposes one or more actions.
- Action: one operation with explicit IsPossible, IsUseful and Execute/result phases.
- Value: named, cached query over PartySnapshot/coordinator facts.
- Multiplier: contextual priority adjustment or veto. Hard safety constraints remain explicit checks.
- DecisionTrace: bounded explanation of candidates, rejection reasons, priorities, reservations and execution results.

CMaNGOS prerequisite/alternative/continuer relationships are optional. Add them only when a concrete Vanilla behavior needs them.

## 3. Decision and execution rules

1. Build or reuse the current same-map party snapshot.
2. Determine role, combat state and active encounter context.
3. Evaluate active triggers using cached facts.
4. Generate candidate actions.
5. Check IsPossible: spell known, cooldown/GCD, resources, form/stance, target validity, range, line of sight and Core cast restrictions.
6. Check IsUseful: meaningful heal deficit, threat to recover, interrupt window, CC requirement or encounter mechanic.
7. Apply party reservations and encounter safety constraints.
8. Calculate deterministic priority and select the best candidate.
9. Revalidate immediately before execution and call vMaNGOS.
10. Record result and rejected alternatives in bounded DecisionTrace.

A normal decision cycle executes at most one primary GCD action. Movement and target/facing operations use separate, explicitly arbitrated channels. A failed cast is never reported as successful merely because a target was found.

## 4. Strategy composition

Every bot evaluates a composition:
- Generic safety.
- Class abilities and class constraints.
- Role: tank, healer, ranged DPS, melee DPS, support.
- Party: target assignment, healing/CC/interrupt reservations, pull and recovery state.
- Encounter: mechanic-specific constraints and phase behavior.

Encounter strategies can veto class actions or propose mechanic actions. Actual Vanilla mechanics and spell data must be independently verified.

## 5. First vertical slice

1. Bounded same-map PartySnapshot with group member state and hostile attackers.
2. PartyCoordinator shard with expiring target and heal reservations.
3. PartyBotEngine with Strategy, Trigger, Action, Value and Multiplier interfaces.
4. Read-only Shadow mode; no action executor calls.
5. One real behavior migrated behind Strategy mode, initially healing triage/reservation or Warrior target/threat assignment, selected after the remaining audit.
6. Legacy fallback when new engine is disabled or unavailable.

The first slice must prove the architecture, not attempt every class.

## 6. Rollout modes

- Legacy: current class routines execute; default during development.
- Shadow: new engine evaluates/traces candidates; Legacy remains the only executor.
- Strategy: new engine owns a migrated decision domain; Legacy cannot issue competing actions for that domain.

Migration is per decision domain. No Strategy mode becomes default before in-game validation.

## 7. Threading, lifetime and performance

MapManager schedules instance maps as separate DoUpdate tasks; Map::UpdatePlayers calls WorldObject::UpdateHelper; Player::Update calls Unit::Update and then m_AI->UpdateAI(p_time). Same-map bots execute within the owning map update context, while different maps may update in parallel.

A same-map shard is the ownership boundary for coordinator mutations. PacketProcessing.h permits Group roster reads in PACKET_PROCESS_MAP and classifies group writes as PACKET_PROCESS_WORLD; group opcodes are processed before map tasks. Build snapshots from GetMemberSlots() GUIDs and resolve through the current Map; never dereference cross-map Player pointers. Keep all shard mutation in the owning map context; never share mutable shard state across map shards.

PartyBotAI timers are independent. Snapshot cache epoch and reservation expiry cannot assume synchronized bot ticks. No synchronous DB work, network calls or inference calls belong in the AI tick. Triggers must be cheap; scans should be centralized and cached; storage must be bounded.

## 8. Compatibility constraints

- Client/data: Vanilla 1.12.1, build 5875.
- Core base: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa.
- Language: C++14.
- Target: Linux ARM64, Oracle A1, 1 OCPU / 6 GB.
- No external inference service.
- No DB schema changes for first engine milestones.
- No production changes without explicit user approval.

## 9. Acceptance criteria

A milestone is complete only when source/docs are committed together; pinned Core builds on Linux ARM64 and supported CI targets; Legacy behavior remains unchanged; Shadow mode is bounded and explainable; Strategy mode has no duplicate GCD decisions with Legacy; positive/negative paths are tested; owner reconnect/instance persistence remains intact; and in-game testing is recorded separately from CI.

A CI pass is not an in-game test or deployment authorization.

## 10. Current runtime integration

The Core-agnostic decision layer and live data adapter are now connected to PartyBotAI behind an opt-in Shadow flag:

- PartyCoordinatorRegistry is weakly indexed by (groupId, mapId, instanceId), uses a short registry mutex, and returns shared shard ownership.
- Each PartyBotAI retains its current coordinator shard and releases reservations when it changes shard, leaves a supported group, or disables Shadow mode. Cleanup mutates a shard only when it belongs to the current map update thread; stale reservations on an old map shard expire naturally rather than being mutated from another map's thread.
- PartySnapshotBuilder copies same-map member GUIDs and scalar state, plus hostile/attacker/threat facts. It resolves units only through the current Map and retains no Core object pointers.
- PartyCoordinator provides snapshot epoch progression, bounded-age cache lookup, explicit invalidation and reservation expiry/release.
- PartyBotAI calls GetOrBuild and PartyBotEngine::Evaluate from its map update context only.

`PlayerBot.EngineShadow` defaults to 0. When set to 1, the runtime builds/reuses the snapshot and calls the read-only Evaluate path. `ExecuteSelected` is never called from Shadow mode; Legacy remains the sole action executor. The hook skips battleground groups (group ID zero) and unavailable snapshots.

`PlayerBot.DecisionTrace=1` additionally emits a rate-limited `[PartyBotShadow]` summary and up to three `[PartyBotShadowDecision]` details per bot every ten seconds. Details identify role, strategy, target GUID, priority, selection status and reason. Candidate-only healer triage, tank peel and DPS assist strategies are registered; zero candidates may still be normal when no strategy trigger has an eligible live target.

## 11. Validation and next milestone

The standalone C++14 tests now cover decision arbitration and rejection states, reservation ownership/expiry, snapshot cache freshness including millisecond-counter wraparound, coordinator key identity, battleground bypass and concurrent registry lookup.

The dedicated Linux ARM64 / Vanilla 5875 workflow validates these tests plus a full Core configure/build/link. It does not prove live snapshot correctness or in-game behavior.

The first role vertical slice should start with healing triage: use the new same-map snapshot and shared reservation model to propose healer actions in Shadow mode, compare the proposals with Legacy behavior, and only later add an execution adapter behind a domain-specific Strategy gate. No Strategy-mode execution becomes active before separate in-game validation.


## Role-aware Shadow strategy slice

The live snapshot records each same-map PartyBot's actual `CombatBotBaseAI::GetRole()` as Tank, Healer, RangedDamage or MeleeDamage. A separate `isMainTank` flag comes from the Group main-tank GUID, so a human player can be recognized as Main Tank even when their combat role remains Unknown. Other non-PartyBot players remain Unknown rather than having their roles guessed.

The hostile set contains the union of (a) units currently attacking a same-map party member and (b) units currently targeted by a same-map party member. Each hostile record retains its current victim and copied threat references. Party references are resolved only through the snapshot; current victim is preferred, with the highest online/accessibile threat entry used as a fallback when the victim is absent.

Three candidate-only strategies are registered on each PartyBot's Shadow engine:
- `healing_triage`: proposes same-map living members below 90% health, ranking lower health first with emergency and tank weighting.
- `tank_target`: proposes living hostiles currently attacking a non-tank party member, prioritizing healers.
- `dps_target`: proposes current party targets, favoring the Main Tank's target, and also considers hostiles attacking party members.

These are coarse target/triage intents, not spell-level actions. They do not validate spell availability, line of sight, range, healing-over-time effects, incoming heals, threat thresholds, class-specific rotations or encounter mechanics. Candidate Actions return `Skipped` if ever executed; Shadow integration calls `Evaluate` only and continues to leave Legacy as the sole executor.

For each rate-limited summary, up to three `[PartyBotShadowDecision]` lines report role, strategy, action, target GUID, priority, selected status and arbitration reason. This provides candidate detail for live comparison without changing gameplay.
