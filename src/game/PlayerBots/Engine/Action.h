#ifndef MANGOS_PARTYBOT_ACTION_H
#define MANGOS_PARTYBOT_ACTION_H
#include "DecisionContext.h"
#include "ThreatImpact.h"
namespace PartyBotEngine {
class Action {
public:
    virtual ~Action() {}
    virtual std::string const& GetName() const = 0;
    virtual bool IsPossible(DecisionContext const&, ActionCandidate const&, std::string& reason) const = 0;
    virtual bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string& reason) const = 0;
    virtual ActionResult Execute(DecisionContext&, ActionCandidate const&) = 0;
    // Safe default: until an action can enumerate its full threat impact, it is
    // unknown, never implicitly zero-cost. Estimates contain GUID/scalar data only.
    virtual ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                                        ActionCandidate const&) const {
        ThreatImpactEstimate estimate;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "action_threat_impact_not_implemented";
        return estimate;
    }
};
}
#endif
