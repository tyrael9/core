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
- [~] Replace candidate-list-only arbitration with the required action/event queue semantics. The execution boundary now materializes selected report entries into a bounded 32-entry priority queue with stable FIFO ties, fail-closed overflow, index-only payloads and immediate pre-dispatch Core/threat revalidation. Standalone tests cover ordering, ties, capacity and clearing. This is the first queue-backed dispatch slice, not yet a persistent cross-tick event queue, cadence scheduler or continuation/result-listener system.

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

- [~] Implement encounter identity and start/end detection. The map/instance-scoped PartyCoordinator now assigns process-unique encounter IDs, deduplicates party observations, starts on live online member/companion combat, and ends after a configurable quiet settle interval (default 30 seconds) or same-map party/shadow teardown. Pure tests cover duplicate observations, companion combat, wrap-safe settle timing and explicit close. Explicit end records for cross-map/instance transitions remain open because an old map shard must not be mutated from another map thread.
- [~] Add stable structured event schema with sequence numbers and schema version. Versioned key/value encounter_start/encounter_end records now carry encounter ID, per-encounter sequence, group/map/instance, bot GUID, elapsed time and reason; boundary records bypass the ordinary per-tick detail cap. Remaining event families and end-to-end report correlation are still open.
- [~] Record party roster, role/state snapshots and bounded combat heartbeats. Initial `party_snapshot` heartbeat is emitted once per map/instance coordinator (default 2 seconds, configurable through `PlayerBot.EngineEncounterHeartbeatMs`), shares encounter sequence numbers, records up to five members/companions and eight hostiles, and explicitly reports omitted/truncated facts. Pure tests cover shared deduplication, cadence, sequence reset and uint32 clock wrap. Detailed per-bot state, threat samples and full event-budget/terminal truncation handling remain open.
- [~] Record trigger results, candidates, rejection reasons, arbitration and selected action. Active-encounter Shadow summaries and up to three prioritized candidates per bot/10-second trace window now emit `[PartyBotEngineTrace]` records with shared encounter IDs/sequences, snapshot epoch, role/strategy/action/spell/channel/target/priority/status/reason. This remains read-only candidate evidence; trigger events, full arbitration detail and authoritative execution results remain open.
- [~] Record action attempt and authoritative vMaNGOS result separately. The central Legacy `CombatBotBaseAI::DoCastSpell` path emits paired `cast_request_attempt` / `cast_request_result` with a shared action_id and ordered encounter sequences. The Core `Spell::prepare` hook binds that action to the root Spell object; `Spell::finish` emits `cast_terminal` with actual Core success/failure, and `Spell::cancel` marks interruption. Twelve lifecycle records per bot AI tick are allowed with an explicit overflow marker. `SPELL_CAST_OK` remains request acceptance, not completion. Direct CastSpell bypasses, terminal events after encounter closure, movement/item actions and downstream action effects remain open.
- [~] Record target changes, threat samples, reservations, casts, health transitions and deaths. The shared party heartbeat now includes up to two actual Core ThreatManager references per hostile with threat/online/accessibility/melee-reachability and omitted counts. Successful Legacy `PartyBotAI::AttackStart` victim switches during an active encounter emit encounter-sequenced `target_change` records. Engine threat-budget veto, reservation attempt/accepted/rejected and release/rollback outcomes now emit bounded encounter events from the actual executor/coordinator path. `Player::SetDeathState` now notifies an attached PlayerAI after Core commits an alive/dead transition; PartyBotAI emits encounter-sequenced `death` / `resurrect` records with health/max-health, bypassing ordinary per-tick detail caps. This currently covers PartyBots with an attached PartyBotAI, not human party members or damage-by-damage health deltas. Reservation refresh/expiry/conflict sources outside Engine admission, health deltas, and broader target-change sources remain open.
- [~] Protect high-value events from ordinary rate limits; emit explicit truncation/error markers. Encounter start/end boundaries and death/resurrection records bypass the ordinary detail cap; cast and reservation lifecycle streams have independent per-tick caps with one `trace_limit` marker per category, and target changes have their own cap/marker. Party heartbeat records expose omitted entity counts, per-hostile threat-reference omissions and a payload `truncated` flag. Remaining work: add an explicit terminal/global event-budget marker and cover overflow/truncation paths in tests; do not silently claim the entire trace is lossless.
- [ ] Keep tracing bounded and configurable for the 1 OCPU ARM64 host.
- [ ] Extend the read-only deployment diagnostics collector to produce one report/raw log bundle by encounter ID.

**Acceptance:** after a fight, the operator can run one read-only command and attach a report that makes it possible to distinguish perception, policy, coordination, eligibility, execution and encounter-model failures. No DB schema change is required.

Trace design: DECISION-TRACE.md.

## Phase 4 — complete decision loop and exclusive execution runtime

- [ ] Implement typed named Values with bounded cache, expiry and explicit invalidation.
- [ ] Implement Trigger check intervals/cadence and state-transition events.
- [~] Implement action queue priorities, deterministic ordering and bounded processing. Initial per-dispatch queue is bounded to 32 entries and processes at most eight entries per dispatch; current channel arbitration means ordinary reports remain smaller. Persistent queue lifecycle, event insertion, expiry and cross-tick scheduling remain open.
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
    - [x] P7.3a: extend the typed impact taxonomy to distinguish periodic damage, periodic power assist and periodic mana leech; validate source/bound enums, finite nonnegative threat points, nonzero GUIDs/reason/epoch and exact snapshot-epoch matching with pure tests. Periodic life-leech's damage/self-heal fan-out reuses the typed PeriodicDamage and PeriodicHealAssist rows.
    - [~] P7.3b: begin Core-backed per-action estimation with Core-backed direct-damage, direct-heal-assist and positive flat-spell-threat components. Push #44238072768 verifies applied-damage threat parity; full-fixture push #44242344188 verifies the direct-heal assist upper bound (85.8 vs actual 14.5 per hostile); push #44251387021 (ada5988) confirms the positive flat spell_threat path and exact Core positive/negative effect-mask guard without regressions. Primary controlled Pet threat-owner identity and budget admission also passed a real Core Pet fixture in push #44255823652 (petThreat=30, petHeadroom=80). These remain components; the whole Action remains Unknown until other sources are covered.
      - [x] Add negative/harmful flat spell-threat component using the exact successful Core target/effect-mask set, Core mixed-sign classification, per-target `CanCauseThreatOnMask`, harmful-threat attributes and `ThreatCalcHelper::CalcThreat`. Core-backed runtime CI `#485` on `0d22aff` found loaded spell `99`; estimated threat `9.000` matched the actual ThreatManager delta `9.000` with unchanged target health. The same fixture verifies fail-closed behavior for incomplete target sets, invalid masks, duplicate/unresolved targets and mixed-sign spells. This remains a component, not a complete spell-action estimate; SpellScript/proc closure and periodic/action side effects remain open.
    - [x] P7.3c: add bounded floating-point reservations keyed by hostile and actual threat owner, with wrap-safe map-tick expiry, idempotent action rows, and complete action/bot-owner cleanup. Pure tests cover fan-out aggregation, pet threat ownership, expiry, invalid values and capacity; full Linux ARM64 test/build CI passed in #44228603914.
    - [~] P7.3d: integrate per-hostile headroom and reservations into action eligibility/priority, then validate against actual Core-generated threat and prove the Engine never mutates Core threat/victim state.
      - [x] Add fail-closed candidate gating: every Action must explicitly return NoThreat or a complete, snapshot-epoch-matched impact estimate; Unknown/malformed estimates are rejected, never treated as zero.
      - [x] Check each hostile/actual threat-owner pair against Core-derived headroom, existing in-flight reservations and higher-priority candidates already admitted in the same decision report.
      - [x] Revalidate immediately before execution; reserve all positive impact rows before calling the action, roll back on reservation/execution failure, and retain successful reservations until declared expiry or explicit action release. Standalone tests also cover a competing reservation inserted between Evaluate/ExecuteSelected and an atomic multi-row rollback when the 128-entry cap is reached.
      - [~] Add production pre-action estimates for damage, healing/assist fan-out, periodic effects, flat spell threat, pets and other indirect recipients. Direct-heal assist has a Core-derived upper-bound component; positive flat spell threat for the supplied healed target is included when Core's whole effect mask is positive. Primary controlled Pet identity/threat snapshot and budget admission are now supported: the actual Pet GUID remains the threat owner, while a unique online same-map Player owner proves party membership and anchors reservation cleanup. Push #44255823652 on 295368c passed a full 5875 fixture with a real temporary Core Pet (petCreated=1, petSnapshot=1, petBudget=1, petThreat=30, petHeadroom=80, petMelee=1) and explicit HostileReference cleanup. This is identity/budget support, not a complete Pet action estimator; direct damage still has applied-damage parity only, and spell damage, DoTs/HoTs, guardians, totems and transient summons remain open. Periodic-path audit: HoT ticks call OnPeriodicCalculateAmount, apply target SpellHealingBonusTaken, run ProcDamageAndSpell even at full health, then distribute 50% of actual gain times spell threat multiplier over the healed target's current hostile references; the Aura also exposes OnPeriodicTrigger/OnPeriodicTickEnd and Health Funnel can add self-damage. The DoT path is now traced through Aura::PeriodicTick: stored Aura amount or target-max-health percentage, script callback and family-specific tick scaling, target damage-taken modifiers, dither, immunity/absorb/resist, proc dispatch, health clamp, then Unit::DealDamage(DOT) and the hostile ThreatManager's applied-damage threat calculation. Curse of Doom can add a kill follow-up; periodic leech/funnel combine hostile damage with caster healing-assist threat. These are source-audit findings; no future-tick HoT/DoT estimate is promoted because tick state, proc/script closure and recipient fan-out are not bounded. Subsequent applied-event components now cover one already-applied ordinary DoT tick and one already-applied ordinary HoT tick; they do not predict future ticks or close the enclosing spell action.
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


## Direct spell-damage pipeline audit update (2026-11-18)

The next P7.3b audit traced the ordinary direct spell path across `Spell::HandleEffects`, `Spell::DoSpellHitOnUnit`, `SpellCaster::CalculateSpellEffectValue`, `SpellCaster::CalculateSpellDamage`, `SpellCaster::DealSpellDamage`, `Unit::DealDamage` and `ThreatCalcHelper::CalcThreat`.

The core path is not a single base-points-to-threat calculation:
- Each effect starts with `CalculateSpellEffectValue`: level scaling, base dice/random range, combo-point contribution and active spell modifiers can change the effect value at cast/effect execution time.
- `OnEffectExecute` runs before the effect handler and may alter `spell->damage`, cancel the effect, consume state or cast another spell. `OnHit` runs after damage calculation/mitigation setup but before the proc and final damage application, and may also alter behavior.
- Direct damage is aggregated into `m_damage`; the hit path applies caster/target damage modifiers, crit, armor and final dithering in `CalculateSpellDamage`. Absorb/resist/block and `DealDamageMods` then adjust the amount before it is applied.
- The spell hit path calls `ProcDamageAndSpell` before `DealSpellDamage`. Proc-triggered spells may therefore create additional damage/threat, potentially with different actors, targets and timing.
- The applied direct damage then reaches `Unit::DealDamage`, where non-player victims receive `damage * GetSpellThreatMultiplier`, followed by `AddThreat` and `ThreatCalcHelper::CalcThreat` (spell-mod threat, critical-threat modifier and total threat modifier).
- Helpful/harmful flat `spell_threat` entries are a separate path: positive entries assist each successful eligible target's hostile references; negative entries add threat to the affected hostile. This must be added to the same action's full recipient set, not silently omitted.

The registered class/item SpellScript inventory also contains damage-changing or additional-effect examples: Ferocious Bite and Eviscerate add energy/combo/AP scaling; Bloodthirst uses attack power; Shield Slam adds block value; Execute consumes rage and has a separate rage-scaled triggered damage spell; Hammer of Wrath and Judgement of Command adjust damage; Holy Shock selects a separate damage/heal spell; Holy Nova launches a second spell on successful finish; Warrior's Wrath and Touch of Weakness cast follow-up spells; Flametongue computes weapon-speed/spell-power damage and casts 10444; Meteor divides damage across its selected target set. This is a sample from the registered class/item modules, not an exhaustive audit of every database binding or active proc aura.

**Conclusion:** the current direct-damage helper remains applied-damage parity evidence only. A future pre-cast estimate cannot be promoted to Complete merely by calling the Core formula on spell-table base points. It must resolve the effect values and current caster/target modifiers, identify the registered SpellScript, model or conservatively bound script changes and all triggered casts, account for the active proc closure, and enumerate every hostile/actual threat-owner row including flat spell threat. Generic direct spell actions therefore remain Unknown until that full action-specific closure is proven. No Core threat or victim state is modified by the estimator.


## Proc-trigger closure audit update (2026-11-18)

A follow-up trace of `SpellCaster::ProcDamageAndSpell`, `ProcDamageAndSpell_real`, `Unit::ProcDamageAndSpellFor` and `Unit::HandleTriggers` confirms that proc side effects are a live-state graph, not a fixed property of the spell ID. The Core scans the current attacker's aura holders and, where applicable, the victim's aura holders; filters by proc flags, proc-extra outcomes, spell family masks, cooldown/charge state, apply time and instant-vs-delayed proc mode; then dispatches the collected proc list. Depending on server configuration, some procs execute instantly while others are queued for delayed processing. The proc handler contains spell-family-specific branches that select alternate trigger spell IDs, derive custom base points from the actual damage/original damage, target health, aura amount, or other current state, and can issue multiple triggered casts (including a separate caster-side and victim-side cast).

This closes the current audit gap: a pre-cast damage estimate that does not inspect the active caster and target aura/proc state cannot know the triggered spell closure or every threat owner/hostile recipient. The estimate also cannot assume only the attacker procs: the victim's defensive/retaliation procs may produce additional damage or other effects. A static whitelist of ordinary damage spell IDs is insufficient because the active aura state and proc outcomes change per cast.

Keep generic direct spell actions `Unknown` until the estimator either:
- builds a bounded, snapshot-epoch-scoped closure over all eligible attacker and victim proc auras and their triggered spells, including delayed processing and recursive proc limits; or
- deliberately supports a narrower action class whose eligibility proves that no relevant proc aura can fire and whose registered SpellScript/flat-threat paths are completely modeled.

No such closure is implemented yet. This is source-audit evidence only; no combat policy was relaxed.


## Negative flat spell-threat routing audit (2026-11-18)

The next P7.3b/P7.3d gap is now documented from Core's `Spell::HandleThreatSpells` path. Positive flat threat fans out through the affected target's HostileRefManager; negative flat threat is added directly to each successful affected hostile's ThreatManager with the caster as threat owner. Core rejects mixed-sign active effect masks and filters each target by its successful effect mask. The existing estimator covers only the positive flat-threat component within direct-heal assist; negative flat spell threat remains unsupported.

Implementation slice complete: the component-scoped negative flat-threat estimator requires the complete successful target set and exact per-target effect masks, reproduces Core's negative-effect classification and mixed-sign rejection, applies Core's ThreatCalcHelper modifiers, and fails closed on unresolved targets or incomplete masks. The real Core cast fixture and its fail-closed cases passed in CI #485. Generic spell actions remain Unknown because SpellScript, proc closure, and other action side effects are separate blockers.


## Negative flat spell-threat estimator implementation (2026-11-18)

- [x] Add a component-scoped Core estimator for negative `spell_threat` routing. It requires a caller-proven complete successful target set and exact per-target effect masks; reproduces Core's negative-effect classification, mixed-sign rejection, harmful-threat suppression, `CanCauseThreatOnMask` filter and `ThreatCalcHelper` modifier; emits caster-owned threat rows on the affected hostile.
- [x] Add a real Core cast fixture comparing the estimate with the actual ThreatManager delta, plus fail-closed tests for incomplete target sets, mixed-sign effects, invalid masks and unresolved targets. Full runtime CI #485 on 0d22aff passed.
- [ ] Keep the enclosing action `Unknown` until SpellScript/proc/other effect closure is separately proven.


## Negative flat spell-threat Core cast fixture (2026-11-18)

- [x] Add a Core-backed runtime fixture that discovers a real harmful single-effect aura spell with a loaded `spell_threat` row, evaluates the component using the exact one-target effect mask, performs the real triggered Core cast, and compares the hostile ThreatManager delta to the estimate. The fixture also asserts no direct health damage, so the measured delta isolates the flat spell-threat route.
- [x] Make the runtime CI script require the new PASS marker; the fixture is no longer an optional diagnostic that can silently be omitted.
- [x] Verify ARM64 runtime CI #485 and its artifact: loaded spell 99 produced estimated 9.000 and actual ThreatManager delta 9.000; all fail-closed cases passed.
- [x] Add negative-path coverage for incomplete target sets, invalid masks, mixed-sign effects, duplicate targets and unresolved targets.



## Applied periodic-damage tick component (2026-11-18)

- [x] Add a component estimator that accepts only the actual applied damage from one completed periodic tick, then uses Core GetSpellThreatMultiplier and ThreatCalcHelper to produce an exact PeriodicDamage row. The enclosing impact estimate remains Unknown; this is not a forecast of future ticks.
- [x] Add a Core simulator fixture for the pinned 5875 Shadow Word: Pain rank 8 aura (spell 10894): real triggered cast, no AuraScript callback, ordinary Aura update loop, exactly one observed Core periodic tick, actual health loss and hostile ThreatManager delta compared with the component.
- [x] Require the dedicated periodic-damage PASS marker in tools/partybot-simulator/run-ci.sh.
- [x] Verify the ARM64 runtime CI and uploaded artifact: trusted full-runtime CI #507 on 59db1044 passed; spell 10894 ticked once (0>1), 142 applied damage matched estimated threat 142.000 and actual ThreatManager delta 142.000 (`matched=1`, cleanup=1).
- Future DoT casts, HoTs, periodic power, leech/funnel, periodic procs and all action-level future-tick forecasts remain Unknown until their callback/proc closure, changing tick state and full threat-recipient fan-out are bounded.



## Applied periodic-healing assist component and parity fixture (2026-11-18)

- [x] Add EstimateAppliedPeriodicHealingAssistComponent for one already-completed Core periodic heal tick. It consumes effective healing gain, uses Core's 50% periodic-heal assist coefficient and spell-threat multiplier, divides across live hostile references on the healed Unit, applies Core ThreatCalcHelper modifiers and mirrors hard-CC assist suppression. It emits PeriodicHealAssist rows while the enclosing estimate remains Unknown.
- [x] Allow self-target periodic healing at the component boundary; self-target HoTs are valid Core cases and are not excluded merely because caster and healed target GUIDs match.
- [x] Add a full Core simulator fixture using Rejuvenation rank 10 (spell 9841), two Core hostile references, real Aura application and normal Aura tick scheduling. Compare each actual ThreatManager delta with the component.
- [x] Require the periodic-healing parity PASS marker in tools/partybot-simulator/run-ci.sh.
- [x] Verify fixture-backed ARM64 runtime CI and inspect its artifact: CI #529 on 2d643f1513aa0fb9c4ef9ab632665d887afbbf35 passed; one real Rejuvenation tick healed 189, with two hostile ThreatManager deltas of 47.250 each matching the component estimate (`estimateShape=1`, `matched=1`, cleanup=1), followed by the full simulator `Runtime PASS` marker. Keep generic future-tick prediction, AuraScript/proc closure, leech/funnel and complete action-level estimates Unknown.



## Applied periodic-power assist component and Core fixture (2026-11-18)

- [x] Audit Core's two periodic resource-restoration branches. In 1.12, PERIODIC_ENERGIZE generates 50% assist threat from effective gains except when the resource is Mana or Happiness; OBS_MOD_MANA is a separate percent-of-max-Mana path that does generate 50% assist threat. PERIODIC_MANA_LEECH remains a distinct drain/transfer path.
- [x] Add EstimateAppliedPeriodicPowerAssistComponent using actual ModifyPower gain, exact aura effect index/power type validation, Core spell-threat multiplier and ThreatCalcHelper, complete live hostile-reference fan-out, Core hard-CC assist suppression and 1.12 Mana/Happiness exemptions. The enclosing estimate stays Unknown.
- [x] Add a Core simulator fixture using the exact SpellMgr-owned Rejuvenation rank 10 entry (9841), temporarily overriding only its Aura type/resource metadata with an RAII guard and restoring the original DBC fields after each tick/removal. It preserves Core's native SpellEntry pointer identity rather than passing a copied entry. The native 5875 scan found no ENERGY PERIODIC_ENERGIZE row (`energy=0`), so that branch must be exercised with this bounded Core-owned metadata override; mana PERIODIC_ENERGIZE and OBS_MOD_MANA are treated the same way for fixture consistency. CI #552 on `50adcd73` shows DoT/HoT runtime parity passing, while the power stage correctly failed closed at the missing energy candidate precondition. Direct HTTPS fixture download support is present to avoid recurring Google Drive quota failures.
- [x] Require a dedicated periodic-power PASS marker in tools/partybot-simulator/run-ci.sh.
- [ ] Re-run fixture-backed ARM64 runtime CI with the SpellMgr-owned scoped override. Only after all three Aura branches match their actual ThreatManager values and the full simulator reaches Runtime PASS may periodic-power runtime parity be marked verified. Future-tick forecasts, PERIODIC_MANA_LEECH, leech/funnel and whole-action completeness remain separate work.



## Applied periodic-mana-leech component and Core fixture (2026-11-18)

- [x] Audit `SPELL_AURA_PERIODIC_MANA_LEECH`: actual threat derives from the caster's effective `ModifyPower` gain, not target-side drained amount; Core applies 50% and the spell-threat multiplier, then calls `Unit::AddThreat` directly on the leeched hostile. This is not assist fan-out.
- [x] Add `PeriodicManaLeech` to the typed threat-impact taxonomy and pure source-contract tests.
- [x] Add `EstimateAppliedPeriodicManaLeechComponent`, consuming the actual caster power gain and reproducing Core non-assist `ThreatCalcHelper` behavior. It rejects unresolved/suppressed threat states and leaves the enclosing estimate Unknown.
- [x] Add a Core simulator fixture that discovers a native one-effect 5875 periodic-mana-leech Aura, casts it through the native SpellMgr entry, advances one real Aura tick, and compares target mana loss, caster effective mana gain and the leeched hostile's ThreatManager delta. CI requires a dedicated PASS marker.
- [ ] Verify the fixture on the trusted ARM64 runtime runner and inspect its artifact. The full extracted-data download is currently blocked by the Google Drive quota; `run-ci.sh` now also supports a direct HTTPS object URL / OCI read-only PAR. Mark runtime parity only after the real one-tick comparison and final Runtime PASS.
- [ ] Keep future-tick mana-leech prediction Unknown until Mark of Kazzak depletion explosion (21056 -> 21058), Improved Drain Mana's additional 15%/30% damage ticks, AuraScript callbacks and all other side effects are bounded. Leech/funnel action-level closure remains separate.



## Applied periodic-life-leech composite component and fixture (2026-11-18)

- [x] Extend the applied periodic damage component to accept Core's shared PERIODIC_LEECH / HEALTH_FUNNEL damage path.
- [x] Extend the applied periodic heal-assist component to accept the corresponding self-heal effect metadata; continue to consume only actual effective heal gain from `DealHeal`.
- [x] Add a two-hostile Core simulator scenario: hostile A receives periodic-leech damage; the caster heals itself; Core damage threat is measured on A and Core HostileRefManager assist threat is measured on A+B. The fixture compares A's combined delta (damage + heal assist) and B's assist-only delta against the two component estimates.
- [x] Require a dedicated periodic-life-leech composite PASS marker and include the stage in the final simulator aggregate.
- [ ] Await trusted ARM64 runtime execution and inspect the artifact. The build/pure-test workflow remains separate from this runtime gate because the current Google Drive extracted-data URL is quota-limited; direct HTTPS object/PAR download support is available.
- [ ] Keep future-tick leech/funnel action estimates Unknown while AuraScript, health-side effects, trigger/proc closure and all recipient fan-out remain unbounded.

## Player alive-state trace lifecycle regression (2026-11-19)

- [x] Add a simulator-only PlayerAI observer to the existing real five-Player Core Group death/recovery scenario. The observer is attached to the Main Tank immediately before Core DealDamage kills it, then restored before fixture cleanup.
- [x] Verify the real Player::SetDeathState hook reports after both JUST_DIED and CORPSE state assignments.
- [x] Explicitly advance JUST_DIED -> CORPSE and verify PartyBotAI's same-state filter semantics: two raw callbacks, one unchanged-state callback, and exactly one alive -> dead semantic transition.
- [x] Make the enclosing F10 death/recovery scenario fail if either observer assertion fails, and make run-ci.sh require the dedicated PASS marker.
- [x] Verify the corrected death + resurrection observer suite on ARM64 Core compilation and fixture-backed runtime CI. Push run [#44448514086](https://github.com/tyrael9/core/actions/runs/44448514086) on `66cdc447` passed the full Core build, pure tests and fixture-backed runtime; both dedicated death and resurrection PASS markers were present. This validates the Core callback boundary and transition semantics, not PartyBotAI formatted encounter-record emission end-to-end.


### Death-state callback semantics correction

The first runtime attempt correctly exposed a Core lifecycle nuance: Player::SetDeathState invokes the observer after both JUST_DIED and the subsequent CORPSE state assignment. The callback is therefore a state sample, not a promise that each invocation represents a transition. PartyBotAI's OnAliveStateChanged correctly filters unchanged states (wasAlive == isAlive). The regression now records raw callback count separately from actual transitions and requires two callbacks (JUST_DIED + CORPSE), exactly one unchanged-state callback, and exactly one alive -> dead transition. The earlier wording that Core suppressed the CORPSE callback was inaccurate and has been replaced.


### Resurrection observer runtime coverage added (2026-11-19)

- [x] Extend the real five-Player death/recovery fixture: after both post-death snapshots and target-retarget assertions are captured, resurrect the Main Tank through Core Player::ResurrectPlayer(1.0f, false).
- [x] Attach a fresh PlayerAI observer and require exactly one dead -> alive transition, with the Player alive after the call.
- [x] Gate F10 and run-ci.sh on the dedicated resurrection PASS marker.
- [ ] Verify the corrected death + resurrection observer suite on fixture-backed ARM64 runtime CI. This validates the Core callback boundary and transition semantics, not PartyBotAI's formatted trace emission end-to-end.
