#ifndef MANGOS_PARTYBOT_DECISION_TYPES_H
#define MANGOS_PARTYBOT_DECISION_TYPES_H
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace PartyBotEngine {
using Guid = std::uint64_t;
enum class ActionChannel : std::uint8_t { GlobalCooldown, Movement, Targeting, Utility };
enum class ActionResult : std::uint8_t { NotAttempted, Success, Impossible, Useless, Failed, Skipped };
enum class CandidateStatus : std::uint8_t { RejectedMissingAction, RejectedImpossible, RejectedUseless, RejectedMultiplier, RejectedInvalidPriority, NotSelected, Selected, Executed, ExecutionFailed };
enum class ReservationType : std::uint8_t { Heal, CrowdControl, Interrupt, Target, Dispel, Resurrection };
struct ActionCandidate {
    std::string actionName;
    Guid targetGuid = 0;
    std::uint32_t spellId = 0;
    float basePriority = 0.0f;
    float finalPriority = 0.0f;
    ActionChannel channel = ActionChannel::GlobalCooldown;
};
struct CandidateEvaluation {
    ActionCandidate candidate;
    CandidateStatus status = CandidateStatus::NotSelected;
    ActionResult actionResult = ActionResult::NotAttempted;
    std::string reason;
};
struct DecisionReport {
    std::uint32_t snapshotEpoch = 0;
    std::vector<CandidateEvaluation> candidates;
    std::vector<std::size_t> selectedIndices;
};
}
#endif
