#ifndef MANGOS_PARTYBOT_POINT_MOVEMENT_TERMINAL_LISTENER_H
#define MANGOS_PARTYBOT_POINT_MOVEMENT_TERMINAL_LISTENER_H

#include "DecisionTypes.h"
#include <cstdint>

namespace PartyBotEngine
{
enum class PointMovementTerminalStatus : std::uint8_t
{
    Reached,
    Interrupted,
    Failed
};

// A point-movement terminal is distinct from the synchronous action result.
// Reached is emitted only after natural spline completion and destination check.
struct PointMovementTerminalEvent
{
    ActionId actionId = 0;
    std::uint64_t encounterId = 0;
    Guid moverGuid = 0;
    std::uint32_t pointId = 0;
    PointMovementTerminalStatus status = PointMovementTerminalStatus::Failed;
    std::uint32_t occurredAtMs = 0;
    float finalDistance = 0.0f;
};

class PointMovementTerminalListener
{
public:
    virtual ~PointMovementTerminalListener() {}
    virtual void OnPointMovementTerminal(PointMovementTerminalEvent const& event) = 0;
};
} // namespace PartyBotEngine

#endif