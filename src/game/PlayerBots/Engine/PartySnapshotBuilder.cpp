#include "PartySnapshotBuilder.h"
#include "CombatBotBaseAI.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
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
    auto addHostile = [&](Unit* hostileUnit) -> HostileSnapshot*
    {
        if (!hostileUnit || !hostileUnit->IsInWorld() ||
            hostileUnit->GetMap() != map ||
            group->IsMember(hostileUnit->GetObjectGuid()))
            return nullptr;

        Guid const hostileGuid = hostileUnit->GetObjectGuid().GetRawValue();
        auto inserted = hostiles.emplace(hostileGuid, HostileSnapshot());
        if (!inserted.second)
            return &inserted.first->second;

        HostileSnapshot& hostile = inserted.first->second;
        hostile.guid = hostileGuid;
        hostile.victimGuid = hostileUnit->GetVictim()
            ? hostileUnit->GetVictim()->GetObjectGuid().GetRawValue() : 0;
        if (Creature* creature = hostileUnit->ToCreature())
        {
            CreatureInfo const* info = creature->GetCreatureInfo();
            if (info)
            {
                hostile.entry = info->entry;
                hostile.rank = info->rank;
                hostile.worldBoss = creature->IsWorldBoss();
            }
        }
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
        return &hostile;
    };

    auto addHostileReference = [&](HostileReference* reference)
    {
        if (!reference || !reference->getSource() || !reference->getTarget())
            return;

        ThreatManager* source = reference->getSource();
        Unit* hostileUnit = source->getOwner();
        HostileSnapshot* hostile = addHostile(hostileUnit);
        if (!hostile)
            return;

        Guid const targetGuid = reference->getUnitGuid().GetRawValue();
        if (reference->getTarget()->GetObjectGuid().GetRawValue() != targetGuid)
            return;

        // The online threat list was copied by addHostile. This path also
        // preserves references held by the party member when Core has moved
        // them to the hostile's offline threat container.
        for (ThreatEntrySnapshot const& existing : hostile->threat)
            if (existing.unitGuid == targetGuid)
                return;

        ThreatEntrySnapshot threat;
        threat.unitGuid = targetGuid;
        threat.threat = reference->getThreat();
        threat.online = reference->isOnline();
        threat.accessible = reference->isAccessable();
        Unit* threatTarget = reference->getTarget();
        threat.meleeReachable = threatTarget &&
            hostileUnit->CanReachWithMeleeAutoAttack(threatTarget);
        hostile->threat.push_back(threat);
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
        CombatBotBaseAI* combatAI = dynamic_cast<CombatBotBaseAI*>(member->AI());
        memberData.isPlayerBot = combatAI != nullptr;
        if (combatAI)
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
        memberData.orientation = member->GetOrientation();
        memberData.shapeshiftForm = static_cast<std::uint8_t>(member->GetShapeshiftForm());

        Spell* currentSpell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        std::uint8_t currentSpellType = CURRENT_GENERIC_SPELL;
        if (!currentSpell)
        {
            currentSpell = member->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            currentSpellType = CURRENT_CHANNELED_SPELL;
        }
        if (!currentSpell)
        {
            currentSpell = member->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
            currentSpellType = CURRENT_AUTOREPEAT_SPELL;
        }
        if (currentSpell && currentSpell->m_spellInfo)
        {
            memberData.currentSpellId = currentSpell->m_spellInfo->Id;
            memberData.currentSpellState = currentSpell->getState();
            memberData.currentSpellType = currentSpellType;
            memberData.currentSpellTargetGuid =
                currentSpell->m_targets.getUnitTargetGuid().GetRawValue();
        }
        memberData.online = member->IsInWorld();
        memberData.alive = member->IsAlive();
        memberData.inCombat = member->IsInCombat();
        snapshot.members.push_back(memberData);

        // Record the Player's primary controlled pet as a distinct threat actor.
        // Keep its GUID as the actual Core threat owner; ownerGuid proves party
        // membership and is used only for coordinator reservation cleanup.
        if (Pet* pet = member->GetPet()) {
            if (pet->IsInWorld() && pet->GetMap() == map &&
                pet->GetMapId() == snapshot.mapId &&
                pet->GetInstanceId() == snapshot.instanceId &&
                pet->GetOwnerGuid() == member->GetObjectGuid()) {
                PartyCompanionSnapshot companion;
                companion.guid = pet->GetObjectGuid().GetRawValue();
                companion.ownerGuid = memberData.guid;
                companion.mapId = pet->GetMapId();
                companion.instanceId = pet->GetInstanceId();
                companion.health = pet->GetHealth();
                companion.maxHealth = pet->GetMaxHealth();
                companion.victimGuid = pet->GetVictim()
                    ? pet->GetVictim()->GetObjectGuid().GetRawValue() : 0;
                companion.x = pet->GetPositionX();
                companion.y = pet->GetPositionY();
                companion.z = pet->GetPositionZ();
                companion.online = pet->IsInWorld();
                companion.alive = pet->IsAlive();
                companion.inCombat = pet->IsInCombat();
                snapshot.companions.push_back(companion);

                // Discover hostile references held by or targeting the pet too.
                addHostile(pet->GetVictim());
                for (Unit* attacker : pet->GetAttackers())
                    addHostile(attacker);
                for (HostileReference* reference = pet->GetHostileRefManager().getFirst();
                     reference; reference = reference->next())
                    addHostileReference(reference);
            }
        }

        // Include current attackers and targets, plus every hostile that
        // holds a reference to this member. Healing/assist threat traverses
        // HostileRefManager, so the current victim alone is not a complete set.
        addHostile(member->GetVictim());
        for (Unit* attacker : member->GetAttackers())
            addHostile(attacker);
        for (HostileReference* reference = member->GetHostileRefManager().getFirst();
             reference; reference = reference->next())
            addHostileReference(reference);
    }

    if (snapshot.members.empty())
        return false;

    snapshot.hostiles.reserve(hostiles.size());
    for (auto& entry : hostiles)
        snapshot.hostiles.push_back(std::move(entry.second));
    return true;
}

} // namespace PartyBotEngine
