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
- ThreatManager::getThreat(Unit*, bool) reads threat for a unit and optionally searches the offline container.
- ThreatManager::getThreatList() returns the active online ThreatList; the offline container is private.
- ThreatManager::getCurrentVictim() returns the current hostile reference.
- HostileReference exposes `getThreat()`, `getUnitGuid()`, `isOnline()`, `isAccessable()`, `getTarget()`, `getSource()` and `next()`.
- Unit::GetHostileRefManager() exposes the unit-side `HostileRefManager`; its `getFirst()` chain links every hostile source that currently holds a threat reference to the unit.
- `HostileReference::getSource()->getOwner()` resolves the hostile source Unit; `ThreatManager::getThreat(Unit*, true)` includes offline references.
- `HostileRefManager::threatAssist` divides the supplied amount by the reference count unless `pSingleTarget` is set, then calls `addThreat(..., isAssistThreat=true)` for every linked hostile source. Healing/assist impact therefore spans the entire member-side reference list, not just the current attacker. Stage F12 runtime CI `#44215877767` proves PartySnapshotBuilder discovers a hostile through this member-side list after it has switched its victim to an outside-Group unit, and also copies the Tank's reference after Core moves it to the offline threat container (`getThreat(tank, false) == 0`, `getThreat(tank, true)` retains the value). `getThreat()` returns the reference's current threat value, including any temporary threat modification currently applied by taunt. `isOnline()` and `isAccessable()` are distinct: the header documents cases where a target is online but inaccessible to melee movement while still attackable by range.

`Unit::CanReachWithMeleeAutoAttack(Unit const*)` is a public Core API. The implementation delegates to `CanReachWithMeleeAutoAttackAtPosition` and uses the hostile's combat reach, target combat reach, `BASE_MELEERANGE_OFFSET`, minimum `ATTACK_DISTANCE`, leeway and melee Z reach. PartySnapshotBuilder now calls this API on the hostile Creature for each online threat reference and copies only the resulting boolean; Stage F11 runtime CI validates both true and false cases.

`ThreatManager::getHostileTarget()` updates the threat container and calls `ThreatContainer::selectNextVictim(attacker, currentVictim)`. The container sorts references by descending threat. The selector validates attack targets, immediately aborts selection for a target outside the threat area, and makes a first pass that skips low-priority targets (melee-school immune, secondary threat target and certain immobilized/chase-distance cases), followed by a fallback pass that allows those targets. Relative to a valid current victim, the top eligible challenger must exceed 1.3 times current threat to switch regardless of range, or exceed 1.1 times current threat while `CanReachWithMeleeAutoAttack(challenger)` is true. Both comparisons are strict `>` checks. If the current victim is invalid/removed or no longer eligible, this threshold comparison may not apply.

Threat-based tank decisions must use the actual hostile creature's threat list and distinguish offline/inaccessible references. Do not infer threat rank solely from current victim, and do not reimplement Core's entire victim selector in PartyBotEngine.

## 4. Spell eligibility and execution APIs verified

Source files: src/game/PlayerBots/CombatBotBaseAI.h/.cpp, src/game/Spells/Spell.h and PartyBotAI.

Existing public CombatBotBaseAI methods include CanTryToCastSpell(Unit const*, SpellEntry const*) const, DoCastSpell(Unit*, SpellEntry const*) -> SpellCastResult, heal/dispel target selection and validation helpers.

CanTryToCastSpell currently checks casting prevention, spell readiness, GCD, caster/target aura-state requirements, power cost, target spell immunity, shapeshift restrictions, duplicate aura application and min/max range.

It is not a complete universal target validator: line of sight, friendly/hostile relationship, encounter safety, CC reservations and effect-specific target restrictions remain explicit action checks or are validated by the spell system.

DoCastSpell is the existing execution authority. It sets facing/target, removes mount aura when needed, calls Player::CastSpell and returns the actual SpellCastResult. That result is authoritative. A candidate target or successful eligibility check must never be reported as a successful cast.

The existing code also uses SpellEntry::IsTargetInRange and Player::IsWithinLOSInMap for specific actions. Include these where appropriate rather than assuming CanTryToCastSpell covers them.

## 5. Movement, attack and action-adapter APIs verified

Source files: src/game/Movement/MotionMaster.h and PartyBotAI.cpp.

Existing methods include Player::Attack(Unit*, bool), PartyBotAI::AttackStart(Unit*), MotionMaster::MoveChase, MoveFollow, MovePoint, MoveDistance and MoveIdle, plus Player::StopMoving and MotionMaster::Clear.

### MoveDistanceAction — F43 sixteenth adapter (trusted runtime passed 2026-11-22)

Core MotionMaster::MoveDistance(Unit const*, float) computes a target-relative near point, rejects transport, destinations more than ten yards above the mover, destination LOS failure, zero-distance movement, PathFinder PATHFIND_NOPATH and non-normal shortcuts, then pushes DISTANCING_MOTION_TYPE. Core's MotionMaster stack semantics deliberately preserve a current Chase/Follow under Distancing and restore that exact generator when Distancing expires; this is distinct from ordinary targeted-generator expiry. The generic adapter requires a live current victim and a matching current Chase or party Follow backed by a current online/alive snapshot member. Trusted simulator #44998862867 (https://github.com/tyrael9/core/actions/runs/44998862867) proves Follow and Chase stack preservation/restoration, combat/threat state preservation, unrelated Point and victim-race rejection, cleanup and final Runtime PASS; PR ARM64 build #44998866883 (https://github.com/tyrael9/core/actions/runs/44998866883) passed.

### Own-cast cancellation — seventeenth adapter implemented; CI pending

The adapter uses `SpellCaster::GetCurrentSpell(CurrentSpellTypes)` and the value-only root correlation already carried on Core `Spell` objects. `PartySnapshotBuilder` now copies the current Spell's Engine action ID, encounter ID, trace target GUID and Engine-origin bit alongside its existing spell ID/state/container/Unit-target GUID. `CancelOwnCastAction` accepts only Generic or Channeled slots and requires exact equality against the live Spell's caster, spell ID, current slot, action ID, encounter ID, trace target and Unit-target GUID. The candidate is actor-only: the cast target is an expected identity fact, not a resolver target, so a stale/missing healing target cannot redirect or prevent cancellation of the bot's own cast.

Execution calls only `SpellCaster::InterruptSpell(slot, true)`. This canonical Core method calls `Spell::cancel()`, handles re-entrant cancellation, and clears the current-slot reference. The adapter never stores a Spell pointer in snapshots/candidates/queues, never calls `Spell::cancel()` directly and verifies after return that the original action ID is no longer current. The real-Core Player fixture starts two successive casts of the same spell and proves an old snapshot rejects the replacement action ID; a matching fresh snapshot cancels it, reports an Interrupted terminal and does not apply heal or mana effects. A repeated stale request after completion is rejected. The threat estimator returns NoThreat only for PREPARING; for DELAYED and CASTING it returns Unknown, and Engine arbitration must reject both. The simulator asserts those fail-closed decisions. Live preflight also compares the exact Core Spell state with the snapshot state, rejecting stale state transitions between estimate and execution. This is deliberate until delayed partial impacts and channel aura-removal/script/proc aftermath are fully enumerated. Live DELAYED/CHANNELED cancellation state matrices remain follow-up work. Trusted simulator and ARM64 results are pending.

Movement needs its own arbitration lane. A mechanic escape or forced reposition must not be canceled by routine chase/follow/facing behavior. Do not assume a Move* request succeeded merely because it was issued.

### F39 typed movement terminal implementation

The Engine-only `MotionMaster::MovePointForEngine` entry point carries action/encounter IDs and destination tolerance into `PointMovementGenerator<Player>`; ordinary `MovePoint` and legacy `PlayerAI::MovementInform` remain unchanged. `Update()` records natural spline completion. `Finalize()` emits `Reached` only when the final finite 3D distance is within tolerance, `Interrupted` on forced cleanup/cancellation and `Failed` if the spline naturally completes outside tolerance. PartyBotAI forwards the value-only result to the Engine's bounded event ledger and typed continuation source. Pure tests cover late registration and rejection of both Interrupted and Failed terminals when Reached is required; trusted Push CI #44795113292 proves natural arrival, continuation dispatch and forced ClearType interruption in the real Core Player fixture.

### F40 item-use lifecycle audit

`WorldSession::HandleUseItemOpcode` validates the item-use request and can bind the item before spell preparation. `Player::CastItemUseSpell` creates one Spell per valid ON_USE item spell slot; later slots are triggered and invalid spell IDs are skipped. Thus one item-use request fans out into multiple Spell lifecycles. `Spell::IsCastByItem()` exposes only a boolean, while `m_CastItem` is private and the existing PlayerAI observer has no cast-item GUID/root-request correlation. `TakeCastItem` can decrement charges or destroy expendable items at different points for delayed and immediate spells; `TakeReagents` may clear the cast item when it is also a reagent. A Spell terminal, effect application, charge mutation and item destruction are distinct outcomes. A future aggregate must carry root item-use identity plus per-spell child results and separately observe item mutation; no item-use aggregate source is implemented.

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

The implemented PartySnapshotBuilder follows this contract: it copies GUIDs from Group::GetMemberSlots(), resolves them through the current Map::GetUnit(), includes only current-map/current-instance players, and never stores Group, MemberSlot, Unit or Player pointers in the resulting snapshot. It also captures each member's current attackers and their threat-list entries. PartyBotAI calls `PartySnapshotBuilder::GetOrBuild` from the NewEngine map-thread update path before `ExecuteDecisionCycle`; the real five-Player Group fixture validates roster, scalar copies, cache reuse and teardown.

This conclusion relies on preserving the Core packet-processing classifications. If a future change reclassifies group mutation handlers as MAP or ASYNC, the snapshot safety contract must be revisited.

## 7. Build integration and tests

New game source files are registered in src/game/CMakeLists.txt. Every new Engine .cpp must be added there.

The repository has no dedicated game unit-test target. A standalone C++14 harness under Engine/tests covers pure policy/queue/coordinator semantics. Separately, the opt-in headless simulator constructs real pinned-Core Maps, Players, Groups, Creatures, spells, threat references and movement generators on disposable schemas; it verifies the production snapshot builder and selected Core integration paths. These fixtures are not authenticated production PlayerBots or live-realm tests.

## 8. Current reusable seams

- PartyBotAI::TraceDecision / DecisionTrace.
- PartyBotAI::GetRole and class/role classification helpers.
- PartyBotAI::GetMarkedTarget, SelectPartyAttackTarget and CombatBotBaseAI target validation.
- CombatBotBaseAI::CanTryToCastSpell and DoCastSpell.
- PartyBotAI::UpdateInCombatAI as the narrow integration point.
- Group/Map GUID resolution, Unit::GetAttackers and ThreatManager.

These are integration seams, not a reason to keep old class routines as the main decision engine.

## 9. Remaining action-adapter audit

The earlier Group roster serialization, same-map GUID resolution, hostile reference discovery, offline threat reference handling and PartySnapshotBuilder cache/lifecycle prerequisites are now implemented and covered by Core fixtures. They are no longer blockers for the first generic adapter.

Remaining audit work is action-specific:

- Spell heal, taunt, interrupt, CC, dispel, resurrection and AoE must each prove exact target relation, target state, range, LOS, immunity/control, resource, GCD/cooldown and resulting Core outcome.
- Threat estimates must remain fail-closed until the complete set of Core side effects, recipient fan-out, SpellScript/AuraScript/proc and triggered effects is enumerated. Current component parity fixtures do not make whole-action forecasts complete.
- Player point movement now has a typed terminal source on the opt-in `MotionMaster::MovePointForEngine` path. Natural spline completion plus destination tolerance emits Reached; forced cleanup emits Interrupted; natural completion outside tolerance emits Failed. Legacy MovePoint and MovementInform semantics remain unchanged. F39 pure and real-Core regressions are recorded in ROADMAP.md and TESTING.md.
- F40 completes the item-use lifecycle audit and proves one immediate potion consumption plus zero-charge rejection in the real Core simulator. A typed aggregate source for multi-spell fan-out, delayed impacts, cancellation and separately observed item mutation remains unimplemented.
- `MotionMaster::MovementExpired(false)` expires the active generator through Core's direct or deferred update-safe path, calls `Finalize`, deletes dynamic generators, and preserves the base generator without resetting it. For a top targeted generator, Core also removes stored lower CHASE/FOLLOW generators as part of its existing targeted-movement cleanup; non-targeted movement entries remain. By contrast, `MotionMaster::ClearType` finalizes and erases matching entries but currently does not delete dynamic generators, so new adapters must not use it for allocated movement generators. `StopFollowingAction` checks that the current native FOLLOW generator and target GUID match its value snapshot before calling `MovementExpired(false)`.
- Complete source/destination map and instance lifecycle closure remains open for all production teleport/script paths.
- Preserve the Core packet-processing classifications that make Group roster reads safe from map tasks; revisit the snapshot contract if group mutation opcodes are reclassified.

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

The Stage E runtime assertion requires a known Priest Lesser Heal (2050), a successful non-triggered Core cast result, a pending generic spell/GCD immediately after acceptance, subsequent cast completion, an actual target health increase and mana consumption. This validates one normal Player spell lifecycle. It does not establish production bot session authentication or production class/role adapter completeness. Separate simulator scenarios cover five-member Group behavior, movement/follow, interruption through Core's native relocation path and selected range/LOS/resource rejection cases; those do not substitute for per-adapter acceptance matrices.


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


The Core source condition for movement cancellation requires a Player cast with a nonzero timer, a position delta greater than 0.5 yards, and one of the spell's movement interrupt flags. The runtime fixture must prove that its selected spell actually has the relevant DBC flag before treating a no-heal outcome as expected. Lesser Heal (2050) currently completed after movement, so its data flags and effective cast time are being logged rather than assumed.


## 10. Item-use lifecycle audit — F40

Pinned source path:

- `WorldSession::HandleUseItemOpcode` in `src/game/Handlers/SpellHandler.cpp` validates the self-mover condition, item position/prototype, selected spell slot and `ITEM_SPELLTRIGGER_ON_USE`, equipment state, `CanUseItem`, trade state, combat restrictions, target validity and shapeshift restrictions. Binding-on-use can mutate the item before `Player::CastItemUseSpell` begins.
- `Player::CastItemUseSpell` in `src/game/Objects/Player.cpp` iterates all item prototype spell slots. Each valid ON_USE slot creates a separate `Spell` and calls `prepare`. Subsequent slots use the triggered flag; missing SpellEntry records are logged and skipped. One item request is therefore a fan-out, not a single Spell lifecycle.
- `Spell::IsCastByItem()` exposes only whether a cast item exists. The `m_CastItem` pointer is private in `Spell.h`; current PlayerAI spell observer callbacks receive Spell/action/spell/target data but no cast-item GUID or item-use root identity.
- `Spell::TakeCastItem` changes charges and may destroy expendable items. For delayed spells it is called before delayed impact; for immediate spells it is called after effects. `TakeReagents` can clear the cast item when the same item is also a reagent. Thus `Spell::finish(true)`, actual effect application, charge mutation and item destruction are distinct facts.

**Design consequence:** a typed item-use result must not equate one child Spell terminal with one item-use success or consumption. The future contract needs a root request/action ID, item GUID/entry/slot, per-spell child identities/results, explicit pre-cast rejection/prepare-failure handling and separately observed charge/count/destruction changes. Define aggregation for multi-slot fan-out, skipped slots, triggered children, delayed impacts and cancellation before adding the source or production adapter. Existing CoreSpellTerminal remains valid per-Spell evidence only.


## F41 CoreAction map-thread resolution boundary

`CoreAction` in `Engine/CoreAction.h` makes `Execute(DecisionContext&, ActionCandidate const&)` final. It requires a nonzero Engine action ID, an available `CoreActionRuntime`, a resolved current actor and map, and a resolved target whenever the candidate includes a target GUID. Only then does it call the adapter's `ExecuteCore` implementation. This prevents a derived adapter from accidentally omitting the common actor/target resolution gate.

The live `PartyBotAI::ResolveUnitContext` implementation resolves the actor and target from the current `Player::GetMap()` on the owning map thread. It validates the snapshot bot GUID/online bit, actor `IsInWorld`, current Map identity, Map ID and instance ID against both bot and party snapshot, active encounter shard scope, target presence on the actor's current Map, target `IsInWorld`, target Map identity/Map ID and target instance. It returns only a short-lived `CoreActionResolution`; raw Player/Unit/Map pointers must not escape the synchronous CoreAction call.

All four internal context-copy sites in `PartyBotEngine.cpp` must propagate `context.coreActionRuntime`: `Evaluate`, `ExecuteSelected`, `QueuePendingAction` and `ExecutePendingActions`. Omitting it is a fail-closed no-op at `CoreAction::Execute`, but can silently suppress real actions; F41 adds a pure fake-runtime regression covering both immediate and pending dispatch to prevent that regression. The trusted Core fixture separately proves a valid self-target resolves and missing-target/stale-map/stale-instance cases are rejected before the real movement adapter runs.

F41 is not the complete production executor contract. Each concrete adapter must still validate action-specific live state (alive/hostility/friendliness), range, LOS, known spell, resource, cooldown/GCD and movement/control state immediately before the side effect. No production class/role action adapters are registered yet.
