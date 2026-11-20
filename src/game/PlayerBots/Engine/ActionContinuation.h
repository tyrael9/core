#ifndef MANGOS_PARTYBOT_ACTION_CONTINUATION_H
#define MANGOS_PARTYBOT_ACTION_CONTINUATION_H

#include "DecisionTypes.h"
#include <cstdint>

namespace PartyBotEngine
{
struct DecisionContext;

enum class ActionContinuationEnqueueResult : std::uint8_t
{
    Queued,
    ReleasedImmediately,
    Invalid,
    ParentEncounterInactive,
    ParentTerminalRejected,
    ChildActionNotRegistered,
    ChildLifetimeDisabled,
    DuplicateParent,
    Full,
    PendingQueueFull,
    Expired
};

// Actions call this before initiating their root Core Spell. The request is
// value-only and is released only by the matching successful Core terminal.
class ActionContinuationSink
{
public:
    virtual ~ActionContinuationSink() {}
    virtual ActionContinuationEnqueueResult QueueActionContinuation(
        DecisionContext const& context,
        ActionCandidate const& parentCandidate,
        ActionCandidate const& continuationCandidate,
        std::uint32_t lifetimeMs) const = 0;
};
} // namespace PartyBotEngine

#endif
