#include "PartyCoordinator.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace PartyBotEngine {
namespace {
using RegistryKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
using Registry = std::map<RegistryKey, std::weak_ptr<PartyCoordinator>>;

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

std::mutex& GetRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

void PruneExpiredEntries(Registry& registry) {
    for (auto itr = registry.begin(); itr != registry.end();) {
        if (itr->second.expired())
            itr = registry.erase(itr);
        else
            ++itr;
    }
}
}

bool PartyCoordinator::IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs) {
    return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
}

void PartyCoordinator::PruneExpired(std::uint32_t nowMs) {
    m_reservations.erase(std::remove_if(m_reservations.begin(), m_reservations.end(),
        [nowMs](Reservation const& r) { return IsExpired(nowMs, r.expiresAtMs); }), m_reservations.end());
}

bool PartyCoordinator::TryReserve(Reservation const& reservation, std::uint32_t nowMs) {
    PruneExpired(nowMs);
    if (!reservation.targetGuid || !reservation.ownerGuid || IsExpired(nowMs, reservation.expiresAtMs))
        return false;

    for (auto& existing : m_reservations) {
        if (existing.type != reservation.type || existing.targetGuid != reservation.targetGuid)
            continue;
        if (existing.ownerGuid != reservation.ownerGuid)
            return false;
        existing = reservation;
        return true;
    }

    m_reservations.push_back(reservation);
    return true;
}

bool PartyCoordinator::HasReservation(ReservationType type, Guid targetGuid, Guid exceptOwner,
                                       std::uint32_t nowMs) {
    PruneExpired(nowMs);
    for (auto const& reservation : m_reservations) {
        if (reservation.type == type && reservation.targetGuid == targetGuid &&
            reservation.ownerGuid != exceptOwner)
            return true;
    }
    return false;
}

std::uint32_t PartyCoordinator::GetReservedAmount(ReservationType type, Guid targetGuid,
                                                   Guid exceptOwner, std::uint32_t nowMs) {
    PruneExpired(nowMs);
    std::uint32_t total = 0;
    for (auto const& reservation : m_reservations) {
        if (reservation.type == type && reservation.targetGuid == targetGuid &&
            reservation.ownerGuid != exceptOwner)
            total += reservation.amount;
    }
    return total;
}

void PartyCoordinator::ReleaseOwner(Guid ownerGuid) {
    m_reservations.erase(std::remove_if(m_reservations.begin(), m_reservations.end(),
        [ownerGuid](Reservation const& reservation) { return reservation.ownerGuid == ownerGuid; }),
        m_reservations.end());
}

std::uint32_t PartyCoordinator::NextSnapshotEpoch() {
    ++m_nextSnapshotEpoch;
    if (m_nextSnapshotEpoch == 0)
        ++m_nextSnapshotEpoch;
    return m_nextSnapshotEpoch;
}

void PartyCoordinator::SetSnapshot(PartySnapshot snapshot) {
    m_snapshot = std::move(snapshot);
    m_hasSnapshot = true;
}

PartySnapshot const* PartyCoordinator::GetSnapshotIfFresh(std::uint32_t nowMs,
                                                            std::uint32_t maxAgeMs) const {
    if (!m_hasSnapshot)
        return nullptr;

    // Unsigned subtraction is intentional: it remains correct across uint32 millisecond wraparound.
    if (static_cast<std::uint32_t>(nowMs - m_snapshot.capturedAtMs) > maxAgeMs)
        return nullptr;

    return &m_snapshot;
}

std::shared_ptr<PartyCoordinator> PartyCoordinatorRegistry::GetOrCreate(std::uint32_t groupId,
                                                                         std::uint32_t mapId,
                                                                         std::uint32_t instanceId) {
    // Group ID zero is used by battleground groups; they stay on the Legacy path.
    if (groupId == 0)
        return std::shared_ptr<PartyCoordinator>();

    std::lock_guard<std::mutex> lock(GetRegistryMutex());
    Registry& registry = GetRegistry();

    // Keep stale weak keys bounded during long-running realms with transient groups.
    if (registry.size() > 64)
        PruneExpiredEntries(registry);

    RegistryKey const key(groupId, mapId, instanceId);
    auto itr = registry.find(key);
    if (itr != registry.end()) {
        std::shared_ptr<PartyCoordinator> existing = itr->second.lock();
        if (existing)
            return existing;
    }

    std::shared_ptr<PartyCoordinator> coordinator = std::make_shared<PartyCoordinator>();
    registry[key] = coordinator;
    return coordinator;
}

void PartyCoordinatorRegistry::PruneExpired() {
    std::lock_guard<std::mutex> lock(GetRegistryMutex());
    PruneExpiredEntries(GetRegistry());
}
}
