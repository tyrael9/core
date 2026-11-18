#include "PartyCoordinator.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace PartyBotEngine {
namespace {
using RegistryKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
using Registry = std::map<RegistryKey, std::weak_ptr<PartyCoordinator>>;
constexpr std::size_t MaxThreatReservations = 128;

std::atomic<std::uint64_t>& GetNextEncounterId()
{
    static std::atomic<std::uint64_t> nextId(1);
    return nextId;
}

bool HasElapsed(std::uint32_t nowMs, std::uint32_t sinceMs, std::uint32_t durationMs)
{
    std::uint32_t const elapsed = nowMs - sinceMs;
    // Keep the unsigned subtraction wrap-safe while rejecting backwards or
    // unsupported half-range clock jumps.
    return elapsed < 0x80000000u && elapsed >= durationMs;
}

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
    m_threatReservations.erase(std::remove_if(m_threatReservations.begin(), m_threatReservations.end(),
        [nowMs](ThreatReservation const& r) { return IsExpired(nowMs, r.expiresAtMs); }),
        m_threatReservations.end());
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
    // The logical action owner can differ from the threat recipient (for example a pet).
    m_threatReservations.erase(std::remove_if(m_threatReservations.begin(), m_threatReservations.end(),
        [ownerGuid](ThreatReservation const& reservation) { return reservation.ownerGuid == ownerGuid; }),
        m_threatReservations.end());
}

ThreatActionId PartyCoordinator::NextThreatActionId() {
    // At most 128 IDs can be retained, so a free nonzero ID is guaranteed within
    // one pass unless the uint64_t namespace is pathologically exhausted.
    for (std::size_t attempt = 0; attempt <= m_threatReservations.size(); ++attempt) {
        ++m_nextThreatActionId;
        if (m_nextThreatActionId == 0)
            ++m_nextThreatActionId;

        bool inUse = false;
        for (ThreatReservation const& reservation : m_threatReservations) {
            if (reservation.actionId == m_nextThreatActionId) {
                inUse = true;
                break;
            }
        }
        if (!inUse)
            return m_nextThreatActionId;
    }
    return 0;
}

bool PartyCoordinator::TryReserveThreatImpact(ThreatReservation const& reservation,
                                               std::uint32_t nowMs) {
    PruneExpired(nowMs);

    // Deadlines use the same wrap-safe monotonic millisecond domain as GetMSTime.
    // Require a positive signed delta, which also rejects unsupported deadlines
    // more than INT32_MAX milliseconds into the future.
    if (!reservation.ownerGuid || !reservation.threatOwnerGuid || !reservation.hostileGuid ||
        !reservation.actionId || !std::isfinite(reservation.threatPoints) ||
        reservation.threatPoints <= 0.0f ||
        static_cast<std::int32_t>(reservation.expiresAtMs - nowMs) <= 0)
        return false;

    // One row represents one (action, hostile, actual threat-owner) impact.
    // Re-submitting the same row replaces it, so retries are idempotent.
    for (ThreatReservation& existing : m_threatReservations) {
        if (existing.ownerGuid == reservation.ownerGuid &&
            existing.actionId == reservation.actionId &&
            existing.hostileGuid == reservation.hostileGuid &&
            existing.threatOwnerGuid == reservation.threatOwnerGuid) {
            existing = reservation;
            return true;
        }
    }

    // Fail closed at capacity: never evict another in-flight action's budget.
    if (m_threatReservations.size() >= MaxThreatReservations)
        return false;

    m_threatReservations.push_back(reservation);
    return true;
}

float PartyCoordinator::GetReservedThreatImpact(Guid hostileGuid, Guid threatOwnerGuid,
                                                 ThreatActionId exceptActionId,
                                                 std::uint32_t nowMs) const {
    if (!hostileGuid || !threatOwnerGuid)
        return 0.0f;

    double total = 0.0;
    for (ThreatReservation const& reservation : m_threatReservations) {
        if (!IsExpired(nowMs, reservation.expiresAtMs) &&
            reservation.hostileGuid == hostileGuid &&
            reservation.threatOwnerGuid == threatOwnerGuid &&
            (!exceptActionId || reservation.actionId != exceptActionId))
            total += static_cast<double>(reservation.threatPoints);
    }

    // The bounded list and finite inputs keep the double sum safe; saturate the
    // public float result rather than allowing overflow to become a negative/NaN budget.
    double const maxFloat = static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(std::min(total, maxFloat));
}

void PartyCoordinator::ReleaseThreatAction(ThreatActionId actionId) {
    if (!actionId)
        return;
    m_threatReservations.erase(std::remove_if(m_threatReservations.begin(), m_threatReservations.end(),
        [actionId](ThreatReservation const& reservation) { return reservation.actionId == actionId; }),
        m_threatReservations.end());
}

EncounterTransition PartyCoordinator::ObserveEncounter(PartySnapshot const& snapshot,
                                                                      std::uint32_t nowMs,
                                                                      std::uint32_t settleMs)
{
    EncounterTransition transition;
    if (!snapshot.groupId || !snapshot.epoch)
        return transition;

    bool combatObserved = false;
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (member.online && member.alive && member.inCombat)
        {
            combatObserved = true;
            break;
        }
    }
    if (!combatObserved)
    {
        for (PartyCompanionSnapshot const& companion : snapshot.companions)
        {
            if (companion.online && companion.alive && companion.inCombat)
            {
                combatObserved = true;
                break;
            }
        }
    }

    if (combatObserved)
    {
        if (!m_encounterActive)
        {
            std::uint64_t encounterId = GetNextEncounterId().fetch_add(1, std::memory_order_relaxed);
            if (!encounterId)
                encounterId = GetNextEncounterId().fetch_add(1, std::memory_order_relaxed);

            m_encounterActive = true;
            m_encounterId = encounterId;
            m_encounterSequence = 1;
            m_encounterGroupId = snapshot.groupId;
            m_encounterMapId = snapshot.mapId;
            m_encounterInstanceId = snapshot.instanceId;
            m_encounterStartedAtMs = nowMs;
            m_lastCombatAtMs = nowMs;
            m_lastEncounterHeartbeatAtMs = 0;
            m_hasEncounterHeartbeat = false;

            transition.kind = EncounterTransitionKind::Started;
            transition.encounterId = m_encounterId;
            transition.sequence = m_encounterSequence;
            transition.groupId = snapshot.groupId;
            transition.mapId = snapshot.mapId;
            transition.instanceId = snapshot.instanceId;
            transition.reason = "combat_detected";
            return transition;
        }

        m_lastCombatAtMs = nowMs;
        return transition;
    }

    // The caller supplies a bounded settle interval. Zero or half-range values
    // are not meaningful monotonic deadlines, so do not end an encounter on them.
    if (m_encounterActive && settleMs > 0 && settleMs < 0x80000000u &&
        HasElapsed(nowMs, m_lastCombatAtMs, settleMs))
        return CloseEncounter(nowMs, "combat_settled");

    return transition;
}

EncounterTransition PartyCoordinator::CloseEncounter(std::uint32_t nowMs, char const* reason)
{
    EncounterTransition transition;
    if (!m_encounterActive || !m_encounterId)
        return transition;

    transition.kind = EncounterTransitionKind::Ended;
    transition.encounterId = m_encounterId;
    transition.sequence = NextEncounterSequence();
    transition.groupId = m_encounterGroupId;
    transition.mapId = m_encounterMapId;
    transition.instanceId = m_encounterInstanceId;
    transition.elapsedMs = nowMs - m_encounterStartedAtMs;
    transition.reason = reason ? reason : "explicit_end";

    m_encounterActive = false;
    m_encounterId = 0;
    m_encounterSequence = 0;
    m_encounterGroupId = 0;
    m_encounterMapId = 0;
    m_encounterInstanceId = 0;
    m_encounterStartedAtMs = 0;
    m_lastCombatAtMs = 0;
    m_lastEncounterHeartbeatAtMs = 0;
    m_hasEncounterHeartbeat = false;
    return transition;
}

bool PartyCoordinator::TryBeginEncounterHeartbeat(std::uint32_t nowMs,
                                                    std::uint32_t intervalMs,
                                                    EncounterHeartbeat& heartbeat)
{
    if (!m_encounterActive || !m_encounterId || !intervalMs ||
        intervalMs >= 0x80000000u)
        return false;

    if (m_hasEncounterHeartbeat &&
        !HasElapsed(nowMs, m_lastEncounterHeartbeatAtMs, intervalMs))
        return false;

    m_lastEncounterHeartbeatAtMs = nowMs;
    m_hasEncounterHeartbeat = true;

    heartbeat.encounterId = m_encounterId;
    heartbeat.sequence = NextEncounterSequence();
    heartbeat.groupId = m_encounterGroupId;
    heartbeat.mapId = m_encounterMapId;
    heartbeat.instanceId = m_encounterInstanceId;
    heartbeat.elapsedMs = nowMs - m_encounterStartedAtMs;
    return heartbeat.sequence != 0;
}

std::uint32_t PartyCoordinator::NextEncounterSequence()
{
    if (!m_encounterActive)
        return 0;
    ++m_encounterSequence;
    if (!m_encounterSequence)
        ++m_encounterSequence;
    return m_encounterSequence;
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
