#ifndef MANGOS_PARTYBOT_MULTIPLIER_H
#define MANGOS_PARTYBOT_MULTIPLIER_H
#include "DecisionContext.h"
namespace PartyBotEngine {
class Multiplier {
public:
    virtual ~Multiplier() {}
    virtual std::string const& GetName() const = 0;
    virtual float GetValue(DecisionContext const&, ActionCandidate const&) const = 0;
};
}
#endif
