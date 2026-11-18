#ifndef MANGOS_PARTYBOT_THREAT_IMPACT_H
#define MANGOS_PARTYBOT_THREAT_IMPACT_H

#include "DecisionTypes.h"
#include <cmath>

namespace PartyBotEngine {

// Completeness describes the whole action's affected-hostile set. Unknown may
// still carry known partial rows; those rows must never imply completeness.
enum class ThreatImpactCompleteness : std::uint8_t {
    Unknown,
    Complete,
    NoThreat
};

enum class ThreatImpactSource : std::uint8_t {
    DirectDamage,
    DirectHealAssist,
    PeriodicHealAssist,
    FlatSpellThreat
};

enum class ThreatImpactBound : std::uint8_t {
    Exact,
    UpperBound
};

struct ThreatImpactEntry {
    Guid threatOwnerGuid = 0;
    Guid hostileGuid = 0;
    float threatPoints = 0.0f;
    ThreatImpactSource source = ThreatImpactSource::DirectDamage;
    ThreatImpactBound bound = ThreatImpactBound::UpperBound;
};

struct ThreatImpactEstimate {
    ThreatImpactCompleteness completeness = ThreatImpactCompleteness::Unknown;
    std::uint32_t snapshotEpoch = 0;
    std::vector<ThreatImpactEntry> impacts;
    std::string reason = "threat_impact_not_estimated";
};

// Validate the data contract without consulting live Core objects. A Complete
// estimate must enumerate at least one impact; NoThreat is the only explicit
// complete empty result. Unknown estimates may contain partial known rows.
inline bool IsWellFormedThreatImpactEstimate(ThreatImpactEstimate const& estimate)
{
    if (estimate.completeness == ThreatImpactCompleteness::NoThreat)
        return estimate.impacts.empty();

    if (estimate.completeness == ThreatImpactCompleteness::Complete &&
        estimate.impacts.empty())
        return false;

    for (ThreatImpactEntry const& impact : estimate.impacts) {
        if (!impact.threatOwnerGuid || !impact.hostileGuid ||
            !std::isfinite(impact.threatPoints) || impact.threatPoints < 0.0f)
            return false;
    }
    return true;
}

} // namespace PartyBotEngine

#endif
