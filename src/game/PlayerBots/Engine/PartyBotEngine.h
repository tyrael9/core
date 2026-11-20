#ifndef MANGOS_PARTYBOT_PARTYBOT_ENGINE_H
#define MANGOS_PARTYBOT_PARTYBOT_ENGINE_H
#include "Action.h"
#include "ActionQueue.h"
#include "Multiplier.h"
#include "Strategy.h"
#include "TriggerScheduler.h"
#include "ValueCache.h"
#include <map>
#include <memory>
namespace PartyBotEngine {
struct PendingActionDispatchSummary
{
    std::size_t expired = 0;
    std::size_t dequeued = 0;
    std::size_t staleContext = 0;
    std::size_t rejected = 0;
    std::size_t requeued = 0;
    std::size_t queueFull = 0;
    std::size_t selected = 0;
    std::size_t executed = 0;
    std::size_t failed = 0;
};

class PartyBotEngine {
public:
    void AddStrategy(std::shared_ptr<Strategy> strategy);
    bool RegisterAction(std::shared_ptr<Action> action);
    void AddMultiplier(std::shared_ptr<Multiplier> multiplier);
    bool InvalidateValue(std::string const& name) const;
    void InvalidateAllValues() const;
    void InvalidateDecisionState() const;
    void ResetDecisionState() const;
    bool InvalidateTrigger(std::string const& strategyName,
                           std::string const& triggerName) const;
    void InvalidateAllTriggers() const;
    DecisionReport Evaluate(DecisionContext const& context) const;
    PendingActionEnqueueResult QueuePendingAction(
        DecisionContext const& context, ActionCandidate const& candidate) const;
    std::size_t QueueSelectedActions(
        DecisionContext const& context, DecisionReport const& report) const;
    PendingActionDispatchSummary ExecutePendingActions(
        DecisionContext& context, std::size_t maxActionsPerDispatch = 8) const;
    std::size_t GetPendingActionCount() const { return m_pendingActionQueue.Size(); }
    void ClearPendingActions() const { m_pendingActionQueue.Clear(); }
    // Only call when Strategy mode owns the relevant decision domain.
    void ExecuteSelected(DecisionContext& context, DecisionReport& report) const;
private:
    CandidateEvaluation EvaluateCandidate(
        DecisionContext const& context, ActionCandidate candidate) const;
    std::shared_ptr<Action> FindAction(std::string const& name) const;
    std::vector<std::shared_ptr<Strategy>> m_strategies;
    std::map<std::string, std::shared_ptr<Action>> m_actions;
    std::vector<std::shared_ptr<Multiplier>> m_multipliers;
    mutable ValueCache m_valueCache;
    mutable TriggerScheduler m_triggerScheduler;
    mutable PendingActionQueue m_pendingActionQueue;
};
}
#endif
