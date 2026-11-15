# PartyBotEngine roadmap

This roadmap tracks the new decision engine. Legacy PartyBot fixes are maintenance work and do not count as engine milestones.

## Status key

- [x] Completed in the repository.
- [~] In progress / research or implementation underway.
- [ ] Not started.
- In-game validation and deployment are always tracked separately from code/CI.

## Phase 0 — baseline, lifecycle safety and observability

- [x] Pin the development branch to the exact deployed vMaNGOS baseline.
- [x] Add bounded opt-in DecisionTrace.
- [x] Add initial diagnostics for spell checks, healer target selection, Warrior assist/taunt and Mage AoE.
- [x] Fix healer lifecycle entry when same-map party members are fighting.
- [x] Add configurable owner-disconnect grace period.
- [x] Add ARM64 full-Core build workflow and manual regression checklist.
- [x] Fix pre-heal result reporting so success means a cast was accepted.
- [ ] Test owner disconnect/reconnect and instance persistence in-game.
- [ ] Test healer behavior and capture traces in-game.

## Phase 1 — source research and architecture

- [x] Identify and inspect the current CMaNGOS PlayerBots strategy engine.
- [x] Identify and inspect the active AzerothCore mod-playerbots repository and its raid strategy structure.
- [x] Document source findings, pinned upstream revisions and compatibility limits.
- [x] Define PartyCoordinator, PartySnapshot, PartyBotEngine and Legacy/Shadow/Strategy modes.
- [ ] Audit the exact vMaNGOS update, threading, object lifetime, group, threat, spell and movement APIs.
- [ ] Record the concrete vMaNGOS adapter map in ARCHITECTURE.md before implementation.

## Phase 2 — independent engine foundation

- [ ] Add C++14 Strategy, Trigger, Action, Value and Multiplier interfaces.
- [ ] Add a bounded per-party snapshot with GUID-based references.
- [ ] Add PartyCoordinator with expiring target/heal/CC/interrupt reservations.
- [ ] Add deterministic candidate evaluation and explicit action result states.
- [ ] Add Shadow mode; Legacy remains the only executor.
- [ ] Add unit-level or focused harness tests for eligibility, priorities, expiry and arbitration.
- [ ] Build on Linux ARM64 and supported CI targets.

## Phase 3 — first role vertical slice

- [ ] Select healing-triage-first or tank-threat-first after the API audit.
- [ ] Implement the chosen class/role strategy and its shared reservations.
- [ ] Implement the matching vMaNGOS action adapter.
- [ ] Add Strategy-mode execution for only that decision domain.
- [ ] Prove that Legacy does not issue competing GCD actions.
- [ ] Validate in-game with a real five-player party composition.
- [ ] Migrate the complementary healer/tank domain.

## Phase 4 — five-player group behavior

- [ ] Warrior threat and add pickup.
- [ ] Healing triage, heal reservations, HoT/Shield awareness and mana conservation.
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
- [ ] Ensure encounter actions can constrain class actions without duplicate execution.

## Phase 6 — Vanilla raid strategies

- [ ] Molten Core, encounter by encounter.
- [ ] Onyxia.
- [ ] Zul'Gurub.
- [ ] Blackwing Lair.
- [ ] AQ20.
- [ ] Validate Vanilla creature/spell data, vMaNGOS script behavior and group-size assumptions for every encounter.
- [ ] Consider AQ40/Naxx only after earlier raid support is stable; no commitment is implied.

## Explicit non-goals for early phases

- Replacing the vMaNGOS Player/session/group runtime.
- Rewriting all existing class AI in one change.
- Porting AzerothCore WotLK spell/encounter code.
- Adding external services, ML inference or a scripting runtime.
- Changing production databases or deploying without explicit approval.

## Current gate

The next coding gate is the vMaNGOS API/thread/lifetime audit. Do not add engine execution code until that audit identifies the actual call sites and safe ownership boundaries.
