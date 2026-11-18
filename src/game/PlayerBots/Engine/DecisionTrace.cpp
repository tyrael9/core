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

#include "Engine/PartyCoordinator.h"
#include "Engine/PartySnapshot.h"

#include <algorithm>
#include <cmath>
#include <sstream>

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

void DecisionTrace::RecordPartyHeartbeat(Player const* bot,
                                       PartySnapshot const& snapshot,
                                       EncounterHeartbeat const& heartbeat) const
{
    if (!m_enabled || !bot || !heartbeat.encounterId || !heartbeat.sequence)
        return;

    // A party snapshot is a single, coordinator-throttled line. Keep each
    // entity class bounded and make omissions/truncation explicit.
    std::ostringstream facts;
    std::size_t const memberLimit = 5;
    std::size_t const companionLimit = 5;
    std::size_t const hostileLimit = 8;

    facts << "members=";
    std::size_t emitted = std::min(snapshot.members.size(), memberLimit);
    for (std::size_t i = 0; i < emitted; ++i)
    {
        PartyMemberSnapshot const& member = snapshot.members[i];
        if (i)
            facts << '|';
        facts << member.guid << ',' << static_cast<uint32>(member.classId)
              << ',' << static_cast<uint32>(member.roleId)
              << ',' << (member.isMainTank ? 1 : 0)
              << ',' << (member.online ? 1 : 0)
              << ',' << (member.alive ? 1 : 0)
              << ',' << (member.inCombat ? 1 : 0)
              << ',' << member.health << '/' << member.maxHealth
              << ',' << static_cast<uint32>(member.powerType)
              << ':' << member.power << '/' << member.maxPower
              << ',' << member.victimGuid;
    }

    facts << ";companions=";
    emitted = std::min(snapshot.companions.size(), companionLimit);
    for (std::size_t i = 0; i < emitted; ++i)
    {
        PartyCompanionSnapshot const& companion = snapshot.companions[i];
        if (i)
            facts << '|';
        facts << companion.guid << ',' << companion.ownerGuid
              << ',' << (companion.online ? 1 : 0)
              << ',' << (companion.alive ? 1 : 0)
              << ',' << (companion.inCombat ? 1 : 0)
              << ',' << companion.health << '/' << companion.maxHealth
              << ',' << companion.victimGuid;
    }

    facts << ";hostiles=";
    emitted = std::min(snapshot.hostiles.size(), hostileLimit);
    for (std::size_t i = 0; i < emitted; ++i)
    {
        HostileSnapshot const& hostile = snapshot.hostiles[i];
        if (i)
            facts << '|';
        facts << hostile.guid << ',' << (hostile.alive ? 1 : 0)
              << ',' << (hostile.crowdControlProtected ? 1 : 0)
              << ',' << hostile.health << '/' << hostile.maxHealth
              << ',' << hostile.victimGuid;
    }

    std::string payload = facts.str();
    bool truncated = snapshot.members.size() > memberLimit ||
                     snapshot.companions.size() > companionLimit ||
                     snapshot.hostiles.size() > hostileLimit;
    // Bound the complete facts field so this remains one compact log record.
    std::size_t const maxFactsLength = 1600;
    if (payload.size() > maxFactsLength)
    {
        payload.resize(maxFactsLength);
        truncated = true;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=party_snapshot group=%u map=%u instance=%u bot=%u elapsed_ms=%u snapshot_epoch=%u member_count=%u companion_count=%u hostile_count=%u members_omitted=%u companions_omitted=%u hostiles_omitted=%u facts=%s truncated=%u",
             static_cast<unsigned long long>(heartbeat.encounterId),
             heartbeat.sequence, heartbeat.groupId, heartbeat.mapId,
             heartbeat.instanceId, bot->GetGUIDLow(), heartbeat.elapsedMs,
             snapshot.epoch, static_cast<uint32>(snapshot.members.size()),
             static_cast<uint32>(snapshot.companions.size()),
             static_cast<uint32>(snapshot.hostiles.size()),
             static_cast<uint32>(snapshot.members.size() > memberLimit
                 ? snapshot.members.size() - memberLimit : 0),
             static_cast<uint32>(snapshot.companions.size() > companionLimit
                 ? snapshot.companions.size() - companionLimit : 0),
             static_cast<uint32>(snapshot.hostiles.size() > hostileLimit
                 ? snapshot.hostiles.size() - hostileLimit : 0),
             payload.c_str(), truncated ? 1u : 0u);
}

namespace
{
    std::string SafeTraceToken(char const* value)
    {
        if (!value || !*value)
            return "none";

        std::string token;
        token.reserve(64);
        for (unsigned char c = static_cast<unsigned char>(*value);
             c && token.size() < 64;
             c = static_cast<unsigned char>(*++value))
        {
            bool const safe =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                c == '.' || c == ':';
            token.push_back(safe ? static_cast<char>(c) : '_');
        }
        return token.empty() ? "none" : token;
    }
}

void DecisionTrace::RecordEncounterDecisionSummary(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint32 snapshotEpoch, uint64 encounterId, uint32 sequence,
    uint32 elapsedMs, char const* role,
    uint32 candidateCount, uint32 selectedCount) const
{
    if (!m_enabled || !bot || !encounterId || !sequence ||
        m_recordsThisTick >= MAX_RECORDS_PER_TICK)
        return;

    ++m_recordsThisTick;
    std::string const safeRole = SafeTraceToken(role);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=decision_summary group=%u map=%u instance=%u bot=%u elapsed_ms=%u snapshot_epoch=%u role=%s candidates=%u selected=%u",
             static_cast<unsigned long long>(encounterId), sequence,
             groupId, mapId, instanceId, bot->GetGUIDLow(), elapsedMs,
             snapshotEpoch, safeRole.c_str(), candidateCount, selectedCount);
}

void DecisionTrace::RecordEncounterCandidate(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint32 snapshotEpoch, uint64 encounterId, uint32 sequence,
    uint32 elapsedMs, char const* role, char const* strategy,
    char const* action, uint64 targetGuid, uint32 spellId, uint32 channel,
    float priority, char const* status, char const* reason) const
{
    // PartyBotAI bounds this to a summary plus at most three candidates per
    // bot in its ten-second Shadow detail window. Preserve those correlated
    // records even when ordinary per-tick legacy diagnostics use their quota.
    if (!m_enabled || !bot || !encounterId || !sequence)
        return;

    std::string const safeRole = SafeTraceToken(role);
    std::string const safeStrategy = SafeTraceToken(strategy);
    std::string const safeAction = SafeTraceToken(action);
    std::string const safeStatus = SafeTraceToken(status);
    std::string const safeReason = SafeTraceToken(reason);
    bool const priorityValid = std::isfinite(priority);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=candidate group=%u map=%u instance=%u bot=%u elapsed_ms=%u snapshot_epoch=%u role=%s strategy=%s action=%s spell=%u channel=%u target=%llu priority=%.2f priority_valid=%u status=%s reason=%s",
             static_cast<unsigned long long>(encounterId), sequence,
             groupId, mapId, instanceId, bot->GetGUIDLow(), elapsedMs,
             snapshotEpoch, safeRole.c_str(), safeStrategy.c_str(),
             safeAction.c_str(), spellId, channel,
             static_cast<unsigned long long>(targetGuid),
             priorityValid ? priority : 0.0f, priorityValid ? 1u : 0u,
             safeStatus.c_str(), safeReason.c_str());
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
