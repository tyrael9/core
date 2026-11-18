#ifndef MANGOS_PARTYBOT_THREAT_BUDGET_POLICY_H
#define MANGOS_PARTYBOT_THREAT_BUDGET_POLICY_H

#include "ThreatBudget.h"
#include "ThreatImpact.h"
#include "PartyCoordinator.h"

#include <cmath>
#include <map>
#include <utility>

namespace PartyBotEngine {

enum class ThreatImpactBudgetStatus : std::uint8_t {
    Unknown,
    Safe,
    Exceeded
};

struct ThreatImpactBudgetResult {
    ThreatImpactBudgetStatus status = ThreatImpactBudgetStatus::Unknown;
    Guid hostileGuid = 0;
    Guid threatOwnerGuid = 0;
    float rawHeadroom = 0.0f;
    float alreadyReserved = 0.0f;
    double candidateThreat = 0.0;
    double projectedThreat = 0.0;
    std::string reason = "threat_budget_not_assessed";
};

using ThreatImpactKey = std::pair<Guid, Guid>;
using ThreatImpactTotals = std::map<ThreatImpactKey, double>;

// Assess a complete, epoch-matched action estimate against the real Core
// snapshot projection plus in-flight coordinator reservations and any higher
// priority candidates already admitted in this decision report.
//
// This remains a limited headroom veto, not a copy of Core's victim selector.
// If any recipient cannot be resolved to a valid same-party snapshot member
// and online threat reference, the whole action is Unknown and must fail closed.
inline ThreatImpactBudgetResult AssessThreatImpactBudget(
    PartySnapshot const& snapshot,
    ThreatImpactEstimate const& estimate,
    PartyCoordinator& coordinator,
    std::uint32_t nowMs,
    std::uint32_t maxSnapshotAgeMs,
    ThreatImpactTotals* alreadySelectedInReport = nullptr)
{
    ThreatImpactBudgetResult result;

    if (!IsThreatImpactEstimateForSnapshot(estimate, snapshot.epoch)) {
        result.reason = estimate.completeness == ThreatImpactCompleteness::Unknown
            ? "threat_impact_estimate_unknown"
            : "threat_impact_estimate_invalid_or_stale";
        return result;
    }

    if (estimate.completeness == ThreatImpactCompleteness::Unknown) {
        result.reason = "threat_impact_estimate_unknown";
        return result;
    }

    if (estimate.completeness == ThreatImpactCompleteness::NoThreat) {
        result.status = ThreatImpactBudgetStatus::Safe;
        result.reason = "action_has_no_threat_impact";
        return result;
    }

    ThreatImpactTotals candidateTotals;
    for (ThreatImpactEntry const& impact : estimate.impacts) {
        if (impact.threatPoints <= 0.0f)
            continue;

        ThreatImpactKey const key(impact.hostileGuid, impact.threatOwnerGuid);
        candidateTotals[key] += static_cast<double>(impact.threatPoints);
        if (!std::isfinite(candidateTotals[key])) {
            result.reason = "candidate_threat_sum_not_finite";
            return result;
        }
    }

    // A Complete estimate containing only zero-valued rows is explicit and safe.
    if (candidateTotals.empty()) {
        result.status = ThreatImpactBudgetStatus::Safe;
        result.reason = "complete_estimate_has_no_positive_threat";
        return result;
    }

    // Check every hostile/actual-recipient pair before modifying the local
    // decision totals. A single unknown or unsafe fan-out target rejects all.
    for (auto const& entry : candidateTotals) {
        Guid const hostileGuid = entry.first.first;
        Guid const threatOwnerGuid = entry.first.second;
        ThreatBudgetAssessment const budget = AssessThreatBudget(
            snapshot, hostileGuid, threatOwnerGuid, nowMs, maxSnapshotAgeMs);

        if (budget.status == ThreatBudgetStatus::CurrentVictim)
            continue;

        if (budget.status == ThreatBudgetStatus::AtOrOverBudget) {
            result.status = ThreatImpactBudgetStatus::Exceeded;
            result.hostileGuid = hostileGuid;
            result.threatOwnerGuid = threatOwnerGuid;
            result.rawHeadroom = budget.rawHeadroom;
            result.reason = "member_already_at_or_over_core_threshold";
            return result;
        }

        if (budget.status != ThreatBudgetStatus::BudgetAvailable) {
            result.hostileGuid = hostileGuid;
            result.threatOwnerGuid = threatOwnerGuid;
            result.reason = "threat_budget_snapshot_unknown";
            return result;
        }

        float const reserved = coordinator.GetReservedThreatImpact(
            hostileGuid, threatOwnerGuid, 0, nowMs);
        double const localReserved = alreadySelectedInReport
            ? (*alreadySelectedInReport)[entry.first]
            : 0.0;
        double const projected = static_cast<double>(reserved) +
            localReserved + entry.second;

        result.hostileGuid = hostileGuid;
        result.threatOwnerGuid = threatOwnerGuid;
        result.rawHeadroom = budget.rawHeadroom;
        result.alreadyReserved = reserved;
        result.candidateThreat = entry.second;
        result.projectedThreat = projected;

        if (!std::isfinite(projected) ||
            projected > static_cast<double>(budget.rawHeadroom)) {
            result.status = ThreatImpactBudgetStatus::Exceeded;
            result.reason = "projected_threat_exceeds_core_headroom";
            return result;
        }
    }

    if (alreadySelectedInReport) {
        for (auto const& entry : candidateTotals)
            (*alreadySelectedInReport)[entry.first] += entry.second;
    }

    result.status = ThreatImpactBudgetStatus::Safe;
    result.reason = "projected_threat_within_core_headroom";
    return result;
}

} // namespace PartyBotEngine

#endif
