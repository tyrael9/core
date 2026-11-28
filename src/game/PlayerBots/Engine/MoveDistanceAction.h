#ifndef MANGOS_PARTYBOT_MOVE_DISTANCE_ACTION_H
#define MANGOS_PARTYBOT_MOVE_DISTANCE_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Maps/GridDefines.h"
#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "Movement/TargetedMovementGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "Unit.h"

#include <cmath>
#include <cstdint>

namespace PartyBotEngine
{
// Request native Core distancing away from the bot's established current
// victim. Core validates destination LOS/path reachability and stacks
// DISTANCING above an existing Chase/Follow; Core later restores that exact
// targeted generator when Distancing expires.
class MoveDistanceAction final : public CoreAction
{
public:
    explicit MoveDistanceAction(float distance = 8.0f) : m_distance(distance) {}
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        // Request-only movement has no synchronous threat mutation. This does
        // not predict environmental aggro caused later by the chosen route.
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "move_distance_request_has_no_synchronous_threat_mutation";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        if (!context.bot.online || !context.bot.alive ||
            !candidate.targetGuid || candidate.targetGuid != context.bot.victimGuid ||
            !ObjectGuid(candidate.targetGuid).IsUnit() ||
            candidate.targetGuid == context.bot.guid ||
            !hostile || !hostile->alive || !hostile->positionValid ||
            !IsValidConfiguration() ||
            !MaNGOS::IsValidMapCoord(context.bot.x, context.bot.y, context.bot.z))
        {
            reason = "move_distance_missing_current_victim_or_invalid_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        if (!candidate.targetGuid || candidate.targetGuid != context.bot.victimGuid ||
            !hostile || !hostile->alive || !hostile->positionValid ||
            !IsValidConfiguration())
        {
            reason = "move_distance_current_victim_unavailable";
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
        MotionMaster* motion = actor->GetMotionMaster();
        MovementGenerator const* const preservedMovement = motion->GetCurrent();

        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetGuidBefore = actor->GetTargetGuid();
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        bool const meleeBefore = actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
        uint32 const healthBefore = actor->GetHealth();
        Powers const powerType = actor->GetPowerType();
        uint32 const powerBefore = actor->GetPower(powerType);
        uint32 const targetHealthBefore = target->GetHealth();
        Creature* targetCreature = target->ToCreature();
        bool const tracksThreat = targetCreature && targetCreature->CanHaveThreatList();
        float const threatBefore = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;

        bool const accepted = motion->MoveDistance(target, m_distance);
        bool preservedMovementRemains = false;
        for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
            preservedMovementRemains |= *it == preservedMovement;
        float const threatAfter = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;
        bool const statePreserved =
            actor->GetVictim() == victimBefore && victimBefore == target &&
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetGuidBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == meleeBefore &&
            actor->GetHealth() == healthBefore &&
            actor->GetPower(powerType) == powerBefore &&
            target->GetHealth() == targetHealthBefore &&
            (!tracksThreat || std::fabs(threatAfter - threatBefore) <= 0.001f);
        bool const distancingStarted = accepted &&
            motion->GetCurrent() != preservedMovement &&
            motion->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE &&
            preservedMovementRemains;
        bool const passed = distancingStarted && statePreserved;
        result.result = passed ? ActionResult::Success : ActionResult::Failed;
        result.reason = passed
            ? "move_distance_core_request_accepted_over_preserved_targeted_movement"
            : "move_distance_core_request_or_postcondition_failed";
        return result;
    }

private:
    bool IsValidConfiguration() const
    {
        return std::isfinite(m_distance) && m_distance > 0.0f && m_distance <= 40.0f;
    }

    static bool HasNonMeleeSpell(Player* actor)
    {
        if (!actor)
            return true;
        for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_MAX_SPELL; ++i)
            if (actor->GetCurrentSpell(CurrentSpellTypes(i)))
                return true;
        return false;
    }

    static ChaseMovementGenerator<Player> const* CurrentChase(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster())
            return nullptr;
        MovementGenerator const* current = actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != CHASE_MOTION_TYPE)
            return nullptr;
        return static_cast<ChaseMovementGenerator<Player> const*>(current);
    }

    static FollowMovementGenerator<Player> const* CurrentFollow(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster())
            return nullptr;
        MovementGenerator const* current = actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            return nullptr;
        return static_cast<FollowMovementGenerator<Player> const*>(current);
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
            !hostile->positionValid || !expectedTarget.IsUnit() ||
            candidate.targetGuid == context.bot.guid ||
            candidate.targetGuid != context.bot.victimGuid ||
            context.bot.guid != actor->GetObjectGuid().GetRawValue() ||
            !context.bot.online || !context.bot.alive ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !target->IsInWorld() || !target->IsAlive() ||
            target->GetObjectGuid() != expectedTarget ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId ||
            !IsValidConfiguration())
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_distance_actor_target_or_snapshot_invalid";
            return result;
        }
        if (actor->GetVictim() != target || actor->GetTargetGuid() != expectedTarget)
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_distance_live_victim_mismatch";
            return result;
        }
        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_distance_current_victim_not_attackable";
            return result;
        }
        if (actor->GetTransport() || actor->IsMounted() || actor->IsTaxiFlying())
        {
            result.result = ActionResult::Useless;
            result.reason = "move_distance_actor_transported_or_mounted";
            return result;
        }
        if (actor->HasUnitState(
                UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE |
                UNIT_STATE_POSSESSED | UNIT_STATE_CAN_NOT_REACT))
        {
            result.result = ActionResult::Useless;
            result.reason = "move_distance_actor_cannot_move_or_react";
            return result;
        }
        if (HasNonMeleeSpell(actor))
        {
            result.result = ActionResult::Useless;
            result.reason = "move_distance_non_melee_spell_in_progress";
            return result;
        }

        MotionMaster* motion = actor->GetMotionMaster();
        if (!motion || motion->empty())
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_distance_motion_master_missing";
            return result;
        }

        MovementGeneratorType const currentType = motion->GetCurrentMovementGeneratorType();
        if (currentType == CHASE_MOTION_TYPE)
        {
            ChaseMovementGenerator<Player> const* chase = CurrentChase(actor);
            if (!chase || chase->GetTarget() != target ||
                context.bot.chaseTargetGuid != candidate.targetGuid)
            {
                result.result = ActionResult::Useless;
                result.reason = "move_distance_chase_does_not_match_current_victim";
                return result;
            }
        }
        else if (currentType == FOLLOW_MOTION_TYPE)
        {
            FollowMovementGenerator<Player> const* follow = CurrentFollow(actor);
            Unit* followUnit = follow ? follow->GetTarget() : nullptr;
            Player* followPlayer = followUnit ? followUnit->ToPlayer() : nullptr;
            Guid const followGuid = followPlayer
                ? followPlayer->GetObjectGuid().GetRawValue() : 0;
            PartyMemberSnapshot const* followMember =
                followGuid ? context.snapshot.FindMember(followGuid) : nullptr;
            if (!followPlayer || !followPlayer->IsInWorld() || !followPlayer->IsAlive() ||
                followPlayer->GetMap() != resolved.map ||
                followPlayer->GetMapId() != context.snapshot.mapId ||
                followPlayer->GetInstanceId() != context.snapshot.instanceId ||
                context.bot.followTargetGuid != followGuid ||
                !followMember || !followMember->online || !followMember->alive ||
                followMember->mapId != context.snapshot.mapId ||
                followMember->instanceId != context.snapshot.instanceId)
            {
                result.result = ActionResult::Useless;
                result.reason = "move_distance_follow_target_stale";
                return result;
            }
        }
        else if (currentType != IDLE_MOTION_TYPE || !motion->IsUsingIdleOrDefaultMovement())
        {
            result.result = ActionResult::Useless;
            result.reason = "move_distance_unrelated_movement_active";
            return result;
        }

        float x = 0.0f, y = 0.0f, z = 0.0f;
        target->GetNearPoint(actor, x, y, z, 0.0f, m_distance, target->GetAngle(actor));
        if (!MaNGOS::IsValidMapCoord(x, y, z))
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_distance_destination_invalid";
            return result;
        }
        float const cx = actor->GetPositionX() - target->GetPositionX();
        float const cy = actor->GetPositionY() - target->GetPositionY();
        float const cz = actor->GetPositionZ() - target->GetPositionZ();
        float const dx = x - target->GetPositionX();
        float const dy = y - target->GetPositionY();
        float const dz = z - target->GetPositionZ();
        float const currentDistanceSquared = cx * cx + cy * cy + cz * cz;
        float const destinationDistanceSquared = dx * dx + dy * dy + dz * dz;
        if (actor->GetDistanceSqr(x, y, z) <= 0.01f ||
            currentDistanceSquared >= destinationDistanceSquared - 0.01f)
        {
            result.result = ActionResult::Useless;
            result.reason = "move_distance_already_at_or_beyond_requested_separation";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "move_distance_live_preflight_passed";
        return result;
    }

    float m_distance = 8.0f;
    std::string m_name = "core_move_distance";
};
} // namespace PartyBotEngine

#endif
