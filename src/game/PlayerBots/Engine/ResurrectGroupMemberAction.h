#ifndef MANGOS_PARTYBOT_RESURRECT_GROUP_MEMBER_ACTION_H
#define MANGOS_PARTYBOT_RESURRECT_GROUP_MEMBER_ACTION_H

#include "CoreAction.h"

#include "../CombatBotBaseAI.h"
#include "Group.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"

namespace PartyBotEngine
{
// Cast the Bot's learned native resurrection spell on a live same-Group Corpse.
// Core owns the spell/cast/offer/accept lifecycle; the coordinator reservation
// only prevents concurrent PartyBots from selecting the same corpse.
class ResurrectGroupMemberAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "group_resurrection_spell_does_not_generate_hostile_threat";
        return estimate;
    }

    bool IsPossible(
        DecisionContext const& context, ActionCandidate const& candidate,
        std::string& reason) const override
    {
        PartyMemberSnapshot const* target = context.snapshot.FindMember(candidate.targetGuid);
        if (!context.bot.online || !context.bot.alive || context.bot.inDuel ||
            context.bot.inCombat || context.bot.victimGuid || context.bot.currentSpellId ||
            !context.bot.resurrectionSpellId ||
            candidate.spellId != context.bot.resurrectionSpellId ||
            !candidate.targetGuid || candidate.targetGuid == context.bot.guid ||
            !target || !target->online || target->alive ||
            target->deathState != PartyMemberDeathState::Corpse ||
            target->resurrectionPending ||
            target->mapId != context.snapshot.mapId ||
            target->instanceId != context.snapshot.instanceId)
        {
            reason = "resurrect_group_member_invalid_snapshot";
            return false;
        }
        if (context.coordinator.HasReservation(
                ReservationType::Resurrection, candidate.targetGuid, 0, context.nowMs))
        {
            reason = "resurrect_group_member_already_reserved";
            return false;
        }
        return true;
    }

    bool IsUseful(
        DecisionContext const& context, ActionCandidate const& candidate,
        std::string& reason) const override
    {
        if (context.coordinator.HasReservation(
                ReservationType::Resurrection, candidate.targetGuid, 0, context.nowMs))
        {
            reason = "resurrect_group_member_already_reserved";
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

        Player* const actor = resolved.actor;
        Player* const target = resolved.target->ToPlayer();
        CombatBotBaseAI* const combatAI =
            dynamic_cast<CombatBotBaseAI*>(actor->AI());
        SpellEntry const* const spell = combatAI ? combatAI->m_resurrectionSpell : nullptr;
        Guid const ownerGuid = actor->GetObjectGuid().GetRawValue();

        Reservation reservation;
        reservation.type = ReservationType::Resurrection;
        reservation.targetGuid = candidate.targetGuid;
        reservation.ownerGuid = ownerGuid;
        reservation.spellId = candidate.spellId;
        reservation.expiresAtMs = context.nowMs + 60000u;
        if (!context.coordinator.TryReserve(reservation, context.nowMs))
        {
            result.result = ActionResult::Useless;
            result.reason = "resurrect_group_member_reservation_race";
            return result;
        }

        actor->SetFacingToObject(target);
        actor->SetTargetGuid(target->GetObjectGuid());
        SpellCastResult const castResult = actor->CastSpell(target, spell, false);
        if (castResult != SPELL_CAST_OK)
        {
            context.coordinator.ReleaseReservation(
                ReservationType::Resurrection, candidate.targetGuid, ownerGuid);
            result.result = ActionResult::Failed;
            result.reason = "resurrect_group_member_core_cast_rejected";
            return result;
        }

        // The exact (caster,target) reservation stays until Core's Spell
        // terminal callback; a 60-second expiry bounds missing/cancelled events.
        context.coordinator.InvalidateSnapshot();
        result.result = ActionResult::Success;
        result.reason = "resurrect_group_member_core_cast_started";
        return result;
    }

private:
    static bool HasNonMeleeSpell(Player* actor)
    {
        if (!actor)
            return false;
        for (uint32 index = CURRENT_FIRST_NON_MELEE_SPELL;
             index < CURRENT_MAX_SPELL; ++index)
            if (actor->GetCurrentSpell(CurrentSpellTypes(index)))
                return true;
        return false;
    }

    static ActionExecutionResult Reject(char const* reason)
    {
        ActionExecutionResult result;
        result.result = ActionResult::Impossible;
        result.reason = reason;
        return result;
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        Player* const actor = resolved.actor;
        Player* const target = resolved.target ? resolved.target->ToPlayer() : nullptr;
        CombatBotBaseAI* const combatAI =
            actor ? dynamic_cast<CombatBotBaseAI*>(actor->AI()) : nullptr;
        Group* const group = actor ? actor->GetGroup() : nullptr;
        SpellEntry const* const spell = combatAI ? combatAI->m_resurrectionSpell : nullptr;

        if (!actor || !target || resolved.target != target || !resolved.map ||
            !candidate.targetGuid || candidate.targetGuid == context.bot.guid ||
            target->GetObjectGuid().GetRawValue() != candidate.targetGuid ||
            !actor->IsInWorld() || !target->IsInWorld() ||
            !actor->IsAlive() || target->IsAlive() ||
            actor->GetDeathState() != ALIVE || target->GetDeathState() != CORPSE ||
            actor->InBattleGround() || actor->IsMounted() ||
            !combatAI || !spell || spell->Id != candidate.spellId ||
            context.bot.resurrectionSpellId != candidate.spellId ||
            combatAI->IsInDuel() ||
            actor->IsInCombat() || actor->GetVictim() || HasNonMeleeSpell(actor) ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId ||
            !group || group->isBGGroup() || group->GetId() != context.snapshot.groupId ||
            !group->IsMember(actor->GetObjectGuid()) ||
            target->GetGroup() != group || !group->IsMember(target->GetObjectGuid()) ||
            target->IsRessurectRequested() ||
            !actor->IsWithinLOSInMap(target) ||
            !spell->IsTargetInRange(actor, target) ||
            !combatAI->CanTryToCastSpell(target, spell))
            return Reject("resurrect_group_member_live_preflight_rejected");

        ActionExecutionResult result;
        result.result = ActionResult::Success;
        result.reason = "resurrect_group_member_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_resurrect_group_member";
};
} // namespace PartyBotEngine

#endif
