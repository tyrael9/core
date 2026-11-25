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

enum class HealingTimelineEventKind : std::uint8_t
{
    Heal,
    Damage
};

struct HealingTimelineEvent
{
    std::uint32_t arrivalMs = 0;
    std::uint32_t amount = 0;
    Guid sourceGuid = 0;
    std::uint32_t spellId = 0;
    HealingTimelineEventKind kind = HealingTimelineEventKind::Heal;
};

struct HealingTimelineProjection
{
    std::uint32_t finalHealth = 0;
    std::size_t eventCount = 0;
    std::size_t simultaneousArrivalGroups = 0;
    std::uint64_t effectiveHealing = 0;
    std::uint64_t overheal = 0;
    std::uint64_t appliedDamage = 0;
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
// ceiling. Mixed heal/damage events at one timestamp are rejected because the
// snapshot does not prove Core's application order. Core batching and
// simultaneous per-cast attribution are not guessed.
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
        if (!event.sourceGuid || !event.amount)
            return false;
        if (event.kind == HealingTimelineEventKind::Heal && !event.spellId)
            return false;
        if (event.kind != HealingTimelineEventKind::Heal &&
            event.kind != HealingTimelineEventKind::Damage)
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
            return left.amount < right.amount;
        });

    std::uint64_t health = currentHealth;
    std::size_t index = 0;
    while (index < events.size())
    {
        std::size_t end = index + 1;
        std::uint64_t simultaneousAmount = events[index].amount;
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
            simultaneousAmount += events[end].amount;
            ++end;
        }

        if (firstIsDamage)
        {
            std::uint64_t const applied =
                std::min(health, simultaneousAmount);
            health -= applied;
            projection.appliedDamage += applied;
        }
        else
        {
            if (end - index > 1)
            {
                ++projection.simultaneousArrivalGroups;
                projection.concurrentAttributionUnknown = true;
            }
            std::uint64_t const missing = maxHealth - health;
            std::uint64_t const effective =
                std::min(missing, simultaneousAmount);
            projection.effectiveHealing += effective;
            projection.overheal += simultaneousAmount - effective;
            health += effective;
        }
        index = end;
    }

    projection.finalHealth = static_cast<std::uint32_t>(health);
    projection.eventCount = events.size();
    projection.valid = true;
    return true;
}
} // namespace PartyBotEngine

#endif
