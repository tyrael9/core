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
// These integral constants are passed by reference to std::min in both the
// coordinator and map-thread config adapter, so C++14 requires definitions.
std::uint32_t const PartyCoordinator::HardMaxDetailTraceEvents;
std::uint32_t const PartyCoordinator::HardMaxImportantTraceEvents;

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

bool PartyCoordinator::ReleaseReservation(ReservationType type, Guid targetGuid,
                                                Guid ownerGuid) {
    if (!targetGuid || !ownerGuid)
        return false;
    std::size_t const originalSize = m_reservations.size();
    m_reservations.erase(std::remove_if(m_reservations.begin(), m_reservations.end(),
        [type, targetGuid, ownerGuid](Reservation const& reservation)
        {
            return reservation.type == type && reservation.targetGuid == targetGuid &&
                reservation.ownerGuid == ownerGuid;
        }), m_reservations.end());
    return m_reservations.size() != originalSize;
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

ActionId PartyCoordinator::NextActionId() {
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

    // A wipe is a party-wide state, not the death of one Bot. Require at least
    // one online roster member and no online living members; offline members
    // do not keep an in-world group alive, while an empty snapshot is unknown.
    std::uint32_t onlineMembers = 0;
    std::uint32_t livingMembers = 0;
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (!member.guid || !member.online)
            continue;
        ++onlineMembers;
        if (member.alive)
            ++livingMembers;
    }

    // An active coordinator is owned by one (group, map, instance) shard.
    // A stale snapshot from another shard must never refresh this encounter's
    // quiet-settle clock or close it from a different Map update thread.
    if (m_encounterActive &&
        (snapshot.groupId != m_encounterGroupId ||
         snapshot.mapId != m_encounterMapId ||
         snapshot.instanceId != m_encounterInstanceId))
        return transition;

    if (m_encounterActive && onlineMembers && !livingMembers)
    {
        // Wipe is an immediate encounter terminal. Repeated dead snapshots are
        // naturally idempotent because CloseEncounter clears the active ID.
        EncounterTransition wiped = CloseEncounter(nowMs, "party_wipe");
        m_reservations.clear();
        m_threatReservations.clear();
        return wiped;
    }

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
            m_traceBudgetEncounterId = encounterId;
            m_traceDetailEvents = 0;
            m_traceImportantEvents = 0;
            m_traceLimitMarkerEmitted = false;
            m_traceTerminalEmitted = false;
            m_lastCombatAtMs = nowMs;
            m_lastEncounterHeartbeatAtMs = 0;
            m_hasEncounterHeartbeat = false;
            m_trackedNonBotPartyMembers.clear();

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
    m_trackedNonBotPartyMembers.clear();
    return transition;
}

std::vector<PartyMemberStateChange>
PartyCoordinator::ObserveNonBotPartyMemberStateChanges(PartySnapshot const& snapshot)
{
    std::vector<PartyMemberStateChange> changes;
    if (!m_encounterActive)
    {
        m_trackedNonBotPartyMembers.clear();
        return changes;
    }

    // A coordinator belongs to one (group, map, instance) shard. A stale or
    // cross-shard snapshot must neither emit events nor replace that shard's
    // last known member baselines.
    if (snapshot.groupId != m_encounterGroupId ||
        snapshot.mapId != m_encounterMapId ||
        snapshot.instanceId != m_encounterInstanceId)
        return changes;

    std::vector<TrackedNonBotPartyMemberState> current;
    current.reserve(snapshot.members.size());
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (!member.guid || !member.online || member.isPlayerBot)
            continue;

        auto previous = std::find_if(
            m_trackedNonBotPartyMembers.begin(), m_trackedNonBotPartyMembers.end(),
            [&member](TrackedNonBotPartyMemberState const& state)
            {
                return state.memberGuid == member.guid;
            });
        if (previous != m_trackedNonBotPartyMembers.end() &&
            (previous->health != member.health ||
             previous->maxHealth != member.maxHealth ||
             previous->alive != member.alive))
        {
            PartyMemberStateChange change;
            change.memberGuid = member.guid;
            change.healthBefore = previous->health;
            change.healthAfter = member.health;
            change.maxHealthBefore = previous->maxHealth;
            change.maxHealthAfter = member.maxHealth;
            change.wasAlive = previous->alive;
            change.isAlive = member.alive;
            changes.push_back(change);
        }

        TrackedNonBotPartyMemberState state;
        state.memberGuid = member.guid;
        state.health = member.health;
        state.maxHealth = member.maxHealth;
        state.alive = member.alive;
        current.push_back(state);
    }
    // Leaving this map drops the baseline; rejoining starts a fresh observation.
    m_trackedNonBotPartyMembers.swap(current);
    return changes;
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

void PartyCoordinator::ConfigureEncounterTraceBudget(std::uint32_t detailLimit,
                                                       std::uint32_t importantLimit)
{
    // Keep the first overflow observable while enforcing absolute per-encounter
    // ceilings, even if an operator enters an accidentally huge value.
    m_traceDetailLimit = std::max<std::uint32_t>(1,
        std::min(detailLimit, HardMaxDetailTraceEvents));
    m_traceImportantLimit = std::max<std::uint32_t>(1,
        std::min(importantLimit, HardMaxImportantTraceEvents));
}

EncounterTraceAdmission PartyCoordinator::TryAdmitEncounterTraceEvent(
    std::uint64_t encounterId, EncounterTraceEventClass eventClass)
{
    // Keep the budget tied to the last encounter ID after CloseEncounter so
    // its terminal boundary can still be emitted through the reserved path.
    if (!encounterId || encounterId != m_traceBudgetEncounterId)
        return EncounterTraceAdmission::Suppressed;

    if (eventClass == EncounterTraceEventClass::Terminal)
    {
        if (m_traceTerminalEmitted)
            return EncounterTraceAdmission::Suppressed;
        m_traceTerminalEmitted = true;
        return EncounterTraceAdmission::Accepted;
    }

    if (!m_encounterActive || m_encounterId != encounterId)
        return EncounterTraceAdmission::Suppressed;

    std::uint32_t& used = eventClass == EncounterTraceEventClass::Important
        ? m_traceImportantEvents : m_traceDetailEvents;
    std::uint32_t const limit = eventClass == EncounterTraceEventClass::Important
        ? m_traceImportantLimit : m_traceDetailLimit;
    if (used < limit)
    {
        ++used;
        return EncounterTraceAdmission::Accepted;
    }

    if (!m_traceLimitMarkerEmitted)
    {
        m_traceLimitMarkerEmitted = true;
        return EncounterTraceAdmission::EmitLimitMarker;
    }
    return EncounterTraceAdmission::Suppressed;
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
