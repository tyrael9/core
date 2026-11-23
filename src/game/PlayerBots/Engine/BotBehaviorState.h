#ifndef MANGOS_PARTYBOT_BEHAVIOR_STATE_H
#define MANGOS_PARTYBOT_BEHAVIOR_STATE_H
#include "PartySnapshot.h"
namespace PartyBotEngine
{
enum class BotBehaviorState : std::uint8_t
{
    Unknown = 0, Dead, Corpse, ReleasedGhost, Resurrecting, Combat,
    FollowingLeader, FollowingPartyMember, FollowingUnavailableTarget, Idle
};
static std::uint32_t const InvalidFollowTargetTimeoutMs = 4000;
inline char const* BotBehaviorStateName(BotBehaviorState state)
{
    switch (state)
    {
        case BotBehaviorState::Dead: return "dead";
        case BotBehaviorState::Corpse: return "corpse";
        case BotBehaviorState::ReleasedGhost: return "released_ghost";
        case BotBehaviorState::Resurrecting: return "resurrecting";
        case BotBehaviorState::Combat: return "combat";
        case BotBehaviorState::FollowingLeader: return "following_leader";
        case BotBehaviorState::FollowingPartyMember: return "following_party_member";
        case BotBehaviorState::FollowingUnavailableTarget: return "following_unavailable_target";
        case BotBehaviorState::Idle: return "idle";
        default: return "unknown";
    }
}
inline BotBehaviorState ResolveBotBehaviorState(
    PartySnapshot const& snapshot, PartyMemberSnapshot const& bot)
{
    if (!bot.online) return BotBehaviorState::Dead;
    if (!bot.alive)
    {
        switch (bot.deathState)
        {
            case PartyMemberDeathState::Dying:
            case PartyMemberDeathState::Corpse:
            case PartyMemberDeathState::CorpseFalling:
                return BotBehaviorState::Corpse;
            case PartyMemberDeathState::ReleasedGhost:
                return BotBehaviorState::ReleasedGhost;
            case PartyMemberDeathState::Resurrecting:
                return BotBehaviorState::Resurrecting;
            default:
                // Preserve the original unknown/dead fallback for value-only
                // callers that have not populated the new Core death-state field.
                return BotBehaviorState::Dead;
        }
    }
    if (bot.inCombat || bot.victimGuid) return BotBehaviorState::Combat;
    if (!bot.followTargetGuid) return BotBehaviorState::Idle;
    PartyMemberSnapshot const* target = snapshot.FindMember(bot.followTargetGuid);
    bool const available = target && target->online && target->alive &&
        target->mapId == snapshot.mapId && target->instanceId == snapshot.instanceId;
    if (!available) return BotBehaviorState::FollowingUnavailableTarget;
    return bot.followTargetGuid == snapshot.groupLeaderGuid
        ? BotBehaviorState::FollowingLeader : BotBehaviorState::FollowingPartyMember;
}
struct BotBehaviorStateObservation
{
    BotBehaviorState previousState = BotBehaviorState::Unknown;
    BotBehaviorState state = BotBehaviorState::Unknown;
    std::uint32_t stateSinceMs = 0, elapsedMs = 0;
    bool changed = false;
    char const* reason = "none";
    bool HasTimedOut(std::uint32_t timeoutMs) const
    {
        return timeoutMs && elapsedMs >= timeoutMs;
    }
};
inline char const* BotBehaviorStateTransitionReason(
    BotBehaviorState previous, BotBehaviorState current)
{
    if (current == BotBehaviorState::ReleasedGhost) return "spirit_released";
    if (current == BotBehaviorState::Corpse) return "became_corpse";
    if (current == BotBehaviorState::Resurrecting) return "resurrection_started";
    if (previous == BotBehaviorState::ReleasedGhost) return "ghost_resurrected";
    if (previous == BotBehaviorState::Corpse) return "corpse_resurrected";
    if (current == BotBehaviorState::Dead) return "became_dead";
    if (previous == BotBehaviorState::Dead) return "revived";
    if (current == BotBehaviorState::Combat) return "combat_entered";
    if (previous == BotBehaviorState::Combat) return "combat_ended";
    if (current == BotBehaviorState::FollowingUnavailableTarget) return "follow_target_unavailable";
    if (current == BotBehaviorState::FollowingLeader) return "following_group_leader";
    if (current == BotBehaviorState::FollowingPartyMember) return "following_party_member";
    if (current == BotBehaviorState::Idle) return "became_idle";
    return "state_changed";
}
class BotBehaviorStateMachine
{
public:
    BotBehaviorStateObservation Observe(
        PartySnapshot const& snapshot, PartyMemberSnapshot const& bot, std::uint32_t nowMs)
    {
        BotBehaviorStateObservation observation;
        observation.previousState = m_state;
        BotBehaviorState const resolved = ResolveBotBehaviorState(snapshot, bot);
        bool const contextChanged = m_initialized &&
            (m_groupId != snapshot.groupId || m_mapId != snapshot.mapId ||
             m_instanceId != snapshot.instanceId);
        if (!m_initialized || contextChanged || resolved != m_state)
        {
            observation.changed = true;
            observation.reason = !m_initialized ? "initial_state_observed"
                : (contextChanged ? "party_context_changed"
                                  : BotBehaviorStateTransitionReason(m_state, resolved));
            m_state = resolved;
            m_stateSinceMs = nowMs;
        }
        m_initialized = true;
        m_groupId = snapshot.groupId;
        m_mapId = snapshot.mapId;
        m_instanceId = snapshot.instanceId;
        observation.state = m_state;
        observation.stateSinceMs = m_stateSinceMs;
        observation.elapsedMs = nowMs - m_stateSinceMs;
        return observation;
    }
    BotBehaviorState GetState() const { return m_state; }

    void Reset()
    {
        m_initialized = false;
        m_state = BotBehaviorState::Unknown;
        m_stateSinceMs = m_groupId = m_mapId = m_instanceId = 0;
    }
private:
    bool m_initialized = false;
    BotBehaviorState m_state = BotBehaviorState::Unknown;
    std::uint32_t m_stateSinceMs = 0, m_groupId = 0, m_mapId = 0, m_instanceId = 0;
};
}
#endif
