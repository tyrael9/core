#ifndef MANGOS_PARTYBOT_CORE_ACTION_H
#define MANGOS_PARTYBOT_CORE_ACTION_H

#include "Action.h"
#include "CoreActionRuntime.h"

namespace PartyBotEngine
{
// Every production Core adapter derives from CoreAction. The final execution
// wrapper resolves actor/target/map on the owning map thread, then requires a
// concrete action-specific live-state preflight before ExecuteCore can perform
// any Core side effect. Pending actions pass through this same boundary again.
class CoreAction : public Action
{
public:
    ActionResult Execute(
        DecisionContext& context, ActionCandidate const& candidate) final
    {
        return ExecuteDetailed(context, candidate).result;
    }

    ActionExecutionResult ExecuteDetailed(
        DecisionContext& context, ActionCandidate const& candidate) final
    {
        if (!candidate.actionId)
            return Reject(ActionResult::Impossible, "core_action_missing_action_id");
        if (!context.coreActionRuntime)
            return Reject(ActionResult::Impossible, "core_action_runtime_missing");

        CoreActionResolution const resolved =
            context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid);
        if (!resolved.IsResolved() || !resolved.actor || !resolved.map ||
            (candidate.targetGuid && !resolved.target)) {
            char const* reason = resolved.reason
                ? resolved.reason : "core_action_context_unresolved";
            return Reject(ActionResult::Impossible, reason);
        }

        ActionExecutionResult const preflight =
            ValidateCoreAction(context, candidate, resolved);
        if (preflight.result != ActionResult::Success) {
            if (preflight.reason.empty())
                return Reject(preflight.result, "core_action_action_specific_preflight_rejected");
            return preflight;
        }

        ActionExecutionResult execution =
            ExecuteCore(context, candidate, resolved);
        if (execution.reason.empty()) {
            execution.reason = execution.result == ActionResult::Success
                ? "core_action_side_effect_started"
                : "core_action_side_effect_failed";
        }
        return execution;
    }

protected:
    virtual ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const = 0;

    virtual ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) = 0;

private:
    static ActionExecutionResult Reject(ActionResult result, char const* reason)
    {
        ActionExecutionResult rejected;
        rejected.result = result;
        rejected.reason = reason;
        return rejected;
    }
};
}

#endif
