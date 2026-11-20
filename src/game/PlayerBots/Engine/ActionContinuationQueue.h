#ifndef MANGOS_PARTYBOT_ACTION_CONTINUATION_QUEUE_H
#define MANGOS_PARTYBOT_ACTION_CONTINUATION_QUEUE_H

#include "ActionQueue.h"
#include "ActionContinuation.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

namespace PartyBotEngine
{
class ActionContinuationQueue
{
public:
    struct Entry
    {
        ActionId parentActionId = 0;
        Guid parentTargetGuid = 0;
        std::uint32_t parentSpellId = 0;
        ActionCandidate candidate;
        PendingActionQueue::Context context;
        std::uint32_t enqueuedAtMs = 0;
        std::uint32_t expiresAtMs = 0;
        std::uint64_t ordinal = 0;
    };

    static std::size_t const Capacity = 32;
    static std::uint32_t const MaxLifetimeMs = PendingActionQueue::MaxLifetimeMs;

    ActionContinuationEnqueueResult Enqueue(
        ActionCandidate const& parent, ActionCandidate const& continuation,
        std::uint32_t nowMs, std::uint32_t lifetimeMs,
        PendingActionQueue::Context const& context)
    {
        if (!parent.actionId || !parent.spellId || !parent.targetGuid ||
            continuation.actionName.empty() || !std::isfinite(continuation.finalPriority) ||
            !lifetimeMs || lifetimeMs > MaxLifetimeMs || !context.scoped ||
            !context.encounterActive || !context.encounterId ||
            (continuation.hasDestination &&
                (!std::isfinite(continuation.destinationX) ||
                 !std::isfinite(continuation.destinationY) ||
                 !std::isfinite(continuation.destinationZ))))
            return ActionContinuationEnqueueResult::Invalid;

        PruneExpired(nowMs);
        for (Entry const& existing : m_entries)
            if (existing.parentActionId == parent.actionId &&
                existing.context.encounterId == context.encounterId)
                return ActionContinuationEnqueueResult::DuplicateParent;

        if (m_entries.size() >= Capacity)
            return ActionContinuationEnqueueResult::Full;

        Entry entry;
        entry.parentActionId = parent.actionId;
        entry.parentTargetGuid = parent.targetGuid;
        entry.parentSpellId = parent.spellId;
        entry.candidate = continuation;
        entry.candidate.actionId = 0;
        entry.candidate.threatActionId = 0;
        entry.context = context;
        entry.enqueuedAtMs = nowMs;
        entry.expiresAtMs = nowMs + lifetimeMs;
        entry.ordinal = m_nextOrdinal++;
        m_entries.push_back(entry);
        return ActionContinuationEnqueueResult::Queued;
    }

    bool Take(ActionId parentActionId, std::uint64_t encounterId, Entry& entry)
    {
        for (auto itr = m_entries.begin(); itr != m_entries.end(); ++itr)
        {
            if (itr->parentActionId != parentActionId ||
                itr->context.encounterId != encounterId)
                continue;
            entry = *itr;
            m_entries.erase(itr);
            return true;
        }
        return false;
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

    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs)
    {
        return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
    }

private:
    std::vector<Entry> m_entries;
    std::uint64_t m_nextOrdinal = 0;
};
} // namespace PartyBotEngine

#endif
