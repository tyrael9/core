#ifndef MANGOS_PARTYBOT_CORE_THREAT_IMPACT_ESTIMATOR_H
#define MANGOS_PARTYBOT_CORE_THREAT_IMPACT_ESTIMATOR_H

#include "Creature.h"
#include "HostileRefManager.h"
#include "PlayerBots/Engine/PartySnapshot.h"
#include "PlayerBots/Engine/ThreatImpact.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "Unit.h"

namespace PartyBotEngine {

// Estimate one applied, spell-less direct-damage component using the same
// Core threat multiplier and ThreatCalcHelper used by Unit::DealDamage ->
// Unit::AddThreat -> ThreatManager::addThreat. The caller must pass damage
// already applied by Core (or a clearly identified upper bound), not base or
// tooltip damage. This component is intentionally Unknown as a whole action:
// procs, pet-owner reference side effects and other action threat paths must
// be enumerated before an Action may promote it to Complete.
inline ThreatImpactEstimate EstimateAppliedDirectDamageComponent(
    Unit* threatOwner, Creature* hostile, std::uint32_t appliedDamage,
    SpellSchoolMask schoolMask, std::uint32_t snapshotEpoch,
    ThreatImpactBound bound = ThreatImpactBound::Exact)
{
    ThreatImpactEstimate estimate;
    estimate.snapshotEpoch = snapshotEpoch;
    estimate.reason = "direct_damage_component_only";

    if (!snapshotEpoch || !threatOwner || !hostile ||
        !threatOwner->IsAlive() || !hostile->IsAlive() ||
        !threatOwner->IsInMap(hostile) || threatOwner == hostile ||
        !hostile->CanHaveThreatList()) {
        estimate.reason = "direct_damage_component_missing_or_ineligible_core_state";
        return estimate;
    }

    float const rawThreat = static_cast<float>(appliedDamage) *
        sSpellMgr.GetSpellThreatMultiplier(nullptr);
    if (!std::isfinite(rawThreat) || rawThreat < 0.0f) {
        estimate.reason = "direct_damage_component_invalid_core_multiplier";
        return estimate;
    }

    // This is the same calculation called by ThreatManager::addThreat with
    // pVictim == threatOwner. It includes the actor's Core threat modifiers.
    float const threatPoints = ThreatCalcHelper::CalcThreat(
        threatOwner, rawThreat, false, schoolMask, nullptr);
    if (!std::isfinite(threatPoints) || threatPoints < 0.0f) {
        estimate.reason = "direct_damage_component_invalid_core_threat";
        return estimate;
    }

    ThreatImpactEntry impact;
    impact.threatOwnerGuid = threatOwner->GetObjectGuid().GetRawValue();
    impact.hostileGuid = hostile->GetObjectGuid().GetRawValue();
    impact.threatPoints = threatPoints;
    impact.source = ThreatImpactSource::DirectDamage;
    impact.bound = bound;
    estimate.impacts.push_back(impact);
    return estimate;
}

// Estimate only the direct-heal assist component. Spell::DoAllEffectOnTarget
// passes effective healing gain through the class coefficient and spell threat
// multiplier, then HostileRefManager::threatAssist divides that value over every
// reference held by the healed Unit. The caller supplies a Core-derived upper
// bound for effective healing; this helper deliberately leaves the envelope
// Unknown because flat spell-table threat, triggered procs and other side
// effects are not covered by this component.
inline ThreatImpactEstimate EstimateDirectHealingAssistComponent(
    Unit* realCaster, Unit* healedTarget, float maxEffectiveHealing,
    SpellEntry const* spellInfo, PartySnapshot const& snapshot)
{
    ThreatImpactEstimate estimate;
    estimate.snapshotEpoch = snapshot.epoch;
    estimate.reason = "direct_heal_assist_component_only";

    if (!snapshot.epoch || !realCaster || !healedTarget || !spellInfo ||
        !std::isfinite(maxEffectiveHealing) || maxEffectiveHealing < 0.0f ||
        realCaster->GetTypeId() != TYPEID_PLAYER ||
        healedTarget->GetTypeId() != TYPEID_PLAYER ||
        !realCaster->IsAlive() || !healedTarget->IsAlive() ||
        !realCaster->IsInWorld() || !healedTarget->IsInWorld() ||
        realCaster->GetMap() != healedTarget->GetMap() ||
        realCaster->GetMapId() != snapshot.mapId ||
        healedTarget->GetMapId() != snapshot.mapId ||
        realCaster->GetInstanceId() != snapshot.instanceId ||
        healedTarget->GetInstanceId() != snapshot.instanceId) {
        estimate.reason = "direct_heal_assist_component_missing_or_ineligible_core_state";
        return estimate;
    }

    Guid const casterGuid = realCaster->GetObjectGuid().GetRawValue();
    Guid const targetGuid = healedTarget->GetObjectGuid().GetRawValue();
    PartyMemberSnapshot const* casterMember = snapshot.FindMember(casterGuid);
    PartyMemberSnapshot const* targetMember = snapshot.FindMember(targetGuid);
    if (!casterGuid || !targetGuid || !snapshot.groupId ||
        !casterMember || !targetMember ||
        !casterMember->online || !casterMember->alive ||
        !targetMember->online || !targetMember->alive) {
        estimate.reason = "direct_heal_assist_component_party_members_unresolved";
        return estimate;
    }

    if (spellInfo->HasAttribute(SPELL_ATTR_EX4_NO_HELPFUL_THREAT)) {
        estimate.reason = "direct_heal_assist_suppressed_by_core_spell_attribute";
        return estimate;
    }

    float const classThreatModifier =
        realCaster->GetClass() == CLASS_PALADIN ? 0.25f : 0.5f;
    float const spellThreatMultiplier = sSpellMgr.GetSpellThreatMultiplier(spellInfo);
    double const totalAssistThreat =
        static_cast<double>(maxEffectiveHealing) *
        static_cast<double>(classThreatModifier) *
        static_cast<double>(spellThreatMultiplier);
    if (!std::isfinite(totalAssistThreat) || totalAssistThreat < 0.0 ||
        totalAssistThreat > std::numeric_limits<float>::max()) {
        estimate.reason = "direct_heal_assist_component_invalid_core_multiplier";
        return estimate;
    }

    std::vector<Guid> hostileGuids;
    HostileRefManager& hostileRefs = healedTarget->GetHostileRefManager();
    for (HostileReference* reference = hostileRefs.getFirst();
         reference; reference = reference->next()) {
        Unit* hostileUnit = reference->getSourceUnit();
        if (!hostileUnit || reference->getTarget() != healedTarget ||
            reference->getUnitGuid().GetRawValue() != targetGuid ||
            !hostileUnit->CanHaveThreatList() || !hostileUnit->IsAlive() ||
            !hostileUnit->IsInWorld() || hostileUnit->GetMap() != healedTarget->GetMap()) {
            estimate.reason = "direct_heal_assist_component_hostile_reference_unresolved";
            estimate.impacts.clear();
            return estimate;
        }

        Guid const hostileGuid = hostileUnit->GetObjectGuid().GetRawValue();
        HostileSnapshot const* hostileSnapshot = snapshot.FindHostile(hostileGuid);
        if (!hostileGuid || !hostileSnapshot || !hostileSnapshot->alive) {
            estimate.reason = "direct_heal_assist_component_hostile_missing_from_snapshot";
            estimate.impacts.clear();
            return estimate;
        }

        std::size_t targetReferenceCount = 0;
        for (ThreatEntrySnapshot const& threat : hostileSnapshot->threat)
            if (threat.unitGuid == targetGuid)
                ++targetReferenceCount;
        if (targetReferenceCount != 1) {
            estimate.reason = "direct_heal_assist_component_snapshot_reference_mismatch";
            estimate.impacts.clear();
            return estimate;
        }

        for (Guid existing : hostileGuids) {
            if (existing == hostileGuid) {
                estimate.reason = "direct_heal_assist_component_duplicate_hostile_reference";
                estimate.impacts.clear();
                return estimate;
            }
        }
        hostileGuids.push_back(hostileGuid);
    }

    if (hostileGuids.empty()) {
        estimate.reason = "direct_heal_assist_component_has_no_hostile_references";
        return estimate;
    }

    float const threatPerReference = static_cast<float>(
        totalAssistThreat / static_cast<double>(hostileGuids.size()));
    if (!std::isfinite(threatPerReference) || threatPerReference < 0.0f) {
        estimate.reason = "direct_heal_assist_component_invalid_fanout_value";
        return estimate;
    }

    // ThreatManager::addThreat calls this with pVictim == realCaster and the
    // same spell/school values. CC suppression is deliberately not subtracted:
    // omitting that reduction keeps this a safe upper bound.
    for (Guid hostileGuid : hostileGuids) {
        float const threatPoints = ThreatCalcHelper::CalcThreat(
            realCaster, threatPerReference, false,
            spellInfo->GetSpellSchoolMask(), spellInfo);
        if (!std::isfinite(threatPoints) || threatPoints < 0.0f) {
            estimate.reason = "direct_heal_assist_component_invalid_core_threat";
            estimate.impacts.clear();
            return estimate;
        }

        ThreatImpactEntry impact;
        impact.threatOwnerGuid = casterGuid;
        impact.hostileGuid = hostileGuid;
        impact.threatPoints = threatPoints;
        impact.source = ThreatImpactSource::DirectHealAssist;
        impact.bound = ThreatImpactBound::UpperBound;
        estimate.impacts.push_back(impact);
    }

    // Spell::HandleThreatSpells adds an additional flat-threat component for
    // positive spells. For each successful target it uses the same helpful
    // HostileRefManager fan-out, then ThreatManager::addThreat applies the
    // caster's spell-threat modifiers. This function covers only the supplied
    // healed target; multi-target spell recipients must be enumerated by the
    // caller and the enclosing action remains Unknown.
    SpellThreatEntry const* flatThreatEntry =
        sSpellMgr.GetSpellThreatEntry(spellInfo->Id);
    std::uint8_t effectMask = 0;
    bool positiveForFlatThreat = true;
    for (std::uint8_t i = 0; i < MAX_EFFECT_INDEX; ++i) {
        if (!spellInfo->Effect[i])
            continue;

        effectMask |= static_cast<std::uint8_t>(1u << i);
        if (!spellInfo->IsPositiveEffect(SpellEffectIndex(i)) ||
            // Spell's constructor adds self-damage to m_negativeEffectMask
            // even when SpellEntry::IsPositiveEffect regards it as positive.
            (spellInfo->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE &&
             spellInfo->EffectImplicitTargetA[i] == TARGET_UNIT_CASTER))
            positiveForFlatThreat = false;
    }

    if (flatThreatEntry && flatThreatEntry->threat &&
        positiveForFlatThreat && effectMask &&
        flatThreatEntry->CanCauseThreatOnMask(effectMask)) {
        float const flatThreatPerReference = static_cast<float>(
            flatThreatEntry->threat / static_cast<double>(hostileGuids.size()));
        if (!std::isfinite(flatThreatPerReference) || flatThreatPerReference < 0.0f) {
            estimate.reason = "direct_heal_assist_component_invalid_flat_spell_threat";
            estimate.impacts.clear();
            return estimate;
        }

        for (Guid hostileGuid : hostileGuids) {
            float const threatPoints = ThreatCalcHelper::CalcThreat(
                realCaster, flatThreatPerReference, false,
                spellInfo->GetSpellSchoolMask(), spellInfo);
            if (!std::isfinite(threatPoints) || threatPoints < 0.0f) {
                estimate.reason = "direct_heal_assist_component_invalid_flat_threat_modifier";
                estimate.impacts.clear();
                return estimate;
            }

            ThreatImpactEntry impact;
            impact.threatOwnerGuid = casterGuid;
            impact.hostileGuid = hostileGuid;
            impact.threatPoints = threatPoints;
            impact.source = ThreatImpactSource::FlatSpellThreat;
            impact.bound = ThreatImpactBound::UpperBound;
            estimate.impacts.push_back(impact);
        }
    }

    return estimate;
}


// One successful Unit target recorded by Spell::m_UniqueTargetInfo. The
// caller must supply the exact per-target effectMask and assert that the
// complete successful target set for this cast has been enumerated.
struct NegativeFlatSpellThreatTarget
{
    Unit* target = nullptr;
    std::uint8_t successfulEffectMask = 0;
};

inline ThreatImpactEstimate EstimateNegativeFlatSpellThreatComponent(
    Unit* realCaster, SpellEntry const* spellInfo,
    SpellThreatEntry const* threatEntry,
    std::vector<NegativeFlatSpellThreatTarget> const& successfulTargets,
    bool successfulTargetSetComplete, std::uint32_t snapshotEpoch)
{
    ThreatImpactEstimate estimate;
    estimate.snapshotEpoch = snapshotEpoch;
    estimate.reason = "negative_flat_spell_threat_component_only";

    if (!snapshotEpoch || !realCaster || !spellInfo || !threatEntry ||
        !threatEntry->threat || !successfulTargetSetComplete ||
        !realCaster->IsInWorld() || !realCaster->IsAlive() ||
        spellInfo->HasAttribute(SPELL_ATTR_EX4_NO_HARMFUL_THREAT) ||
        // Core's no-new-threat flag still permits changing an existing
        // reference; without proving that reference exists, fail closed.
        spellInfo->HasAttribute(SPELL_ATTR_EX_NO_THREAT)) {
        estimate.reason = "negative_flat_spell_threat_component_inputs_incomplete";
        return estimate;
    }

    std::uint8_t activeEffectMask = 0;
    std::uint8_t negativeEffectMask = 0;
    for (std::uint8_t i = 0; i < MAX_EFFECT_INDEX; ++i) {
        if (!spellInfo->Effect[i])
            continue;

        std::uint8_t const bit = static_cast<std::uint8_t>(1u << i);
        activeEffectMask |= bit;
        if (!spellInfo->IsPositiveEffect(SpellEffectIndex(i)) ||
            (spellInfo->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE &&
             spellInfo->EffectImplicitTargetA[i] == TARGET_UNIT_CASTER))
            negativeEffectMask |= bit;
    }

    // Spell::HandleThreatSpells aborts the whole flat-threat path for mixed
    // positive/negative active effects. A wholly positive spell belongs to
    // the separate positive-assist component, not this estimator.
    if (!activeEffectMask || !negativeEffectMask ||
        negativeEffectMask != activeEffectMask) {
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.reason = "core_negative_flat_spell_threat_not_applicable";
        return estimate;
    }

    if (successfulTargets.empty()) {
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.reason = "core_negative_flat_spell_threat_no_successful_targets";
        return estimate;
    }

    Guid const casterGuid = realCaster->GetObjectGuid().GetRawValue();
    if (!casterGuid) {
        estimate.reason = "negative_flat_spell_threat_caster_guid_missing";
        return estimate;
    }

    std::vector<Guid> seenTargets;
    for (NegativeFlatSpellThreatTarget const& hit : successfulTargets) {
        Unit* target = hit.target;
        if (!target || !hit.successfulEffectMask ||
            (hit.successfulEffectMask & static_cast<std::uint8_t>(~activeEffectMask)) ||
            !target->IsInWorld() || !target->IsAlive() ||
            target->GetMap() != realCaster->GetMap()) {
            estimate.reason = "negative_flat_spell_threat_target_unresolved";
            estimate.impacts.clear();
            return estimate;
        }

        Guid const targetGuid = target->GetObjectGuid().GetRawValue();
        if (!targetGuid) {
            estimate.reason = "negative_flat_spell_threat_target_guid_missing";
            estimate.impacts.clear();
            return estimate;
        }
        for (Guid seen : seenTargets) {
            if (seen == targetGuid) {
                estimate.reason = "negative_flat_spell_threat_duplicate_target";
                estimate.impacts.clear();
                return estimate;
            }
        }
        seenTargets.push_back(targetGuid);

        if (!threatEntry->CanCauseThreatOnMask(hit.successfulEffectMask) ||
            !target->CanHaveThreatList())
            continue;

        // Spell::HandleThreatSpells calls target->AddThreat(caster, threat,
        // false, schoolMask, spellInfo). AddThreat applies this exact Core
        // threat calculation before storing the row on the affected hostile.
        float const threatPoints = ThreatCalcHelper::CalcThreat(
            realCaster, static_cast<float>(threatEntry->threat), false,
            spellInfo->GetSpellSchoolMask(), spellInfo);
        if (!std::isfinite(threatPoints) || threatPoints < 0.0f) {
            estimate.reason = "negative_flat_spell_threat_invalid_core_modifier";
            estimate.impacts.clear();
            return estimate;
        }

        ThreatImpactEntry impact;
        impact.threatOwnerGuid = casterGuid;
        impact.hostileGuid = targetGuid;
        impact.threatPoints = threatPoints;
        impact.source = ThreatImpactSource::FlatSpellThreat;
        impact.bound = ThreatImpactBound::Exact;
        estimate.impacts.push_back(impact);
    }

    if (estimate.impacts.empty()) {
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.reason = "core_negative_flat_spell_threat_no_eligible_hostile_targets";
        return estimate;
    }

    estimate.reason = "negative_flat_spell_threat_component_only";
    return estimate;
}



// Estimate only one already-applied ordinary periodic-damage tick. Aura::PeriodicTick
// has already resolved scripts, family/tick scaling, target damage-taken bonuses,
// dithering, immunity/absorb/resist, procs and the target-health clamp. Unit::DealDamage
// then multiplies the final applied DOT damage by GetSpellThreatMultiplier(spellInfo)
// and routes it through hostile->AddThreat(threatOwner, ...). This component must
// never be used to predict future ticks from the Aura's nominal stored amount.
inline ThreatImpactEstimate EstimateAppliedPeriodicDamageComponent(
    Unit* threatOwner, Creature* hostile, std::uint32_t appliedDamage,
    SpellEntry const* spellInfo, std::uint32_t snapshotEpoch)
{
    ThreatImpactEstimate estimate;
    estimate.snapshotEpoch = snapshotEpoch;
    estimate.reason = "periodic_damage_component_only";

    bool periodicDamageEffect = false;
    if (spellInfo) {
        for (std::uint8_t i = 0; i < MAX_EFFECT_INDEX; ++i) {
            if (spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
                (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
                 spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE_PERCENT)) {
                periodicDamageEffect = true;
                break;
            }
        }
    }

    if (!snapshotEpoch || !threatOwner || !hostile || !spellInfo ||
        !periodicDamageEffect || !threatOwner->IsAlive() || !hostile->IsAlive() ||
        !threatOwner->IsInWorld() || !hostile->IsInWorld() ||
        !threatOwner->IsInMap(hostile) || threatOwner == hostile ||
        !hostile->CanHaveThreatList() ||
        spellInfo->HasAttribute(SPELL_ATTR_EX4_NO_HARMFUL_THREAT) ||
        // Core may update an existing reference but will not create a new one.
        // Until the estimator proves that reference exists, this stays Unknown.
        spellInfo->HasAttribute(SPELL_ATTR_EX_NO_THREAT)) {
        estimate.reason = "periodic_damage_component_missing_or_ineligible_core_state";
        return estimate;
    }

    if (!appliedDamage) {
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.reason = "periodic_damage_tick_applied_no_damage";
        return estimate;
    }

    float const spellThreatMultiplier = sSpellMgr.GetSpellThreatMultiplier(spellInfo);
    float const rawThreat = static_cast<float>(appliedDamage) * spellThreatMultiplier;
    if (!std::isfinite(spellThreatMultiplier) || spellThreatMultiplier < 0.0f ||
        !std::isfinite(rawThreat) || rawThreat < 0.0f) {
        estimate.reason = "periodic_damage_component_invalid_core_multiplier";
        return estimate;
    }

    // ThreatManager::addThreat calls this exact Core calculation for the
    // threat owner supplied by Unit::DealDamage.
    float const threatPoints = ThreatCalcHelper::CalcThreat(
        threatOwner, rawThreat, false, spellInfo->GetSpellSchoolMask(), spellInfo);
    if (!std::isfinite(threatPoints) || threatPoints < 0.0f) {
        estimate.reason = "periodic_damage_component_invalid_core_threat";
        return estimate;
    }

    ThreatImpactEntry impact;
    impact.threatOwnerGuid = threatOwner->GetObjectGuid().GetRawValue();
    impact.hostileGuid = hostile->GetObjectGuid().GetRawValue();
    impact.threatPoints = threatPoints;
    impact.source = ThreatImpactSource::PeriodicDamage;
    impact.bound = ThreatImpactBound::Exact;
    estimate.impacts.push_back(impact);
    return estimate;
}

} // namespace PartyBotEngine

#endif
