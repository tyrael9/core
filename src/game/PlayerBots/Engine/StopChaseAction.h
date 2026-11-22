#ifndef MANGOS_PARTYBOT_STOP_CHASE_ACTION_H
#define MANGOS_PARTYBOT_STOP_CHASE_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"
#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "Movement/TargetedMovementGenerator.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace PartyBotEngine
{
// Cancel only a native Chase that still targets the Player's live victim.
// Core expiry also drops contiguous targeted movement beneath it, so unrelated
// movement is rejected and the owning policy must reissue Follow.
class StopChaseAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "stop_chase_expires_targeted_movement_without_combat_effects";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        ObjectGuid const target(candidate.targetGuid);
        if (!context.bot.online || !context.bot.alive || !target.IsUnit() ||
            candidate.targetGuid == context.bot.guid ||
            candidate.targetGuid != context.bot.victimGuid ||
            candidate.targetGuid != context.bot.chaseTargetGuid)
        {
            reason = "stop_chase_requires_current_victim_chase_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!candidate.targetGuid ||
            candidate.targetGuid != context.bot.victimGuid ||
            candidate.targetGuid != context.bot.chaseTargetGuid)
        {
            reason = "stop_chase_snapshot_no_longer_matches_current_victim";
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
        ObjectGuid const selection = actor->GetSelectionGuid();
        ObjectGuid const targetGuid = actor->GetTargetGuid();
        Unit* const victim = actor->GetVictim();
        bool const inCombat = actor->IsInCombat();
        bool const melee = actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
        uint32 const health = actor->GetHealth();
        Powers const powerType = actor->GetPowerType();
        uint32 const power = actor->GetPower(powerType);
        uint32 const targetHealth = target->GetHealth();
        float const threat = target->GetThreatManager().getThreat(actor);
        bool const attackerReference =
            std::find(target->GetAttackers().begin(), target->GetAttackers().end(), actor) !=
            target->GetAttackers().end();
        Spell* spells[CURRENT_MAX_SPELL] = {};
        for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
            spells[i] = actor->GetCurrentSpell(CurrentSpellTypes(i));

        // Movement-only cancellation. Never call AttackStop or CombatStop.
        motion->MovementExpired(false);

        bool targetedMovementRemains = false;
        for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
        {
            MovementGeneratorType const type = (*it)->GetMovementGeneratorType();
            targetedMovementRemains |= type == CHASE_MOTION_TYPE || type == FOLLOW_MOTION_TYPE;
        }
        bool spellsPreserved = true;
        for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
            spellsPreserved &= actor->GetCurrentSpell(CurrentSpellTypes(i)) == spells[i];
        bool const attackerReferenceAfter =
            std::find(target->GetAttackers().begin(), target->GetAttackers().end(), actor) !=
            target->GetAttackers().end();
        bool const statePreserved =
            actor->GetVictim() == victim && victim == target &&
            actor->GetSelectionGuid() == selection && actor->GetTargetGuid() == targetGuid &&
            actor->IsInCombat() == inCombat &&
            actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == melee &&
            actor->GetHealth() == health && actor->GetPower(powerType) == power &&
            target->GetHealth() == targetHealth &&
            std::fabs(target->GetThreatManager().getThreat(actor) - threat) <= 0.001f &&
            attackerReferenceAfter == attackerReference && spellsPreserved;
        bool const movementStopped = !targetedMovementRemains &&
            !actor->HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE |
                                 UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
        bool const passed = movementStopped && statePreserved;
        result.result = passed ? ActionResult::Success : ActionResult::Failed;
        result.reason = passed
            ? "stop_chase_expired_targeted_movement_preserving_combat"
            : "stop_chase_expiry_postcondition_failed";
        return result;
    }

private:
    static ChaseMovementGenerator<Player> const* CurrentChase(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster() || actor->GetMotionMaster()->empty())
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
        Unit* target = resolved.target;
        HostileSnapshot const* hostile = context.snapshot.FindHostile(candidate.targetGuid);
        ObjectGuid const expected(candidate.targetGuid);
        if (!actor || !target || !resolved.map || !hostile || !hostile->alive ||
            !expected.IsUnit() || candidate.targetGuid == context.bot.guid ||
            candidate.targetGuid != context.bot.victimGuid ||
            candidate.targetGuid != context.bot.chaseTargetGuid ||
            context.bot.guid != actor->GetObjectGuid().GetRawValue() ||
            !context.bot.online || !context.bot.alive ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !target->IsInWorld() || !target->IsAlive() ||
            target->GetObjectGuid() != expected ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_chase_actor_target_or_snapshot_invalid";
            return result;
        }
        if (actor->GetVictim() != target || actor->GetTargetGuid() != expected)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_chase_live_victim_mismatch";
            return result;
        }
        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_chase_current_victim_not_attackable";
            return result;
        }

        MotionMaster* motion = actor->GetMotionMaster();
        ChaseMovementGenerator<Player> const* chase = CurrentChase(actor);
        if (!motion || !chase)
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_chase_no_current_chase";
            return result;
        }
        if (chase->GetTarget() != target)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_chase_live_generator_target_mismatch";
            return result;
        }

        // DirectExpire/DelayedExpire removes contiguous Chase/Follow beneath
        // the current generator. Do not expose or disturb unrelated movement.
        std::size_t stackCount = 0;
        for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
        {
            ++stackCount;
            if (*it == motion->GetCurrent())
                continue;
            MovementGeneratorType const type = (*it)->GetMovementGeneratorType();
            if (type != CHASE_MOTION_TYPE && type != FOLLOW_MOTION_TYPE &&
                type != IDLE_MOTION_TYPE)
            {
                result.result = ActionResult::Useless;
                result.reason = "stop_chase_unrelated_movement_stacked";
                return result;
            }
        }
        if (stackCount <= 1)
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_chase_no_expirable_movement_stack";
            return result;
        }
        result.result = ActionResult::Success;
        result.reason = "stop_chase_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_stop_chase";
};
} // namespace PartyBotEngine
#endif
