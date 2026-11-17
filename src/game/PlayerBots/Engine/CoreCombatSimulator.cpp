#include "CoreCombatSimulator.h"

#include "AI/CreatureAI.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace PartyBotEngine
{
namespace
{
class SimulatorProbeAI final : public CreatureAI
{
public:
    SimulatorProbeAI(Creature* creature, uint32& updateCount)
        : CreatureAI(creature), m_updateCount(updateCount)
    {
    }

    void UpdateAI(uint32 const /*diff*/) override
    {
        ++m_updateCount;
    }

private:
    uint32& m_updateCount;
};

struct SimulatorActor
{
    Creature* creature = nullptr;
    uint32 updateCount = 0;
};

CreatureInfo const* FindSafeCreatureTemplate()
{
    std::vector<CreatureInfo const*> candidates;
    for (auto const& item : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = item.second.get();
        if (!info || info->entry != item.first || info->rank != CREATURE_ELITE_NORMAL ||
            !info->display_total_probability || !info->display_id[0] ||
            info->npc_flags || info->civilian || info->racial_leader || info->pet_family ||
            info->pet_spell_list_id || info->immunity_flags || info->school_immune_mask ||
            info->mechanic_immune_mask || info->spawn_spell_id || info->totem_spell_id ||
            info->spell_list_id || info->auras || info->script_id || !info->ai_name.empty() ||
            (info->flags_extra & (CREATURE_FLAG_EXTRA_INVISIBLE | CREATURE_FLAG_EXTRA_NO_THREAT_LIST)))
            continue;

        candidates.push_back(info);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](CreatureInfo const* lhs, CreatureInfo const* rhs) { return lhs->entry < rhs->entry; });
    return candidates.empty() ? nullptr : candidates.front();
}

bool FindMutuallyHostileFactions(uint32& firstId, uint32& secondId)
{
    std::vector<FactionTemplateEntry const*> factions;
    for (auto const& item : sObjectMgr.GetFactionTemplateMap())
        if (item.second.ID)
            factions.push_back(&item.second);

    std::sort(factions.begin(), factions.end(),
              [](FactionTemplateEntry const* lhs, FactionTemplateEntry const* rhs) { return lhs->ID < rhs->ID; });

    for (std::size_t i = 0; i < factions.size(); ++i)
    {
        for (std::size_t j = i + 1; j < factions.size(); ++j)
        {
            if (factions[i]->IsHostileTo(*factions[j]) && factions[j]->IsHostileTo(*factions[i]))
            {
                firstId = factions[i]->ID;
                secondId = factions[j]->ID;
                return true;
            }
        }
    }

    return false;
}

bool CreateSimulatorActor(Map& map, CreatureInfo const& info, uint32 factionId,
                          float x, float y, SimulatorActor& actor)
{
    Creature* creature = new Creature();
    CreatureCreatePos position(&map, x, y, 0.0f, 0.0f);
    if (!creature->Create(map.GenerateLocalLowGuid(HIGHGUID_UNIT), position, &info, info.entry))
    {
        delete creature;
        return false;
    }

    // AIM_Initialize is the Core lifecycle entry point for both the default
    // MotionMaster generator and the template-selected AI. Do this before
    // replacing the AI: installing a custom AI before Map::Add makes
    // Creature::AddToWorld skip AIM_Initialize entirely, leaving MotionMaster
    // empty and causing UpdateMotion's !empty() assertion.
    if (!creature->AIM_Initialize())
    {
        delete creature;
        return false;
    }

    // AIM_Initialize created the normal template AI. Replace it explicitly;
    // Creature::SetAI is a raw pointer setter and does not delete the old AI.
    delete creature->AI();
    creature->SetAI(new SimulatorProbeAI(creature, actor.updateCount));

    creature->SetFactionTemplateId(factionId);
    creature->SetMaxHealth(100000);
    creature->SetHealth(100000);

    // A headless map has no player-centered update cells. Active actors are
    // visited by Map::UpdateCells through the Core's normal active-object path.
    creature->SetActiveObjectState(true);
    map.Add(creature);

    actor.creature = creature;
    ObjectGuid const guid = creature->GetObjectGuid();
    if (!creature->IsInWorld() || map.GetCreature(guid) != creature)
    {
        if (creature->IsInWorld())
            map.Remove(creature, false);

        // Never destroy an object while the map might still hold its pointer.
        bool const detached = !creature->IsInWorld() && map.GetCreature(guid) == nullptr;
        if (detached)
        {
            delete creature;
            actor.creature = nullptr;
        }
        return false;
    }

    return true;
}

bool DestroySimulatorActor(Map& map, SimulatorActor& actor)
{
    if (!actor.creature)
        return true;

    ObjectGuid const guid = actor.creature->GetObjectGuid();
    actor.creature->CombatStop(true);
    actor.creature->DeleteThreatList();

    if (actor.creature->IsInWorld())
        map.Remove(actor.creature, false);

    bool const detached = !actor.creature->IsInWorld() && map.GetCreature(guid) == nullptr;
    if (!detached)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Refusing to destroy Creature GUID %u because Map::Remove(false) did not detach it.",
                 actor.creature->GetGUIDLow());
        return false;
    }

    delete actor.creature;
    actor.creature = nullptr;
    return true;
}

bool RunCoreCombatThreatSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    if (!info)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] No safe creature_template found (requires a normal-rank template with a display and no spawn spell, spell list or script).");
        return false;
    }

    uint32 bossFaction = 0;
    uint32 attackerFaction = 0;
    if (!FindMutuallyHostileFactions(bossFaction, attackerFaction))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Could not find a mutually hostile pair in the loaded faction_template data.");
        return false;
    }

    SimulatorActor boss;
    SimulatorActor tank;
    SimulatorActor damageDealer;

    bool created = CreateSimulatorActor(map, *info, bossFaction, 0.0f, 0.0f, boss) &&
                   CreateSimulatorActor(map, *info, attackerFaction, 1.0f, 0.0f, tank) &&
                   CreateSimulatorActor(map, *info, attackerFaction, 2.0f, 0.0f, damageDealer);

    if (!created)
    {
        bool const cleanedBoss = DestroySimulatorActor(map, boss);
        bool const cleanedTank = DestroySimulatorActor(map, tank);
        bool const cleanedDamageDealer = DestroySimulatorActor(map, damageDealer);
        if (!cleanedBoss || !cleanedTank || !cleanedDamageDealer)
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PartyBotSimulator] Actor cleanup failed after a partial create.");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Failed to create and insert all three template-backed Creature actors.");
        return false;
    }

    bool const hostilePair = boss.creature->IsHostileTo(tank.creature) &&
                             tank.creature->IsHostileTo(boss.creature) &&
                             boss.creature->IsHostileTo(damageDealer.creature) &&
                             damageDealer.creature->IsHostileTo(boss.creature);

    // Map cell updates are paced by the Core's wall-clock update-cells
    // interval. Drive bounded real map ticks and require each custom AI to
    // prove that the active-object update path reached it.
    for (uint32 tick = 0; tick < 120 &&
         (!boss.updateCount || !tank.updateCount || !damageDealer.updateCount); ++tick)
    {
        map.Update(50);
        if (!boss.updateCount || !tank.updateCount || !damageDealer.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    bool const aiUpdated = boss.updateCount && tank.updateCount && damageDealer.updateCount;
    bool damageAndThreat = false;
    bool targetSelection = false;
    uint32 initialHealth = 0;
    uint32 healthAfterTank = 0;
    uint32 healthAfterDps = 0;
    uint32 healthAfterFinalHit = 0;
    uint32 tankDamage = 0;
    uint32 dpsDamage = 0;
    uint32 additionalDamage = 0;
    float tankThreat = 0.0f;
    float dpsThreat = 0.0f;
    uint32 selectedAfterTankGuid = 0;
    uint32 selectedBelowThresholdGuid = 0;
    uint32 selectedAboveThresholdGuid = 0;

    if (hostilePair && aiUpdated)
    {
        initialHealth = boss.creature->GetHealth();
        tankDamage = tank.creature->DealDamage(
            boss.creature, 100, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        tankThreat = boss.creature->GetThreatManager().getThreat(tank.creature);
        healthAfterTank = boss.creature->GetHealth();

        Unit* selectedAfterTank = boss.creature->GetThreatManager().getHostileTarget();
        selectedAfterTankGuid = selectedAfterTank ? selectedAfterTank->GetGUIDLow() : 0;

        dpsDamage = damageDealer.creature->DealDamage(
            boss.creature, 105, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        dpsThreat = boss.creature->GetThreatManager().getThreat(damageDealer.creature);
        healthAfterDps = boss.creature->GetHealth();
        Unit* selectedBelowThreshold = boss.creature->GetThreatManager().getHostileTarget();
        selectedBelowThresholdGuid = selectedBelowThreshold ? selectedBelowThreshold->GetGUIDLow() : 0;

        additionalDamage = damageDealer.creature->DealDamage(
            boss.creature, 10, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        dpsThreat = boss.creature->GetThreatManager().getThreat(damageDealer.creature);
        healthAfterFinalHit = boss.creature->GetHealth();
        Unit* selectedAboveThreshold = boss.creature->GetThreatManager().getHostileTarget();
        selectedAboveThresholdGuid = selectedAboveThreshold ? selectedAboveThreshold->GetGUIDLow() : 0;

        damageAndThreat = tankDamage > 0 && dpsDamage > 0 && additionalDamage > 0 &&
                          healthAfterTank < initialHealth && healthAfterDps < healthAfterTank &&
                          healthAfterFinalHit < healthAfterDps && tankThreat > 0.0f && dpsThreat > 0.0f;

        targetSelection = selectedAfterTank == tank.creature &&
                          selectedBelowThreshold == tank.creature &&
                          dpsThreat > 1.1f * tankThreat &&
                          selectedAboveThreshold == damageDealer.creature;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Creature probe: template=%u factions=%u/%u aiUpdates=%u/%u/%u hostile=%u health=%u>%u>%u>%u damage=%u/%u/%u threat=%.1f/%.1f switchThreshold=%.1f selectedGUID=%u/%u/%u damageThreat=%u targetSwitch=%u.",
             info->entry, bossFaction, attackerFaction,
             boss.updateCount, tank.updateCount, damageDealer.updateCount,
             hostilePair ? 1u : 0u, initialHealth, healthAfterTank, healthAfterDps, healthAfterFinalHit,
             tankDamage, dpsDamage, additionalDamage, tankThreat, dpsThreat, 1.1f * tankThreat,
             selectedAfterTankGuid, selectedBelowThresholdGuid, selectedAboveThresholdGuid,
             damageAndThreat ? 1u : 0u, targetSelection ? 1u : 0u);

    // Clear the boss-owned threat references before destroying their targets.
    bool const cleanedBoss = DestroySimulatorActor(map, boss);
    bool const cleanedTank = DestroySimulatorActor(map, tank);
    bool const cleanedDamageDealer = DestroySimulatorActor(map, damageDealer);
    bool const actorsCleaned = cleanedBoss && cleanedTank && cleanedDamageDealer;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Actor cleanup: boss=%u tank=%u damageDealer=%u.",
             cleanedBoss ? 1u : 0u, cleanedTank ? 1u : 0u, cleanedDamageDealer ? 1u : 0u);

    if (!hostilePair || !aiUpdated || !damageAndThreat || !targetSelection || !actorsCleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core combat/threat smoke test failed (actorsCleaned=%u).", actorsCleaned ? 1u : 0u);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: real Creature updates, direct damage, damage-generated threat and 110%% melee target switching.");
    return true;
}
} // namespace

bool RunCoreMapLifecycleSmokeTest(uint32 mapId)
{
    MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(mapId);
    if (!entry || !entry->IsDungeon())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Map %u is not a valid dungeon/raid MapEntry; refusing to create a test map.",
                 mapId);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Creating isolated test map %u.", mapId);

    Map* map = sMapMgr.CreateTestMap(mapId, true, 0.0f, 0.0f);
    if (!map)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] MapManager::CreateTestMap(%u) returned null.", mapId);
        return false;
    }

    uint32 const instanceId = map->GetInstanceId();
    bool const registered = instanceId != 0 && sMapMgr.FindMap(mapId, instanceId) == map;

    // Keep Stage A limited to map registration/registry cleanup. An empty
    // Map::Update can execute instance-script work before simulator actors
    // exist; the first map tick belongs to Stage B, after actors have completed
    // the normal Creature::AIM_Initialize lifecycle above.
    bool const mapRetained = sMapMgr.FindMap(mapId, instanceId) == map;

    bool const combatPassed = registered && mapRetained && RunCoreCombatThreatSmokeTest(*map);

    sMapMgr.DeleteTestMap(map);

    bool const removed = sMapMgr.FindMap(mapId, instanceId) == nullptr;
    if (!registered || !mapRetained || !combatPassed || !removed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Test-map/combat assertions failed (registered=%u retained=%u combat=%u removed=%u instance=%u).",
                 registered ? 1u : 0u, mapRetained ? 1u : 0u, combatPassed ? 1u : 0u,
                 removed ? 1u : 0u, instanceId);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: created, ran a bounded Core Creature combat/threat scenario and deleted map %u instance %u.",
             mapId, instanceId);
    return true;
}
}
