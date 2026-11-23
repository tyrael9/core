#include "ProductionStrategies.h"
#include "FollowPartyLeaderTrigger.h"
#include "FollowPartyMemberAction.h"
#include "PartyBotEngine.h"
#include <memory>
namespace PartyBotEngine
{
bool RegisterInitialNewEngineStrategies(PartyBotEngine& engine)
{
    if (!engine.RegisterAction(std::shared_ptr<Action>(new FollowPartyMemberAction())))
        return false;
    std::shared_ptr<Strategy> strategy(new Strategy("new_engine_party_movement"));
    strategy->AddTrigger(std::shared_ptr<Trigger>(new FollowPartyLeaderTrigger()));
    engine.AddStrategy(strategy);
    return true;
}
}
