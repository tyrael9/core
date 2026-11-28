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
    // For Damage only: Core school bitmask; zero means unknown.
    std::uint32_t schoolMask = 0;
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
    std::uint64_t absorbedDamage = 0;
    std::uint64_t maximumAbsorbedDamage = 0;
    bool schoolAbsorbForecastIncomplete = false;
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
    std::uint32_t amount, std::vector<HealingTimelineEvent>& events,
    std::uint32_t schoolMask = 0)
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
    event.schoolMask = schoolMask;
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

// Project explicit Heal/Damage arrivals against a fixed health ceiling.
// Damage amounts are post-resistance, immediately before Core school absorbs;
// resistance is random and intentionally outside this value-only projection.
// Same-timestamp mixed Heal/Damage or order-sensitive different-school damage
// is rejected. School Absorb pools use Core's (auraMask & damageMask) rule.
inline bool ProjectHealingTimeline(
    std::uint32_t currentHealth, std::uint32_t maxHealth,
    std::vector<HealingTimelineEvent> const& input,
    std::vector<SchoolAbsorbForecastSnapshot> const& inputAbsorbs,
    bool absorbsTruncated,
    HealingTimelineProjection& projection)
{
    projection = HealingTimelineProjection();
    projection.schoolAbsorbForecastIncomplete = absorbsTruncated;
    if (!maxHealth || currentHealth > maxHealth ||
        input.size() > MAX_HEALING_TIMELINE_EVENTS)
        return false;

    for (SchoolAbsorbForecastSnapshot const& absorb : inputAbsorbs)
    {
        if (!absorb.spellId || !absorb.schoolMask ||
            absorb.schoolMask > 0x7fu || !absorb.remainingAmount ||
            absorb.remainingAmount > static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max()) ||
            !absorb.remainingDurationMs)
        {
            projection = HealingTimelineProjection();
            return false;
        }
    }

    std::vector<HealingTimelineEvent> events = input;
    for (HealingTimelineEvent const& event : events)
    {
        if (!event.sourceGuid || event.schoolMask > 0x7fu)
            return false;
        std::uint32_t const maximumAmount =
            event.maximumAmount ? event.maximumAmount : event.amount;
        if (event.kind == HealingTimelineEventKind::Heal)
        {
            if (!event.spellId || !maximumAmount ||
                maximumAmount < event.amount || event.schoolMask != 0)
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
            if (left.schoolMask != right.schoolMask)
                return left.schoolMask < right.schoolMask;
            if (left.amount != right.amount)
                return left.amount < right.amount;
            return left.maximumAmount < right.maximumAmount;
        });

    std::vector<std::uint32_t> absorbRemaining;
    absorbRemaining.reserve(inputAbsorbs.size());
    for (SchoolAbsorbForecastSnapshot const& absorb : inputAbsorbs)
        absorbRemaining.push_back(absorb.remainingAmount);

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
            std::uint32_t const arrivalMs = events[index].arrivalMs;
            bool hasActiveAbsorb = false;
            if (!absorbsTruncated)
            {
                for (std::size_t shieldIndex = 0;
                     shieldIndex < inputAbsorbs.size(); ++shieldIndex)
                {
                    SchoolAbsorbForecastSnapshot const& shield =
                        inputAbsorbs[shieldIndex];
                    if (!absorbRemaining[shieldIndex] ||
                        arrivalMs >= shield.remainingDurationMs)
                        continue;
                    hasActiveAbsorb = true;
                    if (!events[index].schoolMask)
                    {
                        projection = HealingTimelineProjection();
                        return false;
                    }
                    bool const matches =
                        (shield.schoolMask & events[index].schoolMask) != 0;
                    // All same-time damage must see identical eligible shield
                    // pools. Otherwise Core hit/Aura consumption order could
                    // change the result and the snapshot has no hit order.
                    for (std::size_t eventIndex = index + 1;
                         eventIndex < end; ++eventIndex)
                    {
                        if (!events[eventIndex].schoolMask ||
                            (((shield.schoolMask & events[eventIndex].schoolMask) != 0)
                                != matches))
                        {
                            projection = HealingTimelineProjection();
                            return false;
                        }
                    }
                }
            }

            std::uint64_t remainingAfterAbsorb = simultaneousMinimum;
            if (!absorbsTruncated && hasActiveAbsorb)
            {
                // A shield is eligible for the whole timestamp group only
                // when its match result is identical for every hit in it.
                for (std::size_t shieldIndex = 0;
                     shieldIndex < inputAbsorbs.size() &&
                     remainingAfterAbsorb > 0; ++shieldIndex)
                {
                    SchoolAbsorbForecastSnapshot const& shield =
                        inputAbsorbs[shieldIndex];
                    if (!absorbRemaining[shieldIndex] ||
                        arrivalMs >= shield.remainingDurationMs ||
                        !(shield.schoolMask & events[index].schoolMask))
                        continue;
                    std::uint64_t const consumed =
                        std::min<std::uint64_t>(
                            remainingAfterAbsorb, absorbRemaining[shieldIndex]);
                    absorbRemaining[shieldIndex] -=
                        static_cast<std::uint32_t>(consumed);
                    remainingAfterAbsorb -= consumed;
                }
            }
            std::uint64_t const absorbed =
                simultaneousMinimum - remainingAfterAbsorb;
            projection.absorbedDamage += absorbed;
            projection.maximumAbsorbedDamage += absorbed;
            std::uint64_t const lowerApplied =
                std::min(lowerHealth, remainingAfterAbsorb);
            std::uint64_t const upperApplied =
                std::min(upperHealth, remainingAfterAbsorb);
            lowerHealth -= lowerApplied;
            upperHealth -= upperApplied;
            projection.appliedDamage += lowerApplied;
            projection.maximumAppliedDamage += upperApplied;
        }
        else
        {
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

// Existing callers without school-specific shield inputs retain their prior
// semantics and fail closed only on the established event-order boundaries.
inline bool ProjectHealingTimeline(
    std::uint32_t currentHealth, std::uint32_t maxHealth,
    std::vector<HealingTimelineEvent> const& input,
    HealingTimelineProjection& projection)
{
    std::vector<SchoolAbsorbForecastSnapshot> noAbsorbs;
    return ProjectHealingTimeline(
        currentHealth, maxHealth, input, noAbsorbs, false, projection);
}

} // namespace PartyBotEngine

#endif
