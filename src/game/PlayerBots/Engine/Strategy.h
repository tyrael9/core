#ifndef MANGOS_PARTYBOT_STRATEGY_H
#define MANGOS_PARTYBOT_STRATEGY_H
#include "Trigger.h"
#include <memory>
#include <utility>
namespace PartyBotEngine {
class Strategy {
public:
    explicit Strategy(std::string name) : m_name(std::move(name)) {}
    virtual ~Strategy() {}
    std::string const& GetName() const { return m_name; }
    void AddTrigger(std::shared_ptr<Trigger> trigger) { if (trigger) m_triggers.push_back(std::move(trigger)); }
    std::vector<std::shared_ptr<Trigger>> const& GetTriggers() const { return m_triggers; }
private:
    std::string m_name;
    std::vector<std::shared_ptr<Trigger>> m_triggers;
};
}
#endif
