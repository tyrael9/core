#ifndef MANGOS_PARTYBOT_FOLLOW_PARTY_LEADER_TRIGGER_H
#define MANGOS_PARTYBOT_FOLLOW_PARTY_LEADER_TRIGGER_H
#include "Trigger.h"
#include "BotBehaviorState.h"
#include "Formation.h"
namespace PartyBotEngine
{
class FollowPartyLeaderTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetCheckIntervalMs() const override { return 0; }
    bool IsActive(DecisionContext const& context) const override
    {
        bool const stateAllowsFollow =
            context.botBehaviorState == BotBehaviorState::Idle ||
            context.botBehaviorState == BotBehaviorState::FollowingPartyMember ||
            context.botBehaviorState == BotBehaviorState::FollowingUnavailableTarget;
        return stateAllowsFollow && context.bot.online && context.bot.alive &&
            !context.bot.inCombat && !context.bot.victimGuid &&
            !context.bot.currentSpellId;
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        if (!IsActive(context))
            return;
        Guid const leaderGuid = context.snapshot.groupLeaderGuid;
        if (!leaderGuid || leaderGuid == context.bot.guid ||
            leaderGuid == context.bot.followTargetGuid)
            return;
        PartyMemberSnapshot const* leader = context.snapshot.FindMember(leaderGuid);
        if (!leader || !leader->online || !leader->alive ||
            leader->mapId != context.snapshot.mapId ||
            leader->instanceId != context.snapshot.instanceId)
            return;
        std::uint8_t const formationSlot =
            context.coordinator.GetFormationSlot(context.bot.guid);
        if (formationSlot == InvalidFormationSlot)
            return;

        ActionCandidate candidate;
        candidate.actionName = "core_follow_party_member";
        candidate.targetGuid = leaderGuid;
        candidate.formationSlot = formationSlot;
        candidate.basePriority = 50.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }
private:
    std::string m_name = "follow_party_leader";
};
}
#endif
