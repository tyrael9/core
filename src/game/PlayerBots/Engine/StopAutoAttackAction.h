#ifndef MANGOS_PARTYBOT_STOP_AUTO_ATTACK_ACTION_H
#define MANGOS_PARTYBOT_STOP_AUTO_ATTACK_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Map.h"
#include "Player.h"
#include "Spell.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>

namespace PartyBotEngine
{
// Stop only the exact melee victim captured by the value snapshot. This is an
// intentional disengage operation (for example, before a crowd-control cast),
// not a CombatStop: selection, combat state, other attackers and non-melee
// spells are preserved. Core AttackStop owns the melee-stop packet and current
// melee-spell interruption.
class StopAutoAttackAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "stop_auto_attack_emits_no_new_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        ObjectGuid const victimGuid(context.bot.victimGuid);
        if (!context.bot.alive || candidate.targetGuid ||
            !victimGuid.IsUnit() || context.bot.currentSpellId)
        {
            reason = "stop_auto_attack_missing_victim_or_active_spell_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        ObjectGuid const victimGuid(context.bot.victimGuid);
        if (candidate.targetGuid || !victimGuid.IsUnit() ||
            context.bot.currentSpellId)
        {
            reason = "stop_auto_attack_not_needed_from_snapshot";
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
        Unit* victim = resolved.map->GetUnit(ObjectGuid(context.bot.victimGuid));
        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        bool const combatBefore = actor->IsInCombat();
        uint32 const victimHealthBefore = victim->GetHealth();

        Creature* victimCreature = victim->ToCreature();
        bool const tracksThreat =
            victimCreature && victimCreature->CanHaveThreatList();
        float const threatBefore = tracksThreat
            ? victimCreature->GetThreatManager().getThreat(actor) : 0.0f;

        // Do not call CombatStop: it also changes combat state and may interrupt
        // non-melee casts. AttackStop is the narrow Core API for this operation.
        bool const accepted = actor->AttackStop();

        float const threatAfter = tracksThreat
            ? victimCreature->GetThreatManager().getThreat(actor) : 0.0f;
        bool const attackerDetached =
            std::find(victim->GetAttackers().begin(),
                      victim->GetAttackers().end(), actor) ==
            victim->GetAttackers().end();
        bool const noNonMeleeSpellStarted = !HasNonMeleeSpell(actor);
        bool const noHealthOrThreatMutation =
            victim->GetHealth() == victimHealthBefore &&
            (!tracksThreat || std::fabs(threatAfter - threatBefore) <= 0.001f);

        bool const stoppedOnlyExpectedAttack = accepted &&
            actor->GetVictim() == nullptr && actor->GetTargetGuid().IsEmpty() &&
            !actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
            actor->GetSelectionGuid() == selectionBefore &&
            actor->IsInCombat() == combatBefore && attackerDetached &&
            noNonMeleeSpellStarted && noHealthOrThreatMutation;

        result.result = stoppedOnlyExpectedAttack
            ? ActionResult::Success : ActionResult::Failed;
        result.reason = stoppedOnlyExpectedAttack
            ? "stop_auto_attack_stopped_expected_victim"
            : "stop_auto_attack_postcondition_failed";
        return result;
    }

private:
    static bool HasNonMeleeSpell(Player* actor)
    {
        if (!actor)
            return true;

        for (uint32 spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
                return true;
        }
        return false;
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Guid const expectedVictimGuid = context.bot.victimGuid;
        ObjectGuid const expectedVictim(expectedVictimGuid);

        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !expectedVictim.IsUnit() || !actor->IsInWorld() || !actor->IsAlive() ||
            actor->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_auto_attack_actor_or_snapshot_invalid";
            return result;
        }

        Unit* victim = resolved.map->GetUnit(expectedVictim);
        if (!victim || !victim->IsInWorld() || victim->GetMap() != resolved.map ||
            victim->GetMapId() != context.snapshot.mapId ||
            victim->GetInstanceId() != context.snapshot.instanceId ||
            victim->GetObjectGuid() != expectedVictim ||
            actor->GetVictim() != victim ||
            actor->GetTargetGuid() != expectedVictim)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_auto_attack_live_victim_mismatch";
            return result;
        }

        if (!actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_auto_attack_melee_already_stopped";
            return result;
        }

        if (HasNonMeleeSpell(actor))
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_auto_attack_non_melee_spell_in_progress";
            return result;
        }

        bool const attackerReferencePresent =
            std::find(victim->GetAttackers().begin(),
                      victim->GetAttackers().end(), actor) !=
            victim->GetAttackers().end();
        if (!attackerReferencePresent)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_auto_attack_attacker_reference_missing";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "stop_auto_attack_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_stop_auto_attack";
};
} // namespace PartyBotEngine

#endif
