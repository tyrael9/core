#include "ProductionStrategies.h"
#include "FollowPartyLeaderTrigger.h"
#include "FollowPartyMemberAction.h"
#include "StopFollowingAction.h"
#include "StopFollowingUnsafeTrigger.h"
#include "PartyBotEngine.h"
#include <memory>
namespace PartyBotEngine
{
bool RegisterInitialNewEngineStrategies(PartyBotEngine& engine)
{
    if (!engine.RegisterAction(std::shared_ptr<Action>(new FollowPartyMemberAction())))
        return false;
    if (!engine.RegisterAction(std::shared_ptr<Action>(new StopFollowingAction())))
        return false;
    std::shared_ptr<Strategy> strategy(new Strategy("new_engine_party_movement"));
    strategy->AddTrigger(std::shared_ptr<Trigger>(new FollowPartyLeaderTrigger()));
    strategy->AddTrigger(std::shared_ptr<Trigger>(new StopFollowingUnsafeTrigger()));
    engine.AddStrategy(strategy);
    return true;
}
}
