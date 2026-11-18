#ifndef MANGOS_PARTYBOT_THREAT_IMPACT_H
#define MANGOS_PARTYBOT_THREAT_IMPACT_H

#include "DecisionTypes.h"
#include <cmath>

namespace PartyBotEngine {

// Threat impact value types live in DecisionTypes.h so DecisionReport can retain
// the exact estimate used during evaluation and revalidate it before execution.

inline bool IsValidThreatImpactSource(ThreatImpactSource source)
{
    switch (source) {
        case ThreatImpactSource::DirectDamage:
        case ThreatImpactSource::PeriodicDamage:
        case ThreatImpactSource::DirectHealAssist:
        case ThreatImpactSource::PeriodicHealAssist:
        case ThreatImpactSource::PeriodicPowerAssist:
        case ThreatImpactSource::FlatSpellThreat:
            return true;
    }
    return false;
}

inline bool IsValidThreatImpactBound(ThreatImpactBound bound)
{
    switch (bound) {
        case ThreatImpactBound::Exact:
        case ThreatImpactBound::UpperBound:
            return true;
    }
    return false;
}

// Validate the value contract without consulting live Core objects. A Complete
// estimate must enumerate at least one impact; NoThreat is the only explicit
// complete empty result. Unknown estimates may contain partial known rows, but
// must explain why the complete impact set is unavailable.
inline bool IsWellFormedThreatImpactEstimate(ThreatImpactEstimate const& estimate)
{
    if (!estimate.snapshotEpoch || estimate.reason.empty())
        return false;

    if (estimate.completeness == ThreatImpactCompleteness::NoThreat)
        return estimate.impacts.empty();

    if (estimate.completeness == ThreatImpactCompleteness::Complete &&
        estimate.impacts.empty())
        return false;

    if (estimate.completeness != ThreatImpactCompleteness::Unknown &&
        estimate.completeness != ThreatImpactCompleteness::Complete)
        return false;

    bool hasPositiveImpact = false;
    for (ThreatImpactEntry const& impact : estimate.impacts) {
        if (!impact.threatOwnerGuid || !impact.hostileGuid ||
            !std::isfinite(impact.threatPoints) || impact.threatPoints < 0.0f ||
            !IsValidThreatImpactSource(impact.source) ||
            !IsValidThreatImpactBound(impact.bound))
            return false;
        hasPositiveImpact = hasPositiveImpact || impact.threatPoints > 0.0f;
    }

    // A positive threat estimate must declare how long other bots should treat
    // it as in-flight. Keep all deadline deltas within signed uint32 wrap range.
    if (estimate.completeness == ThreatImpactCompleteness::Complete &&
        hasPositiveImpact &&
        (estimate.reservationDurationMs == 0 ||
         estimate.reservationDurationMs > 0x7fffffffu))
        return false;

    return true;
}

// An estimate is usable only for the exact immutable snapshot from which it
// was produced. Callers must treat a mismatched epoch as Unknown, not reuse it.
inline bool IsThreatImpactEstimateForSnapshot(ThreatImpactEstimate const& estimate,
                                               std::uint32_t snapshotEpoch)
{
    return snapshotEpoch && estimate.snapshotEpoch == snapshotEpoch &&
        IsWellFormedThreatImpactEstimate(estimate);
}

} // namespace PartyBotEngine

#endif
