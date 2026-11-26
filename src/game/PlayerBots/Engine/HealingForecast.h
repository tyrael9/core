#ifndef MANGOS_PARTYBOT_HEALING_FORECAST_H
#define MANGOS_PARTYBOT_HEALING_FORECAST_H

#include "PartySnapshot.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace PartyBotEngine
{
constexpr std::size_t MAX_HEALING_TIMELINE_EVENTS = 256;

// Core rand_ditheru(x) returns floor(x) or ceil(x), selected by the fractional
// part. A detached snapshot cannot know the random draw, so preserve both
// reachable bounds. Core returns int32 before casting to uint32; reject values
// outside that representable range rather than inventing a forecast.
inline bool GetCoreRandomDitherBounds(
    float amount, std::uint32_t& minimum, std::uint32_t& maximum)
{
    minimum = 0;
    maximum = 0;
    if (!std::isfinite(amount) || amount <= 0.0f)
        return false;
    double const value = static_cast<double>(amount);
    double const lower = std::floor(value);
    double const upper = std::ceil(value);
    double const coreMaximum = static_cast<double>(
        std::numeric_limits<std::int32_t>::max());
    if (lower < 0.0 || upper > coreMaximum)
        return false;
    minimum = static_cast<std::uint32_t>(lower);
    maximum = static_cast<std::uint32_t>(upper);
    return maximum >= minimum && maximum > 0;
}

enum class HealingTimelineEventKind : std::uint8_t
{
    Heal,
    Damage
};

struct HealingTimelineEvent
{
    std::uint32_t arrivalMs = 0;
    // Minimum amount; for random-dither HoTs this is floor(Core's input).
    std::uint32_t amount = 0;
    // Maximum amount; zero preserves exact-amount semantics for older fixtures.
    std::uint32_t maximumAmount = 0;
    Guid sourceGuid = 0;
    std::uint32_t spellId = 0;
    HealingTimelineEventKind kind = HealingTimelineEventKind::Heal;
};

struct HealingTimelineProjection
{
    // Existing fields describe the conservative lower-bound projection.
    std::uint32_t finalHealth = 0;
    std::uint32_t maximumFinalHealth = 0;
    std::size_t eventCount = 0;
    std::size_t simultaneousArrivalGroups = 0;
    std::uint64_t effectiveHealing = 0;
    std::uint64_t maximumEffectiveHealing = 0;
    std::uint64_t overheal = 0;
    std::uint64_t maximumOverheal = 0;
    std::uint64_t appliedDamage = 0;
    std::uint64_t maximumAppliedDamage = 0;
    bool concurrentAttributionUnknown = false;
    bool valid = false;
};

inline bool AppendPeriodicHealForecastEvents(
    PeriodicHealForecastSnapshot const& forecast,
    std::vector<HealingTimelineEvent>& events)
{
    std::uint32_t const maximumPerTick =
        forecast.healingPerTickUpperBound
            ? forecast.healingPerTickUpperBound : forecast.healingPerTick;
    if (!forecast.casterGuid || !forecast.spellId || !forecast.periodMs ||
        !forecast.remainingTicks || !maximumPerTick ||
        maximumPerTick < forecast.healingPerTick ||
        events.size() > MAX_HEALING_TIMELINE_EVENTS ||
        forecast.remainingTicks > MAX_HEALING_TIMELINE_EVENTS - events.size())
        return false;

    std::uint64_t const lastArrival =
        static_cast<std::uint64_t>(forecast.firstTickMs) +
        static_cast<std::uint64_t>(forecast.remainingTicks - 1u) *
            forecast.periodMs;
    if (lastArrival > std::numeric_limits<std::uint32_t>::max())
        return false;

    for (std::uint32_t i = 0; i < forecast.remainingTicks; ++i)
    {
        HealingTimelineEvent event;
        event.arrivalMs = forecast.firstTickMs + i * forecast.periodMs;
        event.amount = forecast.healingPerTick;
        event.maximumAmount = maximumPerTick;
        event.sourceGuid = forecast.casterGuid;
        event.spellId = forecast.spellId;
        events.push_back(event);
    }
    return true;
}

inline bool AppendDirectHealForecastEvent(
    Guid sourceGuid, std::uint32_t spellId, std::uint32_t arrivalMs,
    std::uint32_t amount, std::vector<HealingTimelineEvent>& events)
{
    if (!sourceGuid || !spellId || !amount ||
        events.size() >= MAX_HEALING_TIMELINE_EVENTS)
        return false;

    HealingTimelineEvent event;
    event.arrivalMs = arrivalMs;
    event.amount = amount;
    event.maximumAmount = amount;
    event.sourceGuid = sourceGuid;
    event.spellId = spellId;
    events.push_back(event);
    return true;
}

inline bool AppendDamageTimelineEvent(
    Guid sourceGuid, std::uint32_t spellId, std::uint32_t arrivalMs,
    std::uint32_t amount, std::vector<HealingTimelineEvent>& events)
{
    if (!sourceGuid || !amount ||
        events.size() >= MAX_HEALING_TIMELINE_EVENTS)
        return false;

    HealingTimelineEvent event;
    event.arrivalMs = arrivalMs;
    event.amount = amount;
    event.maximumAmount = amount;
    event.sourceGuid = sourceGuid;
    event.spellId = spellId;
    event.kind = HealingTimelineEventKind::Damage;
    events.push_back(event);
    return true;
}

inline bool AppendInFlightDirectHealForecastEvent(
    PartyMemberSnapshot const& caster, Guid targetGuid,
    std::vector<HealingTimelineEvent>& events)
{
    if (!caster.guid || !targetGuid || caster.currentSpellEngineAction ||
        !caster.currentSpellId || !caster.currentSpellState ||
        caster.currentSpellTargetGuid != targetGuid ||
        !caster.currentSpellHealingAmount ||
        !caster.currentSpellRemainingCastMs)
        return false;

    return AppendDirectHealForecastEvent(
        caster.guid, caster.currentSpellId,
        caster.currentSpellRemainingCastMs,
        caster.currentSpellHealingAmount, events);
}

// Project explicitly supplied heal/damage arrivals against a fixed health
// ceiling. Existing fields report the conservative lower-bound result;
// companion fields report the reachable upper-bound result. Mixed heal/damage
// events at one timestamp are rejected because the snapshot does not prove
// Core's application order. Per-caster attribution is not guessed.
inline bool ProjectHealingTimeline(
    std::uint32_t currentHealth, std::uint32_t maxHealth,
    std::vector<HealingTimelineEvent> const& input,
    HealingTimelineProjection& projection)
{
    projection = HealingTimelineProjection();
    if (!maxHealth || currentHealth > maxHealth ||
        input.size() > MAX_HEALING_TIMELINE_EVENTS)
        return false;
    std::vector<HealingTimelineEvent> events = input;
    for (HealingTimelineEvent const& event : events)
    {
        if (!event.sourceGuid)
            return false;
        std::uint32_t const maximumAmount =
            event.maximumAmount ? event.maximumAmount : event.amount;
        if (event.kind == HealingTimelineEventKind::Heal)
        {
            // A zero lower bound is valid for a sub-one-point random HoT.
            if (!event.spellId || !maximumAmount ||
                maximumAmount < event.amount)
                return false;
        }
        else if (event.kind == HealingTimelineEventKind::Damage)
        {
            if (!event.amount || maximumAmount != event.amount)
                return false;
        }
        else
            return false;
    }
    std::sort(events.begin(), events.end(),
        [](HealingTimelineEvent const& left,
           HealingTimelineEvent const& right)
        {
            if (left.arrivalMs != right.arrivalMs)
                return left.arrivalMs < right.arrivalMs;
            if (left.kind != right.kind)
                return static_cast<std::uint8_t>(left.kind) <
                       static_cast<std::uint8_t>(right.kind);
            if (left.sourceGuid != right.sourceGuid)
                return left.sourceGuid < right.sourceGuid;
            if (left.spellId != right.spellId)
                return left.spellId < right.spellId;
            if (left.amount != right.amount)
                return left.amount < right.amount;
            return left.maximumAmount < right.maximumAmount;
        });

    std::uint64_t lowerHealth = currentHealth;
    std::uint64_t upperHealth = currentHealth;
    std::size_t index = 0;
    while (index < events.size())
    {
        std::size_t end = index + 1;
        std::uint64_t simultaneousMinimum = events[index].amount;
        std::uint64_t simultaneousMaximum =
            events[index].maximumAmount
                ? events[index].maximumAmount : events[index].amount;
        bool const firstIsDamage =
            events[index].kind == HealingTimelineEventKind::Damage;
        while (end < events.size() &&
               events[end].arrivalMs == events[index].arrivalMs)
        {
            if ((events[end].kind == HealingTimelineEventKind::Damage) !=
                firstIsDamage)
            {
                projection = HealingTimelineProjection();
                return false;
            }
            simultaneousMinimum += events[end].amount;
            simultaneousMaximum += events[end].maximumAmount
                ? events[end].maximumAmount : events[end].amount;
            ++end;
        }
        if (firstIsDamage)
        {
            std::uint64_t const lowerApplied =
                std::min(lowerHealth, simultaneousMinimum);
            std::uint64_t const upperApplied =
                std::min(upperHealth, simultaneousMaximum);
            lowerHealth -= lowerApplied;
            upperHealth -= upperApplied;
            projection.appliedDamage += lowerApplied;
            projection.maximumAppliedDamage += upperApplied;
        }
        else
        {
            // Ordinary healing cannot resurrect a target killed by damage.
            if (!lowerHealth || !upperHealth)
            {
                projection = HealingTimelineProjection();
                return false;
            }
            if (end - index > 1)
            {
                ++projection.simultaneousArrivalGroups;
                projection.concurrentAttributionUnknown = true;
            }
            std::uint64_t const lowerMissing = maxHealth - lowerHealth;
            std::uint64_t const upperMissing = maxHealth - upperHealth;
            std::uint64_t const lowerEffective =
                std::min(lowerMissing, simultaneousMinimum);
            std::uint64_t const upperEffective =
                std::min(upperMissing, simultaneousMaximum);
            projection.effectiveHealing += lowerEffective;
            projection.maximumEffectiveHealing += upperEffective;
            projection.overheal += simultaneousMinimum - lowerEffective;
            projection.maximumOverheal += simultaneousMaximum - upperEffective;
            lowerHealth += lowerEffective;
            upperHealth += upperEffective;
        }
        index = end;
    }
    projection.finalHealth = static_cast<std::uint32_t>(lowerHealth);
    projection.maximumFinalHealth = static_cast<std::uint32_t>(upperHealth);
    projection.eventCount = events.size();
    projection.valid = true;
    return true;
}
} // namespace PartyBotEngine

#endif
