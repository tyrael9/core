#include "PartyBotEngine.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
namespace PartyBotEngine {
void PartyBotEngine::AddStrategy(std::shared_ptr<Strategy> strategy) {
    if (strategy) m_strategies.push_back(std::move(strategy));
}
bool PartyBotEngine::RegisterAction(std::shared_ptr<Action> action) {
    if (!action || action->GetName().empty()) return false;
    std::string const name = action->GetName();
    return m_actions.insert(std::make_pair(name, std::move(action))).second;
}
void PartyBotEngine::AddMultiplier(std::shared_ptr<Multiplier> multiplier) {
    if (multiplier) m_multipliers.push_back(std::move(multiplier));
}
std::shared_ptr<Action> PartyBotEngine::FindAction(std::string const& name) const {
    auto itr = m_actions.find(name);
    return itr == m_actions.end() ? std::shared_ptr<Action>() : itr->second;
}
DecisionReport PartyBotEngine::Evaluate(DecisionContext const& context) const {
    DecisionReport report;
    report.snapshotEpoch = context.snapshot.epoch;
    for (auto const& strategy : m_strategies) {
        if (!strategy) continue;
        for (auto const& trigger : strategy->GetTriggers()) {
            if (!trigger || !trigger->IsActive(context)) continue;
            std::vector<ActionCandidate> candidates;
            trigger->CollectCandidates(context, candidates);
            for (auto const& candidate : candidates) {
                CandidateEvaluation evaluation;
                evaluation.candidate = candidate;
                std::shared_ptr<Action> action = FindAction(candidate.actionName);
                if (!action) {
                    evaluation.status = CandidateStatus::RejectedMissingAction;
                    evaluation.reason = "action_not_registered";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                if (!std::isfinite(candidate.basePriority)) {
                    evaluation.status = CandidateStatus::RejectedInvalidPriority;
                    evaluation.reason = "non_finite_base_priority";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                if (!action->IsPossible(context, candidate, evaluation.reason)) {
                    evaluation.status = CandidateStatus::RejectedImpossible;
                    if (evaluation.reason.empty()) evaluation.reason = "action_not_possible";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                evaluation.reason.clear();
                if (!action->IsUseful(context, candidate, evaluation.reason)) {
                    evaluation.status = CandidateStatus::RejectedUseless;
                    if (evaluation.reason.empty()) evaluation.reason = "action_not_useful";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                float priority = candidate.basePriority;
                bool vetoed = false;
                for (auto const& multiplier : m_multipliers) {
                    if (!multiplier) continue;
                    float const factor = multiplier->GetValue(context, candidate);
                    if (!std::isfinite(factor) || factor <= 0.0f) {
                        evaluation.status = CandidateStatus::RejectedMultiplier;
                        evaluation.reason = multiplier->GetName();
                        if (evaluation.reason.empty()) evaluation.reason = "multiplier_veto";
                        vetoed = true;
                        break;
                    }
                    priority *= factor;
                }
                if (vetoed) {
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                if (!std::isfinite(priority)) {
                    evaluation.status = CandidateStatus::RejectedInvalidPriority;
                    evaluation.reason = "non_finite_final_priority";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                evaluation.candidate.finalPriority = priority;
                evaluation.status = CandidateStatus::NotSelected;
                evaluation.reason.clear();
                report.candidates.push_back(std::move(evaluation));
            }
        }
    }
    std::vector<std::size_t> eligible;
    for (std::size_t i = 0; i < report.candidates.size(); ++i)
        if (report.candidates[i].status == CandidateStatus::NotSelected) eligible.push_back(i);
    std::stable_sort(eligible.begin(), eligible.end(), [&report](std::size_t a, std::size_t b) {
        return report.candidates[a].candidate.finalPriority > report.candidates[b].candidate.finalPriority;
    });
    std::set<ActionChannel> occupiedChannels;
    for (std::size_t index : eligible) {
        CandidateEvaluation& evaluation = report.candidates[index];
        if (!occupiedChannels.insert(evaluation.candidate.channel).second) {
            evaluation.reason = "lower_priority_same_channel";
            continue;
        }
        evaluation.status = CandidateStatus::Selected;
        report.selectedIndices.push_back(index);
    }
    return report;
}
void PartyBotEngine::ExecuteSelected(DecisionContext& context, DecisionReport& report) const {
    for (std::size_t index : report.selectedIndices) {
        if (index >= report.candidates.size()) continue;
        CandidateEvaluation& evaluation = report.candidates[index];
        if (evaluation.status != CandidateStatus::Selected) continue;
        std::shared_ptr<Action> action = FindAction(evaluation.candidate.actionName);
        if (!action) {
            evaluation.status = CandidateStatus::ExecutionFailed;
            evaluation.actionResult = ActionResult::Failed;
            evaluation.reason = "action_unregistered_before_execution";
            continue;
        }
        evaluation.actionResult = action->Execute(context, evaluation.candidate);
        if (evaluation.actionResult == ActionResult::Success)
            evaluation.status = CandidateStatus::Executed;
        else {
            evaluation.status = CandidateStatus::ExecutionFailed;
            if (evaluation.reason.empty()) evaluation.reason = "action_execution_not_successful";
        }
    }
}
}
