#ifndef MANGOS_PARTYBOT_CHASE_TARGET_ACTION_H
#define MANGOS_PARTYBOT_CHASE_TARGET_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "MotionMaster.h"
#include "Player.h"
#include "Spell.h"
#include "TargetedMovementGenerator.h"
#include "Unit.h"

#include <cmath>

namespace PartyBotEngine
{
// Move a Player toward its already-established live combat victim using
// Core's native ChaseMovementGenerator. This is deliberately separate from
// AttackTargetAction: starting auto-attack does not itself promise movement.
class ChaseTargetAction final : public CoreAction
{
public:
    explicit ChaseTargetAction(float chaseDistance = 1.0f,
                               float chaseAngle = 0.0f)
        : m_chaseDistance(chaseDistance), m_chaseAngle(chaseAngle) {}

    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        // The synchronous chase request changes only the movement generator.
        // Future melee swings, damage and resulting threat remain Core-owned.
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "chase_target_request_has_no_synchronous_damage_or_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        if (!context.bot.online || !context.bot.alive ||
            !candidate.targetGuid || candidate.targetGuid != context.bot.victimGuid ||
            !ObjectGuid(candidate.targetGuid).IsUnit() ||
            candidate.targetGuid == context.bot.guid ||
            !hostile || !hostile->alive || !IsValidConfiguration() ||
            context.bot.mounted || context.bot.taxiFlying)
        {
            reason = "chase_target_missing_current_hostile_or_invalid_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!candidate.targetGuid ||
            context.bot.victimGuid != candidate.targetGuid ||
            context.bot.chaseTargetGuid == candidate.targetGuid)
        {
            reason = "chase_target_already_chasing_current_victim";
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
        MotionMaster* motion = actor->GetMotionMaster();

        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetGuidBefore = actor->GetTargetGuid();
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        std::uint32_t const healthBefore = actor->GetHealth();
        std::uint32_t const powerBefore = actor->GetPower(actor->GetPowerType());
        std::uint32_t const targetHealthBefore = target->GetHealth();

        Creature* targetCreature = target->ToCreature();
        bool const tracksThreat = targetCreature && targetCreature->CanHaveThreatList();
        float const threatBefore = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;

        motion->MoveChase(target, m_chaseDistance, m_chaseAngle);

        ChaseMovementGenerator<Player> const* chase = GetCurrentChase(actor);
        float const threatAfter = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;
        bool const movementAccepted = chase && chase->GetTarget() == target;
        bool const actorStatePreserved =
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetGuidBefore &&
            actor->GetVictim() == victimBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->GetHealth() == healthBefore &&
            actor->GetPower(actor->GetPowerType()) == powerBefore;
        bool const targetStatePreserved = target->GetHealth() == targetHealthBefore &&
            (!tracksThreat || std::fabs(threatAfter - threatBefore) <= 0.001f);

        bool const passed = movementAccepted && actorStatePreserved &&
            targetStatePreserved;
        result.result = passed ? ActionResult::Success : ActionResult::Failed;
        result.reason = passed
            ? "chase_target_core_movement_request_accepted"
            : "chase_target_core_movement_or_state_postcondition_failed";
        return result;
    }

private:
    bool IsValidConfiguration() const
    {
        return std::isfinite(m_chaseDistance) && m_chaseDistance >= 0.0f &&
            std::isfinite(m_chaseAngle);
    }

    static ChaseMovementGenerator<Player> const* GetCurrentChase(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster())
            return nullptr;
        MovementGenerator const* current = actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != CHASE_MOTION_TYPE)
            return nullptr;
        return static_cast<ChaseMovementGenerator<Player> const*>(current);
    }

    static FollowMovementGenerator<Player> const* GetCurrentFollow(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster())
            return nullptr;
        MovementGenerator const* current = actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            return nullptr;
        return static_cast<FollowMovementGenerator<Player> const*>(current);
    }

    static bool HasNonMeleeSpell(Player* actor)
    {
        if (!actor)
            return true;
        for (std::uint32_t spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
                return true;
        }
        return false;
    }

    ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Unit* target = resolved.target;
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        ObjectGuid const expectedTarget(candidate.targetGuid);

        if (!actor || !target || !resolved.map || !hostile || !hostile->alive ||
            !expectedTarget.IsUnit() || candidate.targetGuid != context.bot.victimGuid ||
            candidate.targetGuid == context.bot.guid ||
            target->GetObjectGuid() != expectedTarget ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !target->IsInWorld() || !target->IsAlive() ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId ||
            !IsValidConfiguration())
        {
            result.result = ActionResult::Impossible;
            result.reason = "chase_target_live_context_invalid";
            return result;
        }

        if (actor->GetVictim() != target ||
            actor->GetVictim()->GetObjectGuid() != expectedTarget)
        {
            result.result = ActionResult::Impossible;
            result.reason = "chase_target_live_victim_mismatch";
            return result;
        }

        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "chase_target_not_attackable";
            return result;
        }

        if (actor->IsMounted() || actor->IsTaxiFlying())
        {
            result.result = ActionResult::Useless;
            result.reason = "chase_target_actor_mounted_or_on_taxi";
            return result;
        }

        if (actor->HasUnitState(
                UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE |
                UNIT_STATE_POSSESSED | UNIT_STATE_CAN_NOT_REACT))
        {
            result.result = ActionResult::Useless;
            result.reason = "chase_target_actor_cannot_move_or_react";
            return result;
        }

        if (HasNonMeleeSpell(actor))
        {
            result.result = ActionResult::Useless;
            result.reason = "chase_target_non_melee_spell_in_progress";
            return result;
        }

        MotionMaster* motion = actor->GetMotionMaster();
        if (!motion)
        {
            result.result = ActionResult::Impossible;
            result.reason = "chase_target_motion_master_missing";
            return result;
        }

        ChaseMovementGenerator<Player> const* currentChase = GetCurrentChase(actor);
        if (currentChase && currentChase->GetTarget() == target)
        {
            result.result = ActionResult::Useless;
            result.reason = "chase_target_same_core_chase_already_active";
            return result;
        }

        if (!currentChase)
        {
            FollowMovementGenerator<Player> const* currentFollow =
                GetCurrentFollow(actor);
            if (currentFollow)
            {
                // Core MoveChase stacks over the existing Follow generator,
                // but Core's MovementExpired deliberately drops stored targeted
                // generators under a completed Chase. The policy must reissue
                // Follow after combat; only supersede a follow whose live target
                // still matches this snapshot's same-map party roster.
                Unit* followTargetUnit = currentFollow->GetTarget();
                Player* followTarget = followTargetUnit
                    ? followTargetUnit->ToPlayer() : nullptr;
                Guid const followGuid = followTarget
                    ? followTarget->GetObjectGuid().GetRawValue() : 0;
                PartyMemberSnapshot const* followMember =
                    followGuid ? context.snapshot.FindMember(followGuid) : nullptr;
                if (!followTarget || !followTarget->IsInWorld() ||
                    !followTarget->IsAlive() || followTarget->GetMap() != resolved.map ||
                    followTarget->GetMapId() != context.snapshot.mapId ||
                    followTarget->GetInstanceId() != context.snapshot.instanceId ||
                    context.bot.followTargetGuid != followGuid ||
                    !followMember || !followMember->online || !followMember->alive ||
                    followMember->mapId != context.snapshot.mapId ||
                    followMember->instanceId != context.snapshot.instanceId)
                {
                    result.result = ActionResult::Useless;
                    result.reason = "chase_target_follow_target_stale";
                    return result;
                }
            }
            else if (!motion->IsUsingIdleOrDefaultMovement())
            {
                result.result = ActionResult::Useless;
                result.reason = "chase_target_unrelated_movement_active";
                return result;
            }
        }

        if (actor->CanReachWithMeleeAutoAttack(target))
        {
            result.result = ActionResult::Useless;
            result.reason = "chase_target_already_melee_reachable";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "chase_target_live_preflight_passed";
        return result;
    }

    float m_chaseDistance = 1.0f;
    float m_chaseAngle = 0.0f;
    std::string m_name = "core_chase_target";
};
} // namespace PartyBotEngine

#endif
