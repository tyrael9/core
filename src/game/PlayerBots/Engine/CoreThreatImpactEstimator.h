#ifndef MANGOS_PARTYBOT_CORE_THREAT_IMPACT_ESTIMATOR_H
#define MANGOS_PARTYBOT_CORE_THREAT_IMPACT_ESTIMATOR_H

#include "Creature.h"
#include "PlayerBots/Engine/ThreatImpact.h"
#include "SpellMgr.h"
#include "ThreatManager.h"

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

} // namespace PartyBotEngine

#endif
