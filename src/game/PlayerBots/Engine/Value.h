#ifndef MANGOS_PARTYBOT_VALUE_H
#define MANGOS_PARTYBOT_VALUE_H
#include "DecisionContext.h"
namespace PartyBotEngine {
template <class T> class Value {
public:
    virtual ~Value() {}
    virtual T Get(DecisionContext const& context) const = 0;
};
}
#endif
