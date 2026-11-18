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
