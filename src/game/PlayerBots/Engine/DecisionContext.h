#ifndef MANGOS_PARTYBOT_DECISION_CONTEXT_H
#define MANGOS_PARTYBOT_DECISION_CONTEXT_H
#include "PartyCoordinator.h"
#include "PartySnapshot.h"
#include "BotBehaviorState.h"
#include "CoreActionRuntime.h"
namespace PartyBotEngine {
struct DecisionContext;
class ActionContinuationSink;
class ValueCache;

enum class ReservationTraceStage : std::uint8_t {
    BudgetVeto,
    Attempt,
    Result,
    Release
};

enum class EngineActionTraceStage : std::uint8_t {
    Attempt,
    Result
};

struct EngineActionTraceEvent {
    EngineActionTraceStage stage = EngineActionTraceStage::Attempt;
    ActionId actionId = 0;
    char const* actionName = "none";
    Guid targetGuid = 0;
    std::uint32_t spellId = 0;
    ActionChannel channel = ActionChannel::GlobalCooldown;
    float priority = 0.0f;
    ActionResult result = ActionResult::NotAttempted;
    bool attempted = false;
    char const* reason = "none";
};

struct ReservationTraceEvent {
    ReservationTraceStage stage = ReservationTraceStage::Attempt;
    ThreatActionId actionId = 0;
    Guid ownerGuid = 0;
    Guid threatOwnerGuid = 0;
    Guid hostileGuid = 0;
    float threatPoints = 0.0f;
    float rawHeadroom = 0.0f;
    float alreadyReserved = 0.0f;
    std::uint32_t expiresAtMs = 0;
    std::uint32_t reservationRows = 0;
    bool accepted = false;
    char const* reason = "none";
};

// Optional observer. The Strategy/Shadow path leaves this null; the executor
// reports only coordinator outcomes that actually happened.
class EngineActionObserver {
public:
    virtual ~EngineActionObserver() {}
    // Host callback runs synchronously around Action::Execute. It may bind the
    // stable action ID to a Core spell lifecycle but must not perform gameplay.
    virtual void OnEngineActionEvent(DecisionContext const& context,
                                     EngineActionTraceEvent const& event) = 0;
};

class ReservationTraceSink {
public:
    virtual ~ReservationTraceSink() {}
    virtual void RecordReservationEvent(DecisionContext const& context,
                                        ReservationTraceEvent const& event) = 0;
    // Reports the Engine adapter call boundary and its synchronous Action::Execute
    // result. The observer must not interpret this as proof of a final Core effect.
    virtual void RecordEngineActionEvent(DecisionContext const&,
                                         EngineActionTraceEvent const&) {}
};

struct DecisionContext {
    DecisionContext(PartySnapshot const& partySnapshot, PartyMemberSnapshot const& botSnapshot,
                    PartyCoordinator& partyCoordinator, std::uint32_t timeMs,
                    ReservationTraceSink* traceSink = nullptr,
                    ValueCache* decisionValueCache = nullptr,
                    EngineActionObserver* actionObserver = nullptr,
                    ActionContinuationSink const* continuationSink = nullptr,
                    CoreActionRuntime* actionRuntime = nullptr,
                    BotBehaviorState resolvedBehaviorState = BotBehaviorState::Unknown,
                    std::uint32_t resolvedBehaviorStateElapsedMs = 0)
        : snapshot(partySnapshot), bot(botSnapshot), coordinator(partyCoordinator),
          nowMs(timeMs), reservationTrace(traceSink), valueCache(decisionValueCache),
          engineActionObserver(actionObserver), continuationSink(continuationSink),
          coreActionRuntime(actionRuntime),
          botBehaviorState(resolvedBehaviorState == BotBehaviorState::Unknown
              ? ResolveBotBehaviorState(partySnapshot, botSnapshot) : resolvedBehaviorState),
          botBehaviorStateElapsedMs(resolvedBehaviorStateElapsedMs) {}
    PartySnapshot const& snapshot;
    PartyMemberSnapshot const& bot;
    PartyCoordinator& coordinator;
    std::uint32_t nowMs;
    ReservationTraceSink* reservationTrace;
    ValueCache* valueCache;
    EngineActionObserver* engineActionObserver;
    ActionContinuationSink const* continuationSink;
    // Map-thread host resolver. Core object pointers are never stored in snapshots/queues.
    CoreActionRuntime* coreActionRuntime;
    BotBehaviorState botBehaviorState;
    std::uint32_t botBehaviorStateElapsedMs;
};
}
#endif
