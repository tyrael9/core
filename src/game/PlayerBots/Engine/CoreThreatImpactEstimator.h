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

    return estimate;
}

} // namespace PartyBotEngine

#endif
