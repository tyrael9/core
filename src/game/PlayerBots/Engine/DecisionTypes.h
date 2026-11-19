#ifndef MANGOS_PARTYBOT_DECISION_TYPES_H
#define MANGOS_PARTYBOT_DECISION_TYPES_H
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace PartyBotEngine {
using Guid = std::uint64_t;
using ThreatActionId = std::uint64_t;

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
    // Filled by ExecuteSelected before dispatch so asynchronous action
    // completion hooks can release every reservation belonging to the action.
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
    ThreatActionId threatActionId = 0;
    std::string reason;
};
struct DecisionReport {
    std::uint32_t snapshotEpoch = 0;
    std::vector<CandidateEvaluation> candidates;
    std::vector<std::size_t> selectedIndices;
};
}
#endif
