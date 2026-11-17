# PartyBotEngine roadmap

The product goal is a complete independent Vanilla 1.12.1 PartyBot AI implemented in PartyBotEngine. Development is not a per-role migration from Legacy. In New Engine mode, the engine is the only AI decision authority; missing behavior may produce a no-op but must never fall through to Legacy policy.

Legacy PartyBot fixes are maintenance work only. They do not count as new-engine milestones.

## Status key

- [x] Completed in the repository.
- [~] Partially implemented or in progress.
- [ ] Not started.
- CI, in-game testing and deployment are tracked separately.

## Phase 0 — pinned baseline and lifecycle safety

- [x] Pin development to the deployed vMaNGOS revision, Vanilla 5875 and C++14.
- [x] Preserve owner-disconnect grace and reconnect lifecycle behavior.
- [x] Verify owner reconnect within grace, intentional leave cleanup and expiry cleanup on the Oracle test realm.
- [x] Keep database schema and deployment data volumes unchanged.
- [ ] Continue explicit removal/disband and instance-transition regression where relevant.

These tests validate lifecycle-host behavior, not new combat AI behavior.

## Phase 1 — source research and Core integration audit

- [x] Inspect CMaNGOS PlayerBots Strategy/Trigger/Action/Value/Multiplier engine.
- [x] Inspect active AzerothCore mod-playerbots class and raid strategy design.
- [x] Document pinned source revisions and which ideas are architecture references rather than portable code.
- [x] Trace MapManager -> Map -> Player::Update -> PlayerAI::UpdateAI update path.
- [x] Audit same-map versus cross-map update ownership and Group roster read safety.
- [x] Audit initial group, target, attacker, threat, cast and movement APIs.
- [~] Continue action-specific audit for target validity, threat values, spell execution results, range/LOS and movement side effects.

## Phase 2 — independent engine foundation

- [x] Add C++14 Strategy, Trigger, Action, Value and Multiplier interfaces.
- [x] Add GUID/scalar-only PartySnapshot types.
- [x] Implement same-map snapshot builder and weakly indexed coordinator registry.
- [x] Add expiring party reservation primitives.
- [x] Add deterministic candidate evaluation, explicit eligibility results and basic channel arbitration.
- [x] Add standalone tests for decision ordering, invalid candidates, reservations, cache freshness and registry identity.
- [~] Complete production-grade value caching, trigger cadence and queue lifecycle.
- [~] Replace candidate-list-only arbitration with the required action/event queue semantics.

The current role-aware Shadow slice is a proof of the data-to-candidate path only. It is not a shipped behavior and does not justify further expanding coarse candidate-only rules.

## Phase 3 — first-class encounter trace and post-fight analysis

- [ ] Implement encounter identity and start/end detection.
- [ ] Add stable structured event schema with sequence numbers and schema version.
- [ ] Record party roster, role/state snapshots and bounded combat heartbeats.
- [ ] Record trigger results, candidates, rejection reasons, arbitration and selected action.
- [ ] Record action attempt and authoritative vMaNGOS result separately.
- [ ] Record target changes, threat samples, reservations, casts, health transitions and deaths.
- [ ] Protect high-value events from ordinary rate limits; emit explicit truncation/error markers.
- [ ] Keep tracing bounded and configurable for the 1 OCPU ARM64 host.
- [ ] Extend the read-only deployment diagnostics collector to produce one report/raw log bundle by encounter ID.

**Acceptance:** after a fight, the operator can run one read-only command and attach a report that makes it possible to distinguish perception, policy, coordination, eligibility, execution and encounter-model failures. No DB schema change is required.

Trace design: DECISION-TRACE.md.

## Phase 4 — complete decision loop and exclusive execution runtime

- [ ] Implement typed named Values with bounded cache, expiry and explicit invalidation.
- [ ] Implement Trigger check intervals/cadence and state-transition events.
- [ ] Implement action queue priorities, deterministic ordering and bounded processing.
- [ ] Implement prerequisites, alternatives, continuers and action result listeners where required by real Vanilla behavior.
- [ ] Implement useful/possible checks and real multiplier composition with hard safety vetoes.
- [ ] Implement one vMaNGOS action executor with audited adapters and structured results.
- [ ] Revalidate GUID, target state, map/instance, range, LOS, spell, resource and cooldown immediately before execution.
- [ ] Add explicit Legacy versus New Engine runtime authority selection.
- [ ] Prove New Engine mode does not call old class rotations, healing, tank assist/taunt, CC, dispel or target-selection policy.
- [ ] Make missing/unimplemented behavior a traceable no-op; no per-role or per-domain Legacy fallback.
- [ ] Add integration tests proving each action's Core outcome is recorded accurately.

**Acceptance:** a bot can run New Engine mode with no Legacy combat-policy calls. A deliberate no-op is preferable to hidden fallback.

## Phase 5 — generic behavior and complete bot state machine

- [ ] Idle, follow and formation behavior.
- [ ] Party preparation, class buffs and group utility.
- [ ] Food, drink, rest and resource recovery.
- [ ] Pull/engage, target acquisition and assist policies.
- [ ] Generic self-preservation, threat response and movement safety.
- [ ] Combat end, recovery, regroup and wipe handling.
- [ ] Death/release/resurrection integration with existing lifecycle facilities.
- [ ] Explicit state transitions, timeouts and trace explanations.

**Acceptance:** common behavior is owned by the Engine and does not depend on Legacy out-of-combat or lifecycle policy routines. Owner disconnect persistence remains host-managed and regression-tested.

## Phase 6 — full class and role strategy library

All classes and roles are composed within one PartyBotEngine queue; this is not a rollout boundary between old and new AI.

- [ ] Warrior: tank and damage strategies, stance/resource/threat/taunt logic.
- [ ] Priest: healing and damage strategies, shields, dispels and mana management.
- [ ] Druid: healing, caster and feral forms/roles, form-aware transitions.
- [ ] Shaman: healing and damage roles, totems, shocks and utility.
- [ ] Paladin: healing, tanking and damage roles, blessings/seals and utility.
- [ ] Mage: damage, AoE safety, control, interrupts and mana recovery.
- [ ] Warlock: damage, pet control, threat management, drains and curses.
- [ ] Hunter: ranged damage, pet control, traps, threat and range management.
- [ ] Rogue: melee damage, combo points, interrupts, control and survival.
- [ ] Shared class action definitions and class/role composition tests.

**Acceptance:** every supported class can enter New Engine mode; missing spell/behavior coverage is reported rather than delegated to Legacy. Gameplay may be rough during development.

## Phase 7 — party-wide coordination

- [ ] Main Tank and assistant assignments.
- [ ] Shared target selection and add pickup.
- [ ] Threat budgets, aggro prevention and threat recovery.
- [ ] Healing triage with incoming heals, HoTs, shields, effective healing and reservations.
- [ ] CC assignment, protection and break recovery.
- [ ] Interrupt, dispel and resurrection reservations.
- [ ] Pull sequencing, formation and group recovery.
- [ ] Cross-bot decision tests for races, stale data and conflicting actions.

**Acceptance:** party behavior is coordinated through one same-map snapshot/coordinator model, not independent bot guesses.

## Phase 8 — dungeon and raid encounter strategies

- [ ] Encounter registry and reusable phase/mechanic primitives.
- [ ] Five-player dungeons, boss by boss.
- [ ] Molten Core, encounter by encounter.
- [ ] Onyxia.
- [ ] Zul'Gurub.
- [ ] Blackwing Lair.
- [ ] AQ20.
- [ ] Validate creature entries, spell IDs, auras, timing, positioning and vMaNGOS script behavior against Vanilla data.
- [ ] Record support level, known limitations and test traces for each encounter.
- [ ] Consider AQ40/Naxx only after earlier encounters are stable; no commitment implied.

## Hard invariants throughout development

- Only one decision authority per bot at a time.
- New Engine mode never falls through to Legacy policy.
- No unsafe cross-map Core pointers or cross-thread coordinator mutation.
- No unbounded trigger scans, queues, caches, buffers or logs.
- No synchronous DB/network/inference work on map threads.
- Runtime safety, server stability and data/lifecycle integrity remain hard requirements even when experimental combat behavior is allowed to fail.
- Every meaningful decision/action failure should be diagnosable from bounded post-fight traces.
- A successful CI build is not an in-game test and never authorizes deployment.
