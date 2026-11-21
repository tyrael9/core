#ifndef MANGOS_PARTYBOT_STOP_FOLLOWING_ACTION_H
#define MANGOS_PARTYBOT_STOP_FOLLOWING_ACTION_H

#include "CoreAction.h"

#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "Movement/TargetedMovementGenerator.h"
#include "Player.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Stop a live native Player FollowMovementGenerator only when its target GUID
// still matches the value snapshot. MovementExpired(false) follows Core's
// immediate/deferred lifetime rules and preserves non-targeted movement.
class StopFollowingAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "stop_following_removes_movement_only";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        if (!context.bot.alive || candidate.targetGuid ||
            !context.bot.followTargetGuid)
        {
            reason = "stop_following_missing_live_follow_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (candidate.targetGuid || !context.bot.followTargetGuid)
        {
            reason = "stop_following_already_not_following";
            return false;
        }
        return true;
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const override
    {
        return CheckLiveState(context, candidate, resolved);
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result =
            CheckLiveState(context, candidate, resolved);
        if (result.result != ActionResult::Success)
            return result;

        Player* actor = resolved.actor;
        MotionMaster* motion = actor->GetMotionMaster();
        // Handles both direct expiration and safe deferred deletion while the
        // MotionMaster is inside UpdateMotion; false avoids resetting the base.
        motion->MovementExpired(false);

        bool followGeneratorRemains = false;
        for (MotionMaster::const_iterator it = motion->begin();
             it != motion->end(); ++it)
        {
            if ((*it)->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            {
                followGeneratorRemains = true;
                break;
            }
        }

        bool const followStateCleared =
            !actor->HasUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
        bool const stopped = !followGeneratorRemains && followStateCleared;
        result.result = stopped ? ActionResult::Success : ActionResult::Failed;
        result.reason = stopped
            ? "stop_following_stopped"
            : "stop_following_postcondition_failed";
        return result;
    }

private:
    static FollowMovementGenerator<Player> const* GetCurrentFollow(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster() ||
            actor->GetMotionMaster()->empty())
            return nullptr;

        MovementGenerator const* current =
            actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            return nullptr;

        return static_cast<FollowMovementGenerator<Player> const*>(current);
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !context.bot.followTargetGuid || !actor->IsInWorld() ||
            !actor->IsAlive() || actor->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_following_actor_or_snapshot_invalid";
            return result;
        }

        FollowMovementGenerator<Player> const* follow = GetCurrentFollow(actor);
        if (!follow)
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_following_not_currently_following";
            return result;
        }

        Unit* target = follow->GetTarget();
        if (!target || !target->IsInWorld() || target->GetMap() != resolved.map ||
            target->GetMapId() != context.snapshot.mapId ||
            target->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_following_live_target_unavailable";
            return result;
        }

        if (target->GetObjectGuid().GetRawValue() != context.bot.followTargetGuid)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_following_live_target_mismatch";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "stop_following_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_stop_following";
};
} // namespace PartyBotEngine

#endif
