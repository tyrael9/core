# vMaNGOS API and update-context audit

## Baseline inspected

Repository: tyrael9/core, based on vmangos/core.
Implementation branch: work/partybot-engine-foundation.
Pinned deployment Core revision: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa.
Client: Vanilla 1.12.1 / build 5875.
Language: C++14.

This audit records existing APIs and integration seams; it is not yet a complete implementation specification.

## 1. Existing PartyBot update lifecycle

### Update cadence

PartyBotAI::UpdateAI(uint32 const diff) is the host update method. It advances m_updateTimer and returns until the timer passes. It then resets the timer to PB_UPDATE_INTERVAL (1000 ms) and begins a DecisionTrace tick. The constructor initially sets the timer to 2000 ms.

The method handles initialization, owner/group availability and reconnect grace, battleground/taxi handling, crowd-control/death recovery, current casts, food/drink, follow/teleport, out-of-combat behavior, target acquisition, movement/follow and finally combat behavior.

### Exact update call chain verified

1. MapManager::Update schedules one DoUpdate task for each active instance Map object.
2. Map::DoUpdate calls Map::Update.
3. Map::UpdatePlayers iterates that map's player references and calls WorldObject::UpdateHelper::UpdateRealTime.
4. UpdateHelper invokes the virtual WorldObject::Update on the Player object.
5. Player::Update calls Unit::Update, then invokes m_AI->UpdateAI(p_time) when an AI is attached.
6. PartyBotAI::UpdateAI therefore runs as part of that Player's map update task.

Map::UpdatePlayers is called twice during Map::Update. PartyBotAI's ShortTimeTracker throttles its decision work to approximately one-second intervals, so not every player update produces a PartyBot decision tick.

### Narrow combat seam

PartyBotAI::UpdateAI performs lifecycle and target/follow work. When the bot is in combat, or a healer is supporting same-map party combat, it calls PartyBotAI::UpdateInCombatAI(). That method currently handles tank assist/taunt, marked CC and dispels, then switches to per-class combat methods. UpdateOutOfCombatAI separately handles resurrection, CC/dispel and per-class out-of-combat behavior.

The first engine integration should preserve PartyBotAI::UpdateAI and intercept only deliberately migrated domains from UpdateInCombatAI. Tank-assist/taunt, CC and dispel must each be explicitly assigned to Legacy or Strategy; two decision authorities must never issue competing GCD actions.

## 2. Party and target APIs verified

Source files: src/game/PlayerBots/PartyBotAI.h/.cpp, src/game/Objects/Player.h, src/game/Group/Group.h, src/game/Maps/Map.h.

Verified APIs:
- Player::GetGroup() returns the current Group pointer.
- Group::GetFirstMember() returns a GroupReference iterator; source uses itr->getSource() to resolve a Player.
- Group::IsMember(ObjectGuid) checks whether a GUID remains in group member slots, including offline member slots.
- Group::GetLeaderGuid() returns the current leader GUID.
- Group::GetId() returns the group's runtime/database ID.
- Group::GetTargetWithIcon(RaidTargetIcon) returns the GUID associated with a raid target icon.
- Map::GetUnit(ObjectGuid) resolves a Unit on the current map; its declaration warns it should be used for objects on that map.
- Player::GetMap() and map/instance identity are available to scope party observations.

Group::Create assigns m_Id through ObjectMgr::GenerateGroupId(). Battleground groups have no normal persisted group ID (m_Id remains 0); the new coordinator should not use this path for battleground groups. A practical in-memory shard key is (group ID, map ID, instance ID), with group ID 0 excluded.

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

Threat-based tank decisions must use the actual hostile creature's threat list and distinguish offline/inaccessible references. Do not infer threat rank solely from current victim.

## 4. Spell eligibility and execution APIs verified

Source files: src/game/PlayerBots/CombatBotBaseAI.h/.cpp, src/game/Spells/Spell.h and PartyBotAI.

Existing public CombatBotBaseAI methods include CanTryToCastSpell(Unit const*, SpellEntry const*) const, DoCastSpell(Unit*, SpellEntry const*) -> SpellCastResult, heal/dispel target selection and validation helpers.

CanTryToCastSpell currently checks casting prevention, spell readiness, GCD, caster/target aura-state requirements, power cost, target spell immunity, shapeshift restrictions, duplicate aura application and min/max range.

It is not a complete universal target validator: line of sight, friendly/hostile relationship, encounter safety, CC reservations and effect-specific target restrictions remain explicit action checks or are validated by the spell system.

DoCastSpell is the existing execution authority. It sets facing/target, removes mount aura when needed, calls Player::CastSpell and returns the actual SpellCastResult. That result is authoritative. A candidate target or successful eligibility check must never be reported as a successful cast.

The existing code also uses SpellEntry::IsTargetInRange and Player::IsWithinLOSInMap for specific actions. Include these where appropriate rather than assuming CanTryToCastSpell covers them.

## 5. Movement and attack APIs verified

Source files: src/game/Movement/MotionMaster.h and PartyBotAI.cpp.

Existing methods include Player::Attack(Unit*, bool), PartyBotAI::AttackStart(Unit*), MotionMaster::MoveChase, MoveFollow, MovePoint, MoveDistance and MoveIdle, plus Player::StopMoving and MotionMaster::Clear.

Movement needs its own arbitration lane. A mechanic escape or forced reposition must not be canceled by routine chase/follow/facing behavior. Do not assume a Move* request succeeded merely because it was issued.

## 6. Map update and threading observations

Source files: src/game/Maps/MapManager.cpp, src/game/Maps/Map.cpp, src/game/Maps/Map.h, src/game/Objects/Object.h and src/game/Objects/Player.cpp.

MapManager::Update constructs one DoUpdate task for each active instance Map object and submits those tasks to the instance thread pool. Different instance maps can update in parallel. Each task calls DoUpdate on its own Map object, which calls Map::Update.

Map::UpdatePlayers iterates the map's player references, creates a WorldObject::UpdateHelper for each Player and calls UpdateRealTime. UpdateHelper invokes the virtual object's Update method. Player::Update calls Unit::Update, then m_AI->UpdateAI(p_time), on that map update path.

Consequences:
- PartyBots in the same Map object execute in that map's update task.
- Different maps/instances may update in parallel.
- Coordinator state must be sharded by group identity plus map/instance identity.
- Do not add global static mutable party state or share raw pointers across map shards.
- Map::UpdatePlayers runs twice per Map::Update; PartyBotAI's independent one-second timer means bots may tick at different offsets.
- Snapshot cache epoch/expiry and reservation expiry must be explicit.

### Important unresolved Group concurrency risk

Group::GetFirstMember()/GroupReference iteration is not protected by a mutex in Group.h. Group::Create assigns a stable runtime/database group ID, but the group object itself can be observed from more than one map update context if members are on different maps. Group membership changes may therefore race with a snapshot that walks the group member list unless the call path proves those operations are serialized or a safe roster snapshot API is added.

Do not treat the map-scoped coordinator key as proof that Group roster iteration is thread-safe. Before implementing shared roster snapshots, choose and document one of:
1. prove from the actual group/session mutation call paths that roster mutation cannot overlap the PartyBot snapshot read;
2. add a narrowly scoped synchronized Group roster snapshot API and ensure all relevant mutations use the same synchronization;
3. restrict the engine to a proven stable same-map roster and explicitly suspend it during cross-map membership transitions.

Do not add a lock around only the new read path; it would not synchronize with existing mutators that do not take that lock.

## 7. Build integration and tests

New game source files are registered in src/game/CMakeLists.txt. Every new Engine .cpp must be added there.

The repository tree contains no obvious dedicated unit-test directory or existing game unit-test target. Audit top-level CMake options and CI build conventions before adding a test target. The first decision layer should be designed so its pure candidate/priority/reservation logic can be tested without live Map/Player objects.

## 8. Current reusable seams

- PartyBotAI::TraceDecision / DecisionTrace.
- PartyBotAI::GetRole and class/role classification helpers.
- PartyBotAI::GetMarkedTarget, SelectPartyAttackTarget and CombatBotBaseAI target validation.
- CombatBotBaseAI::CanTryToCastSpell and DoCastSpell.
- PartyBotAI::UpdateInCombatAI as the narrow integration point.
- Group/Map GUID resolution, Unit::GetAttackers and ThreatManager.

These are integration seams, not a reason to keep old class routines as the main decision engine.

## 9. Remaining audit work before first Strategy-mode action

- Prove Group roster mutation serialization or implement a safe roster snapshot contract.
- Audit action-specific target validation for heal, taunt, interrupt, CC, dispel, resurrection and AoE.
- Determine how to resolve hostile threat-list GUIDs safely and handle offline/inaccessible references.
- Decide coordinator creation/destruction and snapshot cache invalidation across player update passes.
- Identify existing test targets and the best focused deterministic harness.
- Document what happens when bots split across maps or group membership changes during combat.

No code should depend on guessed TrinityCore/AzerothCore APIs. Real Strategy-mode execution waits until these remaining items are closed.
