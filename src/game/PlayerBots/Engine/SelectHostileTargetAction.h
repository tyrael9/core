#ifndef MANGOS_PARTYBOT_SELECT_HOSTILE_TARGET_ACTION_H
#define MANGOS_PARTYBOT_SELECT_HOSTILE_TARGET_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Select a currently attackable hostile for follow-up actions. This action
// changes only Player selection/target GUID fields; it does not initiate combat,
// move, cast, damage, or generate threat. The caller should use Targeting channel.
class SelectHostileTargetAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "player_selection_only_no_combat_or_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        if (!context.bot.alive || !candidate.targetGuid ||
            candidate.targetGuid == context.bot.guid ||
            !hostile || !hostile->alive)
        {
            reason = "select_hostile_target_missing_or_not_in_hostile_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!candidate.targetGuid ||
            context.bot.selectionGuid == candidate.targetGuid)
        {
            reason = "select_hostile_target_already_selected";
            return false;
        }
        return true;
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const override
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Unit* target = resolved.target;
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);

        if (!actor || !target || !candidate.targetGuid ||
            target->GetObjectGuid().GetRawValue() != candidate.targetGuid ||
            !hostile || !hostile->alive ||
            !actor->IsAlive() || !target->IsAlive())
        {
            result.result = ActionResult::Impossible;
            result.reason = "select_hostile_target_live_context_invalid";
            return result;
        }

        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "select_hostile_target_not_attackable";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "select_hostile_target_live_preflight_passed";
        return result;
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext&, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Unit* target = resolved.target;
        if (!actor || !target ||
            target->GetObjectGuid().GetRawValue() != candidate.targetGuid)
        {
            result.result = ActionResult::Impossible;
            result.reason = "select_hostile_target_context_lost_before_dispatch";
            return result;
        }

        actor->SetSelectionGuid(target->GetObjectGuid());
        bool const selected =
            actor->GetSelectionGuid() == target->GetObjectGuid() &&
            actor->GetTargetGuid() == target->GetObjectGuid();
        result.result = selected ? ActionResult::Success : ActionResult::Failed;
        result.reason = selected
            ? "select_hostile_target_selected"
            : "select_hostile_target_selection_update_failed";
        return result;
    }

private:
    std::string m_name = "core_select_hostile_target";
};
} // namespace PartyBotEngine

#endif
