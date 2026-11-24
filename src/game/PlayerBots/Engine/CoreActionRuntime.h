#ifndef MANGOS_PARTYBOT_CORE_ACTION_RUNTIME_H
#define MANGOS_PARTYBOT_CORE_ACTION_RUNTIME_H

#include "DecisionTypes.h"

class Map;
class Player;
class Unit;

namespace PartyBotEngine
{
struct DecisionContext;

enum class CoreActionResolveStatus : std::uint8_t
{
    Resolved,
    MissingActor,
    ActorSnapshotMismatch,
    ActorOutOfWorld,
    MissingMap,
    SnapshotMapMismatch,
    SnapshotInstanceMismatch,
    EncounterScopeMismatch,
    TargetNotFound,
    TargetOutOfWorld,
    TargetMapMismatch,
    TargetInstanceMismatch
};

// Raw Core pointers are an ephemeral map-thread view only. CoreAction must not
// retain this object or any contained pointer after its synchronous Execute.
struct CoreActionResolution
{
    CoreActionResolveStatus status = CoreActionResolveStatus::MissingActor;
    Player* actor = nullptr;
    Unit* target = nullptr;
    Map* map = nullptr;
    char const* reason = "core_action_actor_missing";

    bool IsResolved() const { return status == CoreActionResolveStatus::Resolved; }
};

class CoreActionRuntime
{
public:
    virtual ~CoreActionRuntime() {}
    virtual CoreActionResolution ResolveUnitContext(
        DecisionContext const& context, Guid targetGuid) const = 0;

    // Optional host capability used by the Engine's generic group-resurrection
    // action. Core/PartyBotAI owns spellbook and class-specific cast checks.
    virtual std::uint32_t GetResurrectionSpellId() const { return 0; }
    virtual bool CanResurrectGroupMember(
        DecisionContext const&, Guid, std::uint32_t) const { return false; }
    virtual bool StartGroupResurrection(
        DecisionContext const&, Guid, std::uint32_t) { return false; }

    // The initial Priest direct-heal vertical slice is deliberately optional.
    // Hosts that do not implement it remain fail-closed.
    virtual bool CanCastDirectHeal(
        DecisionContext const&, Guid, std::uint32_t) const { return false; }
    virtual bool StartDirectHeal(
        DecisionContext const&, Guid, std::uint32_t) { return false; }
};
}

#endif
