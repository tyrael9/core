# vMaNGOS API and update-context audit

## Baseline inspected

Repository: tyrael9/core, based on vmangos/core.
Implementation branch: work/partybot-engine-foundation.
Pinned deployment Core revision: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa.
Client: Vanilla 1.12.1 / build 5875.
Language: C++14.

The audit records existing APIs and integration seams; it is not yet a complete implementation specification.

## 1. Existing PartyBot update lifecycle

### Update cadence

PartyBotAI::UpdateAI(uint32 const diff) is the host update method. It advances m_updateTimer and returns until the timer passes. It then resets the timer to PB_UPDATE_INTERVAL (1000 ms) and begins a DecisionTrace tick. The constructor initially sets the timer to 2000 ms.

The method handles initialization, owner/group availability and reconnect grace, battleground/taxi handling, crowd-control/death recovery, current casts, food/drink, follow/teleport, out-of-combat behavior, target acquisition, movement/follow and finally combat behavior.

### Exact update call chain verified

The pinned source shows this path:

1. MapManager::Update schedules one DoUpdate task for each active instance Map object.
2. Map::DoUpdate calls Map::Update.
3. Map::UpdatePlayers iterates that map's player references and calls WorldObject::UpdateHelper::UpdateRealTime.
4. UpdateHelper invokes the virtual WorldObject::Update on the Player object.
5. Player::Update calls Unit::Update, then invokes m_AI->UpdateAI(p_time) when an AI is attached.
6. PartyBotAI::UpdateAI therefore runs as part of that Player's map update task.

Map::UpdatePlayers is called twice during Map::Update. The PartyBot's own ShortTimeTracker throttles its decision work to approximately one-second intervals, so do not assume every map player pass produces a PartyBot decision tick.

### Narrow combat seam

The current combat dispatch is:

- PartyBotAI::UpdateAI performs lifecycle and target/follow work.
- When the bot is in combat, or a healer is supporting same-map party combat, it calls PartyBotAI::UpdateInCombatAI().
- UpdateInCombatAI currently handles tank assist/taunt, marked CC and dispels, then switches to the per-class UpdateInCombatAI_<Class>() methods.
- UpdateOutOfCombatAI separately handles resurrection, CC/dispel and per-class out-of-combat behavior.

The first engine integration should preserve PartyBotAI::UpdateAI and intercept only the decision domains deliberately migrated from UpdateInCombatAI. Do not replace the lifecycle method. The tank-assist/taunt, CC and dispel prelude must be explicitly assigned to Legacy or Strategy; two decision authorities must never issue competing GCD actions.

## 2. Party and target APIs verified

Source files: src/game/PlayerBots/PartyBotAI.h/.cpp, src/game/Objects/Player.h, src/game/Group/Group.h, src/game/Maps/Map.h.

Verified APIs used by existing PartyBot code:

- Player::GetGroup() returns the current Group pointer.
- Group::GetFirstMember() returns a GroupReference iterator; source uses itr->getSource() to resolve a Player.
- Group::IsMember(ObjectGuid) checks whether the GUID remains in group member slots, including offline member slots.
- Group::GetLeaderGuid() returns the current leader GUID.
- Group::GetTargetWithIcon(RaidTargetIcon) returns the GUID associated with a raid target icon.
- Map::GetUnit(ObjectGuid) resolves a Unit on the current map; the declaration explicitly warns it should be used for objects on that map.
- Player::GetMap() and map/instance identity are available to scope party observations.
- Existing PartyBotAI::GetMarkedTarget and SelectAttackTarget use the group target icon and current Map::GetUnit resolution pattern.

Implementation rule: snapshots and reservations store GUIDs and scalar facts. Resolve GUIDs against the current map immediately before executing an action. Never retain GroupReference, Unit* or Player* across update epochs.

## 3. Hostile and threat APIs verified

Source files: src/game/Objects/Unit.h and src/game/Threat/ThreatManager.h.

Verified Unit APIs:

- Unit::GetAttackers() returns a const AttackerSet reference containing units currently attacking this unit.
- Unit::GetVictim() returns the unit this unit is currently attacking.
- Unit::GetThreatManager() exposes the Unit's ThreatManager.
- Unit::GetAttackerForHelper() can return the current victim or an attacker.

Verified ThreatManager/HostileReference APIs:

- ThreatManager::getThreat(Unit*, bool) reads threat for a unit.
- ThreatManager::getThreatList() returns a const ThreatList.
- ThreatManager::getCurrentVictim() returns the current hostile reference.
- HostileReference exposes getThreat(), getUnitGuid(), isOnline() and isAccessable().

Design implication: the first snapshot can use party member attacker sets for defensive target assignment. Threat-based tank decisions should use the actual hostile creature's threat list and distinguish offline/inaccessible references. Do not infer threat rank solely from the current victim pointer.

## 4. Spell eligibility and execution APIs verified

Source files: src/game/PlayerBots/CombatBotBaseAI.h/.cpp, src/game/Spells/Spell.h, plus calls from PartyBotAI.

Existing public CombatBotBaseAI methods include:

- CanTryToCastSpell(Unit const*, SpellEntry const*) const
- DoCastSpell(Unit*, SpellEntry const*) -> SpellCastResult
- SelectHealTarget(), SelectPeriodicHealTarget(), SelectDispelTarget()
- IsValidHealTarget(), IsValidHostileTarget()
- FindAndPreHealTarget()

CanTryToCastSpell currently checks:
- m_preventCasting;
- Player::IsSpellReady;
- Player::HasGCD;
- caster/target aura-state requirements;
- Spell::CalculatePowerCost and available health/power;
- target spell immunity;
- shapeshift cast restrictions;
- duplicate aura application;
- min/max range using the spell range store and combat distance.

It does not itself constitute a complete universal target validator: line of sight, friendly/hostile relationship, encounter safety, CC reservations and all effect-specific target restrictions must remain explicit action checks or be validated by the spell system.

DoCastSpell is the execution authority in the existing bot layer. It sets facing/target, removes mount aura when needed, calls Player::CastSpell and returns the actual SpellCastResult. That result is authoritative. A candidate target or successful eligibility check must never be reported as a successful cast.

The existing code also uses SpellEntry::IsTargetInRange and Player::IsWithinLOSInMap for specific actions. Include these where appropriate rather than assuming CanTryToCastSpell covers them.

## 5. Movement and attack APIs verified

Source files: src/game/Movement/MotionMaster.h and PartyBotAI.cpp.

Existing methods include:
- Player::Attack(Unit*, bool)
- PartyBotAI::AttackStart(Unit*) which attacks and calls BeginChasing
- MotionMaster::MoveChase(Unit*, ...)
- MotionMaster::MoveFollow(Unit*, distance, angle)
- MotionMaster::MovePoint(...)
- MotionMaster::MoveDistance(Unit const*, distance)
- MotionMaster::MoveIdle()
- Player::StopMoving(), MotionMaster::Clear(...)

Movement needs its own arbitration lane. A mechanic escape or forced reposition must not be canceled by routine chase/follow/facing behavior. Existing movement APIs can execute decisions, but the engine must not assume a Move* request succeeded merely because it was issued.

## 6. Map update and threading observations

Source files: src/game/Maps/MapManager.cpp, src/game/Maps/Map.cpp, src/game/Maps/Map.h, src/game/Objects/Object.h and src/game/Objects/Player.cpp.

MapManager::Update constructs one DoUpdate task for each active instance Map object and submits those tasks to the instance thread pool. Different instance maps can update in parallel. Each task calls DoUpdate on its own Map object, which calls Map::Update.

Map::UpdatePlayers iterates the map's player references. It creates a WorldObject::UpdateHelper for each Player and calls UpdateRealTime. UpdateHelper invokes the virtual object's Update method. Player::Update calls Unit::Update, then m_AI->UpdateAI(p_time), on that same map update path.

Consequences:
- PartyBots in the same Map object are updated within that map's update task, not as independent instance-map tasks.
- Different maps/instances may update in parallel.
- Coordinator state must be sharded by group identity plus map/instance identity.
- Do not add global static mutable party state or share raw pointers across map shards.
- The map update loop calls UpdatePlayers twice per Map::Update; PartyBotAI's independent one-second timer means bots may tick at different offsets.
- Snapshot cache epoch/expiry and reservation expiry must be explicit. Do not assume all bots tick on the same millisecond.

This establishes the map-level ownership boundary for the intended coordinator. The implementation must still avoid re-entrant updates and keep all shard mutation inside the owning map's update context.

## 7. Build integration verified

New game source files are registered in src/game/CMakeLists.txt. The existing PartyBot diagnostic source is explicitly listed there. Every new Engine .cpp must be added to that list; adding a header alone is not sufficient.

No dedicated PartyBot unit-test harness has been confirmed yet. Audit the repository's existing test targets before deciding whether to add focused standalone tests or a small deterministic harness.

## 8. Current reusable seams

- PartyBotAI::TraceDecision / DecisionTrace: bounded diagnostic path.
- PartyBotAI::GetRole and class/role classification helpers: current role facts can seed the first snapshot.
- PartyBotAI::GetMarkedTarget, SelectPartyAttackTarget and CombatBotBaseAI target validation: existing Vanilla behavior to preserve or deliberately replace.
- CombatBotBaseAI::CanTryToCastSpell and DoCastSpell: candidate eligibility and execution adapters.
- PartyBotAI::UpdateInCombatAI: narrow integration point after lifecycle handling.
- Group and Map APIs: same-map snapshot and GUID resolution.
- Unit::GetAttackers and ThreatManager: defensive target/threat observations.

These are integration seams, not a reason to keep the old class routines as the main decision engine.

## 9. Remaining audit work before first Strategy-mode action

- Confirm GroupReference iteration cannot be invalidated by snapshot collection; do not mutate group membership while iterating.
- Audit target validation and spell execution separately for heal, taunt, interrupt, CC, dispel, resurrection and AoE.
- Determine how to resolve hostile threat-list GUIDs safely and handle offline/inaccessible references.
- Identify existing test targets and the best focused harness for decision logic.
- Decide how a map-scoped coordinator is created, owned and destroyed without dangling state when Group or Map unloads.
- Define snapshot cache invalidation across the two player update passes and independently phased PartyBot timers.

No code should depend on guessed TrinityCore/AzerothCore APIs. Real Strategy-mode execution waits until these remaining items are closed.
