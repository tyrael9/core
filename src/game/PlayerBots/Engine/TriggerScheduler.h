#ifndef MANGOS_PARTYBOT_TRIGGER_SCHEDULER_H
#define MANGOS_PARTYBOT_TRIGGER_SCHEDULER_H
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
namespace PartyBotEngine {
class TriggerScheduler {
public:
    static std::size_t const Capacity = 128;
    static std::uint32_t const MaxIntervalMs = 0x7fffffffu;
    bool ShouldEvaluate(std::string const& triggerKey,
                        std::uint32_t intervalMs,
                        std::uint32_t nowMs) {
        if (triggerKey.empty() || !intervalMs || intervalMs > MaxIntervalMs)
            return true;
        auto found = m_entries.find(triggerKey);
        if (found == m_entries.end()) {
            // Fail open at capacity: an untracked trigger runs every update.
            if (m_entries.size() >= Capacity) return true;
            Entry entry;
            entry.hasLastEvaluation = true;
            entry.lastEvaluationAtMs = nowMs;
            m_entries.insert(std::make_pair(triggerKey, entry));
            return true;
        }
        Entry& entry = found->second;
        if (!entry.hasLastEvaluation || entry.invalidated ||
            HasElapsed(nowMs, entry.lastEvaluationAtMs, intervalMs)) {
            entry.hasLastEvaluation = true;
            entry.invalidated = false;
            entry.lastEvaluationAtMs = nowMs;
            return true;
        }
        return false;
    }
    bool Invalidate(std::string const& triggerKey) {
        auto found = m_entries.find(triggerKey);
        if (found == m_entries.end()) return false;
        found->second.invalidated = true;
        return true;
    }
    void InvalidateAll() {
        for (auto& entry : m_entries) entry.second.invalidated = true;
    }
    void Clear() { m_entries.clear(); }
    std::size_t Size() const { return m_entries.size(); }
private:
    struct Entry {
        bool hasLastEvaluation = false;
        bool invalidated = false;
        std::uint32_t lastEvaluationAtMs = 0;
    };
    static bool HasElapsed(std::uint32_t nowMs, std::uint32_t previousMs,
                           std::uint32_t intervalMs) {
        return static_cast<std::int32_t>(nowMs - previousMs) >=
            static_cast<std::int32_t>(intervalMs);
    }
    std::map<std::string, Entry> m_entries;
};
} // namespace PartyBotEngine
#endif
