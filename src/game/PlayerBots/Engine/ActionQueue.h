#ifndef MANGOS_PARTYBOT_ACTION_QUEUE_H
#define MANGOS_PARTYBOT_ACTION_QUEUE_H

#include "DecisionTypes.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

namespace PartyBotEngine
{

// A bounded, per-decision-batch queue. It stores report indices rather than
// Core pointers, so queued work is revalidated by the executor at dispatch.
class ActionQueue
{
public:
    struct Entry
    {
        std::size_t reportIndex = 0;
        float priority = 0.0f;
        std::uint32_t ordinal = 0;
    };

    static std::size_t const Capacity = 32;

    bool Push(std::size_t reportIndex, float priority)
    {
        if (m_entries.size() >= Capacity || !std::isfinite(priority))
            return false; // Fail closed; never evict an already queued action.
        Entry entry;
        entry.reportIndex = reportIndex;
        entry.priority = priority;
        entry.ordinal = m_nextOrdinal++;
        m_entries.push_back(entry);
        return true;
    }

    bool PopNext(Entry& entry)
    {
        if (m_entries.empty())
            return false;

        std::size_t best = 0;
        for (std::size_t i = 1; i < m_entries.size(); ++i)
        {
            Entry const& candidate = m_entries[i];
            Entry const& current = m_entries[best];
            if (candidate.priority > current.priority ||
                (candidate.priority == current.priority && candidate.ordinal < current.ordinal))
                best = i;
        }

        entry = m_entries[best];
        m_entries.erase(m_entries.begin() + best);
        return true;
    }

    std::size_t Size() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }
    void Clear() { m_entries.clear(); }

private:
    std::vector<Entry> m_entries;
    std::uint32_t m_nextOrdinal = 0;
};

enum class PendingActionEnqueueResult : std::uint8_t
{
    Queued,
    UpdatedExisting,
    Full,
    Invalid,
    ActionNotRegistered,
    LifetimeDisabled,
    NotEligible
};

// Cross-update action-event storage. Entries own only ActionCandidate's
// scalar/string data; they never retain Core object pointers.
class PendingActionQueue
{
public:
    struct Context
    {
        std::uint32_t groupId = 0;
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        Guid botGuid = 0;
        std::uint64_t encounterId = 0;
        bool encounterActive = false;
        bool scoped = false;
    };

    struct Entry
    {
        ActionCandidate candidate;
        Context context;
        float priority = 0.0f;
        std::uint32_t enqueuedAtMs = 0;
        std::uint32_t expiresAtMs = 0;
        std::uint64_t ordinal = 0;
    };

    static std::size_t const Capacity = 32;
    static std::uint32_t const MaxLifetimeMs = 0x7fffffffu;

    // Once an entry has waited this long, age takes precedence over new work.
    // Live entries have TTL < 2^31 ms, so unsigned elapsed-time arithmetic is
    // wrap-safe for every entry that can still be present in the queue.
    static std::uint32_t const FairnessWaitMs = 3000u;

    static bool ComesBefore(Entry const& left, Entry const& right,
                            std::uint32_t nowMs)
    {
        std::uint32_t const leftWaitMs = nowMs - left.enqueuedAtMs;
        std::uint32_t const rightWaitMs = nowMs - right.enqueuedAtMs;
        bool const leftPromoted = leftWaitMs >= FairnessWaitMs;
        bool const rightPromoted = rightWaitMs >= FairnessWaitMs;
        if (leftPromoted != rightPromoted)
            return leftPromoted;
        if (leftPromoted && leftWaitMs != rightWaitMs)
            return leftWaitMs > rightWaitMs;
        if (!leftPromoted && left.priority != right.priority)
            return left.priority > right.priority;
        return left.ordinal < right.ordinal;
    }

    PendingActionEnqueueResult Enqueue(ActionCandidate const& candidate,
                                       std::uint32_t nowMs,
                                       std::uint32_t lifetimeMs)
    {
        return Enqueue(candidate, nowMs, lifetimeMs, Context());
    }

    PendingActionEnqueueResult Enqueue(ActionCandidate const& candidate,
                                       std::uint32_t nowMs,
                                       std::uint32_t lifetimeMs,
                                       Context const& context)
    {
        if (candidate.actionName.empty() || !std::isfinite(candidate.finalPriority) ||
            !lifetimeMs || lifetimeMs > MaxLifetimeMs ||
            (candidate.hasDestination &&
                (!std::isfinite(candidate.destinationX) ||
                 !std::isfinite(candidate.destinationY) ||
                 !std::isfinite(candidate.destinationZ))))
            return PendingActionEnqueueResult::Invalid;

        PruneExpired(nowMs);
        for (Entry& existing : m_entries)
        {
            if (!HasSameIdentity(existing.candidate, candidate) ||
                !SameContext(existing.context, context))
                continue;

            existing.candidate = candidate;
            existing.candidate.actionId = 0;
            existing.candidate.threatActionId = 0;
            existing.context = context;
            existing.priority = candidate.finalPriority;
            return PendingActionEnqueueResult::UpdatedExisting;
        }

        if (m_entries.size() >= Capacity)
            return PendingActionEnqueueResult::Full;

        Entry entry;
        entry.candidate = candidate;
        entry.candidate.actionId = 0;
        entry.candidate.threatActionId = 0;
        entry.context = context;
        entry.priority = candidate.finalPriority;
        entry.enqueuedAtMs = nowMs;
        entry.expiresAtMs = nowMs + lifetimeMs;
        entry.ordinal = m_nextOrdinal++;
        m_entries.push_back(entry);
        return PendingActionEnqueueResult::Queued;
    }

    bool PopNext(std::uint32_t nowMs, Entry& entry)
    {
        PruneExpired(nowMs);
        if (m_entries.empty())
            return false;

        std::size_t best = 0;
        for (std::size_t i = 1; i < m_entries.size(); ++i)
            if (ComesBefore(m_entries[i], m_entries[best], nowMs))
                best = i;

        entry = m_entries[best];
        m_entries.erase(m_entries.begin() + best);
        return true;
    }

    std::size_t PruneExpired(std::uint32_t nowMs)
    {
        std::size_t const previousSize = m_entries.size();
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [nowMs](Entry const& entry) { return IsExpired(nowMs, entry.expiresAtMs); }),
            m_entries.end());
        return previousSize - m_entries.size();
    }

    bool Requeue(Entry const& entry, std::uint32_t nowMs)
    {
        if (IsExpired(nowMs, entry.expiresAtMs) ||
            entry.candidate.actionName.empty() ||
            !std::isfinite(entry.priority) ||
            m_entries.size() >= Capacity)
            return false;
        m_entries.push_back(entry);
        if (m_nextOrdinal <= entry.ordinal)
            m_nextOrdinal = entry.ordinal + 1;
        return true;
    }

    std::size_t Size() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }

    void Clear()
    {
        m_entries.clear();
        m_nextOrdinal = 0;
    }

private:
    static bool SameContext(Context const& left, Context const& right)
    {
        return left.scoped == right.scoped &&
            (!left.scoped ||
                (left.groupId == right.groupId &&
                 left.mapId == right.mapId &&
                 left.instanceId == right.instanceId &&
                 left.botGuid == right.botGuid &&
                 left.encounterId == right.encounterId &&
                 left.encounterActive == right.encounterActive));
    }

    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs)
    {
        // Valid because Enqueue rejects lifetimes >= 2^31 milliseconds.
        return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
    }

    static bool HasSameIdentity(ActionCandidate const& left,
                                ActionCandidate const& right)
    {
        return left.actionName == right.actionName &&
            left.targetGuid == right.targetGuid &&
            left.spellId == right.spellId &&
            left.channel == right.channel &&
            left.hasDestination == right.hasDestination &&
            (!left.hasDestination ||
                (left.destinationX == right.destinationX &&
                 left.destinationY == right.destinationY &&
                 left.destinationZ == right.destinationZ));
    }

    std::vector<Entry> m_entries;
    std::uint64_t m_nextOrdinal = 0;
};


} // namespace PartyBotEngine

#endif
