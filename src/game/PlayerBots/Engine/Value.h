#ifndef MANGOS_PARTYBOT_VALUE_H
#define MANGOS_PARTYBOT_VALUE_H
#include "DecisionContext.h"
#include "ValueCache.h"
#include <functional>
#include <string>
#include <utility>
namespace PartyBotEngine {
template <class T>
class Value {
public:
    virtual ~Value() {}
    virtual T Get(DecisionContext const& context) const = 0;
};
// Named typed decision fact. The provider returns copied value data, never a
// Core pointer or reference that could outlive the current map update.
template <class T>
class CachedValue final : public Value<T> {
public:
    using Compute = std::function<T(DecisionContext const&)>;
    CachedValue(std::string name, std::uint32_t lifetimeMs, Compute compute)
        : m_name(std::move(name)), m_lifetimeMs(lifetimeMs),
          m_compute(std::move(compute)) {}
    T Get(DecisionContext const& context) const override {
        if (!context.valueCache) return m_compute(context);
        return context.valueCache->GetOrCompute<T>(
            m_name, context.snapshot.epoch, context.nowMs, m_lifetimeMs,
            [this, &context]() { return m_compute(context); });
    }
    std::string const& GetName() const { return m_name; }
private:
    std::string m_name;
    std::uint32_t m_lifetimeMs;
    Compute m_compute;
};
} // namespace PartyBotEngine
#endif
