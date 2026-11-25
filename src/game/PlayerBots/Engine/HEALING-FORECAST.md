# Priest healing forecast boundary

## Purpose

F54c extends the snapshot-only Priest triage input with conservative estimates of healing that is already in flight or already attached to a party member. It is not a castability decision, does not change Core's heal resolution, and does not close the action's threat estimate.

All data stored in `PartySnapshot` is scalar/GUID-only. No `Player`, `Unit`, `Spell` or `Aura` pointer is retained after `PartySnapshotBuilder::Build`.

## Credited sources

1. **In-flight direct heal.** A source member contributes only when its current Core Spell is in PREPARING or CASTING state, is not channeled, and contains exactly one effect: `SPELL_EFFECT_HEAL`. The effect must use exactly one explicit single-unit selector (`TARGET_UNIT_CASTER`, `TARGET_UNIT_FRIEND` or `TARGET_UNIT`), target a same-Group Player on this map/instance, and have no SpellScript (`ScriptId == 0`). The amount is calculated by Core's `Spell::CalculateDamage` against that target and rounded downward for the forecast. Mixed effects, area/party/chain targeting, scripts and unresolved targets are not credited.
2. **HoT.** Non-scripted `SPELL_AURA_PERIODIC_HEAL` auras contribute the Core-stored caster-adjusted tick amount, adjusted through Core's `SpellHealingBonusTaken`, multiplied by the finite remaining tick count (`GetAuraMaxTicks() - GetAuraTicks()`). Auras with scripts, no caster, invalid/nonpositive amounts, expired duration or unknown tick cadence are omitted.
3. **Absorb.** `SPELL_AURA_SCHOOL_ABSORB` auras contribute their current remaining `m_amount` only when `m_miscvalue == 0x7f`, the complete seven-school mask, and the Aura has no `AuraScript`. Configured `SpellScript` auras remain excluded except for the exact Vanilla Power Word: Shield ranks registered by `spell_priest_power_word_shield` (17, 592, 600, 3747, 6065, 6066, 10898, 10899, 10900, 10901 and 27607). That audited `OnHit` callback only applies Weakened Soul after a successful shield hit; it does not mutate the absorb Aura or its amount. The live Core fixture confirmed Spell 17's `ScriptId=669`, no AuraScript, all-school mask 127 and remaining absorb 48. Other scripted absorbs remain excluded. School-specific absorbs are not credited because the future damage school is unknown. `SPELL_AURA_MANA_SHIELD` is excluded because its absorb consumes a separate, changing mana resource.

## F54c trusted Core runtime evidence

Trusted push [#45538919881](https://github.com/tyrael9/core/actions/runs/45538919881) ran the full Vanilla 5875 Core simulator and emitted `engineCastSkipped=1 externalDirectCaptured=1 externalAmount=56 hotCaptured=1 hotAmount=45 allSchoolAbsorbCaptured=1 absorbAmount=48`, followed by the dedicated F54c PASS marker and final `Runtime PASS`. This confirms the audited Power Word: Shield absorb is captured from the real Core Aura despite registered SpellScript ID 669; its AuraScript is null and the seven-school mask is 127. The allowlist remains restricted to the exact audited ranks.

## Reservation and de-duplication

F54b Heal reservations remain the authority for Engine-owned direct casts. The snapshot builder deliberately leaves `currentSpellHealingAmount` at zero for an Engine-owned cast. Triage credits the other-Bot reservation from the shared `PartyCoordinator`, so the same in-flight cast is not counted twice. Non-Engine player casts are credited from the caster member's `currentSpellTargetGuid` and `currentSpellHealingAmount`.

The direct-cast, HoT and all-school absorb amounts are added to the shared reservation amount using 64-bit intermediate arithmetic. The result is capped at the target's current raw health deficit before projected health is ranked. This prevents integer wrap and prevents the forecast from assigning more than full health.

## Deliberate non-goals and next milestone

F54c is a coarse amount forecast, not a time simulation. It does not infer when a cast will land, when each HoT tick will occur, whether the target takes damage before arrival, which absorb school will match future damage, whether a heal will be partly or wholly overheal, or how competing heals resolve in Core order. It also does not estimate helpful-threat fan-out or proc/script side effects. Unknown or unsupported values are never upgraded to a complete threat estimate.

F54d's timer/schedule and actual effective-healing/concurrency slices are now trusted-runtime validated. Push #45547778038 observed a real Renew tick with one point of effective gain against a nine-point expected tick, a conservative eight-point overheal lower bound, an exact direct-heal one-health-deficit clamp/overheal case, and two same-time Core heals with aggregate effective healing 150, overheal 50 and unknown per-caster attribution. These fixtures validate Core observations; they do not enable production healing. Explicit damage arrivals are being added to the pure timeline, while a real Core damage-between-heals parity fixture is the next gate. Random HoT rounding bounds, cast pushback/interruption after the snapshot, same-timestamp Core Heal/Damage ordering, full Core ordering and threat closure remain open.

## F54d time-aware foundation — timer/schedule and effective-healing/concurrency slices trusted-runtime validated; damage interleaving in progress

Push [#45541255150](https://github.com/tyrael9/core/actions/runs/45541255150) passed the timer/schedule parity gate. The runtime recorded `directTimerCaptured=1 directRemainingMs=1500 hotTimerCaptured=1 hotFirstTickMs=3000 hotPeriodMs=3000 hotRemainingTicks=5`, followed by final `Runtime PASS`. Artifact [#14990075861](https://github.com/tyrael9/core/actions/runs/45541255150/artifacts/14990075861).

Push [#45547778038](https://github.com/tyrael9/core/actions/runs/45547778038) passed the actual Core effective-healing/concurrency gate. The F54d marker recorded a real direct-cast health gain of 69 and exact direct-heal overheal of 99 at a one-health deficit; a real Renew Aura tick advanced from 0 to 1 with one point of effective healing against a nine-point expected tick and an eight-point conservative overheal lower bound; two same-time Core DealHeal applications produced 150 aggregate effective healing and 50 overheal, with per-caster attribution explicitly unknown. The dedicated F54d PASS marker and final simulator `Runtime PASS` followed. Artifact [#14992557137](https://github.com/tyrael9/core/actions/runs/45547778038/artifacts/14992557137).

The pure timeline now distinguishes Heal and Damage events. Explicit damage at a distinct timestamp reduces projected health before later healing; mixed Heal/Damage events sharing a timestamp are rejected because Core ordering is not established. Pure regressions cover damage before a heal, damage between two heals, and the ambiguous same-time rejection. This implementation still needs a trusted Core runtime fixture that applies real damage between actual heal arrivals. It does not change production triage or reservation sizing, and simultaneous healing still carries no per-caster attribution.

Random HoT tick amount exactness, cast pushback/interruption after the snapshot, same-timestamp Core Heal/Damage ordering, absorb-school matching against predicted damage, full Core ordering and threat closure remain open. The whole direct-heal threat estimate remains Unknown.
## Shared uncovered-deficit accounting

F54c uses one pure helper, `GetPriestUncoveredMissingHealth`, for both target triage and the direct-heal action adapter. It subtracts supported non-Engine direct casts, ordinary remaining HoT ticks, universal absorbs and other Bots' Heal reservations from the current deficit, with 64-bit accumulation and a deficit cap. `PriestDirectHealAction::IsUseful` and `ExecuteCore` use that same result before deciding whether a cast is useful and how much to reserve. This prevents the selector and action adapter from disagreeing or reserving healing already covered by known incoming effects.
