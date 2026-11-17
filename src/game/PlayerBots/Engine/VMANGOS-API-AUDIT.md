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

### Group roster read/write safety verified

The pinned Core explicitly documents the packet-thread contract in src/game/PacketProcessing.h:

- PACKET_PROCESS_MAP is safe to read and iterate Groups, but unsafe to write Groups.
- PACKET_PROCESS_GROUP is an alias of PACKET_PROCESS_WORLD and is allowed to read/modify/add/remove Groups.
- Group opcodes such as CMSG_GROUP_INVITE and CMSG_GROUP_ACCEPT are registered with PACKET_PROCESS_GROUP in src/game/Server/Protocol/Opcodes.cpp.
- Map::ProcessSessionPackets uses MapSessionFilter for map/spell/movement processing; group mutation handlers are not assigned to those map packet classes.
- World::UpdateSessions processes world packets before sMapMgr.Update schedules the map tasks.

This resolves the earlier concern for the normal group packet path: Group roster writes are serialized in the World packet phase, while map tasks may safely read/iterate Groups. PACKET_PROCESS_ASYNC can overlap map updates, but group mutation opcodes are classified as PACKET_PROCESS_GROUP, not ASYNC.

The implemented PartySnapshotBuilder follows this contract: it copies GUIDs from Group::GetMemberSlots(), resolves them through the current Map::GetUnit(), includes only current-map/current-instance players, and never stores Group, MemberSlot, Unit or Player pointers in the resulting snapshot. It also captures each member's current attackers and their threat-list entries. The builder is compiled but not yet called by PartyBotAI.

This conclusion relies on preserving the Core packet-processing classifications. If a future change reclassifies group mutation handlers as MAP or ASYNC, the snapshot safety contract must be revisited.

## 7. Build integration and tests

New game source files are registered in src/game/CMakeLists.txt. Every new Engine .cpp must be added there.

The repository has no dedicated game unit-test target. A standalone C++14 test harness exists under Engine/tests and is run by the PartyBot Core build workflow. It tests the pure decision layer, reservation amounts/expiry, snapshot cache age and coordinator shard identity without live Map/Player objects. The live snapshot builder is compile-tested by the full Core build.

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


## 10. Role and target snapshot adapter

The role-aware snapshot adapter reads `Player::AI()` on the current Map and uses `dynamic_cast<CombatBotBaseAI*>` before reading `GetRole()`. The role enum mapping is explicit: `ROLE_TANK`, `ROLE_HEALER`, `ROLE_RANGE_DPS` and `ROLE_MELEE_DPS`; other PlayerAI types and `ROLE_INVALID` remain Unknown. The Group's `GetMainTankGuid()` is copied separately into `PartyMemberSnapshot::isMainTank`.

Hostiles are copied from both each member's current `GetVictim()` and `GetAttackers()`. The builder rejects a Group member as a hostile, requires the Unit to belong to the current Map, and stores only GUID/scalar data. Threat-list GUIDs are resolved to snapshot members in the pure engine; current victim is preferred and threat is a fallback only when the victim does not resolve to an available group member.

This slice intentionally does not expose live Unit pointers to strategies, infer roles for human players, inspect pet attackers as a distinct entity, or treat the hostile set as a complete nearby-enemy scan. It also does not prove role assignments or target quality in-game; those require Shadow decision logs and operator comparison.


## 7. Spell execution audit — simulator Stage D

Additional source inspection for the pinned revision:

- \`SpellCaster::CastSpell(SpellCaster*, SpellEntry const*, bool triggered, ...)\` returns the authoritative \`SpellCastResult\`.
- \`SpellEntry::Effect[0..2]\` identifies the spell effect slots; the simulator validates that its configured spell includes \`SPELL_EFFECT_HEAL\`.
- \`Unit::GetHealth()\` / \`GetMaxHealth()\` expose the resulting target state. The simulator does not synthesize a heal by calling \`ModifyHealth\` or \`SetHealth\` after casting.
- The existing \`CombatBotBaseAI::DoCastSpell\` confirms the normal PlayerBot path uses \`Player::CastSpell(target, spell, false)\` and treats \`SPELL_CAST_OK\` as the authoritative immediate result. Its companion \`CanTryToCastSpell\` performs additional readiness, GCD, power, aura, immunity, shapeshift and range checks.

Stage D currently uses spell 2050 (Lesser Heal, Rank 1) with \`triggered=true\` on a same-faction simulator Creature. This makes the test deterministic and proves the Core Spell/SpellEffects heal path, but intentionally does not validate player spell ownership, normal mana/GCD/cast-time rules, interruption, or completion events. Those remain mandatory before production PlayerBot spell execution.


## 8. Player-backed non-triggered spell execution — simulator Stage E

Pinned Core source supports a test Player without inventing a parallel fake player type:

- \`Player(WorldSession*)\` initializes the real Player object and its session-backed menu; \`Player::Create\` initializes class/race data, stats, default spells and a Core player GUID.
- \`ObjectMgr::GeneratePlayerLowGuid\` supplies a Core-generated low GUID; the simulator uses \`Player::SelectRandomAppearance\` and the normal Player creation API.
- The fixture mirrors \`PlayerBotAI::SpawnNewPlayer\` for location, MotionMaster, packet broadcaster, map insertion, ObjectMgr cache and ObjectAccessor registration. It uses a local \`WorldSession\` with no socket and no production account; no DB character row is created.
- \`SpellCaster::IsSpellReady\`, \`HasGCD\`, \`GetCurrentSpell\` and non-triggered \`CastSpell\` are used to distinguish readiness, GCD, pending cast and completion.
- \`WorldSession::LogoutPlayer(false)\` performs normal in-world Player removal/deletion; the fixture checks the session's Player pointer is cleared before deleting the remaining Creature actor.

The Stage E runtime assertion requires a known Priest Lesser Heal (2050), a successful non-triggered Core cast result, a pending generic spell/GCD immediately after acceptance, subsequent cast completion, an actual target health increase and mana consumption. This validates one normal Player spell lifecycle. It does not yet establish production bot session authentication, Group membership, movement interruption, or all failure/rejection paths.


Stage E runtime note: \`Map::Update(uint32 t_diff)\` calls \`UpdatePlayers()\`, but that method compares the real \`WorldTimer::getMSTime()\` against \`m_lastPlayersUpdate\` and the configured player-update interval. The synthetic \`t_diff\` alone does not force a Player update. The simulator sleeps for a bounded 250 ms between map updates so the real monotonic cadence advances; the normal \`Player::Update -> Unit::Update -> EventProcessor\` path completes the cast.


## 9. Core movement adapter audit — simulator Stage F

- \`MotionMaster::MovePoint(id, x, y, z, options, speed, finalOrientation)\` is the pinned Core primitive for one-shot movement to a point.
- \`MOVE_NONE\` uses the Core \`PointMovementGenerator\` and \`MoveSplineInit::MoveTo\` without requiring the pathfinder/navmesh. The spline and resulting Unit position remain Core-owned; the simulator does not directly relocate the actor.
- The Engine adapter re-resolves mover and leader GUIDs on the test map, checks life, faction, map/instance, lost-control state and finite destination coordinates before invoking the movement generator.
- \`Unit::Update\` advances \`MotionMaster::UpdateMotion\`; the headless map fixture must mark Creature actors active and advance the real wall-clock map update cadence.
- Runtime CI verifies the movement generator becomes \`POINT_MOTION_TYPE\`, the actor reaches the snapshot-derived formation destination and both actors are detached before test-map deletion.

This proves only one point movement. Continuous \`MoveFollow\`, pathfinding, cast interruption on movement and defensive movement remain separate acceptance criteria.


Stage F2 confirms the pinned \`Spell::update\` movement-interrupt rule: while a non-triggered cast with \`SPELL_INTERRUPT_FLAG_MOVEMENT\` / moving-cancel semantics is pending, a real Player position change is detected against the cast-start position and Core cancels the spell. The simulator invokes only the real Engine movement action and waits for normal Player/Unit update cadence; it does not call \`InterruptSpell\` directly.


Stage F2 requires a priming Player update before casting because \`Map::UpdatePlayers\` computes its diff from the previous real wall-clock player update. The fixture ensures the first post-MovePoint update is a normal bounded tick, allowing \`Spell::update\` to observe the new position before the cast timer expires.
