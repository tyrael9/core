#ifndef MANGOS_PARTYBOT_FORMATION_H
#define MANGOS_PARTYBOT_FORMATION_H

#include "PartySnapshot.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace PartyBotEngine
{
// The deployed family-party target is one leader plus at most four followers.
// Slots are value-only and are owned by the shared (group,map,instance) coordinator.
enum : std::uint8_t
{
    InvalidFormationSlot = 0xff,
    MaxPartyFormationSlots = 4
};

// Keep this a C++14 aggregate so offset tables stay allocation-free.
struct FollowFormationOffset
{
    float distance;
    float angle;
};

inline FollowFormationOffset GetFollowFormationOffset(std::uint8_t slot)
{
    static float const Pi = 3.14159265358979323846f;
    static float const QuarterPi = 0.78539816339744830962f;
    switch (slot)
    {
        case 1: return {3.25f, Pi - QuarterPi};
        case 2: return {3.25f, Pi + QuarterPi};
        case 3: return {4.75f, Pi};
        case 0:
        default:
            // Slot zero and unknown slots preserve the original single-bot behavior.
            return {2.5f, Pi};
    }
}

struct FormationSlotAssignment
{
    Guid botGuid = 0;
    std::uint8_t slot = InvalidFormationSlot;
};

// Reconciled only while building a same-map PartySnapshot on its owning map
// thread. Existing assignments survive roster order and unrelated joins/leaves;
// a context change or removal of the assigned Bot releases only that assignment.
class FormationAssignments
{
public:
    void Reconcile(PartySnapshot const& snapshot)
    {
        if (!snapshot.groupId || !snapshot.groupLeaderGuid)
        {
            Reset();
            return;
        }

        bool const contextChanged =
            m_groupId != snapshot.groupId ||
            m_mapId != snapshot.mapId ||
            m_instanceId != snapshot.instanceId;
        if (contextChanged)
        {
            m_assignments.clear();
            m_groupId = snapshot.groupId;
            m_mapId = snapshot.mapId;
            m_instanceId = snapshot.instanceId;
        }

        std::vector<Guid> eligibleBots;
        eligibleBots.reserve(snapshot.members.size());
        for (PartyMemberSnapshot const& member : snapshot.members)
        {
            if (member.guid && member.guid != snapshot.groupLeaderGuid &&
                member.isPlayerBot && member.online &&
                member.mapId == snapshot.mapId &&
                member.instanceId == snapshot.instanceId)
                eligibleBots.push_back(member.guid);
        }
        std::sort(eligibleBots.begin(), eligibleBots.end());
        eligibleBots.erase(
            std::unique(eligibleBots.begin(), eligibleBots.end()),
            eligibleBots.end());

        m_assignments.erase(
            std::remove_if(m_assignments.begin(), m_assignments.end(),
                [&eligibleBots](FormationSlotAssignment const& assignment)
                {
                    return !std::binary_search(
                        eligibleBots.begin(), eligibleBots.end(), assignment.botGuid);
                }),
            m_assignments.end());

        bool occupied[MaxPartyFormationSlots] = {};
        for (FormationSlotAssignment const& assignment : m_assignments)
            if (assignment.slot < MaxPartyFormationSlots)
                occupied[assignment.slot] = true;

        for (Guid botGuid : eligibleBots)
        {
            bool alreadyAssigned = false;
            for (FormationSlotAssignment const& assignment : m_assignments)
                if (assignment.botGuid == botGuid)
                {
                    alreadyAssigned = true;
                    break;
                }
            if (alreadyAssigned)
                continue;

            std::uint8_t slot = 0;
            while (slot < MaxPartyFormationSlots && occupied[slot])
                ++slot;
            if (slot >= MaxPartyFormationSlots)
                break;

            FormationSlotAssignment assignment;
            assignment.botGuid = botGuid;
            assignment.slot = slot;
            m_assignments.push_back(assignment);
            occupied[slot] = true;
        }
    }

    std::uint8_t GetSlot(Guid botGuid) const
    {
        if (!botGuid)
            return InvalidFormationSlot;
        for (FormationSlotAssignment const& assignment : m_assignments)
            if (assignment.botGuid == botGuid)
                return assignment.slot;
        return InvalidFormationSlot;
    }

    void Reset()
    {
        m_assignments.clear();
        m_groupId = 0;
        m_mapId = 0;
        m_instanceId = 0;
    }

private:
    std::vector<FormationSlotAssignment> m_assignments;
    std::uint32_t m_groupId = 0;
    std::uint32_t m_mapId = 0;
    std::uint32_t m_instanceId = 0;
};
} // namespace PartyBotEngine

#endif
