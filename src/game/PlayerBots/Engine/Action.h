#ifndef MANGOS_PARTYBOT_ACTION_H
#define MANGOS_PARTYBOT_ACTION_H
#include "DecisionContext.h"
namespace PartyBotEngine {
class Action {
public:
    virtual ~Action() {}
    virtual std::string const& GetName() const = 0;
    virtual bool IsPossible(DecisionContext const&, ActionCandidate const&, std::string& reason) const = 0;
    virtual bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string& reason) const = 0;
    virtual ActionResult Execute(DecisionContext&, ActionCandidate const&) = 0;
};
}
#endif
