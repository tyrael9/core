#ifndef MANGOS_PARTYBOT_DECISION_CONTEXT_H
#define MANGOS_PARTYBOT_DECISION_CONTEXT_H
#include "PartyCoordinator.h"
#include "PartySnapshot.h"
namespace PartyBotEngine {
struct DecisionContext;

enum class ReservationTraceStage : std::uint8_t {
    BudgetVeto,
    Attempt,
    Result,
    Release
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
class ReservationTraceSink {
public:
    virtual ~ReservationTraceSink() {}
    virtual void RecordReservationEvent(DecisionContext const& context,
                                        ReservationTraceEvent const& event) = 0;
};

struct DecisionContext {
    DecisionContext(PartySnapshot const& partySnapshot, PartyMemberSnapshot const& botSnapshot,
                    PartyCoordinator& partyCoordinator, std::uint32_t timeMs,
                    ReservationTraceSink* traceSink = nullptr)
        : snapshot(partySnapshot), bot(botSnapshot), coordinator(partyCoordinator),
          nowMs(timeMs), reservationTrace(traceSink) {}
    PartySnapshot const& snapshot;
    PartyMemberSnapshot const& bot;
    PartyCoordinator& coordinator;
    std::uint32_t nowMs;
    ReservationTraceSink* reservationTrace;
};
}
#endif
