#ifndef MANGOS_PARTYBOT_DIRECT_HEAL_IMPACT_CONTRACT_H
#define MANGOS_PARTYBOT_DIRECT_HEAL_IMPACT_CONTRACT_H

#include <cstdint>

namespace PartyBotEngine
{

// Value-only view of one Core Spell::TargetInfo row. Keeping this contract
// independent of Core types lets the pure Engine test target exercise every
// accepted/rejected resolved-target shape without manufacturing Core objects.
struct ResolvedDirectHealTarget
{
    std::uint64_t targetGuid = 0;
    std::uint32_t timeDelayMs = 0;
    std::uint8_t effectMask = 0;
    std::uint8_t missCondition = 0;
    std::uint8_t reflectResult = 0;
    bool deleted = false;
    bool processed = false;
};

// A direct-heal admission gate may consume this only when Core has enumerated
// exactly the expected single Unit recipient and no GameObject/Item fan-out.
// This deliberately does not prove Group membership, map identity, proc/child
// closure or threat completeness; those remain separate Core-owned gates.
inline bool IsExactSingleResolvedDirectHealTarget(
    std::uint64_t expectedTargetGuid,
    std::uint32_t resolvedUnitTargetCount,
    std::uint32_t resolvedGameObjectTargetCount,
    std::uint32_t resolvedItemTargetCount,
    ResolvedDirectHealTarget const& target,
    std::uint32_t expectedDelayMs,
    std::uint8_t expectedEffectMask,
    std::uint8_t noMissCondition,
    std::uint8_t noReflectResult)
{
    return expectedTargetGuid &&
        resolvedUnitTargetCount == 1 &&
        resolvedGameObjectTargetCount == 0 &&
        resolvedItemTargetCount == 0 &&
        target.targetGuid == expectedTargetGuid &&
        target.timeDelayMs == expectedDelayMs &&
        target.effectMask == expectedEffectMask &&
        target.missCondition == noMissCondition &&
        target.reflectResult == noReflectResult &&
        !target.deleted && !target.processed;
}

} // namespace PartyBotEngine

#endif
