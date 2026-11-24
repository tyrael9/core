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

For positive spells, the same envelope now also includes the flat-threat branch from `Spell::HandleThreatSpells`: the helper mirrors Core's `m_negativeEffectMask` construction (including the self-damage exception), checks `SpellThreatEntry::CanCauseThreatOnMask`, divides the table value across the same hostile references, and applies `ThreatCalcHelper` with the spell school. This only covers the supplied healed target's positive helpful-threat fan-out; other targets and harmful flat-threat routing remain outside this component.

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
- CurrentVictim rows do not use the DPS/healer aggro-headroom veto. Missing, offline, stale, duplicate, guardian/totem/transient-summon or otherwise unresolvable references remain Unknown. Primary controlled Pets are now copied as PartyCompanionSnapshot values with their own GUID, owner GUID, map/instance and online/alive state. A Pet threat owner is accepted only when exactly one companion row resolves to exactly one online/alive same-map Player owner and exactly one real hostile ThreatEntry; budget uses the Pet's own threat and meleeReachable values, never the owner's GUID. Runtime CI #44255823652 verified a Core Pet with 30 threat, 80 headroom and melee reachability against a real hostile. The owner GUID is for party-membership validation and ReleaseOwner cleanup only.
- ExecuteSelected repeats the budget check immediately before the action. For a complete positive estimate it creates one action ID, aggregates duplicate rows, reserves the entire hostile/recipient fan-out before calling the executor, and rolls back every row if reservation or action execution fails. Successful reservations remain until the estimate's declared duration expires; an action with an explicit completion hook may release them earlier using the returned CandidateEvaluation::threatActionId.
- Candidate reports and Shadow diagnostics distinguish RejectedThreatUnknown from RejectedThreatBudget. The current Shadow target/heal placeholders have no actual spell/action threat model, so they now report Unknown rather than being selected as free-threat actions. Shadow still only calls Evaluate; it does not execute or mutate Core combat state.

This is deliberately component-level, not complete combat coverage: there are not yet production Actions that return Complete pre-action threat forecasts. The spell-less direct-damage helper validates the Core formula from damage already applied; it is parity evidence, not a future spell-cast prediction. Direct healing assist has a Core-derived pre-cast upper-bound component; positive flat spell-table threat for the supplied healed target is also included when the spell's entire Core effect mask is positive. Flat helpful threat to other spell targets, harmful flat-threat routing, triggered side effects, periodic healing/power, DoTs/HoTs, spell damage, taunt and threat reduction remain unimplemented or unverified. Runtime evidence: the damage component matched a real 100-damage ThreatManager delta (`impactMatches=1`) in push CI `#44238072768`; the direct-heal component passed the full 5875 Player/Group fixture in push CI `#44242344188`, with two hostile rows bounded at `85.8` each and actual per-hostile healing-threat deltas `14.5` each. Follow-up push `#44251387021` on `ada5988` passed the fixture with the exact Core positive/negative effect-mask guard; Lesser Heal has no nonzero `spell_threat` row, so the new nonzero flat-table branch still needs a fixture that exercises it. The helper must not be promoted to `Complete` until all spell/action threat paths are enumerated.

Pure tests cover multi-hostile fan-out aggregation, coordinator reservations, higher-priority local budget consumption, unknown/unresolved companion fail-closed behavior, primary Pet owner validation and actual-Pet-GUID budget projection, current-victim handling, TTL validation, pre-execution reservation, rollback after failed execution, and projection overflow. Full-fixture generic budget/race validation passed in push CI #44238072768; direct-heal-assist upper-bound validation passed in #44242344188; primary controlled Pet identity/budget/runtime cleanup passed in #44255823652.


### Periodic Aura threat audit (not yet an estimator)

For `SPELL_AURA_PERIODIC_HEAL` / `SPELL_AURA_OBS_MOD_HEALTH`, `SpellAuras.cpp` applies the aura's stored positive amount, calls `AuraScript::OnPeriodicCalculateAmount`, applies `SpellHealingBonusTaken`, and computes the actual health gain with `ModifyHealth`. It then calls `ProcDamageAndSpell` and sends `gain * 0.5 * GetSpellThreatMultiplier(spellProto)` through the healed target's `HostileRefManager::threatAssist`. `HostileRefManager::threatAssist` divides that amount by the current reference count unless `pSingleTarget` is set, then adds threat to each hostile source using the healed target as the threat owner. A full-health target still runs the proc path but returns before the healing-assist call.

The aura holder captures max duration and script pointer at construction. `Aura` exposes base amount, stack, max duration, max tick count, current tick and periodic timer. Those fields alone are not a safe future-threat forecast: `OnPeriodicCalculateAmount` can alter each tick; target-side healing-taken modifiers and missing health can change before future ticks; `ProcDamageAndSpell` can trigger other effects even when the heal gain is zero; and AuraScript also exposes `OnPeriodicTrigger` and `OnPeriodicTickEnd`. Health Funnel-style periodic healing can additionally damage the caster through a separate `DealDamage` path. Do not return Complete or NoThreat for a generic HoT based only on base points and duration.

For periodic damage, the pinned `Aura::PeriodicTick` path is equally stateful. `SPELL_AURA_PERIODIC_DAMAGE` starts from the Aura's stored `m_modifier.m_amount` (or the percent-of-current-max-health value for `SPELL_AURA_PERIODIC_DAMAGE_PERCENT`), then invokes `AuraScript::OnPeriodicCalculateAmount`. Core has additional per-tick family rules for Consecration, Curse of Agony and Starshards. It then applies the target's `SpellDamageBonusTaken` or `MeleeDamageBonusTaken`, integer dithering, immunity, absorb/resist and `DealDamageMods`, dispatches attacker/victim periodic proc flags, clamps to the target's remaining health and calls `Unit::DealDamage(..., DOT, ..., spellProto, true, ...)`. For a hostile Creature victim, `Unit::DealDamage` adds threat from the final applied damage multiplied by `GetSpellThreatMultiplier(spellProto)` and routes it through the hostile's `AddThreat` / `ThreatManager`; `Unit::AddThreat` still enforces harmful-threat suppression and `ThreatCalcHelper` modifiers. Thus the applied-tick threat formula is known only after the tick's actual final damage is known. It is not the nominal Aura amount.

This tick can also produce non-primary consequences: periodic procs can trigger spells, killing blows can invoke spell-specific follow-ups (for example Curse of Doom's Doomguard summon roll), and periodic leech/health-funnel use a separate branch that damages the target and then heals the caster with its own healing-assist threat fan-out. Percent DoTs depend on the target's max health at tick time; scripted and family-specific amount changes depend on Aura tick count and current state. A generic pre-cast DoT estimate therefore cannot safely sum stored amount × tick count.

Implementation boundary: keep generic periodic heal/DoT/periodic power Actions Unknown until the action estimator proves its AuraScript and proc-trigger side-effect set, snapshots or conservatively bounds each future tick, and enumerates every actual threat owner/hostile recipient. The existing `PeriodicHealAssist` source enum is taxonomy only; it is not evidence of a working estimator. The same audit must inspect periodic damage's `OnPeriodicCalculateAmount`, target mitigation, proc path and `DealDamage` result before estimating DoTs.

### Primary controlled Pet snapshot and budget support

PartySnapshotBuilder now records each same-map Player's primary Unit::GetPet() as a separate PartyCompanionSnapshot. The record is value-only: Pet GUID, Player owner GUID, map/instance, health, position, victim and online/alive/combat flags. The builder discovers hostiles from the Pet's victim, attackers and HostileRefManager, so the hostile snapshot contains the actual Pet ThreatEntry (including reachability) rather than projecting the Player owner's threat onto the Pet.

AssessThreatBudget first resolves an impact GUID as a Player member; if absent, it may resolve exactly one companion and exactly one online/alive owner in the same snapshot map/instance. It then finds the unique hostile ThreatEntry for the Pet GUID and calculates 110%/130% headroom using that Pet row's own meleeReachable value and threat. Missing, duplicate, offline/dead, wrong-map or ownerless companion rows remain Unknown. Engine reservations also keep threatOwnerGuid=petGuid while ownerGuid=playerGuid, so threat attribution stays faithful to Core and owner teardown releases Pet reservations.

The full five-Player Group fixture creates a real temporary Core Pet(SUMMON_PET) for the Healer, inserts 30 threat with Core ThreatManager, rebuilds the production snapshot, validates the companion/hostile reference and budget, then clears the hostile's Pet reference and detaches the Pet without Unsummon or persistence writes. Trusted push CI #44255823652 on 295368c passed with petCreated=1, petSnapshot=1, petBudget=1, petThreat=30.0, petHeadroom=80.0, petMelee=1, and final simulator Runtime PASS.

This support is intentionally limited to the primary controlled Pet returned by Unit::GetPet(). Guardians, totems, temporary summoned creatures and charmed units do not yet have companion snapshot identity and must remain fail-closed.

### Core-backed runtime probe added

The combined five-Player Group/two-add simulator now contains a P7.3d decision probe. It first creates a tiny, real healing-assist threat reference on the Healer through Core HostileRefManager::threatAssist. The production PartySnapshotBuilder then captures the Tank's actual current-victim references and the Healer's online non-victim references for both adds. The pure Core projection must classify the first add using the 110% melee threshold and the second using the 130% ranged threshold.

A test-only action adapter uses the existing CoreThreatImpactEstimator / ThreatCalcHelper path for a spell-less direct-damage input. The ranged case doubles its proposed damage until its estimated impact exceeds the actual snapshot headroom; PartyBotEngine must return RejectedThreatBudget without executing it. The melee case estimates a safe 50-damage input; the Engine must select it, create a reservation with an action ID and leave both ThreatManagers and their current victims unchanged. The harness then applies the exact input through real Player::DealDamage, compares the measured ThreatManager delta to the pre-action estimate, verifies the add still targets the Tank, and releases the action reservation as the completion callback would.

This runtime scenario passed on the full Google Drive-backed 5875 fixture in trusted push CI `#44238072768` on `d23d865`. The real-group F9 line reported the generic budget/race flags and the simulator-wide `Runtime PASS`; artifact `partybot-simulator-d23d865377fd27d92b8e451814adcd506a40ad4c` was uploaded. The follow-up direct-heal assist component also passed on the same full fixture in push CI `#44242344188` on `fa61d64`: F9 reported `healAssistBound=1`, `upper=85.8/85.8`, `delta=14.5/14.5`, then emitted the simulator-wide `Runtime PASS`; artifact `partybot-simulator-fa61d648167df9a958ad94aff19353c78cec3172` was uploaded. This verifies a conservative component bound in this scenario, not complete spell coverage.


The standalone suite verifies the execution-time race window: an action can be selected with 15 points of headroom, then another bot can reserve six points before dispatch; ExecuteSelected rechecks the coordinator and refuses the now-unsafe 21-point projection without invoking the action. The full five-Player/two-add Core fixture now repeats that race against live ThreatManager state: after Evaluate selects the safe 50-damage candidate, a competing reservation is inserted for the same hostile/actual recipient; ExecuteSelected returns failure without invoking the action or mutating Core threat/victim state, and the competing reservation is explicitly released. Push CI `#44238072768` logged `executionRaceRejected=1`. A separate capacity test fills 127 of 128 slots, then attempts a two-recipient action whose first row would consume the final slot; when the second row is rejected, the Engine releases the action ID and restores the coordinator to its original 127 rows. Both tests preserve unrelated reservations and prove that partial fan-out reservations are never left behind.


The workflow's fixture-backed runtime step runs on trusted branch pushes; pull-request jobs continue to skip secret access. A workflow_dispatch trigger is declared for future use, but GitHub does not expose manual dispatch until the workflow file is present on the repository default branch. In this Draft PR, the trusted push event is the active runtime path. The runner verifies the 5875 archive checksum and uploads bounded simulator logs without publishing or deploying an image.


### AuraScript callback and periodic side-effect audit (2026-11-18)

The pinned Core's `AuraScript` contract in `src/game/ScriptMgr.h` is broader than a periodic amount hook. It includes value/duration calculation, before/after apply, proc eligibility and execution, absorb/mana-absorb adjustment, periodic amount calculation, periodic trigger replacement/suppression, periodic dummy and tick-end callbacks, and AreaAura target filtering. Any one of these can change the effect or add/remove behavior around an aura; a forecast that only reads `EffectBasePoints`, duration and amplitude is not a complete action model.

Concrete registered spell-script examples found in the 5875 class/item/special spell modules:

- **Gnomish Death Ray periodic damage** (`spell_item.cpp`): `OnAuraValueCalculate` chooses a random 100–500 value; `OnPeriodicCalculateAmount` can zero the tick if the channel is gone and accumulates the amount; on expiry, `OnAfterApply` casts a separate `SPELL_GDR_DAMAGE_HIT` at the target's selected target. This is not a stable base-point DoT and has a second damage recipient/path.
- **Mage Ignite** (`spell_mage.cpp`): `OnProc` derives 4–20% of the triggering hit, then updates/refreshes or creates DoT 12654. Stack state, prior hit size and refresh timing determine future damage.
- **Hunter Frost Trap** (`spell_hunter.cpp`): `OnPeriodicTrigger` calls `ProcDamageAndSpell` for trap activation, allowing proc/talent effects beyond the nominal periodic trigger.
- **Shaman Mana Tide** (`spell_shaman.cpp`): `OnPeriodicTrigger` rolls a dithered custom amount, casts the trigger spell itself, and sets `spellInfo=nullptr` to suppress Core's default trigger.
- **Warlock Curse of Idiocy** (`spell_warlock.cpp`): `OnPeriodicTrigger` may suppress the default trigger based on accumulated stat-loss state.
- **Darkmoon Faire turret/Flamethrower** (`spell_special.cpp`): the periodic callback drains mana, removes the aura when insufficient, and suppresses the trigger in that branch. **Cannibalize**'s tick-end callback only changes emote state; it is a useful example of a non-threat periodic callback, not grounds to generalize all callbacks as harmless.
- **Wyvern Sting** (`spell_hunter.cpp`): aura removal can cast a follow-up spell. Apply/remove callbacks therefore also belong in the action's side-effect audit even though they are not periodic ticks.

This inventory is evidence of concrete exceptions, not an exhaustive proof over every spell, database script binding or proc chain. The estimator must resolve the actual aura script selected for the spell, account for callback state and any triggered spells/procs, and enumerate every actual threat owner and hostile recipient. Until then:
- generic DoT/HoT/periodic power actions remain `Unknown`;
- do not infer `NoThreat` from a callback that appears cosmetic in one spell;
- do not sum nominal future tick amounts as if target mitigation, health gain, current hostile-reference fan-out, proc outcomes and script-mutated amounts were fixed;
- keep the existing direct-damage/direct-heal/flat-spell/Pet support strictly component-scoped.

CI validation for this documentation state: trusted push `#44264594529` passed pure tests, Linux ARM64 configure/build, simulator harness syntax, and the optional full 5875 Core-backed runtime fixture; its log ended in `Runtime PASS` and uploaded artifact `partybot-simulator-992c25f7b38f149bf2faac3490c93384591b5c2a`. PR workflow `#44264603355` passed. The ordinary CI Build run `#44264594534` was skipped by its event/condition configuration; it is not a failure and supplies no additional validation.


### Direct spell damage: cast-time calculation and side-effect audit (2026-11-18)

The ordinary direct spell path was traced through the pinned Vanilla 5875 Core:

1. `Spell::HandleEffects` calls `CalculateDamage(effect, target)`, which delegates to `SpellCaster::CalculateSpellEffectValue`. The latter uses the spell's current/base effect points, level/base-level/max-level scaling, dice/random range, combo-point contribution and applicable spell modifiers. These values are not necessarily the static DBC base points.
2. Before the effect handler runs, `SpellScript::OnEffectExecute` may modify `spell->damage`, suppress the effect, mutate state or cast another spell. Registered examples include:
   - Ferocious Bite: adds combo-point/attack-power and remaining-energy scaling, then consumes energy.
   - Eviscerate: adds combo-point/attack-power scaling.
   - Bloodthirst: scales damage by current attack power and target creature-type modifiers.
   - Shield Slam: adds current shield block value.
   - Execute: a dummy script computes a rage-scaled custom damage spell; the damage spell consumes rage.
   - Hammer of Wrath and Judgement of Command: adjust the direct damage through target/caster modifiers; Judgement of Command can halve damage if the target is not stunned.
   - Holy Shock, Holy Nova, Warrior's Wrath and Touch of Weakness select/cast separate follow-up spells.
   - Flametongue computes a custom proc amount from spell power and weapon speed; Meteor divides damage among the actual selected targets.
3. The ordinary hit pipeline aggregates effect damage, then invokes `SpellCaster::CalculateSpellDamage`. For magical spells this applies `SpellDamageBonusDone`, `SpellDamageBonusTaken`, possible critical damage and physical armor mitigation, followed by final integer dithering. Melee/ranged spell classes use their corresponding melee damage bonus functions. The target's absorb/resist/block and `DealDamageMods` are then applied before final application.
4. `SpellScript::OnHit` can run after damage calculation but before procs/final damage. The hit path invokes `ProcDamageAndSpell` before `DealSpellDamage`; proc auras can create additional triggered spells and threat events. A direct spell forecast that only models its primary hit therefore misses a potentially state-dependent chain.
5. `SpellCaster::DealSpellDamage` enters `Unit::DealDamage`. For a non-player hostile victim, the applied damage (not pre-mitigation damage) is multiplied by the spell threat multiplier and passed to `AddThreat`. `ThreatCalcHelper::CalcThreat` then applies the hated unit's spell-mod threat, critical-threat modifier and total threat modifier.
6. Separately, `Spell::HandleSpellThreat` processes flat `spell_threat` data for successful target/effect masks. Positive entries use each successful target's `HostileRefManager::threatAssist`; negative entries add threat to the affected hostile. Both paths must be included in the same action estimate.

The script inventory above was obtained by inspecting the registered class/item spell-script modules on the pinned branch. It is deliberately described as a concrete sample, not an exhaustive proof over all script bindings, proc-triggered spells, talents, item effects or encounter scripts.

**Estimator boundary:** do not promote generic direct spell damage to `Complete` based only on `EffectBasePoints`, `CalculateSpellEffectValue` or the final damage formula. The pre-cast estimator must resolve the actual selected spell script, current random/state-dependent effect inputs, target-specific modifiers and mitigation bounds, OnHit changes, proc-trigger closure, flat spell-threat routing, and every hostile/actual threat-owner recipient. If any of those are unknown, stale or unbounded, the action remains `Unknown`. Existing applied-damage parity tests validate only the Core damage-to-threat calculation after damage is known; they do not establish a safe future-cast prediction.


### Active proc-aura closure is part of the direct spell action

`SpellCaster::ProcDamageAndSpell` dispatches through `ProcDamageAndSpell_real`. The latter scans the attacker's current aura holders with `ProcDamageAndSpellFor(false, ...)` and, for applicable non-instant processing, also scans the victim's current aura holders with `ProcDamageAndSpellFor(true, ...)`. Eligibility depends on current proc flags/outcome, spell-proc event filters, cooldown and charge state, aura apply time, and instant/delayed proc mode. With spell proc delay configured, instant-target procs are handled immediately while the remaining proc checks are queued and later re-resolve the victim GUID before dispatch. `HandleTriggers` then executes the collected proc list.

The proc handler is not a simple fixed trigger lookup. For example, the inspected Vanilla code contains state-dependent handlers for:
- Mage Persistent Shield, deriving a triggered absorb amount from the actual incoming amount;
- Warlock Pyroclasm, selecting behavior and chance based on the triggering spell/tick count;
- Priest Blessed Recovery, deriving custom heal base points from the triggering damage;
- Hunter aspects, filtering on the actual triggering amount;
- Paladin Seal of Righteousness (on older supported builds), deriving damage from weapon speed and seal amount, then additionally firing weapon combat procs;
- Paladin Judgement of Light/Wisdom, where the actual attacker becomes the triggered heal/mana caster.

These are examples, not an exhaustive proc catalog. The proc graph depends on both actors' active aura state and can produce more than one triggered action, so the primary spell ID alone cannot define a complete forecast. The estimator must account for attacker-side and victim-side procs, their timing, recursive/triggered spell behavior and actual caster/target identities. If it cannot prove the relevant proc closure is empty or bounded, the direct spell action remains `Unknown`.


### Negative flat spell-threat routing audit (2026-11-18)

The pinned Core implementation is `Spell::HandleThreatSpells` in `src/game/Spells/Spell.cpp`. It runs from `Spell::_handle_immediate_phase` after target-list construction and before per-target effect execution. The estimator must therefore model the already-resolved successful target/effect-mask set, not infer recipients from the spell's primary target alone.

Core classifies the spell using the same constructor rule used to populate `m_negativeEffectMask`: for every effect index, a non-positive `SpellEntry::IsPositiveEffect` marks that bit negative, and self-targeted `SPELL_EFFECT_SCHOOL_DAMAGE` is also marked negative. It then builds the active effect mask from non-empty effects. If any active bit is negative but not all active bits are negative, Core ignores the flat threat entry entirely. Otherwise the entry is positive or negative as a whole.

For each successful target, Core additionally requires `SpellThreatEntry::CanCauseThreatOnMask(ihit.effectMask)`. Positive threat calls the target's `HostileRefManager::threatAssist`, distributing the event over every hostile reference held by that target. Negative threat instead requires the affected target to `CanHaveThreatList()` and calls that target's `AddThreat(caster, threat, false, schoolMask, spellInfo)`. This is a different recipient topology: the affected hostile itself is the threat-list owner, and the caster is the threat owner. Core's `AddThreat` path applies `ThreatCalcHelper::CalcThreat` modifiers.

The positive flat-threat component is included inside the direct-heal-assist helper for the supplied healed target. The negative route is implemented separately by `EstimateNegativeFlatSpellThreatComponent`; its caller must supply the exact successful per-target effect masks and attest that the target set is complete. The helper validates target identity/liveness/map membership, rejects duplicate or unresolved targets and invalid masks, applies Core's mixed-sign rule, and emits one impact row per eligible hostile. If target enumeration, miss state, effect mask, or complete target-set proof is missing, the component remains `Unknown`; it must not guess that a spell has no flat threat.

This is a source-path audit only. No estimator behavior or runtime eligibility has been relaxed.


### Negative flat spell-threat estimator implementation slice (2026-11-18)

`EstimateNegativeFlatSpellThreatComponent` now projects only the negative `Spell::HandleThreatSpells` component from caller-supplied successful Core targets and their exact effect masks. The caller must explicitly attest that the successful target set is complete; absent that proof, unresolved targets, duplicate targets, missing masks, cross-map targets, dead targets, or a missing caster produce `Unknown`. The helper reproduces the active/negative effect-mask classification (including self-targeted school damage), Core's mixed-sign early return, `CanCauseThreatOnMask` filtering, harmful-threat suppression, conservative rejection of `SPELL_ATTR_EX_NO_THREAT` (because Core may update an existing reference but suppress creation of a new one), and the `ThreatCalcHelper::CalcThreat` modifier path. It emits one `FlatSpellThreat` row per eligible affected hostile, with the caster GUID as threat owner and the hostile GUID as threat-list owner.

The helper intentionally leaves the enclosing estimate `Unknown` when it emits component rows: this is not a complete spell-action forecast and does not close SpellScript, proc, or other side-effect paths. A component-level `NoThreat` is returned only when Core's negative flat-threat branch is provably inapplicable or has no eligible successful hostile targets. This component is runtime-fixture-verified by the Core cast parity fixture below; it remains a component only, not a complete spell-action forecast.


### Core cast parity fixture (2026-11-18)

The Core simulator now discovers a real loaded harmful one-effect aura spell with a `spell_threat` entry, estimates its single successful hostile target with effect mask `0x01`, executes the real triggered `Unit::CastSpell` path, and compares the actual hostile ThreatManager delta with the component's exact estimate. The fixture requires the hostile's health to remain unchanged so direct-damage threat cannot contaminate the measured flat-threat delta. The CI harness now requires the dedicated PASS marker.

This is a deliberately narrow runtime proof for the negative/harmful flat-threat route. On CI run `#485` (commit `0d22aff`), loaded spell `99` produced an estimated `9.000` threat and an actual Core ThreatManager delta of `9.000`, with unchanged target health. The fixture also exercises incomplete target-set, invalid effect-mask, duplicate target, unresolved target, and mixed-sign rejection; the CI harness requires dedicated PASS markers for both parity and fail-closed checks. This does not prove generic spell target enumeration, SpellScript/proc closure, periodic ticks, or action-level completeness.



### Applied periodic-damage tick component and Core runtime fixture (2026-11-18)

EstimateAppliedPeriodicDamageComponent is deliberately narrower than a DoT estimator. Its caller supplies the **already-applied** damage from one completed Core tick. The helper verifies that the spell contains a periodic-damage Aura effect, rejects missing/dead/cross-map actors and harmful-threat suppression, then applies Core's GetSpellThreatMultiplier(spellInfo) and ThreatCalcHelper::CalcThreat to that actual tick amount. It emits one exact PeriodicDamage impact row while leaving the enclosing estimate Unknown.

The boundary is important: Aura::PeriodicTick has already run the real AuraScript::OnPeriodicCalculateAmount, family/tick-specific scaling, target damage-taken modifiers, dithering, immunity/absorb/resist, proc dispatch, health clamp, and Unit::DealDamage(..., DOT, ..., spellProto, ...). Unit::DealDamage then bases hostile threat on the final applied damage. The helper does not calculate those steps, does not call PeriodicTick, and must not be used to forecast future ticks from stored Aura amount multiplied by remaining tick count.

The accompanying simulator fixture uses the pinned 5875 Shadow Word: Pain rank 8 spell (10894) only when its loaded DBC row matches the expected single periodic-damage Aura shape and has no AuraScript callback. It applies the aura through a real triggered Core cast, advances the normal active Creature/Aura update loop until exactly one tick occurs, then compares the observed health loss and ThreatManager delta against the component. The CI harness requires a dedicated PASS marker. This proves applied-tick parity for one bounded ordinary aura, not the generic pre-cast DoT path or any scripted/proc-driven DoT. HoT, periodic power, leech/funnel, future tick prediction and action-level completeness remain Unknown.

This fixture passed in trusted full-runtime CI #507 on commit 59db1044. The artifact log recorded ticks 0>1, 142 applied damage, estimated threat 142.000 and actual ThreatManager delta 142.000 (`matched=1`, cleanup=1). The runtime harness's final `Runtime PASS` marker confirms the new check did not bypass the existing Core stages.



### Applied periodic-healing assist component and Core runtime fixture (2026-11-18)

EstimateAppliedPeriodicHealingAssistComponent accepts only the **actual effective health gain** from one completed periodic-healing tick. It rejects missing/dead/out-of-map actors, unresolved hostile references, harmful side-effect paths with a separate caster health-loss event (SpellVisual 163), harmful/new-reference suppression attributes, and non-finite Core multipliers. It computes the same 50% periodic-heal assist coefficient used by Aura::PeriodicTick, divides that assist total by the healed Unit's live HostileRefManager recipient count, applies Core ThreatCalcHelper modifiers per hostile, and emits PeriodicHealAssist rows. Hard-CC suppression is mirrored from ThreatManager::addThreat; the enclosing estimate remains Unknown.

This is intentionally not a forecast of future HoT ticks. The input must be the effective healing gain after AuraScript amount calculation, SpellHealingBonusTaken, dithering and target-health clamping. Core dispatches periodic proc callbacks before calling HostileRefManager::threatAssist, so the actual recipient references must still be known and stable for the particular event. Additional proc-triggered effects, aura scripts, caster-health side effects and target fan-out outside this component are not closed by this helper.

The simulator fixture uses Rejuvenation rank 10 (spell 9841), verifies that it is a one-effect ordinary periodic-heal Aura without an AuraScript, creates two real hostile references on a damaged friendly target through Core DealDamage, applies the real triggered Core aura, waits for a real Aura tick via the normal active-object scheduler, then compares the actual effective healing and each of the two hostile ThreatManager deltas with the component. The CI runner requires a dedicated PASS marker. This fixture passed the full ARM64/Core-backed runtime in CI #529 on commit 2d643f1513aa0fb9c4ef9ab632665d887afbbf35. The artifact recorded one real tick (ticks 0>1), 189 effective healing, two hostile references, and matching estimated/actual deltas of 47.250/47.250 on each hostile; `estimateShape=1`, `matched=1`, and `cleanup=1`. The final simulator `Runtime PASS` marker confirms that the full Stage A-F12 suite also completed. This is applied-event component parity only; it does not establish future-tick HoT prediction or action-level completeness.



### Applied periodic-power assist component and Core parity fixture (2026-11-18)

Core has two related but distinct helpful-resource periodic paths:

- `SPELL_AURA_PERIODIC_ENERGIZE` applies `OnPeriodicCalculateAmount`, then calls `Unit::ModifyPower`. In Vanilla 1.12 (`SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4`), it sends 50% of the actual gain through the energized target's `HostileRefManager::threatAssist` only when the power is neither Mana nor Happiness. Those two power types are explicitly exempt.
- `SPELL_AURA_OBS_MOD_MANA` is handled separately. Its amount is a percentage of the target's maximum Mana; after `ModifyPower(POWER_MANA, ...)`, Core sends 50% of the actual gain through the same assist fan-out. It is not subject to the periodic-energize Mana exemption. Its handler also initializes the periodic timer itself.

`EstimateAppliedPeriodicPowerAssistComponent` takes the **actual effective resource gain** from one completed tick, the power type and exact aura effect index. It validates that the spell/effect/power tuple matches the two Core branches, returns component `NoThreat` for PERIODIC_ENERGIZE Mana/Happiness, rejects harmful/helpful threat suppression and unresolved recipients, applies the Core spell threat multiplier and `ThreatCalcHelper`, mirrors hard-CC assist suppression, and emits `PeriodicPowerAssist` rows. The enclosing estimate remains `Unknown`; this is not a prediction of future ticks. `SPELL_AURA_PERIODIC_MANA_LEECH` is a different direct-target path, now covered by a separate applied-event component: Core drains at most the target's current matching power, applies the aura script amount callback, derives `gain_amount` from `EffectMultipleValue` plus any Player spellmod, then uses the caster's actual `ModifyPower` return (resource-cap clamped) to calculate 50% × spell threat multiplier and calls `leechedTarget->AddThreat(caster, ...)` directly. This does not fan out across HostileRefManager recipients. If target power reaches zero, spell 21056 (Mark of Kazzak) triggers 21058; caster auras 17864/18393 (Improved Drain Mana) dispatch an additional 15%/30% periodic-damage tick. These are separate side-effect rows and keep the enclosing action estimate Unknown.

The simulator fixture uses the exact SpellMgr-owned Rejuvenation rank 10 entry (spell 9841), temporarily overriding only `EffectApplyAuraName[0]` and `EffectMiscValue[0]` while the isolated scenario executes. It does not pass a copied `SpellEntry` to Core: `Spell::Spell`'s native-pointer identity assertion remains satisfied. An RAII guard restores both original DBC fields after the aura is removed on every exit path. This is a bounded Core-runtime seam for Aura branch behavior, not a claim that the canonical 5875 DBC contains an energy PERIODIC_ENERGIZE spell; the native SpellMgr scan found none (`energy=0`, `mana-energize=13`, `observed-mana=2`). The three cases exercise energy PERIODIC_ENERGIZE (threat expected), mana PERIODIC_ENERGIZE (the 1.12 exemption expected), and OBS_MOD_MANA (mana assist threat expected), each with two real hostile references. CI #552 on commit `50adcd73` confirmed that real DoT and HoT stages still pass, but the power fixture correctly failed closed because no native energy candidate existed. Runtime parity for periodic power is therefore still pending.



### Applied periodic-mana-leech threat component and fixture (2026-11-18)

`EstimateAppliedPeriodicManaLeechComponent` accepts only the **actual caster power gain returned by Core `ModifyPower`** for one already-applied tick. It validates the exact effect type/index/power type, both live same-map actors, the hostile's `CanHaveThreatList`, the Core harmful-threat suppression attribute, game-master threat-owner rejection, and finite spell-threat/ThreatCalcHelper output. It emits one `PeriodicManaLeech` row keyed by caster GUID (threat owner) and leeched hostile GUID (ThreatManager owner). It uses non-assist `ThreatCalcHelper` semantics because Core calls `Unit::AddThreat` directly, not `HostileRefManager::threatAssist`. It returns Unknown for `SPELL_ATTR_EX_NO_THREAT`, because Core may update an existing reference but not create a new one and the full online/offline target threat list is not proven by the snapshot.

The enclosing estimate remains Unknown. Mark of Kazzak depletion (21056 -> 21058), Improved Drain Mana (17864 / 18393) bonus periodic damage (15% / 30%), AuraScript callbacks and other tick side effects are not folded into this single direct mana-gain component. A simulator fixture discovers an ordinary native single-effect 5875 periodic-mana-leech aura, excludes the known explosion and Improved Drain Mana side-effect cases, applies it to a real hostile Creature, advances one ordinary Core Aura tick, and compares target Mana loss, caster effective Mana gain and the target ThreatManager delta. Its dedicated ARM64 runtime marker is pending.



### Applied periodic-life-leech damage plus self-heal assist (2026-11-18)

Core groups `SPELL_AURA_PERIODIC_LEECH` and `SPELL_AURA_PERIODIC_HEALTH_FUNNEL` in one tick branch. For the bounded ordinary path it calculates damage after the Aura amount callback, target damage-taken modifiers, absorb/resist and health clamp; calls `pCaster->DealDamage(target, ..., DOT, spellProto)`; derives the self-heal from the actual `new_damage` times the effect multiplier; calls `pCaster->DealHeal(pCaster, ...)`; and then distributes 50% of the **actual effective self-heal gain** times the spell threat multiplier through the caster's HostileRefManager. The target's damage threat and the caster's healing-assist fan-out are separate Core ThreatManager changes. The same hostile can receive both, while other hostiles referencing the caster receive only the healing-assist portion.

The existing applied periodic-damage component now accepts PERIODIC_LEECH/HEALTH_FUNNEL effect metadata, and the applied periodic-healing-assist component accepts the corresponding self-heal effect metadata. A new Core fixture uses one real caster and two real hostile references, damages hostile A with a real periodic-leech Aura tick, and compares: (A) damage-component estimate + half of the caster's heal-assist estimate, and (B) the other half of the caster's heal-assist estimate. It filters scripted/visual-163/attribute-suppressed entries and keeps the enclosing action Unknown. Full runtime verification is pending.


## Direct-heal threat estimator — F53c implementation status (2026-11-24)

The current CoreThreatImpactEstimator has a test-only direct-heal component projection:

- Healing-assist threat is bounded from maximum effective healing, the audited class coefficient (Priest 0.5 / Paladin 0.25), the Core spell threat multiplier, and the healed Player's live HostileRefManager recipient set.
- Positive flat spell_threat is a separate path. It is divided over the same verified hostile references, then passed through Core ThreatCalcHelper modifiers; it is not multiplied by the heal spell's GetSpellThreatMultiplier.
- A trusted Vanilla 5875 fixture temporarily inserts spell_threat=24 for Lesser Heal in the disposable simulator DB. With two hostile references, the estimator predicts 12.0 flat threat per hostile. Core's two actual ThreatManagers each gain 12.0; actual combined healer threat deltas are 24.5/24.5, below the combined assist-plus-flat upper bound 97.8/97.8. The fixture restores the original DB row before exit.
- Helpful proc risk is fail-closed on both caster and healed target. The gate rejects active AuraScript holders, matching side-specific helpful-spell proc flags, and currently audited hard-coded helpful proc cases including Priest Inspiration. The real Inspiration aura fixture verifies no impacts while present and restores the component-only estimate after removal.

These checks validate components and selected proc-risk cases, not the complete direct-heal cast envelope. F53d now adds real Core no-reference and zero-flat fixtures; F53e still owns secondary triggered-spell/recipient closure, and F53f owns action reservation/terminal cleanup. Until all of these are closed, the direct-heal candidate remains Unknown at the whole-action threat-budget boundary and must not be production-registered.


## F53d — direct-heal recipient-empty and zero-flat boundaries (trusted runtime pending)

The F53d simulator extension adds two real Core cast cases under the disposable simulator database:

1. **No hostile references, positive flat row present.** The fixture temporarily installs a positive Lesser Heal flat spell-threat row, then damages the healer and casts Lesser Heal on itself while its HostileRefManager has no references. The estimator must return a snapshot-matched NoThreat, AssessThreatImpactBudget must return Safe, the real cast must heal and spend Mana, and no hostile reference or ThreatManager delta may appear. This checks both the direct healing-assist path and the positive flat HandleThreatSpells path against the actual empty recipient list.

2. **Zero flat row with two references.** The fixture changes the temporary Lesser Heal row to zero and reloads SpellMgr. Core's Spell::HandleThreatSpells exits when SpellThreatEntry::threat == 0, while the ordinary healing-assist path remains active. The estimator must emit exactly two DirectHealAssist upper-bound entries and no FlatSpellThreat entry. The real heal's two ThreatManager deltas must match the assist-only Core ThreatCalcHelper calculation. The original spell_threat rows must be restored and independently verified before the fixture reports PASS.

**Negative amount boundary:** SpellThreatEntry::threat is uint16, and SpellMgr::LoadSpellThreats reads the field with Field::GetUInt16(). A negative persisted flat-threat amount is outside the representable Core value domain; the pure CalculateDirectHealThreatComponents helper rejects a negative input. This must not be confused with a harmful spell: harmful flat threat still uses a positive unsigned row amount, but Core routes it to the affected target's own ThreatManager rather than helpful HostileRefManager::threatAssist.

The source and simulator markers are implemented, but no runtime acceptance is claimed until a trusted branch-push run passes the full fixture-backed Vanilla 5875 simulator and final Runtime PASS. The direct-heal action remains Unknown overall and unregistered in production NewEngine.
