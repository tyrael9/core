#ifndef MANGOS_PARTYBOT_DECISION_TYPES_H
#define MANGOS_PARTYBOT_DECISION_TYPES_H
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include <limits>
namespace PartyBotEngine {
using Guid = std::uint64_t;
using ActionId = std::uint64_t;
// Kept as an alias for existing threat-reservation adapter APIs.
using ThreatActionId = ActionId;

enum class ThreatImpactCompleteness : std::uint8_t {
    Unknown,
    Complete,
    NoThreat
};
enum class ThreatImpactSource : std::uint8_t {
    DirectDamage,
    PeriodicDamage,
    DirectHealAssist,
    PeriodicHealAssist,
    PeriodicPowerAssist,
    PeriodicManaLeech,
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
    // Pending threat is kept reserved until this bounded duration expires or
    // the owning action is explicitly released.
    std::uint32_t reservationDurationMs = 0;
    std::vector<ThreatImpactEntry> impacts;
    std::string reason = "threat_impact_not_estimated";
};
enum class PartyRole : std::uint8_t { Unknown, Tank, MainTank, Healer, RangedDamage, MeleeDamage, Support };
enum class ActionChannel : std::uint8_t { GlobalCooldown, Movement, Targeting, Utility };
enum class ActionResult : std::uint8_t { NotAttempted, Success, Impossible, Useless, Failed, Skipped };
enum class CandidateStatus : std::uint8_t {
    RejectedMissingAction,
    RejectedImpossible,
    RejectedUseless,
    RejectedMultiplier,
    RejectedInvalidPriority,
    RejectedThreatUnknown,
    RejectedThreatBudget,
    NotSelected,
    Selected,
    Executed,
    ExecutionFailed
};
enum class ReservationType : std::uint8_t { Heal, CrowdControl, Interrupt, Target, Dispel, Resurrection };
enum class TriggerEvaluationStatus : std::uint8_t {
    CadenceSkipped,
    Inactive,
    Active
};
struct TriggerEvaluation {
    std::string strategyName;
    std::string triggerName;
    std::uint32_t intervalMs = 0;
    std::uint32_t candidateCount = 0;
    TriggerEvaluationStatus status = TriggerEvaluationStatus::CadenceSkipped;
};
struct ActionCandidate {
    std::string strategyName;
    std::string triggerName;
    std::string actionName;
    Guid targetGuid = 0;
    std::uint32_t spellId = 0;
    // Assigned by ExecuteSelected before adapter dispatch; remains available
    // to asynchronous action/result listeners even when the action reserves no threat.
    ActionId actionId = 0;
    // Nonzero only when this action owns one or more in-flight threat rows.
    ThreatActionId threatActionId = 0;
    float basePriority = 0.0f;
    float finalPriority = 0.0f;
    ActionChannel channel = ActionChannel::GlobalCooldown;
    float destinationX = 0.0f, destinationY = 0.0f, destinationZ = 0.0f;
    bool hasDestination = false;
};
struct CandidateEvaluation {
    ActionCandidate candidate;
    CandidateStatus status = CandidateStatus::NotSelected;
    ActionResult actionResult = ActionResult::NotAttempted;
    ThreatImpactEstimate threatImpact;
    ActionId actionId = 0;
    ThreatActionId threatActionId = 0;
    std::string reason;
};
struct DecisionReport {
    static std::size_t const MaxTriggerEvaluations = 64;
    static std::size_t const MaxCandidateTraceRows = 3;
    std::uint32_t snapshotEpoch = 0;
    std::vector<TriggerEvaluation> triggerEvaluations;
    std::uint32_t omittedTriggerEvaluations = 0;
    std::vector<CandidateEvaluation> candidates;
    std::vector<std::size_t> selectedIndices;
};

// Bounded diagnostic sampling is prioritised independently of registration
// order: candidate-producing active triggers first, then other active
// triggers, inactive triggers and finally cadence-skipped triggers.
// Candidate count breaks ties among actionable triggers; stable_sort preserves
// registration order for all remaining ties.
inline std::uint32_t TriggerTracePriority(TriggerEvaluation const& evaluation) {
    if (evaluation.status == TriggerEvaluationStatus::Active)
        return evaluation.candidateCount ? 0u : 1u;
    if (evaluation.status == TriggerEvaluationStatus::Inactive)
        return 2u;
    return 3u;
}

inline std::vector<std::size_t> SelectPriorityTriggerEvaluationIndices(
    DecisionReport const& report, std::size_t maxRows) {
    std::vector<std::size_t> indices;
    if (!maxRows)
        return indices;
    indices.reserve(report.triggerEvaluations.size());
    for (std::size_t i = 0; i < report.triggerEvaluations.size(); ++i)
        indices.push_back(i);
    std::stable_sort(indices.begin(), indices.end(),
        [&report](std::size_t left, std::size_t right) {
            TriggerEvaluation const& a = report.triggerEvaluations[left];
            TriggerEvaluation const& b = report.triggerEvaluations[right];
            std::uint32_t const aPriority = TriggerTracePriority(a);
            std::uint32_t const bPriority = TriggerTracePriority(b);
            if (aPriority != bPriority)
                return aPriority < bPriority;
            if (aPriority == 0 && a.candidateCount != b.candidateCount)
                return a.candidateCount > b.candidateCount;
            return false;
        });
    if (indices.size() > maxRows)
        indices.resize(maxRows);
    return indices;
}

inline std::vector<std::size_t> SelectPriorityCandidateEvaluationIndices(
    DecisionReport const& report, std::size_t maxRows) {
    std::vector<std::size_t> indices;
    if (!maxRows)
        return indices;

    // selectedIndices remains authoritative after ExecuteSelected rewrites
    // Selected -> Executed/ExecutionFailed.
    std::vector<std::uint8_t> selected(report.candidates.size(), 0);
    for (std::size_t index : report.selectedIndices)
        if (index < selected.size())
            selected[index] = 1;

    indices.reserve(report.candidates.size());
    for (std::size_t i = 0; i < report.candidates.size(); ++i)
        indices.push_back(i);

    auto priority = [&report](std::size_t index) {
        float const value = report.candidates[index].candidate.finalPriority;
        return std::isfinite(value) ? value : -std::numeric_limits<float>::max();
    };
    std::stable_sort(indices.begin(), indices.end(),
        [&selected, &priority](std::size_t left, std::size_t right) {
            bool const leftSelected = selected[left] != 0;
            bool const rightSelected = selected[right] != 0;
            if (leftSelected != rightSelected)
                return leftSelected;
            return priority(left) > priority(right);
        });
    if (indices.size() > maxRows)
        indices.resize(maxRows);
    return indices;
}

struct DecisionSummary {
    std::uint32_t candidateCount = 0;
    std::uint32_t candidateDetailLimit = 0;
    std::uint32_t candidateDetailOmitted = 0;
    // Count arbitration winners from selectedIndices, even after execution
    // changes their CandidateStatus to Executed or ExecutionFailed.
    std::uint32_t selectedCount = 0;
    std::uint32_t channelLoserCount = 0;
    std::uint32_t missingActionCount = 0;
    std::uint32_t impossibleCount = 0;
    std::uint32_t uselessCount = 0;
    std::uint32_t multiplierVetoCount = 0;
    std::uint32_t invalidPriorityCount = 0;
    std::uint32_t threatUnknownCount = 0;
    std::uint32_t threatBudgetVetoCount = 0;
    std::uint32_t executedCount = 0;
    std::uint32_t executionFailedCount = 0;
};

inline std::uint32_t BoundedDecisionCount(std::size_t count) {
    return count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(count);
}

inline void IncrementDecisionSummaryCount(std::uint32_t& count) {
    if (count != std::numeric_limits<std::uint32_t>::max())
        ++count;
}

inline DecisionSummary SummarizeDecisionReport(DecisionReport const& report) {
    DecisionSummary summary;
    summary.candidateCount = BoundedDecisionCount(report.candidates.size());
    std::vector<std::size_t> const candidateDetailIndices =
        SelectPriorityCandidateEvaluationIndices(
            report, DecisionReport::MaxCandidateTraceRows);
    summary.candidateDetailLimit = BoundedDecisionCount(
        DecisionReport::MaxCandidateTraceRows);
    summary.candidateDetailOmitted = BoundedDecisionCount(
        report.candidates.size() - candidateDetailIndices.size());
    summary.selectedCount = BoundedDecisionCount(report.selectedIndices.size());
    for (CandidateEvaluation const& evaluation : report.candidates) {
        switch (evaluation.status) {
            case CandidateStatus::RejectedMissingAction:
                IncrementDecisionSummaryCount(summary.missingActionCount);
                break;
            case CandidateStatus::RejectedImpossible:
                IncrementDecisionSummaryCount(summary.impossibleCount);
                break;
            case CandidateStatus::RejectedUseless:
                IncrementDecisionSummaryCount(summary.uselessCount);
                break;
            case CandidateStatus::RejectedMultiplier:
                IncrementDecisionSummaryCount(summary.multiplierVetoCount);
                break;
            case CandidateStatus::RejectedInvalidPriority:
                IncrementDecisionSummaryCount(summary.invalidPriorityCount);
                break;
            case CandidateStatus::RejectedThreatUnknown:
                IncrementDecisionSummaryCount(summary.threatUnknownCount);
                break;
            case CandidateStatus::RejectedThreatBudget:
                IncrementDecisionSummaryCount(summary.threatBudgetVetoCount);
                break;
            case CandidateStatus::NotSelected:
                if (evaluation.reason == "lower_priority_same_channel")
                    IncrementDecisionSummaryCount(summary.channelLoserCount);
                break;
            case CandidateStatus::Executed:
                IncrementDecisionSummaryCount(summary.executedCount);
                break;
            case CandidateStatus::ExecutionFailed:
                IncrementDecisionSummaryCount(summary.executionFailedCount);
                break;
            case CandidateStatus::Selected:
                break;
        }
    }
    return summary;
}
}
#endif
