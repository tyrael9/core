#ifndef MANGOS_PARTYBOT_RESURRECTION_POLICY_H
#define MANGOS_PARTYBOT_RESURRECTION_POLICY_H

#include "PartySnapshot.h"

namespace PartyBotEngine
{
// Vanilla 1.12 class IDs. Keep this value-only policy independent of Core
// object headers so the exact legacy class-level suppression rule is unit-testable.
inline bool IsResurrectionHealerClass(std::uint8_t classId)
{
    switch (classId)
    {
        case 2:  // Paladin
        case 5:  // Priest
        case 7:  // Shaman
        case 11: // Druid
            return true;
        default:
            return false;
    }
}

// Preserve PartyBotAI::ShouldAutoRevive semantics without dereferencing Core
// objects: a released ghost may always self-resurrect; a corpse may only do so
// when no online party member is in combat, no living healer-class member is
// present, and a living party member is nearby on the same map/instance.
inline bool ShouldAutoResurrectSelf(
    PartySnapshot const& snapshot, PartyMemberSnapshot const& bot)
{
    if (!bot.guid || !bot.online || bot.alive)
        return false;

    if (bot.deathState == PartyMemberDeathState::ReleasedGhost)
        return true;

    if (bot.deathState != PartyMemberDeathState::Corpse)
        return false;

    bool alivePlayerNearby = false;
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (!member.guid || member.guid == bot.guid || !member.online)
            continue;

        if (member.inCombat)
            return false;

        if (!member.alive)
            continue;

        if (IsResurrectionHealerClass(member.classId))
            return false;

        if (member.mapId != snapshot.mapId ||
            member.instanceId != snapshot.instanceId)
            continue;

        float const dx = member.x - bot.x;
        float const dy = member.y - bot.y;
        float const dz = member.z - bot.z;
        if (dx * dx + dy * dy + dz * dz <= 225.0f)
            alivePlayerNearby = true;
    }

    return alivePlayerNearby;
}
} // namespace PartyBotEngine

#endif
