#ifndef MANGOS_PARTYBOT_CANCEL_OWN_CAST_ACTION_H
#define MANGOS_PARTYBOT_CANCEL_OWN_CAST_ACTION_H

#include "CoreAction.h"

#include "Player.h"
#include "Spell.h"

namespace PartyBotEngine
{
// Cancel only the exact in-flight root Spell previously started by this
// PartyBotEngine bot. The snapshot carries action/encounter/target identity;
// execution resolves the current Player and Spell slot again on the map thread.
// No Spell* is retained in a snapshot, candidate, queue or continuation.
class CancelOwnCastAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        // Spell::cancel performs Core-owned cleanup but does not launch a new
        // damage/heal/threat effect. Existing cast effects remain Core-owned.
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "cancel_own_cast_only_cleans_up_the_existing_core_spell";
        return estimate;
    }

    bool IsPossible(
        DecisionContext const& context, ActionCandidate const& candidate,
        std::string& reason) const override
    {
        if (!context.bot.online || !context.bot.alive ||
            !context.bot.currentSpellEngineAction ||
            !context.bot.currentSpellActionId ||
            !candidate.spellId ||
            candidate.spellId != context.bot.currentSpellId ||
            candidate.targetGuid ||
            !IsSupportedContainer(context.bot.currentSpellType))
        {
            reason = "cancel_own_cast_missing_engine_owned_spell_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(
        DecisionContext const& context, ActionCandidate const& candidate,
        std::string& reason) const override
    {
        if (!candidate.spellId ||
            candidate.spellId != context.bot.currentSpellId ||
            !context.bot.currentSpellEngineAction ||
            !context.bot.currentSpellActionId ||
            !IsSupportedContainer(context.bot.currentSpellType) ||
            !IsCancellableState(context.bot.currentSpellState))
        {
            reason = "cancel_own_cast_spell_not_cancellable";
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
        CurrentSpellTypes const spellType =
            static_cast<CurrentSpellTypes>(context.bot.currentSpellType);
        ActionId const expectedActionId = context.bot.currentSpellActionId;

        // Use Core's canonical interrupt path. It handles re-entrant
        // cancellation and current-slot reference ownership. Never call
        // Spell::cancel directly or retain the Spell pointer across the call.
        actor->InterruptSpell(spellType, true);

        // A re-entrant replacement is safe: only the captured action ID counts
        // as the original. A different spell/action in the same slot is not it.
        Spell* remaining = actor->GetCurrentSpell(spellType);
        bool const originalStillCurrent = remaining &&
            remaining->IsPartyBotTraceEngineAction() &&
            remaining->GetPartyBotTraceActionId() == expectedActionId;

        result.result = originalStillCurrent
            ? ActionResult::Failed : ActionResult::Success;
        result.reason = originalStillCurrent
            ? "cancel_own_cast_core_left_original_spell_current"
            : "cancel_own_cast_core_interrupted_exact_spell";
        return result;
    }

private:
    static bool IsSupportedContainer(std::uint8_t spellType)
    {
        return spellType == CURRENT_GENERIC_SPELL ||
            spellType == CURRENT_CHANNELED_SPELL;
    }

    static bool IsCancellableState(std::uint32_t state)
    {
        return state == SPELL_STATE_PREPARING ||
            state == SPELL_STATE_CASTING ||
            state == SPELL_STATE_DELAYED;
    }

    static ActionExecutionResult Reject(ActionResult result, char const* reason)
    {
        ActionExecutionResult rejected;
        rejected.result = result;
        rejected.reason = reason;
        return rejected;
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        if (!resolved.actor || !resolved.map || resolved.target ||
            candidate.targetGuid || !candidate.spellId || !context.bot.guid ||
            context.bot.guid != resolved.actor->GetObjectGuid().GetRawValue() ||
            !context.bot.online || !context.bot.alive ||
            !resolved.actor->IsInWorld() || !resolved.actor->IsAlive() ||
            resolved.actor->GetMap() != resolved.map ||
            resolved.actor->GetMapId() != context.snapshot.mapId ||
            resolved.actor->GetInstanceId() != context.snapshot.instanceId ||
            !context.bot.currentSpellEngineAction ||
            !context.bot.currentSpellActionId ||
            candidate.spellId != context.bot.currentSpellId ||
            !IsSupportedContainer(context.bot.currentSpellType))
        {
            return Reject(
                ActionResult::Impossible,
                "cancel_own_cast_live_actor_or_snapshot_invalid");
        }

        CurrentSpellTypes const spellType =
            static_cast<CurrentSpellTypes>(context.bot.currentSpellType);
        Spell* spell = resolved.actor->GetCurrentSpell(spellType);
        if (!spell || !spell->m_spellInfo ||
            spell->GetCaster() != resolved.actor ||
            spell->m_spellInfo->Id != candidate.spellId ||
            !IsCancellableState(spell->getState()) ||
            !spell->IsPartyBotTraceEngineAction() ||
            spell->GetPartyBotTraceActionId() != context.bot.currentSpellActionId ||
            spell->GetPartyBotTraceEncounterId() !=
                context.bot.currentSpellEncounterId ||
            spell->GetPartyBotTraceTargetGuid() !=
                context.bot.currentSpellTraceTargetGuid ||
            spell->m_targets.getUnitTargetGuid().GetRawValue() !=
                context.bot.currentSpellTargetGuid)
        {
            return Reject(
                ActionResult::Impossible,
                "cancel_own_cast_live_spell_identity_mismatch");
        }

        return Reject(
            ActionResult::Success,
            "cancel_own_cast_live_spell_identity_verified");
    }

    std::string m_name = "core_cancel_own_cast";
};
} // namespace PartyBotEngine

#endif
