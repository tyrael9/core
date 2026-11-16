#ifndef MANGOS_PARTYBOT_DECISION_CONTEXT_H
#define MANGOS_PARTYBOT_DECISION_CONTEXT_H
#include "PartyCoordinator.h"
#include "PartySnapshot.h"
namespace PartyBotEngine {
struct DecisionContext {
    DecisionContext(PartySnapshot const& partySnapshot, PartyMemberSnapshot const& botSnapshot,
                    PartyCoordinator& partyCoordinator, std::uint32_t timeMs)
        : snapshot(partySnapshot), bot(botSnapshot), coordinator(partyCoordinator), nowMs(timeMs) {}
    PartySnapshot const& snapshot;
    PartyMemberSnapshot const& bot;
    PartyCoordinator& coordinator;
    std::uint32_t nowMs;
};
}
#endif
