#ifndef MANGOS_PARTYBOT_ACTION_RESULT_LISTENER_H
#define MANGOS_PARTYBOT_ACTION_RESULT_LISTENER_H

#include "DecisionContext.h"

namespace PartyBotEngine
{
// This event describes the synchronous Engine dispatcher boundary only.
// attempted=false means the dispatcher rejected the selected candidate before
// calling Action::Execute. attempted=true means this is the direct return from
// Action::Execute. Neither case claims that a later Core Spell has completed.
struct ActionResultEvent
{
    ActionCandidate candidate;
    ActionId actionId = 0;
    // Encounter identity is captured at the dispatcher boundary so a result
    // cannot satisfy a dependency after a map/encounter lifecycle transition.
    std::uint64_t encounterId = 0;
    ActionResult result = ActionResult::NotAttempted;
    bool attempted = false;
    std::uint32_t snapshotEpoch = 0;
    std::uint32_t occurredAtMs = 0;
    std::string reason;
};

class ActionResultListener
{
public:
    virtual ~ActionResultListener() {}
    virtual void OnActionResult(DecisionContext const& context,
                                ActionResultEvent const& event) = 0;
};
} // namespace PartyBotEngine

#endif
