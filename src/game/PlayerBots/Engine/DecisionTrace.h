/*
 * PartyBot decision tracing support.
 *
 * This file is part of the VMaNGOS project and is distributed under
 * the GNU General Public License, version 2 or (at your option) any later version.
 */

#ifndef MANGOS_PARTY_BOT_DECISION_TRACE_H
#define MANGOS_PARTY_BOT_DECISION_TRACE_H

#include "Common.h"
#include "Engine/DecisionContext.h"

class Player;
class Unit;
class SpellEntry;

namespace PartyBotEngine
{
class PartyCoordinator;
struct PartyMemberStateChange;

class DecisionTrace : public ReservationTraceSink
{
public:
    DecisionTrace();

    // Begin a bounded trace window for one PartyBot AI update.
    void BeginTick() const;
    bool IsEnabled() const { return m_enabled; }
    void RecordReservationEvent(DecisionContext const& context,
                                ReservationTraceEvent const& event) override;
    void RecordEngineActionEvent(DecisionContext const& context,
                                 EngineActionTraceEvent const& event) override;

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
    // A successful Legacy AttackStart target switch, after Core changes the actual victim.
    void RecordEncounterTargetChange(Player const* bot, uint32 groupId, uint32 mapId,
                                     uint32 instanceId, uint64 encounterId,
                                     uint32 sequence, uint32 elapsedMs,
                                     uint64 oldVictimGuid, uint64 newVictimGuid,
                                     char const* source) const;
    // Death/resurrection transitions are high-value encounter events, not per-tick detail.
    void RecordEncounterAliveState(Player const* bot, uint32 groupId, uint32 mapId,
                                   uint32 instanceId, uint64 encounterId,
                                   uint32 sequence, uint32 elapsedMs,
                                   bool wasAlive, bool isAlive) const;
    bool RecordEncounterPartyMemberState(
        Player const* bot, PartyCoordinator& coordinator,
        uint32 groupId, uint32 mapId, uint32 instanceId,
        uint64 encounterId, uint32 elapsedMs,
        PartyMemberStateChange const& change) const;

    // Encounter-correlated Shadow summary and candidate records share the coordinator sequence.
    void RecordEncounterDecisionSummary(Player const* bot, uint32 groupId, uint32 mapId,
                                        uint32 instanceId, uint32 snapshotEpoch,
                                        uint64 encounterId, uint32 sequence,
                                        uint32 elapsedMs, char const* role,
                                        DecisionSummary const& summary) const;
    void RecordEncounterTriggerSummary(Player const* bot, uint32 groupId, uint32 mapId,
                                       uint32 instanceId, uint64 encounterId,
                                       uint32 sequence, uint32 elapsedMs,
                                       uint32 total, uint32 evaluated,
                                       uint32 active, uint32 inactive,
                                       uint32 cadenceSkipped, uint32 omitted) const;
    void RecordEncounterTrigger(Player const* bot, uint32 groupId, uint32 mapId,
                                uint32 instanceId, uint64 encounterId,
                                uint32 sequence, uint32 elapsedMs,
                                char const* role, char const* strategy,
                                char const* trigger, uint32 intervalMs,
                                char const* status, uint32 candidateCount) const;
    void RecordEncounterCandidate(Player const* bot, uint32 groupId, uint32 mapId,
                                  uint32 instanceId, uint32 snapshotEpoch,
                                  uint64 encounterId, uint32 sequence,
                                  uint32 elapsedMs, char const* role,
                                  char const* strategy, char const* trigger,
                                  char const* action, uint64 targetGuid,
                                  uint32 spellId, uint32 channel,
                                  float priority, char const* status,
                                  char const* reason) const;

    // Pair the actual Legacy Core CastSpell call with its synchronous result.
    bool RecordEncounterCastAttempt(Player const* bot, uint32 groupId, uint32 mapId,
                                    uint32 instanceId, uint64 encounterId,
                                    uint32 sequence, uint32 elapsedMs,
                                    uint64 actionId, uint64 targetGuid,
                                    uint32 spellId) const;
    // SPELL_CAST_OK means accepted, not that a non-triggered cast has completed.
    void RecordEncounterCastResult(Player const* bot, uint32 groupId, uint32 mapId,
                                   uint32 instanceId, uint64 encounterId,
                                   uint32 sequence, uint32 elapsedMs,
                                   uint64 actionId, uint64 targetGuid,
                                   uint32 spellId, uint32 resultCode,
                                   bool accepted, char const* reason) const;
    void RecordEncounterCastTerminal(Player const* bot, uint32 groupId, uint32 mapId,
                                     uint32 instanceId, uint64 encounterId,
                                     uint32 sequence, uint32 elapsedMs,
                                     uint64 actionId, uint32 spellId,
                                     uint64 targetGuid, bool success,
                                     bool interrupted) const;

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
