#ifndef MANGOS_PARTYBOT_SHADOW_STRATEGIES_H
#define MANGOS_PARTYBOT_SHADOW_STRATEGIES_H

namespace PartyBotEngine {
class PartyBotEngine;

// Register candidate-only role strategies. Their Action::Execute implementations
// fail closed; Shadow runtime must call Evaluate only and never ExecuteSelected.
void RegisterDefaultShadowStrategies(PartyBotEngine& engine);

} // namespace PartyBotEngine

#endif
