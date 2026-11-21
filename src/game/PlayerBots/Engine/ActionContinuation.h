#ifndef MANGOS_PARTYBOT_ACTION_CONTINUATION_H
#define MANGOS_PARTYBOT_ACTION_CONTINUATION_H

#include "CoreSpellTerminalListener.h"
#include "PointMovementTerminalListener.h"
#include "ItemUseTerminalListener.h"
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

enum class ActionDependencyEventSource : std::uint8_t
{
    CoreSpellTerminal,
    ActionResult,
    PointMovementTerminal,
    ItemUseTerminal
};

// One required result from a prior Engine action. CoreSpellTerminal expresses
// the later Core Spell::finish outcome; ActionResult expresses only the
// synchronous Action::Execute boundary; PointMovementTerminal expresses the
// later, destination-checked PointMovementGenerator result. Sources stay distinct.
struct ActionDependencyPrerequisite
{
    ActionDependencyEventSource source = ActionDependencyEventSource::CoreSpellTerminal;
    ActionId actionId = 0;
    std::uint64_t encounterId = 0;
    Guid targetGuid = 0;
    std::uint32_t spellId = 0;
    CoreSpellTerminalStatus requiredStatus = CoreSpellTerminalStatus::Succeeded;
    std::string expectedActionName;
    ActionResult requiredActionResult = ActionResult::Success;
    bool requiredAttempted = true;
    std::uint32_t pointId = 0;
    PointMovementTerminalStatus requiredMovementStatus = PointMovementTerminalStatus::Reached;
    ItemUseId itemUseId = 0;
    ItemUseAggregateStatus requiredItemUseStatus = ItemUseAggregateStatus::AllSucceeded;
};

// Source compatibility for F34/F35 callers; new generic dependencies should
// prefer ActionDependencyPrerequisite.
using ActionTerminalPrerequisite = ActionDependencyPrerequisite;

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
