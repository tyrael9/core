#ifndef MANGOS_PARTYBOT_PARTY_COORDINATOR_H
#define MANGOS_PARTYBOT_PARTY_COORDINATOR_H
#include "PartySnapshot.h"
#include "Formation.h"
#include <memory>
#include <vector>
namespace PartyBotEngine {
struct Reservation {
    ReservationType type = ReservationType::Target;
    Guid targetGuid = 0, ownerGuid = 0;
    std::uint32_t expiresAtMs = 0, amount = 0, spellId = 0;
};

// A pending threat-producing action may affect multiple hostiles and may
// generate threat under a pet/guardian GUID rather than the acting bot GUID.
// ownerGuid identifies the bot that owns the action; threatOwnerGuid is the
// exact Unit GUID whose ThreatManager reference will receive the threat.
struct ThreatReservation {
    Guid ownerGuid = 0;
    Guid threatOwnerGuid = 0;
    Guid hostileGuid = 0;
    ThreatActionId actionId = 0;
    float threatPoints = 0.0f;
    std::uint32_t expiresAtMs = 0;
};

enum class EncounterTransitionKind : std::uint8_t { None, Started, Ended };

struct PartyMemberStateChange
{
    Guid memberGuid = 0;
    std::uint32_t healthBefore = 0, healthAfter = 0;
    std::uint32_t maxHealthBefore = 0, maxHealthAfter = 0;
    bool wasAlive = false, isAlive = false;
};

struct EncounterHeartbeat
{
    std::uint64_t encounterId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t groupId = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t elapsedMs = 0;
};

enum class EncounterTraceEventClass : std::uint8_t
{
    Detail,
    Important,
    Terminal
};

enum class EncounterTraceAdmission : std::uint8_t
{
    Accepted,
    Suppressed,
    EmitLimitMarker
};

struct EncounterTransition {
    EncounterTransitionKind kind = EncounterTransitionKind::None;
    std::uint64_t encounterId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t groupId = 0;
    std::uint32_t mapId = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t elapsedMs = 0;
    char const* reason = "none";
};

class PartyCoordinator {
public:
    bool TryReserve(Reservation const& reservation, std::uint32_t nowMs);
    bool HasReservation(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs);
    std::uint32_t GetReservedAmount(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs);
    void ReleaseOwner(Guid ownerGuid);
    bool ReleaseReservation(ReservationType type, Guid targetGuid, Guid ownerGuid);

    // Stable, value-only formation slots shared by every Bot in this party shard.
    void ReconcileFormationAssignments(PartySnapshot const& snapshot)
    { m_formationAssignments.Reconcile(snapshot); }
    std::uint8_t GetFormationSlot(Guid botGuid) const
    { return m_formationAssignments.GetSlot(botGuid); }
    void PruneExpired(std::uint32_t nowMs);
    std::vector<Reservation> const& GetReservations() const { return m_reservations; }

    // Action IDs are coordinator-local and nonzero. The legacy-named wrapper
    // remains for callers that allocate IDs specifically for threat rows.
    ActionId NextActionId();
    ThreatActionId NextThreatActionId() { return NextActionId(); }
    bool TryReserveThreatImpact(ThreatReservation const& reservation, std::uint32_t nowMs);
    float GetReservedThreatImpact(Guid hostileGuid, Guid threatOwnerGuid,
                                  ThreatActionId exceptActionId, std::uint32_t nowMs) const;
    void ReleaseThreatAction(ThreatActionId actionId);
    void ReleaseThreatAction(ThreatActionId actionId, Guid ownerGuid);
    std::size_t GetThreatReservationCount() const { return m_threatReservations.size(); }

    // One map/instance-scoped encounter lifecycle shared by every bot in the party.
    // Calls consume value-only snapshots; no Core object pointers are retained.
    EncounterTransition ObserveEncounter(PartySnapshot const& snapshot,
                                         std::uint32_t nowMs,
                                         std::uint32_t settleMs);
    EncounterTransition CloseEncounter(std::uint32_t nowMs, char const* reason);
    // Emit one shared, interval-limited party snapshot heartbeat per active encounter.
    bool TryBeginEncounterHeartbeat(std::uint32_t nowMs, std::uint32_t intervalMs,
                                    EncounterHeartbeat& heartbeat);
    std::vector<PartyMemberStateChange> ObserveNonBotPartyMemberStateChanges(
        PartySnapshot const& snapshot);
    std::uint32_t NextEncounterSequence();
    EncounterTraceAdmission TryAdmitEncounterTraceEvent(
        std::uint64_t encounterId, EncounterTraceEventClass eventClass);
    // Defaults keep normal encounter traces compact on the 1 OCPU ARM64 host.
    static std::uint32_t const MaxDetailTraceEvents = 1024;
    static std::uint32_t const MaxImportantTraceEvents = 256;
    // Runtime settings may lower the defaults, but cannot make one encounter unbounded.
    static std::uint32_t const HardMaxDetailTraceEvents = 4096;
    static std::uint32_t const HardMaxImportantTraceEvents = 1024;
    void ConfigureEncounterTraceBudget(std::uint32_t detailLimit,
                                       std::uint32_t importantLimit);
    std::uint64_t GetEncounterId() const { return m_encounterId; }
    std::uint32_t GetEncounterGroupId() const { return m_encounterGroupId; }
    std::uint32_t GetEncounterMapId() const { return m_encounterMapId; }
    std::uint32_t GetEncounterInstanceId() const { return m_encounterInstanceId; }
    std::uint32_t GetEncounterElapsedMs(std::uint32_t nowMs) const {
        return m_encounterActive ? nowMs - m_encounterStartedAtMs : 0;
    }
    bool HasActiveEncounter() const { return m_encounterActive; }

    std::uint32_t NextSnapshotEpoch();
    void SetSnapshot(PartySnapshot snapshot);
    void InvalidateSnapshot() { m_hasSnapshot = false; }
    PartySnapshot const* GetSnapshotIfFresh(std::uint32_t nowMs, std::uint32_t maxAgeMs) const;

private:
    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs);
    std::vector<Reservation> m_reservations;
    std::vector<ThreatReservation> m_threatReservations;
    FormationAssignments m_formationAssignments;
    struct TrackedNonBotPartyMemberState
    {
        Guid memberGuid = 0;
        std::uint32_t health = 0, maxHealth = 0;
        bool alive = false;
    };
    std::vector<TrackedNonBotPartyMemberState> m_trackedNonBotPartyMembers;
    PartySnapshot m_snapshot;
    ThreatActionId m_nextThreatActionId = 0;
    std::uint64_t m_encounterId = 0;
    std::uint32_t m_encounterSequence = 0;
    std::uint32_t m_encounterGroupId = 0;
    std::uint32_t m_encounterMapId = 0;
    std::uint32_t m_encounterInstanceId = 0;
    std::uint32_t m_encounterStartedAtMs = 0;
    std::uint32_t m_lastCombatAtMs = 0;
    std::uint32_t m_lastEncounterHeartbeatAtMs = 0;
    std::uint32_t m_nextSnapshotEpoch = 0;
    std::uint64_t m_traceBudgetEncounterId = 0;
    std::uint32_t m_traceDetailEvents = 0;
    std::uint32_t m_traceImportantEvents = 0;
    std::uint32_t m_traceDetailLimit = MaxDetailTraceEvents;
    std::uint32_t m_traceImportantLimit = MaxImportantTraceEvents;
    bool m_traceLimitMarkerEmitted = false;
    bool m_traceTerminalEmitted = false;
    bool m_encounterActive = false;
    bool m_hasEncounterHeartbeat = false;
    bool m_hasSnapshot = false;
};

// Registry holds weak references only. PartyBotAI instances must retain a shared_ptr
// for the lifetime of their current (group,map,instance) shard.
class PartyCoordinatorRegistry {
public:
    static std::shared_ptr<PartyCoordinator> GetOrCreate(std::uint32_t groupId,
                                                          std::uint32_t mapId,
                                                          std::uint32_t instanceId);
    static void PruneExpired();
};
}
#endif
