#ifndef MANGOS_PARTYBOT_HEAL_RESERVATION_TERMINAL_LISTENER_H
#define MANGOS_PARTYBOT_HEAL_RESERVATION_TERMINAL_LISTENER_H
#include "CoreSpellTerminalListener.h"
#include "PartyCoordinator.h"
#include <memory>
namespace PartyBotEngine {
class HealReservationTerminalListener final : public CoreSpellTerminalListener {
public:
    HealReservationTerminalListener(Guid ownerGuid,
        std::shared_ptr<PartyCoordinator> const& coordinator)
        : m_ownerGuid(ownerGuid), m_coordinator(coordinator) {}
    void SetCoordinator(std::shared_ptr<PartyCoordinator> const& coordinator) {
        m_coordinator = coordinator;
    }
    void OnCoreSpellTerminal(CoreSpellTerminalEvent const& event) override {
        if (!m_ownerGuid || !event.actionId || !event.targetGuid || !event.spellId)
            return;
        std::shared_ptr<PartyCoordinator> coordinator = m_coordinator.lock();
        if (!coordinator || (event.encounterId &&
            coordinator->GetEncounterId() != event.encounterId))
            return;
        coordinator->ReleaseReservation(ReservationType::Heal, event.targetGuid,
            m_ownerGuid, event.actionId, event.spellId);
    }
private:
    Guid m_ownerGuid = 0;
    std::weak_ptr<PartyCoordinator> m_coordinator;
};
}
#endif
