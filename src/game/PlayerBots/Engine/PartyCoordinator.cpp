#include "PartyCoordinator.h"
#include <algorithm>
namespace PartyBotEngine {
bool PartyCoordinator::IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs) {
    return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
}
void PartyCoordinator::PruneExpired(std::uint32_t nowMs) {
    m_reservations.erase(std::remove_if(m_reservations.begin(), m_reservations.end(),
        [nowMs](Reservation const& r) { return IsExpired(nowMs, r.expiresAtMs); }), m_reservations.end());
}
bool PartyCoordinator::TryReserve(Reservation const& reservation, std::uint32_t nowMs) {
    PruneExpired(nowMs);
    if (!reservation.targetGuid || !reservation.ownerGuid || IsExpired(nowMs, reservation.expiresAtMs)) return false;
    for (auto& existing : m_reservations) {
        if (existing.type != reservation.type || existing.targetGuid != reservation.targetGuid) continue;
        if (existing.ownerGuid != reservation.ownerGuid) return false;
        existing = reservation;
        return true;
    }
    m_reservations.push_back(reservation);
    return true;
}
bool PartyCoordinator::HasReservation(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs) {
    PruneExpired(nowMs);
    for (auto const& r : m_reservations)
        if (r.type == type && r.targetGuid == targetGuid && r.ownerGuid != exceptOwner) return true;
    return false;
}
std::uint32_t PartyCoordinator::GetReservedAmount(ReservationType type, Guid targetGuid, Guid exceptOwner, std::uint32_t nowMs) {
    PruneExpired(nowMs);
    std::uint32_t total = 0;
    for (auto const& r : m_reservations)
        if (r.type == type && r.targetGuid == targetGuid && r.ownerGuid != exceptOwner) total += r.amount;
    return total;
}
void PartyCoordinator::ReleaseOwner(Guid ownerGuid) {
    m_reservations.erase(std::remove_if(m_reservations.begin(), m_reservations.end(),
        [ownerGuid](Reservation const& r) { return r.ownerGuid == ownerGuid; }), m_reservations.end());
}
}
