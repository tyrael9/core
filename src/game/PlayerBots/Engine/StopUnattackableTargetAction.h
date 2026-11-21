#ifndef MANGOS_PARTYBOT_STOP_UNATTACKABLE_TARGET_ACTION_H
#define MANGOS_PARTYBOT_STOP_UNATTACKABLE_TARGET_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Creature.h"
#include "Map.h"
#include "Player.h"
#include "Unit.h"

#include <algorithm>

namespace PartyBotEngine
{
// Detach only the Player's current victim when a value snapshot says it is no
// longer attackable. Final preflight re-resolves the victim on the actor's Map
// and repeats the live Core hostility/targetability checks. This deliberately
// uses AttackStop: CombatStop would also clear combat state and other attackers.
class StopUnattackableTargetAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "attack_stop_only_does_not_generate_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        if (!context.bot.alive || candidate.targetGuid ||
            !context.bot.victimGuid || context.bot.victimAttackable)
        {
            reason = "stop_unattackable_target_not_possible_from_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (candidate.targetGuid || !context.bot.victimGuid ||
            context.bot.victimAttackable)
        {
            reason = "stop_unattackable_target_not_needed";
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
        Guid const expectedVictimGuid = context.bot.victimGuid;
        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !expectedVictimGuid || context.bot.victimAttackable)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_unattackable_target_snapshot_or_actor_invalid";
            return result;
        }

        Unit* victim = resolved.map->GetUnit(ObjectGuid(expectedVictimGuid));
        if (!victim || !victim->IsInWorld() || victim->GetMap() != resolved.map ||
            victim->GetMapId() != context.snapshot.mapId ||
            victim->GetInstanceId() != context.snapshot.instanceId ||
            actor->GetVictim() != victim)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_unattackable_target_live_victim_mismatch";
            return result;
        }

        if (IsAttackableBy(actor, victim))
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_unattackable_target_live_victim_still_attackable";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "stop_unattackable_target_live_preflight_passed";
        return result;
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Guid const expectedVictimGuid = context.bot.victimGuid;
        Unit* victim = resolved.map
            ? resolved.map->GetUnit(ObjectGuid(expectedVictimGuid)) : nullptr;
        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !expectedVictimGuid || !victim || !victim->IsInWorld() ||
            victim->GetMap() != resolved.map ||
            victim->GetInstanceId() != context.snapshot.instanceId ||
            actor->GetVictim() != victim)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_unattackable_target_context_changed_before_dispatch";
            return result;
        }

        if (IsAttackableBy(actor, victim))
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_unattackable_target_became_attackable";
            return result;
        }

        bool const wasInCombat = actor->IsInCombat();
        if (!actor->AttackStop())
        {
            result.result = ActionResult::Failed;
            result.reason = "stop_unattackable_target_attack_stop_failed";
            return result;
        }

        bool const attackerDetached =
            std::find(victim->GetAttackers().begin(), victim->GetAttackers().end(), actor) ==
            victim->GetAttackers().end();
        bool const stopped = actor->GetVictim() == nullptr &&
            actor->GetTargetGuid().IsEmpty() &&
            !actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
            attackerDetached && actor->IsInCombat() == wasInCombat;
        result.result = stopped ? ActionResult::Success : ActionResult::Failed;
        result.reason = stopped
            ? "stop_unattackable_target_stopped"
            : "stop_unattackable_target_postcondition_failed";
        return result;
    }

private:
    static bool IsAttackableBy(Player* actor, Unit* victim)
    {
        if (!actor || !victim || !victim->IsAlive() || !victim->IsInWorld())
            return false;
        if ((victim->IsPlayer() && victim->ToPlayer()->IsGameMaster()) ||
            (victim->ToCreature() && victim->ToCreature()->IsInEvadeMode()))
            return false;
        return actor->CanAttack(victim);
    }

    std::string m_name = "core_stop_unattackable_target";
};
} // namespace PartyBotEngine

#endif
