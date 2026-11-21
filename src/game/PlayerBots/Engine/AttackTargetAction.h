#ifndef MANGOS_PARTYBOT_ATTACK_TARGET_ACTION_H
#define MANGOS_PARTYBOT_ATTACK_TARGET_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include <cmath>

namespace PartyBotEngine
{
// Start or retarget Core auto-attack against the hostile already selected by
// the Player. This is a synchronous request only: subsequent swings and their
// damage/threat remain owned by Core's normal combat update path.
//
// This action deliberately calls Player::Attack through the CoreAction
// resolver. It does not call PartyBotAI::AttackStart or any Legacy policy path.
class AttackTargetAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "attack_target_request_has_no_synchronous_damage_or_threat";
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
            context.bot.selectionGuid != candidate.targetGuid ||
            !hostile || !hostile->alive)
        {
            reason = "attack_target_not_selected_live_hostile_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!candidate.targetGuid ||
            context.bot.victimGuid == candidate.targetGuid)
        {
            reason = "attack_target_already_current_victim";
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
        ActionExecutionResult result = CheckLiveState(context, candidate, resolved);
        if (result.result != ActionResult::Success)
            return result;

        Player* actor = resolved.actor;
        Unit* target = resolved.target;
        ObjectGuid const expectedTarget(candidate.targetGuid);
        uint32 const targetHealthBefore = target->GetHealth();

        Creature* targetCreature = target->ToCreature();
        bool const tracksThreat = targetCreature && targetCreature->CanHaveThreatList();
        float const threatBefore = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;

        // Do not route through PartyBotAI::AttackStart: that method belongs to
        // Legacy and also performs Legacy-specific buff/trace bookkeeping.
        bool const accepted = actor->Attack(target, true);
        float const threatAfter = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;
        bool const noImmediateEffect =
            target->GetHealth() == targetHealthBefore &&
            (!tracksThreat || std::fabs(threatAfter - threatBefore) <= 0.001f);
        bool const selectedTargetPreserved =
            actor->GetSelectionGuid() == expectedTarget &&
            actor->GetTargetGuid() == expectedTarget;

        bool const started = accepted && actor->GetVictim() == target &&
            selectedTargetPreserved && noImmediateEffect;
        result.result = started ? ActionResult::Success : ActionResult::Failed;
        result.reason = started
            ? "attack_target_auto_attack_request_accepted"
            : "attack_target_core_attack_postcondition_failed";
        return result;
    }

private:
    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Unit* target = resolved.target;
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);

        if (!actor || !target || !resolved.map || !hostile || !hostile->alive ||
            !candidate.targetGuid ||
            target->GetObjectGuid().GetRawValue() != candidate.targetGuid ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !target->IsInWorld() || !target->IsAlive() ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "attack_target_live_context_invalid";
            return result;
        }

        ObjectGuid const expectedTarget(candidate.targetGuid);
        if (actor->GetSelectionGuid() != expectedTarget ||
            actor->GetTargetGuid() != expectedTarget)
        {
            result.result = ActionResult::Impossible;
            result.reason = "attack_target_live_selection_mismatch";
            return result;
        }

        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "attack_target_not_attackable";
            return result;
        }

        if (actor->HasUnitState(
                UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        {
            result.result = ActionResult::Useless;
            result.reason = "attack_target_actor_cannot_attack";
            return result;
        }

        if (actor->GetVictim() == target)
        {
            result.result = ActionResult::Useless;
            result.reason = "attack_target_already_current_victim";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "attack_target_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_attack_target";
};
} // namespace PartyBotEngine

#endif
