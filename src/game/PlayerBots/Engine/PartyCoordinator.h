#ifndef MANGOS_PARTYBOT_PARTY_COORDINATOR_H
#define MANGOS_PARTYBOT_PARTY_COORDINATOR_H
#include "PartySnapshot.h"
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
class PartyCoordinator {
public:
    bool TryReserve(Reservation const& reservation, std::uint32_t nowMs);
    bool HasReservation(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs);
    std::uint32_t GetReservedAmount(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs);
    void ReleaseOwner(Guid ownerGuid);
    void PruneExpired(std::uint32_t nowMs);
    std::vector<Reservation> const& GetReservations() const { return m_reservations; }

    // Action IDs are coordinator-local, nonzero and unique among retained reservations.
    ThreatActionId NextThreatActionId();
    bool TryReserveThreatImpact(ThreatReservation const& reservation, std::uint32_t nowMs);
    float GetReservedThreatImpact(Guid hostileGuid, Guid threatOwnerGuid,
                                  ThreatActionId exceptActionId, std::uint32_t nowMs) const;
    void ReleaseThreatAction(ThreatActionId actionId);
    std::size_t GetThreatReservationCount() const { return m_threatReservations.size(); }

    std::uint32_t NextSnapshotEpoch();
    void SetSnapshot(PartySnapshot snapshot);
    void InvalidateSnapshot() { m_hasSnapshot = false; }
    PartySnapshot const* GetSnapshotIfFresh(std::uint32_t nowMs, std::uint32_t maxAgeMs) const;

private:
    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs);
    std::vector<Reservation> m_reservations;
    std::vector<ThreatReservation> m_threatReservations;
    PartySnapshot m_snapshot;
    ThreatActionId m_nextThreatActionId = 0;
    std::uint32_t m_nextSnapshotEpoch = 0;
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
