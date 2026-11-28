#ifndef MANGOS_PARTYBOT_PRIEST_HEALING_POLICY_H
#define MANGOS_PARTYBOT_PRIEST_HEALING_POLICY_H

#include "HealerPolicy.h"

namespace PartyBotEngine
{
// Compatibility names preserve existing Priest action/tests while the actual
// triage, deficit, reservation and rank-selection primitives are class-neutral.
using PriestDirectHealSelection = DirectHealSpellSelection;
using PriestHealingTargetSelection = HealerTargetSelection;
using PriestHealingDecisionSelection = HealerDecisionSelection;

constexpr std::uint8_t PRIEST_HEALER_CLASS_ID = 5;

inline std::uint32_t GetPriestUncoveredMissingHealth(
    PartySnapshot const& snapshot,
    PartyMemberSnapshot const& target,
    Guid healerGuid,
    PartyCoordinator* coordinator,
    std::uint32_t nowMs)
{
    return GetUncoveredMissingHealth(
        snapshot, target, healerGuid, coordinator, nowMs);
}

inline bool SelectPriestHealingTargetWithReservations(
    PartySnapshot const& snapshot, Guid healerGuid,
    PartyCoordinator* coordinator, std::uint32_t nowMs,
    PriestHealingTargetSelection& selection)
{
    selection = PriestHealingTargetSelection();
    PartyMemberSnapshot const* healer = snapshot.FindMember(healerGuid);
    if (!healer || healer->classId != PRIEST_HEALER_CLASS_ID)
        return false;
    return SelectHealerTargetWithReservations(
        snapshot, healerGuid, coordinator, nowMs, selection);
}

inline bool SelectPriestHealingTarget(
    PartySnapshot const& snapshot, Guid healerGuid,
    PriestHealingTargetSelection& selection)
{
    return SelectPriestHealingTargetWithReservations(
        snapshot, healerGuid, nullptr, 0, selection);
}

inline bool SelectPriestHealingTarget(
    PartySnapshot const& snapshot, Guid healerGuid,
    PartyCoordinator& coordinator, std::uint32_t nowMs,
    PriestHealingTargetSelection& selection)
{
    return SelectPriestHealingTargetWithReservations(
        snapshot, healerGuid, &coordinator, nowMs, selection);
}

inline bool SelectPriestDirectHealSpell(
    PartyMemberSnapshot const& healer,
    std::uint32_t missingHealth,
    PriestDirectHealSelection& selection)
{
    return SelectDirectHealSpell(healer, missingHealth, selection);
}

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

inline bool SelectPriestHealingDecision(
    PartySnapshot const& snapshot, Guid healerGuid,
    PartyCoordinator& coordinator, std::uint32_t nowMs,
    PriestHealingDecisionSelection& decision)
{
    decision = PriestHealingDecisionSelection();
    if (!SelectPriestHealingTarget(
            snapshot, healerGuid, coordinator, nowMs, decision.target))
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
} // namespace PartyBotEngine

#endif
