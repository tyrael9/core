#ifndef MANGOS_PARTYBOT_CORE_ACTION_H
#define MANGOS_PARTYBOT_CORE_ACTION_H

#include "Action.h"
#include "CoreActionRuntime.h"

namespace PartyBotEngine
{
// Every production Core adapter should derive from CoreAction. The final
// Execute wrapper requires a current map-thread actor/target resolution on
// every dispatch, including pending actions. Derived adapters then recheck
// action-specific state (alive/hostility/range/LOS/resources/cooldowns) in
// ExecuteCore immediately before invoking the Core side effect.
class CoreAction : public Action
{
public:
    ActionResult Execute(
        DecisionContext& context, ActionCandidate const& candidate) final
    {
        if (!candidate.actionId || !context.coreActionRuntime)
            return ActionResult::Impossible;

        CoreActionResolution const resolved =
            context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid);
        if (!resolved.IsResolved() || !resolved.actor || !resolved.map ||
            (candidate.targetGuid && !resolved.target))
            return ActionResult::Impossible;

        return ExecuteCore(context, candidate, resolved);
    }

protected:
    virtual ActionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) = 0;
};
}

#endif
