# PartyBot Core build, runtime and trace testing

## Automated compile CI

The focused workflow at .github/workflows/partybot-core-build.yml runs the standalone PartyBot Engine tests, Python unit tests for the read-only encounter report collector, and builds the pinned Vanilla 1.12.1 / build 5875 C++14 Core on Linux ARM64, matching the Oracle A1 target. Trusted branch pushes additionally run the fixture-backed simulator when the extracted-data URL and checksum secrets are available; PR jobs deliberately skip that fixture runtime because secrets are not exposed.

A successful run proves configure/compile/link in that CI environment. It does not prove runtime behavior, live snapshot correctness, instance persistence, database compatibility beyond compilation or successful operation on the Oracle realm. It is not permission to deploy.

## Publishing and deployment

The ARM64 image workflow is maintained at .github/workflows/publish-partybot-image.yml. Always use the exact tag and Core SHA from the successful workflow run; do not infer a tag from an earlier run or from this document.

Publishing an image does not deploy it. Deployment is a separate user-authorized operation through the helper in tyrael9/vmangos-deploy/scripts/partybot-deploy.sh. Do not replace database images or touch database volumes. Players should log out before a server-image replacement because it disconnects active clients.

No CI task, documentation edit or image publish grants permission to restart or modify the Oracle server.

## Lifecycle regression — Legacy host

Owner-disconnect persistence was runtime-tested on the Oracle test realm on 2026-11-15:
- owner reconnect within the grace period preserved party membership, bot positions and instance state;
- intentional owner leave removed the bots;
- expiry beyond the grace period cleaned up the bots.

This validates the existing lifecycle-host/Legacy path only. Keep it as a regression test when modifying PartyBotAI host integration.

## Runtime authority and safe smoke-test boundary

The runtime exposes three explicit values through `PlayerBot.EngineAuthority`:

- `0` Legacy (default): the existing PartyBot runtime remains authoritative.
- `1` Shadow: evaluate and log Engine observations; Legacy remains the sole executor.
- `2` NewEngine: exclusive Engine authority; `PartyBotAI::UpdateAI` returns before Legacy follow/heal/combat/class policy.

`PlayerBot.EngineShadow` remains a backward-compatible read-only opt-in while authority is Legacy. NewEngine excludes the observational `shadow_*` probes from executable policy. Its decision cycle and pending dispatcher are wired, but no production class/role Core adapters are registered. An empty NewEngine registry deliberately does nothing; it never falls back to Legacy. Keep the default at Legacy.

The trusted Core fixture for F29 invokes the real `PartyBotAI::UpdateAI` under a per-instance NewEngine override and requires one Engine decision cycle, zero Shadow probe strategies, zero Legacy policy entries/dispatches and a valid coordinator. This proves authority isolation and deliberate no-op behavior, not gameplay readiness.

## F41 CoreAction resolver and context-copy regression

The standalone C++14 suite uses a fake `CoreActionRuntime` and a concrete test `CoreAction`. It proves the resolver remains present in Engine-created contexts for candidate evaluation, immediate execution, pending enqueue/revalidation and pending execution. The test action succeeds only after the final CoreAction wrapper receives a resolved actor/target/map; the pending action executes only after fresh eligibility checks and final Core resolution.

The real Core simulator preflight requires `resolved=1 selfTarget=1 missingTargetRejected=1 staleMapRejected=1 staleInstanceRejected=1`. Trusted push CI [#44821298517](https://github.com/tyrael9/core/actions/runs/44821298517) passed the fixture-backed simulator through final `Runtime PASS`; artifact [partybot-simulator-1fe34c9](https://github.com/tyrael9/core/actions/runs/44821298517/artifacts/14672696314). The added pure resolver-propagation regression passed PR CI [#44822853602](https://github.com/tyrael9/core/actions/runs/44822853602); the matching trusted push run is validating the same test plus full runtime.

This validates the generic resolver seam and context transport only. It is not a registered production combat action and does not replace per-adapter Core acceptance/rejection tests or live-realm/in-game testing.

## Current encounter trace evidence and open gates

The structured encounter stream now includes encounter boundaries/IDs/sequences, party snapshots and Core threat references, sampled Trigger and candidate details, target changes, Engine threat-reservation outcomes, action-attempt/ActionResult/Core Spell terminal correlation, Player damage records, bot alive-state records, ordinary Group-member sampled health/death/resurrection, a shared detail/important-event budget, terminal/overflow handling and shutdown flush. The local read-only report collector is implemented and unit-tested.

The trace is bounded, sampled and not lossless. Remaining gaps include richer class/AI state-machine facts, primary boss selection, more arbitration alternatives than the bounded candidate sample, per-hit health deltas for ordinary non-PlayerBots Group members, complete cross-map/instance closure and integration into the deployment diagnostics command. The wrapper integration exists in Draft `tyrael9/vmangos-deploy` PR #1 and has syntax/collector CI evidence, but remains unmerged and has not been tested on the live Oracle host.

For the full event schema and fixture acceptance records see DECISION-TRACE.md. Do not treat Shadow candidates as proof of action execution or cast acceptance.

## Post-fight collection workflow

Target experience:
1. Finish the Boss encounter.
2. Run one read-only diagnostics command on the Oracle host.
3. Attach the encounter report and raw matching log lines in chat.
4. Analysis classifies failures as perception, policy, coordination, eligibility, execution, encounter-model or trace-gap issues.

The repository-local collector is available from the Core checkout:

```sh
python3 tools/partybot_trace_report.py /path/to/mangosd.log --encounter-id 12345 --output-dir ./partybot-reports
```

When the encounter ID is unknown, a time window can discover encounters represented in that period:

```sh
python3 tools/partybot_trace_report.py /path/to/log-directory --since 2026-11-19T05:00:00Z --until 2026-11-19T06:00:00Z --output-dir ./partybot-reports
```

The tool writes a Markdown timeline/diagnostic summary and a companion raw log containing all available trace lines for the selected encounter IDs. Input files are never changed. Time-window arguments must use the same timezone style as source log timestamps. Integration is present in Draft `tyrael9/vmangos-deploy` PR #1, including exact-ID/time-window selectors and a mode-restricted report directory; its isolated workflow passed syntax/collector checks, but the PR remains unmerged and has not been exercised on the Oracle host. The wrapper must remain read-only: it must not restart containers, change configuration, run SQL, access/mutate database volumes or deploy images.

The deployment repository's Draft PR #1 contains the read-only report collector integration. Until that PR is merged and run on the live host, the existing deployed command must not be assumed to provide full encounter reports.

## Pure decision-engine tests

Run from the repository root:

    bash src/game/PlayerBots/Engine/tests/run_tests.sh

Current test coverage includes deterministic priority ordering, one action per channel, Possible/Useful rejection, missing action registration, reservation conflicts/expiry, non-finite priority rejection, snapshot cache freshness, registry identity and role-aware candidate generation.

These tests cover pure decision logic only. They do not validate live PartyBotAI integration, map-thread safety or vMaNGOS action execution.

## New Engine acceptance gates

### Generic engine/runtime foundations — implemented and tested

- Value cache, trigger cadence, lifecycle invalidation and reset hooks.
- Bounded immediate and pending queues, fresh dequeue revalidation, one shared eight-action decision-cycle budget, channel and aggregate threat admission.
- Exclusive NewEngine authority with an explicit empty-registry no-op and zero Legacy policy fallback.
- Bounded ActionResult/Core Spell terminal listeners; typed AND/alternative dependencies; late event registration; idempotency tombstones; starvation resistance; ready-backlog preservation under queue pressure.
- Core-backed spell, movement/follow, range/LOS/resource, Group/death recovery and NewEngine authority fixtures.

## F43 CoreAction executor and seventeen generic adapters

### F43 exact own-cast cancellation (trusted-runtime validated)

The trusted Core simulator creates a real Player in a one-member Group, starts two successive non-triggered Player spells with the same spell ID and target, and captures unique Engine action/encounter/target identities through PlayerAI::OnSpellCastPreparing. PartySnapshotBuilder must copy those identities as values. A stale first-cast snapshot is then executed while the second cast owns the same Core slot; the action must reject the mismatched action ID and leave the replacement untouched. A fresh snapshot then cancels the replacement through SpellCaster::InterruptSpell, requiring one matching Interrupted Core terminal and no target-health or Player-mana effects. Repeating the old request after cancellation must be rejected. The fixture asserts Group, Player and Creature cleanup. This covers real PREPARING cancellation; Engine arbitration also rejects real DELAYED Fireball and CHANNELED/CASTING Mind Flay snapshots as RejectedThreatUnknown. The fixture separately invokes the CoreAction wrapper in isolation to validate actual Core cleanup: delayed Fireball impact suppression, Mind Flay Interrupted terminal, cleared current channel slot and UNIT_CHANNEL_SPELL, target aura removal and no further ticks. Only PREPARING receives NoThreat; isolated calls do not grant Engine eligibility. The trusted runtime logged snapshotIdentity=1, firstInterrupted=1, replacementIdentity=1, preparingNoThreat=1, delayedUnknown=1, castingUnknown=1, delayedStateDriftRejected=1, castingStateDriftRejected=1, staleRejected=1, replacementPreserved=1, selected=1, cancelled=1, repeatedRejected=1, noEffects=1, fireballDelayed=1, fireballThreatRejected=1, fireballCancelled=1, fireballImpactSuppressed=1, mindFlayChannel=1, mindFlayThreatRejected=1, mindFlayCancelled=1, mindFlayAuraRemoved=1, mindFlayNoFurtherTicks=1 and cleanup=1. Trusted push [#45063532285](https://github.com/tyrael9/core/actions/runs/45063532285) reached final Runtime PASS; matching PR ARM64 build [#45063539061](https://github.com/tyrael9/core/actions/runs/45063539061) passed. Artifact: [partybot-simulator-931b5e3](https://github.com/tyrael9/core/actions/runs/45063532285/artifacts/14770955128). Nothing is registered in production NewEngine.

### F44 initial DPS assist policy-composition coverage (simulator/test-only)

The trusted Core fixture [#44992135927](https://github.com/tyrael9/core/actions/runs/44992135927) proves Main Tank target priority over a healer-peel candidate, crowd-control exclusion, and staged dispatch through the existing SelectHostileTargetAction then AttackTargetAction. The pure C++14 regression now also covers:
- Main Tank (300) versus healer peel (260), and non-Main-Tank Tank assist (280) versus healer peel (260).
- Selection-to-attack progression after a fresh snapshot, including switching to the Main Tank target while the bot's current victim is different.
- Suppression of the bot's already-current victim and crowd-control-protected targets.
- Fallback to online, accessible party threat references when the hostile's victim is not a party member; offline/inaccessible references are ignored.
- No proposal when only dead/offline members assist, no party member can be resolved as victim/assist, or the bot is not an online living DPS role.

This remains simulator/test-only. No production Strategy registration, generic action registration or NewEngine execution is enabled.

The common CoreAction wrapper requires a nonzero action ID, fresh map-thread actor/target/map resolution and a concrete ValidateCoreAction pass before ExecuteCore. ActionExecutionResult and ExecuteDetailed preserve exact validation/execution reasons in candidate state and traces. Pure C++14 regression coverage proves immediate and pending dispatch both use the action-specific preflight, a veto prevents ExecuteCore, and the exact veto reason survives into the candidate result.

SelectHostileTargetAction only calls Player::SetSelectionGuid for a live target that passes Core Unit::CanAttack. It does not call Unit::Attack, cast, move, deal damage or generate threat. The fixture proves a real hostile can be selected and a friendly Creature falsely present in a stale positive HostileSnapshot is rejected by the final live check, leaving selection/target/victim unchanged.

ClearIdleTargetAction clears a stale selection only when the Player is genuinely idle:
- Snapshot eligibility requires alive, nonzero selection GUID, no victim, no combat flag/current spell and no candidate target GUID.
- The final live preflight verifies the snapshot selection still matches both live Player selection and target GUIDs; it rejects an active victim, combat, melee attack state or any current non-melee Spell slot.
- The only side effect is Player::SetSelectionGuid(ObjectGuid()). It does not call AttackStop, CombatStop or interrupt casts.
- The real Core fixture sets a non-resolving selection GUID on a live Player, dispatches the action and proves the stale selection/target becomes empty while victim stays null and combat stays false. It then re-runs hostile acceptance and stale-friendly rejection in the same fixture.

ClearStaleTargetAction is distinct from idle selection cleanup: it removes a selected GUID whose Unit has disappeared, died or left the actor's Map while preserving a different live combat victim:
- Snapshot eligibility requires a non-self selected Unit GUID distinct from the current victim and an active combat/victim state.
- Final preflight verifies the exact live selection/TargetGuid, victim GUID and combat bit, rejects selection drift, still-live selections, and every active non-melee Spell slot.
- Execute only calls `Player::SetSelectionGuid(ObjectGuid())`; it does not stop the victim or alter melee state, combat, attacker references, health or threat.
- The real Core fixture covers a selection race to a different live friendly target, then the accepted stale-selection path. It requires `registered=1 selected=1 driftRejected=1 cleared=1 victimPreserved=1 combatPreserved=1 attackersPreserved=1 threatUnchanged=1`. Trusted push [#44945182411](https://github.com/tyrael9/core/actions/runs/44945182411) emitted the dedicated PASS marker and completed final `Runtime PASS`.



StopUnattackableTargetAction handles a stale current victim without confusing `AttackStop` and `CombatStop`:
- The value-only party snapshot copies the current victim GUID and a `victimAttackable` boolean from live Core state. The action is proposed only when a victim GUID exists and that snapshot boolean is false.
- Final Core preflight resolves that GUID again on the actor's Map, checks that it is still the Player's exact current victim, and re-runs live `CanAttack` plus GM/evade checks. If the snapshot is stale and the victim remains attackable, it returns Useless before any side effect.
- For a live unattackable victim, the action calls only `Unit::AttackStop()`. Postconditions require null victim, empty target GUID, cleared melee state, no actor entry in the victim's attacker set and unchanged Player combat flag. It does not call `CombatStop`, clear other attackers or interrupt non-melee casts.
- The real Core fixture proves an attackable hostile is not proposed, a deliberately stale negative snapshot is rejected without disturbing the attack, and a live faction change makes the target unattackable so AttackStop detaches only the actor reference.

FollowPartyMemberAction starts or retargets only the native Core `FollowMovementGenerator<Player>`:
- PartySnapshotBuilder copies the current FOLLOW target GUID into value-only `followTargetGuid`; neither the generator nor a Core object pointer enters the snapshot.
- Snapshot eligibility requires a live same-map/instance party Player and an idle/no-victim/no-current-spell bot.
- Final Core preflight re-resolves the Player, verifies both Players still belong to the exact same live non-BG Group, rechecks map/instance/alive/friendly/control/cast state, and refuses to overwrite unrelated active movement.
- Same-target follow returns Useless without restarting the generator. A request for a different member retargets only an already-active Follow generator or an idle/default movement slot.
- The real three-Player Group fixture verifies first follow start, snapshot target capture, same-target no-op, retarget to a second member, and stale-snapshot rejection after the first member is removed from the live Group. The teardown explicitly removes its synthetic instance bind before Disband so the shared simulator Map is not reset.

StopFollowingAction expires only a matching current native FollowMovementGenerator:
- Snapshot eligibility requires an alive bot with a nonzero value-only `followTargetGuid` and no target argument.
- Final Core preflight requires the live current generator to be FOLLOW and its target GUID/map/instance to match the snapshot; changed target identity is rejected before mutation, while an already-replaced non-FOLLOW movement returns Useless.
- The adapter calls `MotionMaster::MovementExpired(false)`, not `MoveIdle`, `Clear` or `ClearType`. Core finalizes/deletes the active generator immediately or defers deletion safely while `MotionMaster` is updating.
- The Core fixture rejects a stale target GUID, successfully expires a matching follow, then proves a stale stop request cannot replace an unrelated PointMovementGenerator. Trusted push [#44864122550](https://github.com/tyrael9/core/actions/runs/44864122550) emitted `registered=1 snapshot=1 staleTargetRejected=1 stopped=1 pointMovementPreserved=1` and reached final Runtime PASS.

FaceTargetAction corrects the orientation of a selected, live hostile through Core's own facing-only spline:
- `HostileSnapshot` includes finite value-only x/y/z plus a `positionValid` bit. `IsUseful` follows Core `WorldObject::HasInArc(target)` semantics (pi-radian/180-degree front arc) to avoid repeatedly dispatching an already-satisfied request.
- Final Core preflight resolves the actor/target on the same live Map/instance, confirms both selection and target GUIDs still match, re-runs `CanAttack`, rejects GM/evading targets, moving actors, lost-control state and active non-melee Spells.
- `ExecuteCore` calls `Unit::SetFacingToObject`. Core applies orientation asynchronously through the facing-only spline, so `ActionResult::Success` means the request was accepted, not that the final orientation has already changed. The simulator drives the Player's normal `Update` path, then asserts completed facing, unchanged coordinates, selection/target/victim/combat state and no Spell side effects.
- The accepted/rejected fixture also covers already-facing no-op, a stale positive friendly snapshot rejected by live `CanAttack`, and a moving actor rejected without replacing its PointMovementGenerator. Cleanup is asserted. Threat impact is explicitly `NoThreat`; the adapter only changes orientation.

AttackTargetAction starts Core auto-attack only against the currently selected hostile:
- It uses CoreAction's map-thread resolver and repeats live actor/target/map/instance, selection/target GUID and `CanAttack` validation immediately before dispatch; GM and evading targets are refused.
- It refuses mounted Players and `SPELL_AURA_MOD_UNATTACKABLE`, because Core's `Unit::Attack` rejects mounted Players and otherwise removes that aura as an implicit side effect.
- It calls Core `Unit::Attack` directly, not Legacy `PartyBotAI::AttackStart`. It checks that the Player's victim and selection/target remain the requested Unit and that the synchronous request did not change target health or its ThreatManager value. Subsequent auto-attack swings, damage and threat remain Core-owned; `NoThreat` describes only this synchronous request, not those future swings.
- The fixture proves accepted request, stale-positive friendly snapshot rejected by live `CanAttack`, post-evaluation selection drift rejected before dispatch, no synchronous health/threat change and cleanup. Its immediate success result means Core accepted the attack start; it does not claim a melee swing has already landed.
- Trusted push [#44886268553](https://github.com/tyrael9/core/actions/runs/44886268553) reported `registered=1 selected=1 accepted=1 noImmediateDamageOrThreat=1 staleFriendlyRejected=1 selectionDriftRejected=1`, emitted the AttackTargetAction PASS marker and reached final `Runtime PASS`; PR build [#44886273992](https://github.com/tyrael9/core/actions/runs/44886273992) passed. Artifact: [partybot-simulator-ca634bc](https://github.com/tyrael9/core/actions/runs/44886268553/artifacts/14699344464).

StopAutoAttackAction is the ninth generic adapter. It is actor-only and binds the request to the snapshot victim GUID; final preflight re-resolves that exact Unit on the actor's Map/instance and verifies the live victim/TargetGuid identity, melee state and reverse attacker reference. It refuses to run while a non-melee Spell is active, then calls only `Unit::AttackStop()` (not `CombatStop`). The real Core fixture verifies accepted stop, no-victim no-op and post-evaluation victim-drift rejection while preserving the replacement victim and attacker reference. The accepted stop clears victim/TargetGuid/melee state and detaches the actor, while selection, combat, victim health and ThreatManager remain unchanged. A new real Player matrix now covers that refusal: a non-triggered Lesser Heal (2050) is active while the bot auto-attacks, but the snapshot deliberately says no current spell so the action is selected. Live Core preflight returns `Useless` with `stop_auto_attack_non_melee_spell_in_progress`, while preserving the exact current Spell, victim, TargetGuid, melee state, selection, combat state and reverse attacker reference; the fixture then interrupts the cast for cleanup. Trusted push [#44961184939](https://github.com/tyrael9/core/actions/runs/44961184939) logged `activeCastSelected=1 activeCastRejected=1 activeCastCleanup=1` and final `Runtime PASS`.

Trusted push [#44932408831](https://github.com/tyrael9/core/actions/runs/44932408831) emitted `registered=1 selected=1 stoppedOnly=1 noVictimNotProposed=1 victimDriftRejected=1`; its following FaceTarget fixture reported `setupRestored=1 registered=1 selected=1 requestAccepted=1 facingComplete=1 positionPreserved=1 targetStatePreserved=1 alreadyFacingNoOp=1 staleFriendlyRejected=1 movingRejected=1`. The complete fixture-backed simulator reached final `Runtime PASS`; artifact [partybot-simulator-b4b78eab82979d638c36c5f42b5a316d030a53cf](https://github.com/tyrael9/core/actions/runs/44932408831/artifacts/14718730755). PR ARM64 build [#44932414447](https://github.com/tyrael9/core/actions/runs/44932414447) passed pure tests, report collector, configure, full server/realm build and runtime-harness syntax; its fixture-backed simulator was skipped as expected because PR jobs do not receive repository fixture secrets.

MovePointAction is the tenth generic adapter. It operates on the resolved Player itself and requires an actor-only candidate (targetGuid=0), in-bounds snapshot and destination coordinates via Core MaNGOS::IsValidMapCoord, the same Map/instance and active encounter, a live Player with no victim/combat state or active non-melee Spell, no lost-control/cannot-move/possessed state, and a stopped idle/default movement generator. It returns Useless for an already-reached destination and refuses to replace any unrelated active movement. Execute calls only Core MotionMaster::MovePointForEngine, passing the Engine action ID, encounter ID, generated point ID and arrival tolerance. The synchronous ActionResult::Success means only that Core accepted the request; arrival is separate and must be proven by the typed PointMovementTerminalEvent::Reached with matching action ID, encounter ID, mover GUID, point ID and final-distance tolerance. The NoThreat estimate is scoped to the synchronous request itself; future aggro due to path proximity is not predicted by this adapter.

The trusted fixture verifies actor-only resolution, snapshot eligibility, already-there no-op, out-of-bounds rejection, rejection of unrelated movement and preservation of the exact existing MovementGenerator instance, accepted Engine request, matching Reached terminal, final distance and unchanged Player selection/target/victim/combat/health/power. It advances only the local-session Player using Core Unit::Update, so it does not tick unrelated actors in the shared Map. Trusted push [#44943021972](https://github.com/tyrael9/core/actions/runs/44943021972) emitted `registered=1 actorOnly=1 snapshotEligible=1 alreadyThereNoOp=1 outOfBoundsRejected=1 unrelatedMovementRejected=1 sameGeneratorPreserved=1 accepted=1 reached=1 statePreserved=1` and reached final `Runtime PASS`; artifact [partybot-simulator-1f21304216cb6cf349bcede7c5e486def015722b](https://github.com/tyrael9/core/actions/runs/44943021972/artifacts/14722896312). PR build [#44943026168](https://github.com/tyrael9/core/actions/runs/44943026168) passed pure tests, report collector, full Linux ARM64 Vanilla 5875 server/realm build and harness syntax; its fixture runtime was skipped because PR jobs do not receive fixture secrets.

StopPointMovementAction cancels only the Engine-owned current PointMovementGenerator whose value snapshot action ID, encounter ID and point ID all match the live generator. PartySnapshotBuilder copies those scalar fields only from a current POINT_MOTION_TYPE; ordinary Core/Legacy Point generators have zero Engine IDs and are not eligible. Final CoreAction preflight is actor-only and also requires the active encounter Map/instance to match; Execute calls only MotionMaster::MovementExpired(false), never ClearType/MoveIdle/global reset. It asserts Player selection/target/victim/combat/health/power/current Spell slots stay unchanged.

The trusted Core fixture follows MotionMaster::Mutate exactly: pushing Engine move B over A calls A::Interrupt but leaves A lower in the stack without finalizing it. A stale stop for A is rejected while B remains current; expiring B emits B's matching Interrupted event and restores A; expiring A then emits A's own Interrupted event. It also proves a legacy Point move is not proposed/preserved, and a fresh Engine move C is stopped with its exact matching Interrupted terminal. The fixture reports `registered=1 moveAStarted=1 snapshotCaptured=1 selected=1 moveAStacked=1 noPrematureTerminal=1 replacementBStarted=1 staleRejected=1 replacementPreserved=1 replacementInterrupted=1 moveAResumed=1 moveAInterrupted=1 legacyNotProposed=1 legacyPreserved=1 moveCStarted=1 snapshotPossible=1 selectedC=1 stopExecuted=1 stopInterrupted=1 statePreserved=1` and reaches final `Runtime PASS` in trusted push [#44945182411](https://github.com/tyrael9/core/actions/runs/44945182411). Artifact: [partybot-simulator-22b6b5cc57845e54ed09ba5f921be8316aa15817](https://github.com/tyrael9/core/actions/runs/44945182411/artifacts/14723521850). PR build [#44945186718](https://github.com/tyrael9/core/actions/runs/44945186718) passed pure tests, report collector, full Linux ARM64 Vanilla 5875 server/realm build and harness syntax; PR runtime skipped because fixture secrets are not exposed to PR jobs.

DismountForCombatAction is the twelfth generic adapter. It is target-bound rather than an unconditional movement cleanup: its candidate must name the currently selected live hostile and the value-only PartyMemberSnapshot must report `mounted=true`, `taxiFlying=false`. PartySnapshotBuilder captures both fields directly from Core Player state. Final CoreAction preflight re-resolves the exact target on the same Map/instance, rechecks target life, selection/TargetGuid, `CanAttack`, GM/evade exclusions, taxi-flight state, lost control and all non-melee Spell slots. The only intended Core mutation is `Player::Unmount()`; the adapter does not strip mount auras manually or run Legacy policy. Its threat estimate is scoped to the dismount request and is `NoThreat`.

The fixture locates a valid CreatureDisplayInfo DBC row, mounts the real simulator Player through `Player::Mount`, then runs the production adapter through PartyBotEngine/CoreAction. It asserts `mountStarted=1 registered=1 selected=1 accepted=1`, and verifies mount ID becomes zero while selection/TargetGuid, victim/combat, Player health/power, hostile health and ThreatManager remain unchanged. It also replays the old mounted snapshot after success and requires a live-state Useless rejection, changes selection after evaluation and requires a side-effect-free Impossible rejection plus explicit dismount cleanup, and confirms taxi-flight snapshot state produces no candidate. Trusted push [#44964395002](https://github.com/tyrael9/core/actions/runs/44964395002) logged `display=4 mountStarted=1 registered=1 selected=1 accepted=1 alreadyDismountedRejected=1 selectionDriftRejected=1 raceCleanup=1 taxiNotProposed=1` and reached final `Runtime PASS`. Artifact: [partybot-simulator-3fb1621fccbaecd5ef0f88313bf66af3cfdaa034](https://github.com/tyrael9/core/actions/runs/44964395002/artifacts/14731595715); matching PR build [#44964399919](https://github.com/tyrael9/core/actions/runs/44964399919) passed the full Linux ARM64 / Vanilla 5875 build.

Coverage update (2026-11-22; all seventeen generic adapters trusted-runtime validated):

- The five-Player Group-backed point-movement snapshot integration in `RunEngineCoreRealGroupThreatSnapshotSmokeTest` starts Engine-owned and legacy Point generators on distinct real Group members, retains an idle member and invokes the production `PartySnapshotBuilder::GetOrBuild`. Trusted push [#44961184939](https://github.com/tyrael9/core/actions/runs/44961184939) logged `group=1 engineStarted=1 engineCopied=1 legacyStarted=1 legacyUnowned=1 idleEmpty=1 engineRestored=1 legacyRestored=1 idleUnchanged=1` and the dedicated PASS marker. Implementation: `a7297c0`; hard failure gate: `846d00b`.
- `StopAutoAttackAction` now has a trusted-runtime-validated active-cast refusal case. Commit `cfdab47` starts a real non-triggered Player Lesser Heal (2050) while the bot is attacking, deliberately leaves `currentSpellId=0` in the snapshot, and confirms live Core preflight rejects with `stop_auto_attack_non_melee_spell_in_progress`. The trusted log reports `activeCastSelected=1 activeCastRejected=1 activeCastCleanup=1`; the cast, victim, melee state, selection, combat state and reverse attacker reference remain intact until explicit test cleanup.
- Both new gates passed the full fixture-backed Vanilla 5875 simulator on code head `cfdab47` in trusted push [#44961184939](https://github.com/tyrael9/core/actions/runs/44961184939), which reached final `Runtime PASS`. Its matching PR build [#44961189357](https://github.com/tyrael9/core/actions/runs/44961189357) passed pure Engine tests, report collector, configure, full Linux ARM64 server/realm build and harness syntax. Simulator artifact: [partybot-simulator-cfdab4714ff69223b4e56c57e144bf0556c5b92f](https://github.com/tyrael9/core/actions/runs/44961184939/artifacts/14730086190).

Current-head validation:
- PR CI [#44874582915](https://github.com/tyrael9/core/actions/runs/44874582915) passed pure tests, report collector, configure, full Linux ARM64 Core build and harness syntax.
- Trusted push [#44874579164](https://github.com/tyrael9/core/actions/runs/44874579164) passed the same stages and the complete fixture-backed Vanilla 5875 simulator through final `Runtime PASS`. FaceTarget reported `requestAccepted=1 facingComplete=1 positionPreserved=1 targetStatePreserved=1 alreadyFacingNoOp=1 staleFriendlyRejected=1 movingRejected=1`; the enclosing target fixture reported `cleanup=1`.
- Artifact [partybot-simulator-f6986c8](https://github.com/tyrael9/core/actions/runs/44874579164/artifacts/14695646802) contains the complete trusted runtime logs.
All seventeen adapters remain deliberately unregistered in the production NewEngine registry and have trusted Core runtime fixtures. CancelOwnCastAction's full PREPARING/replacement, DELAYED Fireball, CHANNELED Mind Flay and fail-closed threat matrix passed push [#45063532285](https://github.com/tyrael9/core/actions/runs/45063532285). These demonstrate narrow generic operations, not a complete target policy or playable combat AI.
## Production action readiness — still open

- Register audited production generic/class/role Core adapters with structured outcomes.
- Revalidate GUID, target life/relationship, map/instance, range, LOS, spell, resource, cooldown and control state immediately before every adapter side effect.
- Typed PointMovementTerminal and ItemUseTerminal sources are implemented, bounded and correlated; do not infer completion from generic generator finalization or adapter return. Add a dedicated typed terminal/result source for each future asynchronous Core action kind before exposing dependent continuations.
- Complete action-specific threat forecasts and hard safety policy before enabling actions which can cause threat/CC/resource/movement side effects.
- Extend trace facts for missing state-machine/boss context and close remaining cross-map/deployment diagnostics gaps.
- Add in-game tests separately from fixture-backed CI, and keep Legacy as the default until an explicitly approved test-realm validation.

## Validation vocabulary

- Committed: source and documentation are present in the branch.
- CI passed: the latest relevant workflow run completed successfully.
- In-game tested: a scenario was exercised on a test realm.
- Deployed: the user explicitly approved and performed the intended server update.

A CI pass is never an in-game test and never authorizes deployment.


## Core-backed headless combat simulator

The simulator design and pinned-Core API audit are in CORE-COMBAT-SIMULATOR.md. This is intended as a third test layer:

1. pure decision-engine tests (fast, isolated policy logic);
2. Core-backed headless scenarios (real vMaNGOS damage/threat/target/death behavior, no client);
3. live realm tests (networking, client, lifecycle and long-duration behavior).

The initial audit confirmed a useful seam: `MapManager::CreateTestMap` / `DeleteTestMap`, public `Map::Add` / `Map::Update`, `Creature::Create`, `Unit::Attack`, `Unit::DealDamage`, `Unit::AddThreat` and `ThreatManager`. However, test-map construction depends on initialized global Core services (DBC/ObjectMgr, terrain, persistent state, world configuration and related managers). A standalone object-only test is insufficient.

The opt-in `PartyBot.Simulator.Enable = 1` mode runs a real Core map create/register/delete lifecycle check plus a Stage B actor/update/combat scenario, then exits through the normal WorldRunnable shutdown path. It refuses to start unless `WorldDatabase.Info`, `CharacterDatabase.Info`, `LoginDatabase.Info` and `LogsDatabase.Info` all point to schema names beginning with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`). Simulator mode also skips PID-file creation, realm-online/offline updates, account cleanup, mass-mail processing and all client-facing listeners. Set `PartyBot.Simulator.MapId` to a valid dungeon/raid map (default 33). Run `mangosd` with a disposable copy of all four DBs and the normal client data directory; it does not open the world, RA or SOAP listeners. A successful exit requires map registration/cleanup and the Stage B Core actor/update/damage/threat/target-switch/cleanup assertions. The startup audit found a candidate hook in `Master::Run`: immediately after `sWorld.SetInitialWorldSettings()`, before `WorldRunnable` and before the world listener is opened. Startup already performs DB writes and starts background workers, so simulator mode must use a dedicated disposable DB/configuration and reuse normal shutdown/cleanup; it cannot simply return early. `Map::Update(diff)` takes an explicit diff but still reads wall-clock/global services, so fixed diffs alone do not prove deterministic full-map ticks. The implementation must test or isolate those dependencies. The fixture-backed runtime test has now exercised the temporary Creature scenario; see the current status below. Do not connect the PartyBotEngine executor until the wrapper reports a clean end-to-end pass.


The Stage B source audit has now verified the temporary-Creature lifecycle contract:
- Create actors only from loaded `CreatureInfo` templates and explicit test positions.
- Call `Creature::AIM_Initialize()` before replacing the template AI. It initializes the `MotionMaster` as well as selecting AI. `Creature::SetAI` is a raw pointer setter; delete the prior AI explicitly before installing the simulator-owned probe AI.
- Do not tick the map before Stage B actor setup; the first meaningful map tick occurs after all simulator actors have valid movement generators.
- In an empty headless instance, ordinary non-active Creatures are not guaranteed to receive `Creature::Update`: `Map::UpdateCells` visits cells around players and active non-player objects. Mark simulator actors active before `Map::Add`, and assert the custom AI update counter advances after a tick. Respect the configured wall-clock cell-update cadence; do not count an empty map tick as an actor update.
- Check template-driven spawn spells: `Map::Add` invokes `Creature::CastSpawnSpell()` after insertion.
- Detach temporary actors with `Map::Remove(creature, false)`, then explicitly destroy them. Never use `remove=true` for these actors because that path can save respawn time.
- Remove/destroy every actor before `MapManager::DeleteTestMap`.

The first Core-backed scenario must report separate assertions for actor insertion/lookup, real `Unit::DealDamage` health/death effects, real threat references, `ThreatManager::getHostileTarget` selection and switching, custom `CreatureAI::UpdateAI` callback delivery, and cleanup. Calling a Core method directly is valid evidence for that method's semantics, but must not be presented as proof that a full autonomous combat loop or map tick executed it.


### Stage B Core Creature damage/threat smoke test (runtime and runner gate passed)

The opt-in simulator now attempts a three-Creature scenario on its isolated test map. It chooses an ordinary non-NPC, non-civilian, non-pet normal-rank template with no immunities or spawn/script side effects, plus a mutually hostile faction-template pair from the loaded DB/DBC data. It creates a threat owner plus two hostile actors and verifies each actor is registered by `Map::GetCreature`. Each actor first runs `Creature::AIM_Initialize()` and then replaces the template AI with a probe AI before insertion. The actors are active objects; bounded real map ticks must increment all three AI update counters.

The damage sequence uses real `Unit::DealDamage`: target A deals 100, target B deals 105, then B deals 10 more to the threat owner. The assertions require health loss, damage-generated threat references, target A retained below the 110% melee threshold, and target B selected after crossing it. Logs include exact health/damage/threat values, selected GUIDs, AI update counts and cleanup results. The actors are detached with `Map::Remove(actor, false)`, their threat references are cleared, and map lookup must return null before explicit destruction.

This test uses Creature actors, not Player actors, and does not yet exercise spell casts, healing, movement, PartyBotEngine decisions/execution or encounter scripts. It is a smoke test of real Core damage/threat/update paths only. The bounded tick loop uses wall-clock pacing and is not yet a deterministic replay facility. The implementation passed full Linux ARM64 / Vanilla 5875 compile/link CI; this is build evidence only. The optional GitHub Actions runtime runner is implemented in `tools/partybot-simulator/run-ci.sh`. It runs only on trusted branch pushes and only performs runtime work when both Actions secrets are configured: `PARTYBOT_SIMULATOR_DATA_URL` (HTTPS Google Drive share URL to a tar.gz containing the full extracted-data tree: `5875/dbc`, `maps`, `vmaps`, `mmaps`) and `PARTYBOT_SIMULATOR_DATA_SHA256`. The runner validates those directories, starts the deployment's public vMangos database image, clones all four schemas to the required `partybot_sim_*` prefix, runs `mangosd` against those clones and uploads console/file logs for 14 days. It does not access the Oracle realm. If the secrets are not configured, the workflow explicitly skips runtime execution.

The first configured-fixture run exposed and fixed the empty-`MotionMaster` lifecycle bug. The next run confirmed the fix and passed the actual Core scenario: AI update counters `1/1/1`, health reduction, damage-generated threat, 110% target switch, three successful actor cleanups and map deletion. `mangosd` exited successfully. The workflow was marked failed only because the shell runner still matched the old final PASS string containing `ticked`; the runner was corrected to check the actual combat, actor-cleanup and map-lifecycle PASS lines. The later full fixture-backed push run `#44238072768` on `d23d865` passed the corrected runner and the complete simulator, including all P7.3d budget/race assertions.

The Core Creature update/damage/threat scenario and the fixture-backed Stage A-F12 simulator passed on disposable schemas and real 5875 data in push run `#44238072768`; Stage F13 adds a party-shared encounter trace-budget overflow regression with two real PartyBotAI instances. F13 runtime verification is pending the current push CI.

Do not treat the existing pure tests as Core combat coverage. Do not use mocked damage/threat or direct state mutation as proof of Core behavior. The simulator must be bounded, seedable and explicit about remaining nondeterminism. It must not publish or deploy images.


## Threat-impact reservation coverage

The standalone C++14 suite in src/game/PlayerBots/Engine/tests/run_tests.sh now covers the P7.3c coordinator contract without constructing live Core objects: action fan-out across hostiles and actual threat owners, idempotent row refresh, aggregate pending threat, action completion cleanup, bot-owner cleanup when threat is attributed to a pet GUID, wrap-safe deadline expiry, invalid/expired input rejection, and the bounded 128-row fail-closed capacity. This validates only the reservation data structure and lifecycle; P7.3d still needs to connect these values to action eligibility and prove the result against real Core-generated threat.


## Threat budget policy and Engine admission

The standalone suite exercises the P7.3d policy path as well as the P7.3c reservation container. Complete estimates are aggregated per hostile and actual threat recipient; admission subtracts existing in-flight reservations and higher-priority candidates from the same report. Tests cover multi-hostile fan-out, unresolved/duplicate/offline companion and missing-owner cases, primary Pet GUID threshold projection, Pet-targeted reservations with Player-owner cleanup, current-victim handling, stale epochs, execution-time reservation, failed-action rollback and no-selection when projected threat exceeds headroom. Positive complete estimates require a finite bounded reservation duration. Full-fixture generic admission/race coverage passed in push CI #44238072768; direct-heal assist upper-bound coverage passed in #44242344188; primary controlled Pet identity/budget/runtime cleanup passed in #44255823652.


## Core-backed threat budget admission scenario

The combined production five-Player Group/two-add simulator fixture includes the P7.3d budget/race checks and direct-heal-assist component validation. The generic path creates real assist-threat references through Core, reads hostile/member entries with PartySnapshotBuilder, checks 110% melee and 130% ranged projections, and evaluates over-budget and safe Engine candidates. It verifies no ThreatManager/current-victim mutation before damage, compares the Core damage delta with ThreatCalcHelper, tests execution-time reservation races and releases the action reservation. A second snapshot is built immediately before a real non-triggered Player Lesser Heal; the test estimates its direct-heal assist fan-out using current missing health, then compares each hostile's actual ThreatManager delta to its per-hostile upper bound. Push `#44242344188` passed with `upper=85.8/85.8`, `delta=14.5/14.5`. The helper also now enumerates a positive flat `spell_threat` row for the same healed target when Core's effect mask permits it, but this fixture's Lesser Heal has no nonzero flat entry; push `#44251387021` validates the effect-mask integration and full runtime regression only, not a nonzero flat-threat example. Both trusted pushes reached the full simulator `Runtime PASS` and uploaded logs.


Additional P7.3d regressions cover the gap between selection and execution: a competing bot reservation inserted after Evaluate must cause ExecuteSelected to revalidate and refuse execution. The bounded-capacity test starts with 127 unrelated rows and attempts a two-recipient reservation; the first partial row must be rolled back when the second cannot fit, leaving all 127 original rows intact.


## Primary controlled Pet threat snapshot and budget scenario

The production snapshot builder records the online primary Pet returned by each same-map Group Player as a separate companion value. The actual threat owner remains the Pet GUID. Its owner Player GUID is checked against the Group roster and is used only for logical-action cleanup; it is never substituted as the ThreatManager recipient.

The pure suite covers a valid Pet threat row using the Pet's own online/accessible/melee-reachability values and the Core 110%/130% threshold, plus duplicate companion rows, missing/offline owner, offline Pet, Pet current victim, Pet GUID reservation and Player owner ReleaseOwner cleanup.

The full F9 Core fixture creates a real temporary Pet(SUMMON_PET) owned by the Healer, registers it through the Core Pet/Map lifecycle, inserts a 30-point threat reference through Core, and rebuilds the normal five-Player Group snapshot. Assertions require petCreated=1, petSnapshot=1, petBudget=1, petThreat=30, petHeadroom=80, and petMelee=1; Pet is then detached after its HostileReference is removed and without saving or unsummoning persistent pet data. Trusted push CI #44255823652 on 295368c passed against the full Vanilla 5875 fixture, reached the simulator-wide Runtime PASS, and uploaded artifact partybot-simulator-295368c68574cd8f5e3c0e6de8e36aa21a5017e0.

Scope is limited to the primary controlled Pet. Guardians, totems, temporary summoned creatures and charmed units remain Unknown until equivalent owner/companion snapshot support exists.

## Running the full fixture-backed simulator

The PartyBot Core build workflow keeps Google Drive fixture secrets out of pull-request jobs. On this development branch, the full runtime, secret-presence checks and log-artifact upload are push-only. The workflow also declares workflow_dispatch for future use, but GitHub only exposes that manual trigger after the workflow file exists on the repository default branch; this Draft PR has not added it there. The job reports only whether the URL/checksum secrets are present, never their values; it downloads the archive, verifies SHA-256, runs disposable schemas and uploads simulator logs. It has no image-publish or deployment step.

## Encounter trace lifecycle and heartbeat coverage
The encounter lifecycle suite also verifies shard isolation: once a coordinator has an active encounter, snapshots with a different group ID, map ID or instance ID are ignored whether they report combat or idle. They cannot refresh the active encounter's settle clock or close it. A matching source-shard idle snapshot still closes at the exact configured settle boundary. This is pure coordinator ownership coverage; the Core-backed source/destination Map transition fixture remains open.


The pure C++14 suite covers the shared encounter lifecycle and initial roster heartbeat contract without Core object pointers. It verifies process-unique encounter start, party-member/companion combat detection, duplicate start suppression, quiet-settle end, explicit close, uint32 clock wrap, and monotonic sequence allocation. Heartbeat regressions verify that a map/instance coordinator emits once for all bots, rejects invalid intervals, enforces the configured cadence, shares the encounter sequence, resets its gate for a new encounter and remains wrap-safe. The runtime Shadow integration logs one bounded `party_snapshot` line per active encounter interval; it caps members/companions/hostiles at 5/5/8 and includes explicit omitted counts plus a payload-truncated flag.

The heartbeat is an observational trace only. It does not execute actions, alter Core threat/victim state, retain Core pointers, or make the existing Shadow evaluation authoritative. Full bot-state, trigger/action lifecycle, threat samples, and post-fight collection remain separate Phase 3 work.

## Encounter-correlated Shadow decision trace coverage

During an active encounter, the read-only Shadow evaluation emits one bounded decision summary and at most three prioritized candidate records inside the existing per-bot ten-second detail window. Each record receives its sequence from the shared PartyCoordinator, so party heartbeat, summaries and candidates can be ordered under one encounter ID. Candidate fields include snapshot epoch, role, strategy/action, spell ID, action channel, target GUID, finite-priority validity, status and a token-safe reason. Correlated decision records are separately bounded by the ten-second Shadow window (one summary and at most three candidates) and therefore are not silently dropped by the ordinary per-tick legacy diagnostic quota. The event source is `PartyBotEngine::Evaluate` only; it does not call `ExecuteSelected` and is not evidence that a spell/movement action was attempted or accepted.

These records currently rely on the existing pure evaluation/cadence tests plus ARM64 compile/runtime regression. A dedicated log-format parser test and authoritative action-result correlation remain open.

## Encounter-correlated Legacy cast request result trace

The existing `CombatBotBaseAI::DoCastSpell` path invokes `TraceDecision("spell_cast_attempt", ...)` immediately before the real `Player::CastSpell` call, then invokes `TraceDecision("spell_cast_result", ...)` with the actual synchronous `SpellCastResult`. PartyBotAI emits paired `cast_request_attempt` / `cast_request_result` records sharing an `action_id`. During Core `Spell::prepare`, the owning PlayerAI binds that attempt once to the root Spell object by matching spell ID and any Unit target GUID Core exposes. Destination-location spells intentionally have no Core Unit target GUID, so the original PartyBot target GUID is copied into the Spell trace context separately without changing Core targeting behavior. The root Spell carries only numeric action/encounter IDs and the original trace target GUID; nested triggered spells do not inherit the context. At `Spell::finish`, the PlayerAI callback emits `cast_terminal` with the authoritative `ok` state; `Spell::cancel` sets an interrupted flag before finishing. This handles both synchronous instant completion (terminal can occur before the request-result callback) and asynchronous cast/channel completion (terminal occurs later). Cast lifecycle records consume the shared encounter-wide important-event budget (256 by default, with a hard cap of 1,024) across all PartyBots; the first overflow emits one shared `trace_limit` marker. Runtime limits are configured through `PlayerBot.EngineEncounterMaxImportantEvents`. `SPELL_CAST_OK` still means the request was accepted, while `cast_terminal` reports final Spell outcome; neither means target damage/healing was necessarily applied.

Validation in this slice consists of the C++14/API build plus the full fixture-backed Core regression. The simulator independently proves the Core Player cast acceptance, pending-cast/GCD, completion, interruption, mana and healing semantics, but it does not yet assert the PartyBotAI structured log line itself. A dedicated trace-output parser/integration assertion remains open. Direct raw `Player::CastSpell` sites outside `DoCastSpell`, terminal events after encounter closure and downstream damage/heal application are not yet traced by this hook.

## Encounter heartbeat threat samples and target-change trace

The existing immutable `PartySnapshot` already copies each hostile's real `ThreatManager` references on the map thread. The encounter `party_snapshot` record now emits up to two references per hostile, including unit GUID, threat value in tenths, and online/accessibility/melee-reachability bits. It includes each hostile's total reference count and omitted count; the complete payload remains capped at 1600 characters with the existing `truncated` marker. It does not sort or infer references beyond the order supplied by the Core snapshot.

After a successful `PartyBotAI::AttackStart`, the trace compares the actual Core `GetVictim()` before and after `me->Attack`. During an active encounter, a changed victim emits one `target_change` record with old/new victim GUID, `source=legacy_attack_start`, and the shared encounter sequence. Same-target calls and failed Attack attempts do not emit target-change records. Target changes consume the shared encounter-wide detail budget (1,024 by default, with a hard cap of 4,096) across all PartyBots; runtime limits are configured through `PlayerBot.EngineEncounterMaxDetailEvents`.

This slice does not yet emit coordinator reservation acquire/release/conflict events, health/death transitions, or target changes initiated by other Core sources. No threat values are reconstructed from health or inferred from the selected Shadow candidate.

## Engine threat reservation trace coverage

`DecisionContext` accepts an optional `ReservationTraceSink`; the Legacy PartyBotAI supplies its encounter `DecisionTrace`, while standalone callers and Shadow-only evaluation can leave it null. `PartyBotEngine::ExecuteSelected` emits events at the real budget/reservation coordinator boundaries: budget veto, per-row reservation attempt and accepted/rejected result, partial fan-out rollback, and release after action execution failure. Events preserve action ID, logical owner GUID, actual threat-owner GUID, hostile GUID, threat/headroom/reserved values, expiry and reservation-row count. They are observational; they do not claim that a later Core effect applied.

The pure C++14 suite verifies accepted acquisition, action-failure release, a competing reservation inserted between Evaluate and Execute (budget veto with no second action), and partial multi-recipient capacity failure with rollback of the already-acquired row. It asserts event order, action identity, owner/hostile identity, quantities, rejection reason and release row count. Reservation outcomes consume the shared encounter-wide important-event budget across all PartyBots; the first overflow emits one shared `trace_limit` record. The full fixture-backed ARM64 runtime remains the integration gate for this slice.


## F27 source-to-destination instance transfer regression

The fixture creates a real Stockades dungeon Map (map ID 34) while the source is Razorfen Kraul (map ID 33). This uses a different dungeon map ID because Core rejects entry into another instance of a dungeon map ID while the Group remains bound to the previous instance. It moves the complete five-Player Core Group through `Map::Remove(Player*, false)` and `Map::Add(Player*)`, with two real PartyBotAI observer instances attached. The source encounter must remain active while one PartyBotAI remains on the source Map and close exactly once when the final bot leaves. Both PartyBotAI coordinator references must be cleared by the source `OnMapLeave` callbacks.

After destination insertion, the fixture calls the normal `PartyBotAI::UpdatePartyBotEngineShadow` entry point (Shadow is enabled only in the disposable simulator config). It requires both bots to reacquire the same new `(group,map,instance)` registry shard, verifies a real five-member destination snapshot, and requires a distinct active encounter ID. It then moves every Player back to the source Map, requires the destination encounter's one-shot terminal admission to be consumed, verifies both bot references are cleared and all five Players are restored, and explicitly removes the disposable GroupBind for map 34, then deletes the temporary destination Map. The CI runner requires the F27 PASS marker and exactly three `last_partybot_left_map` end records: the existing F16 regression plus the F27 source and destination closures.

This is a different-Map-ID dungeon transfer regression. It does not yet cover a different Map ID or every production teleport/instance-script ordering. Trusted push CI [#44645889761](https://github.com/tyrael9/core/actions/runs/44645889761) emitted the F27 PASS marker and completed source/destination teardown successfully. The first aggregate runner attempt failed on an F17 trigger-order grep that was not scoped to F17's encounter ID. The gate was corrected; trusted push CI [#44647149956](https://github.com/tyrael9/core/actions/runs/44647149956) later passed the complete ARM64 fixture-backed simulator and final `Runtime PASS`.


## F28 Engine-owned pending action integration

The pure Engine suite covers the Engine-owned cross-tick queue, not only the standalone storage primitive. A concrete Action opts in through GetPendingLifetimeMs; the default is zero, so actions that do not explicitly request a bounded lifetime remain immediate-only. Queue entries contain copied ActionCandidate fields and a context key for group/map/instance/bot/encounter, not Core pointers.

The Engine validates current eligibility at enqueue and repeats Possible/Useful, threat-impact estimation, multiplier priority, channel arbitration and threat-budget assessment when processing the pending queue with a new DecisionContext. Same-channel losers and temporary threat-budget failures can be deferred, but Requeue preserves the original expiry and FIFO ordinal. ResetDecisionState clears pending work; ordinary decision invalidation leaves it for fresh revalidation. PartyBotAI encounter boundaries, map leave, party/instance replacement and shutdown use clear/reset paths.

The tests exercise an action becoming useless after enqueue, a foreign group/map/instance context, priority ordering across same-channel pending actions, and expiry after a deferral. The queue APIs remain disconnected from Legacy and read-only Shadow ticks; F30 invokes the decision-cycle dispatcher only under explicit NewEngine authority.


F28 verification: trusted push CI [#44652673352](https://github.com/tyrael9/core/actions/runs/44652673352) reported `PartyBotEngine tests passed`, completed the Linux ARM64 server/realm build and simulator harness checks, and reached the final Core simulator `Runtime PASS`. PR CI [#44652680243](https://github.com/tyrael9/core/actions/runs/44652680243) also passed the build/test path. The fixture validates Core lifecycle compatibility with pending-queue teardown hooks; direct pending dispatch is exercised by pure C++ tests and is now wired only into the explicit NewEngine decision cycle (F30), not Legacy or Shadow.


## F29 execution authority mode

The runtime exposes three explicit modes through `PlayerBot.EngineAuthority`:

- `0` Legacy: existing behavior and default.
- `1` Shadow: read-only PartyBotEngine evaluation; Legacy remains the sole executor.
- `2` NewEngine: exclusive Engine authority. `PartyBotAI::UpdateAI` returns before Legacy follow/healing/combat/class policy. Unknown settings resolve to Legacy.

The old `PlayerBot.EngineShadow` boolean remains a backward-compatible way to enable read-only evaluation while the authority is Legacy. NewEngine forces Engine evaluation, does not register `shadow_*` candidate probes as executable strategy policy, and processes only actual registered Engine work and pending actions. A reusable generic SelectHostileTargetAction is now implemented and Core-tested but deliberately not registered; no production class/role Core action adapters are registered. An empty registry therefore still deliberately does nothing rather than invoking Legacy. Pure C++ tests cover mode selection; trusted Core-backed runtime Push CI [#44673953340](https://github.com/tyrael9/core/actions/runs/44673953340) now drives the real `PartyBotAI::UpdateAI` with a per-instance NewEngine override and asserts `newEngineTicks=1`, `decisionCycles=1`, `candidates=0`, `shadowPoliciesRegistered=0`, `legacyEntries=0`, `legacyDispatches=0`, and a valid coordinator. The full simulator emitted the dedicated F29 PASS marker and final `Runtime PASS`; PR CI [#44673960390](https://github.com/tyrael9/core/actions/runs/44673960390) passed ARM64 build/tests.


## F30 NewEngine decision-cycle integration

The pure Engine suite exercises the single `ExecuteDecisionCycle` entry point. A selected TTL-enabled action is routed through the Engine-owned pending queue and reflected back into the source report as Executed when same-tick pending dispatch succeeds. Immediate actions reserve their channels before pending dispatch; a pending action on the same channel stays queued and expires at its original deadline if the channel remains occupied. The pending dispatcher shares the immediate action budget and accumulates selected threat impact across pending candidates before evaluating subsequent entries.

The fixture-backed Vanilla runtime still validates Core lifecycle compatibility; live NewEngine action execution remains opt-in and lacks production class adapters.


F30 additionally verifies pending-dispatch threat aggregation. Two different-channel pending actions each estimate 12 threat points to the same hostile/recipient pair; the first is selected and executes, while the combined second projection is deferred and expires at its original deadline.


F30 CI acceptance: trusted push run [#44664709181](https://github.com/tyrael9/core/actions/runs/44664709181) passed the pure suite, report collector, ARM64 build and fixture-backed Vanilla 5875 simulator through final `Runtime PASS`. The F20 shell fixture assertion was updated to require the new `pending_queued=0` field in the complete decision-summary schema; the expected F20 PASS marker appeared on this run. PR run [#44664715713](https://github.com/tyrael9/core/actions/runs/44664715713) passed the pure/build path and correctly skipped the push-only runtime fixture.


## F31 synchronous action result listeners

The Engine exposes a per-instance registry of at most 32 `ActionResultListener` objects. Null, duplicate and over-capacity registration is rejected. Each final dispatcher result emits a value-only `ActionResultEvent` with the copied candidate, nonzero action ID when allocated, result, whether `Action::Execute` was actually attempted, snapshot epoch, dispatch timestamp and reason. The event is delivered even when neither trace nor observer sink is installed. Listener pointers are snapshotted before callback dispatch; registering another listener during a callback only affects later results.

The event represents an Engine dispatch result or synchronous `Action::Execute` return. It does not prove a later Core Spell finished successfully; async Core-terminal events require a separate action-ID correlation and explicit completion stage.


F31 verification: pure regressions cover listener delivery without trace sinks, dispatcher metadata/action ID, null/duplicate/full-capacity rejection and callback-time registration semantics. Current trusted Push CI [#44673953340](https://github.com/tyrael9/core/actions/runs/44673953340) passed those tests, the ARM64 build and full Vanilla 5875 simulator; PR CI [#44673960390](https://github.com/tyrael9/core/actions/runs/44673960390) passed the build/test path. This validates the synchronous dispatcher-result listener boundary only; it does not turn the later `Spell::finish` terminal into the same event.


## F32 Core Spell terminal-result listeners

The Engine owns a bounded CoreSpellTerminalListener registry. Core Spell::prepare binds the current Engine action ID and source bit to the root Spell; Spell::finish later sends a value-only event back through the same PartyBotAI-owned Engine. This terminal stage is distinct from the synchronous ActionResultEvent emitted when Action::Execute returns. The event contains action ID, encounter ID, target GUID, spell ID, completion time and Succeeded/Failed/Interrupted status, with no Core pointer or retained DecisionContext. Listener delivery is independent of DecisionTrace; callbacks use a stable listener snapshot.

Pure tests cover value retention, Succeeded/Interrupted status, null/duplicate/full registration and callback-time registration. Trusted Push CI #44677767691 passed the pure suite, ARM64 build and complete Vanilla 5875 simulator. The real Player heal fixture registered the listener on the same PartyBotAI Engine and received one successful terminal correlated to the original action/encounter/target/spell. PR CI #44677772862 passed the pure/build path and intentionally skipped the push-only fixture. This completes terminal-result transport, not production action adapters or gameplay readiness.

## F33 PendingActionQueue starvation resistance

The queue uses a bounded 3,000 ms age-promotion threshold. Entries waiting less than the threshold retain current revalidated priority-first/FIFO ordering. Once an unexpired entry reaches the threshold, it outranks all not-yet-promoted entries; among promoted entries, the longest wait wins and original FIFO ordinal breaks ties. Unsigned elapsed-time arithmetic is safe across uint32 millisecond wrap because every live TTL is less than 2^31 ms. Duplicate refresh and requeue preserve the original enqueue time, ordinal and expiry.

The executor uses the same comparator after revalidation and refreshes the local priority from the current candidate evaluation before sorting. Pure tests cover the 2,999/3,000 ms boundary, continuous higher-priority arrivals, ordering among promoted entries and clock wrap. An Engine integration test uses same-channel queued work to prove that a long-waiting lower-priority action executes first and the fresh higher-priority action is deferred to the next tick.

Trusted Push CI [#44680955006](https://github.com/tyrael9/core/actions/runs/44680955006), head 5ae65fd4167779a9d98f0c7ccdc34c0aab024e69, passed pure Engine tests, report collector, full Linux ARM64 build and fixture-backed Vanilla 5875 runtime through final Runtime PASS. The same run enforced the F32 terminal-listener marker introduced in run-ci.sh. PR CI [#44680961189](https://github.com/tyrael9/core/actions/runs/44680961189) passed the pure/build path.


## F34 Core-terminal-gated action continuations

An Engine action may register one follow-up candidate through the const ActionContinuationSink in its Engine-owned DecisionContext before starting its root Spell. The queue is bounded to 32 value-only entries and keyed by parent action ID plus encounter, while retaining the parent target/spell identity and owner group/map/instance/bot scope. A matching CoreSpellTerminalEvent with Succeeded status releases the child into the normal PendingActionQueue; Failed, Interrupted, wrong target/spell, foreign encounter, expired TTL or queue teardown must never release it. A small bounded recent-terminal ledger also handles instant spells that finish before a continuation registration call returns.

Released work remains subject to normal PendingActionQueue dequeue revalidation, threat budget, channel arbitration and owner-context checks. It is not executed inside Spell::finish. The simulator's generic heal adapter makes continuation opt-in, preserving unrelated existing heal fixtures. All timestamps in the F34 Core fixture use WorldTimer's monotonic clock so registration TTL and Core terminal timestamps share one time domain.

Pure tests cover the successful path, failure/interruption, identity/context mismatch, expiry, capacity, duplicate parent, late terminal registration and stale post-release context. Trusted Push CI [#44689339405](https://github.com/tyrael9/core/actions/runs/44689339405), head e3be7ef90743b06953ce5caba23ea59de6280c30, passed pure tests, ARM64 build and full Vanilla 5875 runtime. The runtime line logged continuationResult=0 coreTerminal=1 pendingBeforeDispatch=1 dispatchSelected=1 dispatchExecuted=1 dispatchRejected=0 dispatchStale=0 continuationExecuted=1 continuationEnqueued=1 continuationDispatched=1; F34 and F32 markers and final Runtime PASS were present. PR CI [#44689346106](https://github.com/tyrael9/core/actions/runs/44689346106) passed the pure/build path.

## F35–F38 typed dependency and queue-pressure acceptance (complete)

Pure C++ regressions cover AND prerequisites, mixed success/failure and event sources, alternative atomicity and priority/FIFO selection, rejected branches, late registration, out-of-order prerequisites, latest-terminal TTL origin, uint32 clock wrap, failed-parent rejection, expiry, capacity, duplicate/tombstone behavior, stale context, teardown and ready-backlog preservation under a full PendingActionQueue. Trusted fixture-backed runs [#44702889745](https://github.com/tyrael9/core/actions/runs/44702889745), [#44704096882](https://github.com/tyrael9/core/actions/runs/44704096882), [#44713147805](https://github.com/tyrael9/core/actions/runs/44713147805), [#44715114823](https://github.com/tyrael9/core/actions/runs/44715114823) and [#44716873212](https://github.com/tyrael9/core/actions/runs/44716873212) passed the required runtime gates. The last run passed pure tests, report collector, ARM64 server/realm build and the complete Vanilla 5875 simulator through final Runtime PASS.

## F39 movement terminal semantics — implemented and runtime-verified

The Engine-only `MotionMaster::MovePointForEngine` path preserves legacy `MovePoint` and `MovementInform` behavior. The correlated PointMovementGenerator records natural spline completion and emits `Reached` only when the final Player position is within the configured 3D tolerance; forced cleanup emits `Interrupted`, and natural completion outside tolerance emits `Failed`. The pure suite verifies late registration, successful dispatch, Interrupted rejection and Failed/destination-mismatch rejection. Trusted Push CI [#44795113292](https://github.com/tyrael9/core/actions/runs/44795113292) proves a real Player reached the destination, released and dispatched its follow-up, and emitted Interrupted when `ClearType` forced cleanup. PR CI [#44795123687](https://github.com/tyrael9/core/actions/runs/44795123687) passed pure tests and ARM64 build.

## F40 item-use lifecycle audit — source behavior and initial Core baseline

`HandleUseItemOpcode` performs pre-cast validation and may bind an item before `CastItemUseSpell`. `Player::CastItemUseSpell` fans one request out across all valid ON_USE prototype slots; later children are triggered. `Spell::finish` / `Spell::cancel` are per-child outcomes, not an item-use root result. `TakeCastItem` and `TakeReagents` mutate charges/counts independently and at different points for immediate and delayed casts.

The first Core fixture (item 118 / Spell 439) proved a real potion effect and item consumption, plus a fresh zero-charge rejection that retained the item. That fixture was single-slot evidence only.

## F42 item-use aggregate — pure tests and multi-slot Core fixture

The implemented bounded value-only tracker separates:
- The root request (`itemUseId`) and ON_USE fan-out-close boundary.
- Each registered child Spell (child ID, prototype slot, Spell ID, triggered bit and authoritative finish/cancel terminal).
- Independent item mutation facts from `TakeCastItem` / `TakeReagents`.

Each child is registered before Core `Spell::prepare`, so synchronous terminals are safely correlated. The root publishes only after dispatch is closed and all registered children have settled; delayed children remain pending until their terminal or bounded expiry. Invalid SpellEntry slots are marked skipped rather than miscounted as failed children. Mutation events do not imply spell success, and spell success does not imply mutation. Inventory mutation also invalidates cached Engine decisions.

The Engine publishes a value-only `ItemUseTerminal` event and supports a typed continuation prerequisite matched by root ID/status/history. Continuations still enter the normal bounded pending queue and undergo fresh next-dispatch eligibility/context checks.

Pure C++14 coverage in `tests/PartyBotEngineTests.cpp` includes:
- Synchronous multi-child fan-out where the first child terminal occurs before dispatch close.
- Delayed child settlement and mixed/all failure/interruption aggregate outcomes.
- Separate mutation evidence, including interruption after a mutation.
- Capacity, skipped slots, duplicate reports, bounded TTL, late-child expiry and uint32 clock wrap.
- ItemUseTerminal correlation, recent-terminal history, queued-success handling and ordinary pending continuation dispatch.

The Core simulator fixture retains the real single-slot success/zero-charge rejection cases and adds a disposable two-ON_USE-slot prototype. It checks two actual Core child Spells, first child non-triggered and second triggered, the root aggregate, both independently observed finite-charge mutations and cleanup/restoration of the modified prototype. Trusted push [#44839748022](https://github.com/tyrael9/core/actions/runs/44839748022) passed this multi-slot stage.

The follow-up real delayed ON_USE fixture uses Vanilla item 1402 (Brimstone) and spell 143 (Fireball, 2,000 ms cast time). It runs two isolated Player/item cases through the real Core item-use/Spell lifecycle. In the first, the cast is prepared and interrupted before `TakeCastItem`; the item remains at `-5` charges. In the second, Core performs the delayed-cast charge mutation first (`-5` to `-4`), the fixture then cancels the pending Spell before impact, and verifies the mutation event identity/phase, Interrupted child terminal, unchanged target health and no pending root. Both Players and the target are cleaned up.

Trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749) passed the complete ARM64 build and fixture-backed run. Its log reported `preInterrupted=1 preUnmutated=1 preCharge=-5>-5 postInterrupted=1 postMutation=1 mutationBeforeEffects=1 mutationIdentity=1 mutationCount=1 postCharge=-5>-4 postNoImpact=1 postTerminals=1 cleanup=1/1/1`, followed by the F42 PASS marker and final Runtime PASS. Artifact: [partybot-simulator-dfdfdf0](https://github.com/tyrael9/core/actions/runs/44852141749/artifacts/14685618923).

CI evidence:
- PR CI [#44839755150](https://github.com/tyrael9/core/actions/runs/44839755150) passed pure Engine tests, report collector, configure, full Linux ARM64 Core build and runtime-harness syntax. The fixture runtime was skipped on PR by design.
- Trusted push [#44839748022](https://github.com/tyrael9/core/actions/runs/44839748022) is the corrected-gate closure run: all pure tests, report collector, configure, ARM64 build, fixture-backed simulator and final Runtime PASS succeeded. F42's required delayed-use/cancellation evidence is now fully CI-closed by trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749); the two real-Core cancellation cases verified both sides of charge mutation.

Remaining optional item-use coverage: a separate count-based stackable consumable and item-as-reagent mutation fixture have not been added. The real-Core delayed charge-based ON_USE cancellation-before/after-mutation scenarios required for F42 have passed.

## Production action readiness — still open

Seventeen reusable generic adapters are implemented and have action-specific Core preflight, trusted-runtime fixture coverage and matching PR ARM64 build coverage. ChaseTargetAction's thirteenth-adapter matrix covers live-victim MoveChase acceptance, same-target suppression, stale Follow rejection, valid Follow supersession, Core's Chase-expiry cleanup of the prior Follow, already-melee-reachable no-op, live UNIT_STATE_CAN_NOT_MOVE rejection after candidate selection, stale-victim rejection and unrelated Point preservation; trusted push [#44990342234](https://github.com/tyrael9/core/actions/runs/44990342234) reached final Runtime PASS. StopStaleChaseAction (fourteenth) and StopChaseAction (fifteenth) each have trusted accepted/rejected/cleanup matrices; StopChase trusted push [#44994895905](https://github.com/tyrael9/core/actions/runs/44994895905) reached final Runtime PASS. MoveDistanceAction (sixteenth) wraps Core MotionMaster::MoveDistance; trusted push #44998862867 (https://github.com/tyrael9/core/actions/runs/44998862867) verifies live Follow and Chase preservation/restoration, state/threat invariants, unrelated Point rejection, victim-drift rejection and cleanup, then final Runtime PASS; matching PR #44998866883 (https://github.com/tyrael9/core/actions/runs/44998866883) passed ARM64 build. CancelOwnCastAction is the seventeenth; its PREPARING/replacement-identity fixture is awaiting CI and delayed/channelled state matrices remain open. All seventeen remain intentionally unregistered in NewEngine; Legacy remains the default and NewEngine has no Legacy fallback.

StopChaseAction is the fifteenth F43 adapter. It cancels only a current native Chase whose live target still equals the Player's current victim and snapshot GUID. Core MovementExpired(false) also expires contiguous underlying Chase/Follow generators, so unrelated movement below the Chase is rejected before mutation and Follow must be reissued by policy. Postconditions preserve victim, selection, target, combat/melee flags, current Spell slots, health/power, attacker reference and ThreatManager state. Trusted push [#44994895905](https://github.com/tyrael9/core/actions/runs/44994895905) verifies accepted stop, Follow cleanup, stale-snapshot rejection, unrelated Point preservation, victim drift between evaluation and dispatch and cleanup, then final Runtime PASS.

MoveDistanceAction is the sixteenth F43 generic adapter. It wraps Core MotionMaster::MoveDistance, delegates destination/LOS/PathFinder reachability to Core, and requires a live current victim plus matching current Chase or party Follow. Native DISTANCING_MOTION_TYPE temporarily overlays the existing targeted generator; Core retains and restores the same generator on expiry. Trusted push #44998862867 (https://github.com/tyrael9/core/actions/runs/44998862867) logged followStarted=1, followRosterCaptured=1, followSelected=1, followDistanceStarted=1, followPreserved=1, followRestored=1, chaseSelected=1, chaseStarted=1, chasePreserved=1, followStillPresent=1, chaseRestored=1, pointRejected=1, victimRaceRejected=1 and cleanup=1, plus the expected accepted Core reason and final Runtime PASS. Matching PR ARM64 build #44998866883 (https://github.com/tyrael9/core/actions/runs/44998866883) passed. Artifact: partybot-simulator-2beecf67 (https://github.com/tyrael9/core/actions/runs/44998862867/artifacts/14744887692).
