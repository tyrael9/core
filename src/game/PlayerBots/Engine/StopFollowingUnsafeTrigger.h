#ifndef MANGOS_PARTYBOT_STOP_FOLLOWING_UNSAFE_TRIGGER_H
#define MANGOS_PARTYBOT_STOP_FOLLOWING_UNSAFE_TRIGGER_H
#include "Trigger.h"
#include "BotBehaviorState.h"
namespace PartyBotEngine
{
// Stop native Follow on combat entry or when this Bot is promoted to Group Leader.
// If the current target left the party and no usable leader can be followed,
// allow a short grace interval before cleanup.
class StopFollowingUnsafeTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetCheckIntervalMs() const override { return 0; }
    bool IsActive(DecisionContext const& context) const override
    {
        if (!context.bot.online || !context.bot.alive || !context.bot.followTargetGuid)
            return false;
        // A promoted leader must not keep following the previous leader (or
        // another party member). Remaining followers independently retarget
        // through FollowPartyLeaderTrigger on their next decision cycle.
        if (context.snapshot.groupLeaderGuid == context.bot.guid)
            return true;
        if (context.botBehaviorState == BotBehaviorState::Combat)
            return true;
        if (context.botBehaviorState != BotBehaviorState::FollowingUnavailableTarget ||
            context.botBehaviorStateElapsedMs < InvalidFollowTargetTimeoutMs)
            return false;
        PartyMemberSnapshot const* leader =
            context.snapshot.FindMember(context.snapshot.groupLeaderGuid);
        bool const leaderAvailable = leader && leader->online && leader->alive &&
            leader->mapId == context.snapshot.mapId &&
            leader->instanceId == context.snapshot.instanceId;
        return !leaderAvailable;
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        if (!IsActive(context)) return;
        ActionCandidate candidate;
        candidate.actionName = "core_stop_following";
        candidate.basePriority = context.botBehaviorState == BotBehaviorState::Combat
            ? 100.0f
            : (context.snapshot.groupLeaderGuid == context.bot.guid ? 90.0f : 80.0f);
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }
private:
    std::string m_name = "stop_following_unsafe";
};
}
#endif
