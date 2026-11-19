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
#include <limits>
#include <sstream>
#include <string>

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

        // Preserve a small sample of the real Core ThreatManager references.
        // Values are in tenths to keep the one-line payload compact.
        std::size_t const threatLimit = 2;
        std::size_t const threatEmitted = std::min(hostile.threat.size(), threatLimit);
        facts << ",refs=" << static_cast<uint32>(hostile.threat.size()) << '[';
        for (std::size_t threatIndex = 0; threatIndex < threatEmitted; ++threatIndex)
        {
            ThreatEntrySnapshot const& threat = hostile.threat[threatIndex];
            if (threatIndex)
                facts << '|';
            facts << threat.unitGuid << ':'
                  << static_cast<int32>(threat.threat * 10.0f) << ':'
                  << (threat.online ? 1 : 0)
                  << (threat.accessible ? 1 : 0)
                  << (threat.meleeReachable ? 1 : 0);
        }
        facts << "],refs_omitted="
              << static_cast<uint32>(hostile.threat.size() > threatLimit
                  ? hostile.threat.size() - threatLimit : 0);
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
    std::string SafeTraceToken(char const* value);
}

void DecisionTrace::RecordEncounterAliveState(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint64 encounterId, uint32 sequence, uint32 elapsedMs,
    bool wasAlive, bool isAlive) const
{
    if (!m_enabled || !bot || !encounterId || !sequence || wasAlive == isAlive)
        return;

    // Death and resurrection are low-frequency, high-value events. They bypass
    // the ordinary per-tick detail budgets so a wipe cannot disappear in noise.
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=%s group=%u map=%u instance=%u bot=%u elapsed_ms=%u alive_before=%u alive_after=%u health=%u max_health=%u",
             static_cast<unsigned long long>(encounterId), sequence,
             isAlive ? "resurrect" : "death", groupId, mapId, instanceId,
             bot->GetGUIDLow(), elapsedMs, wasAlive ? 1u : 0u,
             isAlive ? 1u : 0u, bot->GetHealth(), bot->GetMaxHealth());
}

bool DecisionTrace::RecordEncounterPartyMemberState(
    Player const* bot, PartyCoordinator& coordinator,
    uint32 groupId, uint32 mapId, uint32 instanceId,
    uint64 encounterId, uint32 elapsedMs,
    PartyMemberStateChange const& change) const
{
    if (!m_enabled || !bot || !encounterId || !change.memberGuid)
        return false;
    bool const lifecycle = change.wasAlive != change.isAlive;
    EncounterTraceEventClass const eventClass = lifecycle
        ? EncounterTraceEventClass::Important : EncounterTraceEventClass::Detail;
    EncounterTraceAdmission const admission =
        coordinator.TryAdmitEncounterTraceEvent(encounterId, eventClass);
    if (admission == EncounterTraceAdmission::EmitLimitMarker)
    {
        uint32 const markerSequence = coordinator.NextEncounterSequence();
        if (markerSequence)
            RecordEncounterBoundary(bot, "trace_limit", groupId, mapId,
                instanceId, encounterId, markerSequence, elapsedMs,
                lifecycle ? "important_budget_exhausted" : "detail_budget_exhausted");
        return false;
    }
    if (admission != EncounterTraceAdmission::Accepted)
        return false;
    uint32 const sequence = coordinator.NextEncounterSequence();
    if (!sequence)
        return false;
    char const* event = lifecycle
        ? (change.isAlive ? "party_member_resurrect" : "party_member_death")
        : "party_member_health";
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=%s group=%u map=%u instance=%u bot=%u elapsed_ms=%u member_guid=%llu health_before=%u health_after=%u max_health_before=%u max_health_after=%u alive_before=%u alive_after=%u",
             static_cast<unsigned long long>(encounterId), sequence, event,
             groupId, mapId, instanceId, bot->GetGUIDLow(), elapsedMs,
             static_cast<unsigned long long>(change.memberGuid),
             change.healthBefore, change.healthAfter,
             change.maxHealthBefore, change.maxHealthAfter,
             change.wasAlive ? 1u : 0u, change.isAlive ? 1u : 0u);
    return true;
}

void DecisionTrace::RecordEncounterTargetChange(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint64 encounterId, uint32 sequence, uint32 elapsedMs,
    uint64 oldVictimGuid, uint64 newVictimGuid, char const* source) const
{
    if (!m_enabled || !bot || !encounterId || !sequence ||
        oldVictimGuid == newVictimGuid)
        return;

    std::string const safeSource = SafeTraceToken(source);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=target_change source=%s group=%u map=%u instance=%u bot=%u elapsed_ms=%u old_victim=%llu new_victim=%llu",
             static_cast<unsigned long long>(encounterId), sequence,
             safeSource.c_str(), groupId, mapId, instanceId,
             bot->GetGUIDLow(), elapsedMs,
             static_cast<unsigned long long>(oldVictimGuid),
             static_cast<unsigned long long>(newVictimGuid));
}

namespace
{
    std::string SafeTraceToken(char const* value)
    {
        if (!value || !*value)
            return "none";

        std::string token;
        token.reserve(64);
        for (std::size_t i = 0; value[i] && token.size() < 64; ++i)
        {
            unsigned char const c = static_cast<unsigned char>(value[i]);
            bool const safe =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                c == '.' || c == ':';
            token.push_back(safe ? static_cast<char>(c) : '_');
        }
        return token.empty() ? "none" : token;
    }
}

void DecisionTrace::RecordReservationEvent(
    DecisionContext const& context, ReservationTraceEvent const& event)
{
    if (!m_enabled || !context.coordinator.HasActiveEncounter() ||
        !context.bot.guid)
        return;

    uint64 const encounterId = context.coordinator.GetEncounterId();
    uint32 const elapsedMs = context.coordinator.GetEncounterElapsedMs(context.nowMs);
    uint32 const groupId = context.snapshot.groupId;
    uint32 const mapId = context.snapshot.mapId;
    uint32 const instanceId = context.snapshot.instanceId;

    EncounterTraceAdmission const admission =
        context.coordinator.TryAdmitEncounterTraceEvent(
            encounterId, EncounterTraceEventClass::Important);
    if (admission == EncounterTraceAdmission::EmitLimitMarker)
    {
        uint32 const markerSequence = context.coordinator.NextEncounterSequence();
        if (markerSequence)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=trace_limit group=%u map=%u instance=%u bot=%llu elapsed_ms=%u reason=important_budget_exhausted",
                     static_cast<unsigned long long>(encounterId), markerSequence,
                     groupId, mapId, instanceId,
                     static_cast<unsigned long long>(context.bot.guid), elapsedMs);
    }
    if (admission != EncounterTraceAdmission::Accepted)
        return;
    uint32 const sequence = context.coordinator.NextEncounterSequence();
    if (!sequence)
        return;

    char const* eventName = "threat_reservation_unknown";
    switch (event.stage)
    {
        case ReservationTraceStage::BudgetVeto:
            eventName = "threat_budget_veto";
            break;
        case ReservationTraceStage::Attempt:
            eventName = "threat_reservation_attempt";
            break;
        case ReservationTraceStage::Result:
            eventName = event.accepted ? "threat_reservation_accepted"
                                       : "threat_reservation_rejected";
            break;
        case ReservationTraceStage::Release:
            eventName = "threat_reservation_release";
            break;
    }

    std::string const safeReason = SafeTraceToken(event.reason);
    auto toTenths = [](float value, bool valid) -> int32 {
        if (!valid)
            return 0;
        double const scaled = static_cast<double>(value) * 10.0;
        double const limit = static_cast<double>(std::numeric_limits<int32>::max());
        return static_cast<int32>(std::max(-limit, std::min(limit, scaled)));
    };
    bool const threatValid = std::isfinite(event.threatPoints);
    bool const headroomValid = std::isfinite(event.rawHeadroom);
    bool const reservedValid = std::isfinite(event.alreadyReserved);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=%s source=partybot_engine_execute group=%u map=%u instance=%u bot=%llu elapsed_ms=%u action_id=%llu owner=%llu threat_owner=%llu hostile=%llu threat_tenths=%d threat_valid=%u headroom_tenths=%d headroom_valid=%u reserved_tenths=%d reserved_valid=%u expires_at_ms=%u rows=%u accepted=%u reason=%s",
             static_cast<unsigned long long>(encounterId), sequence, eventName,
             groupId, mapId, instanceId,
             static_cast<unsigned long long>(context.bot.guid), elapsedMs,
             static_cast<unsigned long long>(event.actionId),
             static_cast<unsigned long long>(event.ownerGuid),
             static_cast<unsigned long long>(event.threatOwnerGuid),
             static_cast<unsigned long long>(event.hostileGuid),
             toTenths(event.threatPoints, threatValid),
             threatValid ? 1u : 0u,
             toTenths(event.rawHeadroom, headroomValid),
             headroomValid ? 1u : 0u,
             toTenths(event.alreadyReserved, reservedValid),
             reservedValid ? 1u : 0u,
             event.expiresAtMs, event.reservationRows,
             event.accepted ? 1u : 0u, safeReason.c_str());
}

void DecisionTrace::RecordEncounterDecisionSummary(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint32 snapshotEpoch, uint64 encounterId, uint32 sequence,
    uint32 elapsedMs, char const* role,
    uint32 candidateCount, uint32 selectedCount) const
{
    // PartyBotAI emits only one summary per bot in its ten-second Shadow
    // detail window. Keep this correlated event independent of the legacy
    // per-tick diagnostic quota.
    if (!m_enabled || !bot || !encounterId || !sequence)
        return;

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

bool DecisionTrace::RecordEncounterCastAttempt(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint64 encounterId, uint32 sequence, uint32 elapsedMs,
    uint32 actionId, uint64 targetGuid, uint32 spellId) const
{
    if (!m_enabled || !bot || !encounterId || !sequence || !actionId)
        return false;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=cast_request_attempt source=legacy executor=DoCastSpell action_id=%u group=%u map=%u instance=%u bot=%u elapsed_ms=%u target=%llu spell=%u",
             static_cast<unsigned long long>(encounterId), sequence, actionId,
             groupId, mapId, instanceId, bot->GetGUIDLow(), elapsedMs,
             static_cast<unsigned long long>(targetGuid), spellId);
    return true;
}

void DecisionTrace::RecordEncounterCastResult(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint64 encounterId, uint32 sequence, uint32 elapsedMs,
    uint32 actionId, uint64 targetGuid, uint32 spellId, uint32 resultCode,
    bool accepted, char const* reason) const
{
    if (!m_enabled || !bot || !encounterId || !sequence || !actionId)
        return;

    std::string const safeReason = SafeTraceToken(reason);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=cast_request_result source=legacy executor=DoCastSpell action_id=%u group=%u map=%u instance=%u bot=%u elapsed_ms=%u target=%llu spell=%u result_code=%u accepted=%u reason=%s",
             static_cast<unsigned long long>(encounterId), sequence, actionId,
             groupId, mapId, instanceId, bot->GetGUIDLow(), elapsedMs,
             static_cast<unsigned long long>(targetGuid), spellId,
             resultCode, accepted ? 1u : 0u, safeReason.c_str());
}

void DecisionTrace::RecordEncounterCastTerminal(
    Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
    uint64 encounterId, uint32 sequence, uint32 elapsedMs,
    uint32 actionId, uint32 spellId, uint64 targetGuid,
    bool success, bool interrupted) const
{
    if (!m_enabled || !bot || !encounterId || !sequence || !actionId)
        return;

    char const* outcome = interrupted ? "interrupted" :
                          (success ? "completed" : "failed");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotEngineTrace] trace_version=1 encounter_id=%llu seq=%u event=cast_terminal source=core_spell_finish action_id=%u group=%u map=%u instance=%u bot=%u elapsed_ms=%u spell=%u target=%llu success=%u interrupted=%u outcome=%s",
             static_cast<unsigned long long>(encounterId), sequence, actionId,
             groupId, mapId, instanceId, bot->GetGUIDLow(), elapsedMs,
             spellId, static_cast<unsigned long long>(targetGuid),
             success ? 1u : 0u, interrupted ? 1u : 0u, outcome);
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
