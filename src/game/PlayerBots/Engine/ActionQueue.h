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
    Invalid
};

// Cross-update action-event storage. Entries own only ActionCandidate's
// scalar/string data; they never retain Core object pointers.
class PendingActionQueue
{
public:
    struct Entry
    {
        ActionCandidate candidate;
        float priority = 0.0f;
        std::uint32_t enqueuedAtMs = 0;
        std::uint32_t expiresAtMs = 0;
        std::uint64_t ordinal = 0;
    };

    static std::size_t const Capacity = 32;
    static std::uint32_t const MaxLifetimeMs = 0x7fffffffu;

    PendingActionEnqueueResult Enqueue(ActionCandidate const& candidate,
                                       std::uint32_t nowMs,
                                       std::uint32_t lifetimeMs)
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
            if (!HasSameIdentity(existing.candidate, candidate))
                continue;

            // A repeated trigger refreshes the latest payload/priority but
            // keeps the original FIFO position and deadline. It cannot keep
            // an event alive forever by re-enqueueing it every update.
            existing.candidate = candidate;
            existing.priority = candidate.finalPriority;
            return PendingActionEnqueueResult::UpdatedExisting;
        }

        if (m_entries.size() >= Capacity)
            return PendingActionEnqueueResult::Full;

        Entry entry;
        entry.candidate = candidate;
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

    std::size_t PruneExpired(std::uint32_t nowMs)
    {
        std::size_t const previousSize = m_entries.size();
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [nowMs](Entry const& entry) { return IsExpired(nowMs, entry.expiresAtMs); }),
            m_entries.end());
        return previousSize - m_entries.size();
    }

    std::size_t Size() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }

    void Clear()
    {
        m_entries.clear();
        m_nextOrdinal = 0;
    }

private:
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
