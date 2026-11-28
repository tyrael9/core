#ifndef MANGOS_PARTYBOT_SHAMAN_HEALING_POLICY_H
#define MANGOS_PARTYBOT_SHAMAN_HEALING_POLICY_H

#include "HealerPolicy.h"

namespace PartyBotEngine
{
constexpr std::uint8_t SHAMAN_HEALER_CLASS_ID = 7;

// Initial Shaman adapter scope is deliberately the six Lesser Healing Wave
// ranks only. Chain Heal is a separate fan-out capability; generic Heal-effect
// presence is not sufficient to make it an eligible one-target spell.
inline bool IsShamanLesserHealingWaveRank(std::uint32_t spellId)
{
    switch (spellId)
    {
        case 8004:  // Rank 1
        case 8008:  // Rank 2
        case 8010:  // Rank 3
        case 10469: // Rank 4
        case 10470: // Rank 5
        case 10471: // Rank 6
            return true;
        default:
            return false;
    }
}

inline bool SelectShamanLesserHealingWave(
    PartyMemberSnapshot const& healer,
    std::uint32_t missingHealth,
    DirectHealSpellSelection& selection)
{
    selection = DirectHealSpellSelection();
    if (healer.classId != SHAMAN_HEALER_CLASS_ID ||
        healer.roleId != static_cast<std::uint8_t>(PartyRole::Healer))
        return false;

    return SelectDirectHealSpellMatching(
        healer, missingHealth, selection,
        [](std::uint32_t spellId)
        {
            return IsShamanLesserHealingWaveRank(spellId);
        });
}

inline bool SelectShamanHealingDecision(
    PartySnapshot const& snapshot,
    Guid healerGuid,
    HealerDecisionSelection& decision)
{
    decision = HealerDecisionSelection();
    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer || healer->classId != SHAMAN_HEALER_CLASS_ID ||
        healer->roleId != static_cast<std::uint8_t>(PartyRole::Healer))
        return false;

    if (!SelectHealerTarget(snapshot, healerGuid, decision.target) ||
        !SelectShamanLesserHealingWave(
            *healer, decision.target.missingHealth, decision.spell))
    {
        decision = HealerDecisionSelection();
        return false;
    }
    return decision.IsValid();
}

} // namespace PartyBotEngine

#endif
