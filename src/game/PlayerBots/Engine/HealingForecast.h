#ifndef MANGOS_PARTYBOT_HEALING_FORECAST_H
#define MANGOS_PARTYBOT_HEALING_FORECAST_H

#include "PartySnapshot.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace PartyBotEngine
{
constexpr std::size_t MAX_HEALING_TIMELINE_EVENTS = 256;

struct HealingTimelineEvent
{
    std::uint32_t arrivalMs = 0;
    std::uint32_t amount = 0;
    Guid sourceGuid = 0;
    std::uint32_t spellId = 0;
};

struct HealingTimelineProjection
{
    std::uint32_t finalHealth = 0;
    std::size_t eventCount = 0;
    std::size_t simultaneousArrivalGroups = 0;
    std::uint64_t effectiveHealing = 0;
    std::uint64_t overheal = 0;
    bool concurrentAttributionUnknown = false;
    bool valid = false;
};

inline bool AppendPeriodicHealForecastEvents(
    PeriodicHealForecastSnapshot const& forecast,
    std::vector<HealingTimelineEvent>& events)
{
    if (!forecast.casterGuid || !forecast.spellId || !forecast.periodMs ||
        !forecast.remainingTicks || !forecast.healingPerTick ||
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
    event.sourceGuid = sourceGuid;
    event.spellId = spellId;
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

// Healing-only projection against a fixed health ceiling. Damage between
// events, Core batching and simultaneous per-cast attribution are not guessed.
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
        if (!event.sourceGuid || !event.spellId || !event.amount)
            return false;

    std::sort(events.begin(), events.end(),
        [](HealingTimelineEvent const& left,
           HealingTimelineEvent const& right)
        {
            if (left.arrivalMs != right.arrivalMs)
                return left.arrivalMs < right.arrivalMs;
            if (left.sourceGuid != right.sourceGuid)
                return left.sourceGuid < right.sourceGuid;
            if (left.spellId != right.spellId)
                return left.spellId < right.spellId;
            return left.amount < right.amount;
        });

    std::uint64_t missing = maxHealth - currentHealth;
    std::size_t index = 0;
    while (index < events.size())
    {
        std::size_t end = index + 1;
        std::uint64_t simultaneousAmount = events[index].amount;
        while (end < events.size() &&
               events[end].arrivalMs == events[index].arrivalMs)
        {
            simultaneousAmount += events[end].amount;
            ++end;
        }
        if (end - index > 1)
        {
            ++projection.simultaneousArrivalGroups;
            projection.concurrentAttributionUnknown = true;
        }
        std::uint64_t const effective =
            std::min(missing, simultaneousAmount);
        projection.effectiveHealing += effective;
        projection.overheal += simultaneousAmount - effective;
        missing -= effective;
        index = end;
    }

    projection.finalHealth = maxHealth - static_cast<std::uint32_t>(missing);
    projection.eventCount = events.size();
    projection.valid = true;
    return true;
}
} // namespace PartyBotEngine

#endif
