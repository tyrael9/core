#ifndef MANGOS_PARTYBOT_EXECUTION_AUTHORITY_H
#define MANGOS_PARTYBOT_EXECUTION_AUTHORITY_H

#include <cstdint>

namespace PartyBotEngine
{
enum class ExecutionAuthorityMode : std::uint8_t
{
    Legacy = 0,
    Shadow = 1,
    NewEngine = 2
};

// Unknown values deliberately resolve to Legacy. Enabling exclusive Engine
// execution must always require the explicit value 2.
inline ExecutionAuthorityMode ResolveExecutionAuthorityMode(std::int32_t configuredValue)
{
    switch (configuredValue)
    {
        case 1: return ExecutionAuthorityMode::Shadow;
        case 2: return ExecutionAuthorityMode::NewEngine;
        case 0:
        default:
            return ExecutionAuthorityMode::Legacy;
    }
}

inline bool ShouldEvaluateEngine(ExecutionAuthorityMode mode,
                                 bool legacyShadowOptIn)
{
    return mode != ExecutionAuthorityMode::Legacy || legacyShadowOptIn;
}

inline bool ShouldRunLegacyPolicy(ExecutionAuthorityMode mode)
{
    return mode != ExecutionAuthorityMode::NewEngine;
}
} // namespace PartyBotEngine

#endif
