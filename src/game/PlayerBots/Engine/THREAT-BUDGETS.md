# Threat budgets and aggro prevention

## Status and goal

This document is the design/audit boundary for the Phase 7 threat-budget work. Stages F11 and F12 have completed the snapshot prerequisites: threat entries carry Core-derived `meleeReachable`, and hostile discovery includes all member-side `HostileRefManager` links, including offline references. ARM64 runtime CI validates both reachability states and the complete hostile-reference discovery path.

Stage P7.1 now provides a pure, snapshot-only threshold/headroom assessment and standalone boundary/unknown-state tests. It is not yet connected to action evaluation and does not estimate any future action's threat impact; no production aggro-prevention multiplier is implemented. The next code change must not turn the observed 110%/130% rules into a second, incomplete copy of Core target selection.

The goal is to let PartyBotEngine assess how much additional threat a member can safely generate against each hostile, coordinate simultaneous actions, and avoid avoidable over-aggro while leaving final victim selection to vMaNGOS.


## Stage P7.1 — snapshot-only threshold assessment

`ThreatBudget.h` adds `AssessThreatBudget(snapshot, hostileGuid, memberGuid)`. It resolves unique hostile/member/threat entries from immutable GUID/scalar data and returns one of `Unknown`, `CurrentVictim`, `BudgetAvailable` or `AtOrOverBudget`, with an explicit unknown reason. It exposes the current victim's threat, the member's threat, the threshold multiplier/value and raw headroom.

The helper applies 1.1 only when the candidate member's copied Core `meleeReachable` bit is true, otherwise 1.3. It uses the current victim's threat as the baseline and treats equality as zero headroom, matching Core's strict greater-than comparisons. An online but inaccessible ranged reference remains assessable; `accessible` is not substituted for `meleeReachable`.

Expired snapshots (using the same wrap-safe millisecond-age calculation as `PartyCoordinator`), missing/duplicate hostile, member or threat entries; missing current victim; offline references; dead/offline members; dead hostiles; and non-finite/negative threat values return `Unknown` rather than fabricating a safe budget. The current victim is reported separately and is not treated as a DPS/healer over-aggro candidate.

This is deliberately a limited numeric projection, not a second target selector. It does not model Core's threat-area checks, eligibility passes, taunt modifiers, immunities, secondary-threat state, immobilization/chase conditions or future threat. It must not be used as an action multiplier until the action-specific threat-impact contract and execution-time revalidation are implemented.

Standalone tests cover melee/ranged thresholds, exact equality and just-below/above values, inaccessible ranged entries, current-victim state, incomplete/invalid snapshot inputs, stale snapshots and uint32 clock wraparound.

## Core remains the authority

Pinned source:
- `src/game/Threat/ThreatManager.cpp`: `ThreatManager::getHostileTarget`, `ThreatContainer::selectNextVictim`, threat sorting and taunt behavior.
- `src/game/Threat/ThreatManager.h`: `HostileReference` threat/status accessors.
- `src/game/Objects/Unit.cpp`: `Unit::CanReachWithMeleeAutoAttack` and its reach calculation.

The Core selector's relevant behavior is:

1. The online threat container is sorted by descending threat.
2. Selection validates candidates with Core's threat-area and attack-target checks. A target outside the attacker's threat area aborts selection; invalid targets are skipped.
3. The first pass skips low-priority candidates such as targets immune to the attacker's melee damage school, secondary threat targets, and certain chase-distance/melee-unreachable cases while the attacker is immobilized. A fallback pass allows low-priority candidates.
4. With a valid current victim, the selector retains it when the highest eligible challenger is the current victim or its threat is at most 110% of current-victim threat.
5. Otherwise, a challenger switches aggro when its threat is strictly greater than 130% of current-victim threat, or strictly greater than 110% and Core says that challenger is within the hostile's melee auto-attack reach.
6. These are comparisons against the **current victim's threat**, not the Main Tank's threat by role label. The 110%/130% decision uses the candidate challenger's melee reachability.

The simulator and Engine must not call `setCurrentVictim`, directly set a Creature victim, mutate ThreatManager references, or reproduce `selectNextVictim` as a competing policy. Core resolves the actual victim after real damage, taunt, death and other threat events.

## Snapshot facts and their limits

The first version of the builder found hostiles through party members' current victims and current attackers only. That is insufficient for assist/healing threat: Core's `HostileRefManager::threatAssist` iterates **every** hostile reference linked to the healed member, including mobs that are currently attacking another unit. Phase 7 Stage F12 extends hostile discovery through each member's `GetHostileRefManager().getFirst()` chain. It resolves the reference source through `HostileReference::getSource()->getOwner()` and copies a missing per-member entry directly from the linked reference, preserving `isOnline()` and `isAccessable()` even when the reference is no longer in the hostile's active online threat list. Runtime CI `#44215877767` passed with an actual hostile whose victim was outside the Group, no Group member still being attacked by it, and an offline Tank reference copied into the snapshot; Core status and victim state were restored before the subsequent F10 death/recovery checks. This closes the hostile-reference discovery prerequisite for the threat-budget work; it does not yet estimate action threat impact.

The current value snapshot has:
- `HostileSnapshot::victimGuid`: the hostile's actual current victim GUID;
- per `ThreatEntrySnapshot`: target GUID, Core threat value, online state, accessible state and Core-derived `meleeReachable`;
- per party member: online/alive state, role/Main Tank marker and current victim GUID.

Important distinctions:
- `meleeReachable=false` means the Core melee-reach predicate returned false at snapshot time. It does not by itself prove the target is invalid or that a ranged attack is possible.
- `accessible` and `meleeReachable` are not interchangeable. Core explicitly documents online-but-inaccessible cases, including targets the creature cannot approach but may still attack at range.
- The snapshot does not currently encode all conditions in `selectNextVictim`, including threat-area result, attack-target validity, damage-school immunity, secondary-threat state, the attacker's immobilization/chase capability, or the exact selection-pass state.
- Threat entries can become stale immediately after a snapshot. Execution must re-resolve GUIDs and revalidate; a snapshot-derived budget is a policy estimate, not a guarantee about the next Core victim.
- A current-victim GUID without a matching valid threat entry, an absent/offline reference, or unknown target eligibility must produce an explicit unknown/unavailable assessment rather than an invented safe budget.

## Budget math: a limited risk projection, not a selector

For one hostile and one potential threat-generating member, let:
- `V` be the current victim's current Core threat;
- `T` be that member's current Core threat;
- `R` be the member's Core `meleeReachable` bit against this hostile;
- `L` be `1.1 * V` when `R=true`, otherwise `1.3 * V`.

The basic observed headroom is `max(0, L - T)`. Core uses strict greater-than comparisons, so equality with `L` does not itself cross that numeric comparison. However, a zero headroom means any positive additional threat could cross it. The policy may apply an explicit conservative safety reserve below the raw Core threshold; that reserve is an Engine policy parameter, not a change to Core semantics.

This scalar is only a per-hostile risk bound. It does not decide which candidate will actually be selected by Core. In particular, the challenger must be the highest eligible reference for the selector to reach its threshold comparison, and Core may ignore or promote targets according to validity/low-priority rules. A later PartyBotEngine implementation should prefer a conservative assessment and defer actual target state to Core.

Recommended assessment states:
- `Unknown`: missing/stale snapshot facts, absent current victim reference, unusable target state, or incomplete impact information;
- `NotThreateningThisHostile`: no known threat impact from the proposed action to this hostile;
- `CurrentVictim`: the member is already the hostile's current victim; this is not a DPS/healer over-aggro warning and should instead feed tank-hold/recovery policy;
- `BudgetAvailable`: current references permit a conservative headroom calculation;
- `AtOrOverBudget`: no positive headroom remains or a known projected impact would exceed the configured safe limit.

The exact enum and names remain an implementation decision; these states define the required semantics.

## Threat is action-specific

A generic priority value is not a threat estimate. Do not infer threat from candidate priority, spell rank, raw heal amount, displayed damage, or distance.

The eventual action-to-threat contract must distinguish at least:

- **Damage to a hostile:** the threat recipient is the hostile target; the threat owner is the acting Player/Pet. The Core damage/spell path and threat modifiers determine actual threat.
- **Healing/assist threat:** Core's `HostileRefManager::threatAssist` distributes assist threat across the healed unit's hostile references. The affected set is therefore not just the mob currently targeting the patient; each hostile reference must be assessed. Do not assume every heal automatically creates threat or that threat equals the raw heal amount. The action must identify the exact Core event/path that will emit the threat.
- **Taunt:** Core `tauntApply` may temporarily raise the taunter's threat reference to the current victim's level, then `tauntFadeOut` removes the temporary modifier. This is not ordinary additive threat and must be represented separately.
- **Threat reduction/reset:** feign death, vanish-like mechanics, threat-percent modification and other effects can remove or reduce references; use their actual Core result rather than modeling them as negative damage.
- **Pets/guardians and indirect actors:** threat ownership and threat recipient must be explicit; do not assume the bot's Player GUID is the reference GUID.

Core `ThreatCalcHelper::CalcThreat` applies spell threat modifiers, critical-threat modifiers and the hated unit's total threat modifier before insertion. Any action estimate that starts from raw damage or another raw amount must use the audited Core calculation path and account for its timing/inputs. If a reliable estimate is unavailable, the action's threat impact is unknown; it must not silently be treated as zero.

## Core action threat paths — Stage P7.2 audit

The pinned Core source has several independent threat-producing paths. An action estimate must name which path it will invoke; one generic damage/heal formula is not sufficient.

| Path | Core source and observed behavior | Consequence for an Engine estimate |
|---|---|---|
| Direct damage | `Unit::DealDamage` computes the damage actually applied, multiplies it by `SpellMgr::GetSpellThreatMultiplier(spellProto)`, then calls the damaged hostile's `AddThreat(actor, threat, crit, damageSchoolMask, spellProto)`. | The input is applied damage, not tooltip/base damage. Hit outcome, armor/resist, crit, school and spell threat multiplier matter. |
| Threat calculation | `Unit::AddThreat` rejects spells with `SPELL_ATTR_EX4_NO_HARMFUL_THREAT`. `ThreatManager::addThreat` validates life/map state; `ThreatCalcHelper::CalcThreat` applies the hated Unit's spell threat modifier, critical-threat aura modifier and total threat modifier. `SPELL_ATTR_EX_NO_THREAT` can prevent creation of a new reference while still allowing updates to an existing reference. | A raw damage estimate is not yet final threat. The relevant modifiers belong to the hostile/actor Core path and may vary by hostile. |
| Direct healing assist | `Spell::DoAllEffectOnTarget` uses the actual `DealHeal` return value (`gain`), multiplies by 0.25 for a Paladin caster or 0.5 for other classes, then by `GetSpellThreatMultiplier`, and calls the healed target's `HostileRefManager::threatAssist`. | Effective health gain and caster class matter. A raw spell heal amount overestimates overheal and is not the emitted threat amount. |
| Assist fan-out | `HostileRefManager::threatAssist` returns for `SPELL_ATTR_EX4_NO_HELPFUL_THREAT`; otherwise it divides the supplied threat by `getSize()` unless `pSingleTarget` is true, then visits every linked hostile reference and calls that source ThreatManager's `addThreat(..., isAssistThreat=true)`. The normal healing call uses the default non-single-target mode. | The same heal can affect multiple hostiles. The divisor is the full linked reference count, not just the currently attacking mobs or online threat-list entries. Stage F12 snapshot discovery is required to enumerate this set. |
| Assist-threat suppression | `ThreatManager::addThreat` zeroes assist threat when the hostile is confused, fleeing or isolated, or stunned with a breakable stun aura; it then runs the same Core threat calculation. | A pre-action estimate must include hostile CC state and modifiers or report a bound/unknown result. A zero contribution from one hostile does not mean the action has no threat impact on other references. |
| Periodic damage | `SpellAuras.cpp` periodic damage ticks call `Unit::DealDamage(..., DOT, ...)`; the normal non-player damage path then applies the spell threat multiplier and calls `AddThreat`. | Each tick is a separate impact based on damage actually applied after bonuses, absorb/resist and health clamping; it is not covered by a one-time direct-damage estimate. |
| Periodic healing / health funnel | `SPELL_AURA_PERIODIC_HEAL` and `SPELL_AURA_OBS_MOD_HEALTH` call `DealHeal`, then send 50% of actual health gain times the spell threat multiplier through the healed target's `threatAssist`. Health funnel/leech paths can also damage a hostile through `DealDamage` and generate self-heal assist threat on the caster's hostile references. | HoT ticks are separate future impacts. Health funnel can produce both damage threat and a multi-hostile healing-assist fan-out; model both or return Unknown. |
| Periodic power assist | `SPELL_AURA_PERIODIC_ENERGIZE` sends 50% of actual power gain through `threatAssist` except Mana and Happiness on supported post-1.8.4 builds. The separate `SPELL_AURA_OBS_MOD_MANA` path also calls `threatAssist` using actual gain. | Do not infer power threat from the heal formula or assume all mana regeneration is threat-free. Distinguish the exact aura path and power type; future ticks need bounded, expiring reservations. |
| Additional spell-table threat | `Spell::DoAllEffectOnTarget` separately reads `SpellThreatEntry`. For clearly positive spells it calls assist threat on each eligible hit target; for clearly negative spells it adds threat to each eligible hostile target. Mixed positive/negative effect masks are deliberately ignored. | Flat spell-table threat is additional to damage/healing threat and can fan out across multiple targets. Spell rank alone is not a complete impact description. |
| Taunt / threat modification | `Unit::TauntApply` and `ThreatManager::tauntApply/tauntFadeOut` use a temporary threat modifier on a reference; threat-percent changes and threat resets use other Core APIs. `HostileReference::getThreat()` reports the currently modified value. | These are state transitions, not additive damage/heal estimates. The snapshot headroom helper observes their current result but must not predict them as ordinary positive/negative threat. |

### Action-impact contract requirements

The initial interface is now `ThreatImpact.h`, exposed through `Action::EstimateThreatImpact`. The default implementation returns `Unknown` with the current snapshot epoch, so an action cannot silently become a zero-threat action by omitting an estimator. `ThreatImpactEstimate` distinguishes `Complete`, `NoThreat` and `Unknown`; `Unknown` may retain partial known rows, while `NoThreat` must have no rows and `Complete` must enumerate at least one. Source categories distinguish direct/periodic damage, direct/periodic healing assist, periodic power assist and flat spell-table threat. Taunt and threat reduction remain non-additive transitions and must stay Unknown until separately modeled.

The eventual action estimator must return a bounded list of per-hostile impacts, each carrying:
- acting threat owner GUID (Player, Pet or other actual Core actor);
- recipient hostile GUID;
- impact amount in **threat points**, not damage/healing units;
- impact kind/source path (direct damage, direct heal assist, periodic tick, flat spell threat, taunt, threat reduction, etc.);
- confidence: `Exact`, `UpperBound`, `NoThreat` or `Unknown`;
- the snapshot epoch / execution window to which the estimate applies.

Assist healing expands into one impact per hostile reference of the healed unit. If the fan-out, effective heal, hostile suppression state, Core modifiers or additional spell-table contribution cannot be bounded, the estimate is `Unknown`; it must not disappear from the list or be treated as zero.

The current `Reservation::amount` is an integer used by existing heal/CC-style reservations. Threat reservations need floating-point threat units and a key per (hostile GUID, threat-owner GUID); do not overload the existing integer field without auditing every existing reservation caller. Reservation expiry must not outlive the action/tick window, and all reservations must be released on owner logout, action failure, cancellation and completion.

## Stage P7.3 — action-specific impact estimators and policy

The first Core-backed damage component is `CoreThreatImpactEstimator.h::EstimateAppliedDirectDamageComponent`. Given damage already applied by Core (or a caller-declared upper bound), it applies the same `SpellMgr::GetSpellThreatMultiplier(nullptr)` and `ThreatCalcHelper::CalcThreat` path used by `Unit::DealDamage` and `ThreatManager::addThreat`. It emits a GUID/scalar row for the actual threat owner and hostile, without changing Core state.

The direct-heal component is `EstimateDirectHealingAssistComponent`. It follows the Vanilla 5875 `Spell::DoAllEffectOnTarget` path: effective heal × class coefficient (Paladin 0.25, otherwise 0.5) × `SpellMgr::GetSpellThreatMultiplier(spellInfo)`, distributed over the healed target's live `HostileRefManager` references; each per-hostile amount then passes through `ThreatCalcHelper::CalcThreat`. The helper verifies every reference resolves to exactly one hostile row in the matching immutable PartySnapshot, rejects missing/duplicate/unresolved references and respects `SPELL_ATTR_EX4_NO_HELPFUL_THREAT`. The maximum effective-heal input can be bounded by the target's current missing health. Core's assist-threat hard-CC suppression is not subtracted, which keeps this component conservative for that case.

This is deliberately a component, not a complete Action estimate: the returned envelope remains `Unknown` even when the row is valid, because procs, pet-owner reference side effects and any additional action threat paths have not been proven absent. It only handles spell-less direct damage; spell damage, periodic ticks, healing, power regeneration, flat spell threat, taunt and threat reduction remain unimplemented. The simulator compares the component against the real ThreatManager increment from a 100-damage Core event and the CI runner requires `impactMatches=1`.
## Implementation sequence

1. **Snapshot assessment primitive — complete (Stage P7.1):** `AssessThreatBudget` locates unique current-victim/member references, reports unknown states explicitly, checks wrap-safe snapshot freshness and calculates raw headroom without touching live Core objects. Pure tests cover melee/ranged thresholds, strict equality, just-below/above values, current victim, missing/offline/dead/invalid inputs and uint32 clock wrap.
2. **Action threat-impact contract — initial shape complete:** `Action::EstimateThreatImpact` returns a GUID/scalar-only envelope with explicit completeness, source category, exact/upper-bound classification and snapshot epoch. The default is Unknown. P7.3 must now implement real per-action estimators using the audited Core source paths; never estimate from priority or raw tooltip values.
3. **Party coordination:** add a separate bounded, expiring floating-point threat-reservation type keyed by (hostile GUID, threat-owner GUID). Do not reuse the existing integer `Reservation::amount`. Reservations must be tied to action/tick windows and released on completion, cancellation, failure and owner logout.
4. **Policy integration:** compose a ThreatBudget multiplier or hard safety veto only after the action impact contract exists. Unknown impact must be handled explicitly; it must not be mapped to an arbitrary zero-cost action. The policy adjusts candidate eligibility/priority, but Core still performs target selection.
5. **Core-backed runtime validation:** extend the simulator with real Players, at least two hostiles, both melee/ranged candidate distances, actual Core-generated threat, and an Engine decision near the threshold. Verify the decision's input against direct Core threat values and verify Core's actual victim afterward. Include a control case proving the Engine does not mutate Core victim/threat state.
6. **Live realm validation:** only after the above, request a separate operator test on the disposable realm. CI success alone never authorizes publishing or deployment.

## Required invariants

- No duplicate implementation of Core's `selectNextVictim`.
- No manual victim changes or direct threat-list manipulation by PartyBotEngine.
- Main Tank role is not a substitute for actual current-victim GUID/threat.
- Every threat impact identifies who receives it, which hostile references are affected, its units and whether it is exact/bounded/unknown.
- No map/instance-crossing Core pointers; resolve GUIDs on the owning map at execution time.
- No unbounded reservations, threat history or per-tick scans.
- Revalidate before executing; post-action snapshots/Core state remain authoritative.
- No hidden fallback to Legacy threat/taunt policy in New Engine mode.


The current value snapshot has:
- `HostileSnapshot::victimGuid`: the hostile's actual current victim GUID;
- per `ThreatEntrySnapshot`: target GUID, Core threat value, online state, accessible state and Core-derived `meleeReachable`;
- per party member: online/alive state, role/Main Tank marker and current victim GUID.

Important distinctions:
- `meleeReachable=false` means the Core melee-reach predicate returned false at snapshot time. It does not by itself prove the target is invalid or that a ranged attack is possible.
- `accessible` and `meleeReachable` are not interchangeable. Core explicitly documents online-but-inaccessible cases, including targets the creature cannot approach but may still attack at range.
- The snapshot does not currently encode all conditions in `selectNextVictim`, including threat-area result, attack-target validity, damage-school immunity, secondary-threat state, the attacker's immobilization/chase capability, or the exact selection-pass state.
- Threat entries can become stale immediately after a snapshot. Execution must re-resolve GUIDs and revalidate; a snapshot-derived budget is a policy estimate, not a guarantee about the next Core victim.
- A current-victim GUID without a matching valid threat entry, an absent/offline reference, or unknown target eligibility must produce an explicit unknown/unavailable assessment rather than an invented safe budget.

## Budget math: a limited risk projection, not a selector

For one hostile and one potential threat-generating member, let:
- `V` be the current victim's current Core threat;
- `T` be that member's current Core threat;
- `R` be the member's Core `meleeReachable` bit against this hostile;
- `L` be `1.1 * V` when `R=true`, otherwise `1.3 * V`.

The basic observed headroom is `max(0, L - T)`. Core uses strict greater-than comparisons, so equality with `L` does not itself cross that numeric comparison. However, a zero headroom means any positive additional threat could cross it. The policy may apply an explicit conservative safety reserve below the raw Core threshold; that reserve is an Engine policy parameter, not a change to Core semantics.

This scalar is only a per-hostile risk bound. It does not decide which candidate will actually be selected by Core. In particular, the challenger must be the highest eligible reference for the selector to reach its threshold comparison, and Core may ignore or promote targets according to validity/low-priority rules. A later PartyBotEngine implementation should prefer a conservative assessment and defer actual target state to Core.

Recommended assessment states:
- `Unknown`: missing/stale snapshot facts, absent current victim reference, unusable target state, or incomplete impact information;
- `NotThreateningThisHostile`: no known threat impact from the proposed action to this hostile;
- `CurrentVictim`: the member is already the hostile's current victim; this is not a DPS/healer over-aggro warning and should instead feed tank-hold/recovery policy;
- `BudgetAvailable`: current references permit a conservative headroom calculation;
- `AtOrOverBudget`: no positive headroom remains or a known projected impact would exceed the configured safe limit.

The exact enum and names remain an implementation decision; these states define the required semantics.

## Threat is action-specific

A generic priority value is not a threat estimate. Do not infer threat from candidate priority, spell rank, raw heal amount, displayed damage, or distance.

The eventual action-to-threat contract must distinguish at least:

- **Damage to a hostile:** the threat recipient is the hostile target; the threat owner is the acting Player/Pet. The Core damage/spell path and threat modifiers determine actual threat.
- **Healing/assist threat:** Core's `HostileRefManager::threatAssist` distributes assist threat across the healed unit's hostile references. The affected set is therefore not just the mob currently targeting the patient; each hostile reference must be assessed. Do not assume every heal automatically creates threat or that threat equals the raw heal amount. The action must identify the exact Core event/path that will emit the threat.
- **Taunt:** Core `tauntApply` may temporarily raise the taunter's threat reference to the current victim's level, then `tauntFadeOut` removes the temporary modifier. This is not ordinary additive threat and must be represented separately.
- **Threat reduction/reset:** feign death, vanish-like mechanics, threat-percent modification and other effects can remove or reduce references; use their actual Core result rather than modeling them as negative damage.
- **Pets/guardians and indirect actors:** threat ownership and threat recipient must be explicit; do not assume the bot's Player GUID is the reference GUID.

Core `ThreatCalcHelper::CalcThreat` applies spell threat modifiers, critical-threat modifiers and the hated unit's total threat modifier before insertion. Any action estimate that starts from raw damage or another raw amount must use the audited Core calculation path and account for its timing/inputs. If a reliable estimate is unavailable, the action's threat impact is unknown; it must not silently be treated as zero.

## Implementation sequence

1. **Snapshot assessment primitive:** add a pure value helper that locates the current victim and member references, reports unknown states explicitly, and calculates the unreserved raw/safe headroom without touching live Core objects. Unit tests must cover melee and ranged thresholds, strict equality, just-over-threshold values, current victim, missing references, offline/dead members and stale/invalid inputs.
2. **Action threat-impact contract:** define how each Action describes affected hostile/member pairs and whether the estimate is exact, bounded or unknown. Keep the interface independent of live Core pointers; Core adapters may resolve GUIDs on the map thread to obtain audited estimates.
3. **Party coordination:** add bounded, expiring reservations for known in-flight threat impacts where simultaneous bots could otherwise all consume the same headroom. Reservation amount/units, target key, action completion/release and owner cleanup must be explicit.
4. **Policy integration:** compose a ThreatBudget multiplier or hard safety veto only after the action impact contract exists. Unknown impact must be handled explicitly; it must not be mapped to an arbitrary zero-cost action. The policy adjusts candidate eligibility/priority, but Core still performs target selection.
5. **Core-backed runtime validation:** extend the simulator with real Players, at least two hostiles, both melee/ranged candidate distances, actual Core-generated threat, and an Engine decision near the threshold. Verify the decision's input against direct Core threat values and verify Core's actual victim afterward. Include a control case proving the Engine does not mutate Core victim/threat state.
6. **Live realm validation:** only after the above, request a separate operator test on the disposable realm. CI success alone never authorizes publishing or deployment.

## Required invariants

- No duplicate implementation of Core's `selectNextVictim`.
- No manual victim changes or direct threat-list manipulation by PartyBotEngine.
- Main Tank role is not a substitute for actual current-victim GUID/threat.
- Every threat impact identifies who receives it, which hostile references are affected, its units and whether it is exact/bounded/unknown.
- No map/instance-crossing Core pointers; resolve GUIDs on the owning map at execution time.
- No unbounded reservations, threat history or per-tick scans.
- Revalidate before executing; post-action snapshots/Core state remain authoritative.
- No hidden fallback to Legacy threat/taunt policy in New Engine mode.


## Stage P7.1 validation note

CI `#44217682323` on commit `705f7f3` passed the standalone PartyBotEngine test step (including stale snapshot and uint32 wraparound cases), CMake configure, full Linux ARM64 Core/server build and simulator harness syntax validation. The optional Core-backed runtime step failed before starting `mangosd`: Google Drive returned its download quota/rate-limit message for the extracted-data fixture. This is an external fixture-hosting failure, not a simulator assertion or a runtime pass. Do not count this run as Core runtime evidence. The previous F12 runtime evidence remains CI `#44215877767`.


## Stage P7.3c — bounded floating-point threat reservations

ThreatReservation is a GUID/scalar-only record. It keeps three identities separate:

- ownerGuid: the bot that owns the pending action and is responsible for cleanup;
- threatOwnerGuid: the exact Player/Pet/guardian Unit whose hostile ThreatManager reference will receive the threat;
- hostileGuid: the hostile whose threat budget is being reserved.

A coordinator-local nonzero ThreatActionId groups all rows produced by one logical action. One action can reserve several hostile/recipient pairs (for example, an AoE or a healing-assist fan-out). Re-submitting the same (owner, action, hostile, threat-owner) row replaces the old value, so retrying an estimate does not double-count it. GetReservedThreatImpact sums pending points by the exact (hostile, threatOwner) pair and can exclude one action while evaluating that action's own reservation.

Reservations accept only finite positive threat points and a future monotonic millisecond deadline. Deadline deltas must be positive and fit within the signed half-range, matching the wrap-safe GetMSTime comparison already used by the coordinator. PruneExpired removes expired threat rows alongside the existing generic reservations. Action completion calls ReleaseThreatAction(actionId) to remove every row in that action's fan-out; bot teardown calls ReleaseOwner(ownerGuid), which also removes rows whose actual threat recipient is a pet. Both paths are deterministic and do not retain Core object pointers.

The threat-reservation collection is capped at 128 rows per group/map/instance coordinator. At capacity, a new action is rejected rather than evicting another in-flight action; refreshing an existing row remains allowed. The sum uses double accumulation and saturates at the largest finite float.

This stage adds coordination primitives only. It does not yet alter candidate eligibility/priority or execute any new combat policy; that integration belongs to P7.3d. Core ThreatManager and victim selection remain authoritative.

Standalone tests cover multiple hostiles and actual threat owners per action, idempotent refresh, concurrent action aggregation, action release, bot-owner cleanup across pet GUIDs, normal and wraparound expiry, invalid identity/value/deadline rejection, the 128-row cap and fail-closed capacity behavior.


## Stage P7.3d — action admission and execution reservations (partial)

The Engine now consumes the action estimate contract rather than treating missing estimates as zero:

- Action::EstimateThreatImpact defaults to Unknown. An action must explicitly declare NoThreat or provide a well-formed Complete estimate for the exact snapshot epoch. A positive complete estimate must also declare a bounded reservation duration (1..INT32_MAX milliseconds).
- Candidate arbitration runs in descending priority order. Each complete estimate is aggregated by (hostileGuid, threatOwnerGuid), then checked against the current Core-derived headroom, all still-live coordinator reservations and higher-priority candidates already admitted in the same decision report.
- CurrentVictim rows do not use the DPS/healer aggro-headroom veto. Missing, offline, stale, duplicate, pet-only or otherwise unresolvable references remain Unknown and fail closed. In particular, pets are not yet copied as independent party members in the current snapshot, so pet-owned threat estimates are intentionally rejected until that snapshot support is added.
- ExecuteSelected repeats the budget check immediately before the action. For a complete positive estimate it creates one action ID, aggregates duplicate rows, reserves the entire hostile/recipient fan-out before calling the executor, and rolls back every row if reservation or action execution fails. Successful reservations remain until the estimate's declared duration expires; an action with an explicit completion hook may release them earlier using the returned CandidateEvaluation::threatActionId.
- Candidate reports and Shadow diagnostics distinguish RejectedThreatUnknown from RejectedThreatBudget. The current Shadow target/heal placeholders have no actual spell/action threat model, so they now report Unknown rather than being selected as free-threat actions. Shadow still only calls Evaluate; it does not execute or mutate Core combat state.

This is deliberately component-level, not complete combat coverage: there are not yet production Actions that return Complete pre-action threat forecasts. The spell-less direct-damage helper validates the Core formula from damage already applied; it is parity evidence, not a future spell-cast prediction. Direct healing assist now has a Core-derived pre-cast upper-bound component, but flat spell-table threat and triggered side effects are not covered. Periodic healing/power, DoTs/HoTs, spell damage, taunt and threat reduction remain unimplemented. Runtime evidence: the damage component matched a real 100-damage ThreatManager delta (`impactMatches=1`) in push CI `#44238072768`; the direct-heal component passed the full 5875 Player/Group fixture in push CI `#44242344188`, with two hostile rows bounded at `85.8` each and actual per-hostile healing-threat deltas `14.5` each (`healAssistBound=1`). The helper must not be promoted to `Complete` until all spell/action threat paths are enumerated.

Pure tests cover multi-hostile fan-out aggregation, coordinator reservations, higher-priority local budget consumption, unknown/pet fail-closed behavior, current-victim handling, TTL validation, pre-execution reservation, rollback after failed execution, and projection overflow. Full-fixture generic budget/race validation passed in push CI `#44238072768`; the direct-heal-assist component's conservative fan-out upper bound passed in push CI `#44242344188`.


### Core-backed runtime probe added

The combined five-Player Group/two-add simulator now contains a P7.3d decision probe. It first creates a tiny, real healing-assist threat reference on the Healer through Core HostileRefManager::threatAssist. The production PartySnapshotBuilder then captures the Tank's actual current-victim references and the Healer's online non-victim references for both adds. The pure Core projection must classify the first add using the 110% melee threshold and the second using the 130% ranged threshold.

A test-only action adapter uses the existing CoreThreatImpactEstimator / ThreatCalcHelper path for a spell-less direct-damage input. The ranged case doubles its proposed damage until its estimated impact exceeds the actual snapshot headroom; PartyBotEngine must return RejectedThreatBudget without executing it. The melee case estimates a safe 50-damage input; the Engine must select it, create a reservation with an action ID and leave both ThreatManagers and their current victims unchanged. The harness then applies the exact input through real Player::DealDamage, compares the measured ThreatManager delta to the pre-action estimate, verifies the add still targets the Tank, and releases the action reservation as the completion callback would.

This runtime scenario passed on the full Google Drive-backed 5875 fixture in trusted push CI `#44238072768` on `d23d865`. The real-group F9 line reported the generic budget/race flags and the simulator-wide `Runtime PASS`; artifact `partybot-simulator-d23d865377fd27d92b8e451814adcd506a40ad4c` was uploaded. The follow-up direct-heal assist component also passed on the same full fixture in push CI `#44242344188` on `fa61d64`: F9 reported `healAssistBound=1`, `upper=85.8/85.8`, `delta=14.5/14.5`, then emitted the simulator-wide `Runtime PASS`; artifact `partybot-simulator-fa61d648167df9a958ad94aff19353c78cec3172` was uploaded. This verifies a conservative component bound in this scenario, not complete spell coverage.


The standalone suite verifies the execution-time race window: an action can be selected with 15 points of headroom, then another bot can reserve six points before dispatch; ExecuteSelected rechecks the coordinator and refuses the now-unsafe 21-point projection without invoking the action. The full five-Player/two-add Core fixture now repeats that race against live ThreatManager state: after Evaluate selects the safe 50-damage candidate, a competing reservation is inserted for the same hostile/actual recipient; ExecuteSelected returns failure without invoking the action or mutating Core threat/victim state, and the competing reservation is explicitly released. Push CI `#44238072768` logged `executionRaceRejected=1`. A separate capacity test fills 127 of 128 slots, then attempts a two-recipient action whose first row would consume the final slot; when the second row is rejected, the Engine releases the action ID and restores the coordinator to its original 127 rows. Both tests preserve unrelated reservations and prove that partial fan-out reservations are never left behind.


The workflow's fixture-backed runtime step runs on trusted branch pushes; pull-request jobs continue to skip secret access. A workflow_dispatch trigger is declared for future use, but GitHub does not expose manual dispatch until the workflow file is present on the repository default branch. In this Draft PR, the trusted push event is the active runtime path. The runner verifies the 5875 archive checksum and uploads bounded simulator logs without publishing or deploying an image.
