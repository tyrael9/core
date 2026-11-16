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
class PartyCoordinator {
public:
    bool TryReserve(Reservation const& reservation, std::uint32_t nowMs);
    bool HasReservation(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs);
    std::uint32_t GetReservedAmount(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs);
    void ReleaseOwner(Guid ownerGuid);
    void PruneExpired(std::uint32_t nowMs);
    std::vector<Reservation> const& GetReservations() const { return m_reservations; }

    std::uint32_t NextSnapshotEpoch();
    void SetSnapshot(PartySnapshot snapshot);
    void InvalidateSnapshot() { m_hasSnapshot = false; }
    PartySnapshot const* GetSnapshotIfFresh(std::uint32_t nowMs, std::uint32_t maxAgeMs) const;

private:
    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs);
    std::vector<Reservation> m_reservations;
    PartySnapshot m_snapshot;
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
