#ifndef MANGOS_PARTYBOT_HEALER_POLICY_H
#define MANGOS_PARTYBOT_HEALER_POLICY_H

#include "PartyCoordinator.h"
#include <algorithm>

namespace PartyBotEngine
{
struct DirectHealSpellSelection
{
    std::uint32_t spellId = 0;
    std::uint32_t baseHealing = 0;
    std::uint32_t manaCost = 0;

    bool IsValid() const { return spellId != 0 && baseHealing != 0; }
};

struct HealerTargetSelection
{
    Guid targetGuid = 0;
    std::uint32_t missingHealth = 0;
    std::uint32_t deficitBasisPoints = 0;

    bool IsValid() const { return targetGuid != 0 && missingHealth != 0; }
};

struct HealerDecisionSelection
{
    HealerTargetSelection target;
    DirectHealSpellSelection spell;

    bool IsValid() const { return target.IsValid() && spell.IsValid(); }
};

// Return only the health deficit not already covered by supported incoming
// heals/absorbs/current casts or another Bot's live Heal reservation.
// This function intentionally has no class gate: spell support and class
// eligibility belong to the adapter, while deficit accounting is shared.
inline std::uint32_t GetUncoveredMissingHealth(
    PartySnapshot const& snapshot,
    PartyMemberSnapshot const& target,
    Guid healerGuid,
    PartyCoordinator* coordinator,
    std::uint32_t nowMs)
{
    if (!target.guid || !target.online || !target.alive ||
        !target.maxHealth || target.health >= target.maxHealth ||
        target.mapId != snapshot.mapId ||
        target.instanceId != snapshot.instanceId)
        return 0;

    std::uint32_t const rawMissing = target.maxHealth - target.health;
    std::uint64_t incoming = target.periodicHealRemainingAmount;
    incoming += target.allSchoolAbsorbRemainingAmount;
    for (PartyMemberSnapshot const& caster : snapshot.members)
    {
        if (caster.guid && caster.online && caster.alive &&
            caster.mapId == snapshot.mapId &&
            caster.instanceId == snapshot.instanceId &&
            !caster.currentSpellEngineAction &&
            caster.currentSpellTargetGuid == target.guid)
            incoming += caster.currentSpellHealingAmount;
    }

    std::uint64_t const reserved = coordinator
        ? coordinator->GetReservedAmount(
            ReservationType::Heal, target.guid, healerGuid, nowMs)
        : 0u;
    std::uint64_t const credited = std::min<std::uint64_t>(
        rawMissing, incoming + reserved);
    return rawMissing - static_cast<std::uint32_t>(credited);
}

// Select the most urgent same-shard party member using effective health
// (current health plus supported incoming credit). This is class-neutral;
// the adapter must validate that the acting class is allowed to heal.
inline bool SelectHealerTargetWithReservations(
    PartySnapshot const& snapshot, Guid healerGuid,
    PartyCoordinator* coordinator, std::uint32_t nowMs,
    HealerTargetSelection& selection)
{
    selection = HealerTargetSelection();
    if (!snapshot.groupId || !healerGuid || snapshot.members.empty())
        return false;
    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer ||
        healer->roleId != static_cast<std::uint8_t>(PartyRole::Healer) ||
        !healer->isPlayerBot || !healer->online || !healer->alive ||
        !healer->maxHealth || !healer->health ||
        healer->health > healer->maxHealth ||
        healer->mapId != snapshot.mapId ||
        healer->instanceId != snapshot.instanceId)
        return false;

    for (std::size_t i = 0; i < snapshot.members.size(); ++i)
    {
        if (!snapshot.members[i].guid)
            return false;
        for (std::size_t j = i + 1; j < snapshot.members.size(); ++j)
            if (snapshot.members[i].guid == snapshot.members[j].guid)
                return false;
    }

    PartyMemberSnapshot const* best = nullptr;
    std::uint32_t bestRemainingMissing = 0;
    std::uint32_t bestEffectiveHealth = 0;
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (!member.guid || !member.online || !member.alive ||
            !member.maxHealth || !member.health ||
            member.health >= member.maxHealth ||
            member.mapId != snapshot.mapId ||
            member.instanceId != snapshot.instanceId)
            continue;

        std::uint32_t const rawMissing = member.maxHealth - member.health;
        std::uint32_t const remaining = GetUncoveredMissingHealth(
            snapshot, member, healerGuid, coordinator, nowMs);
        std::uint32_t const credited = rawMissing - remaining;
        if (!remaining)
            continue;
        std::uint32_t const effectiveHealth = member.health + credited;

        if (!best)
        {
            best = &member;
            bestRemainingMissing = remaining;
            bestEffectiveHealth = effectiveHealth;
            continue;
        }
        std::uint64_t const candidateCross =
            static_cast<std::uint64_t>(effectiveHealth) * best->maxHealth;
        std::uint64_t const bestCross =
            static_cast<std::uint64_t>(bestEffectiveHealth) * member.maxHealth;
        bool prefer = candidateCross < bestCross;
        if (candidateCross == bestCross)
        {
            bool const candidateTank = member.isMainTank ||
                member.roleId == static_cast<std::uint8_t>(PartyRole::Tank) ||
                member.roleId == static_cast<std::uint8_t>(PartyRole::MainTank);
            bool const bestTank = best->isMainTank ||
                best->roleId == static_cast<std::uint8_t>(PartyRole::Tank) ||
                best->roleId == static_cast<std::uint8_t>(PartyRole::MainTank);
            prefer = (candidateTank != bestTank)
                ? candidateTank
                : (remaining != bestRemainingMissing)
                    ? remaining > bestRemainingMissing
                    : member.guid < best->guid;
        }
        if (prefer)
        {
            best = &member;
            bestRemainingMissing = remaining;
            bestEffectiveHealth = effectiveHealth;
        }
    }

    if (!best)
        return false;
    selection.targetGuid = best->guid;
    selection.missingHealth = bestRemainingMissing;
    selection.deficitBasisPoints = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(bestRemainingMissing) * 10000u) /
        best->maxHealth);
    return selection.IsValid();
}

inline bool SelectHealerTarget(
    PartySnapshot const& snapshot, Guid healerGuid,
    HealerTargetSelection& selection)
{
    return SelectHealerTargetWithReservations(
        snapshot, healerGuid, nullptr, 0, selection);
}

inline bool SelectHealerTarget(
    PartySnapshot const& snapshot, Guid healerGuid,
    PartyCoordinator& coordinator, std::uint32_t nowMs,
    HealerTargetSelection& selection)
{
    return SelectHealerTargetWithReservations(
        snapshot, healerGuid, &coordinator, nowMs, selection);
}

// Shared deterministic rank selector. It chooses the smallest affordable
// direct heal that covers the deficit, or the strongest affordable under-heal.
// Castability, target/range/LOS, modifiers and final cost remain Core checks.
inline bool SelectDirectHealSpell(
    PartyMemberSnapshot const& healer,
    std::uint32_t missingHealth,
    DirectHealSpellSelection& selection)
{
    selection = DirectHealSpellSelection();

    if (!healer.isPlayerBot || !healer.online || !healer.alive ||
        healer.roleId != static_cast<std::uint8_t>(PartyRole::Healer) ||
        !missingHealth || healer.directHealSpells.empty())
        return false;

    // Snapshot construction guarantees unique learned spell IDs. Treat a
    // duplicate as corrupted input rather than making an arbitrary choice.
    for (std::size_t i = 0; i < healer.directHealSpells.size(); ++i)
    {
        std::uint32_t const spellId = healer.directHealSpells[i].spellId;
        if (!spellId)
            continue;
        for (std::size_t j = i + 1; j < healer.directHealSpells.size(); ++j)
            if (healer.directHealSpells[j].spellId == spellId)
                return false;
    }

    DirectHealSpellSnapshot const* bestCover = nullptr;
    DirectHealSpellSnapshot const* bestUnder = nullptr;
    for (DirectHealSpellSnapshot const& spell : healer.directHealSpells)
    {
        if (!spell.spellId || !spell.baseHealing || spell.manaCost > healer.power)
            continue;

        if (spell.baseHealing >= missingHealth)
        {
            if (!bestCover ||
                spell.baseHealing < bestCover->baseHealing ||
                (spell.baseHealing == bestCover->baseHealing &&
                 (spell.manaCost < bestCover->manaCost ||
                  (spell.manaCost == bestCover->manaCost &&
                   spell.spellId < bestCover->spellId))))
                bestCover = &spell;
        }
        else if (!bestUnder ||
                 spell.baseHealing > bestUnder->baseHealing ||
                 (spell.baseHealing == bestUnder->baseHealing &&
                  (spell.manaCost < bestUnder->manaCost ||
                   (spell.manaCost == bestUnder->manaCost &&
                    spell.spellId < bestUnder->spellId))))
            bestUnder = &spell;
    }

    DirectHealSpellSnapshot const* selected = bestCover ? bestCover : bestUnder;
    if (!selected)
        return false;

    selection.spellId = selected->spellId;
    selection.baseHealing = selected->baseHealing;
    selection.manaCost = selected->manaCost;
    return selection.IsValid();
}

inline bool SelectHealerDecision(
    PartySnapshot const& snapshot, Guid healerGuid,
    HealerDecisionSelection& decision)
{
    decision = HealerDecisionSelection();
    if (!SelectHealerTarget(snapshot, healerGuid, decision.target))
        return false;
    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer || !SelectDirectHealSpell(
            *healer, decision.target.missingHealth, decision.spell))
    {
        decision = HealerDecisionSelection();
        return false;
    }
    return decision.IsValid();
}

inline bool SelectHealerDecision(
    PartySnapshot const& snapshot, Guid healerGuid,
    PartyCoordinator& coordinator, std::uint32_t nowMs,
    HealerDecisionSelection& decision)
{
    decision = HealerDecisionSelection();
    if (!SelectHealerTarget(
            snapshot, healerGuid, coordinator, nowMs, decision.target))
        return false;
    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer || !SelectDirectHealSpell(
            *healer, decision.target.missingHealth, decision.spell))
    {
        decision = HealerDecisionSelection();
        return false;
    }
    return decision.IsValid();
}
} // namespace PartyBotEngine

#endif
