# vMaNGOS API and update-context audit

## Baseline inspected

Repository: tyrael9/core, based on vmangos/core.
Implementation branch: work/partybot-engine-foundation.
Pinned deployment Core revision: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa.
Client: Vanilla 1.12.1 / build 5875.
Language: C++14.

The audit below is against the pinned source tree. It records existing APIs and integration seams; it is not yet a complete implementation specification.

## 1. Existing PartyBot update lifecycle

### Update cadence

PartyBotAI::UpdateAI(uint32 const diff) is the host update method. It advances m_updateTimer and returns until the timer passes. It then resets the timer to PB_UPDATE_INTERVAL (1000 ms) and begins a DecisionTrace tick. The constructor initially sets the timer to 2000 ms.

The method handles, in order, initialization, owner/group availability and reconnect grace, battleground/taxi handling, crowd-control/death recovery, current casts, food/drink, follow/teleport, out-of-combat behavior, target acquisition, movement/follow and finally combat behavior.

### Narrow combat seam

The current combat dispatch is:

- PartyBotAI::UpdateAI performs lifecycle and target/follow work.
- When the bot is in combat, or a healer is supporting same-map party combat, it calls PartyBotAI::UpdateInCombatAI().
- UpdateInCombatAI currently handles tank assist/taunt, marked CC and dispels, then switches to the per-class UpdateInCombatAI_<Class>() methods.
- UpdateOutOfCombatAI separately handles resurrection, CC/dispel and per-class out-of-combat behavior.

The first engine integration should preserve PartyBotAI::UpdateAI and intercept only the decision domains deliberately migrated from UpdateInCombatAI. Do not replace the lifecycle method. Do not let Legacy and Strategy both issue GCD actions for the same domain. The tank-assist/taunt, CC and dispel prelude must be explicitly assigned to Legacy or Strategy; it cannot be left running as an uncoordinated second decision authority.

## 2. Party and target APIs verified

Source files: src/game/PlayerBots/PartyBotAI.h/.cpp, src/game/Objects/Player.h, src/game/Group/Group.h, src/game/Maps/Map.h.

Verified APIs used by the existing PartyBot code:

- Player::GetGroup() returns the current Group pointer.
- Group::GetFirstMember() returns a GroupReference iterator; the existing source uses itr->getSource() to resolve a Player.
- Group::IsMember(ObjectGuid) checks whether the GUID remains in the group's member slots, including offline member slots.
- Group::GetLeaderGuid() returns the current leader GUID.
- Group::GetTargetWithIcon(RaidTargetIcon) returns the GUID associated with a raid target icon.
- Map::GetUnit(ObjectGuid) resolves a Unit on the current map; the declaration explicitly warns it should be used for objects on that map.
- Player::GetMap() and map/instance identity are available to scope party observations.
- Existing PartyBotAI::GetMarkedTarget and SelectAttackTarget already use the group target icon and current Map::GetUnit resolution pattern.

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

Design implication: the first snapshot can use party member attacker sets for defensive target assignment. Threat-based tank decisions should use the actual hostile creature's threat list and must distinguish offline/inaccessible references. Do not infer threat rank solely from the current victim pointer.

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

DoCastSpell is the execution authority in the existing bot layer. It sets facing/target, removes mount aura when needed, calls Player::CastSpell and returns the actual SpellCastResult. That result is authoritative. A candidate target or a successful eligibility check must never be reported as a successful cast.

The existing code also uses SpellEntry::IsTargetInRange and Player::IsWithinLOSInMap for specific actions. These checks should be included where appropriate, not assumed to be covered by CanTryToCastSpell.

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

Source files: src/game/Maps/MapManager.cpp, src/game/Maps/Map.cpp and src/game/Maps/Map.h.

MapManager::Update constructs one DoUpdate task for each active instance Map object and submits those tasks to the instance thread pool. Different instance maps can therefore update in parallel. Each task calls DoUpdate on its own Map object, which calls Map::Update.

Map::Update performs player update traversals and active-cell updates on that map. Map::UpdatePlayers iterates that map's player references and updates players. The same map object is not submitted as multiple independent instance tasks in one dispatch. This provides a practical ownership boundary for same-map PartyBots, but it does not justify a single unsynchronized coordinator shared by party members on different maps.

Design consequence:
- Coordinator state must be sharded by group identity plus map/instance identity.
- Bots in the same map shard can share party decisions within that map's update context.
- Party members in different maps/instances must not share raw pointers or assume synchronous updates.
- Do not add global static mutable party state.
- Before relying on exact intra-map call ordering, finish tracing WorldObject::UpdateHelper and the Player AI update chain. The map scheduling model alone does not prove that every bot AI update occurs in the same sub-phase or at the same cadence.

A snapshot cache must account for the fact that PartyBotAI has an independent 1-second timer per bot. Do not assume all bots tick on the same millisecond. The cache epoch/expiry and reservation expiry must be explicit and tested.

## 7. Current reusable seams

- PartyBotAI::TraceDecision / DecisionTrace: bounded diagnostic path already exists.
- PartyBotAI::GetRole and class/role classification helpers: current role facts can seed the first snapshot.
- PartyBotAI::GetMarkedTarget, SelectPartyAttackTarget and CombatBotBaseAI target validation: existing Vanilla behavior to preserve or deliberately replace.
- CombatBotBaseAI::CanTryToCastSpell and DoCastSpell: candidate eligibility and execution adapters for the first iteration.
- PartyBotAI::UpdateInCombatAI: narrow integration point after lifecycle handling.
- Group and Map APIs: same-map snapshot and GUID resolution.
- Unit::GetAttackers and ThreatManager: defensive target/threat observations.

These are integration seams, not a reason to keep the old class routines as the main decision engine.

## 8. Remaining audit work before the first engine action

- Trace WorldObject::UpdateHelper through Player::Update and PlayerAI::UpdateAI to prove the exact call path and ownership context.
- Confirm whether GroupReference iteration can be invalidated by any operation performed during a decision tick; snapshots should avoid mutation during iteration.
- Audit target validation and spell execution for each action category (heal, taunt, interrupt, CC, dispel, resurrection, AoE).
- Determine how to read creature threat-list entries safely and resolve their GUIDs on the same map.
- Identify the exact CMake source registration for new Engine .cpp files.
- Identify existing unit-test infrastructure, if any; otherwise add focused deterministic tests without linking a full live map.
- Decide how a map-scoped coordinator is created, owned and destroyed without a dangling pointer when a Group or Map unloads.

No code should depend on guessed TrinityCore/AzerothCore APIs. The above list must be completed before Strategy mode executes real actions.
