# Core-backed PartyBot combat simulator

## Latest trusted runtime — F54n deferred instant-heal recipient race (2026-11-26 PT)

The latest trusted Core-runtime result for this branch is F54n runner-fix commit `eeab512906b7b38cb48adc82bf4c51eecc8af9f7`, in push [#45948124808](https://github.com/tyrael9/core/actions/runs/45948124808). The Linux ARM64 / Vanilla 5875 job passed pure PartyBotEngine tests, report collection, Configure, full Server+Realm build, simulator harness syntax, fixture-secret validation, the complete Core-backed simulator, artifact upload and teardown. GDB reported normal inferior exit and the simulator emitted final `Runtime PASS`. Artifact: [#15175922343](https://github.com/tyrael9/core/actions/runs/45948124808/artifacts/15175922343). Matching PR-safe build [#45948128902](https://github.com/tyrael9/core/actions/runs/45948128902) passed pure tests, report collection, full ARM64 build and harness syntax; fixture secrets/runtime were skipped in PR context.

F54n proves why a supported spell's pre-cast component estimate is not necessarily a complete action estimate. The test uses a real instant self-target Desperate Prayer with one explicit `SPELL_EFFECT_HEAL`, no SpellScript, no cast time, and no channel. Before the Core SpellEvent is processed, the action estimate is `Unknown` with zero impacts and reason `direct_heal_spell_event_deferred_recipient_set_unbounded`; a separate diagnostic component view sees two current hostile references. After Core accepts the cast but before its queued SpellEvent applies the heal, the fixture adds a third hostile reference to the healer. Two real `Unit::Update(50, 50)` ticks then process the SpellEvent. Core applies `55.500` healing-assist threat to that newly added hostile, which was absent from the pre-cast component rows. The final marker was:

```text
rowReady=1 outerGate=1 priestAI=1 hostilesCreated=1 hostilesCleaned=1 aiRestored=1 healerReady=1 inspirationEntry=1 inspirationAbsent=1 learnedBeforeList=1 listBeforeRestore=1 hasSpellAfterCleanup=1 listAfterCleanup=1 supportedShape=1 effect0=10 targetA=1 targetB=0 effect1=0 speed=0.000 castTime=0 scriptId=0 noHelpfulThreat=0 weaponProcs=0 reflectable=0 channeled=0 livePreflight=1 estimateUnknown=1 snapshotMatches=1 impacts=0 componentImpacts=2 targetRefs=2 uniqueHostiles=2 procVeto=1 procAuraRemoved=1 procFlagsRestored=1 castTimeUnknown=1 aoeRejected=1 castStarted=1 eventDeferred=1 lateRecipientAdded=1 lateRecipientUnpredicted=1 lateRecipientThreatApplied=1 lateRecipientThreat=55.500 eventUpdates=2 healApplied=1 threatBoundsMatched=1 stateRestored=1 spellListRestored=1 expectedThreat=429.250/429.250 actualThreat=55.500/55.500
```

The runner checks each required flag, constrains event processing to 1–20 updates, parses the late-recipient threat value and independently requires it to be positive. The dedicated F54n PASS marker, all restoration/cleanup checks, the simulator's aggregate `Runtime PASS` and clean GDB exit were observed. This is a trusted fail-closed regression, not permission to execute Priest healing: the action-level estimate must remain `Unknown` until Core can authorize and reserve the exact recipient set at a safe pre-impact boundary.

F54m remains trusted-runtime validated in [#45932033065](https://github.com/tyrael9/core/actions/runs/45932033065): four successful nested casts healed all five Group Players, both hostile ThreatManagers matched 253.000 incremental threat, the cyclic fifth child was rejected exactly five times and all temporary state was restored. Artifact: [#15168729927](https://github.com/tyrael9/core/actions/runs/45932033065/artifacts/15168729927). F54k's random HoT interval passed in [#45889183898](https://github.com/tyrael9/core/actions/runs/45889183898), artifact [#15150693633](https://github.com/tyrael9/core/actions/runs/45889183898/artifacts/15150693633). F54l's school-aware absorb matching passed in [#45909113490](https://github.com/tyrael9/core/actions/runs/45909113490), artifact [#15158903603](https://github.com/tyrael9/core/actions/runs/45909113490/artifacts/15158903603).

### Current branch head and next F54n slice — action-level threat remains Unknown

Current branch head at this status sync: `eeab512906b7b38cb48adc82bf4c51eecc8af9f7`. The exact direct-heal spell shape, current hostile set and per-hostile Core components can be inspected, but the snapshot taken before `Spell::prepare` is not a stable recipient-set proof. `Spell::prepare` enqueues a `SpellEvent`; F54n demonstrates that a HostileReference can be added before the delayed effect executes. Do not promote the early component rows to `Complete` or `NoThreat`. The next implementation slice is a Core-owned, map-thread pre-impact authorization contract that refreshes the current snapshot/target/hostile set, applies the full threat budget and reserves the exact recipients immediately before any irreversible helpful-threat/heal effect. It must account for SpellScript, AuraScript, caster/recipient procs, target-trigger children and all Core effect recipients. If authorization cannot occur at a boundary that still permits a correct no-side-effect rejection, retain `Unknown`.

`PriestDirectHealAction` and the Priest Healing Strategy remain unregistered. Chain heals, party/AoE heals, implicit multi-target selectors and any unresolved side effect remain fail-closed. The current status in this section is authoritative; older notes below preserve their point-in-time scope.

### Previous trusted slice — F54f generic proc-child parity

The earlier trusted push [#45716916204](https://github.com/tyrael9/core/actions/runs/45716916204), running source commit `35224fc61cb29e1c2bb832224611b8519d8ec08a`, passed the full Linux ARM64 / Vanilla 5875 build and complete Core-backed simulator through final `Runtime PASS`.

F54f executed real delayed `PROC_TRIGGER_SPELL` child heals for both helpful-proc sides. The final marker measured caster control/root/child/total = 48/48/77/125 and recipient = 48/47/76/123. Root + child exactly equals total on both paths; each hostile's healer/tank `ThreatManager` delta matched the Core-derived expectation. The estimator stayed fail-closed with no partial impacts while the proc Aura was active. Temporary root/child/Aura SpellEntry fields and SpellProcEvent data were restored, final five-Player Group/session cleanup passed, and GDB reported normal process exit.

The metric-gate correction in `35224fc` now parses the child-heal fields as numeric effective-health deltas (positive root, child >10, exact root + child = total) rather than incorrectly comparing them with boolean `1`. The trusted run proves the correction and all preceding gates. Artifact: [partybot-simulator-35224fc](https://github.com/tyrael9/core/actions/runs/45716916204/artifacts/15070412741). Matching PR build for docs head `946ea641`: [#45716989280](https://github.com/tyrael9/core/actions/runs/45716989280).

## F54g — SPELL_AURA_ADD_TARGET_TRIGGER (normal, immune, reflect and deleted-target branches trusted-runtime validated)

F54g exercises the non-channeled `Spell::finish` path. Core's `HandleAddTargetTriggerAuras()` processes each non-deleted successful `m_UniqueTargetInfo` recipient and casts the configured child; a successful `SPELL_MISS_REFLECT` redirects the child to the caster. This differs from F54f's delayed generic proc queue.

The five-Player Prayer of Healing (596) / Lesser Heal (2052) fixture has trusted Core evidence for four branches:
1. **Normal fan-out:** `rootTargets=5 childTargets=5 threatMatched=1`; child effective gains `78/76/77/77/77`; each member had two hostile references and both hostile ThreatManagers matched the per-recipient root+child healing-assist projection.
2. **Immune recipient:** a selected school-immune Group member received neither root nor child healing. Core reported `rootTargets=4 childTargets=4 immuneTargetUnhealed=1 threatMatched=1 auraRemoved=1 immunityCleared=1 spellRowsRestored=1`.
3. **Successful reflect-to-caster:** a real reflect Aura on a hostile Creature reflected Shadow Bolt; the target-trigger child healed the caster: `reflectedChildGain=75 reflectedDamage=13 targetUntouched=1 refs=2 hasHostile0=1 hasHostile1=1 threatMatched=1`. Both hostile ThreatManagers matched.
4. **Deleted target after projectile launch:** live control proved a real impact and child cast (`controlRootHit=1 controlChildGain=76 rootDamage=13 delayMs=250`). A second delayed projectile captured a live Creature GUID; that Creature was removed before impact. Core could no longer resolve it and recorded `targetRemoved=1 targetGuidUnresolved=1 casterUnhealed=1 remainingThreatUnchanged=1 auraRemoved=1 spellRowsRestored=1`. No child self-heal or extra healing-assist threat occurred.

The initial deleted-target run exposed a fixture clock defect. Tight `Map::Update(50)` calls do not guarantee Player updates under Map's wall-clock scheduler, while delayed SpellEvent execution belongs to the caster's Core `Unit::Update` / `EventProcessor`. Commit `94c3ad29` fixes the test by explicitly calling `healer->Unit::Update(50, 50)` for the live control and deleted-target impact windows. The fixture still uses Core's actual delayed Spell, TargetInfo GUID, impact resolution and target-trigger handler; the acceptance conditions were not relaxed.

Trusted push [#45771916821](https://github.com/tyrael9/core/actions/runs/45771916821) on `94c3ad298319be502186d43679b05151880ede71` passed pure PartyBotEngine tests, report collection, Configure, Linux ARM64 / Vanilla 5875 Server+Realm build, harness syntax, fixture-secret validation and the complete Core-backed simulator through final `Runtime PASS`. Artifact: [#15094118627](https://github.com/tyrael9/core/actions/runs/45771916821/artifacts/15094118627). Matching PR-safe build [#45771924350](https://github.com/tyrael9/core/actions/runs/45771924350) passed its available checks; secret-backed runtime is intentionally skipped in PR context. The run reached final `Runtime PASS`; temporary Aura/SpellEntry fields and health were restored, five-Player Group/session cleanup passed, and GDB exited normally. The PR-safe build [#45771924350](https://github.com/tyrael9/core/actions/runs/45771924350) passed its checks and skipped secret-backed runtime by design.

The four branches above are validated. F54h separately proves one real three-cast recursive chain; deeper/arbitrary nested closure and the complete action-level threat envelope remain open. Production healing remains disabled: neither `PriestDirectHealAction` nor a Priest Healing Strategy is registered, and the full direct-heal threat estimate remains Unknown.

### F54h — three-cast nested target-trigger chain (trusted-runtime validated)

- One temporary Aura SpellEntry (Inspiration 14892) carries two independently configured `SPELL_AURA_ADD_TARGET_TRIGGER` effects with disjoint family masks. Real Core execution follows `Prayer of Healing (596)` root → `Lesser Heal (2052)` child → `Greater Heal (2060)` grandchild. Root matches effect 0 / mask A; child has mask B and matches effect 1; grandchild retains neither mask, terminating the chain.
- Three measured stages isolate each level: root-only baseline; root + child with effect 1 disabled; then root + child + grandchild with both Aura effects active. Trusted marker: `rowReady=1 rootCast=1 rootTargets=5 stage2AuraAttached=1 stage2Cast=1 childTargets=5 stage2AuraRemoved=1 stage3AuraAttached=1 stage3Cast=1 grandchildTargets=5 threatMatched=1 stage3AuraRemoved=1`.
- Child effective gains across the five real Group Players: `76/78/77/77/77`. Grandchild effective gains: `756/755/755/755/755`. Both hostile Creatures' actual `ThreatManager` deltas matched the sum of separately calculated root, child and grandchild healing-assist threat for every recipient.
- The fixture restored both temporary Aura effect slots, every modified root/child/grandchild SpellEntry field, and all five Players' original health. It removed the Aura after each stage and verified `auraRowsRestored=1 rootRowRestored=1 childRowRestored=1 grandchildRowRestored=1`.
- Trusted push [#45774494967](https://github.com/tyrael9/core/actions/runs/45774494967) on `88aacb4984cc1eeda617df4524c9491bf7eca746` passed pure PartyBotEngine tests, report collector, Configure, full Linux ARM64 / Vanilla 5875 Server+Realm build, harness syntax, fixture-secret validation and the complete Core-backed simulator through final `Runtime PASS`. The nested-fixture implementation is in [code commit `137f301`](https://github.com/tyrael9/core/commit/137f30189c3c25e8e97306c9b917972683f7f7d8); its required runtime marker is enforced by `88aacb`. Runtime artifact: [#15095362705](https://github.com/tyrael9/core/actions/runs/45774494967/artifacts/15095362705). Matching PR-safe build [#45774502681](https://github.com/tyrael9/core/actions/runs/45774502681) passed; secret-backed runtime is intentionally skipped in PR context.
- This proves the representative three-cast recursive path, not every possible depth or proc/trigger combination. Deeper/arbitrary nested chains and the full Priest action-level threat envelope remain open; `PriestDirectHealAction` and the Priest Healing Strategy remain unregistered.

### F54i — real same-millisecond Core Heal/Damage ordering is observed; projection fails closed (trusted-runtime validated)

- The Core fixture synchronously executes a real `DealHeal(40)` → `DealDamage(30, SELF_DAMAGE)` → `DealHeal(40)` sequence against the same Player. It samples `WorldTimer::getMSTime()` immediately around each call and only accepts a fixture attempt when all six samples occupy one actual millisecond bucket.
- Latest trusted marker: `captured=1 attempts=1 coreOrder=1 sameTimestamp=1 projectionRejected=1 firstHeal=40 damage=30 secondHeal=40 observedHealth=1667 firstMs=73685 damageMs=73685 secondMs=73685`. The observed Core health deltas prove the synchronous H-D-H call order (40 / -30 / 40); all three snapshot timeline arrivals carry exactly the same timestamp.
- The fixture builds the three real observed events and requires `ProjectHealingTimeline` to reject the mixed Heal/Damage timestamp, return an invalid projection, and leave its projected event count/final health empty. This records the distinction: Core call order is observable in a live synchronous execution, but a value-only millisecond snapshot cannot infer that order later and must remain fail-closed.
- Code fixture: [`bf0f390`](https://github.com/tyrael9/core/commit/bf0f3905f8b0381df1040a8cc630476ded6871a4); the runner's mandatory F54i marker gate is in [`bd37b68`](https://github.com/tyrael9/core/commit/bd37b6846d5ea64d7d170433156709fd1c66005a).
- The same trusted runtime also revalidated the Mind Flay channel cancellation branch. The previous coarse 12 × 250 ms probe could miss the channel/Aura overlap; [`7db8d60`](https://github.com/tyrael9/core/commit/7db8d6038899b5b81801fd7af4b8924c25e184e7) now polls Core state at 100 ms intervals for up to 8 seconds, without relaxing the required conjunction of `CURRENT_CHANNELED_SPELL`, `SPELL_STATE_CASTING`, `IsChannelActive()` and the real target Aura. This run observed `rowPresent=1 isChannel=1 castResult=255 prepared=1 polls=2 sawChannelSlot=1 sawTargetAura=1 channelStarted=1`; every `CancelOwnCastAction` assertion, including threat veto, exact Core Interrupted terminal, Aura removal and no-further-ticks, was 1.
- Trusted push [#45779336244](https://github.com/tyrael9/core/actions/runs/45779336244) on `7db8d6038899b5b81801fd7af4b8924c25e184e7` passed pure PartyBotEngine tests, the read-only report collector, Configure, full Linux ARM64 / Vanilla 5875 Server+Realm build, simulator harness syntax, fixture-secret validation and the complete Core-backed simulator through final `Runtime PASS`. Runtime artifact: [#15096974787](https://github.com/tyrael9/core/actions/runs/45779336244/artifacts/15096974787). Matching PR-safe build [#45779345935](https://github.com/tyrael9/core/actions/runs/45779345935) passed its build and syntax checks; secret-backed runtime is intentionally skipped in PR context.


### F54j — live Damage Pushback, refreshed in-flight-heal snapshot and interruption invalidation (trusted-runtime validated)

- The real non-Engine Priest Lesser Heal (2052) was captured in PREPARING state. Its SpellEntry had `DAMAGE_PUSHBACK` set and `DAMAGE_CANCELS` clear (`spellFlags=15`).
- Real 1-point caster `SELF_DAMAGE` invoked Core `Spell::Delayed()`; the same Spell timer increased from 1,400 ms to 1,500 ms (+100 ms). The fixture compared the live Spell's `GetCastedTime()`, not a synthesized delay.
- After cache invalidation and rebuild, snapshot epoch advanced 4→5 and the value-only caster snapshot carried the exact 1,500 ms remainder. `AppendInFlightDirectHealForecastEvent` produced one event with that timestamp and this snapshot's recalculated amount. The amount may change between snapshot generations (52→50 here); it is only required to match its own snapshot.
- The exact Spell was interrupted through Core. A second cache invalidation/rebuild advanced epoch 5→6, emitted no active direct-heal amount or remaining-cast timer and produced an empty in-flight heal event list. The target health remained unchanged; original caster health/Mana and target health were restored.
- Mandatory trusted marker: `captured=1 spellEligible=1 castAccepted=1 preSnapshot=1 casterDamage=1 damageTaken=1 timerIncreased=1 castTimeMs=1500 beforeTimerMs=1400 afterTimerMs=1500 pushbackDeltaMs=100 postPushbackSnapshot=1 beforeEpoch=4 afterPushbackEpoch=5 interrupted=1 postInterruptSnapshot=1 afterInterruptEpoch=6 noStaleHealCredit=1 targetUnchanged=1 cleanup=1 spellFlags=15 beforeAmount=52 afterAmount=50`. Implementation and gate are in [commit `971d550`](https://github.com/tyrael9/core/commit/971d550444a9c4061dc2101a0d86f4b8ea623b62).
- Trusted push [#45878776639](https://github.com/tyrael9/core/actions/runs/45878776639) passed all gates through final `Runtime PASS`, including the full ARM64 / Vanilla 5875 build. Artifact [#15145363280](https://github.com/tyrael9/core/actions/runs/45878776639/artifacts/15145363280). Matching PR-safe build [#45878786314](https://github.com/tyrael9/core/actions/runs/45878786314) passed; extracted-data runtime is skipped in PR context by design.
- This fixture validates one real single-target direct-heal pushback/interruption path. F54k has since closed random HoT bounds and F54l has since closed known-school absorb matching. F54m now implements a bounded four-cast/cyclic-fifth fixture but still awaits trusted runtime; deeper/arbitrary nesting and whole-action threat closure remain open. Production Priest healing stays disabled.
---


## Latest Phase 6 runtime evidence — F54d (2026-11-24 PT / 2026-11-25 UTC)

The F54d timer/schedule slice passed trusted Core runtime in [push #45541255150](https://github.com/tyrael9/core/actions/runs/45541255150), confirming a 1,500 ms live direct-cast remainder and a real Renew schedule of first tick 3,000 ms, period 3,000 ms and five remaining ticks. Artifact [#14990075861](https://github.com/tyrael9/core/actions/runs/45541255150/artifacts/14990075861).

The actual effective-healing/concurrency slice passed in [push #45547778038](https://github.com/tyrael9/core/actions/runs/45547778038). The trusted log recorded direct actual gain 69/direct overheal 99, one actual Renew tick with effective gain 1 from an expected 9 and conservative overheal lower bound 8, and concurrent same-time healing effective total 150/overheal 50 with attribution unknown. The dedicated F54d PASS marker and final `Runtime PASS` were present. Artifact [#14992557137](https://github.com/tyrael9/core/actions/runs/45547778038/artifacts/14992557137).

These are isolated fixture-backed Core observations only. Push #45558184533 passed two Core DealHeal calls separated by SELF_DAMAGE DealDamage; projected effective=80/damage=30/overheal=0 matched observed and projected final HP 1667. This proves ordered health application for this fixture only, not hostile attack interleaving or full threat generation. F54i separately validates the same-millisecond mixed Heal/Damage fail-closed boundary. No Priest healing policy is enabled in production. F54j's tested direct-heal pushback/interruption path is trusted-runtime validated; F54k random HoT bounds and F54l known-school absorb matching have since passed trusted runtime. F54m's bounded child-chain fixture is implemented but awaits trusted runtime; complete Core ordering and whole-action threat/proc/recipient closure remain open.

---

## Current Core runtime validation status (2026-11-22)

**F46/F47/F48 are closed; F49

Implementation follow-up `2db0af075dc5ccb66ef561f927e6cf559befaa34` also extends both `PartyBotEngine.cpp` candidate identity comparisons with item GUID, entry, inventory position and requested ON_USE slot. This preserves distinct value-only inventory requests during continuation matching and pending-dispatch reconciliation. Trusted runtime validation remains pending. is being validated.** F46's 22,424-row SpellEntry audit found no ordinary positive self-utility spell with the required proc suppression. F47 inspected all 3,609 loaded item ON_USE slots and identified 29 fully bounded item-backed candidates. F48 executed Elixir of Minor Agility (item 2457 / spell 2374) through the actual Core Player item-use path and verified aura/stat application, typed child/root, stack mutation, cooldown rejection and cleanup; trusted push [#45097019380](https://github.com/tyrael9/core/actions/runs/45097019380) reached final `Runtime PASS`, with matching PR build [#45097024271](https://github.com/tyrael9/core/actions/runs/45097024271). F49 adds a simulator-only Engine-selected CoreAction contract with value-only item identity, live inventory/cooldown preflight, action-ID/root correlation and a stale-item race fixture. It is not registered in production NewEngine; trusted runtime validation remains pending.

**F42 is closed, including delayed cancellation ordering.** Trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749) passed pure tests, report collector, configure, full Linux ARM64 build, full fixture-backed Core simulator and final Runtime PASS. In addition to the real item-118 single-slot success/zero-charge rejection and synthetic two-ON_USE-slot prototype, it runs real Brimstone (item 1402) / Fireball (spell 143) delayed ON_USE cancellation before and after Core charge mutation. The fixture proves pre-mutation interruption leaves charges `-5>-5`; post-mutation interruption records `-5>-4` before effects, leaves target health unchanged and settles the child as Interrupted. Artifact: [partybot-simulator-dfdfdf0](https://github.com/tyrael9/core/actions/runs/44852141749/artifacts/14685618923).

**F43 now has seventeen generic Core adapters, all trusted-runtime validated.** All seventeen remain deliberately unregistered in production NewEngine. CancelOwnCastAction's PREPARING/replacement, delayed Fireball and channelled Mind Flay matrix passed trusted push [#45063532285](https://github.com/tyrael9/core/actions/runs/45063532285) with final Runtime PASS and artifact [#931b5e3](https://github.com/tyrael9/core/actions/runs/45063532285/artifacts/14770955128); PR ARM64 build [#45063539061](https://github.com/tyrael9/core/actions/runs/45063539061) passed. SelectHostileTargetAction accepts a live hostile and rejects a friendly Creature supplied by a stale positive HostileSnapshot without selection/target/victim side effects. ClearIdleTargetAction clears a deliberately stale, non-resolving Player selection GUID only after live preflight proves there is no victim, combat/melee state or active non-melee Spell. StopUnattackableTargetAction rejects stale negative victim snapshots and calls only AttackStop after a live Core recheck proves the current victim is no longer attackable. FollowPartyMemberAction uses a GUID-only current-follow snapshot, starts and retargets Core's native FollowMovementGenerator, rejects a repeated same-target request without restarting, and rejects a stale roster member after it is removed from the live Group. StopFollowingAction requires a live current Follow generator/target GUID match and uses lifetime-safe `MotionMaster::MovementExpired(false)`; trusted push [#44864122550](https://github.com/tyrael9/core/actions/runs/44864122550) verified `registered=1 snapshot=1 staleTargetRejected=1 stopped=1 pointMovementPreserved=1` and final Runtime PASS. Artifact: [partybot-simulator-221e5a3](https://github.com/tyrael9/core/actions/runs/44864122550/artifacts/14691023636).

Trusted push [#44864122550](https://github.com/tyrael9/core/actions/runs/44864122550) validated all five F43 adapters, passed the scoped F24 trace-detail gate and reached final Runtime PASS. StopFollowing marker: `registered=1 snapshot=1 staleTargetRejected=1 stopped=1 pointMovementPreserved=1`; existing Follow marker: `registered=1 firstStarted=1 followTargetCaptured=1 sameTargetNotRestarted=1 retargetRegistered=1 retargeted=1 firstLeaderRemoved=1 staleSelected=1 staleGroupRejected=1`. PR build [#44864129963](https://github.com/tyrael9/core/actions/runs/44864129963) passed full ARM64 build and harness checks. Artifact: [partybot-simulator-221e5a3](https://github.com/tyrael9/core/actions/runs/44864122550/artifacts/14691023636).

F42's required delayed ON_USE and cancellation-before/after-charge-mutation evidence is closed. Additional count-based expendable-item and item-as-reagent coverage is not yet included. F43's seventeen generic adapters, including CancelOwnCastAction, all have action-specific live preflight and trusted fixture-backed accepted/rejected/terminal/cleanup evidence. The complete PREPARING/replacement, delayed Fireball and channelled Mind Flay lifecycle matrix passed trusted push [#45063532285](https://github.com/tyrael9/core/actions/runs/45063532285) with final Runtime PASS; matching PR ARM64 build [#45063539061](https://github.com/tyrael9/core/actions/runs/45063539061) passed. All seventeen adapters remain unregistered in production NewEngine, and this does not establish playable NewEngine combat.

**F44 is trusted-runtime validated and closed as a simulator/test-only policy slice.** Push [#45074121261](https://github.com/tyrael9/core/actions/runs/45074121261) ran the real five-Player Group PartySnapshotBuilder scenario and emitted the required handoff PASS marker plus final Runtime PASS; the five-member roster, role IDs, Main Tank victim, healer-peel reference, staged selection/attack, action-driven invalidation, explicit host retarget invalidation, fresh handoff snapshot and complete cleanup all passed. Matching PR ARM64 build [#45074126327](https://github.com/tyrael9/core/actions/runs/45074126327) passed. Artifact: [partybot-simulator-f50c79e](https://github.com/tyrael9/core/actions/runs/45074121261/artifacts/14774942008). Initial pure policy matrix also passed; the policy remains unregistered in production NewEngine.

MoveDistanceAction is the sixteenth F43 generic adapter. It wraps Core MotionMaster::MoveDistance, delegates destination/LOS/PathFinder reachability to Core, and requires a live current victim plus matching current Chase or party Follow. Native DISTANCING_MOTION_TYPE temporarily overlays the existing targeted generator; Core retains and restores the same generator on expiry. Trusted push #44998862867 (https://github.com/tyrael9/core/actions/runs/44998862867) logged followStarted=1, followRosterCaptured=1, followSelected=1, followDistanceStarted=1, followPreserved=1, followRestored=1, chaseSelected=1, chaseStarted=1, chasePreserved=1, followStillPresent=1, chaseRestored=1, pointRejected=1, victimRaceRejected=1 and cleanup=1, plus the expected accepted Core reason and final Runtime PASS. Matching PR ARM64 build #44998866883 (https://github.com/tyrael9/core/actions/runs/44998866883) passed. Artifact: partybot-simulator-2beecf67 (https://github.com/tyrael9/core/actions/runs/44998862867/artifacts/14744887692).

## F43 — exact own-cast cancellation (trusted-runtime validated)

The real Core Player fixture attaches a PlayerAI observer that assigns unique Engine root action IDs to two successive non-triggered Lesser Heal casts. PartySnapshotBuilder must copy the current Spell's action ID, encounter ID, trace target, Engine-origin bit, Core unit-target GUID, state and current container as scalar values.

The fixture retains the first cast's snapshot, interrupts that cast through Core, and starts a replacement cast with the same spell ID and target. Executing the old snapshot must fail live preflight and leave the replacement current. A fresh snapshot must select CancelOwnCastAction; the adapter calls SpellCaster::InterruptSpell on the exact current Generic slot, and Core reports one matching Interrupted terminal. A second execution using the now-stale snapshot is rejected. Patient health and Player mana are required to remain unchanged, and group/player/creature teardown is asserted.

The trusted fixture validates PREPARING cancellation and stale/replaced/finished identity rejection. It drives Fireball (133) through the Core cast timer into DELAYED; Engine evaluation rejects the Unknown threat estimate, and an isolated CoreAction wrapper call proves the later projectile impact is suppressed. A real Mind Flay (15407) is advanced through Map::Update into CHANNELED/CASTING; Engine evaluation again rejects Unknown threat, while the isolated wrapper verifies the Interrupted terminal, empty current channel slot, zero UNIT_CHANNEL_SPELL field, target aura removal and no further periodic ticks. Direct wrapper calls validate Core lifecycle cleanup only and do not authorize normal Engine dispatch. PREPARING alone receives NoThreat. The fixture reports every identity, stale-race, fail-closed, terminal, effect-suppression, aura-removal and cleanup assertion as 1 and reached final Runtime PASS in trusted push [#45063532285](https://github.com/tyrael9/core/actions/runs/45063532285); matching PR ARM64 build [#45063539061](https://github.com/tyrael9/core/actions/runs/45063539061) passed. Artifact: [partybot-simulator-931b5e3](https://github.com/tyrael9/core/actions/runs/45063532285/artifacts/14770955128). State/action identity remains exact and the adapter remains unregistered in NewEngine.

## Purpose

Build a repeatable headless combat test environment that runs PartyBotEngine against the **real pinned vMaNGOS Core combat implementation**. It shortens the AI development loop: define a party, enemy behavior and encounter timeline; run deterministic ticks; inspect structured events; change strategy code and rerun without asking a player to construct the same fight in-game.

This is an integration-test layer between pure decision-engine tests and live realm validation, not a replacement for live testing.

## Non-negotiable properties

1. **Real Core semantics.** Damage, threat, victim selection, spells, auras, cooldowns, movement and death must come from the pinned Core. A toy combat model is acceptable only for pure strategy unit tests, never as evidence of Core combat behavior.
2. **Headless.** No WoW client or network client session is required.
3. **Isolated.** Tests run in a disposable process and isolated test data/database configuration. They must never connect to the Oracle realm, reuse its credentials, mutate its data volumes or run as part of deployment.
4. **Deterministic where controllable.** Scenario inputs, tick progression and RNG seed are explicit. Remaining nondeterminism (wall-clock, asynchronous map work, unordered iteration, DB-backed state) must be surfaced and reduced before a scenario is labelled deterministic.
5. **Observable.** Each run emits scenario ID, seed, tick, actor GUID, decision trace, Core action result, victim/threat/health/death transitions and a final assertion report.
6. **Safe by default.** A failed assertion or crash ends only the test process. No production server or persistent player/instance state is touched.

## Source audit: existing pinned-Core seams

The initial feasibility pass was performed against this branch's source.

### Useful APIs confirmed

- `MapManager::CreateTestMap(mapid, instanced, x, y)` and `DeleteTestMap(Map*)` are public. For an instanced map, the implementation allocates a fresh instance ID, constructs a `DungeonMap` for dungeon/raid map entries (otherwise `WorldMap`), calls `CreateInstanceData(false)` and registers the map. It deliberately does not call `SpawnActiveObjects()` in this test-map path.
- `Map` exposes public `Add(T*)`, `Update(uint32)`, `GetUnit(ObjectGuid)` and `GetCreature(ObjectGuid)`, candidate seams for adding actors and advancing a map without a client.
- `CreatureCreatePos` and `Creature::Create(..., CreatureInfo const*, ...)` construct a creature from a loaded Core creature template and explicit position.
- `Creature::AddToWorld()` registers the creature with its map and initializes AI if no AI was assigned. `Creature::AIM_Initialize()` initializes both the `MotionMaster` and template-selected AI. A test actor must call this lifecycle method first, then explicitly delete/replace the template AI with its probe AI; installing a custom AI before `Map::Add` skips `AIM_Initialize` and leaves an empty `MotionMaster`.
- `Unit::DealDamage` is the real damage/death path. `Unit::AddThreat` forwards to the owning unit's real `ThreatManager`; the manager exposes threat-list values and victim state.
- `Unit::Attack` performs Core attack-state and attacker-reference bookkeeping and rejects dead, deleted or out-of-world targets.

These are promising seams. They do **not** yet prove that a standalone binary can initialize all required global services or that the full combat update loop can be driven safely outside the normal world process.

### Initialization dependencies

The test-map constructor is not lightweight. It reads global Core services including map DBC storage, ObjectMgr temporary GUID state, terrain, persistent-state manager, world configuration, weather and creature-group services; continent maps may also create worker pools. `CreateTestMap` calls instance-data initialization.

Creature construction depends on initialized creature templates/DBC and world configuration. Adding a creature invokes map registration and AI initialization unless a test AI is installed first.

Therefore, do not instantiate `Map` directly (its constructor is protected), or write a fake Map. The harness must reuse normal Core startup initialization for the services required by the selected test map.

## Recommended execution architecture

### Stage A — isolated Core bootstrap smoke test

Implemented as a configuration-gated one-shot mode in the existing `mangosd` executable. It is intentionally smaller than the eventual simulator:

1. run the normal Core initialization path using a dedicated disposable configuration/database;
2. create a fresh dungeon/raid test map through `sMapMgr.CreateTestMap`;
3. assert that the map is registered and remains registered before the combat scenario;
4. delete it through `sMapMgr.DeleteTestMap` and assert the registry entry is gone;
5. set the process exit code from the assertions and reuse the normal WorldRunnable shutdown path.

The mode skips world/RA/SOAP listeners, realm-online/offline updates, daemon/PID-file behavior, account cleanup and mass-mail processing. Before connecting to any DB it verifies that all four configured schema names begin with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`). This guard prevents accidental use of the ordinary realm schema names; the schemas must still be disposable copies.

This lifecycle check proves only that the real Core can create, register and delete an instance map in headless mode. It does not create actors or prove combat. The first map tick is deliberately deferred until Stage B has inserted actors with initialized movement state.

**Startup hook found:** `Master::Run()` calls `sWorld.SetInitialWorldSettings()` before it launches `WorldRunnable` and before `sWorldSocketMgr.StartWorldNetworking()`. This is the first practical hook: required world data/managers have been initialized, and the world listener has not yet been opened. A test-mode branch can therefore skip the listener and normal world loop while reusing the real startup path.

**Important caveat:** `SetInitialWorldSettings()` is not read-only. It writes startup/maintenance data (including the `uptime` record), starts background workers (character DB, async packets, anticheat and possibly LFG), and the Master has already started IO-context runner threads. Simulator mode must use a dedicated disposable database/configuration and reuse the normal shutdown/cleanup path. Simply returning from `Master::Run()` after a test would leak threads and leave services uncleanly stopped.

The map-level `Map::Update(diff)` accepts an explicit diff, but internally still reads wall-clock values and global services (including the movement broadcaster); it also updates sessions, cells, weather, scripts and instance data. A fixed diff alone does not make full-map ticks deterministic. Stage B must determine whether an instance-map tick can be driven safely or whether a test-only clock seam is required. Do not alter production tick semantics to simplify tests.

### Stage B — first real combat proof

Start small and avoid player/group complexity:

- one test-controlled tank-like creature;
- one test-controlled damage dealer;
- one hostile creature with a threat list;
- fixed RNG seed and explicit tick schedule.

Assertions must prove from Core-owned state:

- actors exist on the same real test map;
- `Unit::Attack` establishes actual attacker/victim state;
- `Unit::DealDamage` changes real health and reaches the real death path;
- `Unit::AddThreat` creates/updates real `ThreatManager` references;
- threat ordering and current victim change as expected when the actual Core selection/update path runs;
- cleanup removes every actor and the temporary map.

A test is not successful merely because a helper directly writes health or a fake threat number. Inspect Core-owned objects after Core methods/ticks run.

The exact mechanism for forcing victim reselection (normal CreatureAI update, threat-event processing or another Core update path) remains an implementation audit item. Do not substitute hand-coded target selection.

### Stage C — Engine-to-Core attack executor (runtime CI passing)

- Build a five-role value-only PartySnapshot from same-map simulator actors.
- Let PartyBotEngine select a tank-assist candidate and execute a real Core `Unit::Attack`.
- Verify the actual Core victim, action result and actor cleanup.

### Stage D — triggered spell-effect proof (runtime CI passing)

- Let the healer trigger select an injured party member.
- Execute Vanilla Lesser Heal (2050) through the real Core Spell/SpellEffects path as a triggered Creature cast.
- Verify the Core cast result and actual health increase.

### Stage E — normal Player spell lifecycle (runtime CI passing)

- Construct a real Core `Player` with a local `WorldSession` in the isolated simulator process.
- Require spellbook ownership and let the Engine select the heal.
- Call ordinary non-triggered `Player::CastSpell(..., false)`; verify acceptance, pending cast, GCD, mana cost, normal completion and actual healing.
- Logout and delete the Player through the Core session/map cleanup path.

This remains a simulator fixture, not a production-authenticated PlayerBot or five-member Core Group. Detailed assertions and limitations are recorded below.

### Stage F — movement and defensive action adapters

- [x] First Core point-movement primitive: PartyBotEngine derives a formation destination from the Main Tank snapshot, the adapter calls `MotionMaster::MovePoint`, and runtime CI verifies the real position reaches the destination.
- [x] Player movement-interrupt fixture: the Engine selects the formation destination, and the simulator models the inbound mover-relocation operation by calling `Player::SetPosition`. This enters the native `Player::SetPosition` → `Unit::HandleInterruptsOnMovement` path; the simulator does not inspect spell flags or cancel spells itself. Runtime CI run `#44145923142` proves `castCancelled=1`, patient health `50000>50000`, movement completion and cleanup.
- [x] Add continuous follow/formation tracking on native Core movement generators, Player cast pause/resume, Main Tank handoff and recovery after target removal. Runtime CI `#44159191502` and `#44163805016` pass these distinct Player/Creature slices.
- [x] Stage F6 action-time range/resource revalidation: CI `#44167535205` confirms Core rejects the stale out-of-range and late no-mana Player heals without side effects.
- [x] Stage F7: real dynamic GameObject LOS rejection and `CAN_NOT_MOVE`/`POSSESSED` movement rejection passed in CI `#44170264755`.
- [x] Stage F8: create and tear down a real five-member Core Group and verify production PartySnapshotBuilder reads its same-map Player roster. CI `#44175017402` passed with five members, accurate value copies, cache reuse, Group/ObjectMgr removal, Player session logout and Map detachment.

### Stage G — dungeon encounter simulation

After the action/executor layer and party-wide combat behavior are stable, evaluate loading a real dungeon map with instance scripts/data. This adds grid/terrain/vmap loading, instance state, scripted creature AI, doors and encounter lifecycle. Full dungeon loading is not a prerequisite for the current Core-backed action proofs.

## Scenario format

Use declarative scenario files (JSON or a small C++ builder initially) with scenario name/schema version, map ID and instance flag, RNG seed, actor templates/roles/positions/test AI, initial health/power/threat, timed events, expected assertions, maximum ticks and wall-clock timeout.

Initial scenarios:

| Scenario | Core behavior under test |
|---|---|
| `basic_damage_death` | Real damage, health transition and death cleanup |
| `threat_ordering` | Real threat list values and victim selection |
| `tank_loses_aggro` | Damage dealer overtakes tank and bot response |
| `healer_pulls_aggro` | Healing threat/hostile retarget and tank peel |
| `tank_dies` | Tank death, add retarget and party recovery |
| `healer_dies` | Healing coverage loss and response |
| `multiple_adds` | Multi-target coordination and target reservations |
| `healer_mana_low` | Real power state, spell costs and recovery policy |

## Determinism and performance

- Advance the map with fixed integer millisecond diffs; do not sleep to simulate time.
- Seed every Core RNG facility used. If RNG is process-global, run scenarios in separate processes or add a scoped test seed/reset API.
- Disable unrelated background services and map worker pools only where Core safely permits it; do not change production combat semantics.
- Start with one map and one update thread. No parallel scenarios until isolation is proven.
- Bound ticks, actors, event buffers and wall-clock duration. Emit the seed and complete scenario input on failure.
- Record compiler, Core SHA, data build and scenario schema version in every report.

## Boundaries and non-goals

- This harness is not an AI and must not grow strategy logic.
- It must not use the live Oracle host or its databases, configs, logs or volumes.
- It must not auto-publish or deploy a server image.
- It does not replace live tests for networking, disconnect persistence, client packets, real player behavior or long-duration stability.
- It must not claim deterministic replay until RNG, tick progression, map threading and external data dependencies are controlled.
- It does not replace pure C++14 decision tests; both layers are required.


### Stage B source audit — actor lifecycle and combat semantics

The pinned source audit now confirms the following exact lifecycle:

- Construct a temporary actor with `Creature::Create(guidLow, CreatureCreatePos(map, x, y, z, o), creatureInfo, firstCreatureId)`. The template must come from the loaded `sObjectMgr.GetCreatureTemplate(entry)`; reject a missing template and do not fabricate a `CreatureInfo`.
- Call `Creature::AIM_Initialize()` before installing the simulator-owned AI. This Core lifecycle method initializes the `MotionMaster` and creates the template-selected AI. `Creature::SetAI` is a raw pointer setter; explicitly delete the template AI before replacing it with the probe AI. Do this before `Map::Add(creature)` so `Creature::AddToWorld` does not replace the probe with a factory AI. The Creature destructor owns and deletes the currently installed AI.
- `Map::Add(Creature*)` sets the map, creates/loads the grid as required, inserts the creature into the grid, calls `AddToWorld`, adds active objects, registers the viewpoint and visibility, and invokes `CastSpawnSpell()`. Test templates must therefore be checked for spawn spells and other template-driven side effects; a custom AI alone does not suppress those.
- `Creature::AddToWorld` registers a normal unit GUID with the map before `Unit::AddToWorld`, then initializes AI only if none was supplied. Avoid database-spawn GUIDs and creature-group/linking behavior for simulator actors.

**Cleanup is safety-critical.** Do not call `Map::Remove(creature, true)` for a temporary simulator actor: the normal remove path calls `CleanupsBeforeDelete`, may call `SaveRespawnTime()`, and deletes the object. The test-specific sequence should be: stop the actor's combat/AI activity, call `Map::Remove(creature, false)` while it is still on the map, then delete the detached object explicitly. The `false` path calls `RemoveFromWorld`, unlinks it from the grid and resets its map without saving respawn time or deleting it. `Creature::~Creature` calls `Unit::CleanupsBeforeDelete` before destroying its AI, so references and combat state are cleaned during explicit destruction. Only after every actor is detached and destroyed may the harness call `MapManager::DeleteTestMap`; the map destructor unloads its remaining grids.

Combat API semantics confirmed:

- `Unit::Attack(victim, meleeAttack)` requires both units alive and the victim in-world; it establishes attack state and attacker references. It is not itself a damage event and does not by itself establish a victim threat entry.
- `Unit::DealDamage(..., DIRECT_DAMAGE, ...)` is the real health/death path. The normal hostile-damage path adds threat to the victim through `victim->AddThreat(attacker, threat, ...)`.
- `Unit::AddThreat` only forwards to `ThreatManager` when the receiving unit can own a threat list and both units are on the same map.
- `ThreatManager::getHostileTarget()` updates/sorts the real threat container and calls `ThreatContainer::selectNextVictim`. The pinned selector includes the normal 110% melee / 130% ranged switching thresholds and target-validity checks. A test that calls this method directly proves Core threat selection, but does not prove that a Creature AI automatically invokes it during a map tick.
- `CreatureAI::UpdateAI(diff)` is the intended test-AI update seam. The simulator must verify from the complete Creature/Map update path that this callback is reached on the selected map tick, rather than assuming that `Map::Update(50)` alone advances every creature.


A further tick-path constraint was confirmed in `Map::UpdateCells`: it visits cells around map players and active non-player objects. An ordinary temporary Creature in an otherwise empty headless instance is not enough to guarantee that its `Creature::Update` (and therefore `CreatureAI::UpdateAI`) is called. The simulator must mark its test actors active with `SetActiveObjectState(true)` before adding them, then assert the AI callback counter actually advances after a map tick. This uses the Core's existing active-object update path; it is not a substitute call to the AI. The cell-update cadence is wall-clock/config driven, so a single immediate `Map::Update(50)` may still not cross the configured cell-update interval. The harness needs a bounded way to satisfy that cadence and must report whether the tick updated the actor rather than assuming it did.

The first Stage B proof should be deliberately split into independently reported assertions: (1) template-backed actors can be created, inserted and found on the map; (2) real `DealDamage` changes health and creates the expected threat reference; (3) the real threat selector chooses and switches victims at the Core thresholds; (4) custom AI receives bounded updates; and (5) all actors are removed without respawn persistence before the test map is deleted. A passing direct-damage/threat proof must not be described as a full autonomous combat simulation.

### Stage B implementation now present (Core runtime assertions and push CI passing)

The simulator now contains a bounded first Core-backed Creature/threat scenario:

- Select a normal-rank creature template deterministically from loaded templates, requiring a display and excluding NPCs, civilians, racial leaders, pets, immune templates, and templates with spawn spells, totem spells, default auras, spell lists, scripts, named AI, invisibility or the no-threat-list flag.
- Select a mutually hostile faction-template pair from the loaded Core faction data, rather than hard-coding Vanilla faction IDs.
- Create one threat-owning Creature and two hostile Creature actors from that real template. Each is inserted with a Core-generated map-local GUID, first runs `AIM_Initialize()`, then has its template AI explicitly replaced with a small probe AI before insertion, is marked active before `Map::Add`, and is verified through both `IsInWorld()` and `Map::GetCreature(guid)`.
- Drive bounded real `Map::Update(50)` calls, paced with short wall-clock sleeps, and require all three probe AIs to receive `UpdateAI`.
- Emit exact health progression, applied damage, tank/DPS threat values, selected target GUIDs at each threshold step, AI update counts and per-actor cleanup results in the simulator log.
- Deal real direct damage to the threat owner from the two hostile actors (100, 105, then +10), verify health loss and damage-generated threat references, and check that the Core threat selector retains the first target below the 110% melee threshold and switches above it.
- Clear combat/threat references, detach each actor with `Map::Remove(actor, false)`, and verify it is no longer in-world or registered in the map before explicit destruction.

This Stage B section is deliberately a **Core Creature damage/threat smoke test**, not an autonomous encounter simulation. Later Stage C–E sections document the separately implemented Engine attack and spell/heal slices; movement, boss scripts and multi-phase encounter mechanics remain future work. The probe AI only counts update callbacks. Actor stats are normalized to a large health pool to keep the test damage non-lethal. The tick loop is bounded but still depends on Core wall-clock cell-update cadence; this does not establish deterministic replay.

## Current status and next gate (2026-11-20)

The simulator is now a substantial fixture-backed test layer for the pinned Vanilla 5875 Core; it is not a full autonomous dungeon/raid simulator and does not authenticate real production PlayerBots.

The most recent complete source-code validation is trusted Push CI [#44821298517](https://github.com/tyrael9/core/actions/runs/44821298517), source head `1fe34c90a848b589a042a8f2077b0647eb0b00ad`. It passed the standalone PartyBotEngine tests, report collector tests, Linux ARM64 `mangosd` + `realmd` build, simulator harness checks, full extracted-data fixture download/checksum, disposable-schema run and final Core simulator `Runtime PASS`. Runtime output proves F39 natural Player point arrival/continuation/forced interruption, F40 real potion effect and item consumption plus zero-charge rejection, and F41 CoreAction map-thread resolution with missing-target/stale-map/stale-instance rejection. Artifact [partybot-simulator-1fe34c9](https://github.com/tyrael9/core/actions/runs/44821298517/artifacts/14672696314) was uploaded. The follow-up pure C++14 resolver-propagation regression passed PR CI [#44822853602](https://github.com/tyrael9/core/actions/runs/44822853602); the matching trusted push runtime job is in progress for that same source head.

Fixture-backed coverage includes:

- Real Core map/Creature lifecycle, AI update, damage, ThreatManager references, native target selection and cleanup.
- Engine-selected `Unit::Attack`, triggered Creature healing and ordinary non-triggered Player Lesser Heal through Core cast acceptance, pending/GCD, mana, completion and actual health change. The Player uses a local no-socket WorldSession, not a production account/session.
- Real five-Player Core Group roster and production PartySnapshotBuilder; multi-add threatAssist/healer threat, Main Tank and healer death cleanup/recovery, companion Pet threat snapshots and stale-reference removal.
- Core-derived MovePoint and FollowMovementGenerator behavior, target handoff/recovery, follow pause/resume during no-movement casting, Core range/resource/LOS/control-state checks, and native relocation-driven cast interruption.
- Source/destination Map/instance transfer and teardown, encounter trace lifecycle, party-wide trace-budget overflow, ordinary member sampled health/death/resurrection, PartyBotAI per-hit damage/death, Trigger and candidate prioritization, Engine action-to-Core Spell terminal correlation and NewEngine authority isolation.
- F35 typed AND/alternative prerequisites, F36 mixed ActionResult + Core Spell terminal prerequisites, F37 idempotent settlement, F38 full-PendingActionQueue ready-backlog preservation and F39 correlated Player point-movement terminal results are covered by pure regressions and trusted push fixture runs. The F39 Player fixture verifies natural spline arrival within destination tolerance, movement-terminal continuation release and next-dispatch execution, plus forced ClearType cleanup producing Interrupted.

The fixture-backed Player and Group scenarios prove selected Core APIs and lifecycle seams; they are not a full authenticated production PlayerBot party, class rotation, continuous dungeon AI or boss encounter. Deterministic replay and Stage G dungeon/raid encounter scripting remain future work. F39 proves a typed terminal-result seam for opt-in Player point movement only. F42 is fully closed by trusted Runtime PASS [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749), including real-Core delayed-use cancellation before/after charge mutation. F41 proves the CoreAction resolver gate. F43's five generic target-maintenance/follow actions have real Core fixture coverage; trusted push [#44864122550](https://github.com/tyrael9/core/actions/runs/44864122550) passed all five adapter fixtures and reached final Runtime PASS, but none is registered as production policy. The simulator still does not establish playable NewEngine combat, class rotations, continuous dungeon AI or boss encounters.

The runner checks all four DB names are isolated under `partybot_sim_*`, reuses normal Core startup/shutdown while skipping public listeners, and never connects to or modifies the Oracle realm. The workflow has no image-publication or deployment step.

### Optional GitHub Actions runtime runner

The repository now contains `tools/partybot-simulator/run-ci.sh`, wired into the ARM64 build workflow as a push-only optional step. It is a no-op until both repository Actions secrets below are configured:

- `PARTYBOT_SIMULATOR_DATA_URL`: HTTPS read-only download URL for a `.tar.gz` archive of the complete extracted-data directory, preserving `5875/dbc/`, `maps/`, `vmaps/` and `mmaps/` at the archive root.
- `PARTYBOT_SIMULATOR_DATA_SHA256`: SHA-256 checksum of that archive.

The database readiness check deliberately requires a TCP connection to `127.0.0.1:3306`, not the container's local socket: the deployment image starts a temporary socket-only MariaDB during seed import before bringing up the normal TCP listener. This avoids launching `mangosd` while the database is still initializing.

The runner extracts the full archive into its isolated `DataDir` and checks that the required DBC files and all three world-data directories are present and nonempty. This supports later terrain, collision, navigation and map-content scenarios without uploading another fixture. It starts the same public vMangos database image used by the pinned deployment wrapper, clones its `mangos`, `characters`, `realmd` and `logs` schemas to `partybot_sim_*`, and points all four Core connections exclusively at those disposable copies. It then runs the one-shot `mangosd` simulator and uploads both console and file logs as a 14-day Actions artifact. It never reads or connects to the Oracle realm.

From the deployment directory, create a single archive while preserving the extracted-data directory structure:

```sh
tar -czf extracted-data-5875.tar.gz -C storage/mangosd/extracted-data maps mmaps vmaps 5875
sha256sum extracted-data-5875.tar.gz
```

On Windows PowerShell (run from the `vmangos-deploy` repository root):

```powershell
$source = ".\storage\mangosd\extracted-data"
tar.exe -czf .\extracted-data-5875.tar.gz -C $source maps mmaps vmaps 5875
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
(Get-FileHash .\extracted-data-5875.tar.gz -Algorithm SHA256).Hash.ToLowerInvariant()
tar.exe -tzf .\extracted-data-5875.tar.gz | Select-Object -First 12
```

The archive listing should start with paths such as `maps/`, `mmaps/`, `vmaps/` and `5875/dbc/`. This is the extracted server data, not the original WoW client `Data` directory.

Upload the archive to a storage service that can supply a stable HTTPS download. Google Drive links are supported through the pinned `gdown` downloader; direct HTTPS object URLs are also supported through `curl`, including a read-only OCI Object Storage Pre-Authenticated Request (PAR). For OCI, keep the bucket private and create a read-only PAR scoped to this single archive object; the PAR URL is a bearer secret, so store it only in the repository Actions secret `PARTYBOT_SIMULATOR_DATA_URL` and never commit or print it. Put the archive SHA-256 in `PARTYBOT_SIMULATOR_DATA_SHA256`. The runner downloads, verifies SHA-256, checks the extracted directory layout and only then starts the disposable Core simulator. Each trusted push to this branch runs the simulator after the full ARM64 build. If either secret is absent, the workflow records a skip rather than attempting a partial runtime test. Google Drive currently rate-limits the selected 5875 archive after repeated runner downloads; a direct HTTPS object URL is the preferred stable source for future runtime runs.

**Runtime update (2026-11-17):** Stage F4 passed on push CI `#44159191502` (`5d3c9c5`): the Core Player FollowMovementGenerator remained paused during five no-movement-cast gate ticks, the ordinary Lesser Heal completed with mana cost, and following resumed to a final separation of `2.00` with cleanup. Stage F5 then passed on CI `#44163805016` (`cfdda0c`): target handoff to a new Main Tank, tracking the new leader instead of the old one, removal of the active target and recovery to the remaining Main Tank all succeeded; final separation was `4.55`, with Follow state and cleanup intact. Stage F6 passed on push CI `#44167535205` (`e52bea6`): stale-snapshot range rejection returned `SPELL_FAILED_OUT_OF_RANGE` (`89`) with no health/mana/cast/GCD side effects, and late mana depletion returned `SPELL_FAILED_NO_POWER` (`77`) with no heal/cast/GCD. Stage F7 passed on push CI `#44170264755` (`e46a246`): Core `Spell::CheckCast` rejected the heal through dynamic GameObject Entry `13965` (`testedModels=13`, `blockedLOS=1`, result `42`), LOS restored after removal, and Core rejected MovePoint for both `CAN_NOT_MOVE` and `POSSESSED`. Stage F8 passed on push CI `#44175017402` (`434231f`): the actual Core Group was registered with five members and a Main Tank assignment; production PartySnapshotBuilder returned all five same-map Player GUIDs and correct scalar state; cache reuse, Core Disband/ObjectMgr removal, session logout and Map detachment all passed, followed by final Runtime PASS. The first Stage F8 attempt (`#44172739682`) had emitted a fixture-local PASS but failed later; the accepted run moved Group teardown after the Player interruption scenario and added explicit Map GUID cleanup assertions. Stage F9 passed on push CI `#44182086347` (`02f0d80`): the single five-Player Group passed initial same-map roster/value/cache checks; two Core hostile adds initially targeted Main Tank; a 20%-max-health nonlethal hit left Tank alive at `343/1717`; ordinary Lesser Heal completed (`1374>1423`, mana `2896>2866`); Core `threatAssist(400)` distributed assist threat and both adds selected healer (`212.2` each); production snapshot copied both threats/victims and Group/session cleanup passed, followed by final Runtime PASS. Stage F10 passed on push CI `#44185236970` (`1a463d8`): after the real five-Player Group's two adds selected the healer through Core `threatAssist`, normal Core lethal damage killed the Main Tank and removed its references from both ThreatManagers; native `SelectHostileTarget` retained the living healer. A refreshed production snapshot marked the Tank dead and copied both healer victims/threat values. Killing the healer then removed both healer references; native selection moved both adds to the surviving DPS, and a second refreshed snapshot marked Tank+Healer dead, DPS alive, copied the DPS as both victims, and contained no stale dead-player threat entries. Group removal, all session cleanup and final Runtime PASS succeeded. Stage F11 adds Core-derived melee reachability to threat snapshots so the engine can distinguish Core's 110% melee and 130% ranged aggro thresholds. Do not jump to dungeon scripts.


## Stage C — PartyBotEngine-to-Core executor vertical slice

The next integration step keeps the simulator on the same isolated test map and reuses the real template-backed Core Creature lifecycle. It creates five party-role actors (tank, healer, melee DPS, ranged DPS and support) plus one mutually hostile target. The roster and hostile are projected into GUID/scalar-only `PartySnapshot` values; no live Core pointers are stored in the decision model.

A simulator Trigger emits a tank-assist candidate. `PartyBotEngine::Evaluate` performs the normal action lookup, eligibility checks and channel arbitration. The selected action adapter resolves the bot and target GUIDs back through the current `Map`, revalidates life/hostility, then calls the pinned Core `Unit::Attack`. The test requires the report to say `Executed`, the action result to be `Success`, and the tank's actual Core victim pointer to equal the hostile Creature. All six actors are detached and destroyed before map deletion.

This is deliberately a narrow proof:
- It validates the real Engine evaluation/execution boundary and one real Core action adapter.
- The five roster entries are simulator Creature actors, not authenticated `Player` objects or a Core `Group`.
- Stage C proves the Engine-to-Core attack boundary only; Stage D and Stage E separately prove triggered Creature healing and normal non-triggered Player healing.
- These slices still do not prove production PlayerBot authentication, real Group membership, movement generators, class rotations or a full dungeon AI. Stage F should add movement/formation and defensive actions with Core precondition revalidation.



## Stage D — PartyBotEngine healer triage and triggered Core spell effect

This completed vertical slice adds a healer-role trigger and a `core_heal` Action adapter. The trigger reads only the value snapshot and emits a healing candidate for an injured living party member. The action resolves caster/target GUIDs on the current test map, checks that the spell is a heal effect and that the two units are alive, friendly and the target is still injured.

The simulator uses Vanilla spell 2050 (Lesser Heal, Rank 1). It requires all of the following:
- PartyBotEngine registers the action, evaluates the healer trigger and selects the injured tank.
- The adapter calls the pinned Core `SpellCaster::CastSpell` API and receives `SPELL_CAST_OK`.
- The Core Spell / SpellEffects path increases the target's actual health; the measured delta is logged and checked.
- Both Creature actors are detached and destroyed before the test map is deleted.

**Important scope limitation:** this first fixture deliberately uses a triggered cast from a template-backed Creature. This still exercises the real Core spell implementation and healing effect, but bypasses player spellbook ownership, mana cost, GCD and cast-time progression. It is not proof that an authenticated PlayerBot can cast a known spell normally. A separate non-triggered Player-backed slice must cover those mechanics before this adapter is used by production PlayerBots.

The optional CI runner now requires both Stage D and Stage E PASS markers in addition to the Stage A/B and Stage C markers. It remains push-only, uses the complete extracted-data fixture and four disposable `partybot_sim_*` schemas, and has no image-publish or deployment side effects.


## Stage E — normal Player spell execution

Stage E adds an actual Core `Player` as the Engine healer. The simulator constructs it through the pinned Player creation path (`Player::Create`, real Player stats/default spell initialization, a local `WorldSession`, map insertion and ObjectAccessor registration), rather than disguising a Creature as a Player. The fixture is not authenticated against a production account and is never persisted; it uses the disposable simulator DB and logs out through `WorldSession::LogoutPlayer(false)` before map teardown.

The Engine candidate still comes from the same healer-triage trigger. The Player action adapter re-resolves the healer and patient GUIDs, requires the healer to actually know spell 2050, checks spell readiness/GCD/current-cast state and friendly living target state, then calls the ordinary non-triggered `Player::CastSpell(..., false)` path.

The runtime assertion distinguishes acceptance from completion:
- The Engine reports the action as executed and the Core returns `SPELL_CAST_OK`.
- A generic spell remains pending immediately after acceptance and the Player GCD is active.
- The Core map ticks advance the normal cast to completion.
- The patient Creature's actual health increases and the Player's mana decreases.
- The Player logs out and is deleted by the Core session/map cleanup path; the patient actor is also destroyed.

This is the first Player-backed spell slice, not yet a production PlayerBot integration. It does not yet exercise a five-member Core Group, concurrent bot casters, cast interruption/movement cancellation, target range/LOS failure matrices, or a full combat rotation. The local session is deliberately not registered as a live account session; all map, spell, and cleanup behavior is exercised on the isolated test map.


### Stage E timing detail

The first Player-backed run confirmed that `Map::Update(t_diff)` does not advance Player spell events merely because a synthetic diff is passed. `Map::UpdatePlayers()` and session movement/spell processing are throttled against the Core's monotonic `WorldTimer`. The fixture therefore waits between bounded map updates, allowing the normal Player update cadence and spell event processor to advance in real time. This is intentional: the simulator must not fake completion by directly advancing a cast timer or applying the heal itself.


## Stage F — Engine-selected Core movement and defensive-action foundations

### Stage F1 — Core point movement

The first movement slice adds a destination payload to `ActionCandidate` and a formation trigger that derives a 2.5-yard trailing position from the Main Tank's snapshot coordinates. The adapter re-resolves mover and leader GUIDs on the current map, checks life/faction/map/instance/control state and finite destination coordinates, then invokes `MotionMaster::MovePoint(..., MOVE_NONE)`.

The fixture uses two active Core Creature actors on the isolated map. Runtime CI requires the Engine to select the leader as the target, the action report to be Executed/Success, the real movement generator to become `POINT_MOTION_TYPE`, the mover's actual distance to decrease and its final position to reach the derived destination before both actors are detached and destroyed.

This proves one Core point-movement primitive and the Engine-to-Core movement boundary. Continuous follow and native Player cast interruption are covered by the later Stage F2/F3 fixtures; formation slot arbitration and defensive movement remain out of scope.


### Stage F2 — movement interrupts an in-flight Player cast

A second Stage F scenario starts an ordinary non-triggered Player Lesser Heal through the Engine, confirms that the cast is pending with GCD, then runs a separate Engine movement decision for the same Player. The formation trigger derives a destination from the Main Tank snapshot. For this Player fixture, the adapter models the inbound mover-relocation entry point by calling `Player::SetPosition` with that destination; the Core owns movement-interrupt policy.

Runtime CI `#44145923142` verifies the native `Player::SetPosition` -> `Unit::HandleInterruptsOnMovement` -> `SpellCaster::InterruptSpellsWithInterruptFlags` path cancels the cast, preserves target health (`50000>50000`), reaches the destination and cleans up the Player/session and Creature. The adapter does not duplicate spell-interruption policy or call `InterruptSpell` directly.

Stage F2 timing note: the fixture performs one ordinary Player/map update before the cast begins. This primes `Map::UpdatePlayers`' wall-clock cadence so the first post-movement update cannot carry a stale multi-second diff and complete the heal before a subsequent tick observes movement. The test relies on real Core Player relocation and movement-interrupt handling; it never advances cast state directly or applies healing itself.

### Stage F2 investigation history (superseded by the passing native-path runtime test)

The earlier failed runs are retained here as debugging history only. Attempts that manually inspected spell flags or called `InterruptSpell` from the simulator adapter were removed because they bypassed Core's native relocation path. Source audit established that inbound mover relocation enters `Player::SetPosition` and `Unit::HandleInterruptsOnMovement`, whereas server-side point movement updates the Player spline later in `Unit::Update`.

The accepted fixture models the inbound relocation seam through `Player::SetPosition`; it does not construct or inject a network movement packet, and it does not implement spell-interruption policy. Runtime CI `#44145923142` passed with the generic spell cleared, target health unchanged, movement complete and actor/session cleanup successful.

### Stage F3 — continuous follow through the native Core FollowMovementGenerator

The follow fixture adds a `core_follow` action and a Main-Tank follow trigger. The action re-resolves the follower and leader from the current map, validates life, faction, map/instance and movement-control state, then calls `MotionMaster::MoveFollow(leader, 2.5f, PI)`.

The simulator does not calculate per-tick movement or write positions. Core's `FollowMovementGenerator` owns path generation, movement updates and target tracking. The runtime scenario requires the Engine to select and execute the follow action, the follower to reach formation range, and then the Main Tank to move 10 yards using Core `MovePoint`. The follower must track that relocation, retain `FOLLOW_MOTION_TYPE`/`UNIT_STATE_FOLLOW`, and finish within 5 yards of the leader before both actors are cleaned up.

Runtime CI `#44153619126` on commit `ccb4707` passed: `initialCaughtUp=1`, `leaderReached=1`, `followerTracked=1`, final separation `1.87`, `followGenerator=1`, `cleanup=1`.

The fixture is still a Core-backed simulator proof using Creature actors, not production PlayerBot follow behavior or a real Core Group. Retargeting/recovery edge cases, formation slot arbitration, LOS/range checks and defensive movement remain future work.

### Stage F4 — Player follow pauses for a non-triggered cast

This fixture uses a Core `Player` with a local `WorldSession` as the follower and a template-backed Creature as the Main Tank. PartyBotEngine selects the normal `core_follow` action, which installs Core's `FollowMovementGenerator<Player>`. After the Player reaches the formation distance and stops, PartyBotEngine selects the existing Player-backed Lesser Heal action against the Main Tank. The leader then moves through its Core `PointMovementGenerator` while the Player's cast remains pending.

The intended Core behavior is the guard in `FollowMovementGenerator<T>::Update`: while `IsNoMovementSpellCasted()` is true, the Core stops the follower and does not start another movement spline. The test observes the real pending Player spell and Core Player position/state; it does not implement a simulator-side movement veto, write the Player position, or directly complete the cast. After the ordinary Core SpellEvent completes the heal and consumes mana, the same FollowMovementGenerator must resume tracking the moving Main Tank.

Acceptance requires the Engine-selected follow and heal actions, a pending/GCD cast over multiple Player updates, the Core `IsNoMovementSpellCasted()` gate returning true on at least two updates, the Player remaining within 0.25 yards of its pre-cast position while that gate is active, the leader reaching its destination, a real completed heal/mana cost, follow resuming after cast completion, final formation distance within 5 yards, the Follow generator remaining active, and Player/session plus Creature cleanup. The fixture waits for two consecutive position-stable samples within formation range, then resets the leader to half health immediately before triage so passive Creature regeneration during the approach cannot invalidate the heal candidate. Runtime CI `#44159191502` on commit `5d3c9c5` passed: `initialCaughtUpStable=1`, `stationarySamples=2`, `castAccepted=1`, `pendingTicks=6`, `coreNoMovementTicks=5`, `maxCastMove=0.00`, `castCompleted=1`, `leaderReached=1`, `followPaused=1`, `followedAfterCast=1`, `finalDistance=2.00`, `followRetained=1`, `cleanup=1`.

The first runtime attempt (`#44157542291`) compiled and ran all previous stages successfully, and showed the Player follow generator tracking the leader after relocation. The new fixture did not reach the cast phase: the Player's arrival condition used the Core `IsStopped()` flag, which did not serve as a reliable settled-position observation for this Player movement; during the approach the injured Creature also regenerated enough health that healer triage emitted no candidate. The revised fixture uses two consecutive position-stable samples and reapplies the half-health test condition immediately before evaluating healer triage. This remains a test-fixture correction; no Core combat policy is being reimplemented.

This is a Player-backed Core simulator fixture, but the test Player still uses a local unauthenticated session and is not a production PlayerBot or real Core Group member.


### Stage F5 — Follow target handoff and recovery

The simulator's `core_follow` action remembers the last successfully selected target GUID. A repeated candidate for the same active FollowMovementGenerator is a no-op; a candidate for a different Main Tank remains useful and is executed through the Core `MotionMaster::MoveFollow` API, which replaces the current movement generator with a new native `FollowMovementGenerator`.

The runtime fixture creates one follower and two friendly leader Creatures. It first follows Main Tank A, then changes the snapshot assignment to Main Tank B while the original FollowMovementGenerator is still active. It requires the Engine to select B and successfully execute the retarget action. Both leaders then move through Core `PointMovementGenerator`s in opposite directions; the follower must arrive within 5 yards of B and remain at least 15 yards from A, proving the Core generator tracks the new leader rather than the stale target.

Next, the fixture removes B while it is the active follow target, advances normal Core map updates, promotes the still-present A back to Main Tank, and requires the same Engine action to recover by installing a new Core FollowMovementGenerator for A. A moves again through Core `MovePoint`; the follower must track A, retain FOLLOW state and clean up all actors.

The simulator does not move the follower, rewrite positions, or synthesize target tracking. It only updates the scalar PartySnapshot, evaluates the Engine, and invokes Core movement APIs. Runtime acceptance also requires both leader movement generators, handoff and recovery decisions, followed-target removal, final formation range, and complete actor cleanup.





### Stage F8 — Real Core five-member Group and production snapshot builder

This fixture creates five real Core Player objects through the same local-session Player construction helper used by the Player spell tests. It creates a normal Core Group with the first Player as leader, registers that Group with ObjectMgr, and adds the other four Players through Group::AddMember. The group is a real five-slot party, not a hand-authored member vector.

The fixture calls the production PartySnapshotBuilder::GetOrBuild using the first Player and a real PartyCoordinator. It requires the resulting value snapshot to use the actual Group ID, Map ID and instance ID, contain exactly five members, and resolve every actual Player GUID through the Group member slots and current Map. It verifies online/alive state, Priest class/level, health/max health, power/max power and coordinates. The test Players have no PartyBotAI attached, so the expected role is explicitly Unknown; this is not misreported as a role-classification test.

A second GetOrBuild call inside the cache window must return the same snapshot epoch/capture time. The fixture then calls Core Group::Disband, verifies every Player has been unlinked, removes the Group from ObjectMgr, and logs out all five local sessions. Group and group_member rows are created/deleted only inside the runner's disposable partybot_sim_* database clones; no production character or group data is modified.

Runtime acceptance requires five unique same-map Player GUIDs, Group registration and membership, a five-member production snapshot, accurate scalar copies, cache reuse, Group/ObjectMgr removal and complete session cleanup.






### Stage F10 — Main Tank and healer death cleanup with multi-add recovery

This scenario continues inside the same five-Player Core Group and the same two hostile Creature threat lists used for F9; no second Player/session/Group fixture is created. A third Player first establishes lower real Core damage threat on both adds while the healer remains above the 130% ranged switch threshold.

The Main Tank is then killed by normal Core Creature DealDamage using its exact current health. The acceptance check requires the Player to enter the Core death state, both mobs' ThreatManager entries for the Tank to disappear through the Player HostileRefManager death cleanup, and native Unit::SelectHostileTarget to leave both adds on the still-living healer. A fresh production PartySnapshotBuilder call from the healer must contain five members, mark the Main Tank dead, and show both hostiles targeting the healer with the surviving threat entries copied.

Next the active healer is killed through another real Core DealDamage event. Its HostileRefManager must remove the healer from both hostile ThreatManagers; after native SelectHostileTarget calls, both mobs must select the remaining DPS. A final fresh production snapshot from that surviving DPS must show the Tank and healer dead, the DPS alive, exactly two hostile snapshots, both current victims set to the DPS, only the remaining live DPS threat entries, and no stale threat entries for either dead player.

The simulator does not remove references or set victims itself. Unit::DealDamage and Unit::SetDeathState own death; HostileRefManager::deleteReferences propagates the reference removals; Unit::SelectHostileTarget and ThreatManager own victim selection; PartySnapshotBuilder reads the resulting live Core state. Acceptance includes a single Group/ObjectMgr disband, both hostile Creature cleanups, all five Player session logouts, Map GUID detachment, and the full runtime PASS marker.


### Stage F9 — five-Player multi-add healer-threat projection

This fixture creates five real Core Player objects and local WorldSessions, registers a real Group, converts it to raid mode only so the pinned Group Main Tank assignment API is available, then marks the first Player as Main Tank. Two template-backed hostile Creatures are inserted on the same Map using the ordinary Core Creature lifecycle.

Both hostiles enter combat with the Main Tank and acquire their initial threat through real Player DealDamage calls. The second Player then completes a normal non-triggered Lesser Heal on the injured Main Tank. The fixture applies one explicit scripted assist-threat event through the Core HostileRefManager::threatAssist API on the healed tank; the API itself distributes that event over the two real hostile references, and each hostile ThreatManager::getHostileTarget must select the healer after the configured event crosses the actual Core threat-switch threshold. The simulator does not write threat values into either ThreatManager, sort a fake threat list, choose a victim itself or duplicate the Core selector.

The fixture then calls the production PartySnapshotBuilder::GetOrBuild from the real Group leader. It requires a five-member roster with the actual Main Tank marker, exactly two hostile snapshots, both hostile current victims set to the healer, and threat entries for Main Tank and healer copied exactly from the corresponding Core ThreatManager values.

The assist amount is an explicit scenario event (400 total across two hostile references), not a claim that this Core/database revision automatically derives threat from Lesser Heal's amount. The first F9 attempt (`#44178467285`, `c41721f`) applied 3000 damage to an unequipped level-60 Priest, killing the Tank before the heal; the zero-health/zero-mana-cost diagnostic and empty hostile-reference list came from Core's normal death cleanup. The next attempt (`#44180425226`) also confirmed that running two independent five-Player Group fixtures sequentially on this headless Map is not a valid lifecycle test. F9 now reuses one five-Player Group: initial roster/value/cache assertions run before combat; Tank damage is 20% of live max health and must leave the Tank alive and injured; the healer faces the target before the normal cast; final threat projection and the single Group/session cleanup run at the end. The intrusive HostileRefManager list is iterated directly, and both initial tank threat values remain in the diagnostics. The real heal is still performed through the ordinary spell pipeline, while the test separately targets the shared Core assist-threat path used for positive spell/buff/heal threat when such an event is emitted. Follow-up scenarios must still cover actual tank death, healer death and re-target recovery across multiple adds.

Acceptance also includes F8's initial same-map roster, Main Tank marker, copied scalar values and cached-snapshot reuse; then all hostile Creature threat references are removed, the single Group is disbanded/removed from ObjectMgr, all five sessions log out, every Player GUID disappears from the Map, and the entire simulator run reaches its final Runtime PASS marker.


### Stage F7 — Core LOS collision and movement-control safety

The LOS fixture creates a real Player healer and injured Creature target with a clear baseline ray. It discovers loaded Vanilla GameObject templates with a collision-bearing door or solid generic model, creates each candidate through GameObject::Create, and inserts it into the map so Core registers the model in the dynamic collision tree. The fixture keeps the first model/orientation that makes Player::IsWithinLOSInMap(target) return false. No simulator LOS flag or synthetic geometry result is used.

PartyBotEngine selects its ordinary Player Lesser Heal while the PartySnapshot still contains the same living injured target. After the closed GameObject model blocks the line, the action calls ordinary Player::CastSpell; Core Spell::CheckCast must return SPELL_FAILED_LINE_OF_SIGHT. The fixture requires no health change, mana cost, pending cast or GCD, then removes the GameObject through the non-persisting map remove path, verifies LOS is restored and cleans the Player/session and Creature.

The movement-control fixture creates a normal Engine formation candidate and evaluates the real SimulatorCoreMovePointAction while the Core mover has UNIT_STATE_CAN_NOT_MOVE, then UNIT_STATE_POSSESSED. Both candidates must be rejected as impossible before execution; no PointMovementGenerator may start and the mover's position must remain unchanged. The adapter's execution-time check applies the same state mask so stale selection cannot bypass the movement safety gate.

The simulator changes no combat or LOS policy. Core dynamic collision and Core Unit state are authoritative; the fixture only creates/removes a real GameObject and injects the corresponding Core unit-state bits to test the executor boundary. A real root/stun aura-to-state transition and additional crowd-control types remain future coverage.

Runtime CI `#44170264755` on `e46a246` passed: `registered=1`, `selected=1`, baseline LOS clear, `blockerTemplates=370`, `testedModels=13`, collision model `blockerEntry=13965`, `blockedLOS=1`, `executionFailed=1`, `castResult=42` (`SPELL_FAILED_LINE_OF_SIGHT`), health `50000>50000`, mana `2896>2896`, no pending/GCD, `losRestored=1`, `cleanup=1`. The movement fixture also passed with `rootedRejected=1`, `possessedRejected=1`, `noMovementStarted=1`, `positionUnchanged=1`, `cleanup=1`.


### Stage F6 — Player heal execution-time range and resource rejection

This fixture deliberately exercises the difference between a value snapshot and authoritative Core state. It first constructs an injured target 60 yards from a real Core Player healer, while the PartySnapshot still contains a stale nearby position for that target. PartyBotEngine is expected to select the heal candidate from the value snapshot; the Engine action then calls ordinary non-triggered Player::CastSpell, and the pinned Core Spell::CheckCast must return SPELL_FAILED_OUT_OF_RANGE. The action result must be ExecutionFailed/Failed, with no health change, mana consumption, pending cast, or GCD.

The second case evaluates a heal candidate while the Player has normal mana, then sets the Player's real mana to zero immediately before ExecuteSelected. The same action must reach Core Spell::CheckPower, which returns SPELL_FAILED_NO_POWER; again, the result must not be reported as success and there must be no health change, cast, GCD or resource consumption.

This is intentionally an execution-boundary proof, not a simulator implementation of spell range or mana-cost formulae. The fixture does not calculate range, LOS, or mana cost itself; it requires the pinned Core Spell pipeline to reject the action and reports that authoritative result separately from the Engine decision.

Runtime acceptance: both candidates are selected from their scalar snapshots, both execution attempts fail with the exact expected Core SpellCastResult, health remains unchanged, no cast/GCD starts, the first case consumes no mana, the second case remains at zero mana, and Player/session plus both Creature fixtures are cleaned up. LOS remains a separate open case until a deterministic blocked VMap line can be identified in the headless test map.

Runtime CI `#44167535205` on `e52bea6` passed. The stale-nearby snapshot/60-yard Core target case returned `SPELL_FAILED_OUT_OF_RANGE` (`89`) with `rangeSelected=1`, `rangeExecutionFailed=1`, health `50000>50000`, mana `2896>2896`, `rangeNoPending=1`, `rangeNoGcd=1`. The late resource depletion case returned `SPELL_FAILED_NO_POWER` (`77`) with `noPowerSelected=1`, `noPowerExecutionFailed=1`, health `50000>50000`, mana `0`, no pending cast/GCD, and `cleanup=1`.


Runtime CI `#44163805016` on commit `cfdda0c` passed: `aiReady=1`, `registered=1`, `initialSelected=1`, `initialFollow=1`, `initialCaughtUp=1`, `handoffSelected=1`, `handoffExecuted=1`, `handoffTargetChanged=1`, both leader movement generators started and reached destination, `followerTrackedNewLeader=1`, `oldLeaderRemoved=1`, `recoverySelected=1`, `recoveryExecuted=1`, `recoveryTargetRestored=1`, `recoveredToAvailableLeader=1`, final separation `4.55`, `followGenerator=1`, `cleanup=1`.



### Stage F11 — Core melee reachability on each threat reference

The production snapshot builder copies `HostileReference::getTarget()` reachability by calling the hostile Creature's native `Unit::CanReachWithMeleeAutoAttack`; it does not derive reachability from scalar distance or party-member-to-member geometry. Each `ThreatEntrySnapshot` keeps only the target GUID, Core threat value/status flags, and the Core-computed melee-reachability bit.

The F11 runtime fixture exercises the same four Main Tank/Healer references across two hostile Creatures. The first hostile is placed close enough for both Players to be melee-reachable; the second stays well outside melee reach. The simulator re-resolves each GUID from the hostile's Map and compares the copied snapshot bit with a fresh direct Core API call. Acceptance now requires exactly four parity matches with the expected coverage split: two reachable and two unreachable. The CI harness separately greps the explicit coverage counters, so a fixture that accidentally places all samples in one state cannot pass.

Runtime CI `#44213077421` on commit `6066306` passed on the Linux ARM64 / Vanilla 5875 runner. The F9 diagnostics reported `meleeReachability=4/4 reachable=2 unreachable=2`; every copied bit matched a fresh `CanReachWithMeleeAutoAttack` call for the same resolved Core Unit. The runner required the 2/2 coverage split, completed the full Core-backed simulator scenario, observed the final Runtime PASS and shut down cleanly. F11 is complete.


### Stage F12 — discover every hostile reference held by a party member

The production builder previously discovered hostiles through member current-victim and attacker relationships. That misses a hostile which still has a threat reference to a party member but has switched its current victim to an outsider or another non-party unit. This matters for healing/assist threat: Core `HostileRefManager::threatAssist` walks every reference held by the healed unit, not only mobs currently attacking it.

Stage F12 extends the builder to enumerate each Group member's `GetHostileRefManager().getFirst()` chain. Each reference's source ThreatManager resolves to the hostile owner; the builder adds that hostile to the snapshot even when no Group member currently has it as an attacker or target. It also merges a missing per-member threat entry from the linked reference itself. This preserves offline references which have been moved out of the hostile's active online `getThreatList()`, together with Core's `isOnline` and `isAccessable` flags.

The runtime fixture uses one real five-Player Group, two hostile Creatures and one same-faction Creature outside the Group. After the second hostile switches its current victim to the outsider, no Group member is its current attacker. The fixture then marks the Tank's reference offline through the Core HostileReference API, proves `getThreat(tank, false)` no longer sees it while `getThreat(tank, true)` still does, and requires PartySnapshotBuilder to discover the hostile from the Tank's HostileRefManager and copy the offline entry. It restores the reference and hostile target before F10's Tank/Healer death-recovery assertions. This direct status transition is a deterministic fixture seam to test the Core offline-container path; the builder itself does not mutate reference state.

Runtime CI `#44215877767` on commit `1a3aa02` passed on the Linux ARM64 / Vanilla 5875 runner. The fixture reported `outsideVictim=1 noPartyAttacker=1 tankRef=1 offlineSnapshot=1 nonVictimHostile=1 restored=1`; the runner required the explicit F12 PASS marker, the prior F10 death-recovery assertions, and the full simulator final Runtime PASS. The offline reference was toggled only through the Core HostileReference API to exercise the offline-container seam; PartySnapshotBuilder itself only read and copied Core state. F12 is complete.


### Stage F13 — shared PartyBotAI encounter trace budget overflow (passed)

The shared encounter budget is owned by the map/instance PartyCoordinator rather than by each bot. Stage F13 uses two distinct PartyBotAI objects attached to two real Players in the five-member Core Group fixture. Both AI objects share one PartyCoordinator and each alternates its actual Core victim between the same two live hostile Creature fixtures using `PartyBotAI::AttackStart`.

The fixture requests 512 successful victim switches from the first bot and 513 from the second. The coordinator must admit 1,024 detail events across both instances, then allow the first overflow to emit the one shared `trace_limit` marker. The simulator requires the combined successful switch count to equal 1,025 and verifies the coordinator suppresses any subsequent detail admission. The runner separately greps for exactly one formatted `event=trace_limit reason=detail_budget_exhausted` line and the explicit Stage F13 PASS marker.

This is a Core integration test of PartyBotAI's real target-change observer and shared encounter budget. It does not simulate combat damage or replace Core target selection. The scenario passed again as part of the full F13-F17 fixture-backed run [#44581612462](https://github.com/tyrael9/core/actions/runs/44581612462).




## Stage F17 — bounded trigger telemetry and originating-trigger correlation (passed)

The fixture registers the real candidate-only Shadow strategy set and evaluates it with a value-only party snapshot containing a healer and one injured party member. The expected report contains three Trigger observations: `healing_triage` active with one candidate, `tank_target` inactive and `dps_target` inactive. The candidate must retain both its strategy name and the originating `trigger_name=healing_triage`.

The fixture passes those observations through the production `DecisionTrace` formatters and the same coordinator detail admission used by encounter traces. `tools/partybot-simulator/run-ci.sh` requires the exact `trigger_summary` counts, all three formatted Trigger rows, the correlated candidate record and the dedicated F17 PASS marker. Trusted ARM64 / Vanilla 5875 push CI [#44581612462](https://github.com/tyrael9/core/actions/runs/44581612462), commit `0ce8cb28aa2e1af47982d8775728ba3b8d772f84`, passed the complete simulator and final Runtime PASS.

This proves bounded decision-observation formatting and Trigger-to-candidate correlation. It does not prove that these Shadow candidates execute or that Legacy policy has been replaced.

## P7.3b applied periodic threat-component fixtures (2026-11-18)

The existing simulator now carries separate bounded applied-event fixtures for periodic DoT, HoT, helpful power restoration and Mana Leech. These are **one completed Core tick** checks; none is a forecast of future ticks or a complete pre-action estimate.

- **Periodic damage:** real Shadow Word: Pain (10894) Aura tick through Core Aura scheduling and `Unit::DealDamage(DOT)`; 142 applied damage matched estimated/actual threat 142.000 (full runtime CI #529).
- **Periodic healing:** real Rejuvenation (9841) Aura tick; 189 effective health gain distributed 47.250 threat to each of two live hostile references, matching both ThreatManager deltas (full runtime CI #529).
- **Periodic power:** the pinned spell store has 13 ordinary Mana PERIODIC_ENERGIZE entries and 2 OBS_MOD_MANA entries, but no native ENERGY PERIODIC_ENERGIZE entry. To exercise all three Core switch branches without violating `Spell::Spell`'s pointer-identity assertion, the fixture keeps the original SpellMgr-owned Rejuvenation entry pointer and temporarily overrides only `EffectApplyAuraName[0]` and `EffectMiscValue[0]` inside an RAII guard. It restores both original DBC fields after the Aura is removed. The tested cases are Energy PERIODIC_ENERGIZE (assist threat), Mana PERIODIC_ENERGIZE (1.12 NoThreat exemption) and OBS_MOD_MANA (Mana assist threat). This is a bounded, isolated Core fixture technique, not a claim that the canonical DBC includes an Energy spell.
- **Periodic Mana Leech:** a native single-effect 5875 SPELL_AURA_PERIODIC_MANA_LEECH entry is selected from SpellMgr. A real triggered cast and Core Aura tick must drain target Mana, credit the caster's effective Mana gain, and match the direct 50%-assist threat component against the leeched hostile's own ThreatManager. The test excludes Mark of Kazzak's depletion explosion and Improved Drain Mana bonus-damage cases, because those are additional impact rows not covered by this component.

Runtime verification for the latest Power/Mana-Leech fixtures is still pending. ARM64 compile and pure Engine tests pass, but trusted push runs have not reached mangosd startup because Google Drive repeatedly returns its per-file download-quota error. `tools/partybot-simulator/run-ci.sh` now accepts a direct HTTPS archive URL via `curl` as well as Drive via `gdown`; an OCI Object Storage read-only PAR is the preferred repeatable source. The archive SHA-256 is checked before extraction. No runtime PASS claim is made until the dedicated markers and the final simulator Runtime PASS are present.



### Periodic-life-leech composite runtime slice (fixture-backed runtime passed)

A further fixture covers Core's shared `SPELL_AURA_PERIODIC_LEECH` damage/self-heal path. It creates one friendly caster and two real hostile references on that caster, applies a native ordinary periodic-leech Aura to hostile A, and waits for one real Core tick. The fixture compares hostile A's ThreatManager delta against the sum of its applied periodic-damage component plus half of the caster's actual self-heal assist, and compares hostile B against the other half of the heal-assist fan-out. It reuses the damage/heal component rows and deliberately leaves the full action estimate Unknown. The CI runner requires a dedicated PASS marker. Trusted push CI [#44556650342](https://github.com/tyrael9/core/actions/runs/44556650342) passed this fixture together with the complete ARM64/Vanilla 5875 runtime and final simulator Runtime PASS. The later full regression run [#44716873212](https://github.com/tyrael9/core/actions/runs/44716873212) also passed the simulator-wide suite.


## Stage F14 — sampled non-PlayerBots member health and lifecycle trace

F14 validates sampled state-delta tracing for an ordinary Player without `CombatBotBaseAI` in the real five-Player Group. The fixture captures a baseline through `PartySnapshotBuilder`, applies actual Core `Unit::DealDamage` for nonlethal and lethal health changes, then calls `Player::ResurrectPlayer`. A fresh map-thread snapshot after each Core operation is diffed by a PartyCoordinator and emitted through the same DecisionTrace formatter/admission path used by PartyBotAI.

The runtime runner requires exactly one `party_member_health`, `party_member_death`, and `party_member_resurrect` record for the same member GUID. Initial baselines and unchanged snapshots emit nothing. PartyBot members are excluded because their PlayerAI callbacks are authoritative. These are sampled health/alive deltas, not a per-hit damage stream.

## Stage F19 — Engine action ID through Core Spell::prepare/finish (passed)

The Player-backed healer fixture uses a real `PartyBotAI` as both the Engine action observer and Core PlayerAI. A PartyBotEngine-selected direct Player heal adapter runs through the real `Player::CastSpell` path. The fixture requires one nonzero 64-bit action ID and the same encounter ID on `engine_action_attempt`, `engine_action_result` and the later `cast_terminal`; the terminal sequence must follow the adapter result. It also checks known-spell ownership, accepted pending cast/GCD, Mana consumption, actual Lesser Heal health gain, cleared active/pending correlation state and Player/session cleanup.

Trusted push CI [#44594126823](https://github.com/tyrael9/core/actions/runs/44594126823), commit `3ad73a5d58effdb019540e8f10861122171165f4`, passed full Linux ARM64 / Vanilla 5875 build and fixture runtime. The observed trace used encounter_id=1/action_id=1 with attempt/result/terminal sequence numbers 2/3/4. This explicitly distinguishes synchronous adapter acceptance from asynchronous Core Spell completion.

## Stage F20 — full-report bounded decision summary (passed)

The `DecisionSummary` projection counts every candidate outcome in the full report: arbitration-selected, executed, execution-failed, lower-priority same-channel, missing-action, impossible, useless, multiplier-veto, invalid-priority, threat-unknown and threat-budget-veto. The Core Player heal fixture emits a summary after `ExecuteSelected`, proving that the selected count survives the status transition to Executed and that the fixed-width key/value record is produced under the coordinator's shared detail budget. Pure tests populate every bucket, including a non-arbitration NotSelected row that must not inflate the channel-loser count.

Trusted push CI [#44612693003](https://github.com/tyrael9/core/actions/runs/44612693003), head `97c5e6017a15df4ab0e2695e4b653ab590e43602`, passed the pure Engine suite, report collector, full ARM64/Vanilla 5875 build, fixture-backed runtime and the final simulator `Runtime PASS`. Runtime evidence included one complete summary (`candidates=1 selected=1 executed=1 execution_failed=0`, all rejection buckets zero), the real Core Lesser Heal effect, Mana `2896>2866`, active LastManaUse and one action_id correlating Engine attempt/result with the later successful Core cast terminal. The fixture's PartyBotAI is intentionally pre-initialized and its Legacy update deferred because this scenario tests the real PlayerAI observer boundary, not legacy bot startup.

## Stage F21 — real member combat state and hostile template identity (passed)

`PartyMemberSnapshot` now copies shapeshift form, current generic/channelled/autorepeat spell ID, Core spell state, current spell type, Unit target GUID, orientation and position. `HostileSnapshot` copies Creature template entry, rank and world-boss flag. These remain value-only fields; no live Unit, Creature or Spell pointer is retained in the snapshot.

The real five-Player Group fixture verifies the initial Cat Form value, then captures a real pending Core Lesser Heal and compares spell ID/state/type/target GUID with the live Spell. It also compares hostile entry/rank/world-boss identity with the two live Core Creature templates and checks position/orientation. The test copies the pending-cast snapshot through a separate PartyCoordinator so it cannot replace the snapshot epoch used by F9's heal-assist estimate. The formatter emits all new facts in the existing 1,600-character bounded `party_snapshot` payload.

Trusted push CI [#44620034025](https://github.com/tyrael9/core/actions/runs/44620034025), commit `2a901df92ef060823917e11da88b145921c0cd76`, passed the full Linux ARM64 / Vanilla 5875 runtime. The log contained `form=1`, pending `cast=2050:1:1:<target-guid>`, member positions, and hostile `entry=29,rank=0,boss=0`; F8/F9/F12 threat and death recovery, F19/F20 and final `Runtime PASS` also passed.

## Stage F22 — per-hit Player damage health deltas (passed)

Core `Unit::DealDamage` now notifies the victim's `PlayerAI` after a real Player health decrease. The callback carries attacker GUID, actual health delta, health before/after, Spell ID (zero for non-spell damage) and damage type. `PartyBotAI` invalidates its decision cache and emits a bounded `damage_taken` detail record only when the bot is attached to an active matching encounter coordinator. Non-PlayerAI group members continue to use the F14 heartbeat lifecycle sampling.

The fixture's real hostile Creature deals lethal direct damage to a PartyBotAI-controlled tank. The expected event reports attacker GUID `17379390962509588093`, damage `1717`, health `1717 → 0`, spell `0`, damage type `0`; the existing death/resurrection callbacks still fire exactly once. Because `Kill` emits the death callback before `Unit::DealDamage` emits the final damage delta, the death event may have the earlier sequence number; the damage event still records the actual final health state.

F22 consumes the shared encounter detail budget. The F13 overflow fixture now uses a fresh isolated coordinator so the damage event cannot shift the exact 1,024-detail admission/overflow boundary. Trusted push CI [#44622617708](https://github.com/tyrael9/core/actions/runs/44622617708), commit `a50c718ad0d12360cb9221588788ed9e73aa6dc5`, passed the complete Linux ARM64 / Vanilla 5875 runtime, including F19-F21, F8/F9/F12 and final Runtime PASS.

## Stage F23 — prioritized bounded Trigger trace sample (passed)

The real Shadow Trigger observation fixture now includes one active healer Trigger producing a candidate, one active Trigger with no candidates, two inactive role Triggers, and three interval-limited Triggers evaluated twice to produce cadence-skipped observations. The production selector must retain the most actionable four: healing_triage, active-empty, tank_target and dps_target. The summary must report `total=7 evaluated=4 active=2 inactive=2 cadence_skipped=3 omitted=0 detail_limit=4 detail_omitted=3`, explicitly separating Engine observation-cap truncation from the four-row logging cap.

Trusted push CI [#44628997647](https://github.com/tyrael9/core/actions/runs/44628997647), head `c0197bfe4b211ced03837e855e19fa80368149eb`, passed the pure suite, full Linux ARM64 / Vanilla 5875 build and fixture-backed runtime. The log contained the exact summary and row order, the dedicated F23 PASS marker, and the complete simulator Runtime PASS. F23 is complete.

## Stage F24 — selected-first bounded candidate detail (passed)

The Player-backed Engine heal fixture now registers one valid `core_player_heal` action plus four candidates whose action names are intentionally unregistered. The full report therefore contains five rows while only the real heal is selected. After `ExecuteSelected` changes the winner's status to `Executed`, the formatter must still retain that candidate first by consulting the report's authoritative `selectedIndices`, not by looking for a mutable `Selected` status. The remaining two detail slots use descending finite final priority with stable original-order ties; the two unlogged alternatives are counted explicitly.

Acceptance requires `candidate_detail_limit=3`, `candidate_detail_omitted=2`, exactly three formatted candidate records, the executed `core_player_heal` row as the first candidate record, and the complete decision summary `candidates=5 selected=1 executed=1 execution_failed=0 missing_action=4`. This is a bounded diagnostic sample, not a replacement for the complete in-memory DecisionReport or a claim that omitted candidates were not evaluated.

Trusted push CI [#44630978690](https://github.com/tyrael9/core/actions/runs/44630978690), commit `ee47cf65fe8153f6eb079c880a275cc90321642a`, passed the pure suite, read-only collector, full Linux ARM64 / Vanilla 5875 build, 1.21 GB extracted-data fixture download and complete simulator runtime. The log contains the exact summary, three candidate rows, the dedicated F24 PASS marker and final Runtime PASS.


## Stage F29 — real PartyBotAI NewEngine authority isolation (passed)

The five-Player Core fixture now temporarily installs a separate `PartyBotAI` instance on the real tank Player, sets a per-instance simulator authority override to NewEngine, and calls the production `PartyBotAI::UpdateAI` entry point. The global simulator configuration remains Shadow-enabled so the surrounding F17/F27 scenarios retain their existing behavior. The fixture requires one NewEngine tick and one `ExecuteDecisionCycle`, zero candidates, no Shadow probe strategies, zero entries into the Legacy policy branch, zero Legacy combat/out-of-combat dispatches, and a live coordinator matching the fixture map/instance. The previous PlayerAI is restored before the fixture performs its normal teardown.

Trusted Push CI [#44673953340](https://github.com/tyrael9/core/actions/runs/44673953340), head `18b17cc7bd6a2aecebe51409b832e0f31104546e`, passed full Linux ARM64 server/realm compilation, pure Engine tests, report collector tests, simulator harness syntax and the real Vanilla 5875 fixture. The runtime log reported `newEngineTicks=1 legacyEntries=0 legacyDispatches=0 decisionCycles=1 candidates=0 shadowPoliciesRegistered=0 coordinator=1`, emitted the dedicated F29 PASS marker and reached final `Runtime PASS`. PR CI [#44673960390](https://github.com/tyrael9/core/actions/runs/44673960390) passed the build/test path. This proves authority isolation and deliberate no-op behavior; it does not claim production class/role adapters or gameplay readiness. Simulator artifact: [partybot-simulator-18b17cc](https://github.com/tyrael9/core/actions/runs/44673953340/artifacts/14601312322).


## Stage F33 — PendingActionQueue starvation resistance (passed)

The pure Engine suite verifies priority ordering just before the fairness boundary (2,999 ms), age promotion at exactly 3,000 ms, oldest-first ordering among promoted actions despite different priorities, and behavior across uint32 millisecond wrap. The Engine-level fixture queues a low-priority same-channel action first and a much higher-priority action at age 2,999 ms; at 3,000 ms the older action is selected and executed, while the newer action is requeued without extending its original TTL and executes on the following tick. The post-revalidation sort uses the same comparator and current revalidated priority, so dynamic multiplier changes remain effective for entries that have not been age-promoted.

Push CI [#44680955006](https://github.com/tyrael9/core/actions/runs/44680955006), head 5ae65fd4167779a9d98f0c7ccdc34c0aab024e69, passed pure tests, ARM64 server/realm compilation, simulator harness syntax and the complete trusted Vanilla 5875 runtime. The run also passed the F32 terminal-listener shell gate and final Runtime PASS. PR CI [#44680961189](https://github.com/tyrael9/core/actions/runs/44680961189) passed the pure/build path.

F33 affects only the Engine-owned pending queue. It does not change Legacy or Shadow scheduling/execution behavior.


## Stage F34 — Core-terminal-gated action continuation (passed)

The real Engine-selected Player Lesser Heal action registers a core_spell_terminal_followup before invoking Core CastSpell. The continuation stores the root action ID, encounter, target and spell identity plus a bounded TTL and owner group/map/instance/bot scope. The root Spell reaches Spell::finish; only its matching successful terminal event moves the follow-up to the Engine PendingActionQueue. The fixture then calls the next pending dispatch and requires one selected/executed child, with no rejected or stale entries. A failed/interrupted or mismatched event cannot unlock the child, and the queue still revalidates all normal action and threat conditions at dispatch time.

The simulator healer helper enables this branch only for the F34 fixture; older Player follow/cast, range/LOS and movement-interrupt fixtures continue to use the original path. The fixture now uses WorldTimer's monotonic clock for snapshot capture, encounter start and Engine context so the Core Spell terminal timestamp and continuation TTL share a clock domain.

Trusted Push CI [#44689339405](https://github.com/tyrael9/core/actions/runs/44689339405), head e3be7ef90743b06953ce5caba23ea59de6280c30, passed pure Engine tests, full Linux ARM64 server/realm build and the complete Vanilla 5875 simulator. The log reported continuationResult=0 coreTerminal=1 continuationWaiters=0 pendingBeforeDispatch=1 dispatchSelected=1 dispatchExecuted=1 dispatchRejected=0 dispatchStale=0 continuationExecuted=1 continuationEnqueued=1 continuationDispatched=1, emitted the dedicated F34 marker, passed the F32 terminal-listener marker and reached final Runtime PASS. PR CI [#44689346106](https://github.com/tyrael9/core/actions/runs/44689346106) passed the pure/build validation.

F34 verifies generic terminal-gated continuation mechanics only; it does not claim class/role rotations or NewEngine gameplay readiness.


## F44 — real five-Player DPS assist handoff (trusted-runtime validated)

A dedicated Core fixture creates a five-Player Group, assigns a Main Tank via the Group API, and installs real PartyBotAI role providers for the DPS actor and healer. The policy consumes copies of PartySnapshotBuilder output rather than hand-constructing the group roster or the primary assist targets. It checks Main Tank victim, healer-peel victim, role IDs, and all five live Group members. The crowd-control veto remains a clearly scoped test-only value overlay because PartySnapshotBuilder does not currently derive that flag from live aura state.

The fixture runs the two-stage select-then-attack sequence, requires each successful action to invalidate the shared PartyCoordinator cache, then changes the Main Tank's target while the DPS bot is still attacking the old target. It proves the cached old view remains observable until explicitly invalidated, the rebuilt snapshot carries the new Main Tank victim and old DPS victim, and the DPS bot selects then attacks the new Main Tank target. The trusted Push workflow is gated on the exact PASS marker plus complete Group/session/actor cleanup. External non-Engine Player target changes still depend on an explicit invalidation call or the production 250ms snapshot freshness bound; this fixture does not claim a Core-wide player target-change callback.

F44 trusted-run closure evidence: the exact gate marker was present in trusted Push #45074121261, followed by the final simulator Runtime PASS. The diagnostic recorded every group, role, selection, cache invalidation, Main Tank retarget, new-target attack and cleanup condition as successful. Matching PR #45074126327 passed the full Linux ARM64 build and harness checks (runtime secrets are intentionally not exposed to the PR job). Artifact: [partybot-simulator-f50c79e](https://github.com/tyrael9/core/actions/runs/45074121261/artifacts/14774942008).


## F45 — count-based item-use mutation fixtures (trusted-runtime validated)

F45 extends F42's charge-focused Core item-use baseline without changing its closure status. A real stackable item 118 / spell 439 cast from a two-item stack must heal, leave the same stack at count one, and emit exactly one CountChanged / BeforeEffects record with Item GUID and 2→1. This is the Core-observed delayed phase: the Player potion heal is subject to spell-effect batching, and TakeCastItem classifies the count mutation before the batched effect resolves. A separate real Player casts from a three-item stack while the item itself is also a two-count spell reagent; Core should consume all three in TakeReagents, clear the CastItem pointer, and emit a single GUID-less ReagentConsumed / BeforeEffects record 3→0, with no duplicate TakeCastItem event. The test restores the shared SpellEntry reagent arrays and verifies full Player/session cleanup. Pure tracker tests cover both event forms and GUID validation. Trusted CI evidence is pending.


Trusted Push [#45076438039](https://github.com/tyrael9/core/actions/runs/45076438039) emitted the exact F45 PASS marker, reported all stack/reagent/cleanup diagnostic flags successful, and reached final Runtime PASS. Matching PR ARM64 build [#45076443461](https://github.com/tyrael9/core/actions/runs/45076443461) passed. Artifact [partybot-simulator-945ad2f](https://github.com/tyrael9/core/actions/runs/45076438039/artifacts/14775915604). F45 is closed; no production item-use action or NewEngine item strategy was registered.

## F48 — real Core item-backed self-utility execution

Trusted push [#45097019380](https://github.com/tyrael9/core/actions/runs/45097019380) verified the real Vanilla Elixir of Minor Agility (item 2457 / spell 2374) via `Player::CastItemUseSpell`: Core `CanUseItem` and initial readiness passed; the caster-only Lesser Agility aura increased actual Player agility 40→44; one ItemUse child succeeded; a separate CountChanged event recorded the same Item GUID and stack 2→1. Clearing GCD did not clear the item cooldown: the immediate retry ended in one failed child with no mutation or additional effect. All roots and Player/session/AI resources were cleaned up and the complete simulator reached final `Runtime PASS`. Matching PR build [#45097024271](https://github.com/tyrael9/core/actions/runs/45097024271) passed.

## F49 — simulator-only Engine-selected item-use action (runtime validation pending)

The Engine candidate carries only value data (Item GUID, entry, encoded inventory position, ON_USE slot). The simulator action resolves the current inventory object at evaluation and repeats exact identity, owner, usability, complete fan-out, combat/cast and cooldown validation in the final CoreAction preflight. Only the strict F47 item-backed self-aura path receives a NoThreat estimate. The fixture tests selection-time-to-dispatch item removal, then a fresh Engine-selected Core item use with action-ID correlation into ItemUseTerminal, actual aura/stat and CountChanged effects, and cooldown rejection before a repeat can be dispatched. It remains an isolated simulator adapter and does not authorize production registration.


## F54d — hostile Creature damage between two real Lesser Heal arrivals (trusted-runtime validated 2026-11-24)

The F54d hostile interleaving fixture uses real Core Players, a real hostile Creature and two hostile references on the healed Player. It captures two ordinary non-triggered Lesser Heal casts and applies a real hostile `Creature::DealDamage` event between the two successful Spell arrivals. The disposable database configures the Lesser Heal flat `spell_threat` row to zero for this case, isolating the ordinary healing-assist path.

Trusted push [#45561037244](https://github.com/tyrael9/core/actions/runs/45561037244) emitted: `captured=1 firstCast=1 firstHeal=58 hostileDamage=30 secondCast=1 secondHeal=51 healRefs=2 flatThreatZero=1 healthMatches=1 threatMatches=1 actualThreat=27.25/27.25 expectedThreat=27.25/27.25 observedHealth=937 projectedHealth=937 projectedEffective=109 projectedDamage=30 projectedOverheal=0`. The per-hostile value matches `(58 + 51) * 0.5 / 2`; the actual Core health after the complete ordered sequence matched the pure projection exactly. The fixture restored temporary database state and the aggregate simulator reached final `Runtime PASS`. Artifact: [partybot-simulator-3a43fca0](https://github.com/tyrael9/core/actions/runs/45561037244/artifacts/14998456148).

This validates only the tested ordered arrival sequence and assist-threat recipients. It does not prove all possible caster/recipient proc auras, SpellScript/AuraScript callbacks, `SPELL_AURA_ADD_TARGET_TRIGGER` child casts, nested children or complete action-level threat closure. Those remain F54e work; the production Priest healing action and strategy remain disabled.


### Historical F54f development notes (superseded by the trusted PASS above)

The earlier entries below record the fixture's intermediate lifecycle, delayed-proc and shell-gate failures. They are historical debugging notes only. The final corrected runner was validated by trusted push #45716916204 and reached the simulator's final `Runtime PASS`; do not interpret the earlier “pending” wording in those notes as current status.

## Runner gate note — F22 damage_taken marker (2026-11-25)

The trusted runtime log showed the actual lethal Player event (`damage_taken`, nonzero attacker and damage, health transition 1717→0, spell 0, damage type 0), and GDB reported normal process exit. The shell gate had incorrectly required a fixture-emitted PASS sentence before checking that same structured event; the fixture emits the event but not that sentence. The runner now checks for exactly one matching structured event first, then prints the PASS sentence itself. The count and field constraints remain strict; only the redundant impossible marker prerequisite was removed. A fresh trusted run is required to validate the revised gate.


## Player alive-state observer marker correction (2026-11-25)

The latest log audit found three shell-gated PASS sentences absent from the fixture output even though the corresponding assertions already existed and contributed to the final F10 acceptance conjunction: death/CORPSE callback filtering, one resurrection observer transition, and PartyBotAI encounter-correlated death/resurrection records. Commit `6519359` adds each PASS sentence conditionally under its existing assertion. This is a fixture observability correction only; it does not bypass or weaken any assertion. Trusted runtime validation is pending.


## F54f runner metric-type correction — resolved

Trusted run [#45716916204](https://github.com/tyrael9/core/actions/runs/45716916204) executed the corrected `run-ci.sh` on commit `35224fc`, validated numeric child-heal accounting for caster and recipient sides, passed all simulator gates, emitted final `Runtime PASS`, completed Group/session cleanup and exited normally under GDB. The current branch head also has a passing PR build [#45716989280](https://github.com/tyrael9/core/actions/runs/45716989280).
