/*
 * PartyBot decision tracing support.
 *
 * This file is part of the VMaNGOS project and is distributed under
 * the GNU General Public License, version 2 or (at your option) any later version.
 */

#include "Engine/DecisionTrace.h"

#include "Config/Config.h"
#include "Player.h"
#include "SpellEntry.h"
#include "World.h"

namespace
{
    uint32 const MAX_RECORDS_PER_TICK = 12;
}

namespace PartyBotEngine
{

DecisionTrace::DecisionTrace()
    : m_recordsThisTick(0), m_enabled(false)
{
}

void DecisionTrace::BeginTick() const
{
    m_recordsThisTick = 0;
    m_enabled = sConfig.GetBoolDefault("PlayerBot.DecisionTrace", false);
}

void DecisionTrace::Record(Player const* bot, char const* event, Unit const* target,
                           SpellEntry const* spell, char const* reason, uint32 detail) const
{
    if (!m_enabled || !bot)
        return;

    if (m_recordsThisTick >= MAX_RECORDS_PER_TICK)
        return;

    ++m_recordsThisTick;

    if (m_recordsThisTick == MAX_RECORDS_PER_TICK)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotDecision] bot=%u trace_limit=%u further_records_suppressed=1",
                 bot->GetGUIDLow(), MAX_RECORDS_PER_TICK);
        return;
    }

    char const* spellName = "-";
    if (spell && !spell->SpellName[0].empty())
        spellName = spell->SpellName[0].c_str();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotDecision] bot=%u class=%u event=%s target=%u spell=%u name=%s reason=%s detail=%u",
             bot->GetGUIDLow(),
             static_cast<uint32>(bot->GetClass()),
             event ? event : "unknown",
             target ? target->GetGUIDLow() : 0,
             spell ? spell->Id : 0,
             spellName,
             reason ? reason : "unspecified",
             detail);
}

void DecisionTrace::RecordEncounterBoundary(Player const* bot, char const* event,
                                                     uint32 groupId, uint32 mapId,
                                                     uint32 instanceId, uint64 encounterId,
                                                     uint32 sequence, uint32 elapsedMs,
                                                     char const* reason) const
{
    // Do not apply MAX_RECORDS_PER_TICK here: losing an encounter boundary would
    // make every later event in that fight impossible to correlate.
    if (!m_enabled || !bot || !encounterId || !sequence)
        return;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=%s group=%u map=%u instance=%u bot=%u elapsed_ms=%u reason=%s",
             static_cast<unsigned long long>(encounterId), sequence,
             event ? event : "unknown", groupId, mapId, instanceId,
             bot->GetGUIDLow(), elapsedMs, reason ? reason : "unspecified");
}

void DecisionTrace::RecordShadowStatus(Player const* bot, char const* status,
                                      uint32 groupId, uint32 mapId, uint32 instanceId) const
{
    if (!m_enabled || !bot || m_recordsThisTick >= MAX_RECORDS_PER_TICK)
        return;

    ++m_recordsThisTick;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotShadowStatus] bot=%u group=%u map=%u instance=%u status=%s",
             bot->GetGUIDLow(), groupId, mapId, instanceId, status ? status : "unknown");
}

void DecisionTrace::RecordShadowDecision(Player const* bot, uint32 groupId, uint32 mapId,
                                         uint32 instanceId, uint32 snapshotEpoch,
                                         char const* role, char const* strategy,
                                         char const* action, uint64 targetGuid, float priority,
                                         char const* status, char const* reason) const
{
    if (!m_enabled || !bot || m_recordsThisTick >= MAX_RECORDS_PER_TICK)
        return;

    ++m_recordsThisTick;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotShadowDecision] bot=%u group=%u map=%u instance=%u epoch=%u role=%s strategy=%s action=%s target=%llu priority=%.2f status=%s reason=%s",
             bot->GetGUIDLow(), groupId, mapId, instanceId, snapshotEpoch,
             role ? role : "unknown", strategy ? strategy : "unknown",
             action ? action : "unknown", static_cast<unsigned long long>(targetGuid),
             priority, status ? status : "unknown", reason ? reason : "none");
}

void DecisionTrace::RecordShadowSummary(Player const* bot, uint32 groupId, uint32 mapId,
                                       uint32 instanceId, uint32 snapshotEpoch,
                                       uint32 memberCount, uint32 hostileCount,
                                       uint32 candidateCount, uint32 selectedCount) const
{
    if (!m_enabled || !bot || m_recordsThisTick >= MAX_RECORDS_PER_TICK)
        return;

    ++m_recordsThisTick;
    if (m_recordsThisTick == MAX_RECORDS_PER_TICK)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotDecision] bot=%u trace_limit=%u further_records_suppressed=1",
                 bot->GetGUIDLow(), MAX_RECORDS_PER_TICK);
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotShadow] bot=%u group=%u map=%u instance=%u epoch=%u members=%u hostiles=%u candidates=%u selected=%u",
             bot->GetGUIDLow(), groupId, mapId, instanceId, snapshotEpoch,
             memberCount, hostileCount, candidateCount, selectedCount);
}

} // namespace PartyBotEngine
