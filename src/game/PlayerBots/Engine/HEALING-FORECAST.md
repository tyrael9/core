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

F54d now begins with Core timer/schedule capture and a pure arrival timeline. Remaining work is to prove arrival ordering against real Core application, observe actual effective DealHeal gain and overheal, model damage-before-arrival and simultaneous/concurrent healing through Core fixtures, then integrate only the proven bounds. Priest production registration remains blocked until the complete heal threat envelope is proven and the action returns a snapshot-matched, complete estimate.

## F54d time-aware foundation — first implementation slice

The snapshot now records a direct heal's remaining Core cast timer and a bounded per-HoT schedule (first tick delay, period, remaining ticks and target-adjusted amount per tick). The pure HealingForecast.h helper expands HoT ticks into millisecond arrival events and projects aggregate effective healing/overheal. Same-time events are grouped and explicitly marked as having unknown per-cast attribution; the projection assumes no intervening damage. A per-member schedule cap of 32 has an explicit truncation flag.

The real-Core F54d fixture compares the copied direct-cast timer with the live Spell timer and the copied Renew schedule with the live Aura timer/cadence. This initial slice does not yet alter target triage or reservation amounts. It does not claim Core effective-heal parity, model damage between arrivals, resolve cast-versus-HoT ordering races, or close healing threat. Those require subsequent fixtures and integration; the whole Priest direct-heal estimate remains Unknown.

## Shared uncovered-deficit accounting

F54c uses one pure helper, `GetPriestUncoveredMissingHealth`, for both target triage and the direct-heal action adapter. It subtracts supported non-Engine direct casts, ordinary remaining HoT ticks, universal absorbs and other Bots' Heal reservations from the current deficit, with 64-bit accumulation and a deficit cap. `PriestDirectHealAction::IsUseful` and `ExecuteCore` use that same result before deciding whether a cast is useful and how much to reserve. This prevents the selector and action adapter from disagreeing or reserving healing already covered by known incoming effects.
