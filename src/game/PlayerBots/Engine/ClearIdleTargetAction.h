#ifndef MANGOS_PARTYBOT_CLEAR_IDLE_TARGET_ACTION_H
#define MANGOS_PARTYBOT_CLEAR_IDLE_TARGET_ACTION_H

#include "CoreAction.h"

#include "Player.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Clear a stale Player selection only when the live Player is completely idle.
// A nonzero selection snapshot with no victim is not sufficient: final preflight
// rechecks combat, melee state, all current Spell slots and both live GUID fields.
class ClearIdleTargetAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "clear_idle_selection_only_no_combat_or_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        if (!context.bot.alive || candidate.targetGuid ||
            !context.bot.selectionGuid || context.bot.inCombat ||
            context.bot.victimGuid || context.bot.currentSpellId)
        {
            reason = "clear_idle_target_not_possible_from_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (candidate.targetGuid || !context.bot.selectionGuid)
        {
            reason = "clear_idle_target_already_clear";
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
        std::uint64_t const expectedSelection = context.bot.selectionGuid;
        if (!actor || resolved.target || candidate.targetGuid ||
            !expectedSelection || !actor->IsAlive() ||
            actor->GetSelectionGuid().GetRawValue() != expectedSelection ||
            actor->GetTargetGuid().GetRawValue() != expectedSelection)
        {
            result.result = ActionResult::Impossible;
            result.reason = "clear_idle_target_live_selection_mismatch";
            return result;
        }

        if (actor->IsInCombat() || actor->GetVictim() ||
            actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            result.result = ActionResult::Useless;
            result.reason = "clear_idle_target_player_not_idle";
            return result;
        }

        for (uint32 spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
            {
                result.result = ActionResult::Useless;
                result.reason = "clear_idle_target_spell_in_progress";
                return result;
            }
        }

        result.result = ActionResult::Success;
        result.reason = "clear_idle_target_live_preflight_passed";
        return result;
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        std::uint64_t const expectedSelection = context.bot.selectionGuid;
        if (!actor || resolved.target || candidate.targetGuid ||
            !expectedSelection ||
            actor->GetSelectionGuid().GetRawValue() != expectedSelection ||
            actor->GetTargetGuid().GetRawValue() != expectedSelection ||
            actor->IsInCombat() || actor->GetVictim() ||
            actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            result.result = ActionResult::Impossible;
            result.reason = "clear_idle_target_context_changed_before_dispatch";
            return result;
        }

        actor->SetSelectionGuid(ObjectGuid());
        bool const cleared = actor->GetSelectionGuid().IsEmpty() &&
            actor->GetTargetGuid().IsEmpty() && actor->GetVictim() == nullptr;
        result.result = cleared ? ActionResult::Success : ActionResult::Failed;
        result.reason = cleared
            ? "clear_idle_target_cleared"
            : "clear_idle_target_clear_failed";
        return result;
    }

private:
    std::string m_name = "core_clear_idle_target";
};
} // namespace PartyBotEngine

#endif
