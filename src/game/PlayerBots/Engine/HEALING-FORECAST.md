# Priest healing forecast boundary

## Purpose

F54c extends the snapshot-only Priest triage input with conservative estimates of healing that is already in flight or already attached to a party member. It is not a castability decision, does not change Core's heal resolution, and does not close the action's threat estimate.

All data stored in `PartySnapshot` is scalar/GUID-only. No `Player`, `Unit`, `Spell` or `Aura` pointer is retained after `PartySnapshotBuilder::Build`.

## Credited sources

1. **In-flight direct heal.** A source member contributes only when its current Core Spell is in PREPARING or CASTING state, is not channeled, and contains exactly one effect: `SPELL_EFFECT_HEAL`. The effect must use exactly one explicit single-unit selector (`TARGET_UNIT_CASTER`, `TARGET_UNIT_FRIEND` or `TARGET_UNIT`), target a same-Group Player on this map/instance, and have no SpellScript (`ScriptId == 0). The amount is calculated by Core's `Spell::CalculateDamage` against that target and rounded downward for the forecast. Mixed effects, area/party/chain targeting, scripts and unresolved targets are not credited.
2. **HoT.** Non-scripted `SPELL_AURA_PERIODIC_HEAL` auras contribute the Core-stored caster-adjusted tick amount, adjusted through Core's `SpellHealingBonusTaken`, multiplied by the finite remaining tick count (`GetAuraMaxTicks() - GetAuraTicks()`). Auras with scripts, no caster, invalid/nonpositive amounts, expired duration or unknown tick cadence are omitted.
3. **Absorb.** Non-scripted `SPELL_AURA_SCHOOL_ABSORB` auras contribute their current remaining `m_amount` only when `m_miscvalue == 0x7f`, the complete seven-school mask. School-specific absorbs are not credited because the future damage school is unknown. `SPELL_AURA_MANA_SHIELD` is excluded because its absorb consumes a separate, changing mana resource.

## Reservation and de-duplication

F54b Heal reservations remain the authority for Engine-owned direct casts. The snapshot builder deliberately leaves `currentSpellHealingAmount` at zero for an Engine-owned cast. Triage credits the other-Bot reservation from the shared `PartyCoordinator`, so the same in-flight cast is not counted twice. Non-Engine player casts are credited from the caster member's `currentSpellTargetGuid` and `currentSpellHealingAmount`.

The direct-cast, HoT and all-school absorb amounts are added to the shared reservation amount using 64-bit intermediate arithmetic. The result is capped at the target's current raw health deficit before projected health is ranked. This prevents integer wrap and prevents the forecast from assigning more than full health.

## Deliberate non-goals and next milestone

F54c is a coarse amount forecast, not a time simulation. It does not infer when a cast will land, when each HoT tick will occur, whether the target takes damage before arrival, which absorb school will match future damage, whether a heal will be partly or wholly overheal, or how competing heals resolve in Core order. It also does not estimate helpful-threat fan-out or proc/script side effects. Unknown or unsupported values are never upgraded to a complete threat estimate.

F54d must introduce the time/effective-healing/overheal/concurrent-arrival boundary model and corresponding Core fixtures. Priest production registration remains blocked until the complete heal threat envelope is proven and the action returns a snapshot-matched, complete estimate.
