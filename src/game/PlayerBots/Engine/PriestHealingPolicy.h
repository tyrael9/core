#ifndef MANGOS_PARTYBOT_PRIEST_HEALING_POLICY_H
#define MANGOS_PARTYBOT_PRIEST_HEALING_POLICY_H

#include "PartySnapshot.h"

namespace PartyBotEngine
{
struct PriestDirectHealSelection
{
    std::uint32_t spellId = 0;
    std::uint32_t baseHealing = 0;
    std::uint32_t manaCost = 0;

    bool IsValid() const { return spellId != 0 && baseHealing != 0; }
};

struct PriestHealingTargetSelection
{
    Guid targetGuid = 0;
    std::uint32_t missingHealth = 0;
    std::uint32_t deficitBasisPoints = 0;

    bool IsValid() const { return targetGuid != 0 && missingHealth != 0; }
};

struct PriestHealingDecisionSelection
{
    PriestHealingTargetSelection target;
    PriestDirectHealSelection spell;

    bool IsValid() const { return target.IsValid() && spell.IsValid(); }
};

inline bool SelectPriestDirectHealSpell(
    PartyMemberSnapshot const& healer, std::uint32_t missingHealth,
    PriestDirectHealSelection& selection);

// F54a: the Vanilla Priest class ID is 5. Select a same-shard Player from the
// immutable party roster. Lowest health fraction wins; exact percentage ties
// prefer Main Tank, then larger absolute deficit, then lower GUID. Integer
// cross multiplication avoids floating-point ordering drift. Incoming heals,
// HoTs, absorbs and effective healing are not predicted by this baseline.
inline bool SelectPriestHealingTarget(
    PartySnapshot const& snapshot, Guid healerGuid,
    PriestHealingTargetSelection& selection)
{
    selection = PriestHealingTargetSelection();
    if (!snapshot.groupId || !healerGuid || snapshot.members.empty())
        return false;

    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer || healer->classId != 5 ||
        healer->roleId != static_cast<std::uint8_t>(PartyRole::Healer) ||
        !healer->isPlayerBot || !healer->online || !healer->alive ||
        !healer->maxHealth || !healer->health ||
        healer->health > healer->maxHealth ||
        healer->mapId != snapshot.mapId ||
        healer->instanceId != snapshot.instanceId)
        return false;

    // A duplicated GUID makes FindMember and target identity ambiguous.
    for (std::size_t i = 0; i < snapshot.members.size(); ++i)
    {
        if (!snapshot.members[i].guid)
            return false;
        for (std::size_t j = i + 1; j < snapshot.members.size(); ++j)
            if (snapshot.members[i].guid == snapshot.members[j].guid)
                return false;
    }

    PartyMemberSnapshot const* best = nullptr;
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (!member.guid || !member.online || !member.alive ||
            !member.maxHealth || !member.health ||
            member.health >= member.maxHealth ||
            member.mapId != snapshot.mapId ||
            member.instanceId != snapshot.instanceId)
            continue;

        if (!best)
        {
            best = &member;
            continue;
        }

        // Compare health fractions exactly without float precision drift.
        std::uint64_t const candidateCross =
            static_cast<std::uint64_t>(member.health) * best->maxHealth;
        std::uint64_t const bestCross =
            static_cast<std::uint64_t>(best->health) * member.maxHealth;
        bool prefer = candidateCross < bestCross;
        if (candidateCross == bestCross)
        {
            bool const candidateTank = member.isMainTank ||
                member.roleId == static_cast<std::uint8_t>(PartyRole::Tank) ||
                member.roleId == static_cast<std::uint8_t>(PartyRole::MainTank);
            bool const bestTank = best->isMainTank ||
                best->roleId == static_cast<std::uint8_t>(PartyRole::Tank) ||
                best->roleId == static_cast<std::uint8_t>(PartyRole::MainTank);
            std::uint32_t const candidateMissing = member.maxHealth - member.health;
            std::uint32_t const bestMissing = best->maxHealth - best->health;
            prefer = (candidateTank != bestTank)
                ? candidateTank
                : (candidateMissing != bestMissing)
                    ? candidateMissing > bestMissing
                    : member.guid < best->guid;
        }
        if (prefer)
            best = &member;
    }

    if (!best)
        return false;

    selection.targetGuid = best->guid;
    selection.missingHealth = best->maxHealth - best->health;
    selection.deficitBasisPoints = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(selection.missingHealth) * 10000u) /
        best->maxHealth);
    return selection.IsValid();
}

// Compose target triage with the learned-rank/mana selector. Failure clears the
// whole result so callers cannot retain a target-only partial heal decision.
inline bool SelectPriestHealingDecision(
    PartySnapshot const& snapshot, Guid healerGuid,
    PriestHealingDecisionSelection& decision)
{
    decision = PriestHealingDecisionSelection();
    if (!SelectPriestHealingTarget(snapshot, healerGuid, decision.target))
        return false;
    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer || !SelectPriestDirectHealSpell(
            *healer, decision.target.missingHealth, decision.spell))
    {
        decision = PriestHealingDecisionSelection();
        return false;
    }
    return decision.IsValid();
}

// Select from the exact direct-heal ranks copied into the current snapshot.
// Prefer the smallest affordable base heal that covers the deficit; when no
// affordable rank covers it, use the strongest affordable rank. This is only
// a deterministic rank/triage primitive: Core still decides live castability,
// bonus healing, range, line of sight, spell effects and final mana spending.
inline bool SelectPriestDirectHealSpell(
    PartyMemberSnapshot const& healer,
    std::uint32_t missingHealth,
    PriestDirectHealSelection& selection)
{
    selection = PriestDirectHealSelection();

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
} // namespace PartyBotEngine

#endif
