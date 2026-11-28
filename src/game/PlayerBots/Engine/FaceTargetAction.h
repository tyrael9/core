#ifndef MANGOS_PARTYBOT_FACE_TARGET_ACTION_H
#define MANGOS_PARTYBOT_FACE_TARGET_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include <cmath>

namespace PartyBotEngine
{
// Face the currently selected hostile using Core's native facing-only spline.
// This adapter never changes selection, attacks, casts, translates the actor,
// damages or generates threat; the short facing spline is advanced by Core.
class FaceTargetAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "face_target_orientation_only_no_threat";
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
            !hostile || !hostile->alive || !hostile->positionValid ||
            !std::isfinite(hostile->x) || !std::isfinite(hostile->y) ||
            !std::isfinite(context.bot.x) || !std::isfinite(context.bot.y) ||
            !std::isfinite(context.bot.orientation))
        {
            reason = "face_target_missing_selected_hostile_or_position";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        if (!hostile || !hostile->positionValid ||
            !std::isfinite(hostile->x) || !std::isfinite(hostile->y) ||
            !std::isfinite(context.bot.x) || !std::isfinite(context.bot.y) ||
            !std::isfinite(context.bot.orientation))
        {
            reason = "face_target_snapshot_position_unavailable";
            return false;
        }

        float const dx = hostile->x - context.bot.x;
        float const dy = hostile->y - context.bot.y;
        if (dx == 0.0f && dy == 0.0f)
        {
            reason = "face_target_same_position";
            return false;
        }

        // Core HasInArc(target) uses a pi-radian arc (a 180-degree front arc).
        // The snapshot check avoids repeatedly dispatching an already-satisfied
        // action; live Core preflight remains authoritative before SetFacingToObject.
        float const targetAngle = std::atan2(dy, dx);
        float const delta = std::atan2(
            std::sin(targetAngle - context.bot.orientation),
            std::cos(targetAngle - context.bot.orientation));
        float const halfFrontArc = 1.57079632679489661923f;
        if (std::fabs(delta) <= halfFrontArc)
        {
            reason = "face_target_already_in_front";
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
        Unit* target = resolved.target;
        float const xBefore = actor->GetPositionX();
        float const yBefore = actor->GetPositionY();
        float const zBefore = actor->GetPositionZ();
        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetGuidBefore = actor->GetTargetGuid();
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        uint32 const targetHealthBefore = target->GetHealth();

        // Core itself ignores this request while moving; the preflight above
        // refuses that case so the action reports a truthful result.
        actor->SetFacingToObject(target);

        // SetFacingToObject launches Core's native facing-only spline. The
        // final WorldObject orientation is updated by UpdateSplineMovement,
        // so do not require IsFacingTarget to change synchronously here. The
        // preflight was repeated immediately before dispatch on this same map
        // thread, so Core's IsStopped() early-return guard has just passed.
        bool const positionPreserved =
            std::fabs(actor->GetPositionX() - xBefore) <= 0.001f &&
            std::fabs(actor->GetPositionY() - yBefore) <= 0.001f &&
            std::fabs(actor->GetPositionZ() - zBefore) <= 0.001f;
        bool const targetStatePreserved =
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetGuidBefore &&
            actor->GetVictim() == victimBefore &&
            actor->IsInCombat() == combatBefore &&
            target->GetHealth() == targetHealthBefore;

        bool noSpellStarted = true;
        for (uint32 spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
            {
                noSpellStarted = false;
                break;
            }
        }

        bool const passed = positionPreserved &&
            targetStatePreserved && noSpellStarted;
        result.result = passed ? ActionResult::Success : ActionResult::Failed;
        result.reason = passed
            ? "face_target_facing_request_accepted"
            : "face_target_facing_request_postcondition_failed";
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

        if (!actor || !target || !resolved.map || !hostile ||
            !hostile->alive || !hostile->positionValid ||
            !candidate.targetGuid ||
            target->GetObjectGuid().GetRawValue() != candidate.targetGuid ||
            !actor->IsInWorld() || !actor->IsAlive() || !target->IsInWorld() ||
            !target->IsAlive() || actor->GetMap() != resolved.map ||
            target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "face_target_live_context_invalid";
            return result;
        }

        ObjectGuid const expectedTarget(candidate.targetGuid);
        if (actor->GetSelectionGuid() != expectedTarget ||
            actor->GetTargetGuid() != expectedTarget)
        {
            result.result = ActionResult::Impossible;
            result.reason = "face_target_live_selection_mismatch";
            return result;
        }

        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "face_target_not_attackable";
            return result;
        }

        if (!actor->IsStopped())
        {
            result.result = ActionResult::Useless;
            result.reason = "face_target_actor_moving";
            return result;
        }

        if (actor->HasUnitState(
                UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        {
            result.result = ActionResult::Useless;
            result.reason = "face_target_actor_cannot_reorient";
            return result;
        }

        for (uint32 spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
            {
                result.result = ActionResult::Useless;
                result.reason = "face_target_non_melee_spell_in_progress";
                return result;
            }
        }

        if (actor->IsFacingTarget(target))
        {
            result.result = ActionResult::Useless;
            result.reason = "face_target_live_already_satisfied";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "face_target_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_face_target";
};
} // namespace PartyBotEngine

#endif
