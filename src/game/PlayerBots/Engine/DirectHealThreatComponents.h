#ifndef MANGOS_PARTYBOT_DIRECT_HEAL_THREAT_COMPONENTS_H
#define MANGOS_PARTYBOT_DIRECT_HEAL_THREAT_COMPONENTS_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace PartyBotEngine
{
// Proc callbacks may run for either side of a direct heal. The Core caller
// supplies the side-specific helpful-spell proc flag; scripted Aura callbacks
// and audited hard-coded proc cases remain unsafe even without DB proc flags.
// The first production healing slice deliberately accepts exactly one unit
// selector: explicit friendly/party/raid/unit, or self-only CASTER. Chain-heal,
// near-caster, party/AoE, script and location target modes remain outside scope.
inline bool IsSupportedDirectHealTargetMode(
    std::uint32_t targetMode,
    std::uint32_t targetUnitFriend,
    std::uint32_t targetUnitParty,
    std::uint32_t targetUnitRaid,
    std::uint32_t targetUnit,
    std::uint32_t targetUnitCaster)
{
    return targetMode == targetUnitFriend ||
        targetMode == targetUnitParty ||
        targetMode == targetUnitRaid ||
        targetMode == targetUnit ||
        targetMode == targetUnitCaster;
}

inline bool IsSupportedDirectHealTargetRelation(
    bool hasCasterTargetSelector, bool targetIsCaster)
{
    return !hasCasterTargetSelector || targetIsCaster;
}

inline bool IsSupportedSingleTargetDirectHealEffectShape(
    std::uint32_t activeEffectCount,
    bool allActiveEffectsAreDirectHeals,
    bool targetAIsNone,
    bool targetBIsNone,
    bool targetAIsSupportedFriendlyUnit,
    bool targetBIsSupportedFriendlyUnit)
{
    if (activeEffectCount != 1 || !allActiveEffectsAreDirectHeals)
        return false;

    // Exactly one side selects the one explicit unit target. Reject two
    // selectors, zero selectors, and target modes that expand implicitly.
    return (targetAIsNone && targetBIsSupportedFriendlyUnit && !targetBIsNone) ||
        (targetBIsNone && targetAIsSupportedFriendlyUnit && !targetAIsNone);
}

inline bool HasPotentialHelpfulSpellProc(
    bool hasAuraScript, bool hasHardcodedProc,
    std::uint32_t configuredProcFlags, std::uint32_t sideHelpfulProcFlag)
{
    return hasAuraScript || hasHardcodedProc ||
        (configuredProcFlags & sideHelpfulProcFlag) != 0;
}

// SpellScript callbacks can create secondary spells in OnCast, OnHit,
// OnAfterHit or OnSuccessfulFinish. Until those script-specific effects are
// audited, any spell with a configured ScriptId is outside the estimator's
// closed direct-heal threat set.
inline bool HasUnboundedDirectHealSpellScript(bool hasConfiguredScript)
{
    return hasConfiguredScript;
}

// The pinned Core's TriggerSpell and TriggerMissile handlers create nested
// casts. Dummy and ScriptEffect handlers can also dispatch arbitrary spells
// through hard-coded cases or ScriptMgr callbacks. Until their child spell,
// caster and recipient closure is audited, any such effect makes the direct
// heal's secondary threat recipients unbounded.
inline bool HasUnboundedDirectHealSecondarySpellEffect(
    bool triggerSpell, bool triggerMissile,
    bool dummyEffect, bool scriptEffect)
{
    return triggerSpell || triggerMissile || dummyEffect || scriptEffect;
}

// Spell::HandleAddTargetTriggerAuras scans active caster auras by their
// individual effect index, not only effect zero. A matching effect can cast a
// secondary spell against each successful target (or the caster on reflect).
inline bool HasMatchingAddTargetTriggerAuraEffect(
    bool hasActiveAuraEffect, bool isAddTargetTrigger,
    bool affectedOnHealingSpell)
{
    return hasActiveAuraEffect && isAddTargetTrigger &&
        affectedOnHealingSpell;
}

// Raw per-reference inputs for the two independent helpful-threat paths in
// Vanilla Core. The assist path applies the class coefficient and spell threat
// multiplier before HostileRefManager::threatAssist fans it out. The positive
// spell_threat row is fanned out separately; ThreatCalcHelper subsequently
// applies the actor's Core threat modifiers to both amounts.
//
// This helper only performs the arithmetic split. It does not prove that the
// cast's target set, active proc closure or secondary threat recipients are
// complete, and must never be used by itself to promote an action to Complete.
struct DirectHealThreatComponents
{
    float assistThreatPerReference = 0.0f;
    float flatThreatPerReference = 0.0f;
};

inline bool CalculateDirectHealThreatComponents(
    float maxEffectiveHealing,
    float classThreatModifier,
    float spellThreatMultiplier,
    float positiveFlatSpellThreat,
    std::size_t hostileReferenceCount,
    DirectHealThreatComponents& components)
{
    components = DirectHealThreatComponents();

    if (!hostileReferenceCount ||
        !std::isfinite(maxEffectiveHealing) || maxEffectiveHealing < 0.0f ||
        !std::isfinite(classThreatModifier) || classThreatModifier < 0.0f ||
        classThreatModifier > 1.0f ||
        !std::isfinite(spellThreatMultiplier) || spellThreatMultiplier < 0.0f ||
        !std::isfinite(positiveFlatSpellThreat) || positiveFlatSpellThreat < 0.0f)
        return false;

    double const divisor = static_cast<double>(hostileReferenceCount);
    double const assist = static_cast<double>(maxEffectiveHealing) *
        static_cast<double>(classThreatModifier) *
        static_cast<double>(spellThreatMultiplier) / divisor;
    double const flat = static_cast<double>(positiveFlatSpellThreat) / divisor;
    double const maxFloat = std::numeric_limits<float>::max();

    if (!std::isfinite(assist) || assist < 0.0 || assist > maxFloat ||
        !std::isfinite(flat) || flat < 0.0 || flat > maxFloat)
        return false;

    components.assistThreatPerReference = static_cast<float>(assist);
    components.flatThreatPerReference = static_cast<float>(flat);
    return std::isfinite(components.assistThreatPerReference) &&
        std::isfinite(components.flatThreatPerReference);
}
} // namespace PartyBotEngine

#endif
