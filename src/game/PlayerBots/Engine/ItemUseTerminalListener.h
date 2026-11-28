#ifndef MANGOS_PARTYBOT_ITEM_USE_TERMINAL_LISTENER_H
#define MANGOS_PARTYBOT_ITEM_USE_TERMINAL_LISTENER_H

#include "ItemUseAggregate.h"

namespace PartyBotEngine
{
class ItemUseTerminalListener
{
public:
    virtual ~ItemUseTerminalListener() {}
    virtual void OnItemUseTerminal(ItemUseTerminalEvent const& event) = 0;
};
} // namespace PartyBotEngine

#endif
