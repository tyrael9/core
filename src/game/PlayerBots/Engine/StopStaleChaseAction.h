#ifndef MANGOS_PARTYBOT_STOP_STALE_CHASE_ACTION_H
#define MANGOS_PARTYBOT_STOP_STALE_CHASE_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "Movement/TargetedMovementGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Expire only the current native ChaseMovementGenerator when its captured
// target is no longer the Player's live victim. Core MovementExpired(false)
// also removes contiguous targeted Follow/Chase generators underneath it;
// the policy must reissue Follow when appropriate. Unrelated movement beneath
// the stale Chase is rejected before mutation.
class StopStaleChaseAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "stop_stale_chase_expires_targeted_movement_only";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        ObjectGuid const chaseGuid(context.bot.chaseTargetGuid);
        if (!context.bot.online || !context.bot.alive || candidate.targetGuid ||
            !chaseGuid.IsUnit() || context.bot.chaseTargetGuid == context.bot.guid ||
            context.bot.chaseTargetGuid == context.bot.victimGuid)
        {
            reason = "stop_stale_chase_missing_stale_chase_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (candidate.targetGuid || !context.bot.chaseTargetGuid ||
            context.bot.chaseTargetGuid == context.bot.victimGuid)
        {
            reason = "stop_stale_chase_current_victim_matches_chase";
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
        MovementGenerator const* const staleChase = motion->GetCurrent();
        Unit* const victimBefore = actor->GetVictim();
        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetGuidBefore = actor->GetTargetGuid();
        bool const combatBefore = actor->IsInCombat();
        std::uint32_t const healthBefore = actor->GetHealth();
        Powers const powerType = actor->GetPowerType();
        std::uint32_t const powerBefore = actor->GetPower(powerType);
        Spell* spellsBefore[CURRENT_MAX_SPELL] = {};
        for (std::uint32_t spellIndex = 0; spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
            spellsBefore[spellIndex] =
                actor->GetCurrentSpell(CurrentSpellTypes(spellIndex));

        // Core safely defers deletion if called while MotionMaster is updating.
        // false avoids resetting the base generator; contiguous targeted
        // Follow/Chase generators underneath this Chase are finalized too.
        motion->MovementExpired(false);

        bool staleChaseRemains = false;
        bool targetedMovementRemains = false;
        for (MotionMaster::const_iterator it = motion->begin();
             it != motion->end(); ++it)
        {
            if (*it == staleChase)
                staleChaseRemains = true;

            MovementGeneratorType const type = (*it)->GetMovementGeneratorType();
            if (type == CHASE_MOTION_TYPE || type == FOLLOW_MOTION_TYPE)
                targetedMovementRemains = true;
        }

        bool spellsPreserved = true;
        for (std::uint32_t spellIndex = 0; spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)) !=
                spellsBefore[spellIndex])
            {
                spellsPreserved = false;
                break;
            }
        }

        bool const actorStatePreserved =
            actor->GetVictim() == victimBefore &&
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetGuidBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->GetHealth() == healthBefore &&
            actor->GetPower(powerType) == powerBefore &&
            spellsPreserved;

        bool const expired = !staleChaseRemains && !targetedMovementRemains &&
            motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE &&
            actorStatePreserved;
        result.result = expired ? ActionResult::Success : ActionResult::Failed;
        result.reason = expired
            ? "stop_stale_chase_expired_targeted_generators_preserving_combat"
            : "stop_stale_chase_expiry_postcondition_failed";
        return result;
    }

private:
    static ChaseMovementGenerator<Player> const* GetCurrentChase(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster() ||
            actor->GetMotionMaster()->empty())
            return nullptr;

        MovementGenerator const* current = actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != CHASE_MOTION_TYPE)
            return nullptr;

        return static_cast<ChaseMovementGenerator<Player> const*>(current);
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Guid const expectedChaseGuid = context.bot.chaseTargetGuid;
        ObjectGuid const expectedChase(expectedChaseGuid);

        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !expectedChase.IsUnit() || expectedChaseGuid == context.bot.guid ||
            !context.bot.online || !context.bot.alive ||
            context.bot.guid != actor->GetObjectGuid().GetRawValue() ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            actor->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_stale_chase_actor_or_snapshot_invalid";
            return result;
        }

        Unit* const liveVictim = actor->GetVictim();
        Guid const liveVictimGuid = liveVictim
            ? liveVictim->GetObjectGuid().GetRawValue() : 0;
        if (liveVictimGuid != context.bot.victimGuid)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_stale_chase_live_victim_snapshot_mismatch";
            return result;
        }

        if (liveVictimGuid == expectedChaseGuid)
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_stale_chase_target_is_current_victim";
            return result;
        }

        ChaseMovementGenerator<Player> const* chase = GetCurrentChase(actor);
        if (!chase)
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_stale_chase_no_current_chase";
            return result;
        }

        Unit* const chaseTarget = chase->GetTarget();
        if (!chaseTarget || !chaseTarget->IsInWorld() ||
            chaseTarget->GetMap() != resolved.map ||
            chaseTarget->GetMapId() != context.snapshot.mapId ||
            chaseTarget->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_stale_chase_live_target_unavailable";
            return result;
        }

        if (chaseTarget->GetObjectGuid() != expectedChase)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_stale_chase_live_generator_mismatch";
            return result;
        }

        // MovementExpired(false) intentionally removes contiguous targeted
        // Follow/Chase generators beneath this Chase. Do not let it expose or
        // perturb a Point, charge, home or other unrelated movement generator.
        MotionMaster* motion = actor->GetMotionMaster();
        for (MotionMaster::const_iterator it = motion->begin();
             it != motion->end(); ++it)
        {
            if (*it == motion->GetCurrent())
                continue;

            MovementGeneratorType const type = (*it)->GetMovementGeneratorType();
            if (type != CHASE_MOTION_TYPE && type != FOLLOW_MOTION_TYPE &&
                type != IDLE_MOTION_TYPE)
            {
                result.result = ActionResult::Useless;
                result.reason = "stop_stale_chase_unrelated_movement_stacked";
                return result;
            }
        }

        result.result = ActionResult::Success;
        result.reason = "stop_stale_chase_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_stop_stale_chase";
};
} // namespace PartyBotEngine

#endif
