#ifndef MANGOS_PARTYBOT_PARTY_SNAPSHOT_H
#define MANGOS_PARTYBOT_PARTY_SNAPSHOT_H
#include "DecisionTypes.h"
#include <cstddef>
#include <vector>
namespace PartyBotEngine {
enum class PartyMemberDeathState : std::uint8_t
{
    Alive = 0,
    Dying,
    Corpse,
    ReleasedGhost,
    Resurrecting,
    CorpseFalling,
    Unknown
};
struct DirectHealSpellSnapshot
{
    std::uint32_t spellId = 0;
    std::uint32_t baseHealing = 0;
    std::uint32_t manaCost = 0;
    // Actual Core cast time, including the actor's current modifiers.
    std::uint32_t castTimeMs = 0;
    // This certifies only the one-target direct-heal effect shape. It does
    // not certify proc closure, threat bounds or Core pre-impact admission.
    bool supportedSingleTargetDirect = true;

    DirectHealSpellSnapshot() = default;
    DirectHealSpellSnapshot(std::uint32_t spell, std::uint32_t healing,
                            std::uint32_t mana, std::uint32_t castTime = 0,
                            bool supportedSingleTarget = true)
        : spellId(spell), baseHealing(healing), manaCost(mana),
          castTimeMs(castTime),
          supportedSingleTargetDirect(supportedSingleTarget) {}
};
constexpr std::size_t MAX_PERIODIC_HEAL_FORECASTS_PER_MEMBER = 32;

struct PeriodicHealForecastSnapshot
{
    Guid casterGuid = 0;
    std::uint32_t spellId = 0;
    std::uint32_t firstTickMs = 0;
    std::uint32_t periodMs = 0;
    std::uint32_t remainingTicks = 0;
    // Core rand_ditheru may return floor(amount) or ceil(amount) on each tick.
    // healingPerTick is the guaranteed lower bound; zero upper means exact legacy fixture data.
    std::uint32_t healingPerTick = 0;
    std::uint32_t healingPerTickUpperBound = 0;
};

constexpr std::size_t MAX_SCHOOL_ABSORB_FORECASTS_PER_MEMBER = 16;

struct SchoolAbsorbForecastSnapshot
{
    std::uint32_t spellId = 0;
    std::uint32_t schoolMask = 0;
    std::uint32_t remainingAmount = 0;
    // UINT32_MAX represents a non-expiring Core Aura.
    std::uint32_t remainingDurationMs = 0;
};

struct PartyMemberSnapshot {
    Guid guid = 0;
    std::uint8_t classId = 0, roleId = 0, level = 0, powerType = 0;
    std::uint32_t mapId = 0, instanceId = 0;
    std::uint32_t health = 0, maxHealth = 0, power = 0, maxPower = 0;
    // Conservative F54c forecast values copied from live Core state; no Core pointers survive the build.
    std::uint32_t periodicHealRemainingAmount = 0;
    std::uint32_t allSchoolAbsorbRemainingAmount = 0;
    // F54l: bounded, value-only school-specific absorb pools. Truncation
    // invalidates the entire set for projection; no partial shield credit.
    std::vector<SchoolAbsorbForecastSnapshot> schoolAbsorbForecasts;
    bool schoolAbsorbForecastsTruncated = false;
    // Bounded F54d timing detail. A truncated list is never a complete timeline.
    std::vector<PeriodicHealForecastSnapshot> periodicHealForecasts;
    bool periodicHealForecastsTruncated = false;
    Guid victimGuid = 0;
    Guid selectionGuid = 0;
    Guid followTargetGuid = 0;
    std::uint8_t followFormationSlot = 0xff;
    ActionId pointMovementActionId = 0;
    std::uint64_t pointMovementEncounterId = 0;
    std::uint32_t pointMovementPointId = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    std::uint32_t currentSpellId = 0, currentSpellState = 0;
    // Core Spell::m_timer copied while the current direct heal is preparing/casting.
    std::uint32_t currentSpellRemainingCastMs = 0;
    std::uint32_t resurrectionSpellId = 0;
    std::vector<DirectHealSpellSnapshot> directHealSpells;
    Guid currentSpellTargetGuid = 0;
    // Core-calculated heal amount for a currently preparing/casting, explicit single-target direct heal.
    // Engine-owned casts are excluded because their F54b Heal reservation already represents them.
    std::uint32_t currentSpellHealingAmount = 0;
    ActionId currentSpellActionId = 0;
    std::uint64_t currentSpellEncounterId = 0;
    Guid currentSpellTraceTargetGuid = 0;
    bool currentSpellEngineAction = false;
    std::uint8_t shapeshiftForm = 0, currentSpellType = 0xff;
    PartyMemberDeathState deathState = PartyMemberDeathState::Alive;
    bool resurrectionPending = false, inDuel = false;
    bool online = false, alive = false, inCombat = false, isMainTank = false;
    bool victimAttackable = false;
    bool isPlayerBot = false;
    bool mounted = false, taxiFlying = false;
    Guid chaseTargetGuid = 0;
};
struct PartyCompanionSnapshot {
    Guid guid = 0, ownerGuid = 0, victimGuid = 0;
    std::uint32_t mapId = 0, instanceId = 0;
    std::uint32_t health = 0, maxHealth = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool online = false, alive = false, inCombat = false;
};
struct ThreatEntrySnapshot {
    Guid unitGuid = 0;
    float threat = 0.0f;
    bool online = false, accessible = false, meleeReachable = false;
};
struct HostileSnapshot {
    Guid guid = 0, victimGuid = 0;
    std::uint32_t entry = 0, rank = 0;
    std::uint32_t health = 0, maxHealth = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool positionValid = false;
    bool alive = false, worldBoss = false, crowdControlProtected = false;
    std::vector<ThreatEntrySnapshot> threat;
};
// Immutable-by-convention value data. Never store live Core object pointers here.
struct PartySnapshot {
    std::uint32_t groupId = 0, mapId = 0, instanceId = 0, epoch = 0, capturedAtMs = 0;
    Guid groupLeaderGuid = 0;
    std::vector<PartyMemberSnapshot> members;
    std::vector<PartyCompanionSnapshot> companions;
    std::vector<HostileSnapshot> hostiles;
    PartyMemberSnapshot const* FindMember(Guid guid) const {
        for (auto const& member : members) if (member.guid == guid) return &member;
        return nullptr;
    }
    PartyCompanionSnapshot const* FindCompanion(Guid guid) const {
        for (auto const& companion : companions) if (companion.guid == guid) return &companion;
        return nullptr;
    }
    HostileSnapshot const* FindHostile(Guid guid) const {
        for (auto const& hostile : hostiles) if (hostile.guid == guid) return &hostile;
        return nullptr;
    }
};
}
#endif
