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
- [~] Continue action-specific audit for target validity, threat values, spell execution results, range/LOS and movement side effects; F6/F7 validate Core range, resource, LOS and immobility rejection. F8-F10 establish the real five-Player Group, multi-add healer threat and death-reference recovery; F11 validates both Core melee-reachability states; F12 validates complete hostile discovery through member-side HostileRefManager links, including offline references. Remaining audits will feed the threat-budget and action-execution stages.

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

## Phase 2.5 — Core-backed headless combat simulator

- [x] Audit pinned-Core test-map, map-tick, creature-construction, damage, attack and threat APIs.
- [x] Confirm `MapManager::CreateTestMap` / `DeleteTestMap` and public map add/update APIs.
- [x] Locate the hook: `Master::Run` after `SetInitialWorldSettings` and before `WorldRunnable` / world-listener startup.
- [x] Add opt-in `mangosd` simulator mode after Core initialization; skip world/RA/SOAP listeners, realm-online updates and daemon/PID-file behavior, then reuse normal WorldRunnable shutdown.
- [x] Refuse simulator startup unless all four configured DB schema names use the explicit `partybot_sim_` prefix (configurable).
- [x] Add a real `CreateTestMap` / `DeleteTestMap` lifecycle check with registry/cleanup assertions (map ticking is exercised after Stage B actors are fully initialized).
- [x] Execute Stage A/Stage B against disposable DB copies and real 5875 data; observe clean Core shutdown.
- [x] Prove real Core damage, damage-generated threat, threat ordering and victim reselection with runtime Core-state assertions.
- [~] Bound map tick progression and document wall-clock cadence; deterministic RNG/replay control remains.
- [x] Add a Stage B push-only CI runtime job with strict timeout and no deployment/image-publish side effects.
- [x] Connect PartyBotEngine to a fixture-backed five-member party snapshot and a real Core Unit::Attack executor; runtime CI verifies selection, execution result, actual victim and cleanup.
- [x] Add a spell-aware healer-triage action using the real Core Spell/SpellEffects pipeline; runtime CI verifies a triggered Lesser Heal effect on a Creature fixture.
- [x] Add a Player-backed healer fixture using a real WorldSession/Player object and ordinary non-triggered Lesser Heal. Runtime CI verifies known-spell ownership, Core acceptance, pending cast/GCD, mana consumption, cast completion, real healing and logout cleanup. This is a simulator Player, not yet an authenticated production PlayerBot.
- [x] Add a Core PointMovementGenerator formation action selected by PartyBotEngine; runtime CI verifies the movement generator starts, the real position reaches the snapshot-derived destination and both actors are cleaned up.
- [x] Validate movement interruption against the native Core Player relocation path. The Engine-selected simulator action models inbound mover relocation with `Player::SetPosition`, which owns the movement-interrupt policy; runtime CI `#44145923142` proves the pending cast is cancelled and patient health remains unchanged (`50000>50000`), with movement completion and cleanup.
### Stage F2 investigation history (superseded by the passing native-path runtime test)

- Early attempts exposed that clearing the generic spell slot manually was not a valid proof of the native movement-interrupt path; those manual adapter-side experiments were removed.
- Source audit distinguished inbound client relocation (`WorldSession::HandleMoverRelocation` → `Player::SetPosition` → `Unit::HandleInterruptsOnMovement`) from server-driven point movement (`Unit::UpdateSplineMovement`, later in the Unit update order). The final F2 fixture now exercises the inbound relocation seam without implementing spell-interruption policy in the simulator.
- Final runtime verification: CI `#44145923142`, commit `cb5be3d`, passed with `castCancelled=1`, patient health unchanged (`50000>50000`), movement completion and cleanup.

- [x] Validate the initial native Core follow behavior: PartyBotEngine selects `MoveFollow`, and the Core `FollowMovementGenerator` tracks the Main Tank after it relocates. Runtime CI `#44153619126` reports `initialCaughtUp=1`, `leaderReached=1`, `followerTracked=1`, final separation `1.87`, active follow generator and cleanup.
- [x] Validate Player follow safety while casting: the simulator fixture uses the Core Player FollowMovementGenerator, an Engine-selected non-triggered Lesser Heal and a moving Main Tank; it verifies stable formation arrival, pending cast/GCD, Core `IsNoMovementSpellCasted()` ticks, no follower displacement during the cast, completed healing/mana cost and follow resumption. Runtime CI `#44159191502` on `5d3c9c5` passed (`pendingTicks=6`, `coreNoMovementTicks=5`, `maxCastMove=0.00`, `castCompleted=1`, `leaderReached=1`, `followPaused=1`, `followedAfterCast=1`, final distance `2.00`, `followRetained=1`, `cleanup=1`).
- [x] Extend follow/formation coverage: Stage F5 validates Engine-selected Main Tank handoff and recovery after the followed target disappears. Runtime CI `#44163805016` on `cfdda0c` passed (`handoffTargetChanged=1`, `followerTrackedNewLeader=1`, `oldLeaderRemoved=1`, `recoveryTargetRestored=1`, `recoveredToAvailableLeader=1`, final distance `4.55`, `followGenerator=1`, `cleanup=1`).
- [x] Validate Player heal execution-time range/resource rejection. Runtime CI `#44167535205` on `e52bea6` passed: stale nearby snapshot versus actual 60-yard target returned `SPELL_FAILED_OUT_OF_RANGE` (`89`), health remained `50000`, mana `2896>2896`, no pending cast/GCD; mana depleted after evaluation returned `SPELL_FAILED_NO_POWER` (`77`), health remained `50000`, mana stayed `0`, no pending cast/GCD, cleanup succeeded.
- [x] Validate Player heal LOS rejection through a real closed GameObject collision model, and refuse Engine-selected MovePoint while Core reports `UNIT_STATE_CAN_NOT_MOVE` or `UNIT_STATE_POSSESSED`. Runtime CI `#44170264755` on `e46a246` passed: `blockerTemplates=370`, `testedModels=13`, `blockerEntry=13965`, `blockedLOS=1`, `castResult=42` (`SPELL_FAILED_LINE_OF_SIGHT`), health `50000>50000`, mana `2896>2896`, no pending/GCD, LOS restored and cleanup; both control-state candidates rejected with no movement and cleanup.
- [x] Add a five-member real Core Group fixture and verify production PartySnapshotBuilder resolves the same-map Player roster, copies GUID/scalar state, reuses a fresh cache entry, then Core-disbands the Group and logs out all five Players. Runtime CI `#44175017402` on `434231f` passed (`members=5`, Main Tank/roster snapshot valid, `valuesCopied=1`, `cacheRetained=1`, `groupRemoved=1`, all Player GUIDs detached, `cleanup=1`, final `Runtime PASS`).
- [x] Stage F9 combines F8's real five-Player Group/PartySnapshotBuilder roster and cache checks with two hostile adds, a normal non-triggered heal, Core HostileRefManager::threatAssist and threat-target switching, using one Group and one session set. Runtime CI `#44182086347` on `02f0d80` passed: five members, initial roster/value/cache checks, Tank health `1717>343` while alive, normal Lesser Heal `1374>1423` with mana `2896>2866`, two hostile references, assist input `400`, healer threat `212.2/212.2`, both current targets healer, final snapshot/threat copy and cleanup.
- [ ] Evaluate full dungeon/instance scripts as a later tier.

**Acceptance:** a headless CI scenario runs real pinned-Core combat methods and verifies damage, threat, target changes, Engine-selected attacks, a triggered Creature heal, a known-spell non-triggered Player cast through pending/GCD/completion, actual health and mana changes, and cleanup. A toy combat model or direct mutation of expected results does not count.

Design and audit: CORE-COMBAT-SIMULATOR.md. This phase is test infrastructure, not an alternative AI architecture.

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
- [x] Stage F10 validates Core Tank and Healer death cleanup with two adds. Runtime CI `#44185236970` on `1a463d8` passed: lethal Core DealDamage removed both dead Players from both hostile ThreatManagers; native `SelectHostileTarget` kept both adds on Healer after Tank death, then selected surviving DPS after Healer death; production snapshots reflected both dead roster states, current victims and remaining threat entries; Group/session cleanup and Runtime PASS succeeded.
- [x] Stage F11 adds Core-derived `meleeReachable` to every copied threat entry using `hostileUnit->CanReachWithMeleeAutoAttack(threatTarget)`. Runtime CI `#44213077421` on `6066306` passed the production five-Player Group/two-add fixture with `meleeReachability=4/4 reachable=2 unreachable=2`; all four snapshot values matched direct Core API calls and the complete simulator reached final Runtime PASS.
- [x] Stage F12 extends hostile discovery through each party member's Core `HostileRefManager`, including hostiles no longer attacking any Group member and references moved to the offline threat container. Runtime CI `#44215877767` on `1a3aa02` passed: `outsideVictim=1`, `noPartyAttacker=1`, `tankRef=1`, `offlineSnapshot=1`, `nonVictimHostile=1`, `restored=1`; the F10 Tank/Healer death recovery and complete simulator Runtime PASS also succeeded.
- [~] Threat budgets, aggro prevention and broader recovery.
  - [x] Stage P7.1: pure snapshot-only `AssessThreatBudget` projection with Core's 110% melee / 130% ranged thresholds, strict boundaries, explicit Unknown states and wrap-safe freshness. Tests and ARM64 Core build passed in CI `#44217682323` on `705f7f3`; the runtime step was blocked before mangosd startup by Google Drive download quota.
  - [x] Stage P7.2: audit direct damage, direct/periodic healing, assist fan-out, flat spell threat and Core modifiers; add GUID/scalar-only `ThreatImpact` contract, defaulting to Unknown and distinguishing Complete/NoThreat/Unknown.
  - [~] Stage P7.3: connect action-specific impact estimates, reservations and budget policy without duplicating Core target selection.
    - [x] P7.3a: extend the typed impact taxonomy to distinguish periodic damage and periodic power assist; validate source/bound enums, finite nonnegative threat points, nonzero GUIDs/reason/epoch and exact snapshot-epoch matching with pure tests.
    - [~] P7.3b: begin Core-backed per-action estimation with Core-backed direct-damage, direct-heal-assist and positive flat-spell-threat components. Push #44238072768 verifies applied-damage threat parity; full-fixture push #44242344188 verifies the direct-heal assist upper bound (85.8 vs actual 14.5 per hostile); push #44251387021 (ada5988) confirms the positive flat spell_threat path and exact Core positive/negative effect-mask guard without regressions. Primary controlled Pet threat-owner identity and budget admission also passed a real Core Pet fixture in push #44255823652 (petThreat=30, petHeadroom=80). These remain components; the whole Action remains Unknown until other sources are covered.
    - [x] P7.3c: add bounded floating-point reservations keyed by hostile and actual threat owner, with wrap-safe map-tick expiry, idempotent action rows, and complete action/bot-owner cleanup. Pure tests cover fan-out aggregation, pet threat ownership, expiry, invalid values and capacity; full Linux ARM64 test/build CI passed in #44228603914.
    - [~] P7.3d: integrate per-hostile headroom and reservations into action eligibility/priority, then validate against actual Core-generated threat and prove the Engine never mutates Core threat/victim state.
      - [x] Add fail-closed candidate gating: every Action must explicitly return NoThreat or a complete, snapshot-epoch-matched impact estimate; Unknown/malformed estimates are rejected, never treated as zero.
      - [x] Check each hostile/actual threat-owner pair against Core-derived headroom, existing in-flight reservations and higher-priority candidates already admitted in the same decision report.
      - [x] Revalidate immediately before execution; reserve all positive impact rows before calling the action, roll back on reservation/execution failure, and retain successful reservations until declared expiry or explicit action release. Standalone tests also cover a competing reservation inserted between Evaluate/ExecuteSelected and an atomic multi-row rollback when the 128-entry cap is reached.
      - [~] Add production pre-action estimates for damage, healing/assist fan-out, periodic effects, flat spell threat, pets and other indirect recipients. Direct-heal assist has a Core-derived upper-bound component; positive flat spell threat for the supplied healed target is included when Core's whole effect mask is positive. Primary controlled Pet identity/threat snapshot and budget admission are now supported: the actual Pet GUID remains the threat owner, while a unique online same-map Player owner proves party membership and anchors reservation cleanup. Push #44255823652 on 295368c passed a full 5875 fixture with a real temporary Core Pet (petCreated=1, petSnapshot=1, petBudget=1, petThreat=30, petHeadroom=80, petMelee=1) and explicit HostileReference cleanup. This is identity/budget support, not a complete Pet action estimator; direct damage still has applied-damage parity only, and spell damage, DoTs/HoTs, guardians, totems and transient summons remain open. Periodic-path audit: HoT ticks call OnPeriodicCalculateAmount, apply target SpellHealingBonusTaken, run ProcDamageAndSpell even at full health, then distribute 50% of actual gain times spell threat multiplier over the healed target's current hostile references; the Aura also exposes OnPeriodicTrigger/OnPeriodicTickEnd and Health Funnel can add self-damage. These paths are now documented, but no HoT/DoT estimate is promoted: script/proc side effects and future tick state must be bounded first.
      - [x] Extend Core-backed runtime CI to verify threshold admission/rejection against actual ThreatManager values, reservation races and cleanup, and unchanged Core victim/threat state. Push run #44238072768 on d23d865 passed the full 5875 fixture with budget rejection, execution-race rejection, safe reservation, unchanged Core state and cleanup. Direct-heal component runtime passed in #44242344188; the follow-up real primary Pet threat-snapshot/budget/cleanup extension passed the full fixture in #44255823652 (petCreated=1, petSnapshot=1, petBudget=1, petThreat=30, petHeadroom=80, petMelee=1).
        - [x] Add a real five-Player Group/two-add probe: Core HostileRefManager assist creates non-victim threat references; production PartySnapshotBuilder feeds both the 110% melee and 130% ranged budget assessments; Core ThreatCalcHelper estimates a spell-less damage action; Engine rejects an over-budget candidate and accepts/reserves a safe candidate; real DealDamage verifies the estimated threat delta; action release and unchanged Core victim/threat state are asserted. Full Linux ARM64 compile/pure-test and fixture-backed runtime passed in `#44238072768` on `d23d865`.
        - [x] Run the expanded scenario on the trusted push runner with the full 5875 fixture. Run `#44238072768` on `d23d865` completed successfully and uploaded artifact `partybot-simulator-d23d865377fd27d92b8e451814adcd506a40ad4c`; the log includes `executionRaceRejected=1` and the final simulator `Runtime PASS`. PR jobs remain secret-free; `workflow_dispatch` is still unavailable until the workflow file reaches the repository default branch.
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



## Periodic AuraScript audit update (2026-11-18)

The pinned 5875 spell-script modules were inspected for AuraScript subclasses and periodic/proc callbacks. This confirms that generic periodic effects cannot be forecast from spell-table base points alone:

- Hunter Wyvern Sting casts a follow-up spell when its aura is removed; Frost Trap invokes `ProcDamageAndSpell` from `OnPeriodicTrigger`.
- Mage Ignite is proc-driven: it derives new DoT damage from the triggering hit, then stacks/refreshes or applies spell 12654. Its future tick amount is therefore coupled to prior combat events.
- Shaman Mana Tide replaces the default periodic trigger with a custom, dithered triggered spell and suppresses the default cast.
- Warlock Curse of Idiocy conditionally suppresses its periodic trigger based on the target's accumulated stat loss.
- The Gnomish Death Ray periodic aura chooses a random 100–500 amount, accumulates tick damage, and casts a separate damage spell when the aura expires.
- Darkmoon Faire turret/Flamethrower auras consume mana in the periodic callback and can remove themselves before the default trigger; Cannibalize's tick-end callback changes emote state only. Other inspected aura scripts also perform apply/remove-triggered casts or proc handling.

This is a concrete audit sample, not proof that every spell/aura script or proc chain has been exhaustively enumerated. P7.3b/P7.3d remain partial; generic DoT, HoT, periodic power and triggered-effect actions stay Unknown until their complete callback/proc and recipient sets are bounded. The full runtime fixture on commit `992c25f` passed in trusted push CI `#44264594529`; PR build `#44264603355` also passed. Ordinary CI Build push `#44264594534` was skipped by workflow conditions and is not counted as a pass or failure.
