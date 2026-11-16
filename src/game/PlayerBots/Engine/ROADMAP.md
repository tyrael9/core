# PartyBotEngine roadmap

Legacy PartyBot fixes are maintenance work and do not count as new-engine milestones.

## Status key

- [x] Completed in the repository.
- [~] In progress / research or implementation underway.
- [ ] Not started.
- In-game validation and deployment are tracked separately from code/CI.

## Phase 0 — baseline, lifecycle safety and observability

- [x] Pin development to exact deployed vMaNGOS baseline.
- [x] Add bounded opt-in DecisionTrace and initial class decision diagnostics.
- [x] Fix healer lifecycle entry when same-map party members are fighting.
- [x] Add configurable owner-disconnect grace period.
- [x] Add ARM64 full-Core build workflow and manual regression checklist.
- [x] Fix pre-heal result reporting so success means a cast was accepted.
- [ ] Test owner disconnect/reconnect and instance persistence in-game.
- [ ] Test healer behavior and capture traces in-game.

## Phase 1 — source research and architecture

- [x] Inspect CMaNGOS PlayerBots strategy engine.
- [x] Inspect active AzerothCore mod-playerbots and raid strategy structure.
- [x] Document upstream source revisions, findings and compatibility limits.
- [x] Define PartyCoordinator, PartySnapshot, PartyBotEngine and Legacy/Shadow/Strategy modes.
- [x] Trace MapManager -> Map -> Player::Update -> PlayerAI::UpdateAI update path.
- [x] Verify same-map vs cross-map update ownership.
- [x] Map group, target icon, attacker, threat, spell, cast and movement APIs.
- [x] Identify CMake source registration.
- [x] Verify Group roster reads are safe in map context via PACKET_PROCESS_MAP / PACKET_PROCESS_GROUP classification.
- [ ] Audit action-specific target validation and threat-list resolution.
- [ ] Decide coordinator destruction and snapshot cache invalidation.

## Phase 2 — independent engine foundation

- [x] Add C++14 Strategy, Trigger, Action, Value and Multiplier interfaces.
- [x] Add value-only PartySnapshot data types with GUID-based references.
- [ ] Implement the live same-map snapshot builder after resolving Group roster concurrency.
- [x] Add pure PartyCoordinator reservation storage with expiry/conflict/release behavior.
- [x] Add weakly indexed map/instance coordinator registry; PartyBotAI shared ownership hook remains pending.
- [x] Add deterministic candidate evaluation, channel arbitration and explicit action result states.
- [~] Evaluation is read-only and can support Shadow mode; runtime integration is not wired yet.
- [x] Add standalone deterministic tests for eligibility, priorities, expiry and arbitration.
- [ ] Build on Linux ARM64 and supported CI targets.

## Phase 3 — first role vertical slice

- [ ] Select healing-triage-first or tank-threat-first after remaining API audit.
- [ ] Implement the chosen class/role strategy and shared reservations.
- [ ] Implement the matching vMaNGOS action adapter.
- [ ] Implement Strategy-mode execution for only that decision domain.
- [ ] Prove Legacy does not issue competing GCD actions.
- [ ] Validate in-game with a real five-player party composition.
- [ ] Migrate complementary healer/tank domain.

## Phase 4 — five-player group behavior

- [ ] Warrior threat and add pickup.
- [ ] Healing triage, reservations, HoT/Shield awareness and mana conservation.
- [ ] CC target assignment and break protection.
- [ ] Interrupt, dispel and resurrection coordination.
- [ ] Pull, combat, recovery, wipe and regroup state.
- [ ] Mage target selection, AoE safety and interrupt behavior.
- [ ] Druid, Shaman, Paladin, Hunter, Warlock and Rogue strategies.
- [ ] Validate each behavior in-game; retain per-domain Legacy fallback until passed.

## Phase 5 — five-player encounter strategies

- [ ] Encounter registry, phase model and reusable mechanic primitives.
- [ ] Implement and validate dungeons individually.
- [ ] Record supported bosses, mechanics, limitations and test results.
- [ ] Ensure encounter actions constrain class actions without duplicate execution.

## Phase 6 — Vanilla raid strategies

- [ ] Molten Core, encounter by encounter.
- [ ] Onyxia.
- [ ] Zul'Gurub.
- [ ] Blackwing Lair.
- [ ] AQ20.
- [ ] Validate Vanilla creature/spell data, vMaNGOS scripts and group-size assumptions.
- [ ] Consider AQ40/Naxx only after earlier raid support is stable; no commitment implied.

## Current gate

The source research and map/update/API audit are committed. The remaining gates are the live snapshot builder, map/instance coordinator registry lifetime, action-specific validation and cache expiry. The packet-processing contract allows safe Group roster reads in map context; the builder will copy GUIDs from Group::GetMemberSlots() and resolve only on the current Map.


The live snapshot builder and coordinator registry are now compiled primitives. The next step is to attach a shard to each PartyBotAI, call GetOrBuild only from its map update context, and expose the read-only evaluation report in opt-in Shadow mode.