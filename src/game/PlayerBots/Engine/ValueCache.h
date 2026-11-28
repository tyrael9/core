#ifndef MANGOS_PARTYBOT_VALUE_CACHE_H
#define MANGOS_PARTYBOT_VALUE_CACHE_H
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>
namespace PartyBotEngine {
class ValueCache {
public:
    static std::size_t const Capacity = 128;
    static std::uint32_t const MaxLifetimeMs = 0x7fffffffu;
    template <class T, class Compute>
    T GetOrCompute(std::string const& name, std::uint32_t snapshotEpoch,
                   std::uint32_t nowMs, std::uint32_t lifetimeMs,
                   Compute compute) const {
        static_assert(std::is_copy_constructible<T>::value,
                      "cached decision values must be copy constructible");
        if (name.empty() || !lifetimeMs || lifetimeMs > MaxLifetimeMs)
            return compute();
        PruneExpired(nowMs);
        auto found = m_entries.find(name);
        if (found != m_entries.end()) {
            Entry const& entry = found->second;
            if (entry.snapshotEpoch == snapshotEpoch &&
                !IsExpired(nowMs, entry.expiresAtMs) &&
                entry.type == std::type_index(typeid(T))) {
                std::shared_ptr<T const> cached =
                    std::static_pointer_cast<T const>(entry.value);
                return *cached;
            }
            m_entries.erase(found);
        }
        T computed = compute();
        if (m_entries.size() < Capacity) {
            Entry entry;
            entry.type = std::type_index(typeid(T));
            entry.value = std::make_shared<T const>(computed);
            entry.snapshotEpoch = snapshotEpoch;
            entry.expiresAtMs = nowMs + lifetimeMs;
            m_entries.insert(std::make_pair(name, std::move(entry)));
        }
        // Never evict a live fact under capacity pressure. Compute without
        // caching instead, so overflow cannot change decision correctness.
        return computed;
    }
    bool Invalidate(std::string const& name) const { return m_entries.erase(name) != 0; }
    void InvalidateAll() const { m_entries.clear(); }
    std::size_t PruneExpired(std::uint32_t nowMs) const {
        std::size_t removed = 0;
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (IsExpired(nowMs, it->second.expiresAtMs)) {
                it = m_entries.erase(it);
                ++removed;
            } else ++it;
        }
        return removed;
    }
    std::size_t Size() const { return m_entries.size(); }
private:
    struct Entry {
        Entry() : type(typeid(void)) {}
        std::type_index type;
        std::shared_ptr<void const> value;
        std::uint32_t snapshotEpoch = 0;
        std::uint32_t expiresAtMs = 0;
    };
    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs) {
        return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
    }
    mutable std::map<std::string, Entry> m_entries;
};
} // namespace PartyBotEngine
#endif
