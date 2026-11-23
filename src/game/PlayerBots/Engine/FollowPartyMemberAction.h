#ifndef MANGOS_PARTYBOT_FOLLOW_PARTY_MEMBER_ACTION_H
#define MANGOS_PARTYBOT_FOLLOW_PARTY_MEMBER_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Group.h"
#include "MotionMaster.h"
#include "MovementGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "TargetedMovementGenerator.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Start or retarget the native Core Player FollowMovementGenerator to a live
// same-Group party member. Current follow identity is copied into the snapshot
// as a GUID so an unchanged request never restarts the movement generator.
class FollowPartyMemberAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "follow_movement_does_not_start_combat_or_generate_threat";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        PartyMemberSnapshot const* target =
            context.snapshot.FindMember(candidate.targetGuid);
        if (!context.bot.alive || context.bot.inCombat || context.bot.victimGuid ||
            context.bot.currentSpellId || !candidate.targetGuid ||
            candidate.targetGuid == context.bot.guid ||
            !target || !target->online || !target->alive ||
            target->mapId != context.snapshot.mapId ||
            target->instanceId != context.snapshot.instanceId)
        {
            reason = "follow_party_member_missing_or_invalid_snapshot_member";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!candidate.targetGuid ||
            candidate.targetGuid == context.bot.followTargetGuid)
        {
            reason = "follow_party_member_already_following_target";
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
        Player* target = resolved.target->ToPlayer();
        actor->GetMotionMaster()->MoveFollow(target, m_followDistance, m_followAngle);

        FollowMovementGenerator<Player> const* follow = GetCurrentFollow(actor);
        bool const started = follow && follow->GetTarget() == target &&
            actor->HasUnitState(UNIT_STATE_FOLLOW);
        result.result = started ? ActionResult::Success : ActionResult::Failed;
        result.reason = started
            ? "follow_party_member_started"
            : "follow_party_member_core_generator_not_started";
        return result;
    }

private:
    static FollowMovementGenerator<Player> const* GetCurrentFollow(Player* actor)
    {
        if (!actor || !actor->GetMotionMaster())
            return nullptr;
        MovementGenerator const* current = actor->GetMotionMaster()->GetCurrent();
        if (!current || current->GetMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            return nullptr;
        return static_cast<FollowMovementGenerator<Player> const*>(current);
    }

    static bool HasNonMeleeSpell(Player* actor)
    {
        if (!actor)
            return false;
        for (uint32 spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
                return true;
        return false;
    }

    static ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        Player* target = resolved.target ? resolved.target->ToPlayer() : nullptr;
        PartyMemberSnapshot const* targetSnapshot =
            context.snapshot.FindMember(candidate.targetGuid);
        Group* actorGroup = actor ? actor->GetGroup() : nullptr;

        if (!actor || !target || resolved.target != target ||
            !candidate.targetGuid || candidate.targetGuid == context.bot.guid ||
            target->GetObjectGuid().GetRawValue() != candidate.targetGuid ||
            !targetSnapshot || !targetSnapshot->online || !targetSnapshot->alive ||
            targetSnapshot->guid != candidate.targetGuid ||
            targetSnapshot->mapId != context.snapshot.mapId ||
            targetSnapshot->instanceId != context.snapshot.instanceId ||
            !actor->IsInWorld() || !target->IsInWorld() ||
            !actor->IsAlive() || !target->IsAlive() ||
            actor->GetMap() != resolved.map || target->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            target->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            target->GetInstanceId() != context.snapshot.instanceId ||
            !actorGroup || actorGroup->isBGGroup() ||
            actorGroup->GetId() != context.snapshot.groupId ||
            !actorGroup->IsMember(actor->GetObjectGuid()) ||
            target->GetGroup() != actorGroup ||
            !actorGroup->IsMember(target->GetObjectGuid()) ||
            !actor->IsFriendlyTo(target))
        {
            result.result = ActionResult::Impossible;
            result.reason = "follow_party_member_live_group_or_target_invalid";
            return result;
        }

        if (actor->HasUnitState(UNIT_STATE_LOST_CONTROL |
                                UNIT_STATE_CAN_NOT_MOVE |
                                UNIT_STATE_POSSESSED))
        {
            result.result = ActionResult::Impossible;
            result.reason = "follow_party_member_actor_cannot_move";
            return result;
        }

        if (actor->IsInCombat() || actor->GetVictim() || HasNonMeleeSpell(actor))
        {
            result.result = ActionResult::Useless;
            result.reason = actor->IsInCombat()
                ? "follow_party_member_actor_in_combat"
                : (actor->GetVictim()
                    ? "follow_party_member_actor_has_combat_victim"
                    : "follow_party_member_actor_casting");
            return result;
        }

        FollowMovementGenerator<Player> const* currentFollow = GetCurrentFollow(actor);
        if (currentFollow && currentFollow->GetTarget() == target)
        {
            result.result = ActionResult::Useless;
            result.reason = "follow_party_member_same_target_already_active";
            return result;
        }

        if (!currentFollow && !actor->GetMotionMaster()->IsUsingIdleOrDefaultMovement())
        {
            result.result = ActionResult::Useless;
            result.reason = "follow_party_member_unrelated_movement_active";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "follow_party_member_live_preflight_passed";
        return result;
    }

    float m_followDistance = 2.5f;
    float m_followAngle = 3.14159265358979323846f;
    std::string m_name = "core_follow_party_member";
};
} // namespace PartyBotEngine

#endif
