#ifndef MANGOS_PARTYBOT_PRIEST_DIRECT_HEAL_ACTION_H
#define MANGOS_PARTYBOT_PRIEST_DIRECT_HEAL_ACTION_H

#include "CoreAction.h"

namespace PartyBotEngine
{
// Testable Core adapter for the first Priest direct-heal vertical slice.
// Its threat envelope intentionally remains Unknown until all relevant
// heal-triggered proc/secondary effects can be bounded; it is not registered
// by ProductionStrategies and therefore cannot cast through live NewEngine.
class PriestDirectHealAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "priest_direct_heal_threat_envelope_not_closed";
        return estimate;
    }

    bool IsPossible(
        DecisionContext const& context, ActionCandidate const& candidate,
        std::string& reason) const override
    {
        if (!context.bot.isPlayerBot || !context.bot.online || !context.bot.alive ||
            context.bot.classId != CLASS_PRIEST ||
            context.bot.roleId != static_cast<std::uint8_t>(PartyRole::Healer) ||
            context.bot.currentSpellId || !candidate.targetGuid || !candidate.spellId ||
            candidate.channel != ActionChannel::GlobalCooldown ||
            context.snapshot.mapId != context.bot.mapId ||
            context.snapshot.instanceId != context.bot.instanceId)
        {
            reason = "priest_direct_heal_invalid_healer_or_candidate";
            return false;
        }

        PartyMemberSnapshot const* target =
            context.snapshot.FindMember(candidate.targetGuid);
        if (!target || !target->online || !target->alive ||
            target->mapId != context.snapshot.mapId ||
            target->instanceId != context.snapshot.instanceId ||
            !target->maxHealth || target->health >= target->maxHealth)
        {
            reason = "priest_direct_heal_target_unavailable_or_full";
            return false;
        }

        bool const spellAvailable = std::any_of(
            context.bot.directHealSpells.begin(),
            context.bot.directHealSpells.end(),
            [&context, &candidate](DirectHealSpellSnapshot const& spell)
            {
                return spell.spellId == candidate.spellId &&
                    spell.baseHealing && spell.manaCost <= context.bot.power;
            });
        if (!spellAvailable)
        {
            reason = "priest_direct_heal_spell_unknown_or_unaffordable";
            return false;
        }

        return true;
    }

    bool IsUseful(
        DecisionContext const& context, ActionCandidate const& candidate,
        std::string& reason) const override
    {
        PartyMemberSnapshot const* target =
            context.snapshot.FindMember(candidate.targetGuid);
        if (!target || !target->maxHealth || target->health >= target->maxHealth)
        {
            reason = "priest_direct_heal_no_missing_health";
            return false;
        }
        return true;
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const&) const override
    {
        if (!context.coreActionRuntime ||
            !context.coreActionRuntime->CanCastDirectHeal(
                context, candidate.targetGuid, candidate.spellId))
            return Reject("priest_direct_heal_live_preflight_rejected");

        return Success("priest_direct_heal_live_preflight_passed");
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const&) override
    {
        if (!context.coreActionRuntime ||
            !context.coreActionRuntime->StartDirectHeal(
                context, candidate.targetGuid, candidate.spellId))
            return Reject("priest_direct_heal_core_cast_rejected");

        return Success("priest_direct_heal_core_cast_started");
    }

private:
    static ActionExecutionResult Reject(char const* reason)
    {
        ActionExecutionResult result;
        result.result = ActionResult::Impossible;
        result.reason = reason;
        return result;
    }

    static ActionExecutionResult Success(char const* reason)
    {
        ActionExecutionResult result;
        result.result = ActionResult::Success;
        result.reason = reason;
        return result;
    }

    std::string m_name = "core_priest_direct_heal";
};
} // namespace PartyBotEngine

#endif
