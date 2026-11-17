#include "PartySnapshotBuilder.h"
#include "CombatBotBaseAI.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "ThreatManager.h"
#include "Unit.h"
#include <map>
#include <utility>

namespace PartyBotEngine {

namespace {
PartyRole ToPartyRole(CombatBotRoles role)
{
    switch (role)
    {
        case ROLE_TANK: return PartyRole::Tank;
        case ROLE_HEALER: return PartyRole::Healer;
        case ROLE_RANGE_DPS: return PartyRole::RangedDamage;
        case ROLE_MELEE_DPS: return PartyRole::MeleeDamage;
        default: return PartyRole::Unknown;
    }
}
}

PartySnapshot const* PartySnapshotBuilder::GetOrBuild(Player* bot, PartyCoordinator& coordinator,
                                                       std::uint32_t nowMs, std::uint32_t maxAgeMs)
{
    if (PartySnapshot const* cached = coordinator.GetSnapshotIfFresh(nowMs, maxAgeMs))
        return cached;
    PartySnapshot snapshot;
    if (!Build(bot, snapshot, coordinator.NextSnapshotEpoch(), nowMs))
        return nullptr;
    coordinator.SetSnapshot(std::move(snapshot));
    return coordinator.GetSnapshotIfFresh(nowMs, maxAgeMs);
}

bool PartySnapshotBuilder::Build(Player* bot, PartySnapshot& snapshot,
                                  std::uint32_t epoch, std::uint32_t nowMs)
{
    if (!bot || !bot->IsInWorld())
        return false;
    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map || group->isBGGroup() || !group->GetId())
        return false;

    snapshot.groupId = group->GetId();
    snapshot.mapId = bot->GetMapId();
    snapshot.instanceId = bot->GetInstanceId();
    snapshot.epoch = epoch;
    snapshot.capturedAtMs = nowMs;

    ObjectGuid const mainTankGuid = group->GetMainTankGuid();
    std::map<Guid, HostileSnapshot> hostiles;

    // All data is copied into GUID/scalar values. Live pointers remain local to
    // this map update and never enter the cached PartySnapshot.
    auto addHostile = [&](Unit* hostileUnit)
    {
        if (!hostileUnit || !hostileUnit->IsInWorld() ||
            hostileUnit->GetMap() != map ||
            group->IsMember(hostileUnit->GetObjectGuid()))
            return;

        Guid const hostileGuid = hostileUnit->GetObjectGuid().GetRawValue();
        auto inserted = hostiles.emplace(hostileGuid, HostileSnapshot());
        if (!inserted.second)
            return;

        HostileSnapshot& hostile = inserted.first->second;
        hostile.guid = hostileGuid;
        hostile.victimGuid = hostileUnit->GetVictim()
            ? hostileUnit->GetVictim()->GetObjectGuid().GetRawValue() : 0;
        hostile.health = hostileUnit->GetHealth();
        hostile.maxHealth = hostileUnit->GetMaxHealth();
        hostile.alive = hostileUnit->IsAlive();

        ThreatList const& threatList = hostileUnit->GetThreatManager().getThreatList();
        for (HostileReference* reference : threatList)
        {
            if (!reference)
                continue;
            ThreatEntrySnapshot threat;
            threat.unitGuid = reference->getUnitGuid().GetRawValue();
            threat.threat = reference->getThreat();
            threat.online = reference->isOnline();
            threat.accessible = reference->isAccessable();
            Unit* threatTarget = reference->getTarget();
            threat.meleeReachable = threatTarget &&
                hostileUnit->CanReachWithMeleeAutoAttack(threatTarget);
            hostile.threat.push_back(threat);
        }
    };

    // Group reads are safe in PACKET_PROCESS_MAP. Resolve only GUIDs on this
    // Map; never dereference a GroupReference Player* from another map.
    for (Group::MemberSlotList::const_iterator itr = group->GetMemberSlots().begin();
         itr != group->GetMemberSlots().end(); ++itr)
    {
        Unit* unit = map->GetUnit(itr->guid);
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
            continue;
        Player* member = static_cast<Player*>(unit);
        if (member->GetMapId() != snapshot.mapId || member->GetInstanceId() != snapshot.instanceId)
            continue;

        PartyMemberSnapshot memberData;
        memberData.guid = member->GetObjectGuid().GetRawValue();
        memberData.classId = member->GetClass();
        memberData.roleId = static_cast<std::uint8_t>(PartyRole::Unknown);
        if (CombatBotBaseAI* combatAI = dynamic_cast<CombatBotBaseAI*>(member->AI()))
            memberData.roleId = static_cast<std::uint8_t>(ToPartyRole(combatAI->GetRole()));
        memberData.isMainTank = member->GetObjectGuid() == mainTankGuid;
        memberData.level = member->GetLevel();
        memberData.powerType = static_cast<std::uint8_t>(member->GetPowerType());
        memberData.mapId = member->GetMapId();
        memberData.instanceId = member->GetInstanceId();
        memberData.health = member->GetHealth();
        memberData.maxHealth = member->GetMaxHealth();
        memberData.power = member->GetPower(member->GetPowerType());
        memberData.maxPower = member->GetMaxPower(member->GetPowerType());
        memberData.victimGuid = member->GetVictim()
            ? member->GetVictim()->GetObjectGuid().GetRawValue() : 0;
        memberData.x = member->GetPositionX();
        memberData.y = member->GetPositionY();
        memberData.z = member->GetPositionZ();
        memberData.online = member->IsInWorld();
        memberData.alive = member->IsAlive();
        memberData.inCombat = member->IsInCombat();
        snapshot.members.push_back(memberData);

        // Include mobs currently attacking a party member and mobs that party
        // members are attacking, giving both peel and assist strategies targets.
        addHostile(member->GetVictim());
        for (Unit* attacker : member->GetAttackers())
            addHostile(attacker);
    }

    if (snapshot.members.empty())
        return false;

    snapshot.hostiles.reserve(hostiles.size());
    for (auto& entry : hostiles)
        snapshot.hostiles.push_back(std::move(entry.second));
    return true;
}

} // namespace PartyBotEngine
