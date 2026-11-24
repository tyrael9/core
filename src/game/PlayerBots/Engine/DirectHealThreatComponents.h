#ifndef MANGOS_PARTYBOT_DIRECT_HEAL_THREAT_COMPONENTS_H
#define MANGOS_PARTYBOT_DIRECT_HEAL_THREAT_COMPONENTS_H

#include <cmath>
#include <cstddef>
#include <limits>

namespace PartyBotEngine
{
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
