#ifndef MANGOS_PARTYBOT_DIRECT_HEAL_IMPACT_CONTRACT_H
#define MANGOS_PARTYBOT_DIRECT_HEAL_IMPACT_CONTRACT_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace PartyBotEngine
{

// Value-only view of one Core Spell::TargetInfo row. Keeping this contract
// independent of Core types lets the pure Engine test target exercise every
// accepted/rejected resolved-target shape without manufacturing Core objects.
struct ResolvedDirectHealTarget
{
    std::uint64_t targetGuid = 0;
    std::uint64_t timeDelayMs = 0;
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
    std::uint64_t expectedDelayMs,
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


struct ResolvedDirectHealImpactContext
{
    std::uint64_t expectedCasterGuid = 0;
    std::uint64_t currentCasterGuid = 0;
    std::uint64_t expectedRecipientGuid = 0;
    std::uint64_t currentRecipientGuid = 0;

    std::uint32_t expectedGroupId = 0;
    std::uint32_t casterGroupId = 0;
    std::uint32_t recipientGroupId = 0;

    std::uint32_t expectedMapId = 0;
    std::uint32_t casterMapId = 0;
    std::uint32_t recipientMapId = 0;
    std::uint32_t expectedInstanceId = 0;
    std::uint32_t casterInstanceId = 0;
    std::uint32_t recipientInstanceId = 0;

    bool casterAndRecipientShareGroup = false;
    bool casterIsGroupMember = false;
    bool recipientIsGroupMember = false;
    bool casterAndRecipientShareMap = false;
    bool casterInWorld = false;
    bool recipientInWorld = false;
    bool casterAlive = false;
    bool recipientAlive = false;
};

// Validate the impact-time identity of a party heal against the context
// captured when the Engine requested deferred effects. This is necessary but
// not sufficient for authorization: spell shape, successful target rows,
// proc/child closure and complete threat impact remain separate gates.
inline bool IsCurrentLiveGroupDirectHealImpactContext(
    ResolvedDirectHealImpactContext const& context)
{
    return context.expectedCasterGuid &&
        context.expectedRecipientGuid &&
        context.expectedGroupId &&
        context.currentCasterGuid == context.expectedCasterGuid &&
        context.currentRecipientGuid == context.expectedRecipientGuid &&
        context.casterGroupId == context.expectedGroupId &&
        context.recipientGroupId == context.expectedGroupId &&
        context.casterAndRecipientShareGroup &&
        context.casterIsGroupMember &&
        context.recipientIsGroupMember &&
        context.casterMapId == context.expectedMapId &&
        context.recipientMapId == context.expectedMapId &&
        context.casterInstanceId == context.expectedInstanceId &&
        context.recipientInstanceId == context.expectedInstanceId &&
        context.casterAndRecipientShareMap &&
        context.casterInWorld &&
        context.recipientInWorld &&
        context.casterAlive &&
        context.recipientAlive;
}

 
// The direct-heal threat envelope is tied to the exact live hostile references
// observed for its recipient. GUIDs are canonical only when sorted, nonzero
// and unique; an empty set is a valid identity snapshot, not proof of NoThreat.
inline bool IsCanonicalDirectHealThreatRecipientSet(
    std::vector<std::uint64_t> const& hostileGuids)
{
    for (std::size_t index = 0; index < hostileGuids.size(); ++index)
    {
        if (!hostileGuids[index] ||
            (index && hostileGuids[index - 1] >= hostileGuids[index]))
            return false;
    }
    return true;
}

inline bool IsExactDirectHealThreatRecipientSet(
    std::vector<std::uint64_t> const& expected,
    std::vector<std::uint64_t> const& current)
{
    return IsCanonicalDirectHealThreatRecipientSet(expected) &&
        IsCanonicalDirectHealThreatRecipientSet(current) &&
        expected == current;
}

} // namespace PartyBotEngine

#endif
