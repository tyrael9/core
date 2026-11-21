#ifndef MANGOS_PARTYBOT_CLEAR_STALE_TARGET_ACTION_H
#define MANGOS_PARTYBOT_CLEAR_STALE_TARGET_ACTION_H

#include "CoreAction.h"

#include "Map.h"
#include "Player.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Clear a selection whose Unit has disappeared, died or left the actor's Map.
// Unlike ClearIdleTargetAction this can run during another live encounter victim:
// it never stops/retargets that victim, clears combat state or changes threat.
class ClearStaleTargetAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "clear_stale_selection_only_preserves_victim_and_combat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        if (!context.bot.alive || candidate.targetGuid ||
            !context.bot.selectionGuid ||
            context.bot.selectionGuid == context.bot.guid ||
            context.bot.selectionGuid == context.bot.victimGuid ||
            (!context.bot.inCombat && !context.bot.victimGuid))
        {
            reason = "clear_stale_target_not_possible_from_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (candidate.targetGuid || !context.bot.selectionGuid ||
            context.bot.selectionGuid == context.bot.victimGuid ||
            (!context.bot.inCombat && !context.bot.victimGuid))
        {
            reason = "clear_stale_target_not_actionable";
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
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        bool const meleeBefore = actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
        uint32 const selectedHealthBefore = resolved.map->GetUnit(
            ObjectGuid(context.bot.selectionGuid))
                ? resolved.map->GetUnit(ObjectGuid(context.bot.selectionGuid))->GetHealth()
                : 0;

        // Player::SetSelectionGuid only updates the selection and UNIT_FIELD_TARGET
        // GUIDs. The current Unit::m_attacking pointer and its attacker reference
        // are deliberately left untouched.
        actor->SetSelectionGuid(ObjectGuid());

        Unit* const selectedAfter = resolved.map->GetUnit(
            ObjectGuid(context.bot.selectionGuid));
        uint32 const selectedHealthAfter = selectedAfter
            ? selectedAfter->GetHealth() : 0;
        bool const cleared =
            actor->GetSelectionGuid().IsEmpty() && actor->GetTargetGuid().IsEmpty();
        bool const combatPreserved =
            actor->GetVictim() == victimBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == meleeBefore;
        bool const staleTargetUnchanged =
            selectedHealthAfter == selectedHealthBefore;

        bool const passed = cleared && combatPreserved && staleTargetUnchanged;
        result.result = passed ? ActionResult::Success : ActionResult::Failed;
        result.reason = passed
            ? "clear_stale_target_cleared_selection_preserving_combat"
            : "clear_stale_target_postcondition_failed";
        return result;
    }

private:
    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Guid const expectedSelection = context.bot.selectionGuid;
        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !expectedSelection || expectedSelection == context.bot.guid ||
            expectedSelection == context.bot.victimGuid ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            actor->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            actor->GetSelectionGuid().GetRawValue() != expectedSelection ||
            actor->GetTargetGuid().GetRawValue() != expectedSelection)
        {
            result.result = ActionResult::Impossible;
            result.reason = "clear_stale_target_actor_or_selection_mismatch";
            return result;
        }

        Unit* const liveVictim = actor->GetVictim();
        Guid const liveVictimGuid = liveVictim
            ? liveVictim->GetObjectGuid().GetRawValue() : 0;
        if (liveVictimGuid != context.bot.victimGuid ||
            actor->IsInCombat() != context.bot.inCombat)
        {
            result.result = ActionResult::Impossible;
            result.reason = "clear_stale_target_combat_snapshot_mismatch";
            return result;
        }

        if (liveVictimGuid == expectedSelection)
        {
            result.result = ActionResult::Useless;
            result.reason = "clear_stale_target_is_current_victim";
            return result;
        }

        if (!actor->IsInCombat() && !liveVictim)
        {
            result.result = ActionResult::Useless;
            result.reason = "clear_stale_target_idle_owned_by_clear_idle_action";
            return result;
        }

        Unit* const selected = resolved.map->GetUnit(ObjectGuid(expectedSelection));
        if (selected && selected->IsInWorld() &&
            selected->GetMap() == resolved.map &&
            selected->GetMapId() == context.snapshot.mapId &&
            selected->GetInstanceId() == context.snapshot.instanceId &&
            selected->IsAlive())
        {
            result.result = ActionResult::Useless;
            result.reason = "clear_stale_target_selection_still_resolves_live";
            return result;
        }

        // A spell may have retained the selected target GUID independently of
        // the Player's current selection. Leave the GUID untouched until all
        // current non-melee spell slots have terminated.
        for (uint32 spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
            {
                result.result = ActionResult::Useless;
                result.reason = "clear_stale_target_spell_in_progress";
                return result;
            }
        }

        result.result = ActionResult::Success;
        result.reason = "clear_stale_target_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_clear_stale_target";
};
} // namespace PartyBotEngine

#endif
