#ifndef MANGOS_PARTYBOT_DISMOUNT_FOR_COMBAT_ACTION_H
#define MANGOS_PARTYBOT_DISMOUNT_FOR_COMBAT_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Creature.h"
#include "Player.h"
#include "Spell.h"
#include "Unit.h"

#include <cmath>

namespace PartyBotEngine
{
// Dismount only when a live, selected hostile is the reason to do so. The
// Player::Unmount implementation remains the sole owner of Core dismount,
// mount-aura interruption and pet restoration semantics.
class DismountForCombatAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "dismount_for_combat_emits_no_damage_or_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        ObjectGuid const targetGuid(candidate.targetGuid);
        if (!context.bot.alive || !context.bot.mounted ||
            context.bot.taxiFlying || !targetGuid.IsUnit() ||
            candidate.targetGuid == context.bot.guid ||
            context.bot.selectionGuid != candidate.targetGuid ||
            !hostile || !hostile->alive)
        {
            reason = "dismount_for_combat_not_mounted_for_selected_hostile";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!context.bot.mounted || context.bot.taxiFlying ||
            !candidate.targetGuid ||
            context.bot.selectionGuid != candidate.targetGuid)
        {
            reason = "dismount_for_combat_not_needed_from_snapshot";
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
        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetBefore = actor->GetTargetGuid();
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        std::uint32_t const healthBefore = actor->GetHealth();
        std::uint32_t const powerBefore = actor->GetPower(actor->GetPowerType());
        std::uint32_t const mountDisplayBefore = actor->GetMountID();
        std::uint32_t const targetHealthBefore = target->GetHealth();

        Creature* targetCreature = target->ToCreature();
        bool const tracksThreat = targetCreature && targetCreature->CanHaveThreatList();
        float const threatBefore = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;

        UnitDismountResult const dismountResult = actor->Unmount();

        float const threatAfter = tracksThreat
            ? targetCreature->GetThreatManager().getThreat(actor) : 0.0f;
        bool const noThreatOrTargetMutation =
            target->GetHealth() == targetHealthBefore &&
            (!tracksThreat || std::fabs(threatAfter - threatBefore) <= 0.001f);
        bool const actorStatePreserved =
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetBefore &&
            actor->GetVictim() == victimBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->GetHealth() == healthBefore &&
            actor->GetPower(actor->GetPowerType()) == powerBefore &&
            !HasNonMeleeSpell(actor);

        bool const dismounted =
            dismountResult == DISMOUNTRESULT_OK &&
            mountDisplayBefore != 0 &&
            !actor->IsMounted() && actor->GetMountID() == 0;
        bool const passed = dismounted && actorStatePreserved &&
            noThreatOrTargetMutation;
        result.result = passed ? ActionResult::Success : ActionResult::Failed;
        result.reason = passed
            ? "dismount_for_combat_unmounted_without_combat_side_effects"
            : "dismount_for_combat_postcondition_failed";
        return result;
    }

private:
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

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Unit* target = resolved.target;
        HostileSnapshot const* hostile =
            context.snapshot.FindHostile(candidate.targetGuid);
        ObjectGuid const expectedTarget(candidate.targetGuid);

        if (!actor || !target || !resolved.map || !hostile || !hostile->alive ||
            !expectedTarget.IsUnit() || candidate.targetGuid == context.bot.guid ||
            target->GetObjectGuid() != expectedTarget ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !target->IsInWorld() || !target->IsAlive() ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "dismount_for_combat_live_context_invalid";
            return result;
        }

        if (!context.bot.mounted || context.bot.taxiFlying ||
            !actor->IsMounted() || actor->IsTaxiFlying())
        {
            result.result = ActionResult::Useless;
            result.reason = "dismount_for_combat_actor_not_mounted_or_on_taxi";
            return result;
        }

        if (actor->GetSelectionGuid() != expectedTarget ||
            actor->GetTargetGuid() != expectedTarget)
        {
            result.result = ActionResult::Impossible;
            result.reason = "dismount_for_combat_live_selection_mismatch";
            return result;
        }

        if ((target->IsPlayer() && target->ToPlayer()->IsGameMaster()) ||
            (target->ToCreature() && target->ToCreature()->IsInEvadeMode()) ||
            !actor->CanAttack(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "dismount_for_combat_target_not_attackable";
            return result;
        }

        if (actor->HasUnitState(
                UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        {
            result.result = ActionResult::Useless;
            result.reason = "dismount_for_combat_actor_cannot_act";
            return result;
        }

        if (HasNonMeleeSpell(actor))
        {
            result.result = ActionResult::Useless;
            result.reason = "dismount_for_combat_non_melee_spell_in_progress";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "dismount_for_combat_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_dismount_for_combat";
};
} // namespace PartyBotEngine

#endif
