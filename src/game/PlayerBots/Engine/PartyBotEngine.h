#ifndef MANGOS_PARTYBOT_PARTYBOT_ENGINE_H
#define MANGOS_PARTYBOT_PARTYBOT_ENGINE_H
#include "Action.h"
#include "ActionResultListener.h"
#include "CoreSpellTerminalListener.h"
#include "ActionContinuationQueue.h"
#include "ActionQueue.h"
#include "Multiplier.h"
#include "Strategy.h"
#include "TriggerScheduler.h"
#include "ValueCache.h"
#include <map>
#include <memory>
#include <vector>
namespace PartyBotEngine {
struct PendingActionDispatchSummary
{
    std::size_t expired = 0;
    std::size_t expiredContinuations = 0;
    std::size_t dequeued = 0;
    std::size_t staleContext = 0;
    std::size_t rejected = 0;
    std::size_t requeued = 0;
    std::size_t queueFull = 0;
    std::size_t selected = 0;
    std::size_t executed = 0;
    std::size_t failed = 0;
    std::size_t newlyQueued = 0;
    std::vector<CandidateEvaluation> completedCandidates;
};

class PartyBotEngine : public ActionContinuationSink {
public:
    void AddStrategy(std::shared_ptr<Strategy> strategy);
    bool RegisterAction(std::shared_ptr<Action> action);
    bool AddActionResultListener(std::shared_ptr<ActionResultListener> listener);
    bool AddCoreSpellTerminalListener(std::shared_ptr<CoreSpellTerminalListener> listener);
    std::size_t GetCoreSpellTerminalListenerCount() const { return m_coreSpellTerminalListeners.size(); }
    void NotifyCoreSpellTerminal(CoreSpellTerminalEvent const& event) const;
    ActionContinuationEnqueueResult QueueActionDependency(
        DecisionContext const& context, ActionDependencyRequest const& request,
        ActionCandidate const& continuationCandidate) const override;
    ActionContinuationEnqueueResult QueueActionContinuation(
        DecisionContext const& context, ActionCandidate const& parentCandidate,
        ActionCandidate const& continuationCandidate, std::uint32_t lifetimeMs) const override;
    std::size_t GetPendingContinuationCount() const { return m_actionContinuationQueue.Size(); }
    std::size_t GetActionResultListenerCount() const {
        return m_actionResultListeners.size();
    }
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
        DecisionContext const& context, DecisionReport& report) const;
    PendingActionDispatchSummary ExecutePendingActions(
        DecisionContext& context, std::size_t maxActionsPerDispatch = 8,
        std::uint32_t blockedChannelMask = 0) const;
    PendingActionDispatchSummary ExecuteDecisionCycle(
        DecisionContext& context, DecisionReport& report) const;
    std::size_t GetPendingActionCount() const { return m_pendingActionQueue.Size(); }
    void ClearPendingActions() const { m_pendingActionQueue.Clear(); m_actionContinuationQueue.Clear(); m_recentCoreSpellTerminals.clear(); m_recentActionResults.clear(); }
    // Only call when Strategy mode owns the relevant decision domain.
    void ExecuteSelected(DecisionContext& context, DecisionReport& report) const;
private:
    CandidateEvaluation EvaluateCandidate(
        DecisionContext const& context, ActionCandidate candidate) const;
    void NotifyActionResult(DecisionContext const& context,
                            CandidateEvaluation const& evaluation,
                            ActionId actionId, bool attempted,
                            ActionResult result, char const* reason) const;
    std::shared_ptr<Action> FindAction(std::string const& name) const;
    bool ReleaseContinuation(ActionContinuationQueue::Entry const& entry,
                             std::uint32_t nowMs) const;
    bool ReleaseReadyActionDependencies(
        std::uint32_t nowMs, ActionCandidate const* registeringCandidate = nullptr) const;
    void RememberCoreSpellTerminal(CoreSpellTerminalEvent const& event) const;
    void RememberActionResult(ActionResultEvent const& event) const;
    std::vector<std::shared_ptr<Strategy>> m_strategies;
    std::map<std::string, std::shared_ptr<Action>> m_actions;
    std::vector<std::shared_ptr<Multiplier>> m_multipliers;
    std::vector<std::shared_ptr<ActionResultListener>> m_actionResultListeners;
    std::vector<std::shared_ptr<CoreSpellTerminalListener>> m_coreSpellTerminalListeners;
    mutable ValueCache m_valueCache;
    mutable TriggerScheduler m_triggerScheduler;
    mutable PendingActionQueue m_pendingActionQueue;
    mutable ActionContinuationQueue m_actionContinuationQueue;
    mutable std::vector<CoreSpellTerminalEvent> m_recentCoreSpellTerminals;
    mutable std::vector<ActionResultEvent> m_recentActionResults;
};
}
#endif
