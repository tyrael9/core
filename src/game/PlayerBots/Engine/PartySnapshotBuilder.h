#ifndef MANGOS_PARTYBOT_PARTY_SNAPSHOT_BUILDER_H
#define MANGOS_PARTYBOT_PARTY_SNAPSHOT_BUILDER_H
#include "PartyCoordinator.h"
class Player;
namespace PartyBotEngine {
class PartySnapshotBuilder {
public:
    // Returns a cached snapshot when it is within maxAgeMs, otherwise rebuilds it.
    // The caller must retain the coordinator and use this only on its owning map thread.
    static PartySnapshot const* GetOrBuild(Player* bot, PartyCoordinator& coordinator,
                                           std::uint32_t nowMs, std::uint32_t maxAgeMs);
private:
    static bool Build(Player* bot, PartySnapshot& snapshot,
                      std::uint32_t epoch, std::uint32_t nowMs);
};
}
#endif
