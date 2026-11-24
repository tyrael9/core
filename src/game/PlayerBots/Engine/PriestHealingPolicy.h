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
