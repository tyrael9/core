#ifndef MANGOS_PARTYBOT_CORE_SPELL_TERMINAL_LISTENER_H
#define MANGOS_PARTYBOT_CORE_SPELL_TERMINAL_LISTENER_H
#include <cstdint>
namespace PartyBotEngine {
enum class CoreSpellTerminalStatus : std::uint8_t { Succeeded, Failed, Interrupted };
// Final Core Spell::finish outcome; separate from synchronous Action::Execute.
struct CoreSpellTerminalEvent {
    std::uint64_t actionId = 0;
    std::uint64_t encounterId = 0;
    std::uint64_t targetGuid = 0;
    std::uint32_t spellId = 0;
    std::uint32_t occurredAtMs = 0;
    CoreSpellTerminalStatus status = CoreSpellTerminalStatus::Failed;
};
class CoreSpellTerminalListener {
public:
    virtual ~CoreSpellTerminalListener() {}
    virtual void OnCoreSpellTerminal(CoreSpellTerminalEvent const& event) = 0;
};
}
#endif
