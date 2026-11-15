#ifndef MANGOS_PARTYBOT_TRIGGER_H
#define MANGOS_PARTYBOT_TRIGGER_H
#include "DecisionContext.h"
namespace PartyBotEngine {
class Trigger {
public:
    virtual ~Trigger() {}
    virtual std::string const& GetName() const = 0;
    virtual bool IsActive(DecisionContext const& context) const = 0;
    virtual void CollectCandidates(DecisionContext const&, std::vector<ActionCandidate>& candidates) const = 0;
};
}
#endif
