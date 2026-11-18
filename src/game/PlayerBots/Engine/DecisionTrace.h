/*
 * PartyBot decision tracing support.
 *
 * This file is part of the VMaNGOS project and is distributed under
 * the GNU General Public License, version 2 or (at your option) any later version.
 */

#ifndef MANGOS_PARTY_BOT_DECISION_TRACE_H
#define MANGOS_PARTY_BOT_DECISION_TRACE_H

#include "Common.h"

class Player;
class Unit;
class SpellEntry;

namespace PartyBotEngine
{
struct PartySnapshot;
struct EncounterHeartbeat;
}

namespace PartyBotEngine
{

class DecisionTrace
{
public:
    DecisionTrace();

    // Begin a bounded trace window for one PartyBot AI update.
    void BeginTick() const;
    bool IsEnabled() const { return m_enabled; }

    // Record one eligibility rejection or spell execution result.
    void Record(Player const* bot, char const* event, Unit const* target,
                SpellEntry const* spell, char const* reason, uint32 detail = 0) const;

    // Encounter boundaries are high-value records and bypass the per-tick detail cap.
    void RecordEncounterBoundary(Player const* bot, char const* event,
                                  uint32 groupId, uint32 mapId, uint32 instanceId,
                                  uint64 encounterId, uint32 sequence,
                                  uint32 elapsedMs, char const* reason) const;

    // One coordinator-throttled, bounded roster/hostile snapshot per encounter heartbeat.
    void RecordPartyHeartbeat(Player const* bot, PartySnapshot const& snapshot,
                              EncounterHeartbeat const& heartbeat) const;

    // Record a bounded status when Shadow cannot produce an evaluation.
    void RecordShadowStatus(Player const* bot, char const* status,
                            uint32 groupId, uint32 mapId, uint32 instanceId) const;

    // Record a bounded summary of read-only PartyBotEngine Shadow evaluation.
    void RecordShadowSummary(Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
                             uint32 snapshotEpoch, uint32 memberCount, uint32 hostileCount,
                             uint32 candidateCount, uint32 selectedCount) const;

    // Record one candidate/selection detail; caller limits these to a few per summary.
    void RecordShadowDecision(Player const* bot, uint32 groupId, uint32 mapId, uint32 instanceId,
                              uint32 snapshotEpoch, char const* role, char const* strategy,
                              char const* action, uint64 targetGuid, float priority,
                              char const* status, char const* reason) const;

private:
    mutable uint32 m_recordsThisTick;
    mutable bool m_enabled;
};

} // namespace PartyBotEngine

#endif
