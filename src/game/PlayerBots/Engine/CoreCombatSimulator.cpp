#include "CoreCombatSimulator.h"

#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"

namespace PartyBotEngine
{
bool RunCoreMapLifecycleSmokeTest(std::uint32_t mapId)
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

    std::uint32_t const instanceId = map->GetInstanceId();
    bool const registered = instanceId != 0 && sMapMgr.FindMap(mapId, instanceId) == map;

    sMapMgr.DeleteTestMap(map);

    bool const removed = sMapMgr.FindMap(mapId, instanceId) == nullptr;
    if (!registered || !removed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Test-map lifecycle assertion failed (registered=%u removed=%u instance=%u).",
                 registered ? 1u : 0u, removed ? 1u : 0u, instanceId);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: created and deleted real Core test map %u instance %u.",
             mapId, instanceId);
    return true;
}
}
