#ifndef MANGOS_PARTYBOT_AUTO_RESURRECT_SELF_ACTION_H
#define MANGOS_PARTYBOT_AUTO_RESURRECT_SELF_ACTION_H

#include "CoreAction.h"
#include "ResurrectionPolicy.h"

#include "Group.h"
#include "Player.h"
#include "Spell.h"

namespace PartyBotEngine
{
// Execute the existing host's automatic resurrection behavior through the
// exclusive NewEngine action path. Core still owns corpse creation, spirit
// release, graveyard relocation and the actual Player resurrection transition.
class AutoResurrectSelfAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "self_resurrection_does_not_generate_hostile_threat";
        return estimate;
    }

    bool IsPossible(
        DecisionContext const& context, ActionCandidate const&,
        std::string& reason) const override
    {
        if (!ShouldAutoResurrectSelf(context.snapshot, context.bot))
        {
            reason = "auto_resurrect_self_policy_not_satisfied";
            return false;
        }
        return true;
    }

    bool IsUseful(
        DecisionContext const& context, ActionCandidate const&,
        std::string& reason) const override
    {
        if (context.bot.alive)
        {
            reason = "auto_resurrect_self_actor_already_alive";
            return false;
        }
        return true;
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const&,
        CoreActionResolution const& resolved) const override
    {
        return CheckLiveState(context, resolved);
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const&,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result = CheckLiveState(context, resolved);
        if (result.result != ActionResult::Success)
            return result;

        Player* const actor = resolved.actor;
        actor->ResurrectPlayer(0.5f);
        actor->SpawnCorpseBones();
        actor->CastSpell(actor, 2479, true); // Honorless Target, matching Legacy.

        bool const resurrected = actor->IsAlive() &&
            actor->GetDeathState() == ALIVE;
        result.result = resurrected ? ActionResult::Success : ActionResult::Failed;
        result.reason = resurrected
            ? "auto_resurrect_self_core_resurrection_completed"
            : "auto_resurrect_self_core_resurrection_failed";
        return result;
    }

private:
    static ActionExecutionResult Reject(char const* reason)
    {
        ActionExecutionResult result;
        result.result = ActionResult::Impossible;
        result.reason = reason;
        return result;
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, CoreActionResolution const& resolved)
    {
        Player* const actor = resolved.actor;
        if (!actor || resolved.target || !actor->IsInWorld() ||
            actor->IsAlive() || actor->InBattleGround() ||
            !actor->GetMap() ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            actor->GetObjectGuid().GetRawValue() != context.bot.guid)
            return Reject("auto_resurrect_self_actor_or_map_invalid");

        DeathState const deathState = actor->GetDeathState();
        if (deathState != CORPSE && deathState != DEAD)
            return Reject("auto_resurrect_self_core_death_state_not_eligible");

        PartyMemberDeathState const snapshotDeathState = context.bot.deathState;
        bool const snapshotMatches =
            (deathState == CORPSE && snapshotDeathState == PartyMemberDeathState::Corpse) ||
            (deathState == DEAD && snapshotDeathState == PartyMemberDeathState::ReleasedGhost);
        if (!snapshotMatches)
            return Reject("auto_resurrect_self_death_state_snapshot_stale");

        Group* const group = actor->GetGroup();
        if (!group || group->isBGGroup() || group->GetId() != context.snapshot.groupId ||
            !group->IsMember(actor->GetObjectGuid()))
            return Reject("auto_resurrect_self_group_invalid");

        // Re-evaluate the complete live Group, including members on another
        // map. Snapshot eligibility can be stale or omit remote members.
        if (deathState == CORPSE)
        {
            bool alivePlayerNearby = false;
            for (GroupReference* itr = group->GetFirstMember();
                 itr != nullptr; itr = itr->next())
            {
                Player* const member = itr->getSource();
                if (!member || member == actor)
                    continue;
                if (member->IsInCombat())
                    return Reject("auto_resurrect_self_party_member_in_combat");
                if (!member->IsAlive())
                    continue;
                if (CombatBotBaseAI::IsHealerClass(member->GetClass()))
                    return Reject("auto_resurrect_self_living_healer_present");
                if (actor->IsWithinDistInMap(member, 15.0f))
                    alivePlayerNearby = true;
            }
            if (!alivePlayerNearby)
                return Reject("auto_resurrect_self_no_living_party_member_nearby");
        }

        ActionExecutionResult result;
        result.result = ActionResult::Success;
        result.reason = "auto_resurrect_self_live_preflight_passed";
        return result;
    }

    std::string m_name = "core_resurrect_self";
};
} // namespace PartyBotEngine

#endif
