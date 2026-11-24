#ifndef MANGOS_PARTYBOT_THREAT_RESERVATION_TERMINAL_LISTENER_H
#define MANGOS_PARTYBOT_THREAT_RESERVATION_TERMINAL_LISTENER_H

#include "CoreSpellTerminalListener.h"
#include "PartyCoordinator.h"
#include <memory>

namespace PartyBotEngine
{
// Releases only this bot's in-flight threat rows when the matching Core Spell
// reaches any terminal state. The weak coordinator binding follows map-shard
// changes without retaining a shard or allowing stale terminals to affect a
// different encounter.
class ThreatReservationTerminalListener final : public CoreSpellTerminalListener
{
public:
    ThreatReservationTerminalListener(
        Guid ownerGuid, std::shared_ptr<PartyCoordinator> const& coordinator)
        : m_ownerGuid(ownerGuid), m_coordinator(coordinator)
    {
    }

    void SetCoordinator(std::shared_ptr<PartyCoordinator> const& coordinator)
    {
        m_coordinator = coordinator;
    }

    void OnCoreSpellTerminal(CoreSpellTerminalEvent const& event) override
    {
        if (!m_ownerGuid || !event.actionId || !event.encounterId)
            return;

        std::shared_ptr<PartyCoordinator> coordinator = m_coordinator.lock();
        if (!coordinator || coordinator->GetEncounterId() != event.encounterId)
            return;

        coordinator->ReleaseThreatAction(event.actionId, m_ownerGuid);
    }

private:
    Guid m_ownerGuid = 0;
    std::weak_ptr<PartyCoordinator> m_coordinator;
};
} // namespace PartyBotEngine

#endif
