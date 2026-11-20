#ifndef MANGOS_PARTYBOT_ACTION_CONTINUATION_H
#define MANGOS_PARTYBOT_ACTION_CONTINUATION_H

#include "CoreSpellTerminalListener.h"
#include "DecisionTypes.h"
#include <cstdint>
#include <vector>

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

// One required terminal outcome from a prior Engine-owned Core Spell.
struct ActionTerminalPrerequisite
{
    ActionId actionId = 0;
    std::uint64_t encounterId = 0;
    Guid targetGuid = 0;
    std::uint32_t spellId = 0;
    CoreSpellTerminalStatus requiredStatus = CoreSpellTerminalStatus::Succeeded;
};

// Multiple prerequisites are ANDed. A non-zero alternativeGroupId makes
// this candidate one branch in a bounded one-of-N group. All N branches must
// be registered and all their prerequisite terminals observed before choice.
struct ActionDependencyRequest
{
    std::vector<ActionTerminalPrerequisite> prerequisites;
    std::uint32_t lifetimeMs = 0;
    std::uint64_t alternativeGroupId = 0;
    std::uint8_t alternativeIndex = 0;
    std::uint8_t alternativeCount = 0;
};

class ActionContinuationSink
{
public:
    virtual ~ActionContinuationSink() {}
    virtual ActionContinuationEnqueueResult QueueActionDependency(
        DecisionContext const& context,
        ActionDependencyRequest const& request,
        ActionCandidate const& continuationCandidate) const = 0;
    // F34 convenience: a single successful terminal prerequisite, no alternatives.
    virtual ActionContinuationEnqueueResult QueueActionContinuation(
        DecisionContext const& context,
        ActionCandidate const& parentCandidate,
        ActionCandidate const& continuationCandidate,
        std::uint32_t lifetimeMs) const = 0;
};
} // namespace PartyBotEngine

#endif
