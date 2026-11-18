#ifndef MANGOS_PARTYBOT_PARTY_SNAPSHOT_H
#define MANGOS_PARTYBOT_PARTY_SNAPSHOT_H
#include "DecisionTypes.h"
namespace PartyBotEngine {
struct PartyMemberSnapshot {
    Guid guid = 0;
    std::uint8_t classId = 0, roleId = 0, level = 0, powerType = 0;
    std::uint32_t mapId = 0, instanceId = 0;
    std::uint32_t health = 0, maxHealth = 0, power = 0, maxPower = 0;
    Guid victimGuid = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool online = false, alive = false, inCombat = false, isMainTank = false;
};
struct PartyCompanionSnapshot {
    Guid guid = 0, ownerGuid = 0, victimGuid = 0;
    std::uint32_t mapId = 0, instanceId = 0;
    std::uint32_t health = 0, maxHealth = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool online = false, alive = false, inCombat = false;
};
struct ThreatEntrySnapshot {
    Guid unitGuid = 0;
    float threat = 0.0f;
    bool online = false, accessible = false, meleeReachable = false;
};
struct HostileSnapshot {
    Guid guid = 0, victimGuid = 0;
    std::uint32_t health = 0, maxHealth = 0;
    bool alive = false, crowdControlProtected = false;
    std::vector<ThreatEntrySnapshot> threat;
};
// Immutable-by-convention value data. Never store live Core object pointers here.
struct PartySnapshot {
    std::uint32_t groupId = 0, mapId = 0, instanceId = 0, epoch = 0, capturedAtMs = 0;
    std::vector<PartyMemberSnapshot> members;
    std::vector<PartyCompanionSnapshot> companions;
    std::vector<HostileSnapshot> hostiles;
    PartyMemberSnapshot const* FindMember(Guid guid) const {
        for (auto const& member : members) if (member.guid == guid) return &member;
        return nullptr;
    }
    PartyCompanionSnapshot const* FindCompanion(Guid guid) const {
        for (auto const& companion : companions) if (companion.guid == guid) return &companion;
        return nullptr;
    }
    HostileSnapshot const* FindHostile(Guid guid) const {
        for (auto const& hostile : hostiles) if (hostile.guid == guid) return &hostile;
        return nullptr;
    }
};
}
#endif
