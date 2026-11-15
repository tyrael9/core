#ifndef MANGOS_PARTYBOT_PARTYBOT_ENGINE_H
#define MANGOS_PARTYBOT_PARTYBOT_ENGINE_H
#include "Action.h"
#include "Multiplier.h"
#include "Strategy.h"
#include <map>
#include <memory>
namespace PartyBotEngine {
class PartyBotEngine {
public:
    void AddStrategy(std::shared_ptr<Strategy> strategy);
    bool RegisterAction(std::shared_ptr<Action> action);
    void AddMultiplier(std::shared_ptr<Multiplier> multiplier);
    DecisionReport Evaluate(DecisionContext const& context) const;
    // Only call when Strategy mode owns the relevant decision domain.
    void ExecuteSelected(DecisionContext& context, DecisionReport& report) const;
private:
    std::shared_ptr<Action> FindAction(std::string const& name) const;
    std::vector<std::shared_ptr<Strategy>> m_strategies;
    std::map<std::string, std::shared_ptr<Action>> m_actions;
    std::vector<std::shared_ptr<Multiplier>> m_multipliers;
};
}
#endif
