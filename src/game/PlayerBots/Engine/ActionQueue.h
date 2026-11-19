#ifndef MANGOS_PARTYBOT_ACTION_QUEUE_H
#define MANGOS_PARTYBOT_ACTION_QUEUE_H

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

} // namespace PartyBotEngine

#endif
