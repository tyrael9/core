#include "CoreCombatSimulator.h"

#include "AI/CreatureAI.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "Movement/TargetedMovementGenerator.h"
#include "Movement/MovementGenerator.h"
#include "Movement/TargetedMovementGenerator.h"
#include "PlayerBots/Engine/Action.h"
#include "PlayerBots/Engine/CoreAction.h"
#include "PlayerBots/Engine/SelectHostileTargetAction.h"
#include "PlayerBots/Engine/ClearIdleTargetAction.h"
#include "PlayerBots/Engine/ClearStaleTargetAction.h"
#include "PlayerBots/Engine/StopUnattackableTargetAction.h"
#include "PlayerBots/Engine/FollowPartyMemberAction.h"
#include "PlayerBots/Engine/Formation.h"
#include "PlayerBots/Engine/FaceTargetAction.h"
#include "PlayerBots/Engine/StopFollowingAction.h"
#include "PlayerBots/Engine/AttackTargetAction.h"
#include "PlayerBots/Engine/DpsAssistTrigger.h"
#include "PlayerBots/Engine/ChaseTargetAction.h"
#include "PlayerBots/Engine/StopAutoAttackAction.h"
#include "PlayerBots/Engine/DismountForCombatAction.h"
#include "PlayerBots/Engine/MovePointAction.h"
#include "PlayerBots/Engine/StopPointMovementAction.h"
#include "PlayerBots/Engine/StopStaleChaseAction.h"
#include "PlayerBots/Engine/StopChaseAction.h"
#include "PlayerBots/Engine/MoveDistanceAction.h"
#include "PlayerBots/Engine/CancelOwnCastAction.h"
#include "PlayerBots/Engine/PartyBotEngine.h"
#include "PlayerBots/Engine/ExecutionAuthority.h"
#include "PlayerBots/Engine/PartySnapshotBuilder.h"
#include "PlayerBots/Engine/PartyCoordinator.h"
#include "PlayerBots/Engine/PartySnapshot.h"
#include "PlayerBots/Engine/CoreThreatImpactEstimator.h"
#include "PlayerBots/Engine/ThreatBudgetPolicy.h"
#include "PlayerBots/Engine/Strategy.h"
#include "PlayerBots/Engine/Trigger.h"
#include "PlayerBots/Engine/ShadowStrategies.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "AI/PlayerAI.h"
#include "PlayerBots/PartyBotAI.h"
#include "Spell.h"
#include "SpellCastTargetsInfo.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace PartyBotEngine
{
namespace
{
class SimulatorProbeAI final : public CreatureAI
{
public:
    SimulatorProbeAI(Creature* creature, uint32& updateCount)
        : CreatureAI(creature), m_updateCount(updateCount)
    {
    }

    void UpdateAI(uint32 const /*diff*/) override
    {
        ++m_updateCount;
    }

private:
    uint32& m_updateCount;
};

class SimulatorAliveStateObserver final : public PlayerAI
{
public:
    explicit SimulatorAliveStateObserver(Player* player) : PlayerAI(player) {}

    void OnAliveStateChanged(bool wasAlive, bool isAlive) override
    {
        ++callbackCount;
        if (wasAlive == isAlive)
        {
            ++sameStateCallbacks;
            return;
        }

        ++transitionCount;
        lastWasAlive = wasAlive;
        lastIsAlive = isAlive;
    }

    uint32 callbackCount = 0;
    uint32 sameStateCallbacks = 0;
    uint32 transitionCount = 0;
    bool lastWasAlive = false;
    bool lastIsAlive = false;
};

struct SimulatorActor
{
    Creature* creature = nullptr;
    uint32 updateCount = 0;
};

ThreatImpactEstimate MakeSimulatorNoThreatEstimate(DecisionContext const& context,
                                                     char const* reason)
{
    ThreatImpactEstimate estimate;
    estimate.completeness = ThreatImpactCompleteness::NoThreat;
    estimate.snapshotEpoch = context.snapshot.epoch;
    estimate.reason = reason;
    return estimate;
}

ThreatImpactEstimate EstimateSimulatorHealThreat(DecisionContext const& context)
{
    // The isolated heal fixtures intentionally contain no hostile snapshot
    // entries. If any party hostile exists, healing-assist fan-out is not
    // modelled here and must remain Unknown rather than being treated as free.
    if (!context.snapshot.hostiles.empty()) {
        ThreatImpactEstimate estimate;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "simulator_healing_assist_threat_not_modelled";
        return estimate;
    }
    return MakeSimulatorNoThreatEstimate(
        context, "simulator_fixture_has_no_party_hostile_recipients");
}

CreatureInfo const* FindSafeCreatureTemplate()
{
    std::vector<CreatureInfo const*> candidates;
    for (auto const& item : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = item.second.get();
        if (!info || info->entry != item.first || info->rank != CREATURE_ELITE_NORMAL ||
            !info->display_total_probability || !info->display_id[0] ||
            info->npc_flags || info->civilian || info->racial_leader || info->pet_family ||
            info->pet_spell_list_id || info->immunity_flags || info->school_immune_mask ||
            info->mechanic_immune_mask || info->spawn_spell_id || info->totem_spell_id ||
            info->spell_list_id || info->auras || info->script_id || !info->ai_name.empty() ||
            (info->flags_extra & (CREATURE_FLAG_EXTRA_INVISIBLE | CREATURE_FLAG_EXTRA_NO_THREAT_LIST)))
            continue;

        candidates.push_back(info);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](CreatureInfo const* lhs, CreatureInfo const* rhs) { return lhs->entry < rhs->entry; });
    return candidates.empty() ? nullptr : candidates.front();
}

bool FindMutuallyHostileFactions(uint32& firstId, uint32& secondId)
{
    std::vector<FactionTemplateEntry const*> factions;
    for (auto const& item : sObjectMgr.GetFactionTemplateMap())
        if (item.second.ID)
            factions.push_back(&item.second);

    std::sort(factions.begin(), factions.end(),
              [](FactionTemplateEntry const* lhs, FactionTemplateEntry const* rhs) { return lhs->ID < rhs->ID; });

    for (std::size_t i = 0; i < factions.size(); ++i)
    {
        for (std::size_t j = i + 1; j < factions.size(); ++j)
        {
            if (factions[i]->IsHostileTo(*factions[j]) && factions[j]->IsHostileTo(*factions[i]))
            {
                firstId = factions[i]->ID;
                secondId = factions[j]->ID;
                return true;
            }
        }
    }

    return false;
}

bool CreateSimulatorActor(Map& map, CreatureInfo const& info, uint32 factionId,
                          float x, float y, SimulatorActor& actor)
{
    Creature* creature = new Creature();
    CreatureCreatePos position(&map, x, y, 0.0f, 0.0f);
    if (!creature->Create(map.GenerateLocalLowGuid(HIGHGUID_UNIT), position, &info, info.entry))
    {
        delete creature;
        return false;
    }

    // AIM_Initialize is the Core lifecycle entry point for both the default
    // MotionMaster generator and the template-selected AI. Do this before
    // replacing the AI: installing a custom AI before Map::Add makes
    // Creature::AddToWorld skip AIM_Initialize entirely, leaving MotionMaster
    // empty and causing UpdateMotion's !empty() assertion.
    if (!creature->AIM_Initialize())
    {
        delete creature;
        return false;
    }

    // AIM_Initialize created the normal template AI. Replace it explicitly;
    // Creature::SetAI is a raw pointer setter and does not delete the old AI.
    delete creature->AI();
    creature->SetAI(new SimulatorProbeAI(creature, actor.updateCount));

    creature->SetFactionTemplateId(factionId);
    creature->SetMaxHealth(100000);
    creature->SetHealth(100000);

    // A headless map has no player-centered update cells. Active actors are
    // visited by Map::UpdateCells through the Core's normal active-object path.
    creature->SetActiveObjectState(true);
    map.Add(creature);

    actor.creature = creature;
    ObjectGuid const guid = creature->GetObjectGuid();
    if (!creature->IsInWorld() || map.GetCreature(guid) != creature)
    {
        if (creature->IsInWorld())
            map.Remove(creature, false);

        // Never destroy an object while the map might still hold its pointer.
        bool const detached = !creature->IsInWorld() && map.GetCreature(guid) == nullptr;
        if (detached)
        {
            delete creature;
            actor.creature = nullptr;
        }
        return false;
    }

    return true;
}

bool DestroySimulatorActor(Map& map, SimulatorActor& actor)
{
    if (!actor.creature)
        return true;

    ObjectGuid const guid = actor.creature->GetObjectGuid();
    actor.creature->CombatStop(true);
    actor.creature->DeleteThreatList();

    if (actor.creature->IsInWorld())
        map.Remove(actor.creature, false);

    bool const detached = !actor.creature->IsInWorld() && map.GetCreature(guid) == nullptr;
    if (!detached)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Refusing to destroy Creature GUID %u because Map::Remove(false) did not detach it.",
                 actor.creature->GetGUIDLow());
        return false;
    }

    delete actor.creature;
    actor.creature = nullptr;
    return true;
}

bool CreateSimulatorPrimaryPet(Map& map, Player& owner, CreatureInfo const& info,
                               std::unique_ptr<Pet>& pet)
{
    std::unique_ptr<Pet> candidate(new Pet(SUMMON_PET));
    CreatureCreatePos position(
        &map, owner.GetPositionX() + 1.0f, owner.GetPositionY(),
        owner.GetPositionZ(), owner.GetOrientation());

    if (!candidate->Create(
            map.GenerateLocalLowGuid(HIGHGUID_PET), position, &info,
            sObjectMgr.GeneratePetNumber()))
        return false;

    candidate->SetOwnerGuid(owner.GetObjectGuid());
    candidate->SetCreatorGuid(owner.GetObjectGuid());
    candidate->SetFactionTemplateId(owner.GetFactionTemplateId());
    if (!candidate->InitStatsForLevel(owner.GetLevel(), &owner) ||
        !candidate->AIM_Initialize())
        return false;

    candidate->SetActiveObjectState(true);
    owner.SetPet(candidate.get());
    map.Add(static_cast<Creature*>(candidate.get()));

    bool const registered = candidate->IsInWorld() &&
        map.GetPet(candidate->GetObjectGuid()) == candidate.get() &&
        owner.GetPet() == candidate.get();
    if (!registered)
    {
        if (owner.GetPetGuid() == candidate->GetObjectGuid())
            owner.SetPet(nullptr);
        if (candidate->IsInWorld())
        {
            candidate->CleanupsBeforeDelete();
            map.Remove(static_cast<Creature*>(candidate.get()), false);
        }
        return false;
    }

    pet = std::move(candidate);
    return true;
}

bool DestroySimulatorPrimaryPet(Map& map, Player* owner, std::unique_ptr<Pet>& pet)
{
    if (!pet)
        return true;

    Pet* const rawPet = pet.get();
    ObjectGuid const guid = rawPet->GetObjectGuid();
    if (owner && owner->GetPetGuid() == guid)
        owner->SetPet(nullptr);

    if (rawPet->IsInWorld())
    {
        // Delete both directions of the real Core HostileReference links before
        // detaching the Pet. Do not call Unsummon/SavePetToDB in this fixture.
        rawPet->CleanupsBeforeDelete();
        map.Remove(static_cast<Creature*>(rawPet), false);
    }

    bool const detached = !rawPet->IsInWorld() && map.GetPet(guid) == nullptr;
    if (!detached)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Refusing to destroy Pet GUID %u because Map::Remove(false) did not detach it.",
                 rawPet->GetGUIDLow());
        pet.release();
        return false;
    }

    pet.reset();
    return true;
}


class SimulatorThreatBudgetProbeAction final : public Action
{
public:
    explicit SimulatorThreatBudgetProbeAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        Unit* attacker = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* target = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        Creature* hostile = target ? target->ToCreature() : nullptr;
        if (!attacker || !hostile || !attacker->IsAlive() || !hostile->IsAlive() ||
            !attacker->IsHostileTo(hostile) || !hostile->CanHaveThreatList() ||
            !candidate.spellId)
        {
            reason = "threat_budget_probe_actor_or_damage_invalid";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string&) const override
    {
        return true;
    }

    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                                ActionCandidate const& candidate) const override
    {
        Unit* attacker = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* target = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        Creature* hostile = target ? target->ToCreature() : nullptr;

        // This isolated test action has exactly one spell-less direct-damage
        // effect and no proc, pet, periodic, flat-spell or assist side effects.
        // Use the pinned Core ThreatCalcHelper for the forecast; the runtime
        // test applies the same DealDamage input and compares actual threat.
        ThreatImpactEstimate estimate = EstimateAppliedDirectDamageComponent(
            attacker, hostile, candidate.spellId, SPELL_SCHOOL_MASK_NORMAL,
            context.snapshot.epoch, ThreatImpactBound::Exact);
        if (estimate.impacts.size() != 1)
            return estimate;

        estimate.completeness = ThreatImpactCompleteness::Complete;
        estimate.reservationDurationMs = 5000;
        estimate.reason = "isolated_core_spell_less_direct_damage_action";
        return estimate;
    }

    ActionResult Execute(DecisionContext&, ActionCandidate const& candidate) override
    {
        // Model an accepted pending action. The fixture then completes it by
        // calling Core DealDamage and releases this action ID as its completion
        // callback would. Execute itself must not change Core threat or victim.
        m_lastThreatActionId = candidate.threatActionId;
        m_lastDamage = candidate.spellId;
        ++m_executeCount;
        return m_lastThreatActionId && m_lastDamage
            ? ActionResult::Success : ActionResult::Failed;
    }

    ThreatActionId GetLastThreatActionId() const { return m_lastThreatActionId; }
    uint32 GetLastDamage() const { return m_lastDamage; }
    uint32 GetExecuteCount() const { return m_executeCount; }

private:
    Map& m_map;
    std::string m_name = "core_threat_budget_probe";
    ThreatActionId m_lastThreatActionId = 0;
    uint32 m_lastDamage = 0;
    uint32 m_executeCount = 0;
};

class SimulatorThreatBudgetProbeTrigger final : public Trigger
{
public:
    SimulatorThreatBudgetProbeTrigger(Guid hostileGuid, uint32 damage)
        : m_hostileGuid(hostileGuid), m_damage(damage) {}
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && m_hostileGuid && m_damage;
    }
    void CollectCandidates(DecisionContext const&, std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_threat_budget_probe";
        candidate.targetGuid = m_hostileGuid;
        candidate.spellId = m_damage; // Test-only: exact spell-less DealDamage input.
        candidate.basePriority = 100.0f;
        candidate.channel = ActionChannel::GlobalCooldown;
        candidates.push_back(candidate);
    }
private:
    Guid m_hostileGuid = 0;
    uint32 m_damage = 0;
    std::string m_name = "simulator_core_threat_budget_probe";
};

class SimulatorCoreAttackAction final : public Action
{
public:
    explicit SimulatorCoreAttackAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }
    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        // Unit::Attack changes target/motion state only; no damage or threat is generated.
        return MakeSimulatorNoThreatEstimate(context, "simulator_attack_only_changes_victim");
    }
    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        HostileSnapshot const* hostile = context.snapshot.FindHostile(candidate.targetGuid);
        Unit* attacker = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* target = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!hostile || !hostile->alive || !attacker || !target ||
            !attacker->IsAlive() || !target->IsAlive() || !attacker->IsHostileTo(target))
        {
            reason = "core_attack_target_unavailable_or_not_hostile";
            return false;
        }
        return true;
    }
    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        Unit* attacker = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* target = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!attacker || !target || attacker->GetVictim() == target)
        {
            reason = "core_attack_already_active_or_actor_missing";
            return false;
        }
        return true;
    }
    ActionResult Execute(DecisionContext& context, ActionCandidate const& candidate) override
    {
        Unit* attacker = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* target = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!attacker || !target || !attacker->Attack(target, true))
            return ActionResult::Failed;
        return attacker->GetVictim() == target ? ActionResult::Success : ActionResult::Failed;
    }
private:
    Map& m_map;
    std::string m_name = "core_attack";
};

class SimulatorStopUnattackableTargetTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && context.bot.victimGuid != 0 &&
               !context.bot.victimAttackable;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_stop_unattackable_target";
        candidate.basePriority = 45.0f;
        candidate.channel = ActionChannel::Targeting;
        candidates.push_back(candidate);
    }
private:
    std::string m_name = "simulator_stop_unattackable_target";
};

class SimulatorStopAutoAttackTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && context.bot.victimGuid != 0 &&
               context.bot.currentSpellId == 0;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_stop_auto_attack";
        candidate.basePriority = 38.0f;
        candidate.channel = ActionChannel::Utility;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_stop_auto_attack";
};

class SimulatorStopPointMovementTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive &&
            context.bot.pointMovementActionId != 0 &&
            context.bot.pointMovementEncounterId != 0 &&
            context.bot.pointMovementPointId != 0 &&
            context.bot.pointMovementEncounterId ==
                context.coordinator.GetEncounterId();
    }

    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_stop_point_movement";
        candidate.basePriority = 42.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_stop_point_movement";
};

class SimulatorSelectHostileTargetTrigger final : public Trigger
{
public:
    explicit SimulatorSelectHostileTargetTrigger(Guid targetGuid)
        : m_targetGuid(targetGuid) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && m_targetGuid != 0;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_select_hostile_target";
        candidate.targetGuid = m_targetGuid;
        candidate.basePriority = 40.0f;
        candidate.channel = ActionChannel::Targeting;
        candidates.push_back(candidate);
    }

private:
    Guid m_targetGuid = 0;
    std::string m_name = "simulator_select_hostile_target";
};

class SimulatorAttackTargetTrigger final : public Trigger
{
public:
    explicit SimulatorAttackTargetTrigger(Guid targetGuid)
        : m_targetGuid(targetGuid) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && m_targetGuid != 0 &&
               context.bot.selectionGuid == m_targetGuid;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_attack_target";
        candidate.targetGuid = m_targetGuid;
        candidate.basePriority = 35.0f;
        candidate.channel = ActionChannel::Targeting;
        candidates.push_back(candidate);
    }

private:
    Guid m_targetGuid = 0;
    std::string m_name = "simulator_attack_target";
};

class SimulatorChaseTargetTrigger final : public Trigger
{
public:
    explicit SimulatorChaseTargetTrigger(Guid targetGuid)
        : m_targetGuid(targetGuid) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && m_targetGuid != 0 &&
               context.bot.victimGuid == m_targetGuid;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_chase_target";
        candidate.targetGuid = m_targetGuid;
        candidate.basePriority = 34.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    Guid m_targetGuid = 0;
    std::string m_name = "simulator_chase_target";
};

class SimulatorStopStaleChaseTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.online && context.bot.alive &&
            context.bot.chaseTargetGuid != 0 &&
            context.bot.chaseTargetGuid != context.bot.victimGuid;
    }

    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_stop_stale_chase";
        candidate.basePriority = 44.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_stop_stale_chase";
};

class SimulatorMoveDistanceTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.online && context.bot.alive && context.bot.victimGuid != 0;
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_move_distance";
        candidate.targetGuid = context.bot.victimGuid;
        candidate.basePriority = 44.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }
private:
    std::string m_name = "simulator_move_distance";
};

class SimulatorStopChaseTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.online && context.bot.alive &&
            context.bot.victimGuid != 0 &&
            context.bot.chaseTargetGuid == context.bot.victimGuid;
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_stop_chase";
        candidate.targetGuid = context.bot.chaseTargetGuid;
        candidate.basePriority = 43.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }
private:
    std::string m_name = "simulator_stop_chase";
};

class SimulatorDismountForCombatTrigger final : public Trigger
{
public:
    explicit SimulatorDismountForCombatTrigger(Guid targetGuid)
        : m_targetGuid(targetGuid) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && context.bot.mounted &&
               !context.bot.taxiFlying && m_targetGuid != 0 &&
               context.bot.selectionGuid == m_targetGuid;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_dismount_for_combat";
        candidate.targetGuid = m_targetGuid;
        candidate.basePriority = 38.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    Guid m_targetGuid = 0;
    std::string m_name = "simulator_dismount_for_combat";
};

class SimulatorFaceTargetTrigger final : public Trigger
{
public:
    explicit SimulatorFaceTargetTrigger(Guid targetGuid)
        : m_targetGuid(targetGuid) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && m_targetGuid != 0 &&
               context.bot.selectionGuid == m_targetGuid;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_face_target";
        candidate.targetGuid = m_targetGuid;
        candidate.basePriority = 30.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    Guid m_targetGuid = 0;
    std::string m_name = "simulator_face_target";
};

class SimulatorClearIdleTargetTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && context.bot.selectionGuid != 0 &&
               context.bot.victimGuid == 0 && !context.bot.inCombat &&
               context.bot.currentSpellId == 0;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_clear_idle_target";
        candidate.basePriority = 35.0f;
        candidate.channel = ActionChannel::Targeting;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_clear_idle_target";
};

class SimulatorClearStaleTargetTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && context.bot.selectionGuid != 0 &&
               context.bot.selectionGuid != context.bot.victimGuid &&
               (context.bot.inCombat || context.bot.victimGuid != 0);
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_clear_stale_target";
        candidate.basePriority = 38.0f;
        candidate.channel = ActionChannel::Targeting;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_clear_stale_target";
};

class SimulatorFollowPartyMemberTrigger final : public Trigger
{
public:
    explicit SimulatorFollowPartyMemberTrigger(
        Guid targetGuid, std::uint8_t formationSlot = InvalidFormationSlot)
        : m_targetGuid(targetGuid), m_formationSlot(formationSlot) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && m_targetGuid != 0;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_follow_party_member";
        candidate.targetGuid = m_targetGuid;
        candidate.formationSlot = m_formationSlot;
        candidate.basePriority = 50.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    Guid m_targetGuid = 0;
    std::uint8_t m_formationSlot = InvalidFormationSlot;
    std::string m_name = "simulator_follow_party_member";
};

class SimulatorStopFollowingTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && context.bot.followTargetGuid != 0;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "core_stop_following";
        candidate.basePriority = 30.0f;
        candidate.channel = ActionChannel::Movement;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_stop_following";
};

class SimulatorTankAssistTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive &&
               context.bot.roleId == static_cast<std::uint8_t>(PartyRole::Tank);
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (HostileSnapshot const& hostile : context.snapshot.hostiles)
        {
            if (!hostile.alive || hostile.crowdControlProtected)
                continue;
            ActionCandidate candidate;
            candidate.actionName = "core_attack";
            candidate.targetGuid = hostile.guid;
            candidate.basePriority = 100.0f;
            candidate.channel = ActionChannel::Targeting;
            candidates.push_back(candidate);
        }
    }
private:
    std::string m_name = "simulator_tank_assist";
};



static uint32 constexpr SIMULATOR_HEAL_SPELL_ID = 2050; // Lesser Heal (Rank 1), Vanilla 1.12.

class SimulatorCoreHealAction final : public Action
{
public:
    explicit SimulatorCoreHealAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        return EstimateSimulatorHealThreat(context);
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        PartyMemberSnapshot const* patientSnapshot = context.snapshot.FindMember(candidate.targetGuid);
        Unit* caster = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* patient = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(candidate.spellId);
        bool const isHealingSpell = spell &&
            (spell->Effect[0] == SPELL_EFFECT_HEAL ||
             spell->Effect[1] == SPELL_EFFECT_HEAL ||
             spell->Effect[2] == SPELL_EFFECT_HEAL);
        if (!patientSnapshot || !patientSnapshot->alive || !caster || !patient ||
            !caster->IsAlive() || !patient->IsAlive() || !caster->IsFriendlyTo(patient) ||
            !isHealingSpell || patient->GetHealth() >= patient->GetMaxHealth())
        {
            reason = "core_heal_target_or_spell_unavailable";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        Unit* patient = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!patient || patient->GetHealth() >= patient->GetMaxHealth())
        {
            reason = "core_heal_target_already_full_or_missing";
            return false;
        }
        return true;
    }

    ActionResult Execute(DecisionContext& context, ActionCandidate const& candidate) override
    {
        Unit* caster = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* patient = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(candidate.spellId);
        if (!caster || !patient || !spell || !caster->IsAlive() || !patient->IsAlive() ||
            !caster->IsFriendlyTo(patient) || patient->GetHealth() >= patient->GetMaxHealth())
            return ActionResult::Impossible;

        uint32 const healthBefore = patient->GetHealth();
        caster->SetFacingToObject(patient);

        // Triggered=true keeps this headless Creature fixture independent of
        // player spellbook, mana, GCD and cast-time state. The Core still runs
        // the real Spell/SpellEffects pipeline and returns its authoritative
        // SpellCastResult; the test separately requires an actual heal effect.
        SpellCastResult const castResult = caster->CastSpell(patient, spell, true);
        uint32 const healthAfter = patient->GetHealth();
        m_lastCastResult = castResult;
        m_lastHealing = healthAfter > healthBefore ? healthAfter - healthBefore : 0;
        return castResult == SPELL_CAST_OK && healthAfter > healthBefore
            ? ActionResult::Success : ActionResult::Failed;
    }

    SpellCastResult GetLastCastResult() const { return m_lastCastResult; }
    uint32 GetLastHealing() const { return m_lastHealing; }

private:
    Map& m_map;
    std::string m_name = "core_heal";
    SpellCastResult m_lastCastResult = SPELL_FAILED_DONT_REPORT;
    uint32 m_lastHealing = 0;
};

class SimulatorHealerTriageTrigger final : public Trigger
{
public:
    explicit SimulatorHealerTriageTrigger(std::string actionName = "core_heal")
        : m_actionName(std::move(actionName))
    {
    }

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive &&
               context.bot.roleId == static_cast<std::uint8_t>(PartyRole::Healer);
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (PartyMemberSnapshot const& member : context.snapshot.members)
        {
            if (!member.alive || member.guid == context.bot.guid ||
                !member.maxHealth || member.health >= member.maxHealth)
                continue;
            ActionCandidate candidate;
            candidate.actionName = m_actionName;
            candidate.targetGuid = member.guid;
            candidate.spellId = SIMULATOR_HEAL_SPELL_ID;
            candidate.basePriority = 100.0f *
                (1.0f - static_cast<float>(member.health) / member.maxHealth);
            candidate.channel = ActionChannel::GlobalCooldown;
            candidates.push_back(candidate);
        }
    }
private:
    std::string m_name = "simulator_healer_triage";
    std::string m_actionName;
};

class SimulatorTraceProbeTrigger final : public Trigger
{
public:
    SimulatorTraceProbeTrigger(std::string name, uint32 intervalMs)
        : m_name(std::move(name)), m_intervalMs(intervalMs) {}
    std::string const& GetName() const override { return m_name; }
    uint32 GetCheckIntervalMs() const override { return m_intervalMs; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive;
    }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>&) const override {}
private:
    std::string m_name;
    uint32 m_intervalMs = 0;
};

bool RunEngineCoreSpellHealSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor healer;
    SimulatorActor patient;
    bool created = CreateSimulatorActor(map, *info, partyFaction, 0.0f, 4.0f, healer) &&
                   CreateSimulatorActor(map, *info, partyFaction, 1.0f, 4.0f, patient);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, patient);
        cleaned = DestroySimulatorActor(map, healer) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Spell/heal fixture creation failed (cleanup=%u).", cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120 && (!healer.updateCount || !patient.updateCount); ++tick)
    {
        map.Update(50);
        if (!healer.updateCount || !patient.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    healer.creature->SetLevel(60);
    patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);
    uint32 const healthBefore = patient.creature->GetHealth();

    PartySnapshot snapshot;
    snapshot.groupId = 2;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 2;
    snapshot.capturedAtMs = 2000;

    PartyMemberSnapshot healerMember;
    healerMember.guid = healer.creature->GetObjectGuid().GetRawValue();
    healerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    healerMember.mapId = map.GetId();
    healerMember.instanceId = map.GetInstanceId();
    healerMember.health = healer.creature->GetHealth();
    healerMember.maxHealth = healer.creature->GetMaxHealth();
    healerMember.online = true;
    healerMember.alive = healer.creature->IsAlive();
    snapshot.members.push_back(healerMember);

    PartyMemberSnapshot patientMember;
    patientMember.guid = patient.creature->GetObjectGuid().GetRawValue();
    patientMember.roleId = static_cast<std::uint8_t>(PartyRole::Tank);
    patientMember.mapId = map.GetId();
    patientMember.instanceId = map.GetInstanceId();
    patientMember.health = healthBefore;
    patientMember.maxHealth = patient.creature->GetMaxHealth();
    patientMember.online = true;
    patientMember.alive = patient.creature->IsAlive();
    patientMember.isMainTank = true;
    snapshot.members.push_back(patientMember);

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 2000);
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCoreHealAction> healAction(new SimulatorCoreHealAction(map));
    bool const registered = engine.RegisterAction(healAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_healer_triage"));
    strategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>());
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == patientMember.guid &&
        report.candidates[report.selectedIndices[0]].candidate.spellId == SIMULATOR_HEAL_SPELL_ID &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(context, report);

    uint32 const healthAfter = patient.creature->GetHealth();
    bool const executed = !report.selectedIndices.empty() &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed &&
        report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Success &&
        healAction->GetLastCastResult() == SPELL_CAST_OK && healthAfter > healthBefore &&
        healAction->GetLastHealing() == healthAfter - healthBefore;

    bool cleaned = DestroySimulatorActor(map, patient);
    cleaned = DestroySimulatorActor(map, healer) && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine spell/heal: aiReady=%u registered=%u candidates=%u selected=%u castResult=%u health=%u>%u healed=%u cleanup=%u.",
             (healer.updateCount != 0 && patient.updateCount != 0) ? 1u : 0u,
             registered ? 1u : 0u, static_cast<uint32>(report.candidates.size()),
             selected ? 1u : 0u, static_cast<uint32>(healAction->GetLastCastResult()),
             healthBefore, healthAfter, healAction->GetLastHealing(), cleaned ? 1u : 0u);

    if (!registered || !selected || !executed || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine-to-Core spell/heal smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine selected healer triage and the Core Spell pipeline applied a real Lesser Heal effect.");
    return true;
}



// This fixture follows the same Core Player construction sequence as
// PlayerBotAI::SpawnNewPlayer, but keeps its WorldSession local to the test.
bool CreateSimulatorPlayer(Map& map, uint32 factionId, WorldSession& session, Player*& player,
                          float x = 0.0f, float y = 6.0f, float z = 4.0f)
{
    player = new Player(&session);
    uint8 gender = GENDER_MALE;
    uint8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
    Player::SelectRandomAppearance(RACE_HUMAN, gender, hairStyle, hairColor, face, facialHair, skin);
    uint32 const guidLow = sObjectMgr.GeneratePlayerLowGuid();
    std::string const name = sObjectMgr.GenerateFreePlayerName();
    if (!player->Create(guidLow, name, RACE_HUMAN, CLASS_PRIEST, gender,
                        skin, face, hairStyle, hairColor, facialHair))
    {
        delete player;
        player = nullptr;
        return false;
    }

    player->SetLocationMapId(map.GetId());
    player->SetLocationInstanceId(map.GetInstanceId());
    player->SetAutoInstanceSwitch(false);
    player->GetMotionMaster()->Initialize();
    player->Relocate(x, y, z, 0.0f);
    player->SetMap(&map);
    player->SaveRecallPosition();
    player->CreatePacketBroadcaster();
    player->SetFactionTemplateId(factionId);
    player->GiveLevel(60);

    if (!map.Add(player))
    {
        delete player;
        player = nullptr;
        return false;
    }

    sObjectMgr.InsertPlayerInCache(player);
    session.SetPlayer(player);
    sObjectAccessor.AddObject(player);
    player->SetCanModifyStats(true);
    player->UpdateAllStats();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    return player->IsInWorld() && player->HasSpell(SIMULATOR_HEAL_SPELL_ID);
}

class CaptureCoreSpellTerminalListener final : public CoreSpellTerminalListener
{
public:
    void OnCoreSpellTerminal(CoreSpellTerminalEvent const& event) override
    {
        events.push_back(event);
    }
    std::vector<CoreSpellTerminalEvent> events;
};
class CapturePointMovementTerminalListener final : public PointMovementTerminalListener
{
public:
    void OnPointMovementTerminal(PointMovementTerminalEvent const& event) override
    {
        events.push_back(event);
    }
    std::vector<PointMovementTerminalEvent> events;
};

class SimulatorCoreSpellContinuationAction final : public Action
{
public:
    explicit SimulatorCoreSpellContinuationAction(
        uint32& executeCount, std::string name = "core_spell_terminal_followup")
        : m_executeCount(executeCount), m_name(std::move(name)) {}
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetPendingLifetimeMs() const override { return 5000; }
    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        return MakeSimulatorNoThreatEstimate(
            context, "simulator_spell_continuation_has_no_threat");
    }
    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        PartyMemberSnapshot const* target = context.snapshot.FindMember(candidate.targetGuid);
        if (!target || !target->alive || !context.bot.alive)
        {
            reason = "continuation_target_or_owner_unavailable";
            return false;
        }
        return true;
    }
    bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string&) const override
    {
        return true;
    }
    ActionResult Execute(DecisionContext&, ActionCandidate const&) override
    {
        ++m_executeCount;
        return ActionResult::Success;
    }
private:
    uint32& m_executeCount;
    std::string m_name;
};

class SimulatorCorePlayerPointMoveAction final : public CoreAction
{
public:
    SimulatorCorePlayerPointMoveAction(
        uint32 pointId, float arrivalTolerance,
        ActionCandidate continuationCandidate)
        : m_pointId(pointId), m_arrivalTolerance(arrivalTolerance),
          m_continuationCandidate(std::move(continuationCandidate)) {}

    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        return MakeSimulatorNoThreatEstimate(
            context, "simulator_player_point_movement_has_no_combat_effect");
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        CoreActionResolution const resolved = context.coreActionRuntime
            ? context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid)
            : CoreActionResolution();
        Player* mover = resolved.actor;
        if (!resolved.IsResolved())
        {
            reason = resolved.reason;
            return false;
        }
        if (!mover || resolved.target != mover || !mover->IsAlive() ||
            candidate.targetGuid != context.bot.guid || !candidate.hasDestination ||
            !std::isfinite(candidate.destinationX) ||
            !std::isfinite(candidate.destinationY) ||
            !std::isfinite(candidate.destinationZ) ||
            !context.coordinator.HasActiveEncounter() ||
            mover->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE |
                                UNIT_STATE_POSSESSED))
        {
            reason = "simulator_player_point_move_actor_or_destination_invalid";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        CoreActionResolution const resolved = context.coreActionRuntime
            ? context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid)
            : CoreActionResolution();
        Player* mover = resolved.actor;
        if (!resolved.IsResolved())
        {
            reason = resolved.reason;
            return false;
        }
        if (!mover || resolved.target != mover)
        {
            reason = "simulator_player_point_move_actor_missing";
            return false;
        }
        float const dx = mover->GetPositionX() - candidate.destinationX;
        float const dy = mover->GetPositionY() - candidate.destinationY;
        float const dz = mover->GetPositionZ() - candidate.destinationZ;
        if (mover->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE ||
            dx * dx + dy * dy + dz * dz <= m_arrivalTolerance * m_arrivalTolerance)
        {
            reason = "simulator_player_point_move_already_moving_or_at_destination";
            return false;
        }
        return true;
    }

    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const override
    {
        ActionExecutionResult result;
        Player* mover = resolved.actor;
        uint64 const encounterId = context.coordinator.GetEncounterId();
        if (!mover || resolved.target != mover || !candidate.actionId || !encounterId ||
            !context.continuationSink || !candidate.hasDestination ||
            !std::isfinite(m_arrivalTolerance) || m_arrivalTolerance <= 0.0f ||
            !mover->IsAlive() ||
            mover->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE |
                                UNIT_STATE_POSSESSED) ||
            mover->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE ||
            !std::isfinite(candidate.destinationX) ||
            !std::isfinite(candidate.destinationY) ||
            !std::isfinite(candidate.destinationZ)) {
            result.result = ActionResult::Impossible;
            result.reason = "simulator_player_point_move_live_preflight_rejected";
            return result;
        }

        float const dx = mover->GetPositionX() - candidate.destinationX;
        float const dy = mover->GetPositionY() - candidate.destinationY;
        float const dz = mover->GetPositionZ() - candidate.destinationZ;
        if (dx * dx + dy * dy + dz * dz <= m_arrivalTolerance * m_arrivalTolerance) {
            result.result = ActionResult::Useless;
            result.reason = "simulator_player_point_move_destination_already_reached";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "simulator_player_point_move_live_preflight_passed";
        return result;
    }

    ActionExecutionResult ExecuteCore(DecisionContext& context, ActionCandidate const& candidate,
                                      CoreActionResolution const& resolved) override
    {
        Player* mover = resolved.actor;
        uint64 const encounterId = context.coordinator.GetEncounterId();
        ActionExecutionResult result;

        ActionDependencyRequest request;
        request.lifetimeMs = 4000;
        ActionDependencyPrerequisite movement;
        movement.source = ActionDependencyEventSource::PointMovementTerminal;
        movement.actionId = candidate.actionId;
        movement.encounterId = encounterId;
        movement.targetGuid = context.bot.guid;
        movement.pointId = m_pointId;
        movement.requiredMovementStatus = PointMovementTerminalStatus::Reached;
        request.prerequisites.push_back(movement);

        ActionContinuationEnqueueResult const queued =
            context.continuationSink->QueueActionDependency(
                context, request, m_continuationCandidate);
        if (queued != ActionContinuationEnqueueResult::Queued) {
            result.result = ActionResult::Failed;
            result.reason = "simulator_player_point_move_continuation_rejected";
            return result;
        }

        m_started = mover->GetMotionMaster()->MovePointForEngine(
            m_pointId, candidate.destinationX, candidate.destinationY,
            candidate.destinationZ, MOVE_NONE, candidate.actionId, encounterId,
            m_arrivalTolerance);
        result.result = m_started ? ActionResult::Success : ActionResult::Failed;
        result.reason = m_started ? "simulator_player_point_move_started"
                                  : "simulator_player_point_move_start_rejected";
        return result;
    }

    bool DidStartMovement() const { return m_started; }

private:
    uint32 m_pointId;
    float m_arrivalTolerance;
    ActionCandidate m_continuationCandidate;
    std::string m_name = "core_player_point_move_engine";
    bool m_started = false;
};


class SimulatorCorePlayerHealAction final : public Action
{
public:
    explicit SimulatorCorePlayerHealAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }
    void SetContinuationCandidate(ActionCandidate const& candidate)
    {
        m_continuationCandidate = candidate;
        m_registerContinuation = true;
    }
    void DisableContinuationRegistration() { m_registerContinuation = false; }
    void SetActionResultAndSpellDependencyCandidate(ActionCandidate const& candidate)
    {
        m_actionResultAndSpellCandidate = candidate;
        m_registerActionResultAndSpellDependency = true;
    }
    ActionContinuationEnqueueResult GetContinuationResult() const
    {
        return m_continuationResult;
    }
    ActionContinuationEnqueueResult GetActionResultAndSpellDependencyResult() const
    {
        return m_actionResultAndSpellDependencyResult;
    }

    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        return EstimateSimulatorHealThreat(context);
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        Unit* casterUnit = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Player* caster = casterUnit ? casterUnit->ToPlayer() : nullptr;
        PartyMemberSnapshot const* patientSnapshot = context.snapshot.FindMember(candidate.targetGuid);
        Unit* patient = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(candidate.spellId);
        bool const isHealingSpell = spell &&
            (spell->Effect[0] == SPELL_EFFECT_HEAL ||
             spell->Effect[1] == SPELL_EFFECT_HEAL ||
             spell->Effect[2] == SPELL_EFFECT_HEAL);
        if (!caster || !spell || !caster->IsAlive() || !caster->HasSpell(candidate.spellId) ||
            !caster->IsSpellReady(spell) || caster->HasGCD(spell) ||
            caster->IsNonMeleeSpellCasted(false) || !patientSnapshot ||
            !patientSnapshot->alive || !patient || !patient->IsAlive() ||
            !caster->IsFriendlyTo(patient) || !isHealingSpell ||
            patient->GetHealth() >= patient->GetMaxHealth())
        {
            reason = "player_spell_not_known_ready_or_target_invalid";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        Unit* unit = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Player* caster = unit ? unit->ToPlayer() : nullptr;
        Unit* patient = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!caster || !patient || !caster->IsAlive() || !patient->IsAlive() ||
            patient->GetHealth() >= patient->GetMaxHealth() ||
            caster->IsNonMeleeSpellCasted(false))
        {
            reason = "player_heal_no_longer_useful";
            return false;
        }
        return true;
    }

    ActionResult Execute(DecisionContext& context, ActionCandidate const& candidate) override
    {
        if (m_registerContinuation)
        {
            if (!context.continuationSink)
                return ActionResult::Failed;
            m_continuationResult = context.continuationSink->QueueActionContinuation(
                context, candidate, m_continuationCandidate, 4000);
            if (m_continuationResult != ActionContinuationEnqueueResult::Queued &&
                m_continuationResult != ActionContinuationEnqueueResult::ReleasedImmediately)
                return ActionResult::Failed;
        }
        if (m_registerActionResultAndSpellDependency)
        {
            if (!context.continuationSink)
                return ActionResult::Failed;
            ActionDependencyRequest request;
            request.lifetimeMs = 4000;

            ActionDependencyPrerequisite actionResult;
            actionResult.source = ActionDependencyEventSource::ActionResult;
            actionResult.actionId = candidate.actionId;
            actionResult.encounterId = context.coordinator.GetEncounterId();
            actionResult.targetGuid = candidate.targetGuid;
            actionResult.spellId = candidate.spellId;
            actionResult.expectedActionName = candidate.actionName;
            actionResult.requiredActionResult = ActionResult::Success;
            actionResult.requiredAttempted = true;
            request.prerequisites.push_back(actionResult);

            ActionDependencyPrerequisite spellTerminal;
            spellTerminal.source = ActionDependencyEventSource::CoreSpellTerminal;
            spellTerminal.actionId = candidate.actionId;
            spellTerminal.encounterId = context.coordinator.GetEncounterId();
            spellTerminal.targetGuid = candidate.targetGuid;
            spellTerminal.spellId = candidate.spellId;
            spellTerminal.requiredStatus = CoreSpellTerminalStatus::Succeeded;
            request.prerequisites.push_back(spellTerminal);

            m_actionResultAndSpellDependencyResult =
                context.continuationSink->QueueActionDependency(
                    context, request, m_actionResultAndSpellCandidate);
            if (m_actionResultAndSpellDependencyResult !=
                ActionContinuationEnqueueResult::Queued)
                return ActionResult::Failed;
        }

        Unit* unit = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Player* caster = unit ? unit->ToPlayer() : nullptr;
        Unit* patient = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(candidate.spellId);
        if (!caster || !patient || !spell || !caster->IsAlive() || !patient->IsAlive() ||
            !caster->HasSpell(candidate.spellId) || !caster->IsSpellReady(spell) ||
            caster->HasGCD(spell) || caster->IsNonMeleeSpellCasted(false) ||
            !caster->IsFriendlyTo(patient) || patient->GetHealth() >= patient->GetMaxHealth())
            return ActionResult::Impossible;

        caster->SetFacingToObject(patient);
        caster->SetTargetGuid(patient->GetObjectGuid());
        m_manaBefore = caster->GetPower(POWER_MANA);
        m_lastCastResult = caster->CastSpell(patient, spell, false);
        m_manaAfterAccept = caster->GetPower(POWER_MANA);
        m_castPendingAfterAccept = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr;
        m_gcdAfterAccept = caster->HasGCD(spell);
        return m_lastCastResult == SPELL_CAST_OK ? ActionResult::Success : ActionResult::Failed;
    }

    SpellCastResult GetLastCastResult() const { return m_lastCastResult; }
    uint32 GetManaBefore() const { return m_manaBefore; }
    uint32 GetManaAfterAccept() const { return m_manaAfterAccept; }
    bool WasCastPendingAfterAccept() const { return m_castPendingAfterAccept; }
    bool HadGcdAfterAccept() const { return m_gcdAfterAccept; }

private:
    Map& m_map;
    std::string m_name = "core_player_heal";
    SpellCastResult m_lastCastResult = SPELL_FAILED_DONT_REPORT;
    uint32 m_manaBefore = 0;
    uint32 m_manaAfterAccept = 0;
    bool m_castPendingAfterAccept = false;
    bool m_gcdAfterAccept = false;
    ActionCandidate m_continuationCandidate;
    bool m_registerContinuation = false;
    ActionContinuationEnqueueResult m_continuationResult = ActionContinuationEnqueueResult::Invalid;
    ActionCandidate m_actionResultAndSpellCandidate;
    bool m_registerActionResultAndSpellDependency = false;
    ActionContinuationEnqueueResult m_actionResultAndSpellDependencyResult =
        ActionContinuationEnqueueResult::Invalid;
};



class SimulatorFormationTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive &&
            context.bot.roleId != static_cast<std::uint8_t>(PartyRole::Tank) &&
            context.bot.roleId != static_cast<std::uint8_t>(PartyRole::MainTank);
    }

    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (PartyMemberSnapshot const& leader : context.snapshot.members)
        {
            if (!leader.alive || !leader.isMainTank || leader.guid == context.bot.guid ||
                leader.mapId != context.bot.mapId || leader.instanceId != context.bot.instanceId)
                continue;

            float const dx = context.bot.x - leader.x;
            float const dy = context.bot.y - leader.y;
            float const dz = context.bot.z - leader.z;
            float const distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared <= 36.0f)
                continue;

            float const distance = std::sqrt(distanceSquared);
            if (distance <= 0.1f)
                continue;

            constexpr float FORMATION_DISTANCE = 2.5f;
            ActionCandidate candidate;
            candidate.actionName = "core_move_point";
            candidate.targetGuid = leader.guid;
            candidate.destinationX = leader.x + dx / distance * FORMATION_DISTANCE;
            candidate.destinationY = leader.y + dy / distance * FORMATION_DISTANCE;
            candidate.destinationZ = leader.z;
            candidate.hasDestination = true;
            candidate.basePriority = 70.0f;
            candidate.channel = ActionChannel::Movement;
            candidates.push_back(candidate);
            break;
        }
    }
private:
    std::string m_name = "simulator_formation";
};


class SimulatorFollowTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive &&
            context.bot.roleId != static_cast<std::uint8_t>(PartyRole::Tank) &&
            context.bot.roleId != static_cast<std::uint8_t>(PartyRole::MainTank);
    }

    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (PartyMemberSnapshot const& leader : context.snapshot.members)
        {
            if (!leader.alive || !leader.isMainTank || leader.guid == context.bot.guid ||
                leader.mapId != context.bot.mapId || leader.instanceId != context.bot.instanceId)
                continue;

            float const dx = context.bot.x - leader.x;
            float const dy = context.bot.y - leader.y;
            float const dz = context.bot.z - leader.z;
            if (dx * dx + dy * dy + dz * dz <= 36.0f)
                continue;

            ActionCandidate candidate;
            candidate.actionName = "core_follow";
            candidate.targetGuid = leader.guid;
            candidate.basePriority = 72.0f;
            candidate.channel = ActionChannel::Movement;
            candidates.push_back(candidate);
            break;
        }
    }

private:
    std::string m_name = "simulator_follow";
};

class SimulatorCoreFollowAction final : public Action
{
public:
    explicit SimulatorCoreFollowAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        return MakeSimulatorNoThreatEstimate(context, "simulator_follow_has_no_combat_effect");
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        Unit* follower = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* leader = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        PartyMemberSnapshot const* leaderSnapshot = context.snapshot.FindMember(candidate.targetGuid);
        if (!follower || !leader || !leaderSnapshot || !leaderSnapshot->alive ||
            !follower->IsInWorld() || !leader->IsInWorld() ||
            !follower->IsAlive() || !leader->IsAlive() ||
            !follower->IsFriendlyTo(leader) ||
            follower->GetMapId() != leader->GetMapId() ||
            follower->GetInstanceId() != leader->GetInstanceId() ||
            follower->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
        {
            reason = "core_follow_target_or_actor_invalid";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        Unit* follower = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* leader = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!follower || !leader)
        {
            reason = "core_follow_actor_missing";
            return false;
        }

        bool const followAlreadyActive =
            follower->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
            follower->HasUnitState(UNIT_STATE_FOLLOW);
        if (followAlreadyActive && m_lastTargetGuid == candidate.targetGuid)
        {
            reason = "core_follow_same_target_already_active";
            return false;
        }

        // A different Main Tank is a meaningful retarget even when the
        // follower happens to be close to the new leader. Reissuing MoveFollow
        // lets the Core replace the old FollowMovementGenerator target.
        if (!followAlreadyActive)
        {
            float const dx = follower->GetPositionX() - leader->GetPositionX();
            float const dy = follower->GetPositionY() - leader->GetPositionY();
            float const dz = follower->GetPositionZ() - leader->GetPositionZ();
            if (dx * dx + dy * dy + dz * dz <= 25.0f)
            {
                reason = "core_follow_within_formation_range";
                return false;
            }
        }
        return true;
    }

    ActionResult Execute(DecisionContext& context, ActionCandidate const& candidate) override
    {
        m_started = false;
        Unit* follower = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* leader = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!follower || !leader || !follower->IsAlive() || !leader->IsAlive() ||
            !follower->IsFriendlyTo(leader) ||
            follower->GetMapId() != leader->GetMapId() ||
            follower->GetInstanceId() != leader->GetInstanceId() ||
            follower->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
            return ActionResult::Impossible;

        // Delegate continuous target tracking, path generation and movement
        // updates to the pinned Core FollowMovementGenerator. The simulator
        // does not synthesize movement steps or mutate actor positions.
        static float constexpr FORMATION_OFFSET = 2.5f;
        static float constexpr FOLLOW_ANGLE_BEHIND = 3.14159265358979323846f;
        follower->GetMotionMaster()->MoveFollow(leader, FORMATION_OFFSET, FOLLOW_ANGLE_BEHIND);
        m_started = follower->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
                    follower->HasUnitState(UNIT_STATE_FOLLOW);
        if (m_started)
            m_lastTargetGuid = candidate.targetGuid;
        return m_started ? ActionResult::Success : ActionResult::Failed;
    }

    bool DidStartFollow() const { return m_started; }
    uint64 GetLastTargetGuid() const { return m_lastTargetGuid; }

private:
    Map& m_map;
    std::string m_name = "core_follow";
    bool m_started = false;
    uint64 m_lastTargetGuid = 0;
};


class SimulatorCoreMovePointAction final : public Action
{
public:
    explicit SimulatorCoreMovePointAction(Map& map, bool simulateClientRelocation = false)
        : m_map(map), m_simulateClientRelocation(simulateClientRelocation) {}
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        return MakeSimulatorNoThreatEstimate(context, "simulator_movement_has_no_combat_effect");
    }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        Unit* mover = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* leader = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        PartyMemberSnapshot const* leaderSnapshot = context.snapshot.FindMember(candidate.targetGuid);
        if (!candidate.hasDestination || !std::isfinite(candidate.destinationX) ||
            !std::isfinite(candidate.destinationY) || !std::isfinite(candidate.destinationZ) ||
            !mover || !leader || !leaderSnapshot || !leaderSnapshot->alive ||
            !mover->IsInWorld() || !leader->IsInWorld() || !mover->IsAlive() ||
            !leader->IsAlive() || !mover->IsFriendlyTo(leader) ||
            mover->GetMapId() != leader->GetMapId() ||
            mover->GetInstanceId() != leader->GetInstanceId() ||
            mover->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
        {
            reason = "core_move_point_destination_or_actor_invalid";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        Unit* mover = m_map.GetUnit(ObjectGuid(context.bot.guid));
        if (!mover)
        {
            reason = "core_move_point_mover_missing";
            return false;
        }
        float const dx = mover->GetPositionX() - candidate.destinationX;
        float const dy = mover->GetPositionY() - candidate.destinationY;
        float const dz = mover->GetPositionZ() - candidate.destinationZ;
        if (dx * dx + dy * dy + dz * dz <= 1.0f ||
            mover->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            reason = "core_move_point_already_at_destination_or_moving";
            return false;
        }
        return true;
    }

    ActionResult Execute(DecisionContext& context, ActionCandidate const& candidate) override
    {
        Unit* mover = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Unit* leader = m_map.GetUnit(ObjectGuid(candidate.targetGuid));
        if (!mover || !leader || !candidate.hasDestination ||
            !mover->IsAlive() || !leader->IsAlive() ||
            !mover->IsFriendlyTo(leader) ||
            mover->GetMapId() != leader->GetMapId() ||
            mover->GetInstanceId() != leader->GetInstanceId() ||
            mover->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
            return ActionResult::Impossible;

        if (m_simulateClientRelocation)
        {
            // Model the inbound mover-relocation operation handled by
            // WorldSession::HandleMoverRelocation. Player::SetPosition is the
            // Core entry point and owns HandleInterruptsOnMovement; the
            // simulator must not duplicate spell-interruption policy.
            Player* player = mover->ToPlayer();
            if (!player)
                return ActionResult::Impossible;

            float const oldX = player->GetPositionX();
            float const oldY = player->GetPositionY();
            float const oldZ = player->GetPositionZ();
            if (!player->SetPosition(candidate.destinationX, candidate.destinationY,
                                     candidate.destinationZ, player->GetOrientation()))
                return ActionResult::Failed;

            float const dx = player->GetPositionX() - oldX;
            float const dy = player->GetPositionY() - oldY;
            float const dz = player->GetPositionZ() - oldZ;
            m_started = dx * dx + dy * dy + dz * dz > 0.01f;
        }
        else
        {
            // Server-driven point movement remains a Core MotionMaster action.
            // MOVE_NONE avoids a pathfinder dependency; the spline and
            // relocation remain Core-owned.
            mover->GetMotionMaster()->MovePoint(1, candidate.destinationX,
                                                 candidate.destinationY,
                                                 candidate.destinationZ, MOVE_NONE);
            m_started = mover->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
        }
        return m_started ? ActionResult::Success : ActionResult::Failed;
    }

    bool DidStartMovement() const { return m_started; }

private:
    Map& m_map;
    std::string m_name = "core_move_point";
    bool m_simulateClientRelocation = false;
    bool m_started = false;
};



class SimulatorSpellLifecycleProbeAI final : public PlayerAI
{
public:
    explicit SimulatorSpellLifecycleProbeAI(Player* player) : PlayerAI(player) {}

    uint64 OnSpellCastPreparing(Spell const* /*spell*/, uint32 spellId,
                                uint64 coreTargetGuid, uint64& encounterId,
                                uint64& traceTargetGuid, bool& engineAction) override
    {
        encounterId = 0;
        traceTargetGuid = 0;
        engineAction = false;
        // Only the first Spell created by the observed Player::CastSpell call
        // receives the test action ID. Triggered child spells must not inherit it.
        if (m_bound)
            return 0;

        m_bound = true;
        ++prepareCalls;
        preparedSpellId = spellId;
        coreTargetGuidAtPrepare = coreTargetGuid;
        encounterId = expectedEncounterId;
        traceTargetGuid = coreTargetGuid;
        return expectedActionId;
    }

    void OnSpellCastFinished(Spell const* /*spell*/, uint64 actionId,
                             uint64 encounterId, uint32 spellId,
                             uint64 targetGuid, bool success,
                             bool interrupted, bool /*engineAction*/) override
    {
        ++finishCalls;
        finishedActionId = actionId;
        finishedEncounterId = encounterId;
        finishedSpellId = spellId;
        finishedTargetGuid = targetGuid;
        finishedSuccessfully = success;
        finishedInterrupted = interrupted;
    }

    static uint32 const expectedActionId = 0x51a7u;
    static uint64 const expectedEncounterId = 0x50424f54ull;

    uint32 prepareCalls = 0;
    uint32 finishCalls = 0;
    uint32 preparedSpellId = 0;
    uint64 coreTargetGuidAtPrepare = 0;
    uint64 finishedActionId = 0;
    uint64 finishedEncounterId = 0;
    uint32 finishedSpellId = 0;
    uint64 finishedTargetGuid = 0;
    bool finishedSuccessfully = false;
    bool finishedInterrupted = false;

private:
    bool m_bound = false;
};



class SimulatorOwnCastObserverAI final : public PlayerAI
{
public:
    explicit SimulatorOwnCastObserverAI(Player* player) : PlayerAI(player) {}

    uint64 OnSpellCastPreparing(
        Spell const* /*spell*/, uint32 /*spellId*/, uint64 coreTargetGuid,
        uint64& encounterId, uint64& traceTargetGuid,
        bool& engineAction) override
    {
        encounterId = 0x50424f54ull;
        traceTargetGuid = coreTargetGuid;
        engineAction = true;
        ++prepareCount;
        lastPreparedActionId = nextActionId++;
        return lastPreparedActionId;
    }

    void OnSpellCastFinished(
        Spell const* /*spell*/, uint64 actionId, uint64 encounterId,
        uint32 spellId, uint64 targetGuid, bool success,
        bool interrupted, bool /*engineAction*/) override
    {
        ++finishCount;
        lastFinishedActionId = actionId;
        lastFinishedEncounterId = encounterId;
        lastFinishedSpellId = spellId;
        lastFinishedTargetGuid = targetGuid;
        lastFinishedSuccess = success;
        lastFinishedInterrupted = interrupted;
    }

    uint64 nextActionId = 0xabc100ull;
    uint32 prepareCount = 0;
    uint32 finishCount = 0;
    uint64 lastPreparedActionId = 0;
    uint64 lastFinishedActionId = 0;
    uint64 lastFinishedEncounterId = 0;
    uint32 lastFinishedSpellId = 0;
    uint64 lastFinishedTargetGuid = 0;
    bool lastFinishedSuccess = false;
    bool lastFinishedInterrupted = false;
};

class SimulatorCoreActionRuntime final : public CoreActionRuntime
{
public:
    explicit SimulatorCoreActionRuntime(Map& map) : m_map(map) {}

    CoreActionResolution ResolveUnitContext(
        DecisionContext const& context, Guid targetGuid) const override
    {
        CoreActionResolution result;
        Unit* actorUnit = m_map.GetUnit(ObjectGuid(context.bot.guid));
        Player* actor = actorUnit ? actorUnit->ToPlayer() : nullptr;
        if (!actor || !actor->IsInWorld())
        {
            result.status = CoreActionResolveStatus::ActorOutOfWorld;
            result.reason = "simulator_core_action_actor_missing";
            return result;
        }
        if (context.snapshot.mapId != m_map.GetId() ||
            context.snapshot.instanceId != m_map.GetInstanceId() ||
            context.bot.mapId != context.snapshot.mapId ||
            context.bot.instanceId != context.snapshot.instanceId)
        {
            result.status = CoreActionResolveStatus::SnapshotMapMismatch;
            result.reason = "simulator_core_action_snapshot_mismatch";
            return result;
        }

        Unit* target = nullptr;
        if (targetGuid)
        {
            target = m_map.GetUnit(ObjectGuid(targetGuid));
            if (!target || !target->IsInWorld() ||
                target->GetMap() != &m_map ||
                target->GetInstanceId() != m_map.GetInstanceId())
            {
                result.status = CoreActionResolveStatus::TargetNotFound;
                result.reason = "simulator_core_action_target_missing";
                return result;
            }
        }

        result.status = CoreActionResolveStatus::Resolved;
        result.actor = actor;
        result.target = target;
        result.map = &m_map;
        result.reason = "simulator_core_action_context_resolved";
        return result;
    }

private:
    Map& m_map;
};

class SimulatorCancelOwnCastTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }

    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.online && context.bot.alive &&
            context.bot.currentSpellEngineAction &&
            context.bot.currentSpellActionId &&
            context.bot.currentSpellId &&
            (context.bot.currentSpellType == CURRENT_GENERIC_SPELL ||
             context.bot.currentSpellType == CURRENT_CHANNELED_SPELL);
    }

    void CollectCandidates(
        DecisionContext const& context,
        std::vector<ActionCandidate>& candidates) const override
    {
        if (!IsActive(context))
            return;
        ActionCandidate candidate;
        candidate.actionName = "core_cancel_own_cast";
        candidate.spellId = context.bot.currentSpellId;
        candidate.basePriority = 950.0f;
        candidate.channel = ActionChannel::Utility;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "simulator_cancel_own_cast";
};

bool RunEngineCoreCancelOwnCastActionSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor patient;
    if (!CreateSimulatorActor(map, *info, partyFaction, 8.0f, 6.0f, patient))
        return false;
    patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);

    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = CreateSimulatorPlayer(
        map, partyFaction, botSession, bot, 0.0f, 6.0f, 0.0f);
    if (!botCreated)
    {
        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        else if (bot)
            delete bot;
        DestroySimulatorActor(map, patient);
        return false;
    }

    ObjectGuid const botGuid = bot->GetObjectGuid();
    ObjectGuid const patientGuid = patient.creature->GetObjectGuid();
    SimulatorOwnCastObserverAI spellObserver(bot);
    bot->SetAI(&spellObserver);

    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;
    bool const groupCreated = group->Create(botGuid, bot->GetName());
    if (groupCreated)
    {
        sObjectMgr.AddGroup(group.get());
        groupRegistered = true;
    }

    auto cleanup = [&]() -> bool
    {
        if (bot)
        {
            if (bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                bot->InterruptSpell(CURRENT_GENERIC_SPELL, true);
            if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                bot->InterruptSpell(CURRENT_CHANNELED_SPELL, true);
            bot->SetAI(nullptr);
        }

        bool ungrouped = true;
        if (group && group->GetId())
        {
            group->UnbindInstance(map.GetId(), false);
            group->Disband(true, botGuid);
            ungrouped = !bot || bot->GetGroup() == nullptr;
            if (groupRegistered)
            {
                sObjectMgr.RemoveGroup(group.get());
                groupRegistered = false;
            }
            group.reset();
        }

        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        bool const sessionCleaned = !botSession.GetPlayer();
        bool const playerDetached = !botGuid || map.GetUnit(botGuid) == nullptr;
        bot = nullptr;
        bool const patientCleaned = DestroySimulatorActor(map, patient);
        return ungrouped && sessionCleaned && playerDetached && patientCleaned;
    };

    if (!groupCreated || !groupRegistered)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] CancelOwnCast Group setup failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    PartyCoordinator coordinator;
    uint32 snapshotTime = WorldTimer::getMSTime();
    PartySnapshot firstCastSnapshot;
    SpellCastResult const firstCastResult =
        bot->CastSpell(patient.creature, SIMULATOR_HEAL_SPELL_ID, false);
    uint64 const firstActionId = spellObserver.lastPreparedActionId;
    PartySnapshot const* firstBuilt = PartySnapshotBuilder::GetOrBuild(
        bot, coordinator, snapshotTime + 1, 0);
    if (firstBuilt)
        firstCastSnapshot = *firstBuilt;
    PartyMemberSnapshot const* firstBot =
        firstCastSnapshot.FindMember(botGuid.GetRawValue());
    Spell* firstCurrentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    bool const firstCastCaptured = firstCastResult == SPELL_CAST_OK &&
        spellObserver.prepareCount == 1 && firstActionId &&
        firstCurrentSpell &&
        firstCurrentSpell->getState() == SPELL_STATE_PREPARING &&
        firstBot &&
        firstBot->currentSpellId == SIMULATOR_HEAL_SPELL_ID &&
        firstBot->currentSpellType == CURRENT_GENERIC_SPELL &&
        firstBot->currentSpellActionId == firstActionId &&
        firstBot->currentSpellEngineAction &&
        firstBot->currentSpellEncounterId == 0x50424f54ull &&
        firstBot->currentSpellTargetGuid == patientGuid.GetRawValue() &&
        firstBot->currentSpellTraceTargetGuid == patientGuid.GetRawValue();

    bot->InterruptSpell(CURRENT_GENERIC_SPELL, true);
    bool const firstTerminalCaptured = spellObserver.finishCount == 1 &&
        spellObserver.lastFinishedActionId == firstActionId &&
        spellObserver.lastFinishedSpellId == SIMULATOR_HEAL_SPELL_ID &&
        spellObserver.lastFinishedInterrupted && !spellObserver.lastFinishedSuccess &&
        bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;

    SpellCastResult const replacementCastResult =
        bot->CastSpell(patient.creature, SIMULATOR_HEAL_SPELL_ID, false);
    uint64 const replacementActionId = spellObserver.lastPreparedActionId;
    snapshotTime += 2;
    PartySnapshot replacementSnapshot;
    PartySnapshot const* replacementBuilt = PartySnapshotBuilder::GetOrBuild(
        bot, coordinator, snapshotTime, 0);
    if (replacementBuilt)
        replacementSnapshot = *replacementBuilt;
    PartyMemberSnapshot const* replacementBot =
        replacementSnapshot.FindMember(botGuid.GetRawValue());
    Spell* replacementSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    bool const replacementCaptured = replacementCastResult == SPELL_CAST_OK &&
        spellObserver.prepareCount == 2 && replacementActionId != firstActionId &&
        replacementSpell &&
        replacementSpell->getState() == SPELL_STATE_PREPARING &&
        replacementBot &&
        replacementBot->currentSpellId == SIMULATOR_HEAL_SPELL_ID &&
        replacementBot->currentSpellType == CURRENT_GENERIC_SPELL &&
        replacementBot->currentSpellActionId == replacementActionId &&
        replacementBot->currentSpellEngineAction;

    PartyBotEngine engine;
    std::shared_ptr<CancelOwnCastAction> cancelAction(
        new CancelOwnCastAction());
    bool const registered = engine.RegisterAction(cancelAction);
    std::shared_ptr<Strategy> strategy(
        new Strategy("simulator_cancel_own_cast"));
    strategy->AddTrigger(std::make_shared<SimulatorCancelOwnCastTrigger>());
    engine.AddStrategy(strategy);
    SimulatorCoreActionRuntime actionRuntime(map);

    ActionCandidate estimateCandidate;
    PartyMemberSnapshot delayedEstimateMember = *replacementBot;
    delayedEstimateMember.currentSpellState = SPELL_STATE_DELAYED;
    DecisionContext delayedEstimateContext(
        replacementSnapshot, delayedEstimateMember, coordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
        &actionRuntime);
    DecisionReport delayedEstimateReport = engine.Evaluate(delayedEstimateContext);
    bool const delayedFailsClosed =
        delayedEstimateReport.selectedIndices.empty() &&
        delayedEstimateReport.candidates.size() == 1 &&
        delayedEstimateReport.candidates[0].status ==
            CandidateStatus::RejectedThreatUnknown &&
        delayedEstimateReport.candidates[0].reason ==
            "threat_impact_estimate_unknown";

    ActionCandidate staleStateCandidate;
    staleStateCandidate.actionId = 0xcace1001ull;
    staleStateCandidate.spellId = SIMULATOR_HEAL_SPELL_ID;
    ActionExecutionResult const delayedStateResult =
        cancelAction->ExecuteDetailed(delayedEstimateContext, staleStateCandidate);
    bool const delayedStateDriftRejected =
        delayedStateResult.result == ActionResult::Impossible &&
        delayedStateResult.reason == "cancel_own_cast_live_spell_state_changed" &&
        spellObserver.finishCount == 1;

    PartyMemberSnapshot castingEstimateMember = *replacementBot;
    castingEstimateMember.currentSpellState = SPELL_STATE_CASTING;
    DecisionContext castingEstimateContext(
        replacementSnapshot, castingEstimateMember, coordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
        &actionRuntime);
    DecisionReport castingEstimateReport = engine.Evaluate(castingEstimateContext);
    bool const castingFailsClosed =
        castingEstimateReport.selectedIndices.empty() &&
        castingEstimateReport.candidates.size() == 1 &&
        castingEstimateReport.candidates[0].status ==
            CandidateStatus::RejectedThreatUnknown &&
        castingEstimateReport.candidates[0].reason ==
            "threat_impact_estimate_unknown";

    ActionExecutionResult const castingStateResult =
        cancelAction->ExecuteDetailed(castingEstimateContext, staleStateCandidate);
    bool const castingStateDriftRejected =
        castingStateResult.result == ActionResult::Impossible &&
        castingStateResult.reason == "cancel_own_cast_live_spell_state_changed" &&
        spellObserver.finishCount == 1;

    PartyMemberSnapshot preparingEstimateMember = *replacementBot;
    DecisionContext preparingEstimateContext(
        replacementSnapshot, preparingEstimateMember, coordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
        &actionRuntime);
    ThreatImpactEstimate const preparingEstimate =
        cancelAction->EstimateThreatImpact(preparingEstimateContext, estimateCandidate);
    bool const preparingNoThreat =
        preparingEstimate.completeness == ThreatImpactCompleteness::NoThreat &&
        preparingEstimate.reason ==
            "cancel_own_preparing_cast_has_no_applied_spell_effect";

    bool staleSnapshotRejected = false;
    bool replacementPreserved = false;
    if (firstCastCaptured && firstTerminalCaptured && replacementCaptured &&
        firstBot && replacementBot)
    {
        DecisionContext staleContext(
            firstCastSnapshot, *firstBot, coordinator, WorldTimer::getMSTime(),
            nullptr, nullptr, nullptr, nullptr, &actionRuntime);
        DecisionReport staleReport = engine.Evaluate(staleContext);
        bool const staleSelected = staleReport.selectedIndices.size() == 1 &&
            staleReport.candidates[staleReport.selectedIndices[0]].candidate.actionName ==
                "core_cancel_own_cast" &&
            staleReport.candidates[staleReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (staleSelected)
            engine.ExecuteSelected(staleContext, staleReport);
        CandidateEvaluation const* staleEvaluation = staleSelected
            ? &staleReport.candidates[staleReport.selectedIndices[0]] : nullptr;
        staleSnapshotRejected = staleEvaluation &&
            staleEvaluation->status == CandidateStatus::ExecutionFailed &&
            staleEvaluation->actionResult == ActionResult::Impossible &&
            staleEvaluation->reason ==
                "cancel_own_cast_live_spell_identity_mismatch" &&
            spellObserver.finishCount == 1;
        Spell* currentAfterStale = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        replacementPreserved = currentAfterStale &&
            currentAfterStale->GetPartyBotTraceActionId() == replacementActionId;
    }

    bool currentSelected = false;
    bool currentCancelled = false;
    bool repeatedCancelRejected = false;
    bool noEffects = false;
    if (staleSnapshotRejected && replacementPreserved && replacementBot)
    {
        DecisionContext currentContext(
            replacementSnapshot, *replacementBot, coordinator,
            WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
            &actionRuntime);
        DecisionReport currentReport = engine.Evaluate(currentContext);
        currentSelected = currentReport.selectedIndices.size() == 1 &&
            currentReport.candidates[currentReport.selectedIndices[0]].candidate.actionName ==
                "core_cancel_own_cast" &&
            currentReport.candidates[currentReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;

        uint32 const patientHealthBefore = patient.creature->GetHealth();
        uint32 const manaBeforeCancel = bot->GetPower(POWER_MANA);
        if (currentSelected)
            engine.ExecuteSelected(currentContext, currentReport);
        CandidateEvaluation const* currentEvaluation = currentSelected
            ? &currentReport.candidates[currentReport.selectedIndices[0]] : nullptr;
        currentCancelled = currentEvaluation &&
            currentEvaluation->status == CandidateStatus::Executed &&
            currentEvaluation->actionResult == ActionResult::Success &&
            bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
            spellObserver.finishCount == 2 &&
            spellObserver.lastFinishedActionId == replacementActionId &&
            spellObserver.lastFinishedEncounterId == 0x50424f54ull &&
            spellObserver.lastFinishedSpellId == SIMULATOR_HEAL_SPELL_ID &&
            spellObserver.lastFinishedTargetGuid == patientGuid.GetRawValue() &&
            spellObserver.lastFinishedInterrupted &&
            !spellObserver.lastFinishedSuccess;
        noEffects = patient.creature->GetHealth() == patientHealthBefore &&
            bot->GetPower(POWER_MANA) == manaBeforeCancel;

        DecisionReport repeatedReport = engine.Evaluate(currentContext);
        bool const repeatedSelected = repeatedReport.selectedIndices.size() == 1 &&
            repeatedReport.candidates[repeatedReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (repeatedSelected)
            engine.ExecuteSelected(currentContext, repeatedReport);
        CandidateEvaluation const* repeatedEvaluation = repeatedSelected
            ? &repeatedReport.candidates[repeatedReport.selectedIndices[0]] : nullptr;
        repeatedCancelRejected = repeatedEvaluation &&
            repeatedEvaluation->status == CandidateStatus::ExecutionFailed &&
            repeatedEvaluation->actionResult == ActionResult::Impossible &&
            repeatedEvaluation->reason ==
                "cancel_own_cast_live_spell_identity_mismatch" &&
            spellObserver.finishCount == 2 &&
            bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
    }


    // Live Core DELAYED projectile cancellation. Fireball rank 2 (143) is
    // verified by the simulator DBC fixture as a 2s cast with 24 yd/s speed.
    // At 24 yards the projectile remains pending long enough to cancel after
    // Core transitions the Spell to DELAYED but before its impact event.
    bool fireballDelayedObserved = false;
    bool fireballThreatRejected = false;
    bool fireballCancelled = false;
    bool fireballImpactSuppressed = false;
    SpellEntry const* fireballInfo = sSpellMgr.GetSpellEntry(143);
    if (fireballInfo && fireballInfo->speed > 0.0f)
    {
        bot->LearnSpell(143, false);
        patient.creature->SetFactionTemplateId(unusedHostileFaction);
        patient.creature->Relocate(
            24.0f, 6.0f, 0.0f, patient.creature->GetOrientation());
        patient.creature->SetHealth(patient.creature->GetMaxHealth());

        SpellCastResult const fireballCastResult =
            bot->CastSpell(patient.creature, 143, false);
        uint64 const fireballActionId = spellObserver.lastPreparedActionId;
        bool const fireballPrepared =
            fireballCastResult == SPELL_CAST_OK &&
            fireballActionId != 0 &&
            bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr &&
            bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)->getState() ==
                SPELL_STATE_PREPARING;

        for (uint32 tick = 0; fireballPrepared && tick < 40; ++tick)
        {
            Spell* current = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!current)
                break;
            if (current->getState() == SPELL_STATE_DELAYED)
            {
                fireballDelayedObserved = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            map.Update(250);
        }

        PartySnapshot fireballSnapshot;
        PartySnapshot const* fireballBuilt = fireballDelayedObserved
            ? PartySnapshotBuilder::GetOrBuild(
                bot, coordinator, WorldTimer::getMSTime(), 0)
            : nullptr;
        if (fireballBuilt)
            fireballSnapshot = *fireballBuilt;
        bool const fireballSnapshotBuilt = fireballBuilt != nullptr;
        PartyMemberSnapshot const* fireballMember = fireballSnapshotBuilt
            ? fireballSnapshot.FindMember(botGuid.GetRawValue()) : nullptr;
        if (fireballMember)
        {
            DecisionContext fireballContext(
                fireballSnapshot, *fireballMember, coordinator,
                WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
                &actionRuntime);
            DecisionReport fireballReport = engine.Evaluate(fireballContext);
            fireballThreatRejected =
                fireballReport.selectedIndices.empty() &&
                fireballReport.candidates.size() == 1 &&
                fireballReport.candidates[0].status ==
                    CandidateStatus::RejectedThreatUnknown &&
                fireballReport.candidates[0].reason ==
                    "threat_impact_estimate_unknown";

            ActionCandidate fireballCancelCandidate;
            fireballCancelCandidate.actionId = 0xcace1002ull;
            fireballCancelCandidate.spellId = 143;
            uint32 const healthAtCancel = patient.creature->GetHealth();
            ActionExecutionResult const cancelResult =
                cancelAction->ExecuteDetailed(
                    fireballContext, fireballCancelCandidate);
            fireballCancelled =
                cancelResult.result == ActionResult::Success &&
                cancelResult.reason ==
                    "cancel_own_cast_core_interrupted_exact_spell" &&
                bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                spellObserver.finishCount == 3 &&
                spellObserver.lastFinishedActionId == fireballActionId &&
                spellObserver.lastFinishedSpellId == 143 &&
                spellObserver.lastFinishedTargetGuid == patientGuid.GetRawValue() &&
                spellObserver.lastFinishedInterrupted &&
                !spellObserver.lastFinishedSuccess;

            // Wait beyond Fireball's target-relative travel time. A cancelled
            // delayed Spell must not deliver its later direct-damage effect.
            for (uint32 tick = 0; tick < 8; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                map.Update(250);
            }
            fireballImpactSuppressed =
                patient.creature->GetHealth() >= healthAtCancel &&
                !patient.creature->HasAura(143);
        }
    }

    // Live Core CHANNELED/CASTING cancellation. Mind Flay (15407) is a
    // Classic Priest channel. Its normal SpellEvent progression must install
    // CURRENT_CHANNELED_SPELL and the channel aura before the wrapper acts.
    bool mindFlayChannelStarted = false;
    bool mindFlayThreatRejected = false;
    bool mindFlayCancelled = false;
    bool mindFlayAuraRemoved = false;
    bool mindFlayNoFurtherTicks = false;
    SpellEntry const* mindFlayInfo = sSpellMgr.GetSpellEntry(15407);
    if (mindFlayInfo && mindFlayInfo->IsChanneledSpell())
    {
        bot->LearnSpell(15407, false);
        patient.creature->Relocate(
            8.0f, 6.0f, 0.0f, patient.creature->GetOrientation());
        patient.creature->SetFactionTemplateId(unusedHostileFaction);
        bot->InterruptSpell(CURRENT_GENERIC_SPELL, true);
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL, true);

        for (uint32 tick = 0; tick < 24 &&
             (!bot->IsSpellReady(mindFlayInfo) ||
              bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) ||
              bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL)); ++tick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            map.Update(250);
        }

        SpellCastResult const mindFlayCastResult =
            bot->CastSpell(patient.creature, 15407, false);
        uint64 const mindFlayActionId = spellObserver.lastPreparedActionId;
        bool const mindFlayPrepared =
            mindFlayCastResult == SPELL_CAST_OK && mindFlayActionId != 0;
        for (uint32 tick = 0; mindFlayPrepared && tick < 12; ++tick)
        {
            Spell* current = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (current && current->getState() == SPELL_STATE_CASTING &&
                current->IsChannelActive())
            {
                mindFlayChannelStarted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            map.Update(250);
        }

        PartySnapshot mindFlaySnapshot;
        PartySnapshot const* mindFlayBuilt = mindFlayChannelStarted
            ? PartySnapshotBuilder::GetOrBuild(
                bot, coordinator, WorldTimer::getMSTime(), 0)
            : nullptr;
        if (mindFlayBuilt)
            mindFlaySnapshot = *mindFlayBuilt;
        bool const mindFlaySnapshotBuilt = mindFlayBuilt != nullptr;
        PartyMemberSnapshot const* mindFlayMember = mindFlaySnapshotBuilt
            ? mindFlaySnapshot.FindMember(botGuid.GetRawValue()) : nullptr;
        if (mindFlayMember)
        {
            DecisionContext mindFlayContext(
                mindFlaySnapshot, *mindFlayMember, coordinator,
                WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
                &actionRuntime);
            DecisionReport mindFlayReport = engine.Evaluate(mindFlayContext);
            mindFlayThreatRejected =
                mindFlayReport.selectedIndices.empty() &&
                mindFlayReport.candidates.size() == 1 &&
                mindFlayReport.candidates[0].status ==
                    CandidateStatus::RejectedThreatUnknown &&
                mindFlayReport.candidates[0].reason ==
                    "threat_impact_estimate_unknown";

            bool const mindFlayAuraWasActive =
                patient.creature->HasAura(15407);
            uint32 const healthAtCancel = patient.creature->GetHealth();
            ActionCandidate mindFlayCancelCandidate;
            mindFlayCancelCandidate.actionId = 0xcace1003ull;
            mindFlayCancelCandidate.spellId = 15407;
            ActionExecutionResult const cancelResult =
                cancelAction->ExecuteDetailed(
                    mindFlayContext, mindFlayCancelCandidate);
            mindFlayCancelled =
                cancelResult.result == ActionResult::Success &&
                cancelResult.reason ==
                    "cancel_own_cast_core_interrupted_exact_spell" &&
                bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) == nullptr &&
                bot->GetUInt32Value(UNIT_CHANNEL_SPELL) == 0 &&
                spellObserver.finishCount == 4 &&
                spellObserver.lastFinishedActionId == mindFlayActionId &&
                spellObserver.lastFinishedSpellId == 15407 &&
                spellObserver.lastFinishedTargetGuid == patientGuid.GetRawValue() &&
                spellObserver.lastFinishedInterrupted &&
                !spellObserver.lastFinishedSuccess;
            mindFlayAuraRemoved = mindFlayAuraWasActive &&
                !patient.creature->HasAura(15407);

            for (uint32 tick = 0; tick < 8; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                map.Update(250);
            }
            mindFlayNoFurtherTicks =
                patient.creature->GetHealth() >= healthAtCancel &&
                !patient.creature->HasAura(15407);
        }
    }

    bool const beforeCleanup =
        firstCastCaptured && firstTerminalCaptured && replacementCaptured &&
        registered && preparingNoThreat && delayedFailsClosed &&
        castingFailsClosed && delayedStateDriftRejected &&
        castingStateDriftRejected && staleSnapshotRejected && replacementPreserved &&
        currentSelected && currentCancelled && repeatedCancelRejected && noEffects &&
        fireballDelayedObserved && fireballThreatRejected && fireballCancelled &&
        fireballImpactSuppressed && mindFlayChannelStarted &&
        mindFlayThreatRejected && mindFlayCancelled && mindFlayAuraRemoved &&
        mindFlayNoFurtherTicks;
    bool const cleaned = cleanup();

    sLog.Out(LOG_BASIC, beforeCleanup && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] CancelOwnCastAction: snapshotIdentity=%u firstInterrupted=%u replacementIdentity=%u preparingNoThreat=%u delayedUnknown=%u castingUnknown=%u delayedStateDriftRejected=%u castingStateDriftRejected=%u staleRejected=%u replacementPreserved=%u selected=%u cancelled=%u repeatedRejected=%u noEffects=%u fireballDelayed=%u fireballThreatRejected=%u fireballCancelled=%u fireballImpactSuppressed=%u mindFlayChannel=%u mindFlayThreatRejected=%u mindFlayCancelled=%u mindFlayAuraRemoved=%u mindFlayNoFurtherTicks=%u cleanup=%u.",
        firstCastCaptured ? 1u : 0u, firstTerminalCaptured ? 1u : 0u,
        replacementCaptured ? 1u : 0u, preparingNoThreat ? 1u : 0u,
        delayedFailsClosed ? 1u : 0u, castingFailsClosed ? 1u : 0u,
        delayedStateDriftRejected ? 1u : 0u, castingStateDriftRejected ? 1u : 0u,
        staleSnapshotRejected ? 1u : 0u, replacementPreserved ? 1u : 0u,
        currentSelected ? 1u : 0u, currentCancelled ? 1u : 0u,
        repeatedCancelRejected ? 1u : 0u, noEffects ? 1u : 0u,
        fireballDelayedObserved ? 1u : 0u, fireballThreatRejected ? 1u : 0u,
        fireballCancelled ? 1u : 0u, fireballImpactSuppressed ? 1u : 0u,
        mindFlayChannelStarted ? 1u : 0u, mindFlayThreatRejected ? 1u : 0u,
        mindFlayCancelled ? 1u : 0u, mindFlayAuraRemoved ? 1u : 0u,
        mindFlayNoFurtherTicks ? 1u : 0u, cleaned ? 1u : 0u);

    if (!beforeCleanup || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] CancelOwnCastAction Core cancellation fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] PASS: CancelOwnCastAction cancelled exact Engine-owned PREPARING, DELAYED Fireball and CHANNELED Mind Flay Player spells through Core SpellCaster::InterruptSpell, rejected stale/replaced and unknown-threat dispatch, emitted matching Interrupted terminals, suppressed delayed impacts and removed the channel aura.");
    return true;
}

bool RunEngineCoreMovementInterruptSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor patient;
    bool const patientCreated = CreateSimulatorActor(map, *info, partyFaction, 0.0f, 6.0f, patient);
    if (!patientCreated)
    {
        DestroySimulatorActor(map, patient);
        return false;
    }

    WorldSession healerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* healer = nullptr;
    bool const healerCreated = CreateSimulatorPlayer(map, partyFaction, healerSession, healer,
                                                       10.0f, 6.0f, 0.0f);
    if (!healerCreated)
    {
        if (healerSession.GetPlayer())
            healerSession.LogoutPlayer(false);
        else if (healer)
            delete healer;
        DestroySimulatorActor(map, patient);
        return false;
    }

    SimulatorSpellLifecycleProbeAI* lifecycleProbe =
        new SimulatorSpellLifecycleProbeAI(healer);
    healer->SetAI(lifecycleProbe);

    patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);
    uint32 const healthBefore = patient.creature->GetHealth();

    SpellEntry const* const interruptProbeSpell = sSpellMgr.GetSpellEntry(SIMULATOR_HEAL_SPELL_ID);
    if (interruptProbeSpell)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] Player interrupt probe: spell=%u castTime=%u interruptFlags=%u movementFlag=%u auraFlags=%u channelFlags=%u.",
                 SIMULATOR_HEAL_SPELL_ID, interruptProbeSpell->GetCastTime(healer),
                 interruptProbeSpell->InterruptFlags,
                 interruptProbeSpell->HasSpellInterruptFlag(SPELL_INTERRUPT_FLAG_MOVEMENT) ? 1u : 0u,
                 interruptProbeSpell->AuraInterruptFlags,
                 interruptProbeSpell->ChannelInterruptFlags);

    // Prime the real wall-clock Player update cadence before starting the
    // cast. Otherwise a stale m_lastPlayersUpdate can make the first update
    // after MovePoint carry a large diff and complete the spell before the
    // next tick observes the changed position.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    map.Update(250);

    PartySnapshot snapshot;
    snapshot.groupId = 5;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 5;
    snapshot.capturedAtMs = 5000;

    PartyMemberSnapshot healerMember;
    healerMember.guid = healer->GetObjectGuid().GetRawValue();
    healerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    healerMember.mapId = map.GetId();
    healerMember.instanceId = map.GetInstanceId();
    healerMember.health = healer->GetHealth();
    healerMember.maxHealth = healer->GetMaxHealth();
    healerMember.online = true;
    healerMember.alive = healer->IsAlive();
    healerMember.x = healer->GetPositionX();
    healerMember.y = healer->GetPositionY();
    healerMember.z = healer->GetPositionZ();
    snapshot.members.push_back(healerMember);

    PartyMemberSnapshot patientMember;
    patientMember.guid = patient.creature->GetObjectGuid().GetRawValue();
    patientMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
    patientMember.mapId = map.GetId();
    patientMember.instanceId = map.GetInstanceId();
    patientMember.health = healthBefore;
    patientMember.maxHealth = patient.creature->GetMaxHealth();
    patientMember.online = true;
    patientMember.alive = patient.creature->IsAlive();
    patientMember.isMainTank = true;
    patientMember.x = patient.creature->GetPositionX();
    patientMember.y = patient.creature->GetPositionY();
    patientMember.z = patient.creature->GetPositionZ();
    snapshot.members.push_back(patientMember);

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 5000);

    PartyBotEngine healEngine;
    std::shared_ptr<SimulatorCorePlayerHealAction> healAction(new SimulatorCorePlayerHealAction(map));
    bool const healRegistered = healEngine.RegisterAction(healAction);
    std::shared_ptr<Strategy> healStrategy(new Strategy("simulator_healer_triage"));
    healStrategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>("core_player_heal"));
    healEngine.AddStrategy(healStrategy);
    DecisionReport healReport = healEngine.Evaluate(context);
    bool const healSelected = healReport.selectedIndices.size() == 1 &&
        healReport.candidates[healReport.selectedIndices[0]].candidate.targetGuid == patientMember.guid &&
        healReport.candidates[healReport.selectedIndices[0]].status == CandidateStatus::Selected;
    healEngine.ExecuteSelected(context, healReport);
    bool const castAccepted = !healReport.selectedIndices.empty() &&
        healReport.candidates[healReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        healReport.candidates[healReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        healAction->GetLastCastResult() == SPELL_CAST_OK &&
        healAction->WasCastPendingAfterAccept() && healAction->HadGcdAfterAccept();

    PartyBotEngine moveEngine;
    std::shared_ptr<SimulatorCoreMovePointAction> moveAction(new SimulatorCoreMovePointAction(map, true));
    bool const moveRegistered = moveEngine.RegisterAction(moveAction);
    std::shared_ptr<Strategy> moveStrategy(new Strategy("simulator_formation"));
    moveStrategy->AddTrigger(std::make_shared<SimulatorFormationTrigger>());
    moveEngine.AddStrategy(moveStrategy);
    DecisionReport moveReport = moveEngine.Evaluate(context);
    bool const moveSelected = moveReport.selectedIndices.size() == 1 &&
        moveReport.candidates[moveReport.selectedIndices[0]].candidate.targetGuid == patientMember.guid &&
        moveReport.candidates[moveReport.selectedIndices[0]].status == CandidateStatus::Selected;
    moveEngine.ExecuteSelected(context, moveReport);
    bool const moveExecuted = !moveReport.selectedIndices.empty() &&
        moveReport.candidates[moveReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        moveReport.candidates[moveReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        moveAction->DidStartMovement();

    float const destinationX = moveReport.selectedIndices.empty() ? healerMember.x :
        moveReport.candidates[moveReport.selectedIndices[0]].candidate.destinationX;
    float const destinationY = moveReport.selectedIndices.empty() ? healerMember.y :
        moveReport.candidates[moveReport.selectedIndices[0]].candidate.destinationY;
    float const destinationZ = moveReport.selectedIndices.empty() ? healerMember.z :
        moveReport.candidates[moveReport.selectedIndices[0]].candidate.destinationZ;

    for (uint32 tick = 0; tick < 20; ++tick)
    {
        float const dx = healer->GetPositionX() - destinationX;
        float const dy = healer->GetPositionY() - destinationY;
        float const dz = healer->GetPositionZ() - destinationZ;
        bool const atDestination = dx * dx + dy * dy + dz * dz <= 1.0f;
        bool const castEnded = healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
        if (atDestination && castEnded)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
        if (tick == 0)
        {
            Spell* const pendingSpell = healer->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[PartyBotSimulator] First Core movement update: genericSpellPresent=%u spellId=%u state=%u patientHealth=%u.",
                     pendingSpell ? 1u : 0u,
                     pendingSpell && pendingSpell->m_spellInfo ? pendingSpell->m_spellInfo->Id : 0u,
                     pendingSpell ? static_cast<uint32>(pendingSpell->getState()) : 0u,
                     patient.creature->GetHealth());
        }
    }

    uint32 const healthAfter = patient.creature->GetHealth();
    bool const castCancelled = healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                               healthAfter == healthBefore;
    bool const lifecycleInterrupted =
        lifecycleProbe->prepareCalls == 1 && lifecycleProbe->finishCalls == 1 &&
        lifecycleProbe->preparedSpellId == SIMULATOR_HEAL_SPELL_ID &&
        lifecycleProbe->coreTargetGuidAtPrepare == patient.creature->GetObjectGuid().GetRawValue() &&
        lifecycleProbe->finishedActionId == SimulatorSpellLifecycleProbeAI::expectedActionId &&
        lifecycleProbe->finishedEncounterId == SimulatorSpellLifecycleProbeAI::expectedEncounterId &&
        lifecycleProbe->finishedSpellId == SIMULATOR_HEAL_SPELL_ID &&
        lifecycleProbe->finishedTargetGuid == patient.creature->GetObjectGuid().GetRawValue() &&
        !lifecycleProbe->finishedSuccessfully && lifecycleProbe->finishedInterrupted;
    uint32 const lifecyclePrepareCalls = lifecycleProbe->prepareCalls;
    uint32 const lifecycleFinishCalls = lifecycleProbe->finishCalls;
    float const finalDx = healer->GetPositionX() - destinationX;
    float const finalDy = healer->GetPositionY() - destinationY;
    float const finalDz = healer->GetPositionZ() - destinationZ;
    bool const movementCompleted = finalDx * finalDx + finalDy * finalDy +
                                   finalDz * finalDz <= 1.0f;

    healerSession.LogoutPlayer(false);
    bool const playerCleaned = healerSession.GetPlayer() == nullptr;
    healer = nullptr;
    bool const patientCleaned = DestroySimulatorActor(map, patient);
    bool const cleaned = playerCleaned && patientCleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Movement interrupts Player cast: healRegistered=%u healSelected=%u castAccepted=%u moveRegistered=%u moveSelected=%u moveExecuted=%u castCancelled=%u lifecycle=%u prepare=%u finish=%u health=%u>%u movementCompleted=%u cleanup=%u.",
             healRegistered ? 1u : 0u, healSelected ? 1u : 0u, castAccepted ? 1u : 0u,
             moveRegistered ? 1u : 0u, moveSelected ? 1u : 0u, moveExecuted ? 1u : 0u,
             castCancelled ? 1u : 0u, lifecycleInterrupted ? 1u : 0u,
             lifecyclePrepareCalls, lifecycleFinishCalls, healthBefore, healthAfter,
             movementCompleted ? 1u : 0u, cleaned ? 1u : 0u);

    if (!healRegistered || !healSelected || !castAccepted ||
        !moveRegistered || !moveSelected || !moveExecuted ||
        !castCancelled || !lifecycleInterrupted || !movementCompleted || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine movement/cast-interrupt smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Engine-selected Player movement interrupted a pending Core Lesser Heal cast without applying the heal.");
    return true;
}

bool RunEngineCoreMovementSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor leader;
    SimulatorActor mover;
    bool created = CreateSimulatorActor(map, *info, partyFaction, 0.0f, 6.0f, leader) &&
                   CreateSimulatorActor(map, *info, partyFaction, 10.0f, 6.0f, mover);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, mover);
        cleaned = DestroySimulatorActor(map, leader) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Movement fixture creation failed (cleanup=%u).", cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120 && (!leader.updateCount || !mover.updateCount); ++tick)
    {
        map.Update(50);
        if (!leader.updateCount || !mover.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    PartySnapshot snapshot;
    snapshot.groupId = 4;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 4;
    snapshot.capturedAtMs = 4000;

    PartyMemberSnapshot moverMember;
    moverMember.guid = mover.creature->GetObjectGuid().GetRawValue();
    moverMember.roleId = static_cast<std::uint8_t>(PartyRole::MeleeDamage);
    moverMember.mapId = map.GetId();
    moverMember.instanceId = map.GetInstanceId();
    moverMember.health = mover.creature->GetHealth();
    moverMember.maxHealth = mover.creature->GetMaxHealth();
    moverMember.online = true;
    moverMember.alive = mover.creature->IsAlive();
    moverMember.x = mover.creature->GetPositionX();
    moverMember.y = mover.creature->GetPositionY();
    moverMember.z = mover.creature->GetPositionZ();
    snapshot.members.push_back(moverMember);

    PartyMemberSnapshot leaderMember;
    leaderMember.guid = leader.creature->GetObjectGuid().GetRawValue();
    leaderMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
    leaderMember.mapId = map.GetId();
    leaderMember.instanceId = map.GetInstanceId();
    leaderMember.health = leader.creature->GetHealth();
    leaderMember.maxHealth = leader.creature->GetMaxHealth();
    leaderMember.online = true;
    leaderMember.alive = leader.creature->IsAlive();
    leaderMember.isMainTank = true;
    leaderMember.x = leader.creature->GetPositionX();
    leaderMember.y = leader.creature->GetPositionY();
    leaderMember.z = leader.creature->GetPositionZ();
    snapshot.members.push_back(leaderMember);

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 4000);
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCoreMovePointAction> moveAction(new SimulatorCoreMovePointAction(map));
    bool const registered = engine.RegisterAction(moveAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_formation"));
    strategy->AddTrigger(std::make_shared<SimulatorFormationTrigger>());
    engine.AddStrategy(strategy);

    float const distanceBefore = mover.creature->GetDistance(leader.creature);
    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == leaderMember.guid &&
        report.candidates[report.selectedIndices[0]].candidate.hasDestination &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(context, report);
    bool const executed = !report.selectedIndices.empty() &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed &&
        report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Success &&
        moveAction->DidStartMovement();
    MovementGeneratorType const movementTypeAfterExecute =
        mover.creature->GetMotionMaster()->GetCurrentMovementGeneratorType();

    float const destinationX = report.selectedIndices.empty() ? moverMember.x :
        report.candidates[report.selectedIndices[0]].candidate.destinationX;
    float const destinationY = report.selectedIndices.empty() ? moverMember.y :
        report.candidates[report.selectedIndices[0]].candidate.destinationY;
    float const destinationZ = report.selectedIndices.empty() ? moverMember.z :
        report.candidates[report.selectedIndices[0]].candidate.destinationZ;
    for (uint32 tick = 0; tick < 20; ++tick)
    {
        float const dx = mover.creature->GetPositionX() - destinationX;
        float const dy = mover.creature->GetPositionY() - destinationY;
        float const dz = mover.creature->GetPositionZ() - destinationZ;
        if (dx * dx + dy * dy + dz * dz <= 1.0f)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    float const distanceAfter = mover.creature->GetDistance(leader.creature);
    float const destinationDx = mover.creature->GetPositionX() - destinationX;
    float const destinationDy = mover.creature->GetPositionY() - destinationY;
    float const destinationDz = mover.creature->GetPositionZ() - destinationZ;
    bool const moved = distanceAfter + 1.0f < distanceBefore &&
        destinationDx * destinationDx + destinationDy * destinationDy +
        destinationDz * destinationDz <= 1.0f;

    bool cleaned = DestroySimulatorActor(map, mover);
    cleaned = DestroySimulatorActor(map, leader) && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine movement: aiReady=%u registered=%u selected=%u executed=%u motion=%u distance=%.2f>%.2f reached=%u cleanup=%u.",
             (leader.updateCount && mover.updateCount) ? 1u : 0u,
             registered ? 1u : 0u, selected ? 1u : 0u, executed ? 1u : 0u,
             static_cast<uint32>(movementTypeAfterExecute),
             distanceBefore, distanceAfter, moved ? 1u : 0u, cleaned ? 1u : 0u);

    if (!registered || !selected || !executed || !moved || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine-to-Core movement smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine selected a formation destination and the Core PointMovementGenerator moved the actor to it.");
    return true;
}


bool RunEngineCoreFollowSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor leader;
    SimulatorActor follower;
    bool const created = CreateSimulatorActor(map, *info, partyFaction, 0.0f, 6.0f, leader) &&
                         CreateSimulatorActor(map, *info, partyFaction, 10.0f, 6.0f, follower);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, follower);
        cleaned = DestroySimulatorActor(map, leader) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Follow fixture creation failed (cleanup=%u).", cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120 && (!leader.updateCount || !follower.updateCount); ++tick)
    {
        map.Update(50);
        if (!leader.updateCount || !follower.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    PartySnapshot snapshot;
    snapshot.groupId = 6;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 6;
    snapshot.capturedAtMs = 6000;

    PartyMemberSnapshot followerMember;
    followerMember.guid = follower.creature->GetObjectGuid().GetRawValue();
    followerMember.roleId = static_cast<std::uint8_t>(PartyRole::MeleeDamage);
    followerMember.mapId = map.GetId();
    followerMember.instanceId = map.GetInstanceId();
    followerMember.health = follower.creature->GetHealth();
    followerMember.maxHealth = follower.creature->GetMaxHealth();
    followerMember.online = true;
    followerMember.alive = follower.creature->IsAlive();
    followerMember.x = follower.creature->GetPositionX();
    followerMember.y = follower.creature->GetPositionY();
    followerMember.z = follower.creature->GetPositionZ();
    snapshot.members.push_back(followerMember);

    PartyMemberSnapshot leaderMember;
    leaderMember.guid = leader.creature->GetObjectGuid().GetRawValue();
    leaderMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
    leaderMember.mapId = map.GetId();
    leaderMember.instanceId = map.GetInstanceId();
    leaderMember.health = leader.creature->GetHealth();
    leaderMember.maxHealth = leader.creature->GetMaxHealth();
    leaderMember.online = true;
    leaderMember.alive = leader.creature->IsAlive();
    leaderMember.isMainTank = true;
    leaderMember.x = leader.creature->GetPositionX();
    leaderMember.y = leader.creature->GetPositionY();
    leaderMember.z = leader.creature->GetPositionZ();
    snapshot.members.push_back(leaderMember);

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 6000);
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCoreFollowAction> followAction(new SimulatorCoreFollowAction(map));
    bool const registered = engine.RegisterAction(followAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_continuous_follow"));
    strategy->AddTrigger(std::make_shared<SimulatorFollowTrigger>());
    engine.AddStrategy(strategy);

    float const initialDistance = std::sqrt(
        (follower.creature->GetPositionX() - leader.creature->GetPositionX()) *
        (follower.creature->GetPositionX() - leader.creature->GetPositionX()) +
        (follower.creature->GetPositionY() - leader.creature->GetPositionY()) *
        (follower.creature->GetPositionY() - leader.creature->GetPositionY()) +
        (follower.creature->GetPositionZ() - leader.creature->GetPositionZ()) *
        (follower.creature->GetPositionZ() - leader.creature->GetPositionZ()));

    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.actionName == "core_follow" &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == leaderMember.guid &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(context, report);
    bool const executed = !report.selectedIndices.empty() &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed &&
        report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Success &&
        followAction->DidStartFollow();

    bool initialCaughtUp = false;
    for (uint32 tick = 0; tick < 24; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
        float const dx = follower.creature->GetPositionX() - leader.creature->GetPositionX();
        float const dy = follower.creature->GetPositionY() - leader.creature->GetPositionY();
        float const dz = follower.creature->GetPositionZ() - leader.creature->GetPositionZ();
        float const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance <= 5.0f)
        {
            initialCaughtUp = true;
            break;
        }
    }

    float const followerXBeforeLeaderMove = follower.creature->GetPositionX();
    leader.creature->GetMotionMaster()->MovePoint(2, -10.0f, 6.0f, 0.0f, MOVE_NONE);
    bool const leaderMovementStarted =
        leader.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

    bool followedLeaderRelocation = false;
    for (uint32 tick = 0; tick < 48; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);

        float const leaderDx = leader.creature->GetPositionX() + 10.0f;
        float const leaderDy = leader.creature->GetPositionY() - 6.0f;
        float const leaderDz = leader.creature->GetPositionZ();
        bool const leaderAtDestination =
            leaderDx * leaderDx + leaderDy * leaderDy + leaderDz * leaderDz <= 1.0f;

        float const dx = follower.creature->GetPositionX() - leader.creature->GetPositionX();
        float const dy = follower.creature->GetPositionY() - leader.creature->GetPositionY();
        float const dz = follower.creature->GetPositionZ() - leader.creature->GetPositionZ();
        float const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (leaderAtDestination && distance <= 5.0f &&
            follower.creature->GetPositionX() < followerXBeforeLeaderMove - 5.0f)
        {
            followedLeaderRelocation = true;
            break;
        }
    }

    float const finalDx = follower.creature->GetPositionX() - leader.creature->GetPositionX();
    float const finalDy = follower.creature->GetPositionY() - leader.creature->GetPositionY();
    float const finalDz = follower.creature->GetPositionZ() - leader.creature->GetPositionZ();
    float const finalDistance = std::sqrt(finalDx * finalDx + finalDy * finalDy + finalDz * finalDz);
    bool const followGeneratorRetained =
        follower.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
        follower.creature->HasUnitState(UNIT_STATE_FOLLOW);
    bool const leaderReachedDestination =
        std::fabs(leader.creature->GetPositionX() + 10.0f) <= 1.0f &&
        std::fabs(leader.creature->GetPositionY() - 6.0f) <= 1.0f;

    bool cleaned = DestroySimulatorActor(map, follower);
    cleaned = DestroySimulatorActor(map, leader) && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine Core follow: aiReady=%u registered=%u selected=%u executed=%u initialDistance=%.2f initialCaughtUp=%u leaderMove=%u leaderReached=%u followerTracked=%u finalDistance=%.2f followGenerator=%u cleanup=%u.",
             (leader.updateCount && follower.updateCount) ? 1u : 0u,
             registered ? 1u : 0u, selected ? 1u : 0u, executed ? 1u : 0u,
             initialDistance, initialCaughtUp ? 1u : 0u,
             leaderMovementStarted ? 1u : 0u, leaderReachedDestination ? 1u : 0u,
             followedLeaderRelocation ? 1u : 0u, finalDistance,
             followGeneratorRetained ? 1u : 0u, cleaned ? 1u : 0u);

    if (!leader.updateCount || !follower.updateCount || !registered || !selected || !executed ||
        !initialCaughtUp || !leaderMovementStarted || !leaderReachedDestination ||
        !followedLeaderRelocation || !followGeneratorRetained || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine Core continuous-follow smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine selected Core MoveFollow and the native FollowMovementGenerator tracked the Main Tank after it relocated.");
    return true;
}



bool RunEngineCoreFollowRetargetSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor leaderA;
    SimulatorActor leaderB;
    SimulatorActor follower;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 6.0f, leaderA) &&
        CreateSimulatorActor(map, *info, partyFaction, 15.0f, 6.0f, leaderB) &&
        CreateSimulatorActor(map, *info, partyFaction, 10.0f, 6.0f, follower);
    if (!actorsCreated)
    {
        bool cleaned = DestroySimulatorActor(map, follower);
        cleaned = DestroySimulatorActor(map, leaderB) && cleaned;
        cleaned = DestroySimulatorActor(map, leaderA) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Follow retarget fixture creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120 &&
         (!leaderA.updateCount || !leaderB.updateCount || !follower.updateCount); ++tick)
    {
        map.Update(50);
        if (!leaderA.updateCount || !leaderB.updateCount || !follower.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    bool const actorsReady = leaderA.updateCount && leaderB.updateCount && follower.updateCount;

    auto makeSnapshot = [&](uint64 epoch, bool aIsMainTank, bool bIsMainTank, bool includeB)
    {
        PartySnapshot snapshot;
        snapshot.groupId = 9;
        snapshot.mapId = map.GetId();
        snapshot.instanceId = map.GetInstanceId();
        snapshot.epoch = epoch;
        snapshot.capturedAtMs = epoch * 1000;

        auto appendMember = [&](Unit* unit, PartyRole role, bool isMainTank)
        {
            PartyMemberSnapshot member;
            member.guid = unit->GetObjectGuid().GetRawValue();
            member.roleId = static_cast<std::uint8_t>(role);
            member.mapId = unit->GetMapId();
            member.instanceId = unit->GetInstanceId();
            member.health = unit->GetHealth();
            member.maxHealth = unit->GetMaxHealth();
            member.online = true;
            member.alive = unit->IsAlive();
            member.isMainTank = isMainTank;
            member.x = unit->GetPositionX();
            member.y = unit->GetPositionY();
            member.z = unit->GetPositionZ();
            snapshot.members.push_back(member);
        };

        appendMember(follower.creature, PartyRole::MeleeDamage, false);
        appendMember(leaderA.creature, PartyRole::MainTank, aIsMainTank);
        if (includeB)
            appendMember(leaderB.creature, PartyRole::MainTank, bIsMainTank);
        return snapshot;
    };

    PartyCoordinator coordinator;
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCoreFollowAction> followAction(new SimulatorCoreFollowAction(map));
    bool const registered = engine.RegisterAction(followAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_follow_retarget_recovery"));
    strategy->AddTrigger(std::make_shared<SimulatorFollowTrigger>());
    engine.AddStrategy(strategy);

    PartySnapshot initialSnapshot = makeSnapshot(9, true, false, true);
    DecisionContext initialContext(initialSnapshot, initialSnapshot.members[0], coordinator, 9000);
    DecisionReport initialReport = engine.Evaluate(initialContext);
    bool const initialSelected = initialReport.selectedIndices.size() == 1 &&
        initialReport.candidates[initialReport.selectedIndices[0]].candidate.targetGuid ==
            initialSnapshot.members[1].guid &&
        initialReport.candidates[initialReport.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(initialContext, initialReport);
    bool const initialFollowStarted = !initialReport.selectedIndices.empty() &&
        initialReport.candidates[initialReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        initialReport.candidates[initialReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        followAction->DidStartFollow() &&
        followAction->GetLastTargetGuid() == initialSnapshot.members[1].guid;

    bool initialCaughtUp = false;
    for (uint32 tick = 0; tick < 32 && !initialCaughtUp; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
        float const dx = follower.creature->GetPositionX() - leaderA.creature->GetPositionX();
        float const dy = follower.creature->GetPositionY() - leaderA.creature->GetPositionY();
        float const dz = follower.creature->GetPositionZ() - leaderA.creature->GetPositionZ();
        initialCaughtUp = dx * dx + dy * dy + dz * dz <= 25.0f;
    }

    PartySnapshot handoffSnapshot = makeSnapshot(10, false, true, true);
    DecisionContext handoffContext(handoffSnapshot, handoffSnapshot.members[0], coordinator, 10000);
    DecisionReport handoffReport = engine.Evaluate(handoffContext);
    bool const handoffSelected = handoffReport.selectedIndices.size() == 1 &&
        handoffReport.candidates[handoffReport.selectedIndices[0]].candidate.targetGuid ==
            handoffSnapshot.members[2].guid &&
        handoffReport.candidates[handoffReport.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(handoffContext, handoffReport);
    bool const handoffExecuted = !handoffReport.selectedIndices.empty() &&
        handoffReport.candidates[handoffReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        handoffReport.candidates[handoffReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        followAction->DidStartFollow() &&
        followAction->GetLastTargetGuid() == handoffSnapshot.members[2].guid;
    bool const handoffTargetChanged = followAction->GetLastTargetGuid() == handoffSnapshot.members[2].guid;

    float const followerXBeforeLeaderMoves = follower.creature->GetPositionX();
    leaderA.creature->GetMotionMaster()->MovePoint(11, -10.0f, 6.0f, 0.0f, MOVE_NONE);
    leaderB.creature->GetMotionMaster()->MovePoint(12, 25.0f, 6.0f, 0.0f, MOVE_NONE);
    bool const leaderAMovementStarted =
        leaderA.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    bool const leaderBMovementStarted =
        leaderB.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

    bool leaderAReached = false;
    bool leaderBReached = false;
    bool followerTrackedNewLeader = false;
    for (uint32 tick = 0; tick < 64; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
        float const ax = leaderA.creature->GetPositionX() + 10.0f;
        float const ay = leaderA.creature->GetPositionY() - 6.0f;
        float const az = leaderA.creature->GetPositionZ();
        float const bx = leaderB.creature->GetPositionX() - 25.0f;
        float const by = leaderB.creature->GetPositionY() - 6.0f;
        float const bz = leaderB.creature->GetPositionZ();
        leaderAReached = ax * ax + ay * ay + az * az <= 1.0f;
        leaderBReached = bx * bx + by * by + bz * bz <= 1.0f;

        float const toLeaderB = follower.creature->GetDistance(leaderB.creature);
        float const toOldLeaderA = follower.creature->GetDistance(leaderA.creature);
        bool const movedTowardNewLeader =
            follower.creature->GetPositionX() > followerXBeforeLeaderMoves + 5.0f;
        if (leaderBReached && toLeaderB <= 5.0f && toOldLeaderA >= 15.0f &&
            movedTowardNewLeader)
        {
            followerTrackedNewLeader = true;
            break;
        }
    }

    bool const retargetFollowGeneratorRetained =
        follower.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
        follower.creature->HasUnitState(UNIT_STATE_FOLLOW);

    bool const removedNewLeader = DestroySimulatorActor(map, leaderB);
    for (uint32 tick = 0; tick < 2; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    PartySnapshot recoverySnapshot = makeSnapshot(11, true, false, false);
    DecisionContext recoveryContext(recoverySnapshot, recoverySnapshot.members[0], coordinator, 11000);
    DecisionReport recoveryReport = engine.Evaluate(recoveryContext);
    bool const recoverySelected = recoveryReport.selectedIndices.size() == 1 &&
        recoveryReport.candidates[recoveryReport.selectedIndices[0]].candidate.targetGuid ==
            recoverySnapshot.members[1].guid &&
        recoveryReport.candidates[recoveryReport.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(recoveryContext, recoveryReport);
    bool const recoveryExecuted = !recoveryReport.selectedIndices.empty() &&
        recoveryReport.candidates[recoveryReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        recoveryReport.candidates[recoveryReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        followAction->DidStartFollow() &&
        followAction->GetLastTargetGuid() == recoverySnapshot.members[1].guid;
    bool const recoveryTargetRestored = followAction->GetLastTargetGuid() == recoverySnapshot.members[1].guid;

    float const followerXBeforeRecoveryMove = follower.creature->GetPositionX();
    leaderA.creature->GetMotionMaster()->MovePoint(13, -20.0f, 6.0f, 0.0f, MOVE_NONE);
    bool const recoveryLeaderMovementStarted =
        leaderA.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

    bool recoveryLeaderReached = false;
    bool recoveredToAvailableLeader = false;
    for (uint32 tick = 0; tick < 80; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
        float const dx = leaderA.creature->GetPositionX() + 20.0f;
        float const dy = leaderA.creature->GetPositionY() - 6.0f;
        float const dz = leaderA.creature->GetPositionZ();
        recoveryLeaderReached = dx * dx + dy * dy + dz * dz <= 1.0f;
        float const distance = follower.creature->GetDistance(leaderA.creature);
        if (recoveryLeaderReached && distance <= 5.0f &&
            follower.creature->GetPositionX() < followerXBeforeRecoveryMove - 5.0f)
        {
            recoveredToAvailableLeader = true;
            break;
        }
    }

    float const finalDistance = follower.creature->GetDistance(leaderA.creature);
    bool const followGeneratorRetained =
        follower.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
        follower.creature->HasUnitState(UNIT_STATE_FOLLOW);

    bool cleaned = DestroySimulatorActor(map, follower);
    cleaned = DestroySimulatorActor(map, leaderA) && cleaned;
    cleaned = removedNewLeader && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine Core follow retarget/recovery: aiReady=%u registered=%u initialSelected=%u initialFollow=%u initialCaughtUp=%u handoffSelected=%u handoffExecuted=%u handoffTargetChanged=%u leaderAMove=%u leaderBMove=%u leaderAReached=%u leaderBReached=%u followerTrackedNewLeader=%u oldLeaderRemoved=%u recoverySelected=%u recoveryExecuted=%u recoveryTargetRestored=%u recoveryLeaderMove=%u recoveryLeaderReached=%u recoveredToAvailableLeader=%u finalDistance=%.2f followGenerator=%u cleanup=%u.",
             actorsReady ? 1u : 0u, registered ? 1u : 0u,
             initialSelected ? 1u : 0u, initialFollowStarted ? 1u : 0u,
             initialCaughtUp ? 1u : 0u, handoffSelected ? 1u : 0u,
             handoffExecuted ? 1u : 0u, handoffTargetChanged ? 1u : 0u,
             leaderAMovementStarted ? 1u : 0u, leaderBMovementStarted ? 1u : 0u,
             leaderAReached ? 1u : 0u, leaderBReached ? 1u : 0u,
             followerTrackedNewLeader ? 1u : 0u, removedNewLeader ? 1u : 0u,
             recoverySelected ? 1u : 0u, recoveryExecuted ? 1u : 0u,
             recoveryTargetRestored ? 1u : 0u, recoveryLeaderMovementStarted ? 1u : 0u,
             recoveryLeaderReached ? 1u : 0u, recoveredToAvailableLeader ? 1u : 0u,
             finalDistance, followGeneratorRetained ? 1u : 0u, cleaned ? 1u : 0u);

    if (!actorsReady || !registered || !initialSelected || !initialFollowStarted || !initialCaughtUp ||
        !handoffSelected || !handoffExecuted || !handoffTargetChanged ||
        !leaderAMovementStarted || !leaderBMovementStarted || !leaderAReached || !leaderBReached ||
        !followerTrackedNewLeader || !retargetFollowGeneratorRetained || !removedNewLeader ||
        !recoverySelected || !recoveryExecuted || !recoveryTargetRestored ||
        !recoveryLeaderMovementStarted || !recoveryLeaderReached ||
        !recoveredToAvailableLeader || !followGeneratorRetained || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core follow retarget/recovery smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine retargeted native Core FollowMovementGenerator to the new Main Tank and recovered after that target disappeared.");
    return true;
}

bool RunEngineCorePlayerFollowCastSafetySmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor leader;
    bool const leaderCreated = CreateSimulatorActor(map, *info, partyFaction, 0.0f, 6.0f, leader);
    if (!leaderCreated)
    {
        DestroySimulatorActor(map, leader);
        return false;
    }

    WorldSession followerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* follower = nullptr;
    bool const followerCreated = CreateSimulatorPlayer(map, partyFaction, followerSession,
                                                          follower, 10.0f, 6.0f, 0.0f);
    if (!followerCreated)
    {
        if (followerSession.GetPlayer())
            followerSession.LogoutPlayer(false);
        else if (follower)
            delete follower;
        bool const cleaned = DestroySimulatorActor(map, leader);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Player follow/cast fixture creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    PartySnapshot followSnapshot;
    followSnapshot.groupId = 7;
    followSnapshot.mapId = map.GetId();
    followSnapshot.instanceId = map.GetInstanceId();
    followSnapshot.epoch = 7;
    followSnapshot.capturedAtMs = 7000;

    PartyMemberSnapshot followerMember;
    followerMember.guid = follower->GetObjectGuid().GetRawValue();
    followerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    followerMember.mapId = map.GetId();
    followerMember.instanceId = map.GetInstanceId();
    followerMember.health = follower->GetHealth();
    followerMember.maxHealth = follower->GetMaxHealth();
    followerMember.online = true;
    followerMember.alive = follower->IsAlive();
    followerMember.x = follower->GetPositionX();
    followerMember.y = follower->GetPositionY();
    followerMember.z = follower->GetPositionZ();
    followSnapshot.members.push_back(followerMember);

    PartyMemberSnapshot leaderMember;
    leaderMember.guid = leader.creature->GetObjectGuid().GetRawValue();
    leaderMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
    leaderMember.mapId = map.GetId();
    leaderMember.instanceId = map.GetInstanceId();
    leaderMember.health = leader.creature->GetHealth();
    leaderMember.maxHealth = leader.creature->GetMaxHealth();
    leaderMember.online = true;
    leaderMember.alive = leader.creature->IsAlive();
    leaderMember.isMainTank = true;
    leaderMember.x = leader.creature->GetPositionX();
    leaderMember.y = leader.creature->GetPositionY();
    leaderMember.z = leader.creature->GetPositionZ();
    followSnapshot.members.push_back(leaderMember);

    PartyCoordinator coordinator;
    DecisionContext followContext(followSnapshot, followSnapshot.members[0], coordinator, 7000);
    PartyBotEngine followEngine;
    std::shared_ptr<SimulatorCoreFollowAction> followAction(new SimulatorCoreFollowAction(map));
    bool const followRegistered = followEngine.RegisterAction(followAction);
    std::shared_ptr<Strategy> followStrategy(new Strategy("simulator_player_follow_during_cast"));
    followStrategy->AddTrigger(std::make_shared<SimulatorFollowTrigger>());
    followEngine.AddStrategy(followStrategy);

    DecisionReport followReport = followEngine.Evaluate(followContext);
    bool const followSelected = followReport.selectedIndices.size() == 1 &&
        followReport.candidates[followReport.selectedIndices[0]].candidate.actionName == "core_follow" &&
        followReport.candidates[followReport.selectedIndices[0]].candidate.targetGuid == leaderMember.guid &&
        followReport.candidates[followReport.selectedIndices[0]].status == CandidateStatus::Selected;
    followEngine.ExecuteSelected(followContext, followReport);
    bool const followStarted = !followReport.selectedIndices.empty() &&
        followReport.candidates[followReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        followReport.candidates[followReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        followAction->DidStartFollow();

    bool initialCaughtUpAndStable = false;
    uint32 stationarySamples = 0;
    float previousFollowerX = follower->GetPositionX();
    float previousFollowerY = follower->GetPositionY();
    float previousFollowerZ = follower->GetPositionZ();
    for (uint32 tick = 0; tick < 48; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);

        float const dx = follower->GetPositionX() - leader.creature->GetPositionX();
        float const dy = follower->GetPositionY() - leader.creature->GetPositionY();
        float const dz = follower->GetPositionZ() - leader.creature->GetPositionZ();
        float const distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        float const stepX = follower->GetPositionX() - previousFollowerX;
        float const stepY = follower->GetPositionY() - previousFollowerY;
        float const stepZ = follower->GetPositionZ() - previousFollowerZ;
        float const stepDistance = std::sqrt(stepX * stepX + stepY * stepY + stepZ * stepZ);
        if (distance <= 5.0f && stepDistance <= 0.05f)
            ++stationarySamples;
        else
            stationarySamples = 0;

        previousFollowerX = follower->GetPositionX();
        previousFollowerY = follower->GetPositionY();
        previousFollowerZ = follower->GetPositionZ();
        if (stationarySamples >= 2)
        {
            initialCaughtUpAndStable = true;
            break;
        }
    }

    // Re-prime the real Player update cadence before starting the cast. The
    // cast/follow interaction below is advanced only through normal Core map
    // updates; no spell timer or player position is changed by the fixture.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    map.Update(250);

    // The follower may spend several seconds reaching formation range. Reset
    // the fixture target after that wait so passive Creature regeneration
    // cannot remove the injury before the Engine evaluates healer triage.
    leader.creature->SetHealth(leader.creature->GetMaxHealth() / 2);
    uint32 const patientHealthBefore = leader.creature->GetHealth();
    uint32 const healerManaBefore = follower->GetPower(POWER_MANA);

    PartySnapshot healSnapshot;
    healSnapshot.groupId = 8;
    healSnapshot.mapId = map.GetId();
    healSnapshot.instanceId = map.GetInstanceId();
    healSnapshot.epoch = 8;
    healSnapshot.capturedAtMs = 8000;

    followerMember.x = follower->GetPositionX();
    followerMember.y = follower->GetPositionY();
    followerMember.z = follower->GetPositionZ();
    healSnapshot.members.push_back(followerMember);
    leaderMember.health = leader.creature->GetHealth();
    leaderMember.x = leader.creature->GetPositionX();
    leaderMember.y = leader.creature->GetPositionY();
    leaderMember.z = leader.creature->GetPositionZ();
    healSnapshot.members.push_back(leaderMember);

    DecisionContext healContext(healSnapshot, healSnapshot.members[0], coordinator, 8000);
    PartyBotEngine healEngine;
    std::shared_ptr<SimulatorCorePlayerHealAction> healAction(new SimulatorCorePlayerHealAction(map));
    bool const healRegistered = healEngine.RegisterAction(healAction);
    std::shared_ptr<Strategy> healStrategy(new Strategy("simulator_follow_cast_safety_heal"));
    healStrategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>("core_player_heal"));
    healEngine.AddStrategy(healStrategy);

    DecisionReport healReport = healEngine.Evaluate(healContext);
    bool const healSelected = healReport.selectedIndices.size() == 1 &&
        healReport.candidates[healReport.selectedIndices[0]].candidate.targetGuid == leaderMember.guid &&
        healReport.candidates[healReport.selectedIndices[0]].candidate.spellId == SIMULATOR_HEAL_SPELL_ID &&
        healReport.candidates[healReport.selectedIndices[0]].status == CandidateStatus::Selected;
    healEngine.ExecuteSelected(healContext, healReport);
    bool const castAccepted = !healReport.selectedIndices.empty() &&
        healReport.candidates[healReport.selectedIndices[0]].status == CandidateStatus::Executed &&
        healReport.candidates[healReport.selectedIndices[0]].actionResult == ActionResult::Success &&
        healAction->GetLastCastResult() == SPELL_CAST_OK &&
        healAction->WasCastPendingAfterAccept() && healAction->HadGcdAfterAccept();

    float const followerXBeforeLeaderMove = follower->GetPositionX();
    float const followerYBeforeLeaderMove = follower->GetPositionY();
    float const followerZBeforeLeaderMove = follower->GetPositionZ();
    leader.creature->GetMotionMaster()->MovePoint(3, -20.0f, 6.0f, 0.0f, MOVE_NONE);
    bool const leaderMovementStarted =
        leader.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

    uint32 pendingCastTicks = 0;
    uint32 coreNoMovementGateTicks = 0;
    float maxFollowerDisplacementWhileCasting = 0.0f;
    bool leaderReachedDestination = false;
    for (uint32 tick = 0; tick < 32; ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);

        if (follower->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            ++pendingCastTicks;
            if (follower->IsNoMovementSpellCasted())
            {
                ++coreNoMovementGateTicks;
                float const dx = follower->GetPositionX() - followerXBeforeLeaderMove;
                float const dy = follower->GetPositionY() - followerYBeforeLeaderMove;
                float const dz = follower->GetPositionZ() - followerZBeforeLeaderMove;
                maxFollowerDisplacementWhileCasting = std::max(
                    maxFollowerDisplacementWhileCasting, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }

        float const leaderDx = leader.creature->GetPositionX() + 20.0f;
        float const leaderDy = leader.creature->GetPositionY() - 6.0f;
        float const leaderDz = leader.creature->GetPositionZ();
        leaderReachedDestination =
            leaderDx * leaderDx + leaderDy * leaderDy + leaderDz * leaderDz <= 1.0f;

        if (!follower->GetCurrentSpell(CURRENT_GENERIC_SPELL) && leaderReachedDestination)
            break;
    }

    uint32 const patientHealthAfterCast = leader.creature->GetHealth();
    uint32 const healerManaAfterCast = follower->GetPower(POWER_MANA);
    bool const castCompleted = !follower->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
        patientHealthAfterCast > patientHealthBefore && healerManaAfterCast < healerManaBefore;
    bool const followPausedDuringCast = coreNoMovementGateTicks >= 2 &&
        maxFollowerDisplacementWhileCasting <= 0.25f;

    bool followedAfterCast = false;
    for (uint32 tick = 0; tick < 48; ++tick)
    {
        float const dx = follower->GetPositionX() - leader.creature->GetPositionX();
        float const dy = follower->GetPositionY() - leader.creature->GetPositionY();
        float const dz = follower->GetPositionZ() - leader.creature->GetPositionZ();
        float const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (leaderReachedDestination && distance <= 5.0f &&
            follower->GetPositionX() < followerXBeforeLeaderMove - 5.0f)
        {
            followedAfterCast = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    float const finalDx = follower->GetPositionX() - leader.creature->GetPositionX();
    float const finalDy = follower->GetPositionY() - leader.creature->GetPositionY();
    float const finalDz = follower->GetPositionZ() - leader.creature->GetPositionZ();
    float const finalDistance = std::sqrt(finalDx * finalDx + finalDy * finalDy + finalDz * finalDz);
    bool const followRetained =
        follower->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
        follower->HasUnitState(UNIT_STATE_FOLLOW);

    followerSession.LogoutPlayer(false);
    bool const playerCleaned = followerSession.GetPlayer() == nullptr;
    follower = nullptr;
    bool const leaderCleaned = DestroySimulatorActor(map, leader);
    bool const cleaned = playerCleaned && leaderCleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine Player follow/cast safety: followRegistered=%u followSelected=%u followStarted=%u initialCaughtUpStable=%u stationarySamples=%u healRegistered=%u healSelected=%u castAccepted=%u pendingTicks=%u coreNoMovementTicks=%u maxCastMove=%.2f castCompleted=%u leaderMove=%u leaderReached=%u followPaused=%u followedAfterCast=%u finalDistance=%.2f followRetained=%u cleanup=%u.",
             followRegistered ? 1u : 0u, followSelected ? 1u : 0u, followStarted ? 1u : 0u,
             initialCaughtUpAndStable ? 1u : 0u, stationarySamples,
             healRegistered ? 1u : 0u, healSelected ? 1u : 0u,
             castAccepted ? 1u : 0u, pendingCastTicks, coreNoMovementGateTicks,
             maxFollowerDisplacementWhileCasting, castCompleted ? 1u : 0u,
             leaderMovementStarted ? 1u : 0u, leaderReachedDestination ? 1u : 0u,
             followPausedDuringCast ? 1u : 0u, followedAfterCast ? 1u : 0u,
             finalDistance, followRetained ? 1u : 0u, cleaned ? 1u : 0u);

    if (!followRegistered || !followSelected || !followStarted || !initialCaughtUpAndStable ||
        !healRegistered || !healSelected || !castAccepted || !castCompleted ||
        !leaderMovementStarted || !leaderReachedDestination || !followPausedDuringCast ||
        !followedAfterCast || !followRetained || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine Player follow/cast safety smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Core Player FollowMovementGenerator paused while its non-triggered Lesser Heal cast was pending and resumed tracking the Main Tank after the cast completed.");
    return true;
}



bool RunEngineCorePlayerHealRevalidationSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor outOfRangePatient;
    SimulatorActor noPowerPatient;
    bool const patientsCreated =
        CreateSimulatorActor(map, *info, partyFaction, 60.0f, 6.0f, outOfRangePatient) &&
        CreateSimulatorActor(map, *info, partyFaction, 8.0f, 6.0f, noPowerPatient);
    if (!patientsCreated)
    {
        bool cleaned = DestroySimulatorActor(map, noPowerPatient);
        cleaned = DestroySimulatorActor(map, outOfRangePatient) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Heal revalidation patient creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    WorldSession healerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* healer = nullptr;
    bool const healerCreated = CreateSimulatorPlayer(map, partyFaction, healerSession,
                                                       healer, 0.0f, 6.0f, 0.0f);
    if (!healerCreated)
    {
        if (healerSession.GetPlayer())
            healerSession.LogoutPlayer(false);
        else if (healer)
            delete healer;
        bool cleaned = DestroySimulatorActor(map, noPowerPatient);
        cleaned = DestroySimulatorActor(map, outOfRangePatient) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Heal revalidation Player creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    outOfRangePatient.creature->SetHealth(outOfRangePatient.creature->GetMaxHealth() / 2);
    noPowerPatient.creature->SetHealth(noPowerPatient.creature->GetMaxHealth() / 2);

    PartyCoordinator coordinator;
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCorePlayerHealAction> healAction(new SimulatorCorePlayerHealAction(map));
    bool const registered = engine.RegisterAction(healAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_player_heal_execution_revalidation"));
    strategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>("core_player_heal"));
    engine.AddStrategy(strategy);

    auto makeSnapshot = [&](uint64 epoch, Creature* patient, bool staleNearPosition)
    {
        PartySnapshot snapshot;
        snapshot.groupId = 10;
        snapshot.mapId = map.GetId();
        snapshot.instanceId = map.GetInstanceId();
        snapshot.epoch = epoch;
        snapshot.capturedAtMs = epoch * 1000;

        PartyMemberSnapshot healerMember;
        healerMember.guid = healer->GetObjectGuid().GetRawValue();
        healerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
        healerMember.mapId = map.GetId();
        healerMember.instanceId = map.GetInstanceId();
        healerMember.health = healer->GetHealth();
        healerMember.maxHealth = healer->GetMaxHealth();
        healerMember.online = true;
        healerMember.alive = healer->IsAlive();
        healerMember.x = healer->GetPositionX();
        healerMember.y = healer->GetPositionY();
        healerMember.z = healer->GetPositionZ();
        snapshot.members.push_back(healerMember);

        PartyMemberSnapshot patientMember;
        patientMember.guid = patient->GetObjectGuid().GetRawValue();
        patientMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
        patientMember.mapId = map.GetId();
        patientMember.instanceId = map.GetInstanceId();
        patientMember.health = patient->GetHealth();
        patientMember.maxHealth = patient->GetMaxHealth();
        patientMember.online = true;
        patientMember.alive = patient->IsAlive();
        patientMember.isMainTank = true;
        patientMember.x = staleNearPosition ? healer->GetPositionX() + 5.0f : patient->GetPositionX();
        patientMember.y = healer->GetPositionY();
        patientMember.z = healer->GetPositionZ();
        snapshot.members.push_back(patientMember);
        return snapshot;
    };

    // The value snapshot intentionally says the first injured target was
    // nearby, but the authoritative Core Unit is 60 yards away. The action
    // must not trust stale snapshot coordinates: normal CastSpell/CheckCast
    // owns the final range decision.
    uint32 const rangeHealthBefore = outOfRangePatient.creature->GetHealth();
    PartySnapshot rangeSnapshot = makeSnapshot(10, outOfRangePatient.creature, true);
    DecisionContext rangeContext(rangeSnapshot, rangeSnapshot.members[0], coordinator, 10000);
    DecisionReport rangeReport = engine.Evaluate(rangeContext);
    bool const rangeSelected = rangeReport.selectedIndices.size() == 1 &&
        rangeReport.candidates[rangeReport.selectedIndices[0]].candidate.targetGuid ==
            outOfRangePatient.creature->GetObjectGuid().GetRawValue() &&
        rangeReport.candidates[rangeReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (rangeSelected)
        engine.ExecuteSelected(rangeContext, rangeReport);
    bool const rangeExecutionFailed = rangeSelected &&
        rangeReport.candidates[rangeReport.selectedIndices[0]].status == CandidateStatus::ExecutionFailed &&
        rangeReport.candidates[rangeReport.selectedIndices[0]].actionResult == ActionResult::Failed;
    SpellCastResult const rangeCastResult = healAction->GetLastCastResult();
    uint32 const rangeHealthAfter = outOfRangePatient.creature->GetHealth();
    uint32 const rangeManaBefore = healAction->GetManaBefore();
    uint32 const rangeManaAfter = healAction->GetManaAfterAccept();
    bool const rangeNoPending = !healAction->WasCastPendingAfterAccept();
    bool const rangeNoGcd = !healAction->HadGcdAfterAccept();
    bool const rangeRejected = rangeExecutionFailed &&
        rangeCastResult == SPELL_FAILED_OUT_OF_RANGE &&
        rangeHealthAfter == rangeHealthBefore &&
        rangeManaAfter == rangeManaBefore &&
        healer->GetPower(POWER_MANA) == rangeManaBefore &&
        rangeNoPending && rangeNoGcd;

    // Evaluate against a fresh healthy-resource snapshot, then exhaust the
    // real Player's mana before the selected action is executed. The adapter
    // must surface the Core Spell::CheckPower failure without claiming success.
    uint32 const noPowerHealthBefore = noPowerPatient.creature->GetHealth();
    PartySnapshot noPowerSnapshot = makeSnapshot(11, noPowerPatient.creature, false);
    DecisionContext noPowerContext(noPowerSnapshot, noPowerSnapshot.members[0], coordinator, 11000);
    DecisionReport noPowerReport = engine.Evaluate(noPowerContext);
    bool const noPowerSelected = noPowerReport.selectedIndices.size() == 1 &&
        noPowerReport.candidates[noPowerReport.selectedIndices[0]].candidate.targetGuid ==
            noPowerPatient.creature->GetObjectGuid().GetRawValue() &&
        noPowerReport.candidates[noPowerReport.selectedIndices[0]].status == CandidateStatus::Selected;
    healer->SetPower(POWER_MANA, 0);
    if (noPowerSelected)
        engine.ExecuteSelected(noPowerContext, noPowerReport);
    bool const noPowerExecutionFailed = noPowerSelected &&
        noPowerReport.candidates[noPowerReport.selectedIndices[0]].status == CandidateStatus::ExecutionFailed &&
        noPowerReport.candidates[noPowerReport.selectedIndices[0]].actionResult == ActionResult::Failed;
    SpellCastResult const noPowerCastResult = healAction->GetLastCastResult();
    uint32 const noPowerHealthAfter = noPowerPatient.creature->GetHealth();
    uint32 const noPowerManaAfter = healer->GetPower(POWER_MANA);
    bool const noPowerNoPending = !healAction->WasCastPendingAfterAccept();
    bool const noPowerNoGcd = !healAction->HadGcdAfterAccept();
    bool const noPowerRejected = noPowerExecutionFailed &&
        noPowerCastResult == SPELL_FAILED_NO_POWER &&
        noPowerHealthAfter == noPowerHealthBefore &&
        noPowerManaAfter == 0 &&
        healAction->GetManaBefore() == 0 && healAction->GetManaAfterAccept() == 0 &&
        noPowerNoPending && noPowerNoGcd;

    healerSession.LogoutPlayer(false);
    bool const playerCleaned = healerSession.GetPlayer() == nullptr;
    healer = nullptr;
    bool cleaned = DestroySimulatorActor(map, noPowerPatient);
    cleaned = DestroySimulatorActor(map, outOfRangePatient) && cleaned;
    cleaned = playerCleaned && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Player heal execution revalidation: registered=%u rangeSelected=%u rangeExecutionFailed=%u rangeResult=%u rangeHealth=%u>%u rangeMana=%u>%u rangeNoPending=%u rangeNoGcd=%u noPowerSelected=%u noPowerExecutionFailed=%u noPowerResult=%u noPowerHealth=%u>%u noPowerMana=%u noPowerNoPending=%u noPowerNoGcd=%u cleanup=%u.",
             registered ? 1u : 0u, rangeSelected ? 1u : 0u,
             rangeExecutionFailed ? 1u : 0u, static_cast<uint32>(rangeCastResult),
             rangeHealthBefore, rangeHealthAfter, rangeManaBefore, rangeManaAfter,
             rangeNoPending ? 1u : 0u, rangeNoGcd ? 1u : 0u,
             noPowerSelected ? 1u : 0u, noPowerExecutionFailed ? 1u : 0u,
             static_cast<uint32>(noPowerCastResult),
             noPowerHealthBefore, noPowerHealthAfter, noPowerManaAfter,
             noPowerNoPending ? 1u : 0u, noPowerNoGcd ? 1u : 0u, cleaned ? 1u : 0u);

    if (!registered || !rangeRejected || !noPowerRejected || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Player heal execution-time range/resource revalidation failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Core Player CastSpell rejected a stale out-of-range heal and a late no-mana heal without applying effects, consuming mana, starting a cast or setting GCD.");
    return true;
}


bool RunEngineCorePlayerHealLosSmokeTest(Map& map)
{
    CreatureInfo const* creatureInfo = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!creatureInfo || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor patient;
    if (!CreateSimulatorActor(map, *creatureInfo, partyFaction, 20.0f, 6.0f, patient))
    {
        DestroySimulatorActor(map, patient);
        return false;
    }
    patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);

    WorldSession healerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* healer = nullptr;
    bool const healerCreated = CreateSimulatorPlayer(map, partyFaction, healerSession,
                                                       healer, 0.0f, 6.0f, 0.0f);
    if (!healerCreated)
    {
        if (healerSession.GetPlayer())
            healerSession.LogoutPlayer(false);
        else if (healer)
            delete healer;
        bool const cleaned = DestroySimulatorActor(map, patient);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] LOS Player fixture creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    bool const baseLineOfSight = healer->IsWithinLOSInMap(patient.creature);
    if (!baseLineOfSight)
    {
        healerSession.LogoutPlayer(false);
        healer = nullptr;
        bool const cleaned = DestroySimulatorActor(map, patient);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] LOS fixture has no clear baseline ray before obstacle placement (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    PartySnapshot snapshot;
    snapshot.groupId = 12;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 12;
    snapshot.capturedAtMs = 12000;

    PartyMemberSnapshot healerMember;
    healerMember.guid = healer->GetObjectGuid().GetRawValue();
    healerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    healerMember.mapId = map.GetId();
    healerMember.instanceId = map.GetInstanceId();
    healerMember.health = healer->GetHealth();
    healerMember.maxHealth = healer->GetMaxHealth();
    healerMember.online = true;
    healerMember.alive = healer->IsAlive();
    healerMember.x = healer->GetPositionX();
    healerMember.y = healer->GetPositionY();
    healerMember.z = healer->GetPositionZ();
    snapshot.members.push_back(healerMember);

    PartyMemberSnapshot patientMember;
    patientMember.guid = patient.creature->GetObjectGuid().GetRawValue();
    patientMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
    patientMember.mapId = map.GetId();
    patientMember.instanceId = map.GetInstanceId();
    patientMember.health = patient.creature->GetHealth();
    patientMember.maxHealth = patient.creature->GetMaxHealth();
    patientMember.online = true;
    patientMember.alive = patient.creature->IsAlive();
    patientMember.isMainTank = true;
    patientMember.x = patient.creature->GetPositionX();
    patientMember.y = patient.creature->GetPositionY();
    patientMember.z = patient.creature->GetPositionZ();
    snapshot.members.push_back(patientMember);

    PartyCoordinator coordinator;
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCorePlayerHealAction> healAction(new SimulatorCorePlayerHealAction(map));
    bool const registered = engine.RegisterAction(healAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_player_heal_los_rejection"));
    strategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>("core_player_heal"));
    engine.AddStrategy(strategy);

    DecisionContext context(snapshot, snapshot.members[0], coordinator, 12000);
    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == patientMember.guid &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;

    // Discover a real collision model from the loaded Vanilla gameobject
    // templates. Closed doors and solid generic models are inserted into the
    // Core dynamic collision tree; the fixture never substitutes a LOS flag.
    std::vector<GameObjectInfo const*> blockerTemplates;
    for (auto const& item : sObjectMgr.GetGameObjectInfoMap())
    {
        GameObjectInfo const* info = item.second.get();
        if (!info || !info->displayId || info->id != item.first)
            continue;

        bool const isDoor = info->type == GAMEOBJECT_TYPE_DOOR;
        bool const isNamedSolidGeneric = info->type == GAMEOBJECT_TYPE_GENERIC &&
            (info->name.find("wall") != std::string::npos ||
             info->name.find("Wall") != std::string::npos ||
             info->name.find("barrier") != std::string::npos ||
             info->name.find("Barrier") != std::string::npos ||
             info->name.find("gate") != std::string::npos ||
             info->name.find("Gate") != std::string::npos ||
             info->name.find("fence") != std::string::npos ||
             info->name.find("Fence") != std::string::npos);
        if (isDoor || isNamedSolidGeneric)
            blockerTemplates.push_back(info);
    }
    std::sort(blockerTemplates.begin(), blockerTemplates.end(),
        [](GameObjectInfo const* left, GameObjectInfo const* right)
        {
            return left->id < right->id;
        });

    GameObject* blocker = nullptr;
    uint32 blockerEntry = 0;
    uint32 testedModels = 0;
    float const blockerAngles[] = {0.0f, 1.57079632679f, 0.78539816339f, 2.35619449019f};
    bool blockerCleanupFailure = false;
    for (GameObjectInfo const* info : blockerTemplates)
    {
        if (testedModels >= 48 || blocker)
            break;

        for (float angle : blockerAngles)
        {
            if (testedModels >= 48)
                break;
            ++testedModels;
            GameObject* candidate = new GameObject();
            bool const created = candidate->Create(
                map.GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT), info->id, &map,
                10.0f, 6.0f, 0.0f, angle, 0.0f, 0.0f, 0.0f, 1.0f,
                GO_ANIMPROGRESS_DEFAULT, GO_STATE_READY);
            if (!created)
            {
                delete candidate;
                continue;
            }

            map.Add(candidate);
            ObjectGuid const candidateGuid = candidate->GetObjectGuid();
            bool const added = candidate->IsInWorld() &&
                map.GetGameObject(candidateGuid) == candidate;
            if (!added)
            {
                if (candidate->IsInWorld())
                    map.Remove(candidate, false);
                bool const detached = !candidate->IsInWorld() &&
                    map.GetGameObject(candidateGuid) == nullptr;
                if (detached)
                    delete candidate;
                else
                    blockerCleanupFailure = true;
                if (blockerCleanupFailure)
                    break;
                continue;
            }

            if (!healer->IsWithinLOSInMap(patient.creature))
            {
                blocker = candidate;
                blockerEntry = info->id;
                break;
            }

            map.Remove(candidate, false);
            bool const detached = !candidate->IsInWorld() &&
                map.GetGameObject(candidateGuid) == nullptr;
            if (detached)
                delete candidate;
            else
            {
                blockerCleanupFailure = true;
                break;
            }
        }

        if (blocker || blockerCleanupFailure)
            break;
    }

    bool const blockerFound = blocker != nullptr;
    bool const blockedLineOfSight = blockerFound &&
        !healer->IsWithinLOSInMap(patient.creature);
    uint32 const healthBefore = patient.creature->GetHealth();
    uint32 const manaBefore = healer->GetPower(POWER_MANA);
    if (selected && blockedLineOfSight)
        engine.ExecuteSelected(context, report);

    bool const executionFailed = selected &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::ExecutionFailed &&
        report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Failed;
    SpellCastResult const castResult = healAction->GetLastCastResult();
    uint32 const healthAfter = patient.creature->GetHealth();
    uint32 const manaAfter = healer->GetPower(POWER_MANA);
    bool const noPending = !healAction->WasCastPendingAfterAccept();
    bool const noGcd = !healAction->HadGcdAfterAccept();

    if (blocker && blocker->IsInWorld())
        map.Remove(blocker, false);
    ObjectGuid const blockerGuid = blocker ? blocker->GetObjectGuid() : ObjectGuid();
    bool const blockerDetached = blocker &&
        !blocker->IsInWorld() && map.GetGameObject(blockerGuid) == nullptr;
    if (blockerDetached)
        delete blocker;
    else if (blocker)
        blockerCleanupFailure = true;

    bool const lineOfSightRestored = blockerDetached &&
        healer->IsWithinLOSInMap(patient.creature);
    healerSession.LogoutPlayer(false);
    bool const playerCleaned = healerSession.GetPlayer() == nullptr;
    healer = nullptr;
    bool const patientCleaned = DestroySimulatorActor(map, patient);
    bool const cleaned = blockerFound && !blockerCleanupFailure && blockerDetached &&
        playerCleaned && patientCleaned;

    bool const rejected = registered && selected && blockerFound && blockedLineOfSight &&
        executionFailed && castResult == SPELL_FAILED_LINE_OF_SIGHT &&
        healthAfter == healthBefore && manaAfter == manaBefore &&
        noPending && noGcd && lineOfSightRestored && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Player heal LOS revalidation: registered=%u selected=%u baseLOS=%u blockerTemplates=%u testedModels=%u blockerFound=%u blockerEntry=%u blockedLOS=%u executionFailed=%u castResult=%u health=%u>%u mana=%u>%u noPending=%u noGcd=%u losRestored=%u cleanup=%u.",
             registered ? 1u : 0u, selected ? 1u : 0u, baseLineOfSight ? 1u : 0u,
             static_cast<uint32>(blockerTemplates.size()), testedModels,
             blockerFound ? 1u : 0u, blockerEntry, blockedLineOfSight ? 1u : 0u,
             executionFailed ? 1u : 0u, static_cast<uint32>(castResult),
             healthBefore, healthAfter, manaBefore, manaAfter,
             noPending ? 1u : 0u, noGcd ? 1u : 0u,
             lineOfSightRestored ? 1u : 0u, cleaned ? 1u : 0u);

    if (!rejected)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Player heal Core LOS rejection smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Core Player Spell::CheckCast rejected a heal through a real closed GameObject collision model without applying healing, spending mana or starting a cast.");
    return true;
}


bool RunEngineCoreMovePointControlSafetySmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor leader;
    SimulatorActor mover;
    bool const created =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 6.0f, leader) &&
        CreateSimulatorActor(map, *info, partyFaction, 10.0f, 6.0f, mover);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, mover);
        cleaned = DestroySimulatorActor(map, leader) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] MovePoint control-safety fixture creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    PartySnapshot initialSnapshot;
    initialSnapshot.groupId = 13;
    initialSnapshot.mapId = map.GetId();
    initialSnapshot.instanceId = map.GetInstanceId();
    initialSnapshot.epoch = 13;
    initialSnapshot.capturedAtMs = 13000;

    PartyMemberSnapshot moverMember;
    moverMember.guid = mover.creature->GetObjectGuid().GetRawValue();
    moverMember.roleId = static_cast<std::uint8_t>(PartyRole::MeleeDamage);
    moverMember.mapId = map.GetId();
    moverMember.instanceId = map.GetInstanceId();
    moverMember.health = mover.creature->GetHealth();
    moverMember.maxHealth = mover.creature->GetMaxHealth();
    moverMember.online = true;
    moverMember.alive = mover.creature->IsAlive();
    moverMember.x = mover.creature->GetPositionX();
    moverMember.y = mover.creature->GetPositionY();
    moverMember.z = mover.creature->GetPositionZ();
    initialSnapshot.members.push_back(moverMember);

    PartyMemberSnapshot leaderMember;
    leaderMember.guid = leader.creature->GetObjectGuid().GetRawValue();
    leaderMember.roleId = static_cast<std::uint8_t>(PartyRole::MainTank);
    leaderMember.mapId = map.GetId();
    leaderMember.instanceId = map.GetInstanceId();
    leaderMember.health = leader.creature->GetHealth();
    leaderMember.maxHealth = leader.creature->GetMaxHealth();
    leaderMember.online = true;
    leaderMember.alive = leader.creature->IsAlive();
    leaderMember.isMainTank = true;
    leaderMember.x = leader.creature->GetPositionX();
    leaderMember.y = leader.creature->GetPositionY();
    leaderMember.z = leader.creature->GetPositionZ();
    initialSnapshot.members.push_back(leaderMember);

    PartyCoordinator coordinator;
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCoreMovePointAction> moveAction(new SimulatorCoreMovePointAction(map));
    bool const registered = engine.RegisterAction(moveAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_move_control_safety"));
    strategy->AddTrigger(std::make_shared<SimulatorFormationTrigger>());
    engine.AddStrategy(strategy);

    float const initialX = mover.creature->GetPositionX();
    float const initialY = mover.creature->GetPositionY();
    float const initialZ = mover.creature->GetPositionZ();

    mover.creature->AddUnitState(UNIT_STATE_CAN_NOT_MOVE);
    PartySnapshot rootedSnapshot = initialSnapshot;
    rootedSnapshot.epoch = 13;
    rootedSnapshot.capturedAtMs = 13000;
    DecisionContext rootedContext(rootedSnapshot, rootedSnapshot.members[0], coordinator, 13000);
    DecisionReport rootedReport = engine.Evaluate(rootedContext);
    bool const rootedRejected = rootedReport.selectedIndices.empty() &&
        rootedReport.candidates.size() == 1 &&
        rootedReport.candidates[0].candidate.actionName == "core_move_point" &&
        rootedReport.candidates[0].status == CandidateStatus::RejectedImpossible;

    mover.creature->ClearUnitState(UNIT_STATE_CAN_NOT_MOVE);
    mover.creature->AddUnitState(UNIT_STATE_POSSESSED);
    PartySnapshot possessedSnapshot = initialSnapshot;
    possessedSnapshot.epoch = 14;
    possessedSnapshot.capturedAtMs = 14000;
    DecisionContext possessedContext(possessedSnapshot, possessedSnapshot.members[0], coordinator, 14000);
    DecisionReport possessedReport = engine.Evaluate(possessedContext);
    bool const possessedRejected = possessedReport.selectedIndices.empty() &&
        possessedReport.candidates.size() == 1 &&
        possessedReport.candidates[0].candidate.actionName == "core_move_point" &&
        possessedReport.candidates[0].status == CandidateStatus::RejectedImpossible;

    mover.creature->ClearUnitState(UNIT_STATE_POSSESSED);
    bool const noMovementStarted =
        mover.creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE;
    bool const positionUnchanged =
        mover.creature->GetPositionX() == initialX &&
        mover.creature->GetPositionY() == initialY &&
        mover.creature->GetPositionZ() == initialZ;

    bool cleaned = DestroySimulatorActor(map, mover);
    cleaned = DestroySimulatorActor(map, leader) && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] MovePoint control safety: registered=%u rootedRejected=%u possessedRejected=%u noMovementStarted=%u positionUnchanged=%u cleanup=%u.",
             registered ? 1u : 0u, rootedRejected ? 1u : 0u,
             possessedRejected ? 1u : 0u, noMovementStarted ? 1u : 0u,
             positionUnchanged ? 1u : 0u, cleaned ? 1u : 0u);

    if (!registered || !rootedRejected || !possessedRejected ||
        !noMovementStarted || !positionUnchanged || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] MovePoint control-safety smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine refused Core MovePoint while the mover had CAN_NOT_MOVE or POSSESSED state.");
    return true;
}


bool RunEngineCoreRealGroupThreatSnapshotSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    std::vector<std::unique_ptr<WorldSession>> sessions;
    std::vector<Player*> players;
    std::vector<ObjectGuid> playerGuids;
    SimulatorActor hostiles[2];
    SimulatorActor outsideGroupVictim;
    std::unique_ptr<Pet> primaryPet;
    Guid primaryPetGuid = 0;
    bool primaryPetCreated = false;
    bool petCleanupPassed = true;
    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;

    auto cleanup = [&]() -> bool
    {
        Player* const petOwner = players.size() > 1 ? players[1] : nullptr;
        petCleanupPassed = DestroySimulatorPrimaryPet(map, petOwner, primaryPet) &&
            petCleanupPassed;
        bool actorsCleaned = DestroySimulatorActor(map, hostiles[0]);
        actorsCleaned = DestroySimulatorActor(map, hostiles[1]) && actorsCleaned;
        actorsCleaned = DestroySimulatorActor(map, outsideGroupVictim) && actorsCleaned;

        bool ungrouped = true;
        if (group && group->GetId())
        {
            ObjectGuid initiator = players.empty()
                ? ObjectGuid() : players.back()->GetObjectGuid();
            group->Disband(true, initiator);
            for (Player* player : players)
                if (player && player->GetGroup())
                    ungrouped = false;

            if (groupRegistered)
            {
                sObjectMgr.RemoveGroup(group.get());
                groupRegistered = false;
            }
            group.reset();
        }

        bool sessionsCleaned = true;
        for (std::unique_ptr<WorldSession>& session : sessions)
        {
            if (session && session->GetPlayer())
                session->LogoutPlayer(false);
            if (session && session->GetPlayer())
                sessionsCleaned = false;
        }

        bool playersDetachedFromMap = true;
        for (ObjectGuid const& guid : playerGuids)
            if (map.GetUnit(guid))
                playersDetachedFromMap = false;

        players.clear();
        sessions.clear();
        return petCleanupPassed && actorsCleaned && ungrouped &&
            sessionsCleaned && playersDetachedFromMap;
    };

    // Five real Core Players, all on the same test Map. The first is assigned
    // Main Tank through the actual Group API; the second is the healer.
    for (uint32 index = 0; index < MAX_GROUP_SIZE; ++index)
    {
        std::unique_ptr<WorldSession> session(
            new WorldSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS));
        Player* player = nullptr;
        bool const created = CreateSimulatorPlayer(
            map, partyFaction, *session, player,
            static_cast<float>(index) * 2.0f, 6.0f, 0.0f);
        sessions.push_back(std::move(session));
        if (player)
        {
            players.push_back(player);
            playerGuids.push_back(player->GetObjectGuid());
        }
        if (!created || !player || !player->IsInWorld())
        {
            bool const cleaned = cleanup();
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[PartyBotSimulator] Group threat Player creation failed at member %u (cleanup=%u).",
                     index, cleaned ? 1u : 0u);
            return false;
        }
    }

    bool const groupCreated = group->Create(
        players[0]->GetObjectGuid(), players[0]->GetName());
    if (!groupCreated)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Group threat Group::Create failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }
    sObjectMgr.AddGroup(group.get());
    groupRegistered = true;

    bool membersAdded = group->GetMembersCount() == 1 &&
        players[0]->GetGroup() == group.get();
    for (std::size_t index = 1; index < players.size(); ++index)
    {
        bool const added = group->AddMember(
            players[index]->GetObjectGuid(), players[index]->GetName());
        membersAdded = membersAdded && added &&
            players[index]->GetGroup() == group.get();
        if (!added)
            break;
    }

    if (membersAdded)
    {
        group->ConvertToRaid();
        group->SetMainTank(players[0]->GetObjectGuid());
    }
    bool const mainTankAssigned = membersAdded &&
        group->GetMainTankGuid() == players[0]->GetObjectGuid();

    bool const allPlayersOnSameMap = std::all_of(
        players.begin(), players.end(),
        [&map](Player const* player)
        {
            return player && player->IsInWorld() && player->GetMap() == &map &&
                player->GetMapId() == map.GetId() &&
                player->GetInstanceId() == map.GetInstanceId();
        });

    // F8's roster/value/cache checks are part of this same Group fixture.
    // Do not create and tear down a second five-Player Group on this test Map.
    PartyCoordinator coordinator;
    PartySnapshot const* initialSnapshot = nullptr;
    bool const formProbeApplied = membersAdded && players[0];
    if (formProbeApplied)
        players[0]->SetShapeshiftForm(FORM_CAT);
    if (membersAdded && mainTankAssigned && allPlayersOnSameMap)
        initialSnapshot = PartySnapshotBuilder::GetOrBuild(players[0], coordinator, 15000, 250);
    PartyMemberSnapshot const* initialTankState = initialSnapshot
        ? initialSnapshot->FindMember(players[0]->GetObjectGuid().GetRawValue()) : nullptr;
    bool const initialFormCopied = initialTankState &&
        initialTankState->shapeshiftForm == static_cast<std::uint8_t>(FORM_CAT);
    if (formProbeApplied)
        players[0]->SetShapeshiftForm(FORM_NONE);

    bool const initialSnapshotValid = initialSnapshot &&
        initialSnapshot->groupId == group->GetId() &&
        initialSnapshot->mapId == map.GetId() &&
        initialSnapshot->instanceId == map.GetInstanceId() &&
        initialSnapshot->members.size() == MAX_GROUP_SIZE;
    bool initialRosterResolved = initialSnapshotValid;
    bool initialValuesCopied = initialSnapshotValid;
    bool initialRolesUnknown = initialSnapshotValid;
    for (Player* player : players)
    {
        PartyMemberSnapshot const* member = initialSnapshot
            ? initialSnapshot->FindMember(player->GetObjectGuid().GetRawValue()) : nullptr;
        bool const memberValid = member && member->online && member->alive &&
            member->mapId == map.GetId() && member->instanceId == map.GetInstanceId() &&
            member->classId == CLASS_PRIEST && member->level == player->GetLevel() &&
            member->health == player->GetHealth() &&
            member->maxHealth == player->GetMaxHealth();
        initialRosterResolved = initialRosterResolved && memberValid;
        if (member)
        {
            initialValuesCopied = initialValuesCopied &&
                member->power == player->GetPower(player->GetPowerType()) &&
                member->maxPower == player->GetMaxPower(player->GetPowerType()) &&
                member->x == player->GetPositionX() &&
                member->y == player->GetPositionY() &&
                member->z == player->GetPositionZ() &&
                member->orientation == player->GetOrientation();
            initialRolesUnknown = initialRolesUnknown &&
                member->roleId == static_cast<std::uint8_t>(PartyRole::Unknown);
        }
        else
            initialValuesCopied = false;
    }

    PartySnapshot const* cachedSnapshot =
        PartySnapshotBuilder::GetOrBuild(players[0], coordinator, 15100, 250);
    bool const initialCacheRetained = cachedSnapshot == initialSnapshot &&
        cachedSnapshot && cachedSnapshot->epoch == initialSnapshot->epoch &&
        cachedSnapshot->capturedAtMs == 15000;

    bool actorsCreated = false;
    if (membersAdded && mainTankAssigned && allPlayersOnSameMap &&
        initialSnapshotValid && initialRosterResolved && initialValuesCopied &&
        initialRolesUnknown && initialCacheRetained)
        // Keep the first hostile within Core melee reach of Tank and Healer,
        // while the second remains outside reach of both. The same four
        // references therefore exercise both true and false snapshot values.
        actorsCreated =
            CreateSimulatorActor(map, *info, hostileFaction, 2.0f, 10.0f, hostiles[0]) &&
            CreateSimulatorActor(map, *info, hostileFaction, 16.0f, 6.0f, hostiles[1]) &&
            CreateSimulatorActor(map, *info, partyFaction, 40.0f, 6.0f, outsideGroupVictim);

    if (!actorsCreated)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Group threat hostile creation failed (members=%u mainTank=%u cleanup=%u).",
                 membersAdded ? 1u : 0u, mainTankAssigned ? 1u : 0u,
                 cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120 &&
         (!hostiles[0].updateCount || !hostiles[1].updateCount ||
          !outsideGroupVictim.updateCount); ++tick)
    {
        map.Update(50);
        if (!hostiles[0].updateCount || !hostiles[1].updateCount ||
            !outsideGroupVictim.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    bool const hostileAIReady = hostiles[0].updateCount && hostiles[1].updateCount &&
        outsideGroupVictim.updateCount;

    Player* tank = players[0];
    Player* healer = players[1];
    ObjectGuid const tankGuid = tank->GetObjectGuid();
    ObjectGuid const healerGuid = healer->GetObjectGuid();

    // Start both hostiles on the real Main Tank and create their threat through
    // actual Core damage. No threat list or victim pointer is written here.
    if (hostileAIReady)
    {
        hostiles[0].creature->AI()->AttackStart(tank);
        hostiles[1].creature->AI()->AttackStart(tank);
    }
    uint32 const tankDamage0 = hostileAIReady
        ? tank->DealDamage(hostiles[0].creature, 100, nullptr, DIRECT_DAMAGE,
                           SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    uint32 const tankDamage1 = hostileAIReady
        ? tank->DealDamage(hostiles[1].creature, 100, nullptr, DIRECT_DAMAGE,
                           SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;

    float const tankThreat0 = hostiles[0].creature->GetThreatManager().getThreat(tank);
    float const tankThreat1 = hostiles[1].creature->GetThreatManager().getThreat(tank);
    Unit* const initialVictim0 = hostiles[0].creature->GetThreatManager().getHostileTarget();
    Unit* const initialVictim1 = hostiles[1].creature->GetThreatManager().getHostileTarget();
    bool const bothHostilesInitiallyTargetTank =
        tankDamage0 > 0 && tankDamage1 > 0 &&
        initialVictim0 == tank && initialVictim1 == tank &&
        tankThreat0 > 0.0f && tankThreat1 > 0.0f;

    // Create a real Core Pet owned by the Healer. The hostile gets threat on
    // the Pet GUID itself; Core may also create the owner's zero-threat link.
    // The Tank remains current victim because the Pet starts far below 110%.
    bool const primaryPetEligible = bothHostilesInitiallyTargetTank &&
        players.size() > 1 && players[1] == healer;
    primaryPetCreated = primaryPetEligible &&
        CreateSimulatorPrimaryPet(map, *healer, *info, primaryPet);
    float primaryPetThreat = 0.0f;
    if (primaryPetCreated)
    {
        primaryPetGuid = primaryPet->GetObjectGuid().GetRawValue();
        hostiles[0].creature->AddThreat(
            primaryPet.get(), 30.0f, false, SPELL_SCHOOL_MASK_NORMAL, nullptr);
        primaryPetThreat =
            hostiles[0].creature->GetThreatManager().getThreat(primaryPet.get());
    }

    // Give the Healer a real, deliberately tiny assist-threat reference on
    // each add. The resulting snapshot has a current victim plus a non-victim
    // threat owner, so the Engine can assess both 110%/130% Core headrooms.
    float healerThreatBeforeBudget0 = 0.0f;
    float healerThreatBeforeBudget1 = 0.0f;
    bool threatBudgetSnapshotValid = false;
    bool overBudgetRejected = false;
    bool safeBudgetSelectedAndReserved = false;
    bool executionReservationRaceRejected = false;
    bool enginePreservedCoreThreatState = false;
    bool enginePreservedCoreVictims = false;
    bool reservationReleased = false;
    bool companionSnapshotValid = false;
    bool companionBudgetValid = false;
    float primaryPetHeadroom = 0.0f;
    bool primaryPetCoreMeleeReachable = false;

    if (bothHostilesInitiallyTargetTank)
    {
        tank->GetHostileRefManager().threatAssist(healer, 1.0f);
        healerThreatBeforeBudget0 = hostiles[0].creature->GetThreatManager().getThreat(healer);
        healerThreatBeforeBudget1 = hostiles[1].creature->GetThreatManager().getThreat(healer);

        coordinator.InvalidateSnapshot();
        PartySnapshot const* budgetSnapshot =
            PartySnapshotBuilder::GetOrBuild(tank, coordinator, 15500, 250);
        PartyMemberSnapshot const* healerForBudget = budgetSnapshot
            ? budgetSnapshot->FindMember(healerGuid.GetRawValue()) : nullptr;
        HostileSnapshot const* firstForBudget = budgetSnapshot
            ? budgetSnapshot->FindHostile(hostiles[0].creature->GetObjectGuid().GetRawValue()) : nullptr;
        HostileSnapshot const* secondForBudget = budgetSnapshot
            ? budgetSnapshot->FindHostile(hostiles[1].creature->GetObjectGuid().GetRawValue()) : nullptr;
        PartyCompanionSnapshot const* petForBudget = budgetSnapshot && primaryPetGuid
            ? budgetSnapshot->FindCompanion(primaryPetGuid) : nullptr;
        ThreatEntrySnapshot const* petThreatForBudget = nullptr;
        uint32 petThreatRows = 0;
        if (firstForBudget) {
            for (ThreatEntrySnapshot const& entry : firstForBudget->threat) {
                if (entry.unitGuid == primaryPetGuid) {
                    ++petThreatRows;
                    petThreatForBudget = &entry;
                }
            }
        }
        primaryPetCoreMeleeReachable = primaryPetCreated &&
            hostiles[0].creature->CanReachWithMeleeAutoAttack(primaryPet.get());

        ThreatBudgetAssessment const petAssessment =
            budgetSnapshot && primaryPetGuid
                ? AssessThreatBudget(*budgetSnapshot,
                    hostiles[0].creature->GetObjectGuid().GetRawValue(),
                    primaryPetGuid, 15500, 250)
                : ThreatBudgetAssessment();
        primaryPetHeadroom = petAssessment.rawHeadroom;
        companionSnapshotValid = primaryPetCreated && budgetSnapshot &&
            budgetSnapshot->companions.size() == 1 && petForBudget &&
            petForBudget->guid == primaryPetGuid &&
            petForBudget->ownerGuid == healerGuid.GetRawValue() &&
            petForBudget->online && petForBudget->alive &&
            petForBudget->mapId == map.GetId() &&
            petForBudget->instanceId == map.GetInstanceId() &&
            primaryPet->GetOwnerGuid().GetRawValue() == healerGuid.GetRawValue() &&
            petThreatForBudget && petThreatRows == 1 &&
            std::fabs(petThreatForBudget->threat - primaryPetThreat) < 0.01f &&
            petThreatForBudget->online && petThreatForBudget->accessible &&
            petThreatForBudget->meleeReachable == primaryPetCoreMeleeReachable &&
            primaryPet->GetHostileRefManager().getFirst() != nullptr;
        companionBudgetValid = companionSnapshotValid &&
            petAssessment.status == ThreatBudgetStatus::BudgetAvailable &&
            petAssessment.currentVictimGuid == tankGuid &&
            std::fabs(petAssessment.memberThreat - primaryPetThreat) < 0.01f &&
            petAssessment.usesMeleeThreshold == primaryPetCoreMeleeReachable &&
            std::fabs(petAssessment.thresholdMultiplier -
                (primaryPetCoreMeleeReachable ? 1.1f : 1.3f)) < 0.0001f &&
            petAssessment.rawHeadroom > 0.0f &&
            hostiles[0].creature->GetThreatManager().getHostileTarget() == tank;

        ThreatBudgetAssessment const firstAssessment = budgetSnapshot && healerForBudget
            ? AssessThreatBudget(*budgetSnapshot, hostiles[0].creature->GetObjectGuid().GetRawValue(),
                                 healerGuid.GetRawValue(), 15500, 250)
            : ThreatBudgetAssessment();
        ThreatBudgetAssessment const secondAssessment = budgetSnapshot && healerForBudget
            ? AssessThreatBudget(*budgetSnapshot, hostiles[1].creature->GetObjectGuid().GetRawValue(),
                                 healerGuid.GetRawValue(), 15500, 250)
            : ThreatBudgetAssessment();

        threatBudgetSnapshotValid = budgetSnapshot && budgetSnapshot->epoch &&
            budgetSnapshot->members.size() == MAX_GROUP_SIZE &&
            budgetSnapshot->hostiles.size() == 2 && healerForBudget &&
            healerForBudget->online && healerForBudget->alive &&
            firstForBudget && secondForBudget &&
            firstAssessment.status == ThreatBudgetStatus::BudgetAvailable &&
            secondAssessment.status == ThreatBudgetStatus::BudgetAvailable &&
            std::fabs(firstAssessment.memberThreat - healerThreatBeforeBudget0) < 0.01f &&
            std::fabs(secondAssessment.memberThreat - healerThreatBeforeBudget1) < 0.01f &&
            std::fabs(firstAssessment.currentVictimThreat - tankThreat0) < 0.01f &&
            std::fabs(secondAssessment.currentVictimThreat - tankThreat1) < 0.01f;

        if (threatBudgetSnapshotValid)
        {
            DecisionContext budgetContext(*budgetSnapshot, *healerForBudget, coordinator, 15500);

            // Estimate the amount through the same pinned-Core ThreatCalcHelper
            // used by ThreatManager, not a hand-coded damage == threat assumption.
            auto estimateThreatForDamage = [&](Guid hostileGuid, uint32 damage) -> float
            {
                SimulatorThreatBudgetProbeAction estimator(map);
                ActionCandidate candidate;
                candidate.actionName = "core_threat_budget_probe";
                candidate.targetGuid = hostileGuid;
                candidate.spellId = damage;
                ThreatImpactEstimate const estimate =
                    estimator.EstimateThreatImpact(budgetContext, candidate);
                return estimate.impacts.size() == 1
                    ? estimate.impacts[0].threatPoints : -1.0f;
            };

            // Probe the ranged 130% threshold with an estimate that must exceed
            // available headroom. The evaluator must reject before Execute.
            uint32 overDamage = 1;
            float overEstimatedThreat = estimateThreatForDamage(
                hostiles[1].creature->GetObjectGuid().GetRawValue(), overDamage);
            while (overEstimatedThreat >= 0.0f &&
                   overEstimatedThreat <= secondAssessment.rawHeadroom &&
                   overDamage < 100000u)
            {
                overDamage *= 2;
                overEstimatedThreat = estimateThreatForDamage(
                    hostiles[1].creature->GetObjectGuid().GetRawValue(), overDamage);
            }

            PartyBotEngine overBudgetEngine;
            std::shared_ptr<SimulatorThreatBudgetProbeAction> overBudgetAction(
                new SimulatorThreatBudgetProbeAction(map));
            bool const overRegistered = overBudgetEngine.RegisterAction(overBudgetAction);
            std::shared_ptr<Strategy> overStrategy(new Strategy("simulator_over_budget_probe"));
            overStrategy->AddTrigger(std::make_shared<SimulatorThreatBudgetProbeTrigger>(
                hostiles[1].creature->GetObjectGuid().GetRawValue(), overDamage));
            overBudgetEngine.AddStrategy(overStrategy);
            DecisionReport overReport = overBudgetEngine.Evaluate(budgetContext);
            overBudgetRejected = overRegistered && overEstimatedThreat > secondAssessment.rawHeadroom &&
                overReport.selectedIndices.empty() && overReport.candidates.size() == 1 &&
                overReport.candidates[0].status == CandidateStatus::RejectedThreatBudget &&
                overBudgetAction->GetExecuteCount() == 0;

            // The melee 110% case uses a small direct-damage estimate. Execute
            // only reserves the declared impact; the test applies the actual
            // Core DealDamage immediately afterward and compares real threat.
            uint32 const safeDamage = 50;
            float const safeEstimatedThreat = estimateThreatForDamage(
                hostiles[0].creature->GetObjectGuid().GetRawValue(), safeDamage);

            // Evaluate first, then inject a competing reservation for the same
            // actual threat recipient before dispatch. ExecuteSelected must
            // re-check the live coordinator and refuse the now-unsafe action.
            std::shared_ptr<SimulatorThreatBudgetProbeAction> racedAction(
                new SimulatorThreatBudgetProbeAction(map));
            PartyBotEngine racedEngine;
            bool const racedRegistered = racedEngine.RegisterAction(racedAction);
            std::shared_ptr<Strategy> racedStrategy(new Strategy("simulator_threat_budget_race"));
            racedStrategy->AddTrigger(std::make_shared<SimulatorThreatBudgetProbeTrigger>(
                hostiles[0].creature->GetObjectGuid().GetRawValue(), safeDamage));
            racedEngine.AddStrategy(racedStrategy);
            DecisionReport racedReport = racedEngine.Evaluate(budgetContext);
            bool const racedCandidateSelected = racedRegistered &&
                racedReport.selectedIndices.size() == 1 &&
                racedReport.candidates.size() == 1 &&
                racedReport.candidates[0].status == CandidateStatus::Selected;

            ThreatActionId const competingActionId = coordinator.NextThreatActionId();
            float const competingThreat = firstAssessment.rawHeadroom -
                safeEstimatedThreat + 1.0f;
            ThreatReservation competingReservation;
            competingReservation.ownerGuid = healerGuid.GetRawValue();
            competingReservation.threatOwnerGuid = healerGuid.GetRawValue();
            competingReservation.hostileGuid =
                hostiles[0].creature->GetObjectGuid().GetRawValue();
            competingReservation.actionId = competingActionId;
            competingReservation.threatPoints = competingThreat;
            competingReservation.expiresAtMs = 15500 + 5000;
            bool const competingReservationAccepted = racedCandidateSelected &&
                competingActionId && std::isfinite(competingThreat) && competingThreat > 0.0f &&
                coordinator.TryReserveThreatImpact(competingReservation, 15500);

            float const healerThreatBeforeRace =
                hostiles[0].creature->GetThreatManager().getThreat(healer);
            float const tankThreatBeforeRace =
                hostiles[0].creature->GetThreatManager().getThreat(tank);
            Unit* const victimBeforeRace =
                hostiles[0].creature->GetThreatManager().getHostileTarget();
            if (competingReservationAccepted)
                racedEngine.ExecuteSelected(budgetContext, racedReport);

            CandidateEvaluation const* racedEvaluation = racedReport.candidates.empty()
                ? nullptr : &racedReport.candidates[0];
            bool const executionRejectedAfterRace = competingReservationAccepted &&
                racedEvaluation && racedEvaluation->status == CandidateStatus::ExecutionFailed &&
                racedEvaluation->actionResult == ActionResult::Failed &&
                racedAction->GetExecuteCount() == 0 &&
                racedEvaluation->threatActionId == 0 &&
                racedEvaluation->candidate.threatActionId == 0 &&
                std::fabs(hostiles[0].creature->GetThreatManager().getThreat(healer) -
                          healerThreatBeforeRace) < 0.01f &&
                std::fabs(hostiles[0].creature->GetThreatManager().getThreat(tank) -
                          tankThreatBeforeRace) < 0.01f &&
                hostiles[0].creature->GetThreatManager().getHostileTarget() == victimBeforeRace &&
                hostiles[0].creature->GetThreatManager().getHostileTarget() == tank &&
                std::fabs(coordinator.GetReservedThreatImpact(
                    competingReservation.hostileGuid,
                    healerGuid.GetRawValue(), 0, 15500) - competingThreat) < 0.01f;

            if (competingActionId)
                coordinator.ReleaseThreatAction(competingActionId);
            bool const competingReservationReleased = competingActionId &&
                coordinator.GetReservedThreatImpact(
                    competingReservation.hostileGuid,
                    healerGuid.GetRawValue(), 0, 15500) == 0.0f;
            executionReservationRaceRejected =
                executionRejectedAfterRace && competingReservationReleased;

            std::shared_ptr<SimulatorThreatBudgetProbeAction> safeBudgetAction(
                new SimulatorThreatBudgetProbeAction(map));
            PartyBotEngine safeEngine;
            bool const safeRegistered = safeEngine.RegisterAction(safeBudgetAction);
            std::shared_ptr<Strategy> safeStrategy(new Strategy("simulator_safe_budget_probe"));
            safeStrategy->AddTrigger(std::make_shared<SimulatorThreatBudgetProbeTrigger>(
                hostiles[0].creature->GetObjectGuid().GetRawValue(), safeDamage));
            safeEngine.AddStrategy(safeStrategy);
            DecisionReport safeReport = safeEngine.Evaluate(budgetContext);
            bool const safeSelected = safeRegistered && safeEstimatedThreat > 0.0f &&
                safeEstimatedThreat < firstAssessment.rawHeadroom &&
                safeReport.selectedIndices.size() == 1 &&
                safeReport.candidates[safeReport.selectedIndices[0]].status == CandidateStatus::Selected;

            float const healerThreatBeforeExecute =
                hostiles[0].creature->GetThreatManager().getThreat(healer);
            safeEngine.ExecuteSelected(budgetContext, safeReport);
            CandidateEvaluation const* safeEvaluation = safeReport.candidates.empty()
                ? nullptr : &safeReport.candidates[0];
            bool const actionAccepted = safeSelected && safeEvaluation &&
                safeEvaluation->status == CandidateStatus::Executed &&
                safeBudgetAction->GetExecuteCount() == 1 &&
                safeBudgetAction->GetLastThreatActionId() != 0 &&
                safeEvaluation->threatActionId == safeBudgetAction->GetLastThreatActionId() &&
                safeEvaluation->candidate.threatActionId == safeEvaluation->threatActionId &&
                std::fabs(coordinator.GetReservedThreatImpact(
                    hostiles[0].creature->GetObjectGuid().GetRawValue(),
                    healerGuid.GetRawValue(), 0, 15500) - safeEstimatedThreat) < 0.01f;

            // The Engine itself must not alter Core threat/victim state. Now
            // run the real Core damage operation and compare the exact delta.
            float const healerThreatImmediatelyAfterExecute =
                hostiles[0].creature->GetThreatManager().getThreat(healer);
            bool const noThreatMutationBeforeCoreDamage =
                std::fabs(healerThreatImmediatelyAfterExecute - healerThreatBeforeExecute) < 0.01f &&
                std::fabs(hostiles[0].creature->GetThreatManager().getThreat(tank) - tankThreat0) < 0.01f &&
                std::fabs(hostiles[1].creature->GetThreatManager().getThreat(tank) - tankThreat1) < 0.01f &&
                hostiles[0].creature->GetThreatManager().getHostileTarget() == tank &&
                hostiles[1].creature->GetThreatManager().getHostileTarget() == tank;

            uint32 const actualDamage = healer->DealDamage(
                hostiles[0].creature, safeDamage, nullptr, DIRECT_DAMAGE,
                SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            float const healerThreatAfterCoreDamage =
                hostiles[0].creature->GetThreatManager().getThreat(healer);
            float const actualThreatDelta =
                healerThreatAfterCoreDamage - healerThreatImmediatelyAfterExecute;
            bool const estimatorMatchesCore = actualDamage == safeDamage &&
                std::fabs(actualThreatDelta - safeEstimatedThreat) < 0.01f &&
                hostiles[0].creature->GetThreatManager().getHostileTarget() == tank;

            safeBudgetSelectedAndReserved = actionAccepted && estimatorMatchesCore;
            enginePreservedCoreThreatState = noThreatMutationBeforeCoreDamage;
            enginePreservedCoreVictims = noThreatMutationBeforeCoreDamage &&
                hostiles[0].creature->GetThreatManager().getHostileTarget() == tank &&
                hostiles[1].creature->GetThreatManager().getHostileTarget() == tank;

            if (safeEvaluation && safeEvaluation->threatActionId)
            {
                coordinator.ReleaseThreatAction(safeEvaluation->threatActionId);
                reservationReleased = coordinator.GetReservedThreatImpact(
                    hostiles[0].creature->GetObjectGuid().GetRawValue(),
                    healerGuid.GetRawValue(), 0, 15500) == 0.0f;
            }
        }
    }

    // The healer performs a normal non-triggered Lesser Heal. Before the cast,
    // estimate only its direct healing-assist component from the real Core
    // HostileRefManager fan-out and ThreatCalcHelper. The maximum possible
    // health gain is bounded by the target's currently missing health; this intentionally
    // conservative component does not claim to cover flat spell threat/procs.
    uint32 const patientHealthBeforeDamage = tank->GetHealth();
    uint32 const scriptedTankDamage = std::max<uint32>(1u, tank->GetMaxHealth() / 5u);
    hostiles[0].creature->DealDamage(
        tank, scriptedTankDamage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    uint32 const patientHealthBeforeHeal = tank->GetHealth();
    bool const tankAliveAndInjured = tank->IsAlive() && patientHealthBeforeHeal > 0 &&
        patientHealthBeforeHeal < patientHealthBeforeDamage;
    uint32 const healerManaBefore = healer->GetPower(POWER_MANA);
    SpellEntry const* const healSpell = sSpellMgr.GetSpellEntry(SIMULATOR_HEAL_SPELL_ID);
    coordinator.InvalidateSnapshot();
    PartySnapshot const* healAssistSnapshot =
        PartySnapshotBuilder::GetOrBuild(tank, coordinator, 15600, 250);
    ThreatImpactEstimate const healAssistComponent =
        healAssistSnapshot && healSpell
            ? EstimateDirectHealingAssistComponent(
                  healer, tank,
                  static_cast<float>(tank->GetMaxHealth() - patientHealthBeforeHeal),
                  healSpell, *healAssistSnapshot)
            : ThreatImpactEstimate();
    auto findHealAssistBound = [&](Guid hostileGuid) -> float
    {
        for (ThreatImpactEntry const& impact : healAssistComponent.impacts)
            if (impact.threatOwnerGuid == healerGuid.GetRawValue() &&
                impact.hostileGuid == hostileGuid &&
                impact.source == ThreatImpactSource::DirectHealAssist &&
                impact.bound == ThreatImpactBound::UpperBound)
                return impact.threatPoints;
        return -1.0f;
    };
    float const healAssistUpperBound0 =
        findHealAssistBound(hostiles[0].creature->GetObjectGuid().GetRawValue());
    float const healAssistUpperBound1 =
        findHealAssistBound(hostiles[1].creature->GetObjectGuid().GetRawValue());
    float const healerThreatBeforeHeal0 =
        hostiles[0].creature->GetThreatManager().getThreat(healer);
    float const healerThreatBeforeHeal1 =
        hostiles[1].creature->GetThreatManager().getThreat(healer);
    healer->SetFacingToObject(tank);
    SpellCastResult const healCastResult = healSpell && tankAliveAndInjured
        ? healer->CastSpell(tank, healSpell, false) : SPELL_FAILED_UNKNOWN;

    // Capture the real pending Core cast before Player::Update finishes it.
    PartySnapshot f21CastStateTraceSnapshot;
    bool currentCastSnapshotValid = false;
    if (healCastResult == SPELL_CAST_OK)
    {
        // Use an isolated coordinator: the F9 heal-assist estimate retains the
        // original coordinator snapshot epoch until its own validation ends.
        PartyCoordinator f21Coordinator;
        // Keep the tank in Cat Form only for this immutable snapshot sample;
        // restore the live Player immediately before normal Core updates.
        tank->SetShapeshiftForm(FORM_CAT);
        PartySnapshot const* castStateSnapshot =
            PartySnapshotBuilder::GetOrBuild(healer, f21Coordinator, 15601, 0);
        PartyMemberSnapshot const* healerCastState = castStateSnapshot
            ? castStateSnapshot->FindMember(healerGuid.GetRawValue()) : nullptr;
        PartyMemberSnapshot const* tankCastState = castStateSnapshot
            ? castStateSnapshot->FindMember(tankGuid.GetRawValue()) : nullptr;
        currentCastSnapshotValid = healerCastState && tankCastState &&
            healerCastState->currentSpellId == healSpell->Id &&
            healerCastState->currentSpellType == CURRENT_GENERIC_SPELL &&
            healerCastState->currentSpellState != 0 &&
            healerCastState->currentSpellTargetGuid == tankGuid.GetRawValue() &&
            tankCastState->shapeshiftForm == static_cast<std::uint8_t>(FORM_CAT);
        if (castStateSnapshot)
            f21CastStateTraceSnapshot = *castStateSnapshot;
        tank->SetShapeshiftForm(FORM_NONE);
    }

    for (uint32 tick = 0; tick < 20 &&
         healer->GetCurrentSpell(CURRENT_GENERIC_SPELL); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }
    uint32 const patientHealthAfterHeal = tank->GetHealth();
    uint32 const healerManaAfter = healer->GetPower(POWER_MANA);
    bool const healCompleted = tankAliveAndInjured && healCastResult == SPELL_CAST_OK &&
        tank->IsAlive() && !healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
        patientHealthAfterHeal > patientHealthBeforeHeal &&
        healerManaAfter < healerManaBefore;
    float const healThreatDelta0 =
        hostiles[0].creature->GetThreatManager().getThreat(healer) - healerThreatBeforeHeal0;
    float const healThreatDelta1 =
        hostiles[1].creature->GetThreatManager().getThreat(healer) - healerThreatBeforeHeal1;
    SpellThreatEntry const* const healThreatEntry =
        healSpell ? sSpellMgr.GetSpellThreatEntry(healSpell->Id) : nullptr;
    bool const noFlatSpellThreat = !healThreatEntry || healThreatEntry->threat == 0.0f;
    bool const healAssistBoundValid = healCompleted && noFlatSpellThreat &&
        healAssistSnapshot && healAssistComponent.completeness == ThreatImpactCompleteness::Unknown &&
        IsThreatImpactEstimateForSnapshot(healAssistComponent, healAssistSnapshot->epoch) &&
        healAssistComponent.impacts.size() == 2 &&
        healAssistUpperBound0 >= 0.0f && healAssistUpperBound1 >= 0.0f &&
        patientHealthAfterHeal - patientHealthBeforeHeal <= tank->GetMaxHealth() &&
        healThreatDelta0 >= -0.01f && healThreatDelta1 >= -0.01f &&
        healThreatDelta0 <= healAssistUpperBound0 + 0.01f &&
        healThreatDelta1 <= healAssistUpperBound1 + 0.01f;

    uint32 hostileRefsOnTank = 0;
    for (HostileReference* reference = tank->GetHostileRefManager().getFirst();
         reference; reference = reference->next())
        ++hostileRefsOnTank;
    float constexpr kScriptedAssistThreat = 400.0f;
    if (healCompleted && hostileRefsOnTank == 2)
        tank->GetHostileRefManager().threatAssist(healer, kScriptedAssistThreat);

    float const healerThreat0 = hostiles[0].creature->GetThreatManager().getThreat(healer);
    float const healerThreat1 = hostiles[1].creature->GetThreatManager().getThreat(healer);
    Unit* const selectedAfterAssist0 =
        hostiles[0].creature->GetThreatManager().getHostileTarget();
    Unit* const selectedAfterAssist1 =
        hostiles[1].creature->GetThreatManager().getHostileTarget();
    bool const healerPulledBothAdds = healCompleted && hostileRefsOnTank == 2 &&
        healerThreat0 > tankThreat0 * 1.3f &&
        healerThreat1 > tankThreat1 * 1.3f &&
        selectedAfterAssist0 == healer && selectedAfterAssist1 == healer;

    // Let the Creature AI consume the Core-selected target so the production
    // snapshot builder observes the actual current victim and attacker refs.
    if (healerPulledBothAdds)
    {
        hostiles[0].creature->AI()->AttackStart(healer);
        hostiles[1].creature->AI()->AttackStart(healer);
    }

    PartySnapshot const* snapshot = nullptr;
    if (healerPulledBothAdds)
        snapshot = PartySnapshotBuilder::GetOrBuild(tank, coordinator, 24000, 250);

    bool const snapshotValid = snapshot &&
        snapshot->groupId == group->GetId() &&
        snapshot->mapId == map.GetId() &&
        snapshot->instanceId == map.GetInstanceId() &&
        snapshot->members.size() == MAX_GROUP_SIZE &&
        snapshot->hostiles.size() == 2;
    PartyMemberSnapshot const* tankSnapshot = snapshot
        ? snapshot->FindMember(tankGuid.GetRawValue()) : nullptr;
    PartyMemberSnapshot const* healerSnapshot = snapshot
        ? snapshot->FindMember(healerGuid.GetRawValue()) : nullptr;
    bool const rosterRoleStateValid = tankSnapshot && healerSnapshot &&
        tankSnapshot->isMainTank && tankSnapshot->alive &&
        healerSnapshot->alive && tankSnapshot->classId == CLASS_PRIEST &&
        healerSnapshot->classId == CLASS_PRIEST;

    auto containsThreat = [](HostileSnapshot const* hostile, Guid guid, float expected)
    {
        if (!hostile)
            return false;
        for (ThreatEntrySnapshot const& threat : hostile->threat)
            if (threat.unitGuid == guid)
                return threat.online && threat.accessible &&
                    std::fabs(threat.threat - expected) < 0.01f;
        return false;
    };

    HostileSnapshot const* bossSnapshot = snapshot
        ? snapshot->FindHostile(hostiles[0].creature->GetObjectGuid().GetRawValue()) : nullptr;
    HostileSnapshot const* addSnapshot = snapshot
        ? snapshot->FindHostile(hostiles[1].creature->GetObjectGuid().GetRawValue()) : nullptr;
    bool const bothAddsResolved = bossSnapshot && addSnapshot &&
        bossSnapshot->alive && addSnapshot->alive &&
        bossSnapshot->victimGuid == healerGuid.GetRawValue() &&
        addSnapshot->victimGuid == healerGuid.GetRawValue();
    bool const hostileIdentityCopied = bossSnapshot && addSnapshot &&
        bossSnapshot->entry == hostiles[0].creature->GetCreatureInfo()->entry &&
        bossSnapshot->rank == hostiles[0].creature->GetCreatureInfo()->rank &&
        bossSnapshot->worldBoss == hostiles[0].creature->IsWorldBoss() &&
        addSnapshot->entry == hostiles[1].creature->GetCreatureInfo()->entry &&
        addSnapshot->rank == hostiles[1].creature->GetCreatureInfo()->rank &&
        addSnapshot->worldBoss == hostiles[1].creature->IsWorldBoss();
    bool const threatValuesCopied = containsThreat(
            bossSnapshot, tankGuid.GetRawValue(), tankThreat0) &&
        containsThreat(bossSnapshot, healerGuid.GetRawValue(), healerThreat0) &&
        containsThreat(addSnapshot, tankGuid.GetRawValue(), tankThreat1) &&
        containsThreat(addSnapshot, healerGuid.GetRawValue(), healerThreat1);

    uint32 const groupId = group->GetId();
    bool const registeredBeforeCleanup = groupRegistered &&
        sObjectMgr.GetGroupById(groupId) == group.get();
    uint32 meleeReachabilityChecks = 0;
    uint32 meleeReachableChecks = 0;
    uint32 meleeUnreachableChecks = 0;
    auto snapshotMeleeReachabilityMatchesCore = [&meleeReachabilityChecks,
        &meleeReachableChecks, &meleeUnreachableChecks](
        HostileSnapshot const* hostileSnapshot, Unit* hostileUnit, ObjectGuid memberGuid)
    {
        if (!hostileSnapshot || !hostileUnit)
            return false;
        for (ThreatEntrySnapshot const& entry : hostileSnapshot->threat)
        {
            if (entry.unitGuid != memberGuid.GetRawValue())
                continue;
            Unit* member = hostileUnit->GetMap()->GetUnit(memberGuid);
            bool const matches = member && entry.meleeReachable ==
                hostileUnit->CanReachWithMeleeAutoAttack(member);
            if (matches)
            {
                ++meleeReachabilityChecks;
                if (entry.meleeReachable)
                    ++meleeReachableChecks;
                else
                    ++meleeUnreachableChecks;
            }
            return matches;
        }
        return false;
    };
    bool const coreReachabilityCopied =
        snapshotMeleeReachabilityMatchesCore(bossSnapshot, hostiles[0].creature, tankGuid) &&
        snapshotMeleeReachabilityMatchesCore(bossSnapshot, hostiles[0].creature, healerGuid) &&
        snapshotMeleeReachabilityMatchesCore(addSnapshot, hostiles[1].creature, tankGuid) &&
        snapshotMeleeReachabilityMatchesCore(addSnapshot, hostiles[1].creature, healerGuid) &&
        meleeReachabilityChecks == 4 && meleeReachableChecks == 2 &&
        meleeUnreachableChecks == 2;

    bool const f9ChecksPassed = hostileAIReady && membersAdded && mainTankAssigned &&
        allPlayersOnSameMap && initialSnapshotValid && initialRosterResolved &&
        initialValuesCopied && initialFormCopied && initialRolesUnknown && initialCacheRetained &&
        registeredBeforeCleanup && bothHostilesInitiallyTargetTank &&
        threatBudgetSnapshotValid && overBudgetRejected && executionReservationRaceRejected &&
        safeBudgetSelectedAndReserved && enginePreservedCoreThreatState &&
        enginePreservedCoreVictims && reservationReleased &&
        tankAliveAndInjured && healCompleted && healAssistBoundValid && healerPulledBothAdds &&
        snapshotValid && rosterRoleStateValid && bothAddsResolved && hostileIdentityCopied &&
        currentCastSnapshotValid && threatValuesCopied && coreReachabilityCopied &&
        companionSnapshotValid && companionBudgetValid;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Real Group multi-add healer threat: aiReady=%u members=%u mainTank=%u sameMap=%u initialSnapshot=%u initialRoster=%u initialValues=%u rolesUnknown=%u cache=%u registered=%u tankDamage=%u/%u tankHP=%u-%u/%u tankThreat=%.1f/%.1f initialTankVictim=%u/%u petCreated=%u petSnapshot=%u petBudget=%u petThreat=%.1f petHeadroom=%.1f petMelee=%u budgetSnapshot=%u overBudgetRejected=%u executionRaceRejected=%u safeSelectedReserved=%u threatUnchanged=%u victimUnchanged=%u reservationReleased=%u hostileRefsOnTank=%u healResult=%u heal=%u>%u mana=%u>%u healAssistBound=%u upper=%.1f/%.1f delta=%.1f/%.1f assistInput=%.1f healerThreat=%.1f/%.1f selectedHealer=%u/%u finalSnapshot=%u finalRoster=%u adds=%u threatCopied=%u meleeReachability=%u/4 reachable=%u unreachable=%u.",
             hostileAIReady ? 1u : 0u, membersAdded ? 1u : 0u,
             mainTankAssigned ? 1u : 0u, allPlayersOnSameMap ? 1u : 0u,
             initialSnapshotValid ? 1u : 0u, initialRosterResolved ? 1u : 0u,
             initialValuesCopied ? 1u : 0u, initialRolesUnknown ? 1u : 0u,
             initialCacheRetained ? 1u : 0u, registeredBeforeCleanup ? 1u : 0u,
             tankDamage0, tankDamage1, patientHealthBeforeDamage,
             scriptedTankDamage, patientHealthBeforeHeal, tankThreat0, tankThreat1,
             initialVictim0 == tank ? 1u : 0u, initialVictim1 == tank ? 1u : 0u,
             primaryPetCreated ? 1u : 0u, companionSnapshotValid ? 1u : 0u,
             companionBudgetValid ? 1u : 0u, primaryPetThreat, primaryPetHeadroom,
             primaryPetCoreMeleeReachable ? 1u : 0u,
             threatBudgetSnapshotValid ? 1u : 0u, overBudgetRejected ? 1u : 0u,
             executionReservationRaceRejected ? 1u : 0u,
             safeBudgetSelectedAndReserved ? 1u : 0u,
             enginePreservedCoreThreatState ? 1u : 0u,
             enginePreservedCoreVictims ? 1u : 0u, reservationReleased ? 1u : 0u,
             hostileRefsOnTank, static_cast<uint32>(healCastResult),
             patientHealthBeforeHeal, patientHealthAfterHeal,
             healerManaBefore, healerManaAfter,
             healAssistBoundValid ? 1u : 0u,
             healAssistUpperBound0, healAssistUpperBound1,
             healThreatDelta0, healThreatDelta1, kScriptedAssistThreat,
             healerThreat0, healerThreat1,
             selectedAfterAssist0 == healer ? 1u : 0u,
             selectedAfterAssist1 == healer ? 1u : 0u,
             snapshotValid ? 1u : 0u, rosterRoleStateValid ? 1u : 0u,
             bothAddsResolved ? 1u : 0u, threatValuesCopied ? 1u : 0u,
             meleeReachabilityChecks, meleeReachableChecks, meleeUnreachableChecks);

    if (f9ChecksPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: one real five-Player Core Group passed initial roster/cache validation, two Core hostile adds switched from Main Tank to healer through assist-threat, final threat values and both reachable/unreachable per-reference melee states matched Core, and all Group/session cleanup succeeded.");

    // Remove the test Pet before later Tank/Healer death-retarget scenarios.
    // This checks Core reference cleanup without letting a living Pet become
    // a third surviving threat owner in the subsequent death test.
    petCleanupPassed = DestroySimulatorPrimaryPet(map, healer, primaryPet) &&
        petCleanupPassed;
    bool primaryPetThreatReferenceRemoved = true;
    for (HostileReference* reference :
         hostiles[0].creature->GetThreatManager().getThreatList())
        if (reference && reference->getUnitGuid().GetRawValue() == primaryPetGuid)
            primaryPetThreatReferenceRemoved = false;
    petCleanupPassed = petCleanupPassed && primaryPetThreatReferenceRemoved &&
        (!primaryPetGuid || map.GetPet(ObjectGuid(primaryPetGuid)) == nullptr);

    // F12: a hostile can still hold threat references to a party member
    // without currently attacking any group member. It must be discovered
    // through that member's HostileRefManager, including offline references.
    bool const f12CanProbe = f9ChecksPassed && petCleanupPassed &&
        outsideGroupVictim.creature && outsideGroupVictim.creature->IsInWorld();
    bool hostileVictimOutsideGroup = false;
    bool noPartyMemberCurrentlyAttacked = false;
    bool tankReferenceFound = false;
    bool offlineReferenceCopied = false;
    bool nonVictimHostileDiscovered = false;
    bool victimRestored = false;
    float healerThreat1AfterReferenceProbe = healerThreat1;

    if (f12CanProbe)
    {
        hostiles[1].creature->AI()->AttackStart(outsideGroupVictim.creature);
        hostileVictimOutsideGroup =
            hostiles[1].creature->GetVictim() == outsideGroupVictim.creature;

        noPartyMemberCurrentlyAttacked = true;
        for (Player* member : players)
            for (Unit* attacker : member->GetAttackers())
                if (attacker == hostiles[1].creature)
                    noPartyMemberCurrentlyAttacked = false;

        HostileReference* tankThreatReference = nullptr;
        for (HostileReference* reference = tank->GetHostileRefManager().getFirst();
             reference; reference = reference->next())
        {
            if (reference->getSource() &&
                reference->getSource()->getOwner() == hostiles[1].creature)
            {
                tankThreatReference = reference;
                tankReferenceFound = true;
                break;
            }
        }

        if (tankThreatReference)
            tankThreatReference->setOnlineOfflineState(false);

        PartySnapshot const* referenceSnapshot =
            PartySnapshotBuilder::GetOrBuild(tank, coordinator, 25000, 250);
        HostileSnapshot const* nonVictimHostileSnapshot = referenceSnapshot
            ? referenceSnapshot->FindHostile(hostiles[1].creature->GetObjectGuid().GetRawValue())
            : nullptr;
        bool offlineEntryMatches = false;
        if (nonVictimHostileSnapshot)
        {
            for (ThreatEntrySnapshot const& entry : nonVictimHostileSnapshot->threat)
            {
                if (entry.unitGuid != tankGuid.GetRawValue())
                    continue;
                offlineEntryMatches = !entry.online && !entry.accessible &&
                    std::fabs(entry.threat - tankThreat1) < 0.01f;
                break;
            }
        }

        bool const tankThreatInOfflineContainer =
            hostiles[1].creature->GetThreatManager().getThreat(tank, false) == 0.0f &&
            std::fabs(hostiles[1].creature->GetThreatManager().getThreat(tank, true) -
                      tankThreat1) < 0.01f;
        offlineReferenceCopied = tankThreatReference && !tankThreatReference->isOnline() &&
            offlineEntryMatches && tankThreatInOfflineContainer;

        nonVictimHostileDiscovered = hostileVictimOutsideGroup &&
            noPartyMemberCurrentlyAttacked && tankReferenceFound &&
            referenceSnapshot && referenceSnapshot->hostiles.size() == 2 &&
            nonVictimHostileSnapshot &&
            nonVictimHostileSnapshot->victimGuid ==
                outsideGroupVictim.creature->GetObjectGuid().GetRawValue() &&
            offlineReferenceCopied;

        // Restore Core reference state and the previous hostile target before
        // F10 death/recovery checks continue.
        if (tankThreatReference)
        {
            tankThreatReference->setOnlineOfflineState(true);
            tankThreatReference->setAccessibleState(true);
        }
        hostiles[1].creature->AI()->AttackStart(healer);
        healerThreat1AfterReferenceProbe =
            hostiles[1].creature->GetThreatManager().getThreat(healer);
        victimRestored = hostiles[1].creature->GetVictim() == healer;
    }

    bool const f12ChecksPassed = f12CanProbe && hostileVictimOutsideGroup &&
        noPartyMemberCurrentlyAttacked && tankReferenceFound &&
        offlineReferenceCopied && nonVictimHostileDiscovered && victimRestored;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] HostileRefManager discovery: outsideVictim=%u noPartyAttacker=%u tankRef=%u offlineSnapshot=%u nonVictimHostile=%u restored=%u cleanupPending=1.",
             hostileVictimOutsideGroup ? 1u : 0u,
             noPartyMemberCurrentlyAttacked ? 1u : 0u,
             tankReferenceFound ? 1u : 0u,
             offlineReferenceCopied ? 1u : 0u,
             nonVictimHostileDiscovered ? 1u : 0u,
             victimRestored ? 1u : 0u);
    if (f12ChecksPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: PartySnapshotBuilder discovered a non-current-victim hostile through a party member HostileRefManager and copied its offline threat reference state.");

    // F10: the living DPS establishes lower real damage-threat on both adds.
    // It must not steal aggro from the healer (212+) while it is alive.
    Player* dps = players[2];
    ObjectGuid const dpsGuid = dps->GetObjectGuid();
    uint32 const dpsDamage0 = f9ChecksPassed
        ? dps->DealDamage(hostiles[0].creature, 100, nullptr, DIRECT_DAMAGE,
                          SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    uint32 const dpsDamage1 = f9ChecksPassed
        ? dps->DealDamage(hostiles[1].creature, 100, nullptr, DIRECT_DAMAGE,
                          SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    float const dpsThreat0 = hostiles[0].creature->GetThreatManager().getThreat(dps);
    float const dpsThreat1 = hostiles[1].creature->GetThreatManager().getThreat(dps);
    Unit* const victimBeforeTankDeath0 =
        hostiles[0].creature->GetThreatManager().getHostileTarget();
    Unit* const victimBeforeTankDeath1 =
        hostiles[1].creature->GetThreatManager().getHostileTarget();
    bool const healerHoldsAggroOverDps = dpsDamage0 > 0 && dpsDamage1 > 0 &&
        dpsThreat0 > 0.0f && dpsThreat1 > 0.0f &&
        victimBeforeTankDeath0 == healer && victimBeforeTankDeath1 == healer;

    // Kill the Main Tank through normal Core damage. Unit::DealDamage and
    // Unit::SetDeathState must remove its HostileRefManager links from both
    // hostile ThreatManagers; native selection then retains the living healer.
    SimulatorAliveStateObserver tankAliveObserver(tank);
    PlayerAI* const previousTankAI = tank->AI();
    tank->SetAI(&tankAliveObserver);

    uint32 const tankHealthAtDeath = tank->GetHealth();
    uint32 const tankDeathDamage = f9ChecksPassed && healerHoldsAggroOverDps
        ? hostiles[0].creature->DealDamage(
            tank, tankHealthAtDeath, nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    bool const tankDied = tankDeathDamage > 0 && !tank->IsAlive() && tank->GetHealth() == 0;
    bool const tankDeathObserverValid = tankAliveObserver.transitionCount == 1 &&
        tankAliveObserver.lastWasAlive && !tankAliveObserver.lastIsAlive;

    // Core advances JUST_DIED to CORPSE without another alive/dead transition.
    // Verify the observer does not emit a duplicate death record for that step.
    if (tankDied)
        tank->SetDeathState(CORPSE);
    bool const tankCorpseTransitionSuppressed =
        tankAliveObserver.callbackCount == 2 &&
        tankAliveObserver.sameStateCallbacks == 1 &&
        tankAliveObserver.transitionCount == 1;
    tank->SetAI(previousTankAI);
    bool const tankThreatReferencesRemoved =
        hostiles[0].creature->GetThreatManager().getThreat(tank) == 0.0f &&
        hostiles[1].creature->GetThreatManager().getThreat(tank) == 0.0f;
    bool const tankDeathSelected0 = tankDied && hostiles[0].creature->SelectHostileTarget();
    bool const tankDeathSelected1 = tankDied && hostiles[1].creature->SelectHostileTarget();
    bool const healerRetargetedAfterTankDeath =
        tankDeathSelected0 && tankDeathSelected1 &&
        hostiles[0].creature->GetVictim() == healer &&
        hostiles[1].creature->GetVictim() == healer;

    PartySnapshot const* tankDeathSnapshot = nullptr;
    if (healerRetargetedAfterTankDeath)
        tankDeathSnapshot = PartySnapshotBuilder::GetOrBuild(healer, coordinator, 26000, 250);
    bool const tankDeathSnapshotValid = tankDeathSnapshot &&
        tankDeathSnapshot->groupId == groupId &&
        tankDeathSnapshot->members.size() == MAX_GROUP_SIZE &&
        tankDeathSnapshot->hostiles.size() == 2;
    PartyMemberSnapshot const* tankAfterDeath = tankDeathSnapshot
        ? tankDeathSnapshot->FindMember(tankGuid.GetRawValue()) : nullptr;
    PartyMemberSnapshot const* healerAfterTankDeath = tankDeathSnapshot
        ? tankDeathSnapshot->FindMember(healerGuid.GetRawValue()) : nullptr;
    bool const tankDeathRosterValid = tankAfterDeath && healerAfterTankDeath &&
        tankAfterDeath->isMainTank && !tankAfterDeath->alive && healerAfterTankDeath->alive;
    HostileSnapshot const* tankDeathBossSnapshot = tankDeathSnapshot
        ? tankDeathSnapshot->FindHostile(hostiles[0].creature->GetObjectGuid().GetRawValue()) : nullptr;
    HostileSnapshot const* tankDeathAddSnapshot = tankDeathSnapshot
        ? tankDeathSnapshot->FindHostile(hostiles[1].creature->GetObjectGuid().GetRawValue()) : nullptr;
    bool const tankDeathThreatSnapshotValid =
        tankDeathBossSnapshot && tankDeathAddSnapshot &&
        tankDeathBossSnapshot->victimGuid == healerGuid.GetRawValue() &&
        tankDeathAddSnapshot->victimGuid == healerGuid.GetRawValue() &&
        containsThreat(tankDeathBossSnapshot, healerGuid.GetRawValue(), healerThreat0) &&
        containsThreat(tankDeathAddSnapshot, healerGuid.GetRawValue(), healerThreat1AfterReferenceProbe) &&
        !containsThreat(tankDeathBossSnapshot, tankGuid.GetRawValue(), tankThreat0) &&
        !containsThreat(tankDeathAddSnapshot, tankGuid.GetRawValue(), tankThreat1);

    // Kill the active healer through Core damage. Its hostile references must
    // be removed; both adds then select the remaining DPS with real threat.
    uint32 const healerHealthAtDeath = healer->GetHealth();
    uint32 const healerDeathDamage = tankDeathThreatSnapshotValid
        ? hostiles[0].creature->DealDamage(
            healer, healerHealthAtDeath, nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    bool const healerDied = healerDeathDamage > 0 && !healer->IsAlive() &&
        healer->GetHealth() == 0;
    bool const healerThreatReferencesRemoved =
        hostiles[0].creature->GetThreatManager().getThreat(healer) == 0.0f &&
        hostiles[1].creature->GetThreatManager().getThreat(healer) == 0.0f;
    bool const healerDeathSelected0 = healerDied && hostiles[0].creature->SelectHostileTarget();
    bool const healerDeathSelected1 = healerDied && hostiles[1].creature->SelectHostileTarget();
    bool const dpsRetargetedAfterHealerDeath =
        healerDeathSelected0 && healerDeathSelected1 &&
        hostiles[0].creature->GetVictim() == dps &&
        hostiles[1].creature->GetVictim() == dps;

    PartySnapshot const* healerDeathSnapshot = nullptr;
    if (dpsRetargetedAfterHealerDeath)
        healerDeathSnapshot = PartySnapshotBuilder::GetOrBuild(dps, coordinator, 28000, 250);
    bool const healerDeathSnapshotValid = healerDeathSnapshot &&
        healerDeathSnapshot->groupId == groupId &&
        healerDeathSnapshot->members.size() == MAX_GROUP_SIZE &&
        healerDeathSnapshot->hostiles.size() == 2;
    PartyMemberSnapshot const* tankAfterBothDeaths = healerDeathSnapshot
        ? healerDeathSnapshot->FindMember(tankGuid.GetRawValue()) : nullptr;
    PartyMemberSnapshot const* healerAfterDeath = healerDeathSnapshot
        ? healerDeathSnapshot->FindMember(healerGuid.GetRawValue()) : nullptr;
    PartyMemberSnapshot const* dpsAfterDeaths = healerDeathSnapshot
        ? healerDeathSnapshot->FindMember(dpsGuid.GetRawValue()) : nullptr;
    bool const healerDeathRosterValid = tankAfterBothDeaths && healerAfterDeath &&
        dpsAfterDeaths && !tankAfterBothDeaths->alive && !healerAfterDeath->alive &&
        dpsAfterDeaths->alive;
    HostileSnapshot const* healerDeathBossSnapshot = healerDeathSnapshot
        ? healerDeathSnapshot->FindHostile(hostiles[0].creature->GetObjectGuid().GetRawValue()) : nullptr;
    HostileSnapshot const* healerDeathAddSnapshot = healerDeathSnapshot
        ? healerDeathSnapshot->FindHostile(hostiles[1].creature->GetObjectGuid().GetRawValue()) : nullptr;
    bool const healerDeathThreatSnapshotValid =
        healerDeathBossSnapshot && healerDeathAddSnapshot &&
        healerDeathBossSnapshot->victimGuid == dpsGuid.GetRawValue() &&
        healerDeathAddSnapshot->victimGuid == dpsGuid.GetRawValue() &&
        containsThreat(healerDeathBossSnapshot, dpsGuid.GetRawValue(), dpsThreat0) &&
        containsThreat(healerDeathAddSnapshot, dpsGuid.GetRawValue(), dpsThreat1) &&
        !containsThreat(healerDeathBossSnapshot, tankGuid.GetRawValue(), tankThreat0) &&
        !containsThreat(healerDeathAddSnapshot, tankGuid.GetRawValue(), tankThreat1) &&
        !containsThreat(healerDeathBossSnapshot, healerGuid.GetRawValue(), healerThreat0) &&
        !containsThreat(healerDeathAddSnapshot, healerGuid.GetRawValue(), healerThreat1);

    SimulatorAliveStateObserver tankResurrectionObserver(tank);
    PlayerAI* const previousTankAIForResurrection = tank->AI();
    tank->SetAI(&tankResurrectionObserver);
    if (tankDied && healerDied)
        tank->ResurrectPlayer(1.0f, false);
    bool const tankResurrectionObserved = tankDied && healerDied &&
        tank->IsAlive() && tankResurrectionObserver.callbackCount == 1 &&
        tankResurrectionObserver.transitionCount == 1 &&
        !tankResurrectionObserver.lastWasAlive && tankResurrectionObserver.lastIsAlive;
    tank->SetAI(previousTankAIForResurrection);

    // Exercise the actual PartyBotAI callback and formatted encounter trace,
    // not only the Core PlayerAI observer boundary. Use a fresh active
    // coordinator so both emitted events have a deterministic shared identity.
    std::shared_ptr<PartyCoordinator> partyTraceCoordinator =
        PartyCoordinatorRegistry::GetOrCreate(group->GetId(), map.GetId(), map.GetInstanceId());
    PartySnapshot partyTraceSnapshot;
    partyTraceSnapshot.groupId = group->GetId();
    partyTraceSnapshot.mapId = map.GetId();
    partyTraceSnapshot.instanceId = map.GetInstanceId();
    partyTraceSnapshot.epoch = 1;
    PartyMemberSnapshot partyTraceTank;
    partyTraceTank.guid = tankGuid.GetRawValue();
    partyTraceTank.mapId = map.GetId();
    partyTraceTank.instanceId = map.GetInstanceId();
    partyTraceTank.online = true;
    partyTraceTank.alive = true;
    partyTraceTank.inCombat = true;
    partyTraceSnapshot.members.push_back(partyTraceTank);
    uint32 const partyTraceNow = WorldTimer::getMSTime();
    EncounterTransition const partyTraceStarted =
        partyTraceCoordinator->ObserveEncounter(partyTraceSnapshot, partyTraceNow, 30000);

    PartyBotAI partyTraceAI(
        dps, map.GetId(), map.GetInstanceId(),
        tank->GetPositionX(), tank->GetPositionY(),
        tank->GetPositionZ(), tank->GetOrientation());
    partyTraceAI.SetPlayer(tank);
    partyTraceAI.m_partyCoordinator = partyTraceCoordinator;
    partyTraceAI.m_partyCoordinatorMapId = map.GetId();
    partyTraceAI.m_partyCoordinatorInstanceId = map.GetInstanceId();
    partyTraceAI.m_decisionTrace.BeginTick();
    PlayerAI* const previousTankAIForPartyTrace = tank->AI();
    tank->SetAI(&partyTraceAI);

    uint32 const partyTraceSequenceBefore = partyTraceCoordinator->NextEncounterSequence();
    tank->SetHealth(tank->GetMaxHealth());
    uint32 const partyTraceDeathDamage = partyTraceStarted.kind == EncounterTransitionKind::Started
        ? hostiles[0].creature->DealDamage(
            tank, tank->GetHealth(), nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    bool const partyTraceDied = partyTraceDeathDamage > 0 &&
        !tank->IsAlive() && tank->GetHealth() == 0;
    if (partyTraceDied)
    {
        // Core's JUST_DIED -> CORPSE callback must not consume another sequence.
        tank->SetDeathState(CORPSE);
        tank->ResurrectPlayer(1.0f, false);
    }
    uint32 const partyTraceSequenceAfter = partyTraceCoordinator->NextEncounterSequence();
    bool const partyBotDamageTracePassed =
        partyTraceStarted.kind == EncounterTransitionKind::Started &&
        partyTraceDeathDamage > 0 &&
        partyTraceSequenceAfter == partyTraceSequenceBefore + 4;
    bool const partyBotEncounterLifecycleTraced =
        partyTraceStarted.kind == EncounterTransitionKind::Started &&
        partyTraceDied && tank->IsAlive() && partyBotDamageTracePassed;

    EncounterHeartbeat f21Heartbeat;
    bool const f21HeartbeatReady = partyTraceCoordinator->TryBeginEncounterHeartbeat(
        partyTraceNow, 2000, f21Heartbeat);
    if (f21HeartbeatReady)
        partyTraceAI.m_decisionTrace.RecordPartyHeartbeat(
            tank, f21CastStateTraceSnapshot, f21Heartbeat);
    bool const f21BotStateTracePassed = f21HeartbeatReady &&
        partyTraceAI.m_decisionTrace.IsEnabled() && initialFormCopied &&
        currentCastSnapshotValid && hostileIdentityCopied &&
        f21CastStateTraceSnapshot.members.size() == MAX_GROUP_SIZE;

    // Exercise the actual PartyBotAI target-change callback across two distinct
    // bots sharing the same map/instance PartyCoordinator. The first 1,024
    // structured detail events are admitted; the next one must emit the one
    // shared trace_limit marker, and every later event must be suppressed.
    PartyBotAI secondPartyTraceAI(
        tank, map.GetId(), map.GetInstanceId(),
        dps->GetPositionX(), dps->GetPositionY(),
        dps->GetPositionZ(), dps->GetOrientation());
    secondPartyTraceAI.SetPlayer(dps);
    secondPartyTraceAI.m_partyCoordinator = partyTraceCoordinator;
    secondPartyTraceAI.m_partyCoordinatorMapId = map.GetId();
    secondPartyTraceAI.m_partyCoordinatorInstanceId = map.GetInstanceId();
    secondPartyTraceAI.m_decisionTrace.BeginTick();
    PlayerAI* const previousDpsAIForPartyTrace = dps->AI();
    dps->SetAI(&secondPartyTraceAI);

    // Use a fresh coordinator for the exact F13 1,024-detail budget test;
    // F22's damage event remains part of the separate F10/F21 encounter.
    std::shared_ptr<PartyCoordinator> traceBudgetCoordinator(new PartyCoordinator());
    traceBudgetCoordinator->ConfigureEncounterTraceBudget(
        PartyCoordinator::MaxDetailTraceEvents, PartyCoordinator::MaxImportantTraceEvents);
    PartySnapshot traceBudgetSnapshot = partyTraceSnapshot;
    traceBudgetSnapshot.epoch = 3;
    uint32 const traceBudgetNow = WorldTimer::getMSTime();
    EncounterTransition const traceBudgetStarted =
        traceBudgetCoordinator->ObserveEncounter(traceBudgetSnapshot, traceBudgetNow, 30000);
    partyTraceAI.m_partyCoordinator = traceBudgetCoordinator;
    secondPartyTraceAI.m_partyCoordinator = traceBudgetCoordinator;
    if (traceBudgetStarted.kind == EncounterTransitionKind::Started)
        partyTraceAI.m_decisionTrace.RecordEncounterBoundary(
            tank, "encounter_start", traceBudgetStarted.groupId,
            traceBudgetStarted.mapId, traceBudgetStarted.instanceId,
            traceBudgetStarted.encounterId, traceBudgetStarted.sequence,
            traceBudgetStarted.elapsedMs, traceBudgetStarted.reason);

    auto switchPartyBotTargets = [&](PartyBotAI& ai, Player* actor, uint32 count)
    {
        Unit* const hostileA = hostiles[0].creature;
        Unit* const hostileB = hostiles[1].creature;
        Unit* const firstTarget = actor->GetVictim() == hostileA ? hostileB : hostileA;
        Unit* const secondTarget = firstTarget == hostileA ? hostileB : hostileA;
        uint32 switches = 0;
        for (uint32 i = 0; i < count; ++i)
        {
            uint64 const oldVictim = actor->GetVictim()
                ? actor->GetVictim()->GetObjectGuid().GetRawValue() : 0;
            Unit* const nextTarget = (i & 1u) ? secondTarget : firstTarget;
            if (!ai.AttackStart(nextTarget))
                break;
            uint64 const newVictim = actor->GetVictim()
                ? actor->GetVictim()->GetObjectGuid().GetRawValue() : 0;
            if (oldVictim == newVictim)
                break;
            ++switches;
        }
        return switches;
    };

    uint32 const firstBotSwitches = switchPartyBotTargets(
        partyTraceAI, tank, PartyCoordinator::MaxDetailTraceEvents / 2);
    uint32 const secondBotSwitches = switchPartyBotTargets(
        secondPartyTraceAI, dps,
        PartyCoordinator::MaxDetailTraceEvents / 2 + 1);
    uint32 const sharedTraceSwitches = firstBotSwitches + secondBotSwitches;
    bool const traceBudgetSuppressedAfterOverflow =
        traceBudgetStarted.kind == EncounterTransitionKind::Started &&
        sharedTraceSwitches == PartyCoordinator::MaxDetailTraceEvents + 1 &&
        traceBudgetCoordinator->TryAdmitEncounterTraceEvent(
            traceBudgetStarted.encounterId, EncounterTraceEventClass::Detail) ==
            EncounterTraceAdmission::Suppressed;
    bool const partyBotTraceBudgetPassed = partyBotEncounterLifecycleTraced &&
        traceBudgetSuppressedAfterOverflow;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PartyBotAI shared trace budget: firstBot=%u secondBot=%u total=%u expected=%u suppressedAfterOverflow=%u.",
             firstBotSwitches, secondBotSwitches, sharedTraceSwitches,
             PartyCoordinator::MaxDetailTraceEvents + 1,
             traceBudgetSuppressedAfterOverflow ? 1u : 0u);
    if (partyBotTraceBudgetPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: two real PartyBotAI instances shared one coordinator detail budget, admitted 1,024 target-change events, emitted one trace_limit on overflow and suppressed subsequent detail events.");

    EncounterTransition const traceBudgetEnded =
        traceBudgetCoordinator->CloseEncounter(WorldTimer::getMSTime(), "trace_budget_fixture_complete");
    if (traceBudgetEnded.kind == EncounterTransitionKind::Ended &&
        traceBudgetCoordinator->TryAdmitEncounterTraceEvent(
            traceBudgetEnded.encounterId, EncounterTraceEventClass::Terminal) ==
            EncounterTraceAdmission::Accepted)
        partyTraceAI.m_decisionTrace.RecordEncounterBoundary(
            tank, "encounter_end", traceBudgetEnded.groupId, traceBudgetEnded.mapId,
            traceBudgetEnded.instanceId, traceBudgetEnded.encounterId,
            traceBudgetEnded.sequence, traceBudgetEnded.elapsedMs, traceBudgetEnded.reason);
    partyTraceAI.m_partyCoordinator = partyTraceCoordinator;
    secondPartyTraceAI.m_partyCoordinator = partyTraceCoordinator;

    // F14: observe an ordinary Player (no CombatBotBaseAI) through fresh
    // map-thread snapshots. Damage and resurrection are real Core operations.
    Player* const unmanagedMember = players.size() > 4 ? players[4] : nullptr;
    PartyCoordinator unmanagedCoordinator;
    unmanagedCoordinator.ConfigureEncounterTraceBudget(32, 16);
    EncounterTransition const unmanagedStarted =
        unmanagedMember ? unmanagedCoordinator.ObserveEncounter(
            partyTraceSnapshot, WorldTimer::getMSTime(), 30000) : EncounterTransition();
    auto captureUnmanagedSnapshot = [&](PartySnapshot& snapshot) -> bool
    {
        unmanagedCoordinator.InvalidateSnapshot();
        PartySnapshot const* fresh = PartySnapshotBuilder::GetOrBuild(
            tank, unmanagedCoordinator, WorldTimer::getMSTime(), 0);
        if (!fresh)
            return false;
        snapshot = *fresh;
        return true;
    };
    auto recordUnmanagedChanges = [&](std::vector<PartyMemberStateChange> const& changes)
    {
        uint32 recorded = 0;
        for (PartyMemberStateChange const& change : changes)
            if (partyTraceAI.m_decisionTrace.RecordEncounterPartyMemberState(
                    tank, unmanagedCoordinator, group->GetId(), map.GetId(),
                    map.GetInstanceId(), unmanagedStarted.encounterId,
                    unmanagedCoordinator.GetEncounterElapsedMs(WorldTimer::getMSTime()),
                    change))
                ++recorded;
        return recorded;
    };

    PartySnapshot unmanagedBaselineSnapshot;
    bool const unmanagedBaselineBuilt = unmanagedMember &&
        unmanagedStarted.kind == EncounterTransitionKind::Started &&
        captureUnmanagedSnapshot(unmanagedBaselineSnapshot);
    PartyMemberSnapshot const* unmanagedBaselineMember = unmanagedBaselineBuilt
        ? unmanagedBaselineSnapshot.FindMember(
            unmanagedMember->GetObjectGuid().GetRawValue()) : nullptr;
    std::vector<PartyMemberStateChange> const unmanagedBaselineChanges =
        unmanagedBaselineBuilt
        ? unmanagedCoordinator.ObserveNonBotPartyMemberStateChanges(
            unmanagedBaselineSnapshot) : std::vector<PartyMemberStateChange>();
    bool const unmanagedBaselineValid = unmanagedBaselineMember &&
        !unmanagedBaselineMember->isPlayerBot && unmanagedBaselineMember->online &&
        unmanagedBaselineMember->alive && unmanagedBaselineChanges.empty();

    uint32 const unmanagedHealthBefore = unmanagedMember
        ? unmanagedMember->GetHealth() : 0;
    uint32 const unmanagedHealthDamage = unmanagedBaselineValid
        ? hostiles[0].creature->DealDamage(
            unmanagedMember, 100, nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    PartySnapshot unmanagedHealthSnapshot;
    bool const unmanagedHealthSnapshotBuilt = unmanagedHealthDamage > 0 &&
        unmanagedMember->GetHealth() < unmanagedHealthBefore &&
        captureUnmanagedSnapshot(unmanagedHealthSnapshot);
    std::vector<PartyMemberStateChange> const unmanagedHealthChanges =
        unmanagedHealthSnapshotBuilt
        ? unmanagedCoordinator.ObserveNonBotPartyMemberStateChanges(
            unmanagedHealthSnapshot) : std::vector<PartyMemberStateChange>();
    uint32 const unmanagedHealthRecords =
        unmanagedHealthChanges.size() == 1 &&
        unmanagedHealthChanges[0].memberGuid ==
            unmanagedMember->GetObjectGuid().GetRawValue() &&
        unmanagedHealthChanges[0].wasAlive && unmanagedHealthChanges[0].isAlive
        ? recordUnmanagedChanges(unmanagedHealthChanges) : 0;

    uint32 const unmanagedHealthAtDeath = unmanagedMember
        ? unmanagedMember->GetHealth() : 0;
    uint32 const unmanagedDeathDamage = unmanagedHealthRecords == 1
        ? hostiles[0].creature->DealDamage(
            unmanagedMember, unmanagedHealthAtDeath + 10000u, nullptr,
            DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    PartySnapshot unmanagedDeathSnapshot;
    bool const unmanagedDeathSnapshotBuilt = unmanagedDeathDamage > 0 &&
        !unmanagedMember->IsAlive() && unmanagedMember->GetHealth() == 0 &&
        captureUnmanagedSnapshot(unmanagedDeathSnapshot);
    std::vector<PartyMemberStateChange> const unmanagedDeathChanges =
        unmanagedDeathSnapshotBuilt
        ? unmanagedCoordinator.ObserveNonBotPartyMemberStateChanges(
            unmanagedDeathSnapshot) : std::vector<PartyMemberStateChange>();
    uint32 const unmanagedDeathRecords =
        unmanagedDeathChanges.size() == 1 &&
        unmanagedDeathChanges[0].wasAlive && !unmanagedDeathChanges[0].isAlive
        ? recordUnmanagedChanges(unmanagedDeathChanges) : 0;

    if (unmanagedDeathRecords == 1)
        unmanagedMember->ResurrectPlayer(1.0f, false);
    PartySnapshot unmanagedResurrectionSnapshot;
    bool const unmanagedResurrectionSnapshotBuilt = unmanagedDeathRecords == 1 &&
        unmanagedMember->IsAlive() && unmanagedMember->GetHealth() > 0 &&
        captureUnmanagedSnapshot(unmanagedResurrectionSnapshot);
    std::vector<PartyMemberStateChange> const unmanagedResurrectionChanges =
        unmanagedResurrectionSnapshotBuilt
        ? unmanagedCoordinator.ObserveNonBotPartyMemberStateChanges(
            unmanagedResurrectionSnapshot) : std::vector<PartyMemberStateChange>();
    uint32 const unmanagedResurrectionRecords =
        unmanagedResurrectionChanges.size() == 1 &&
        !unmanagedResurrectionChanges[0].wasAlive &&
        unmanagedResurrectionChanges[0].isAlive
        ? recordUnmanagedChanges(unmanagedResurrectionChanges) : 0;

    bool const unmanagedMemberStateTracePassed =
        unmanagedBaselineValid && unmanagedHealthDamage > 0 &&
        unmanagedHealthRecords == 1 && unmanagedDeathDamage > 0 &&
        unmanagedDeathRecords == 1 && unmanagedResurrectionRecords == 1;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Unmanaged member state trace: baseline=%u healthDamage=%u healthRecord=%u deathDamage=%u deathRecord=%u resurrectionRecord=%u member_guid=%llu.",
             unmanagedBaselineValid ? 1u : 0u, unmanagedHealthDamage,
             unmanagedHealthRecords, unmanagedDeathDamage, unmanagedDeathRecords,
             unmanagedResurrectionRecords,
             static_cast<unsigned long long>(unmanagedMember
                 ? unmanagedMember->GetObjectGuid().GetRawValue() : 0));
    if (unmanagedMemberStateTracePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: real Core DealDamage and Player::ResurrectPlayer produced one sampled health, death and resurrection trace for a non-PlayerBots Group member; member_guid=%llu.",
                 static_cast<unsigned long long>(
                     unmanagedMember->GetObjectGuid().GetRawValue()));
    if (unmanagedCoordinator.HasActiveEncounter())
    {
        EncounterTransition const unmanagedEnded =
            unmanagedCoordinator.CloseEncounter(WorldTimer::getMSTime(),
                                                  "simulator_member_state_complete");
        if (unmanagedEnded.kind == EncounterTransitionKind::Ended &&
            unmanagedCoordinator.TryAdmitEncounterTraceEvent(
                unmanagedEnded.encounterId, EncounterTraceEventClass::Terminal) ==
                EncounterTraceAdmission::Accepted)
            partyTraceAI.m_decisionTrace.RecordEncounterBoundary(
                tank, "encounter_end", unmanagedEnded.groupId, unmanagedEnded.mapId,
                unmanagedEnded.instanceId, unmanagedEnded.encounterId,
                unmanagedEnded.sequence, unmanagedEnded.elapsedMs,
                unmanagedEnded.reason);
    }

    // F15: the explicit server-shutdown hook closes the shared encounter once.
    // The second bot observes the already-closed coordinator and cannot emit a
    // duplicate terminal record. The reserved terminal admission must be consumed
    // even though the ordinary detail budget was exhausted by F13.
    uint64 const shutdownEncounterId = partyTraceCoordinator->GetEncounterId();
    partyTraceAI.OnServerShutdown();
    bool const shutdownFlushClosedEncounter = shutdownEncounterId != 0 &&
        !partyTraceCoordinator->HasActiveEncounter() &&
        partyTraceCoordinator->TryAdmitEncounterTraceEvent(
            shutdownEncounterId, EncounterTraceEventClass::Terminal) ==
            EncounterTraceAdmission::Suppressed;
    secondPartyTraceAI.OnServerShutdown();
    bool const shutdownFlushNoDuplicate = shutdownFlushClosedEncounter &&
        partyTraceCoordinator->TryAdmitEncounterTraceEvent(
            shutdownEncounterId, EncounterTraceEventClass::Terminal) ==
            EncounterTraceAdmission::Suppressed;
    if (shutdownFlushNoDuplicate)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: server-shutdown hook closed an active shared encounter and consumed exactly one reserved terminal admission across two PartyBotAI instances.");

    // F16: exercise the actual source-Map Remove path. The first bot leaving
    // must not close while its party bot partner remains; the final bot leaving
    // must close on the old map thread and consume one terminal admission.
    uint32 const mapLeaveNow = WorldTimer::getMSTime();
    PartySnapshot mapLeaveSnapshot = partyTraceSnapshot;
    mapLeaveSnapshot.epoch = 2;
    EncounterTransition const mapLeaveStarted =
        partyTraceCoordinator->ObserveEncounter(mapLeaveSnapshot, mapLeaveNow, 30000);
    if (mapLeaveStarted.kind == EncounterTransitionKind::Started)
    {
        map.Remove(tank, false);
        bool const firstLeaveKeptEncounter = partyTraceCoordinator->HasActiveEncounter();
        map.Remove(dps, false);
        bool const lastLeaveClosedEncounter = !partyTraceCoordinator->HasActiveEncounter() &&
            partyTraceCoordinator->TryAdmitEncounterTraceEvent(
                mapLeaveStarted.encounterId, EncounterTraceEventClass::Terminal) ==
                EncounterTraceAdmission::Suppressed;
        bool const restoredTank = map.Add(tank);
        bool const restoredDps = map.Add(dps);
        if (firstLeaveKeptEncounter && lastLeaveClosedEncounter && restoredTank && restoredDps)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[PartyBotSimulator] PASS: source Map::Remove retained an encounter while another PartyBotAI remained, then the last bot closed the old map shard exactly once before both Players were re-added.");
    }

    // F27: move the full real Group through Map::Remove/Map::Add into a
    // different Vanilla dungeon Map. A Group can bind only one instance per
    // dungeon map ID, so a second instance of the same Map is invalid Core
    // behavior for this fixture. The source shard must close on its Map thread;
    // the ordinary PartyBotAI Shadow update must then acquire a different
    // registry entry and start an encounter from the destination's real roster.
    // The temporary destination GroupBind is explicitly removed on return.
    static uint32 constexpr F27_DESTINATION_MAP_ID = 34; // Stockades, a separate Vanilla dungeon.
    MapEntry const* destinationEntry =
        sMapStorage.LookupEntry<MapEntry>(F27_DESTINATION_MAP_ID);
    bool const destinationMapAvailable = destinationEntry && destinationEntry->IsDungeon() &&
        group->GetBoundInstance(F27_DESTINATION_MAP_ID) == nullptr;
    Map* destinationMap = destinationMapAvailable
        ? sMapMgr.CreateTestMap(F27_DESTINATION_MAP_ID, true, 0.0f, 0.0f)
        : nullptr;
    bool instanceTransferTracePassed = false;
    if (destinationMap && destinationMap->GetId() != map.GetId() &&
        destinationMap->GetId() == F27_DESTINATION_MAP_ID &&
        destinationMap->GetInstanceId() &&
        destinationMap->GetInstanceId() != map.GetInstanceId())
    {
        partyTraceAI.m_decisionTrace.BeginTick();
        secondPartyTraceAI.m_decisionTrace.BeginTick();
        tank->SetInCombatState();
        partyTraceAI.UpdatePartyBotEngineShadow();
        secondPartyTraceAI.UpdatePartyBotEngineShadow();

        std::shared_ptr<PartyCoordinator> sourceCoordinator = partyTraceAI.m_partyCoordinator;
        std::uint64_t const sourceEncounterId = sourceCoordinator
            ? sourceCoordinator->GetEncounterId() : 0;
        bool const sourceShardReady = sourceCoordinator &&
            sourceCoordinator == partyTraceCoordinator &&
            secondPartyTraceAI.m_partyCoordinator == sourceCoordinator &&
            sourceCoordinator->HasActiveEncounter() && sourceEncounterId &&
            sourceCoordinator->GetEncounterMapId() == map.GetId() &&
            sourceCoordinator->GetEncounterInstanceId() == map.GetInstanceId();

        bool allPlayersStartedOnSource = sourceShardReady;
        for (Player* member : players)
            allPlayersStartedOnSource = allPlayersStartedOnSource && member &&
                member->IsInWorld() && member->GetMap() == &map;

        if (allPlayersStartedOnSource)
        {
            for (Player* member : players)
                map.Remove(member, false);

            bool const allPlayersDetachedFromSource = std::all_of(
                players.begin(), players.end(),
                [](Player const* member) { return member && !member->IsInWorld(); });
            bool const sourceShardClosed = !sourceCoordinator->HasActiveEncounter() &&
                sourceCoordinator->TryAdmitEncounterTraceEvent(
                    sourceEncounterId, EncounterTraceEventClass::Terminal) ==
                    EncounterTraceAdmission::Suppressed;
            bool const sourceAisReleasedShard =
                !partyTraceAI.m_partyCoordinator &&
                !secondPartyTraceAI.m_partyCoordinator &&
                partyTraceAI.m_partyCoordinatorMapId == 0 &&
                secondPartyTraceAI.m_partyCoordinatorMapId == 0;

            bool allPlayersAddedToDestination = allPlayersDetachedFromSource;
            if (allPlayersDetachedFromSource)
                for (Player* member : players)
                    if (!destinationMap->Add(member))
                        allPlayersAddedToDestination = false;

            bool const allPlayersOnDestination = allPlayersAddedToDestination &&
                std::all_of(players.begin(), players.end(),
                    [destinationMap](Player const* member)
                    {
                        return member && member->IsInWorld() &&
                            member->GetMap() == destinationMap &&
                            member->GetMapId() == destinationMap->GetId() &&
                            member->GetInstanceId() == destinationMap->GetInstanceId();
                    });

            std::shared_ptr<PartyCoordinator> destinationCoordinator;
            bool destinationRosterValid = false;
            bool destinationEncounterStarted = false;
            bool destinationShardDistinct = false;
            std::uint64_t destinationEncounterId = 0;
            if (allPlayersOnDestination)
            {
                // This is the same entry point used by the normal PartyBotAI
                // map update. No test-only coordinator injection is performed.
                tank->SetInCombatState();
                partyTraceAI.m_decisionTrace.BeginTick();
                secondPartyTraceAI.m_decisionTrace.BeginTick();
                partyTraceAI.UpdatePartyBotEngineShadow();
                secondPartyTraceAI.UpdatePartyBotEngineShadow();

                destinationCoordinator = partyTraceAI.m_partyCoordinator;
                destinationShardDistinct = destinationCoordinator &&
                    destinationCoordinator != sourceCoordinator &&
                    destinationCoordinator == secondPartyTraceAI.m_partyCoordinator &&
                    destinationCoordinator == PartyCoordinatorRegistry::GetOrCreate(
                        group->GetId(), destinationMap->GetId(),
                        destinationMap->GetInstanceId()) &&
                    destinationMap->GetId() != sourceCoordinator->GetEncounterMapId() &&
                    destinationCoordinator->GetEncounterMapId() == destinationMap->GetId() &&
                    destinationCoordinator->GetEncounterInstanceId() ==
                        destinationMap->GetInstanceId() &&
                    group->GetBoundInstance(destinationMap->GetId()) != nullptr;

                PartySnapshot const* destinationSnapshot = destinationCoordinator
                    ? PartySnapshotBuilder::GetOrBuild(
                        tank, *destinationCoordinator, WorldTimer::getMSTime(), 250)
                    : nullptr;
                destinationRosterValid = destinationSnapshot &&
                    destinationSnapshot->groupId == group->GetId() &&
                    destinationSnapshot->mapId == destinationMap->GetId() &&
                    destinationSnapshot->instanceId == destinationMap->GetInstanceId() &&
                    destinationSnapshot->members.size() == MAX_GROUP_SIZE;
                if (destinationRosterValid)
                {
                    for (Player* member : players)
                    {
                        PartyMemberSnapshot const* row = destinationSnapshot->FindMember(
                            member->GetObjectGuid().GetRawValue());
                        destinationRosterValid = destinationRosterValid && row &&
                            row->online && row->mapId == destinationMap->GetId() &&
                            row->instanceId == destinationMap->GetInstanceId();
                    }
                }

                destinationEncounterId = destinationCoordinator
                    ? destinationCoordinator->GetEncounterId() : 0;
                destinationEncounterStarted = destinationCoordinator &&
                    destinationCoordinator->HasActiveEncounter() &&
                    destinationEncounterId &&
                    destinationEncounterId != sourceEncounterId &&
                    destinationCoordinator->GetEncounterMapId() == destinationMap->GetId() &&
                    destinationCoordinator->GetEncounterInstanceId() ==
                        destinationMap->GetInstanceId();
            }

            // Always move the fixture back, even when one of the assertions
            // above fails, so the enclosing Group/session cleanup stays valid.
            bool allPlayersRemovedFromDestination = allPlayersOnDestination;
            if (allPlayersOnDestination)
                for (Player* member : players)
                    destinationMap->Remove(member, false);
            else
                for (Player* member : players)
                    if (member && member->IsInWorld() && member->GetMap() == destinationMap)
                        destinationMap->Remove(member, false);
            allPlayersRemovedFromDestination = allPlayersRemovedFromDestination &&
                std::all_of(players.begin(), players.end(),
                    [](Player const* member) { return member && !member->IsInWorld(); });

            bool allPlayersRestoredToSource = true;
            for (Player* member : players)
            {
                if (!member)
                {
                    allPlayersRestoredToSource = false;
                    continue;
                }
                if (!member->IsInWorld())
                {
                    if (!map.Add(member))
                        allPlayersRestoredToSource = false;
                }
                else if (member->GetMap() != &map)
                    allPlayersRestoredToSource = false;
            }

            bool const destinationShardClosed = destinationCoordinator &&
                destinationEncounterStarted && !destinationCoordinator->HasActiveEncounter() &&
                destinationCoordinator->TryAdmitEncounterTraceEvent(
                    destinationEncounterId, EncounterTraceEventClass::Terminal) ==
                    EncounterTraceAdmission::Suppressed;

            // DungeonMap::Add binds the Group to the destination map ID. This
            // fixture is intentionally not a persistent dungeon run, so remove
            // only the new test binding after all Players have left that Map.
            if (allPlayersRemovedFromDestination &&
                group->GetBoundInstance(destinationMap->GetId()))
                group->UnbindInstance(destinationMap->GetId());
            bool const destinationGroupBindReleased =
                group->GetBoundInstance(destinationMap->GetId()) == nullptr;
            bool const destinationAisReleasedShard =
                !partyTraceAI.m_partyCoordinator &&
                !secondPartyTraceAI.m_partyCoordinator;
            bool const allPlayersBackOnSource = allPlayersRestoredToSource &&
                std::all_of(players.begin(), players.end(),
                    [&map](Player const* member)
                    {
                        return member && member->IsInWorld() && member->GetMap() == &map &&
                            member->GetMapId() == map.GetId() &&
                            member->GetInstanceId() == map.GetInstanceId();
                    });

            instanceTransferTracePassed = sourceShardReady &&
                allPlayersDetachedFromSource && sourceShardClosed && sourceAisReleasedShard &&
                allPlayersOnDestination && destinationShardDistinct &&
                destinationRosterValid && destinationEncounterStarted &&
                allPlayersRemovedFromDestination && destinationShardClosed &&
                destinationGroupBindReleased && destinationAisReleasedShard &&
                allPlayersBackOnSource;
        }
    }

    // No Player may remain attached to the temporary destination map.
    if (destinationMap)
    {
        for (Player* member : players)
            if (member && member->IsInWorld() && member->GetMap() == destinationMap)
                destinationMap->Remove(member, false);
        for (Player* member : players)
            if (member && !member->IsInWorld())
                map.Add(member);

        bool const destinationMapEmpty = std::all_of(
            players.begin(), players.end(),
            [destinationMap](Player const* member)
            {
                return !member || !member->IsInWorld() || member->GetMap() != destinationMap;
            });
        bool const sourceFixtureRestored = std::all_of(
            players.begin(), players.end(),
            [&map](Player const* member)
            {
                return member && member->IsInWorld() && member->GetMap() == &map;
            });
        if (destinationMapEmpty &&
            group->GetBoundInstance(destinationMap->GetId()))
            group->UnbindInstance(destinationMap->GetId());
        bool const destinationBindCleaned =
            group->GetBoundInstance(destinationMap->GetId()) == nullptr;
        if (destinationMapEmpty && destinationBindCleaned)
            sMapMgr.DeleteTestMap(destinationMap);
        else
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[PartyBotSimulator] Refusing to delete destination map %u instance %u (playersDetached=%u groupBindReleased=%u).",
                     destinationMap->GetId(), destinationMap->GetInstanceId(),
                     destinationMapEmpty ? 1u : 0u, destinationBindCleaned ? 1u : 0u);
        instanceTransferTracePassed = instanceTransferTracePassed &&
            destinationMapEmpty && destinationBindCleaned && sourceFixtureRestored;
    }

    if (instanceTransferTracePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: a real five-Player Group left the source Map, closed its last-PartyBot encounter exactly once, reacquired a distinct destination-map coordinator through PartyBotAI::UpdatePartyBotEngineShadow, started a new encounter from the destination roster, then closed it, removed its temporary GroupBind and restored every Player to the source instance.");

    // F17: exercise the Engine's sampled trigger observations and their
    // encounter-correlated log format with the real strategy registry.
    // A healer with one injured party member yields one active healing trigger,
    // two inactive role triggers and one candidate attributed to healing_triage.
    std::shared_ptr<PartyCoordinator> triggerTraceCoordinator(new PartyCoordinator());
    triggerTraceCoordinator->ConfigureEncounterTraceBudget(32, 16);
    PartySnapshot triggerTraceSnapshot;
    triggerTraceSnapshot.groupId = group->GetId();
    triggerTraceSnapshot.mapId = map.GetId();
    triggerTraceSnapshot.instanceId = map.GetInstanceId();
    triggerTraceSnapshot.epoch = 17;
    PartyMemberSnapshot triggerTraceBot;
    triggerTraceBot.guid = tankGuid.GetRawValue();
    triggerTraceBot.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    triggerTraceBot.mapId = map.GetId();
    triggerTraceBot.instanceId = map.GetInstanceId();
    triggerTraceBot.health = tank->GetMaxHealth();
    triggerTraceBot.maxHealth = tank->GetMaxHealth();
    triggerTraceBot.online = true;
    triggerTraceBot.alive = true;
    triggerTraceBot.inCombat = true;
    triggerTraceSnapshot.members.push_back(triggerTraceBot);
    PartyMemberSnapshot triggerTracePatient;
    triggerTracePatient.guid = dps->GetObjectGuid().GetRawValue();
    triggerTracePatient.roleId = static_cast<std::uint8_t>(PartyRole::MeleeDamage);
    triggerTracePatient.mapId = map.GetId();
    triggerTracePatient.instanceId = map.GetInstanceId();
    triggerTracePatient.health = 250;
    triggerTracePatient.maxHealth = 1000;
    triggerTracePatient.online = true;
    triggerTracePatient.alive = true;
    triggerTracePatient.inCombat = true;
    triggerTraceSnapshot.members.push_back(triggerTracePatient);

    uint32 const triggerTraceNow = WorldTimer::getMSTime();
    EncounterTransition const triggerTraceStarted =
        triggerTraceCoordinator->ObserveEncounter(triggerTraceSnapshot, triggerTraceNow, 30000);
    if (!partyTraceAI.m_engineShadowStrategiesRegistered)
    {
        RegisterDefaultShadowStrategies(partyTraceAI.m_partyBotEngine);
        partyTraceAI.m_engineShadowStrategiesRegistered = true;
    }

    std::shared_ptr<Strategy> activeEmptyStrategy(new Strategy("simulator_trace_active_empty"));
    activeEmptyStrategy->AddTrigger(std::make_shared<SimulatorTraceProbeTrigger>(
        "simulator_trace_active_empty", 0));
    partyTraceAI.m_partyBotEngine.AddStrategy(activeEmptyStrategy);
    for (uint32 i = 0; i < 3; ++i)
    {
        std::string const strategyName = "simulator_trace_cadence_strategy_" + std::to_string(i);
        std::string const triggerName = "simulator_trace_cadence_" + std::to_string(i);
        std::shared_ptr<Strategy> cadenceStrategy(new Strategy(strategyName));
        cadenceStrategy->AddTrigger(std::make_shared<SimulatorTraceProbeTrigger>(
            triggerName, 1000));
        partyTraceAI.m_partyBotEngine.AddStrategy(cadenceStrategy);
    }
    partyTraceAI.m_decisionTrace.BeginTick();

    PartyMemberSnapshot const* triggerBotSnapshot =
        triggerTraceSnapshot.FindMember(triggerTraceBot.guid);
    DecisionReport triggerReport;
    if (triggerTraceStarted.kind == EncounterTransitionKind::Started &&
        triggerBotSnapshot)
    {
        DecisionContext initialTriggerContext(
            triggerTraceSnapshot, *triggerBotSnapshot,
            *triggerTraceCoordinator, triggerTraceNow);
        DecisionReport const initialTriggerReport =
            partyTraceAI.m_partyBotEngine.Evaluate(initialTriggerContext);
        DecisionContext cadenceTriggerContext(
            triggerTraceSnapshot, *triggerBotSnapshot,
            *triggerTraceCoordinator, triggerTraceNow + 1);
        triggerReport = partyTraceAI.m_partyBotEngine.Evaluate(cadenceTriggerContext);
        (void)initialTriggerReport;
    }

    uint32 triggerActiveCount = 0;
    uint32 triggerInactiveCount = 0;
    uint32 triggerSkippedCount = 0;
    for (TriggerEvaluation const& observation : triggerReport.triggerEvaluations)
    {
        if (observation.status == TriggerEvaluationStatus::Active)
            ++triggerActiveCount;
        else if (observation.status == TriggerEvaluationStatus::Inactive)
            ++triggerInactiveCount;
        else
            ++triggerSkippedCount;
    }
    bool const hasAttributedHealingCandidate = triggerReport.candidates.size() == 1 &&
        triggerReport.candidates[0].candidate.strategyName == "healing_triage" &&
        triggerReport.candidates[0].candidate.triggerName == "healing_triage" &&
        triggerReport.candidates[0].candidate.actionName == "shadow_heal_triage";
    std::vector<std::size_t> const prioritizedTriggerIndices =
        SelectPriorityTriggerEvaluationIndices(triggerReport, 4);
    auto triggerAt = [&triggerReport](std::size_t index) -> TriggerEvaluation const& {
        return triggerReport.triggerEvaluations.at(index);
    };
    bool const triggerPriorityOrderPassed = prioritizedTriggerIndices.size() == 4 &&
        triggerAt(prioritizedTriggerIndices[0]).triggerName == "healing_triage" &&
        triggerAt(prioritizedTriggerIndices[0]).status == TriggerEvaluationStatus::Active &&
        triggerAt(prioritizedTriggerIndices[0]).candidateCount == 1 &&
        triggerAt(prioritizedTriggerIndices[1]).triggerName == "simulator_trace_active_empty" &&
        triggerAt(prioritizedTriggerIndices[1]).status == TriggerEvaluationStatus::Active &&
        triggerAt(prioritizedTriggerIndices[1]).candidateCount == 0 &&
        triggerAt(prioritizedTriggerIndices[2]).triggerName == "tank_target" &&
        triggerAt(prioritizedTriggerIndices[2]).status == TriggerEvaluationStatus::Inactive &&
        triggerAt(prioritizedTriggerIndices[3]).triggerName == "dps_target" &&
        triggerAt(prioritizedTriggerIndices[3]).status == TriggerEvaluationStatus::Inactive;
    bool const triggerReportValid =
        triggerTraceStarted.kind == EncounterTransitionKind::Started &&
        triggerBotSnapshot && triggerReport.triggerEvaluations.size() == 7 &&
        triggerReport.omittedTriggerEvaluations == 0 &&
        triggerActiveCount == 2 && triggerInactiveCount == 2 &&
        triggerSkippedCount == 3 && hasAttributedHealingCandidate &&
        triggerPriorityOrderPassed;

    bool triggerSummaryRecorded = false;
    uint32 triggerRowsRecorded = 0;
    bool triggerCandidateRecorded = false;
    if (triggerReportValid && partyTraceAI.m_decisionTrace.IsEnabled())
    {
        uint64 const encounterId = triggerTraceCoordinator->GetEncounterId();
        uint32 const elapsed = triggerTraceCoordinator->GetEncounterElapsedMs(triggerTraceNow);
        if (triggerTraceCoordinator->TryAdmitEncounterTraceEvent(
                encounterId, EncounterTraceEventClass::Detail) ==
            EncounterTraceAdmission::Accepted)
        {
            uint32 const sequence = triggerTraceCoordinator->NextEncounterSequence();
            partyTraceAI.m_decisionTrace.RecordEncounterTriggerSummary(
                tank, triggerTraceSnapshot.groupId, triggerTraceSnapshot.mapId,
                triggerTraceSnapshot.instanceId, encounterId, sequence, elapsed,
                static_cast<uint32>(triggerReport.triggerEvaluations.size()),
                triggerActiveCount + triggerInactiveCount,
                triggerActiveCount, triggerInactiveCount, triggerSkippedCount,
                triggerReport.omittedTriggerEvaluations, 4,
                static_cast<uint32>(triggerReport.triggerEvaluations.size() -
                    prioritizedTriggerIndices.size()));
            triggerSummaryRecorded = sequence != 0;
        }

        for (std::size_t triggerIndex : prioritizedTriggerIndices)
        {
            TriggerEvaluation const& observation =
                triggerReport.triggerEvaluations[triggerIndex];
            if (triggerTraceCoordinator->TryAdmitEncounterTraceEvent(
                    encounterId, EncounterTraceEventClass::Detail) !=
                EncounterTraceAdmission::Accepted)
                continue;
            uint32 const sequence = triggerTraceCoordinator->NextEncounterSequence();
            char const* status = observation.status == TriggerEvaluationStatus::Active
                ? "active" : (observation.status == TriggerEvaluationStatus::Inactive
                    ? "inactive" : "cadence_skipped");
            partyTraceAI.m_decisionTrace.RecordEncounterTrigger(
                tank, triggerTraceSnapshot.groupId, triggerTraceSnapshot.mapId,
                triggerTraceSnapshot.instanceId, encounterId, sequence, elapsed,
                "healer", observation.strategyName.c_str(),
                observation.triggerName.c_str(), observation.intervalMs,
                status, observation.candidateCount);
            if (sequence)
                ++triggerRowsRecorded;
        }

        CandidateEvaluation const& evaluation = triggerReport.candidates[0];
        ActionCandidate const& candidate = evaluation.candidate;
        if (triggerTraceCoordinator->TryAdmitEncounterTraceEvent(
                encounterId, EncounterTraceEventClass::Detail) ==
            EncounterTraceAdmission::Accepted)
        {
            uint32 const sequence = triggerTraceCoordinator->NextEncounterSequence();
            char const* status = evaluation.status == CandidateStatus::RejectedThreatUnknown
                ? "rejected_threat_unknown" : "candidate";
            partyTraceAI.m_decisionTrace.RecordEncounterCandidate(
                tank, triggerTraceSnapshot.groupId, triggerTraceSnapshot.mapId,
                triggerTraceSnapshot.instanceId, triggerTraceSnapshot.epoch,
                encounterId, sequence, elapsed, "healer",
                candidate.strategyName.c_str(), candidate.triggerName.c_str(),
                candidate.actionName.c_str(), candidate.targetGuid,
                candidate.spellId, static_cast<uint32>(candidate.channel),
                candidate.finalPriority, status,
                evaluation.reason.empty() ? "none" : evaluation.reason.c_str());
            triggerCandidateRecorded = sequence != 0;
        }
    }

    bool const f17TriggerTelemetryPassed =
        triggerReportValid && partyTraceAI.m_decisionTrace.IsEnabled() &&
        triggerSummaryRecorded && triggerRowsRecorded == 4 &&
        triggerCandidateRecorded && triggerPriorityOrderPassed;
    if (f17TriggerTelemetryPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: Core-backed PartyBotEngine prioritised trigger telemetry by actionable active state, then active-empty, then inactive; cadence-skipped rows were deprioritised by the four-row cap, while healer-candidate attribution remained intact.");

    // F29: exercise exclusive NewEngine while the bot is in combat. The
    // production out-of-combat follow policy must register but remain inactive.
    PartyBotAI newEngineAuthorityAI(
        dps, map.GetId(), map.GetInstanceId(),
        tank->GetPositionX(), tank->GetPositionY(),
        tank->GetPositionZ(), tank->GetOrientation());
    newEngineAuthorityAI.SetPlayer(tank);
    newEngineAuthorityAI.m_initialized = true;
    newEngineAuthorityAI.m_simulatorExecutionAuthorityOverride =
        static_cast<int32>(ExecutionAuthorityMode::NewEngine);
    tank->SetInCombatState();
    newEngineAuthorityAI.m_updateTimer.Reset(1000);
    PlayerAI* const previousTankAIForNewEngineAuthority = tank->AI();
    tank->SetAI(&newEngineAuthorityAI);
    newEngineAuthorityAI.UpdateAI(1000);
    bool const f29NewEngineAuthorityPassed =
        tank->IsInCombat() &&
        newEngineAuthorityAI.m_simulatorNewEngineTickCount == 1 &&
        newEngineAuthorityAI.m_simulatorLegacyPolicyEntryCount == 0 &&
        newEngineAuthorityAI.m_simulatorLegacyPolicyDispatchCount == 0 &&
        newEngineAuthorityAI.m_simulatorEngineDecisionCycleCount == 1 &&
        newEngineAuthorityAI.m_simulatorEngineCandidateCount == 0 &&
        newEngineAuthorityAI.m_newEngineStrategiesRegistered &&
        !newEngineAuthorityAI.m_engineShadowStrategiesRegistered &&
        newEngineAuthorityAI.m_partyCoordinator &&
        newEngineAuthorityAI.m_partyCoordinatorMapId == map.GetId() &&
        newEngineAuthorityAI.m_partyCoordinatorInstanceId == map.GetInstanceId();
    tank->SetAI(previousTankAIForNewEngineAuthority);
    sLog.Out(LOG_BASIC, f29NewEngineAuthorityPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] NewEngine authority tick: newEngineTicks=%u legacyEntries=%u legacyDispatches=%u decisionCycles=%u candidates=%u newEnginePolicies=%u shadowPolicies=%u inCombat=%u coordinator=%u.",
             newEngineAuthorityAI.m_simulatorNewEngineTickCount,
             newEngineAuthorityAI.m_simulatorLegacyPolicyEntryCount,
             newEngineAuthorityAI.m_simulatorLegacyPolicyDispatchCount,
             newEngineAuthorityAI.m_simulatorEngineDecisionCycleCount,
             newEngineAuthorityAI.m_simulatorEngineCandidateCount,
             newEngineAuthorityAI.m_newEngineStrategiesRegistered ? 1u : 0u,
             newEngineAuthorityAI.m_engineShadowStrategiesRegistered ? 1u : 0u,
             tank->IsInCombat() ? 1u : 0u,
             newEngineAuthorityAI.m_partyCoordinator ? 1u : 0u);
    if (f29NewEngineAuthorityPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: F29 real PartyBotAI::UpdateAI registered the production out-of-combat follow policy but emitted no candidate while the bot was in combat, with no Legacy policy entry or Shadow-policy registration.");

    dps->SetAI(previousDpsAIForPartyTrace);
    secondPartyTraceAI.Remove();
    tank->SetAI(previousTankAIForPartyTrace);
    partyTraceAI.Remove(); // The simulator logs out/deletes Players before this stack frame ends.

    bool const f10ChecksPassed = f9ChecksPassed && petCleanupPassed &&
        f12ChecksPassed && healerHoldsAggroOverDps &&
        tankDied && tankDeathObserverValid && tankCorpseTransitionSuppressed &&
        tankResurrectionObserved && partyBotEncounterLifecycleTraced &&
        partyBotTraceBudgetPassed && unmanagedMemberStateTracePassed &&
        shutdownFlushNoDuplicate && instanceTransferTracePassed &&
        f17TriggerTelemetryPassed && f29NewEngineAuthorityPassed &&
        tankThreatReferencesRemoved && healerRetargetedAfterTankDeath &&
        tankDeathSnapshotValid && tankDeathRosterValid && tankDeathThreatSnapshotValid &&
        healerDied && healerThreatReferencesRemoved && dpsRetargetedAfterHealerDeath &&
        healerDeathSnapshotValid && healerDeathRosterValid && healerDeathThreatSnapshotValid;

    // F43: exercise point-movement correlation through the actual five-Player
    // Group PartySnapshotBuilder only after all threat/death fixtures completed.
    // Select members whose existing generators can safely be stacked: Core's
    // MotionMaster::Mutate deliberately expires Chase/Home/Distancing/Distraction
    // generators rather than preserving them below a new Point movement.
    Player* enginePointPlayer = nullptr;
    Player* legacyPointPlayer = nullptr;
    Player* idlePointPlayer = nullptr;
    MovementGenerator const* enginePointBefore = nullptr;
    MovementGenerator const* legacyPointBefore = nullptr;
    MovementGenerator const* idlePointBefore = nullptr;
    MotionMaster* enginePointMotion = nullptr;
    MotionMaster* legacyPointMotion = nullptr;
    auto canStackPointMovement = [](Player* player) -> bool
    {
        if (!player || !player->IsInWorld() || !player->IsAlive() ||
            player->GetCurrentSpell(CURRENT_GENERIC_SPELL) ||
            player->GetCurrentSpell(CURRENT_CHANNELED_SPELL) ||
            player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            return false;
        MotionMaster* motion = player->GetMotionMaster();
        if (!motion || motion->empty())
            return false;
        MovementGeneratorType const type =
            motion->GetCurrentMovementGeneratorType();
        return type != CHASE_MOTION_TYPE && type != HOME_MOTION_TYPE &&
            type != DISTRACT_MOTION_TYPE && type != DISTANCING_MOTION_TYPE;
    };
    for (std::size_t index = 2; index < players.size(); ++index)
    {
        Player* member = players[index];
        if (!canStackPointMovement(member))
            continue;
        MotionMaster* motion = member->GetMotionMaster();
        MovementGenerator const* current = motion->GetCurrent();
        if (!enginePointPlayer)
        {
            enginePointPlayer = member;
            enginePointMotion = motion;
            enginePointBefore = current;
        }
        else if (!legacyPointPlayer)
        {
            legacyPointPlayer = member;
            legacyPointMotion = motion;
            legacyPointBefore = current;
        }
    }
    // The first two overlay-safe candidates may both be Point generators.
    // Idle correlation can come from any other Group member (including Tank
    // or Healer), because it is only observed, never moved or expired.
    for (Player* member : players)
    {
        if (!member || member == enginePointPlayer ||
            member == legacyPointPlayer)
            continue;
        MotionMaster* motion = member->GetMotionMaster();
        if (!motion || motion->empty() ||
            motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            continue;
        idlePointPlayer = member;
        idlePointBefore = motion->GetCurrent();
        break;
    }

    ActionId const groupPointSnapshotActionId =
        static_cast<ActionId>(0xF4310001ull);
    uint64 const groupPointSnapshotEncounterId = 0xF4310002ull;
    uint32 const groupPointSnapshotPointId = 0xF431u;
    uint32 const legacyPointSnapshotPointId = 0xF432u;
    bool const groupPointMoveStarted = enginePointPlayer && enginePointMotion &&
        enginePointBefore &&
        enginePointMotion->MovePointForEngine(
            groupPointSnapshotPointId, enginePointPlayer->GetPositionX() + 6.0f,
            enginePointPlayer->GetPositionY(), enginePointPlayer->GetPositionZ(),
            MOVE_NONE, groupPointSnapshotActionId,
            groupPointSnapshotEncounterId, 1.5f) &&
        enginePointMotion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
        enginePointMotion->GetCurrent() != enginePointBefore;
    if (legacyPointPlayer && legacyPointMotion)
        legacyPointMotion->MovePoint(
            legacyPointSnapshotPointId, legacyPointPlayer->GetPositionX() + 6.0f,
            legacyPointPlayer->GetPositionY(), legacyPointPlayer->GetPositionZ(),
            MOVE_NONE);
    bool const legacyPointMoveStarted = legacyPointPlayer && legacyPointMotion &&
        legacyPointBefore &&
        legacyPointMotion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
        legacyPointMotion->GetCurrent() != legacyPointBefore;
    bool const idleMemberNotMoving = idlePointPlayer && idlePointBefore &&
        idlePointBefore->GetMovementGeneratorType() != POINT_MOTION_TYPE &&
        idlePointPlayer->GetMotionMaster()->GetCurrent() == idlePointBefore;
    PartyCoordinator pointMovementSnapshotCoordinator;
    PartySnapshot const* pointMovementSnapshot =
        groupPointMoveStarted && legacyPointMoveStarted && idleMemberNotMoving
            ? PartySnapshotBuilder::GetOrBuild(
                enginePointPlayer, pointMovementSnapshotCoordinator,
                WorldTimer::getMSTime(), 0)
            : nullptr;
    PartyMemberSnapshot const* enginePointMember = pointMovementSnapshot
        ? pointMovementSnapshot->FindMember(
            enginePointPlayer->GetObjectGuid().GetRawValue()) : nullptr;
    PartyMemberSnapshot const* legacyPointMember = pointMovementSnapshot
        ? pointMovementSnapshot->FindMember(
            legacyPointPlayer->GetObjectGuid().GetRawValue()) : nullptr;
    PartyMemberSnapshot const* idlePointMember = pointMovementSnapshot
        ? pointMovementSnapshot->FindMember(
            idlePointPlayer->GetObjectGuid().GetRawValue()) : nullptr;
    bool const enginePointCorrelationCopied = enginePointMember &&
        enginePointMember->pointMovementActionId == groupPointSnapshotActionId &&
        enginePointMember->pointMovementEncounterId ==
            groupPointSnapshotEncounterId &&
        enginePointMember->pointMovementPointId == groupPointSnapshotPointId;
    bool const legacyPointHasNoEngineOwnership = legacyPointMember &&
        legacyPointMember->pointMovementActionId == 0 &&
        legacyPointMember->pointMovementEncounterId == 0 &&
        legacyPointMember->pointMovementPointId == legacyPointSnapshotPointId;
    bool const idlePointCorrelationEmpty = idlePointMember &&
        idlePointMember->pointMovementActionId == 0 &&
        idlePointMember->pointMovementEncounterId == 0 &&
        idlePointMember->pointMovementPointId == 0;
    bool const pointMovementSnapshotGroupValid = pointMovementSnapshot &&
        pointMovementSnapshot->groupId == group->GetId() &&
        pointMovementSnapshot->mapId == map.GetId() &&
        pointMovementSnapshot->instanceId == map.GetInstanceId() &&
        pointMovementSnapshot->members.size() == MAX_GROUP_SIZE;

    // Expire only the temporary top generators and prove each previous native
    // generator is restored; do not update the shared Map or advance any Player.
    if (legacyPointMoveStarted)
        legacyPointMotion->MovementExpired(false);
    if (groupPointMoveStarted)
        enginePointMotion->MovementExpired(false);
    bool const enginePointGeneratorRestored = enginePointMotion &&
        enginePointMotion->GetCurrent() == enginePointBefore;
    bool const legacyPointGeneratorRestored = legacyPointMotion &&
        legacyPointMotion->GetCurrent() == legacyPointBefore;
    bool const idlePointGeneratorUnchanged = idlePointPlayer &&
        idlePointPlayer->GetMotionMaster()->GetCurrent() == idlePointBefore;
    bool const pointMovementSnapshotCleanup = enginePointGeneratorRestored &&
        legacyPointGeneratorRestored && idlePointGeneratorUnchanged;
    MovementGeneratorType const engineAfterType = enginePointMotion
        ? enginePointMotion->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    MovementGeneratorType const legacyAfterType = legacyPointMotion
        ? legacyPointMotion->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    bool const pointMovementSnapshotBuilderPassed =
        pointMovementSnapshotGroupValid && groupPointMoveStarted &&
        legacyPointMoveStarted && idleMemberNotMoving &&
        enginePointCorrelationCopied && legacyPointHasNoEngineOwnership &&
        idlePointCorrelationEmpty && pointMovementSnapshotCleanup;
    sLog.Out(LOG_BASIC,
        pointMovementSnapshotBuilderPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] Group PartySnapshotBuilder point movement: group=%u engineStarted=%u engineCopied=%u legacyStarted=%u legacyUnowned=%u idleEmpty=%u engineRestored=%u legacyRestored=%u idleUnchanged=%u engineAfterType=%u legacyAfterType=%u.",
        pointMovementSnapshotGroupValid ? 1u : 0u,
        groupPointMoveStarted ? 1u : 0u,
        enginePointCorrelationCopied ? 1u : 0u,
        legacyPointMoveStarted ? 1u : 0u,
        legacyPointHasNoEngineOwnership ? 1u : 0u,
        idlePointCorrelationEmpty ? 1u : 0u,
        enginePointGeneratorRestored ? 1u : 0u,
        legacyPointGeneratorRestored ? 1u : 0u,
        idlePointGeneratorUnchanged ? 1u : 0u,
        static_cast<uint32>(engineAfterType),
        static_cast<uint32>(legacyAfterType));
    if (pointMovementSnapshotBuilderPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: real five-Player Group PartySnapshotBuilder copied Engine point action/encounter/point correlation from the current Core generator, retained legacy point IDs without Engine ownership, and left idle member movement IDs empty.");
    else
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] Group PartySnapshotBuilder point-movement integration failed after Group threat/death checks.");

    uint32 const groupIdAfterEvents = group->GetId();
    bool const groupStillRegistered = registeredBeforeCleanup &&
        sObjectMgr.GetGroupById(groupIdAfterEvents) == group.get();
    bool const cleaned = cleanup();
    bool const groupRemoved = sObjectMgr.GetGroupById(groupIdAfterEvents) == nullptr;
    bool const cleanupPassed = cleaned && groupRemoved;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Player resurrection observer: observed=%u callbacks=%u transitions=%u alive=%u cleanup=1.",
             tankResurrectionObserved ? 1u : 0u,
             tankResurrectionObserver.callbackCount,
             tankResurrectionObserver.transitionCount,
             tank->IsAlive() ? 1u : 0u);
    if (tankResurrectionObserved)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: Core Player::ResurrectPlayer emitted exactly one dead-to-alive observer transition after tank death recovery snapshots were captured.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Player alive-state observer: death=%u sameStateFiltered=%u callbacks=%u sameStateCallbacks=%u transitions=%u cleanup=1.",
             tankDeathObserverValid ? 1u : 0u,
             tankCorpseTransitionSuppressed ? 1u : 0u,
             tankAliveObserver.callbackCount,
             tankAliveObserver.sameStateCallbacks,
             tankAliveObserver.transitionCount);
    if (tankDeathObserverValid && tankCorpseTransitionSuppressed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: Core Player::SetDeathState invoked the observer for JUST_DIED and CORPSE, while the alive-state transition filter emitted exactly one alive-to-dead event.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PartyBotAI encounter lifecycle: started=%u death=%u resurrection=%u sequenceDelta=%u alive=%u.",
             partyTraceStarted.kind == EncounterTransitionKind::Started ? 1u : 0u,
             partyTraceDied ? 1u : 0u,
             partyBotEncounterLifecycleTraced ? 1u : 0u,
             partyTraceSequenceAfter - partyTraceSequenceBefore - 1u,
             tank->IsAlive() ? 1u : 0u);
    if (partyBotEncounterLifecycleTraced)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: PartyBotAI emitted encounter-correlated death and resurrection trace records through Core PlayerAI callbacks, while the repeated CORPSE state was suppressed.");
    if (partyBotDamageTracePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: F22 recorded the real Core Unit::DealDamage health delta through PlayerAI/PartyBotAI with attacker, spell and damage-type attribution.");
    if (f21BotStateTracePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: F21 copied real Core shapeshift form, pending spell ID/state/target and member position, plus hostile creature entry/rank/world-boss identity into the bounded party heartbeat.");

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Group death recovery: dpsDamage=%u/%u dpsThreat=%.1f/%.1f healerHoldsAggro=%u tankDeath=%u tankRefsRemoved=%u healerRetargetAfterTankDeath=%u/%u tankSnapshot=%u tankRoster=%u tankThreatSnapshot=%u healerDeath=%u healerRefsRemoved=%u dpsRetargetAfterHealerDeath=%u/%u healerSnapshot=%u healerRoster=%u healerThreatSnapshot=%u registered=%u groupRemoved=%u cleanup=%u.",
             dpsDamage0, dpsDamage1, dpsThreat0, dpsThreat1,
             healerHoldsAggroOverDps ? 1u : 0u,
             tankDied ? 1u : 0u, tankThreatReferencesRemoved ? 1u : 0u,
             tankDeathSelected0 ? 1u : 0u, tankDeathSelected1 ? 1u : 0u,
             tankDeathSnapshotValid ? 1u : 0u, tankDeathRosterValid ? 1u : 0u,
             tankDeathThreatSnapshotValid ? 1u : 0u,
             healerDied ? 1u : 0u, healerThreatReferencesRemoved ? 1u : 0u,
             healerDeathSelected0 ? 1u : 0u, healerDeathSelected1 ? 1u : 0u,
             healerDeathSnapshotValid ? 1u : 0u, healerDeathRosterValid ? 1u : 0u,
             healerDeathThreatSnapshotValid ? 1u : 0u,
             registeredBeforeCleanup ? 1u : 0u, groupRemoved ? 1u : 0u,
             cleanupPassed ? 1u : 0u);

    if (!f9ChecksPassed || !petCleanupPassed || !f12ChecksPassed || !groupStillRegistered)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Combined Group F8/F9/F12 threat-reference scenario failed before death recovery.");
        return false;
    }

    if (!f10ChecksPassed || !cleanupPassed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core Tank/Healer death-reference cleanup and multi-add recovery failed.");
        return false;
    }

    // The F43 Group-backed snapshot probe must be a hard runtime gate. A log-only
    // failure here would let the simulator report Runtime PASS without validating
    // that PartySnapshotBuilder copied Engine-owned Point correlation from Core.
    if (!pointMovementSnapshotBuilderPassed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] F43 Group-backed point-movement snapshot integration failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Core death cleanup removed Main Tank and healer references from both adds; SelectHostileTarget retargeted first to the surviving healer and then to the surviving DPS, and PartySnapshotBuilder copied both post-death states.");
    return true;
}




class SimulatorCandidateTraceOverflowTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive &&
            context.bot.roleId == static_cast<std::uint8_t>(PartyRole::Healer);
    }
    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (uint32 i = 0; i < 4; ++i)
        {
            ActionCandidate candidate;
            candidate.actionName = "missing_candidate_trace_probe";
            candidate.targetGuid = context.snapshot.members.size() > 1
                ? context.snapshot.members[1].guid : context.bot.guid;
            candidate.basePriority = 200.0f - static_cast<float>(i) * 10.0f;
            candidate.channel = ActionChannel::Utility;
            candidates.push_back(candidate);
        }
    }
private:
    std::string m_name = "simulator_candidate_trace_overflow";
};

bool RunEngineCoreItemUseSmokeTest(Map& map)
{
    static uint32 const MinorHealingPotionItemId = 118;
    ItemPrototype const* potionPrototype =
        sObjectMgr.GetItemPrototype(MinorHealingPotionItemId);
    uint32 spellSlot = MAX_ITEM_PROTO_SPELLS;
    if (potionPrototype)
    {
        for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (potionPrototype->Spells[i].SpellId &&
                potionPrototype->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                spellSlot = i;
                break;
            }
    }

    bool const prototypeValid = potionPrototype &&
        spellSlot < MAX_ITEM_PROTO_SPELLS &&
        potionPrototype->Spells[spellSlot].SpellCharges < 0 &&
        sSpellMgr.GetSpellEntry(potionPrototype->Spells[spellSlot].SpellId);
    if (!prototypeValid)
        return false;

    // Inventory-backed diagnostic for a deterministic F42 cancellation
    // fixture. Search the exact item_template rows loaded by this simulator
    // together with its pinned Spell.dbc records; do not assume a retail item
    // ID or synthesize timing/effect fields.
    struct DelayedItemUseCandidate
    {
        ItemPrototype const* item = nullptr;
        SpellEntry const* spell = nullptr;
        uint8 slot = 0xff;
        uint32 castTimeMs = 0;
    };
    std::vector<DelayedItemUseCandidate> delayedItemUseCandidates;
    for (auto const& itemPair : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& item = itemPair.second;
        if (item.ItemId != itemPair.first || item.Stackable >= 2)
            continue;

        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            _ItemSpell const& itemSpell = item.Spells[slot];
            if (!itemSpell.SpellId ||
                itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE ||
                !itemSpell.SpellCharges)
                continue;

            SpellEntry const* spell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
            if (!spell || spell->speed <= 0.0f)
                continue;

            uint32 const castTimeMs = spell->GetCastTime(nullptr, nullptr);

            DelayedItemUseCandidate candidate;
            candidate.item = &item;
            candidate.spell = spell;
            candidate.slot = slot;
            candidate.castTimeMs = castTimeMs;
            delayedItemUseCandidates.push_back(candidate);
        }
    }
    std::sort(delayedItemUseCandidates.begin(), delayedItemUseCandidates.end(),
        [](DelayedItemUseCandidate const& lhs,
           DelayedItemUseCandidate const& rhs)
        {
            if (lhs.item->ItemId != rhs.item->ItemId)
                return lhs.item->ItemId < rhs.item->ItemId;
            return lhs.slot < rhs.slot;
        });
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] F42 delayed ON_USE scan: candidates=%u (real item_template + Spell.dbc; ON_USE, finite charges, non-stackable, speed>0; cast time is reported).",
        static_cast<uint32>(delayedItemUseCandidates.size()));
    for (std::size_t i = 0;
         i < delayedItemUseCandidates.size() && i < 24; ++i)
    {
        DelayedItemUseCandidate const& candidate = delayedItemUseCandidates[i];
        ItemPrototype const& item = *candidate.item;
        SpellEntry const& spell = *candidate.spell;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] F42 delayed ON_USE candidate: item=%u name=%s slot=%u charges=%d stackable=%u requiredLevel=%u spell=%u spellName=%s castTimeMs=%u speed=%.2f effect0=%u targetA0=%u targetB0=%u effect1=%u targetA1=%u targetB1=%u.",
            item.ItemId, item.Name1 ? item.Name1 : "<unnamed>",
            candidate.slot, item.Spells[candidate.slot].SpellCharges,
            item.Stackable, item.RequiredLevel, spell.Id,
            spell.SpellName[0].c_str(), candidate.castTimeMs, spell.speed,
            spell.Effect[0], spell.EffectImplicitTargetA[0],
            spell.EffectImplicitTargetB[0], spell.Effect[1],
            spell.EffectImplicitTargetA[1], spell.EffectImplicitTargetB[1]);
    }

    class SimulatorItemUseCapture final : public ItemUseTerminalListener
    {
    public:
        void OnItemUseTerminal(ItemUseTerminalEvent const& event) override
        {
            events.push_back(event);
        }
        std::vector<ItemUseTerminalEvent> events;
    };

    // First Player proves the successful ON_USE path using the same implicit
    // self-target semantics as HandleUseItemOpcode (no explicit unit target).
    WorldSession successSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* successPlayer = nullptr;
    if (!CreateSimulatorPlayer(map, 1, successSession, successPlayer, 0.0f, 9.0f, 4.0f))
    {
        if (successSession.GetPlayer())
            successSession.LogoutPlayer(false);
        else if (successPlayer)
            delete successPlayer;
        return false;
    }

    PartyBotAI successAI(successPlayer, map.GetId(), map.GetInstanceId(),
                         successPlayer->GetPositionX(), successPlayer->GetPositionY(),
                         successPlayer->GetPositionZ(), successPlayer->GetOrientation());
    successAI.SetPlayer(successPlayer);
    successPlayer->SetAI(&successAI);
    std::shared_ptr<SimulatorItemUseCapture> successItemUseCapture(
        new SimulatorItemUseCapture());
    bool const successListenerAdded =
        successAI.m_partyBotEngine.AddItemUseTerminalListener(successItemUseCapture);

    Item* potion = successPlayer->StoreNewItemInInventorySlot(MinorHealingPotionItemId, 1);
    ObjectGuid const potionGuid = potion ? potion->GetObjectGuid() : ObjectGuid();
    int32 const initialCharges = potion ? potion->GetSpellCharges(spellSlot) : 0;
    uint32 const potionHealthBefore = successPlayer->GetMaxHealth() / 2;
    successPlayer->SetHealth(potionHealthBefore);

    SpellCastTargets validTargets;
    validTargets.PrepareForSpellSystem(successPlayer);
    uint64 const successItemUseId = potion
        ? successPlayer->CastItemUseSpell(potion, validTargets) : 0;

    for (uint32 tick = 0; tick < 8 &&
         successPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    uint32 const potionHealthAfter = successPlayer->GetHealth();
    bool const potionConsumed = potionGuid &&
        successPlayer->GetItemByGuid(potionGuid) == nullptr;
    bool const successfulUse = potionGuid && initialCharges < 0 &&
        potionHealthAfter > potionHealthBefore && potionConsumed &&
        successPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;

    std::uint32_t successMutationCount = 0;
    ItemUseMutationEvent successMutation;
    while (successAI.m_partyBotEngine.PopItemUseMutationEvent(successMutation))
        ++successMutationCount;
    bool const successfulAggregate = successListenerAdded &&
        successItemUseId != 0 &&
        successItemUseCapture->events.size() == 1 &&
        successItemUseCapture->events[0].itemUseId == successItemUseId &&
        successItemUseCapture->events[0].status ==
            ItemUseAggregateStatus::AllSucceeded &&
        successItemUseCapture->events[0].childCount == 1 &&
        successItemUseCapture->events[0].completedChildCount == 1 &&
        successItemUseCapture->events[0].observedMutationCount == successMutationCount &&
        successMutationCount >= 1 &&
        successAI.m_partyBotEngine.GetPendingItemUseCount() == 0;
    successPlayer->SetAI(nullptr);
    successSession.LogoutPlayer(false);
    bool const successPlayerCleaned = successSession.GetPlayer() == nullptr;

    // A separate fresh Player avoids the previous potion's item cooldown/GCD;
    // the only deliberate rejection cause is the zero-charge item state.
    WorldSession rejectedSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* rejectedPlayer = nullptr;
    if (!CreateSimulatorPlayer(map, 1, rejectedSession, rejectedPlayer,
                               0.0f, 9.0f, 4.0f))
    {
        if (rejectedSession.GetPlayer())
            rejectedSession.LogoutPlayer(false);
        else if (rejectedPlayer)
            delete rejectedPlayer;
        return false;
    }

    Item* noChargePotion =
        rejectedPlayer->StoreNewItemInInventorySlot(MinorHealingPotionItemId, 1);
    ObjectGuid const noChargeGuid = noChargePotion
        ? noChargePotion->GetObjectGuid() : ObjectGuid();
    if (noChargePotion)
        noChargePotion->SetSpellCharges(spellSlot, 0);
    PartyBotAI rejectedAI(rejectedPlayer, map.GetId(), map.GetInstanceId(),
                          rejectedPlayer->GetPositionX(), rejectedPlayer->GetPositionY(),
                          rejectedPlayer->GetPositionZ(), rejectedPlayer->GetOrientation());
    rejectedAI.SetPlayer(rejectedPlayer);
    rejectedPlayer->SetAI(&rejectedAI);
    std::shared_ptr<SimulatorItemUseCapture> rejectedItemUseCapture(
        new SimulatorItemUseCapture());
    bool const rejectedListenerAdded =
        rejectedAI.m_partyBotEngine.AddItemUseTerminalListener(rejectedItemUseCapture);

    uint32 const rejectedHealthBefore = rejectedPlayer->GetMaxHealth() / 2;
    rejectedPlayer->SetHealth(rejectedHealthBefore);

    SpellCastTargets rejectedTargets;
    rejectedTargets.PrepareForSpellSystem(rejectedPlayer);
    uint64 const rejectedItemUseId = noChargePotion
        ? rejectedPlayer->CastItemUseSpell(noChargePotion, rejectedTargets) : 0;

    for (uint32 tick = 0; tick < 4 &&
         rejectedPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    Item* rejectedStillOwned = noChargeGuid
        ? rejectedPlayer->GetItemByGuid(noChargeGuid) : nullptr;
    bool const rejectedWithoutConsumption = noChargeGuid && rejectedStillOwned &&
        rejectedStillOwned->GetCount() == 1 &&
        rejectedStillOwned->GetSpellCharges(spellSlot) == 0 &&
        rejectedPlayer->GetHealth() == rejectedHealthBefore &&
        rejectedPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
    bool const rejectedAggregate = rejectedListenerAdded &&
        rejectedItemUseId != 0 &&
        rejectedItemUseCapture->events.size() == 1 &&
        rejectedItemUseCapture->events[0].itemUseId == rejectedItemUseId &&
        rejectedItemUseCapture->events[0].status ==
            ItemUseAggregateStatus::AllFailed &&
        rejectedItemUseCapture->events[0].childCount == 1 &&
        rejectedItemUseCapture->events[0].completedChildCount == 1 &&
        rejectedItemUseCapture->events[0].observedMutationCount == 0 &&
        rejectedAI.m_partyBotEngine.GetPendingItemUseCount() == 0;
    rejectedPlayer->SetAI(nullptr);
    rejectedSession.LogoutPlayer(false);
    bool const rejectedPlayerCleaned = rejectedSession.GetPlayer() == nullptr;

    // Temporarily duplicate the potion's ON_USE spell into one empty prototype
    // slot. This override exists only in the disposable simulator process and is
    // restored before returning; the Core still creates/prepares both real Spells.
    uint8 multiSpellSlot = MAX_ITEM_PROTO_SPELLS;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (i != spellSlot && !potionPrototype->Spells[i].SpellId) {
            multiSpellSlot = i;
            break;
        }
    ItemPrototype* mutablePotionPrototype =
        const_cast<ItemPrototype*>(potionPrototype);
    auto const savedFirstSpell = mutablePotionPrototype->Spells[spellSlot];
    auto const savedSecondSpell = mutablePotionPrototype->Spells[
        multiSpellSlot < MAX_ITEM_PROTO_SPELLS ? multiSpellSlot : spellSlot];
    uint32 const savedPotionStackable = mutablePotionPrototype->Stackable;

    bool multiSlotFixturePassed = false;
    std::uint64_t multiRootId = 0;
    std::uint8_t multiChildCount = 0;
    std::uint8_t multiMutationCount = 0;
    bool multiFirstTriggered = true;
    bool multiSecondTriggered = false;
    bool multiChargesConsumed = false;
    bool multiPlayerCleaned = false;

    if (multiSpellSlot < MAX_ITEM_PROTO_SPELLS)
    {
        // Core persists finite spell charges on non-stackable items. Make this
        // disposable one-count fixture exercise that real mutation path.
        mutablePotionPrototype->Stackable = 1;
        mutablePotionPrototype->Spells[spellSlot].SpellCharges = 2;
        mutablePotionPrototype->Spells[multiSpellSlot] =
            mutablePotionPrototype->Spells[spellSlot];
        mutablePotionPrototype->Spells[multiSpellSlot].SpellCharges = 2;

        WorldSession multiSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
        Player* multiPlayer = nullptr;
        if (CreateSimulatorPlayer(map, 1, multiSession, multiPlayer,
                                  0.0f, 9.0f, 4.0f))
        {
            PartyBotAI multiAI(multiPlayer, map.GetId(), map.GetInstanceId(),
                               multiPlayer->GetPositionX(), multiPlayer->GetPositionY(),
                               multiPlayer->GetPositionZ(), multiPlayer->GetOrientation());
            multiAI.SetPlayer(multiPlayer);
            multiPlayer->SetAI(&multiAI);
            std::shared_ptr<SimulatorItemUseCapture> multiCapture(
                new SimulatorItemUseCapture());
            bool const listenerAdded =
                multiAI.m_partyBotEngine.AddItemUseTerminalListener(multiCapture);

            Item* multiPotion =
                multiPlayer->StoreNewItemInInventorySlot(MinorHealingPotionItemId, 1);
            ObjectGuid const multiPotionGuid = multiPotion
                ? multiPotion->GetObjectGuid() : ObjectGuid();
            if (multiPotion)
            {
                multiPotion->SetSpellCharges(spellSlot, 2);
                multiPotion->SetSpellCharges(multiSpellSlot, 2);
            }

            uint32 const multiHealthBefore = multiPlayer->GetMaxHealth() / 2;
            multiPlayer->SetHealth(multiHealthBefore);
            SpellCastTargets multiTargets;
            multiTargets.PrepareForSpellSystem(multiPlayer);
            multiRootId = multiPotion
                ? multiPlayer->CastItemUseSpell(multiPotion, multiTargets) : 0;

            for (uint32 tick = 0; tick < 8 &&
                 (multiAI.m_partyBotEngine.GetPendingItemUseCount() != 0 ||
                  multiPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                map.Update(250);
            }

            Item* multiStillOwned = multiPotionGuid
                ? multiPlayer->GetItemByGuid(multiPotionGuid) : nullptr;
            multiChargesConsumed = multiStillOwned &&
                multiStillOwned->GetCount() == 1 &&
                multiStillOwned->GetSpellCharges(spellSlot) == 1 &&
                multiStillOwned->GetSpellCharges(multiSpellSlot) == 1;

            multiChildCount = multiCapture->events.size() == 1
                ? multiCapture->events[0].childCount : 0;
            if (multiCapture->events.size() == 1 && multiChildCount == 2)
            {
                ItemUseTerminalEvent const& event = multiCapture->events[0];
                multiFirstTriggered = event.children[0].triggered;
                multiSecondTriggered = event.children[1].triggered;
                multiSlotFixturePassed = listenerAdded && multiRootId &&
                    event.itemUseId == multiRootId &&
                    event.status == ItemUseAggregateStatus::AllSucceeded &&
                    event.completedChildCount == 2 &&
                    event.children[0].slotIndex == spellSlot &&
                    event.children[0].spellId == savedFirstSpell.SpellId &&
                    event.children[0].status == ItemUseChildStatus::Succeeded &&
                    !event.children[0].triggered &&
                    event.children[1].slotIndex == multiSpellSlot &&
                    event.children[1].spellId == savedFirstSpell.SpellId &&
                    event.children[1].status == ItemUseChildStatus::Succeeded &&
                    event.children[1].triggered &&
                    event.observedMutationCount == 2 &&
                    multiChargesConsumed &&
                    multiPlayer->GetHealth() > multiHealthBefore &&
                    multiAI.m_partyBotEngine.GetPendingItemUseCount() == 0;

                ItemUseMutationEvent mutation;
                std::uint8_t mutationCount = 0;
                bool sawFirstCharge = false, sawSecondCharge = false;
                while (multiAI.m_partyBotEngine.PopItemUseMutationEvent(mutation))
                {
                    ++mutationCount;
                    if (mutation.kind != ItemUseMutationKind::ChargeChanged ||
                        mutation.childId != event.children[0].childId ||
                        mutation.itemGuid != multiPotionGuid.GetRawValue() ||
                        mutation.beforeCharges != 2 || mutation.afterCharges != 1)
                        multiSlotFixturePassed = false;
                    if (mutation.slotIndex == spellSlot)
                        sawFirstCharge = true;
                    if (mutation.slotIndex == multiSpellSlot)
                        sawSecondCharge = true;
                }
                multiMutationCount = mutationCount;
                multiSlotFixturePassed = multiSlotFixturePassed &&
                    mutationCount == 2 && sawFirstCharge && sawSecondCharge;
            }
            multiPlayer->SetAI(nullptr);
            multiSession.LogoutPlayer(false);
            multiPlayerCleaned = multiSession.GetPlayer() == nullptr;
        }
    }

    mutablePotionPrototype->Spells[spellSlot] = savedFirstSpell;
    if (multiSpellSlot < MAX_ITEM_PROTO_SPELLS) {
        mutablePotionPrototype->Spells[multiSpellSlot] = savedSecondSpell;
        mutablePotionPrototype->Stackable = savedPotionStackable;
    }

    // A pinned Vanilla 5875 item_template row with a real missile spell
    // exercises both sides of Core's TakeCastItem timing. Brimstone casts
    // Fireball (2s cast, 24 yd/s); the first Player cancels while PREPARING,
    // the second is interrupted after launch/mutation while DELAYED.
    static uint32 const DelayedItemId = 1402; // Brimstone
    static uint32 const DelayedSpellId = 143; // Fireball
    ItemPrototype const* delayedPrototype =
        sObjectMgr.GetItemPrototype(DelayedItemId);
    SpellEntry const* delayedSpell = sSpellMgr.GetSpellEntry(DelayedSpellId);
    uint8 delayedSpellSlot = 0xff;
    if (delayedPrototype)
    {
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            _ItemSpell const& itemSpell = delayedPrototype->Spells[slot];
            if (itemSpell.SpellId == DelayedSpellId &&
                itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                delayedSpellSlot = slot;
                break;
            }
        }
    }
    bool const delayedDataValid = delayedPrototype && delayedSpell &&
        delayedSpellSlot < MAX_ITEM_PROTO_SPELLS &&
        delayedPrototype->Spells[delayedSpellSlot].SpellCharges == -5 &&
        delayedPrototype->Stackable < 2 &&
        delayedSpell->GetCastTime(nullptr, nullptr) == 2000 &&
        delayedSpell->speed == 24.0f &&
        delayedSpell->Effect[EFFECT_INDEX_0] == SPELL_EFFECT_SCHOOL_DAMAGE &&
        delayedSpell->EffectImplicitTargetA[EFFECT_INDEX_0] == 6;

    CreatureInfo const* delayedTargetInfo = FindSafeCreatureTemplate();
    uint32 delayedPartyFaction = 0, delayedHostileFaction = 0;
    bool const delayedFactionsValid =
        FindMutuallyHostileFactions(delayedPartyFaction, delayedHostileFaction);
    SimulatorActor delayedTarget;
    bool const delayedTargetCreated = delayedDataValid &&
        delayedTargetInfo && delayedFactionsValid &&
        CreateSimulatorActor(map, *delayedTargetInfo, delayedHostileFaction,
                             20.0f, 9.0f, delayedTarget);
    bool preCancelPlayerCleaned = false;
    bool postCancelPlayerCleaned = false;
    bool preCancelPrepared = false;
    bool preCancelInterrupted = false;
    bool preCancelUnmutated = false;
    bool postCancelDelayed = false;
    bool postCancelInterrupted = false;
    bool postCancelMutation = false;
    bool postCancelNoImpact = false;
    uint32 delayedTargetHealthBefore = delayedTarget.creature
        ? delayedTarget.creature->GetHealth() : 0;
    uint32 delayedTargetHealthAfter = delayedTargetHealthBefore;
    int32 preCancelChargesBefore = 0;
    int32 preCancelChargesAfter = 0;
    int32 postCancelChargesBefore = 0;
    int32 postCancelChargesAfter = 0;
    bool delayedPreListenerAdded = false;
    bool delayedPostListenerAdded = false;
    uint8 preCancelTerminalCount = 0;
    uint8 postCancelTerminalCount = 0;
    uint8 postCancelMutationCount = 0;
    bool postCancelMutationPhaseBeforeEffects = false;
    bool postCancelMutationIdentityValid = false;

    if (delayedTargetCreated)
    {
        WorldSession preCancelSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
        Player* preCancelPlayer = nullptr;
        if (CreateSimulatorPlayer(map, delayedPartyFaction, preCancelSession,
                                  preCancelPlayer, 0.0f, 9.0f, 4.0f))
        {
            PartyBotAI preCancelAI(
                preCancelPlayer, map.GetId(), map.GetInstanceId(),
                preCancelPlayer->GetPositionX(), preCancelPlayer->GetPositionY(),
                preCancelPlayer->GetPositionZ(), preCancelPlayer->GetOrientation());
            preCancelAI.SetPlayer(preCancelPlayer);
            preCancelPlayer->SetAI(&preCancelAI);
            std::shared_ptr<SimulatorItemUseCapture> capture(
                new SimulatorItemUseCapture());
            delayedPreListenerAdded =
                preCancelAI.m_partyBotEngine.AddItemUseTerminalListener(capture);

            Item* item = preCancelPlayer->StoreNewItemInInventorySlot(
                DelayedItemId, 1);
            ObjectGuid const itemGuid = item ? item->GetObjectGuid() : ObjectGuid();
            preCancelChargesBefore = item && delayedSpellSlot < MAX_ITEM_PROTO_SPELLS
                ? item->GetSpellCharges(delayedSpellSlot) : 0;
            SpellCastTargets targets;
            targets.PrepareForSpellSystem(preCancelPlayer);
            if (delayedTarget.creature)
                targets.setUnitTarget(delayedTarget.creature);
            uint64 const itemUseId = item
                ? preCancelPlayer->CastItemUseSpell(item, targets) : 0;
            Spell* const preparingSpell =
                preCancelPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            preCancelPrepared = delayedPreListenerAdded && itemUseId &&
                itemGuid && item->GetCount() == 1 &&
                preCancelChargesBefore == -5 &&
                preCancelPlayer->CanUseItem(item) == EQUIP_ERR_OK &&
                delayedTarget.creature &&
                preCancelPlayer->CanAttack(delayedTarget.creature) &&
                preparingSpell && preparingSpell->m_spellInfo->Id == DelayedSpellId &&
                preparingSpell->getState() == SPELL_STATE_PREPARING;

            if (preCancelPrepared)
            {
                // The cast-time SpellEvent has not fired, so Core has not called
                // TakeCastItem. The regular non-delayed interrupt must preserve
                // the real item and its charge.
                preCancelPlayer->InterruptNonMeleeSpells(
                    false, DelayedSpellId);
                preCancelInterrupted =
                    preCancelPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                    capture->events.size() == 1 &&
                    capture->events[0].itemUseId == itemUseId &&
                    capture->events[0].status == ItemUseAggregateStatus::AllInterrupted &&
                    capture->events[0].childCount == 1 &&
                    capture->events[0].interruptedChildCount == 1 &&
                    capture->events[0].observedMutationCount == 0;
                Item* const itemAfter = preCancelPlayer->GetItemByGuid(itemGuid);
                preCancelChargesAfter = itemAfter && delayedSpellSlot < MAX_ITEM_PROTO_SPELLS
                    ? itemAfter->GetSpellCharges(delayedSpellSlot) : 0;
                preCancelUnmutated = itemAfter == item &&
                    itemAfter->GetCount() == 1 &&
                    preCancelChargesAfter == preCancelChargesBefore &&
                    preCancelAI.m_partyBotEngine.GetPendingItemUseCount() == 0;
                preCancelTerminalCount =
                    capture->events.empty() ? 0 : capture->events[0].completedChildCount;
            }
            preCancelPlayer->SetAI(nullptr);
            preCancelSession.LogoutPlayer(false);
            preCancelPlayerCleaned = preCancelSession.GetPlayer() == nullptr;
        }
        else
        {
            if (preCancelSession.GetPlayer())
                preCancelSession.LogoutPlayer(false);
            else if (preCancelPlayer)
                delete preCancelPlayer;
        }

        // The second Player has a clean cooldown/GCD and a fresh real item.
        WorldSession postCancelSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
        Player* postCancelPlayer = nullptr;
        if (CreateSimulatorPlayer(map, delayedPartyFaction, postCancelSession,
                                  postCancelPlayer, 0.0f, 9.0f, 4.0f))
        {
            PartyBotAI postCancelAI(
                postCancelPlayer, map.GetId(), map.GetInstanceId(),
                postCancelPlayer->GetPositionX(), postCancelPlayer->GetPositionY(),
                postCancelPlayer->GetPositionZ(), postCancelPlayer->GetOrientation());
            postCancelAI.SetPlayer(postCancelPlayer);
            postCancelPlayer->SetAI(&postCancelAI);
            std::shared_ptr<SimulatorItemUseCapture> capture(
                new SimulatorItemUseCapture());
            delayedPostListenerAdded =
                postCancelAI.m_partyBotEngine.AddItemUseTerminalListener(capture);

            Item* item = postCancelPlayer->StoreNewItemInInventorySlot(
                DelayedItemId, 1);
            ObjectGuid const itemGuid = item ? item->GetObjectGuid() : ObjectGuid();
            postCancelChargesBefore = item && delayedSpellSlot < MAX_ITEM_PROTO_SPELLS
                ? item->GetSpellCharges(delayedSpellSlot) : 0;
            SpellCastTargets targets;
            targets.PrepareForSpellSystem(postCancelPlayer);
            if (delayedTarget.creature)
                targets.setUnitTarget(delayedTarget.creature);
            uint64 const itemUseId = item
                ? postCancelPlayer->CastItemUseSpell(item, targets) : 0;
            Spell* const initialSpell =
                postCancelPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            bool const initialPreparing = delayedPostListenerAdded && itemUseId &&
                itemGuid && item->GetCount() == 1 &&
                postCancelChargesBefore == -5 &&
                postCancelPlayer->CanUseItem(item) == EQUIP_ERR_OK &&
                delayedTarget.creature &&
                postCancelPlayer->CanAttack(delayedTarget.creature) &&
                initialSpell && initialSpell->m_spellInfo->Id == DelayedSpellId &&
                initialSpell->getState() == SPELL_STATE_PREPARING;

            ItemUseMutationEvent mutation;
            bool haveLaunchMutation = false;
            // Advance the real EventProcessor in small bounded increments.
            // Stop on the exact update where Core has launched the projectile
            // and TakeCastItem has emitted its BeforeEffects mutation.
            for (uint32 tick = 0; initialPreparing && tick < 350; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                map.Update(10);
                while (postCancelAI.m_partyBotEngine.PopItemUseMutationEvent(mutation))
                {
                    ++postCancelMutationCount;
                    haveLaunchMutation = haveLaunchMutation ||
                        (mutation.itemUseId == itemUseId && mutation.childId != 0);
                }
                Spell* const current =
                    postCancelPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (current && current->m_spellInfo->Id == DelayedSpellId &&
                    current->getState() == SPELL_STATE_DELAYED &&
                    haveLaunchMutation)
                {
                    postCancelDelayed = true;
                    break;
                }
                if (!capture->events.empty())
                    break;
            }

            if (postCancelDelayed && postCancelMutationCount == 1)
            {
                Item* const itemAtLaunch = postCancelPlayer->GetItemByGuid(itemGuid);
                postCancelChargesAfter = itemAtLaunch &&
                    delayedSpellSlot < MAX_ITEM_PROTO_SPELLS
                    ? itemAtLaunch->GetSpellCharges(delayedSpellSlot) : 0;
                // Core deliberately ignores delayed generic spells unless
                // withDelayed=true. Cancel on the same map-thread turn as the
                // charge mutation, before the projectile's future impact event.
                postCancelPlayer->InterruptNonMeleeSpells(
                    true, DelayedSpellId);
                postCancelInterrupted =
                    postCancelPlayer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                    capture->events.size() == 1 &&
                    capture->events[0].itemUseId == itemUseId &&
                    capture->events[0].status == ItemUseAggregateStatus::AllInterrupted &&
                    capture->events[0].childCount == 1 &&
                    capture->events[0].interruptedChildCount == 1 &&
                    capture->events[0].observedMutationCount == 1;
                postCancelMutation =
                    mutation.kind == ItemUseMutationKind::ChargeChanged &&
                    mutation.phase == ItemUseMutationPhase::BeforeEffects &&
                    mutation.slotIndex == delayedSpellSlot &&
                    mutation.itemGuid == itemGuid.GetRawValue() &&
                    mutation.itemEntry == DelayedItemId &&
                    mutation.beforeCharges == -5 && mutation.afterCharges == -4 &&
                    mutation.itemExistsAfter &&
                    postCancelChargesAfter == -4;
                postCancelMutationPhaseBeforeEffects =
                    mutation.phase == ItemUseMutationPhase::BeforeEffects;
                postCancelMutationIdentityValid =
                    capture->events.size() == 1 &&
                    mutation.itemUseId == itemUseId &&
                    mutation.childId == capture->events[0].children[0].childId;
                postCancelTerminalCount = capture->events.empty()
                    ? 0 : capture->events[0].completedChildCount;
                // Let the already-scheduled projectile event mature. A
                // cancelled Spell must be deleted without applying Fireball.
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                map.Update(1000);
                delayedTargetHealthAfter = delayedTarget.creature->GetHealth();
                Item* const itemAfter = postCancelPlayer->GetItemByGuid(itemGuid);
                postCancelNoImpact = delayedTargetHealthAfter ==
                    delayedTargetHealthBefore &&
                    itemAfter == item && itemAfter->GetCount() == 1 &&
                    itemAfter->GetSpellCharges(delayedSpellSlot) == -4 &&
                    postCancelAI.m_partyBotEngine.GetPendingItemUseCount() == 0;
            }

            postCancelPlayer->SetAI(nullptr);
            postCancelSession.LogoutPlayer(false);
            postCancelPlayerCleaned = postCancelSession.GetPlayer() == nullptr;
        }
        else
        {
            if (postCancelSession.GetPlayer())
                postCancelSession.LogoutPlayer(false);
            else if (postCancelPlayer)
                delete postCancelPlayer;
        }
    }

    bool const delayedTargetCleaned = DestroySimulatorActor(map, delayedTarget);
    bool const delayedCancellationFixturePassed = delayedDataValid &&
        delayedTargetCreated && delayedPreListenerAdded && delayedPostListenerAdded &&
        preCancelPrepared && preCancelInterrupted && preCancelUnmutated &&
        postCancelDelayed && postCancelInterrupted && postCancelMutation &&
        postCancelMutationPhaseBeforeEffects && postCancelMutationIdentityValid &&
        postCancelNoImpact && preCancelPlayerCleaned && postCancelPlayerCleaned &&
        delayedTargetCleaned;
    sLog.Out(LOG_BASIC, delayedCancellationFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F42 delayed ON_USE cancellation: data=%u target=%u prePrepared=%u preInterrupted=%u preUnmutated=%u preCharge=%d>%d preTerminals=%u postDelayed=%u postInterrupted=%u postMutation=%u mutationBeforeEffects=%u mutationIdentity=%u mutationCount=%u postCharge=%d>%d postNoImpact=%u postTerminals=%u cleanup=%u/%u/%u.",
        delayedDataValid ? 1u : 0u, delayedTargetCreated ? 1u : 0u,
        preCancelPrepared ? 1u : 0u, preCancelInterrupted ? 1u : 0u,
        preCancelUnmutated ? 1u : 0u, preCancelChargesBefore,
        preCancelChargesAfter, preCancelTerminalCount,
        postCancelDelayed ? 1u : 0u, postCancelInterrupted ? 1u : 0u,
        postCancelMutation ? 1u : 0u,
        postCancelMutationPhaseBeforeEffects ? 1u : 0u,
        postCancelMutationIdentityValid ? 1u : 0u, postCancelMutationCount,
        postCancelChargesBefore, postCancelChargesAfter,
        postCancelNoImpact ? 1u : 0u, postCancelTerminalCount,
        preCancelPlayerCleaned ? 1u : 0u,
        postCancelPlayerCleaned ? 1u : 0u,
        delayedTargetCleaned ? 1u : 0u);

    bool const passed = successfulUse && rejectedWithoutConsumption &&
        successfulAggregate && rejectedAggregate &&
        multiSlotFixturePassed && multiPlayerCleaned &&
        successPlayerCleaned && rejectedPlayerCleaned &&
        delayedCancellationFixturePassed;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] F42 Core item-use aggregate: prototype=%u spellSlot=%u spellId=%u healed=%u>%u consumed=%u successRoot=%u successChildren=%u successMutations=%u rejectedRoot=%u rejectedChildren=%u rejectedMutations=%u multiSlot=%u multiRoot=%u multiChildren=%u firstTriggered=%u secondTriggered=%u multiMutations=%u multiChargesConsumed=%u cleanup=%u/%u/%u.",
             prototypeValid ? 1u : 0u, spellSlot,
             potionPrototype->Spells[spellSlot].SpellId,
             potionHealthBefore, potionHealthAfter, potionConsumed ? 1u : 0u,
             successfulAggregate ? 1u : 0u,
             successfulAggregate ? successItemUseCapture->events[0].childCount : 0u,
             successMutationCount,
             rejectedAggregate ? 1u : 0u,
             rejectedAggregate ? rejectedItemUseCapture->events[0].childCount : 0u,
             rejectedAggregate ? rejectedItemUseCapture->events[0].observedMutationCount : 0u,
             multiSlotFixturePassed ? 1u : 0u, multiRootId ? 1u : 0u,
             multiChildCount, multiFirstTriggered ? 1u : 0u,
             multiSecondTriggered ? 1u : 0u, multiMutationCount,
             multiChargesConsumed ? 1u : 0u,
             successPlayerCleaned ? 1u : 0u, rejectedPlayerCleaned ? 1u : 0u,
             multiPlayerCleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: F42 real Core aggregated single-slot use/rejection, a synthetic two-ON_USE-slot prototype, and a real delayed Brimstone/Fireball ON_USE cancellation before and after its Core charge mutation.");
    return passed;
}




bool RunEngineCoreCountBasedItemMutationSmokeTest(Map& map)
{
    static uint32 const PotionItemId = 118;
    ItemPrototype const* potionPrototype = sObjectMgr.GetItemPrototype(PotionItemId);
    uint8 spellSlot = 0xff;
    if (potionPrototype)
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
            if (potionPrototype->Spells[slot].SpellId &&
                potionPrototype->Spells[slot].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                spellSlot = slot;
                break;
            }

    SpellEntry const* constSpell = potionPrototype && spellSlot < MAX_ITEM_PROTO_SPELLS
        ? sSpellMgr.GetSpellEntry(potionPrototype->Spells[spellSlot].SpellId) : nullptr;
    bool const prototypeValid = potionPrototype &&
        potionPrototype->Stackable >= 2 &&
        spellSlot < MAX_ITEM_PROTO_SPELLS &&
        potionPrototype->Spells[spellSlot].SpellCharges < 0 &&
        constSpell && constSpell->Id == 439;
    if (!prototypeValid)
        return false;

    class SimulatorCountMutationCapture final : public ItemUseTerminalListener
    {
    public:
        void OnItemUseTerminal(ItemUseTerminalEvent const& event) override
        {
            events.push_back(event);
        }
        std::vector<ItemUseTerminalEvent> events;
    };

    bool stackUsePassed = false;
    bool stackPlayerCleaned = false;
    uint32 stackCountBefore = 0;
    uint32 stackCountAfter = 0;
    uint32 stackHealthBefore = 0;
    uint32 stackHealthAfter = 0;
    ItemUseId stackItemUseId = 0;
    ItemUseMutationEvent stackMutation;
    uint8 stackMutationCount = 0;

    {
        WorldSession session(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
        Player* player = nullptr;
        if (CreateSimulatorPlayer(map, 1, session, player, 0.0f, 9.0f, 4.0f))
        {
            PartyBotAI observer(player, map.GetId(), map.GetInstanceId(),
                                player->GetPositionX(), player->GetPositionY(),
                                player->GetPositionZ(), player->GetOrientation());
            observer.SetPlayer(player);
            player->SetAI(&observer);
            std::shared_ptr<SimulatorCountMutationCapture> capture(
                new SimulatorCountMutationCapture());
            bool const listenerAdded =
                observer.m_partyBotEngine.AddItemUseTerminalListener(capture);

            Item* potion = player->StoreNewItemInInventorySlot(PotionItemId, 2);
            ObjectGuid const potionGuid = potion ? potion->GetObjectGuid() : ObjectGuid();
            stackCountBefore = player->GetItemCount(PotionItemId);
            stackHealthBefore = player->GetMaxHealth() / 2;
            player->SetHealth(stackHealthBefore);
            SpellCastTargets targets;
            targets.PrepareForSpellSystem(player);
            stackItemUseId = potion ? player->CastItemUseSpell(potion, targets) : 0;

            for (uint32 tick = 0; tick < 8 &&
                 (observer.m_partyBotEngine.GetPendingItemUseCount() != 0 ||
                  player->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                map.Update(250);
            }

            stackCountAfter = player->GetItemCount(PotionItemId);
            stackHealthAfter = player->GetHealth();
            Item* const remainingPotion = potionGuid
                ? player->GetItemByGuid(potionGuid) : nullptr;
            std::vector<ItemUseMutationEvent> mutations;
            ItemUseMutationEvent mutation;
            while (observer.m_partyBotEngine.PopItemUseMutationEvent(mutation))
                mutations.push_back(mutation);
            stackMutationCount = static_cast<uint8>(mutations.size());
            if (!mutations.empty())
                stackMutation = mutations[0];

            bool const terminalValid = listenerAdded && stackItemUseId &&
                capture->events.size() == 1 &&
                capture->events[0].itemUseId == stackItemUseId &&
                capture->events[0].status == ItemUseAggregateStatus::AllSucceeded &&
                capture->events[0].childCount == 1 &&
                capture->events[0].completedChildCount == 1 &&
                capture->events[0].observedMutationCount == 1 &&
                capture->events[0].children[0].spellId == constSpell->Id &&
                capture->events[0].children[0].status == ItemUseChildStatus::Succeeded;
            bool const mutationValid = mutations.size() == 1 &&
                capture->events.size() == 1 &&
                stackMutation.itemUseId == stackItemUseId &&
                stackMutation.childId == capture->events[0].children[0].childId &&
                stackMutation.itemGuid == potionGuid.GetRawValue() &&
                stackMutation.itemEntry == PotionItemId &&
                stackMutation.kind == ItemUseMutationKind::CountChanged &&
                // Spell batching marks this Player cast as delayed, so Core
                // consumes the stack before the batched heal effect resolves.
                stackMutation.phase == ItemUseMutationPhase::BeforeEffects &&
                stackMutation.slotIndex == 0xff &&
                stackMutation.beforeCount == 2 && stackMutation.afterCount == 1 &&
                stackMutation.beforeCharges == 0 && stackMutation.afterCharges == 0 &&
                stackMutation.itemExistsAfter;
            stackUsePassed = terminalValid && mutationValid &&
                stackCountBefore == 2 && stackCountAfter == 1 &&
                remainingPotion && remainingPotion == potion &&
                remainingPotion->GetCount() == 1 &&
                stackHealthAfter > stackHealthBefore &&
                player->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                observer.m_partyBotEngine.GetPendingItemUseCount() == 0;

            player->InterruptNonMeleeSpells(false);
            player->SetAI(nullptr);
            session.LogoutPlayer(false);
        }
        else if (session.GetPlayer())
            session.LogoutPlayer(false);
        else if (player)
            delete player;
        stackPlayerCleaned = session.GetPlayer() == nullptr;
    }

    struct SpellReagentRestore
    {
        SpellEntry* spell = nullptr;
        int32 reagent[MAX_SPELL_REAGENTS] = {};
        uint32 reagentCount[MAX_SPELL_REAGENTS] = {};
        explicit SpellReagentRestore(SpellEntry* value) : spell(value)
        {
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                reagent[i] = spell->Reagent[i];
                reagentCount[i] = spell->ReagentCount[i];
                spell->Reagent[i] = 0;
                spell->ReagentCount[i] = 0;
            }
        }
        ~SpellReagentRestore()
        {
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                spell->Reagent[i] = reagent[i];
                spell->ReagentCount[i] = reagentCount[i];
            }
        }
    };

    SpellEntry* mutableSpell = const_cast<SpellEntry*>(constSpell);
    bool reagentUsePassed = false;
    bool reagentPlayerCleaned = false;
    bool reagentConfigured = false;
    uint32 reagentCountBefore = 0;
    uint32 reagentCountAfter = 0;
    uint32 reagentHealthBefore = 0;
    uint32 reagentHealthAfter = 0;
    ItemUseId reagentItemUseId = 0;
    ItemUseMutationEvent reagentMutation;
    uint8 reagentMutationCount = 0;

    {
        SpellReagentRestore restore(mutableSpell);
        mutableSpell->Reagent[0] = static_cast<int32>(PotionItemId);
        mutableSpell->ReagentCount[0] = 2;
        reagentConfigured = mutableSpell->Reagent[0] == static_cast<int32>(PotionItemId) &&
            mutableSpell->ReagentCount[0] == 2;

        WorldSession session(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
        Player* player = nullptr;
        if (CreateSimulatorPlayer(map, 1, session, player, 0.0f, 9.0f, 4.0f))
        {
            PartyBotAI observer(player, map.GetId(), map.GetInstanceId(),
                                player->GetPositionX(), player->GetPositionY(),
                                player->GetPositionZ(), player->GetOrientation());
            observer.SetPlayer(player);
            player->SetAI(&observer);
            std::shared_ptr<SimulatorCountMutationCapture> capture(
                new SimulatorCountMutationCapture());
            bool const listenerAdded =
                observer.m_partyBotEngine.AddItemUseTerminalListener(capture);

            // The item is both the cast item and the spell's reagent. The
            // Core CheckItems/TakeReagents special case adds the cast item to
            // the required reagent count, so two reagents require a stack of 3.
            Item* potion = player->StoreNewItemInInventorySlot(PotionItemId, 3);
            ObjectGuid const potionGuid = potion ? potion->GetObjectGuid() : ObjectGuid();
            reagentCountBefore = player->GetItemCount(PotionItemId);
            reagentHealthBefore = player->GetMaxHealth() / 2;
            player->SetHealth(reagentHealthBefore);
            SpellCastTargets targets;
            targets.PrepareForSpellSystem(player);
            reagentItemUseId = potion ? player->CastItemUseSpell(potion, targets) : 0;

            for (uint32 tick = 0; tick < 8 &&
                 (observer.m_partyBotEngine.GetPendingItemUseCount() != 0 ||
                  player->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                map.Update(250);
            }

            reagentCountAfter = player->GetItemCount(PotionItemId);
            reagentHealthAfter = player->GetHealth();
            bool const originalStackGone = potionGuid &&
                player->GetItemByGuid(potionGuid) == nullptr;
            std::vector<ItemUseMutationEvent> mutations;
            ItemUseMutationEvent mutation;
            while (observer.m_partyBotEngine.PopItemUseMutationEvent(mutation))
                mutations.push_back(mutation);
            reagentMutationCount = static_cast<uint8>(mutations.size());
            if (!mutations.empty())
                reagentMutation = mutations[0];

            bool const terminalValid = listenerAdded && reagentItemUseId &&
                capture->events.size() == 1 &&
                capture->events[0].itemUseId == reagentItemUseId &&
                capture->events[0].status == ItemUseAggregateStatus::AllSucceeded &&
                capture->events[0].childCount == 1 &&
                capture->events[0].completedChildCount == 1 &&
                capture->events[0].observedMutationCount == 1 &&
                capture->events[0].children[0].spellId == constSpell->Id &&
                capture->events[0].children[0].status == ItemUseChildStatus::Succeeded;
            bool const mutationValid = mutations.size() == 1 &&
                capture->events.size() == 1 &&
                reagentMutation.itemUseId == reagentItemUseId &&
                reagentMutation.childId == capture->events[0].children[0].childId &&
                reagentMutation.itemGuid == 0 &&
                reagentMutation.itemEntry == PotionItemId &&
                reagentMutation.kind == ItemUseMutationKind::ReagentConsumed &&
                reagentMutation.phase == ItemUseMutationPhase::BeforeEffects &&
                reagentMutation.slotIndex == 0xff &&
                reagentMutation.beforeCount == 3 && reagentMutation.afterCount == 0 &&
                reagentMutation.beforeCharges == 0 && reagentMutation.afterCharges == 0 &&
                !reagentMutation.itemExistsAfter;
            reagentUsePassed = terminalValid && mutationValid &&
                reagentCountBefore == 3 && reagentCountAfter == 0 &&
                originalStackGone && reagentHealthAfter > reagentHealthBefore &&
                player->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                observer.m_partyBotEngine.GetPendingItemUseCount() == 0;

            player->InterruptNonMeleeSpells(false);
            player->SetAI(nullptr);
            session.LogoutPlayer(false);
        }
        else if (session.GetPlayer())
            session.LogoutPlayer(false);
        else if (player)
            delete player;
        reagentPlayerCleaned = session.GetPlayer() == nullptr;
    }

    bool const passed = prototypeValid && stackUsePassed && stackPlayerCleaned &&
        reagentConfigured && reagentUsePassed && reagentPlayerCleaned;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F45 CountBasedItemMutation: prototype=%u stackUse=%u stackCount=%u>%u stackHeal=%u>%u stackMutation=%u stackKind=%u stackPhase=%u stackCleanup=%u reagentConfigured=%u reagentUse=%u reagentCount=%u>%u reagentHeal=%u>%u reagentMutation=%u reagentKind=%u reagentPhase=%u reagentGuidZero=%u reagentCleanup=%u.",
        prototypeValid ? 1u : 0u, stackUsePassed ? 1u : 0u,
        stackCountBefore, stackCountAfter, stackHealthBefore, stackHealthAfter,
        stackMutationCount, static_cast<uint32>(stackMutation.kind),
        static_cast<uint32>(stackMutation.phase), stackPlayerCleaned ? 1u : 0u,
        reagentConfigured ? 1u : 0u, reagentUsePassed ? 1u : 0u,
        reagentCountBefore, reagentCountAfter, reagentHealthBefore, reagentHealthAfter,
        reagentMutationCount, static_cast<uint32>(reagentMutation.kind),
        static_cast<uint32>(reagentMutation.phase),
        reagentMutation.itemGuid == 0 ? 1u : 0u,
        reagentPlayerCleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: real Core stackable item use reduced a two-potion stack to one with a CountChanged before-effects record, and using the cast item as its own reagent consumed three items once with a GUID-less ReagentConsumed before-effects record.");
    return passed;
}


namespace
{
bool IsF46UtilitySelfAura(uint32 auraType)
{
    switch (auraType)
    {
        case SPELL_AURA_MOD_STAT:
        case SPELL_AURA_MOD_RESISTANCE:
        case SPELL_AURA_MOD_INCREASE_SPEED:
        case SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED:
        case SPELL_AURA_WATER_BREATHING:
            return true;
        default:
            return false;
    }
}

// This is deliberately stricter than "a friendly buff". Ordinary non-triggered
// helpful spells still raise Core helpful-ability proc flags. A utility spell
// may be considered for a NoThreat action only when both caster and target
// procs are suppressed, and when it has no ScriptMgr, flat-threat, spell-proc,
// triggered-child, periodic, healing, damage or area-aura path.
bool IsF46BoundedSelfUtilitySpell(SpellEntry const& spell)
{
    if (!spell.Id || !spell.IsPositiveSpell() || spell.IsPassiveSpell() ||
        spell.DmgClass != SPELL_DAMAGE_CLASS_NONE || spell.speed > 0.0f ||
        spell.MaxAffectedTargets || spell.IsAreaOfEffectSpell() ||
        spell.HasAreaAuraEffect() || spell.ScriptId ||
        spell.procFlags || spell.procChance || spell.procCharges ||
        !spell.HasAttribute(SPELL_ATTR_EX3_SUPPRESS_CASTER_PROCS) ||
        !spell.HasAttribute(SPELL_ATTR_EX3_SUPPRESS_TARGET_PROCS) ||
        sSpellMgr.GetSpellThreatEntry(spell.Id) ||
        sSpellMgr.GetSpellProcEvent(spell.Id))
        return false;

    SpellScriptTargetBounds const scriptTargets =
        sSpellMgr.GetSpellScriptTargetBounds(spell.Id);
    if (scriptTargets.first != scriptTargets.second)
        return false;

    for (uint32 slot = 0; slot < MAX_SPELL_TOTEMS; ++slot)
        if (spell.Totem[slot])
            return false;
    for (uint32 slot = 0; slot < MAX_SPELL_REAGENTS; ++slot)
        if (spell.Reagent[slot] || spell.ReagentCount[slot])
            return false;

    bool hasEffect = false;
    for (uint32 index = 0; index < MAX_EFFECT_INDEX; ++index)
    {
        uint32 const effect = spell.Effect[index];
        if (!effect)
        {
            if (spell.EffectTriggerSpell[index])
                return false;
            continue;
        }

        hasEffect = true;
        if (effect != SPELL_EFFECT_APPLY_AURA ||
            !IsF46UtilitySelfAura(spell.EffectApplyAuraName[index]) ||
            spell.EffectImplicitTargetA[index] != TARGET_UNIT_CASTER ||
            spell.EffectImplicitTargetB[index] != 0 ||
            spell.EffectTriggerSpell[index] ||
            spell.EffectChainTarget[index] ||
            spell.EffectRadiusIndex[index])
            return false;
    }
    return hasEffect;
}

bool IsF46PositiveSelfAuraShape(SpellEntry const& spell)
{
    if (!spell.Id || !spell.IsPositiveSpell() || spell.IsPassiveSpell() ||
        spell.DmgClass != SPELL_DAMAGE_CLASS_NONE || spell.speed > 0.0f ||
        spell.MaxAffectedTargets || spell.IsAreaOfEffectSpell() ||
        spell.HasAreaAuraEffect() || spell.ScriptId)
        return false;

    bool hasEffect = false;
    for (uint32 index = 0; index < MAX_EFFECT_INDEX; ++index)
    {
        if (!spell.Effect[index])
            continue;
        hasEffect = true;
        if (spell.Effect[index] != SPELL_EFFECT_APPLY_AURA ||
            !IsF46UtilitySelfAura(spell.EffectApplyAuraName[index]) ||
            spell.EffectImplicitTargetA[index] != TARGET_UNIT_CASTER ||
            spell.EffectImplicitTargetB[index] != 0 ||
            spell.EffectTriggerSpell[index] ||
            spell.EffectChainTarget[index] ||
            spell.EffectRadiusIndex[index])
            return false;
    }
    return hasEffect;
}

bool RunEngineCoreUtilitySpellAuditSmokeTest(Map& /*map*/)
{
    uint32 inspected = 0;
    uint32 positiveSelfAuraShapes = 0;
    uint32 missingProcSuppression = 0;
    uint32 casterProcSuppressed = 0;
    uint32 targetProcSuppressed = 0;
    uint32 spellThreatRows = 0;
    uint32 spellProcEventRows = 0;
    uint32 procFieldRows = 0;
    uint32 threatOrProcRows = 0;
    uint32 strictCandidates = 0;
    uint32 loggedCandidates = 0;
    uint32 loggedNearMisses = 0;

    for (uint32 spellId = 1; spellId < sSpellMgr.GetMaxSpellId(); ++spellId)
    {
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
        if (!spell || spell->Id != spellId)
            continue;
        ++inspected;

        if (!IsF46PositiveSelfAuraShape(*spell))
            continue;
        ++positiveSelfAuraShapes;

        bool const casterSuppressed =
            spell->HasAttribute(SPELL_ATTR_EX3_SUPPRESS_CASTER_PROCS);
        bool const targetSuppressed =
            spell->HasAttribute(SPELL_ATTR_EX3_SUPPRESS_TARGET_PROCS);
        if (casterSuppressed)
            ++casterProcSuppressed;
        if (targetSuppressed)
            ++targetProcSuppressed;
        if (!casterSuppressed || !targetSuppressed)
            ++missingProcSuppression;

        bool const hasSpellThreat = sSpellMgr.GetSpellThreatEntry(spellId) != nullptr;
        bool const hasSpellProcEvent = sSpellMgr.GetSpellProcEvent(spellId) != nullptr;
        bool const hasProcFields = spell->procFlags || spell->procChance || spell->procCharges;
        if (hasSpellThreat)
            ++spellThreatRows;
        if (hasSpellProcEvent)
            ++spellProcEventRows;
        if (hasProcFields)
            ++procFieldRows;
        if (hasSpellThreat || hasSpellProcEvent || hasProcFields)
            ++threatOrProcRows;

        // Keep a small, explicit sample of the actual blockers. In particular,
        // do not silently relax the proc-suppression requirement just because
        // the strict scan finds no candidate.
        if (loggedNearMisses < 8 &&
            (!casterSuppressed || !targetSuppressed || hasSpellThreat ||
             hasSpellProcEvent || hasProcFields))
        {
            uint32 auraType = 0;
            for (uint32 index = 0; index < MAX_EFFECT_INDEX; ++index)
                if (spell->Effect[index])
                {
                    auraType = spell->EffectApplyAuraName[index];
                    break;
                }
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[PartyBotSimulator] F46 self-utility near-miss: spell=%u name=%s aura=%u casterSuppressed=%u targetSuppressed=%u spellThreat=%u spellProcEvent=%u procFlags=%u procChance=%u procCharges=%u.",
                spellId, spell->SpellName[0].c_str(), auraType,
                casterSuppressed ? 1u : 0u, targetSuppressed ? 1u : 0u,
                hasSpellThreat ? 1u : 0u, hasSpellProcEvent ? 1u : 0u,
                spell->procFlags, spell->procChance, spell->procCharges);
            ++loggedNearMisses;
        }

        if (!IsF46BoundedSelfUtilitySpell(*spell))
            continue;

        ++strictCandidates;
        if (loggedCandidates < 20)
        {
            uint32 auraType = 0;
            for (uint32 index = 0; index < MAX_EFFECT_INDEX; ++index)
                if (spell->Effect[index])
                {
                    auraType = spell->EffectApplyAuraName[index];
                    break;
                }
            SpellCastTimesEntry const* castTime =
                sSpellCastTimesStore.LookupEntry(spell->CastingTimeIndex);
            SpellRangeEntry const* range =
                sSpellRangeStore.LookupEntry(spell->rangeIndex);
            uint32 const castTimeMs = castTime ? castTime->CastTime : 0;
            float const maxRange = range ? Spells::GetSpellMaxRange(range) : 0.0f;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[PartyBotSimulator] F46 bounded self-utility candidate: spell=%u name=%s family=%u aura=%u mana=%u powerType=%u castMs=%u rangeIndex=%u maxRange=%.2f recoveryMs=%u categoryRecoveryMs=%u gcdMs=%u script=%u threat=0 procEvent=0 procFields=0 procsSuppressed=1.",
                spellId, spell->SpellName[0].c_str(), spell->SpellFamilyName,
                auraType, spell->manaCost,
                spell->powerType, castTimeMs, spell->rangeIndex, maxRange,
                spell->RecoveryTime, spell->CategoryRecoveryTime,
                spell->StartRecoveryTime, spell->ScriptId);
            ++loggedCandidates;
        }
    }

    bool const auditRan = inspected > 1000 &&
        sSpellMgr.GetMaxSpellId() > 1000;
    sLog.Out(LOG_BASIC, auditRan ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F46 utility spell audit: spellRows=%u positiveSelfAuraShapes=%u casterProcSuppressed=%u targetProcSuppressed=%u missingProcSuppression=%u spellThreatRows=%u spellProcEventRows=%u procFieldRows=%u threatOrProcRows=%u strictCandidates=%u loggedCandidates=%u loggedNearMisses=%u.",
        inspected, positiveSelfAuraShapes, casterProcSuppressed,
        targetProcSuppressed, missingProcSuppression, spellThreatRows,
        spellProcEventRows, procFieldRows, threatOrProcRows,
        strictCandidates, loggedCandidates, loggedNearMisses);

    if (auditRan)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F46 bounded self-utility spell audit scanned the loaded Vanilla 5875 SpellEntry table; only self-aura spells with both caster/target proc suppression and no ScriptMgr, spell-threat, spell-proc, trigger-child, periodic or area path were listed as eligible candidates.");
    return auditRan;
}


// Positive item-backed spells take a distinct Core proc path: Spell::prepareDataForTriggerSystem
// disables proc triggering when m_CastItem is present and the spell is positive.
// It has a small set of family-specific exceptions, so this audit excludes those
// families rather than assuming every positive item spell is proc-free.
bool IsF47BoundedItemUtilitySpell(SpellEntry const& spell)
{
    if (!IsF46PositiveSelfAuraShape(spell) ||
        spell.procFlags || spell.procCharges ||
        sSpellMgr.GetSpellThreatEntry(spell.Id) ||
        sSpellMgr.GetSpellProcEvent(spell.Id))
        return false;

    switch (spell.SpellFamilyName)
    {
        case SPELLFAMILY_MAGE:
        case SPELLFAMILY_WARLOCK:
        case SPELLFAMILY_HUNTER:
        case SPELLFAMILY_PALADIN:
        case SPELLFAMILY_PRIEST:
            return false;
        default:
            break;
    }

    SpellScriptTargetBounds const scriptTargets =
        sSpellMgr.GetSpellScriptTargetBounds(spell.Id);
    if (scriptTargets.first != scriptTargets.second)
        return false;

    for (uint32 slot = 0; slot < MAX_SPELL_TOTEMS; ++slot)
        if (spell.Totem[slot])
            return false;
    for (uint32 slot = 0; slot < MAX_SPELL_REAGENTS; ++slot)
        if (spell.Reagent[slot] || spell.ReagentCount[slot])
            return false;

    return true;
}

bool RunEngineCoreItemUtilityAuditSmokeTest()
{
    uint32 itemPrototypeRows = 0;
    uint32 itemsWithOnUse = 0;
    uint32 eligibleItems = 0;
    uint32 inspectedOnUseSlots = 0;
    uint32 eligibleOnUseSlots = 0;
    uint32 loggedCandidates = 0;

    for (auto const& itemRow : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& item = itemRow.second;
        ++itemPrototypeRows;

        uint32 onUseSlots = 0;
        uint32 safeOnUseSlots = 0;
        bool allOnUseSlotsSafe = true;

        for (uint32 slotIndex = 0; slotIndex < MAX_ITEM_PROTO_SPELLS; ++slotIndex)
        {
            _ItemSpell const& itemSpell = item.Spells[slotIndex];
            if (!itemSpell.SpellId ||
                itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;

            ++onUseSlots;
            ++inspectedOnUseSlots;

            SpellEntry const* spell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
            if (!spell || !IsF47BoundedItemUtilitySpell(*spell))
            {
                allOnUseSlotsSafe = false;
                continue;
            }

            ++safeOnUseSlots;
        }

        if (!onUseSlots)
            continue;

        ++itemsWithOnUse;
        if (!allOnUseSlotsSafe || safeOnUseSlots != onUseSlots)
            continue;

        ++eligibleItems;
        eligibleOnUseSlots += safeOnUseSlots;

        if (loggedCandidates < 64)
        {
            for (uint32 slotIndex = 0;
                 slotIndex < MAX_ITEM_PROTO_SPELLS && loggedCandidates < 64;
                 ++slotIndex)
            {
                _ItemSpell const& itemSpell = item.Spells[slotIndex];
                if (!itemSpell.SpellId ||
                    itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;

                SpellEntry const* spell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
                if (!spell || !IsF47BoundedItemUtilitySpell(*spell))
                    continue;

                uint32 auraType = 0;
                for (uint32 effectIndex = 0; effectIndex < MAX_EFFECT_INDEX; ++effectIndex)
                    if (spell->Effect[effectIndex])
                    {
                        auraType = spell->EffectApplyAuraName[effectIndex];
                        break;
                    }

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[PartyBotSimulator] F47 bounded item-utility candidate: item=%u name=%s class=%u subclass=%u stackable=%u slot=%u spell=%u spellName=%s family=%u aura=%u charges=%d itemCategory=%u spellCategory=%u itemCooldown=%d categoryCooldown=%d spellRecovery=%u gcdMs=%u procFlags=%u procChance=%u procCharges=%u.",
                    item.ItemId, item.Name1 ? item.Name1 : "",
                    item.Class, item.SubClass, item.Stackable, slotIndex,
                    spell->Id, spell->SpellName[0].c_str(), spell->SpellFamilyName,
                    auraType, itemSpell.SpellCharges, itemSpell.SpellCategory,
                    spell->Category, itemSpell.SpellCooldown,
                    itemSpell.SpellCategoryCooldown, spell->RecoveryTime,
                    spell->StartRecoveryTime, spell->procFlags,
                    spell->procChance, spell->procCharges);
                ++loggedCandidates;
            }
        }
    }

    bool const auditRan = itemPrototypeRows > 1000 && itemsWithOnUse > 0;
    sLog.Out(LOG_BASIC, auditRan ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F47 item utility audit: itemPrototypeRows=%u itemsWithOnUse=%u inspectedOnUseSlots=%u eligibleItems=%u eligibleOnUseSlots=%u loggedCandidates=%u.",
        itemPrototypeRows, itemsWithOnUse, inspectedOnUseSlots,
        eligibleItems, eligibleOnUseSlots, loggedCandidates);

    if (auditRan)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F47 audited loaded Vanilla item prototypes and every ON_USE spell slot; only items whose complete ON_USE fan-out consists of positive bounded self-aura spells outside Core's item-proc exception families were listed as candidates.");
    return auditRan;
}

bool RunEngineCoreItemUtilityUseSmokeTest(Map& map)
{
    static uint32 const ItemId = 2457; // Elixir of Minor Agility
    static uint32 const SpellId = 2374; // Lesser Agility

    ItemPrototype const* prototype = sObjectMgr.GetItemPrototype(ItemId);
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(SpellId);
    uint8 spellSlot = 0xff;
    uint32 useSlots = 0;
    bool everyUseSlotSafe = true;
    if (prototype)
    {
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            _ItemSpell const& itemSpell = prototype->Spells[slot];
            if (!itemSpell.SpellId ||
                itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            ++useSlots;
            if (itemSpell.SpellId == SpellId && spellSlot == 0xff)
                spellSlot = slot;
            SpellEntry const* slotSpell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
            if (!slotSpell || !IsF47BoundedItemUtilitySpell(*slotSpell))
                everyUseSlotSafe = false;
        }
    }

    bool const prototypeValid = prototype && spell &&
        prototype->ItemId == ItemId &&
        prototype->Class == ITEM_CLASS_CONSUMABLE &&
        prototype->Stackable >= 2 &&
        useSlots == 1 && spellSlot < MAX_ITEM_PROTO_SPELLS &&
        prototype->Spells[spellSlot].SpellId == SpellId &&
        prototype->Spells[spellSlot].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE &&
        prototype->Spells[spellSlot].SpellCharges < 0 &&
        prototype->Spells[spellSlot].SpellCategoryCooldown > 0 &&
        everyUseSlotSafe &&
        spell->IsPositiveSpell() &&
        spell->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
        spell->EffectApplyAuraName[0] == SPELL_AURA_MOD_STAT &&
        spell->EffectImplicitTargetA[0] == TARGET_UNIT_CASTER &&
        spell->EffectImplicitTargetB[0] == 0 &&
        IsF47BoundedItemUtilitySpell(*spell);
    if (!prototypeValid)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] F48 item-backed utility prototype invalid (prototype=%u spell=%u slot=%u useSlots=%u safe=%u).",
            prototype ? 1u : 0u, spell ? 1u : 0u, spellSlot, useSlots,
            everyUseSlotSafe ? 1u : 0u);
        return false;
    }

    class SimulatorItemUtilityCapture final : public ItemUseTerminalListener
    {
    public:
        void OnItemUseTerminal(ItemUseTerminalEvent const& event) override
        {
            events.push_back(event);
        }
        std::vector<ItemUseTerminalEvent> events;
    };

    WorldSession session(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* player = nullptr;
    if (!CreateSimulatorPlayer(map, 1, session, player, 0.0f, 9.0f, 4.0f))
    {
        if (session.GetPlayer())
            session.LogoutPlayer(false);
        else if (player)
            delete player;
        return false;
    }

    PartyBotAI itemAI(player, map.GetId(), map.GetInstanceId(),
                      player->GetPositionX(), player->GetPositionY(),
                      player->GetPositionZ(), player->GetOrientation());
    itemAI.SetPlayer(player);
    player->SetAI(&itemAI);
    std::shared_ptr<SimulatorItemUtilityCapture> capture(
        new SimulatorItemUtilityCapture());
    bool const listenerAdded =
        itemAI.m_partyBotEngine.AddItemUseTerminalListener(capture);

    Item* item = player->StoreNewItemInInventorySlot(ItemId, 2);
    ObjectGuid const itemGuid = item ? item->GetObjectGuid() : ObjectGuid();
    uint32 const initialCount = player->GetItemCount(ItemId);
    float const agilityBefore = player->GetStat(STAT_AGILITY);
    bool const itemUsable = item && player->CanUseItem(item) == EQUIP_ERR_OK &&
        item->GetCount() == 2 &&
        item->GetSpellCharges(spellSlot) < 0;
    bool const initiallyReady = player->IsSpellReady(spell, prototype);
    SpellCastTargets targets;
    targets.PrepareForSpellSystem(player);

    ItemUseId const firstUseId = itemUsable
        ? player->CastItemUseSpell(item, targets) : 0;

    for (uint32 tick = 0; tick < 8 &&
         (itemAI.m_partyBotEngine.GetPendingItemUseCount() != 0 ||
          player->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    uint32 const countAfterFirst = player->GetItemCount(ItemId);
    float const agilityAfterFirst = player->GetStat(STAT_AGILITY);
    bool const auraApplied = player->HasAura(SpellId);
    Item* const remainingItem = itemGuid ? player->GetItemByGuid(itemGuid) : nullptr;

    ItemUseMutationEvent firstMutation;
    std::vector<ItemUseMutationEvent> firstMutations;
    while (itemAI.m_partyBotEngine.PopItemUseMutationEvent(firstMutation))
        firstMutations.push_back(firstMutation);

    bool const firstTerminalValid = listenerAdded && firstUseId &&
        capture->events.size() == 1 &&
        capture->events[0].itemUseId == firstUseId &&
        capture->events[0].actorGuid == player->GetObjectGuid().GetRawValue() &&
        capture->events[0].itemGuid == itemGuid.GetRawValue() &&
        capture->events[0].itemEntry == ItemId &&
        capture->events[0].status == ItemUseAggregateStatus::AllSucceeded &&
        capture->events[0].childCount == 1 &&
        capture->events[0].completedChildCount == 1 &&
        capture->events[0].succeededChildCount == 1 &&
        capture->events[0].observedMutationCount == 1 &&
        capture->events[0].children[0].slotIndex == spellSlot &&
        capture->events[0].children[0].spellId == SpellId &&
        !capture->events[0].children[0].triggered &&
        capture->events[0].children[0].status == ItemUseChildStatus::Succeeded;
    bool const firstMutationValid = capture->events.size() == 1 &&
        firstMutations.size() == 1 &&
        firstMutations[0].itemUseId == firstUseId &&
        firstMutations[0].childId == capture->events[0].children[0].childId &&
        firstMutations[0].itemGuid == itemGuid.GetRawValue() &&
        firstMutations[0].itemEntry == ItemId &&
        firstMutations[0].kind == ItemUseMutationKind::CountChanged &&
        firstMutations[0].phase != ItemUseMutationPhase::Unknown &&
        firstMutations[0].slotIndex == 0xff &&
        firstMutations[0].beforeCount == 2 &&
        firstMutations[0].afterCount == 1 &&
        firstMutations[0].itemExistsAfter;

    // Remove only the GCD before testing the second request. The item/spell
    // cooldown remains owned by Core's cooldown map and must still reject it.
    player->ResetGCD(spell);
    bool const cooldownBlocked = !player->IsSpellReady(spell, prototype);
    uint64 repeatUseId = 0;
    if (cooldownBlocked && remainingItem == item &&
        item->GetCount() == 1)
        repeatUseId = player->CastItemUseSpell(item, targets);

    for (uint32 tick = 0; tick < 4 &&
         (itemAI.m_partyBotEngine.GetPendingItemUseCount() != 0 ||
          player->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    bool const repeatTerminalValid = repeatUseId &&
        capture->events.size() == 2 &&
        capture->events[1].itemUseId == repeatUseId &&
        capture->events[1].actorGuid == player->GetObjectGuid().GetRawValue() &&
        capture->events[1].itemGuid == itemGuid.GetRawValue() &&
        capture->events[1].itemEntry == ItemId &&
        capture->events[1].status == ItemUseAggregateStatus::AllFailed &&
        capture->events[1].childCount == 1 &&
        capture->events[1].completedChildCount == 1 &&
        capture->events[1].failedChildCount == 1 &&
        capture->events[1].observedMutationCount == 0 &&
        capture->events[1].children[0].spellId == SpellId &&
        capture->events[1].children[0].status == ItemUseChildStatus::Failed;

    ItemUseMutationEvent repeatMutation;
    bool const unexpectedRepeatMutation =
        itemAI.m_partyBotEngine.PopItemUseMutationEvent(repeatMutation);

    uint32 const countAfterRepeat = player->GetItemCount(ItemId);
    float const agilityAfterRepeat = player->GetStat(STAT_AGILITY);
    Item* const itemAfterRepeat = itemGuid ? player->GetItemByGuid(itemGuid) : nullptr;
    bool const firstUseSucceeded = firstTerminalValid && firstMutationValid &&
        initiallyReady && itemUsable &&
        initialCount == 2 && countAfterFirst == 1 &&
        remainingItem && remainingItem == item &&
        auraApplied && agilityAfterFirst > agilityBefore &&
        player->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
    bool const cooldownRejectedRepeat = cooldownBlocked && repeatTerminalValid &&
        !unexpectedRepeatMutation && countAfterRepeat == 1 &&
        itemAfterRepeat == item && itemAfterRepeat->GetCount() == 1 &&
        player->HasAura(SpellId) &&
        agilityAfterRepeat == agilityAfterFirst;
    bool const pendingCleared =
        itemAI.m_partyBotEngine.GetPendingItemUseCount() == 0;

    sLog.Out(LOG_BASIC,
        firstUseSucceeded && cooldownRejectedRepeat && pendingCleared
            ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F48 real item utility: prototype=%u listener=%u canUse=%u initiallyReady=%u useSlots=%u slot=%u spell=%u positive=%u auraApplied=%u agility=%.1f>%.1f count=%u>%u mutation=%u mutationKind=%u mutationPhase=%u root=%u child=%u cooldownBlocked=%u repeatTerminal=%u repeatMutation=%u repeatCount=%u pendingCleared=%u.",
        prototypeValid ? 1u : 0u, listenerAdded ? 1u : 0u,
        itemUsable ? 1u : 0u, initiallyReady ? 1u : 0u, useSlots,
        spellSlot, SpellId, spell->IsPositiveSpell() ? 1u : 0u,
        auraApplied ? 1u : 0u, agilityBefore, agilityAfterFirst,
        initialCount, countAfterFirst, static_cast<uint32>(firstMutations.size()),
        firstMutations.empty() ? 0u : static_cast<uint32>(firstMutations[0].kind),
        firstMutations.empty() ? 0u : static_cast<uint32>(firstMutations[0].phase),
        firstTerminalValid ? 1u : 0u, firstUseId ? 1u : 0u,
        cooldownBlocked ? 1u : 0u, repeatTerminalValid ? 1u : 0u,
        unexpectedRepeatMutation ? 1u : 0u, countAfterRepeat,
        pendingCleared ? 1u : 0u);

    bool const passed = firstUseSucceeded && cooldownRejectedRepeat &&
        pendingCleared && player->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F48 used the real Vanilla Elixir of Minor Agility ON_USE item through Player::CastItemUseSpell; Core applied the self-targeted Lesser Agility aura/stat change, aggregated its successful child and CountChanged stack mutation, then rejected an immediate repeat on the item's cooldown without consuming another item or applying a second effect.");

    if (player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        player->InterruptSpell(CURRENT_GENERIC_SPELL, true);
    player->SetAI(nullptr);
    session.LogoutPlayer(false);
    bool const playerCleaned = session.GetPlayer() == nullptr;
    if (!playerCleaned)
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] F48 item utility Player cleanup failed.");
    return passed && playerCleaned;
}


bool ResolveSimulatorItemUtilityRequest(
    Player* player, ActionCandidate const& candidate, Item*& item,
    SpellEntry const*& spell, char const*& reason)
{
    item = nullptr; spell = nullptr; reason = "item_utility_request_invalid";
    if (!player || !player->IsInWorld() || !player->IsAlive()) {
        reason = "item_utility_actor_unavailable"; return false;
    }
    if (player->IsInCombat()) {
        reason = "item_utility_actor_in_combat"; return false;
    }
    if (player->GetCurrentSpell(CURRENT_GENERIC_SPELL)) {
        reason = "item_utility_actor_cast_in_progress"; return false;
    }
    if (candidate.targetGuid || !candidate.itemGuid || !candidate.itemEntry ||
        candidate.itemPosition == 0xffff ||
        candidate.itemSpellSlot >= MAX_ITEM_PROTO_SPELLS || !candidate.spellId) {
        reason = "item_utility_candidate_identity_invalid"; return false;
    }

    item = player->GetItemByPos(candidate.itemPosition);
    if (!item || item->GetPos() != candidate.itemPosition ||
        item->GetObjectGuid().GetRawValue() != candidate.itemGuid ||
        item->GetOwnerGuid() != player->GetObjectGuid()) {
        item = nullptr; reason = "item_utility_inventory_identity_mismatch"; return false;
    }
    ItemPrototype const* prototype = item->GetProto();
    if (!prototype || prototype->ItemId != candidate.itemEntry ||
        prototype->Class != ITEM_CLASS_CONSUMABLE || prototype->Stackable < 2 ||
        !item->GetCount()) {
        item = nullptr; reason = "item_utility_item_prototype_or_stack_invalid"; return false;
    }
    if (player->CanUseItem(item) != EQUIP_ERR_OK) {
        item = nullptr; reason = "item_utility_core_can_use_item_rejected"; return false;
    }

    _ItemSpell const& requested = prototype->Spells[candidate.itemSpellSlot];
    if (requested.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE ||
        requested.SpellId != candidate.spellId) {
        item = nullptr; reason = "item_utility_requested_on_use_slot_mismatch"; return false;
    }
    uint32 onUseSlots = 0;
    bool requestedSlotFound = false;
    for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot) {
        _ItemSpell const& itemSpell = prototype->Spells[slot];
        if (!itemSpell.SpellId || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;
        ++onUseSlots;
        if (slot == candidate.itemSpellSlot) requestedSlotFound = true;
        SpellEntry const* slotSpell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
        if (!slotSpell || !IsF47BoundedItemUtilitySpell(*slotSpell)) {
            item = nullptr; reason = "item_utility_on_use_fanout_not_fully_bounded"; return false;
        }
    }
    if (onUseSlots != 1 || !requestedSlotFound) {
        item = nullptr; reason = "item_utility_on_use_fanout_not_single_slot"; return false;
    }

    spell = sSpellMgr.GetSpellEntry(candidate.spellId);
    if (!spell || !spell->IsPositiveSpell() || !IsF47BoundedItemUtilitySpell(*spell)) {
        item = nullptr; spell = nullptr;
        reason = "item_utility_spell_not_f47_bounded"; return false;
    }
    if (!player->IsSpellReady(spell, prototype)) {
        item = nullptr; spell = nullptr; reason = "item_utility_cooldown_not_ready"; return false;
    }
    reason = "item_utility_inventory_request_resolved";
    return true;
}

class SimulatorCoreItemUtilityUseAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        CoreActionResolution const resolved = context.coreActionRuntime
            ? context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid)
            : CoreActionResolution();
        if (!resolved.IsResolved() || !resolved.actor) {
            reason = resolved.reason ? resolved.reason : "item_utility_actor_resolution_failed";
            return false;
        }
        Item* item = nullptr; SpellEntry const* spell = nullptr; char const* failure = nullptr;
        if (!ResolveSimulatorItemUtilityRequest(resolved.actor, candidate, item, spell, failure)) {
            reason = failure ? failure : "item_utility_request_rejected"; return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        CoreActionResolution const resolved = context.coreActionRuntime
            ? context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid)
            : CoreActionResolution();
        if (!resolved.IsResolved() || !resolved.actor) {
            reason = resolved.reason ? resolved.reason : "item_utility_actor_resolution_failed";
            return false;
        }
        if (resolved.actor->HasAura(candidate.spellId)) {
            reason = "item_utility_aura_already_active"; return false;
        }
        return true;
    }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const& candidate) const override
    {
        ThreatImpactEstimate estimate;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "item_utility_threat_impact_unproven";
        CoreActionResolution const resolved = context.coreActionRuntime
            ? context.coreActionRuntime->ResolveUnitContext(context, candidate.targetGuid)
            : CoreActionResolution();
        Item* item = nullptr; SpellEntry const* spell = nullptr; char const* failure = nullptr;
        if (!resolved.IsResolved() || !resolved.actor ||
            !ResolveSimulatorItemUtilityRequest(resolved.actor, candidate, item, spell, failure)) {
            estimate.reason = failure ? failure : "item_utility_threat_actor_unresolved";
            return estimate;
        }
        // This NoThreat scope applies only to F47's positive item-cast path;
        // ordinary positive non-triggered Player spells remain excluded by F46.
        return MakeSimulatorNoThreatEstimate(
            context, "core_positive_item_cast_suppresses_procs_for_f47_bounded_self_aura");
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const&, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const override
    {
        ActionExecutionResult result;
        if (!resolved.actor || candidate.targetGuid || !candidate.actionId) {
            result.result = ActionResult::Impossible;
            result.reason = "item_utility_live_preflight_actor_or_action_invalid";
            return result;
        }
        Item* item = nullptr; SpellEntry const* spell = nullptr; char const* failure = nullptr;
        if (!ResolveSimulatorItemUtilityRequest(resolved.actor, candidate, item, spell, failure)) {
            result.result = ActionResult::Impossible;
            result.reason = failure ? failure : "item_utility_live_preflight_rejected";
            return result;
        }
        if (resolved.actor->HasAura(candidate.spellId)) {
            result.result = ActionResult::Useless;
            result.reason = "item_utility_aura_already_active";
            return result;
        }
        result.result = ActionResult::Success;
        result.reason = "item_utility_live_preflight_passed";
        return result;
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext&, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result;
        Item* item = nullptr; SpellEntry const* spell = nullptr; char const* failure = nullptr;
        if (!resolved.actor ||
            !ResolveSimulatorItemUtilityRequest(resolved.actor, candidate, item, spell, failure)) {
            result.result = ActionResult::Failed;
            result.reason = failure ? failure : "item_utility_execute_resolution_failed";
            return result;
        }
        SpellCastTargets targets;
        targets.PrepareForSpellSystem(resolved.actor);
        uint64 const itemUseId = resolved.actor->CastItemUseSpell(
            item, targets, item->GetBagSlot(), item->GetSlot(), candidate.itemSpellSlot);
        result.result = itemUseId ? ActionResult::Success : ActionResult::Failed;
        result.reason = itemUseId ? "item_utility_core_item_use_started"
                                  : "item_utility_core_item_use_root_not_started";
        return result;
    }

private:
    std::string m_name = "core_item_utility_use";
};

class SimulatorItemUtilityUseTrigger final : public Trigger
{
public:
    explicit SimulatorItemUtilityUseTrigger(ActionCandidate candidate)
        : m_candidate(std::move(candidate)) {}
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetCheckIntervalMs() const override { return 0; }
    bool IsActive(DecisionContext const& context) const override {
        return context.bot.online && context.bot.alive;
    }
    void SetCandidate(ActionCandidate const& candidate) { m_candidate = candidate; }
    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override {
        candidates.push_back(m_candidate);
    }
private:
    std::string m_name = "simulator_item_utility";
    ActionCandidate m_candidate;
};

ActionCandidate MakeSimulatorItemUtilityCandidate(Item const& item, uint8 spellSlot,
                                                   uint32 spellId)
{
    ActionCandidate candidate;
    candidate.actionName = "core_item_utility_use";
    candidate.spellId = spellId;
    candidate.itemGuid = item.GetObjectGuid().GetRawValue();
    candidate.itemEntry = item.GetProto()->ItemId;
    candidate.itemPosition = item.GetPos();
    candidate.itemSpellSlot = spellSlot;
    candidate.basePriority = 75.0f;
    candidate.channel = ActionChannel::GlobalCooldown;
    return candidate;
}

bool RunEngineCoreItemUtilityActionSmokeTest(Map& map)
{
    static uint32 const ItemId = 2457;
    static uint32 const SpellId = 2374;
    ItemPrototype const* prototype = sObjectMgr.GetItemPrototype(ItemId);
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(SpellId);
    uint8 spellSlot = 0xff;
    if (prototype)
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
            if (prototype->Spells[slot].SpellId == SpellId &&
                prototype->Spells[slot].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                spellSlot = slot;
    bool const prototypeValid = prototype && spell &&
        prototype->Class == ITEM_CLASS_CONSUMABLE && prototype->Stackable >= 2 &&
        spellSlot < MAX_ITEM_PROTO_SPELLS &&
        prototype->Spells[spellSlot].SpellCharges < 0 &&
        IsF47BoundedItemUtilitySpell(*spell);
    if (!prototypeValid) return false;

    class SimulatorItemUtilityActionCapture final : public ItemUseTerminalListener
    {
    public:
        void OnItemUseTerminal(ItemUseTerminalEvent const& event) override {
            events.push_back(event);
        }
        std::vector<ItemUseTerminalEvent> events;
    };

    WorldSession session(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* player = nullptr;
    if (!CreateSimulatorPlayer(map, 1, session, player, 0.0f, 15.0f, 4.0f)) {
        if (session.GetPlayer()) session.LogoutPlayer(false);
        else if (player) delete player;
        return false;
    }
    PartyBotAI itemAI(player, map.GetId(), map.GetInstanceId(),
                      player->GetPositionX(), player->GetPositionY(),
                      player->GetPositionZ(), player->GetOrientation());
    itemAI.SetPlayer(player);
    player->SetAI(&itemAI);
    std::shared_ptr<SimulatorItemUtilityActionCapture> capture(
        new SimulatorItemUtilityActionCapture());
    bool const listenerAdded = itemAI.m_partyBotEngine.AddItemUseTerminalListener(capture);

    PartySnapshot snapshot;
    snapshot.groupId = 2;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 1;
    snapshot.capturedAtMs = WorldTimer::getMSTime();
    PartyMemberSnapshot bot;
    bot.guid = player->GetObjectGuid().GetRawValue();
    bot.mapId = map.GetId();
    bot.instanceId = map.GetInstanceId();
    bot.online = player->IsInWorld();
    bot.alive = player->IsAlive();
    bot.health = player->GetHealth();
    bot.maxHealth = player->GetMaxHealth();
    snapshot.members.push_back(bot);

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator,
                            WorldTimer::getMSTime(), nullptr, nullptr, &itemAI, nullptr, &itemAI);
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCoreItemUtilityUseAction> action(
        new SimulatorCoreItemUtilityUseAction());
    bool const registered = engine.RegisterAction(action);
    std::shared_ptr<SimulatorItemUtilityUseTrigger> trigger(
        new SimulatorItemUtilityUseTrigger(ActionCandidate()));
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_item_utility"));
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    // Select an exact Item, remove it before live preflight, and require no side effects.
    Item* staleItem = player->StoreNewItemInInventorySlot(ItemId, 1);
    ActionCandidate staleCandidate;
    if (staleItem) staleCandidate = MakeSimulatorItemUtilityCandidate(*staleItem, spellSlot, SpellId);
    trigger->SetCandidate(staleCandidate);
    DecisionReport staleReport = engine.Evaluate(context);
    bool const staleSelected = staleItem && staleReport.selectedIndices.size() == 1;
    uint8 const staleBag = staleItem ? staleItem->GetBagSlot() : 0xff;
    uint8 const staleSlot = staleItem ? staleItem->GetSlot() : 0xff;
    uint16 const stalePosition = staleItem ? staleItem->GetPos() : 0xffff;
    if (staleItem) player->DestroyItem(staleBag, staleSlot, true);
    bool const staleRemoved = !staleItem || player->GetItemByPos(stalePosition) == nullptr;
    if (staleSelected) engine.ExecuteSelected(context, staleReport);
    bool const staleRejected = staleSelected &&
        staleReport.candidates[staleReport.selectedIndices[0]].status == CandidateStatus::ExecutionFailed &&
        staleReport.candidates[staleReport.selectedIndices[0]].actionResult == ActionResult::Impossible &&
        staleReport.candidates[staleReport.selectedIndices[0]].reason ==
            "item_utility_inventory_identity_mismatch";
    ItemUseMutationEvent staleMutation;
    bool const staleMutationAbsent = !itemAI.m_partyBotEngine.PopItemUseMutationEvent(staleMutation);
    bool const staleSideEffectsAbsent = staleRemoved && staleRejected && capture->events.empty() &&
        itemAI.m_partyBotEngine.GetPendingItemUseCount() == 0 && staleMutationAbsent;

    Item* item = player->StoreNewItemInInventorySlot(ItemId, 2);
    float const agilityBeforeUse = player->GetStat(STAT_AGILITY);
    ActionCandidate candidate;
    if (item) candidate = MakeSimulatorItemUtilityCandidate(*item, spellSlot, SpellId);
    trigger->SetCandidate(candidate);
    snapshot.epoch = 2;
    snapshot.capturedAtMs = WorldTimer::getMSTime();
    DecisionReport report = engine.Evaluate(context);
    bool const selected = item && report.selectedIndices.size() == 1;
    std::size_t const selectedIndex = selected ? report.selectedIndices[0] : 0;
    if (selected) engine.ExecuteSelected(context, report);
    for (uint32 tick = 0; tick < 8 &&
         (itemAI.m_partyBotEngine.GetPendingItemUseCount() != 0 ||
          player->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    ActionId const engineActionId = selected ? report.candidates[selectedIndex].actionId : 0;
    uint32 const countAfterUse = player->GetItemCount(ItemId);
    float const agilityAfterUse = player->GetStat(STAT_AGILITY);
    bool const auraApplied = player->HasAura(SpellId);
    bool const actionExecuted = selected &&
        report.candidates[selectedIndex].status == CandidateStatus::Executed &&
        report.candidates[selectedIndex].actionResult == ActionResult::Success;

    ItemUseMutationEvent mutation;
    std::vector<ItemUseMutationEvent> mutations;
    while (itemAI.m_partyBotEngine.PopItemUseMutationEvent(mutation))
        mutations.push_back(mutation);
    bool const terminalValid = capture->events.size() == 1 && engineActionId &&
        capture->events[0].actionId == engineActionId &&
        capture->events[0].actorGuid == bot.guid &&
        capture->events[0].itemGuid == candidate.itemGuid &&
        capture->events[0].itemEntry == ItemId &&
        capture->events[0].status == ItemUseAggregateStatus::AllSucceeded &&
        capture->events[0].childCount == 1 && capture->events[0].succeededChildCount == 1 &&
        capture->events[0].children[0].spellId == SpellId &&
        capture->events[0].children[0].status == ItemUseChildStatus::Succeeded;
    bool const mutationValid = capture->events.size() == 1 && mutations.size() == 1 &&
        mutations[0].itemUseId == capture->events[0].itemUseId &&
        mutations[0].childId == capture->events[0].children[0].childId &&
        mutations[0].itemGuid == candidate.itemGuid && mutations[0].itemEntry == ItemId &&
        mutations[0].kind == ItemUseMutationKind::CountChanged &&
        mutations[0].beforeCount == 2 && mutations[0].afterCount == 1 &&
        mutations[0].itemExistsAfter;

    player->ResetGCD(spell);
    bool const cooldownBlocked = !player->IsSpellReady(spell, prototype);
    snapshot.epoch = 3;
    snapshot.capturedAtMs = WorldTimer::getMSTime();
    DecisionReport repeatReport = engine.Evaluate(context);
    bool const repeatRejected = repeatReport.selectedIndices.empty() &&
        repeatReport.candidates.size() == 1 &&
        repeatReport.candidates[0].status == CandidateStatus::RejectedImpossible &&
        repeatReport.candidates[0].reason == "item_utility_cooldown_not_ready";
    Item* const sameItem = candidate.itemPosition != 0xffff
        ? player->GetItemByPos(candidate.itemPosition) : nullptr;
    bool const noRepeatRootOrMutation = capture->events.size() == 1 &&
        itemAI.m_partyBotEngine.GetPendingItemUseCount() == 0 &&
        !itemAI.m_partyBotEngine.PopItemUseMutationEvent(mutation);
    bool const useSucceeded = listenerAdded && registered && staleSideEffectsAbsent &&
        selected && actionExecuted && engineActionId != 0 && terminalValid && mutationValid &&
        auraApplied && countAfterUse == 1 && sameItem &&
        sameItem->GetObjectGuid().GetRawValue() == candidate.itemGuid &&
        sameItem->GetCount() == 1 && agilityAfterUse > agilityBeforeUse;
    bool const cooldownSafe = cooldownBlocked && repeatRejected && noRepeatRootOrMutation &&
        sameItem && sameItem->GetCount() == 1 && player->HasAura(SpellId);

    sLog.Out(LOG_BASIC, useSucceeded && cooldownSafe ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F49 Engine item utility: prototype=%u listener=%u staleSelected=%u staleRejected=%u staleSideEffectsAbsent=%u selected=%u executed=%u actionId=%llu rootActionMatch=%u aura=%u agility=%.1f>%.1f count=2>%u mutation=%u terminal=%u cooldownBlocked=%u repeatRejected=%u repeatCount=%u pendingCleared=%u.",
        prototypeValid ? 1u : 0u, listenerAdded ? 1u : 0u,
        staleSelected ? 1u : 0u, staleRejected ? 1u : 0u,
        staleSideEffectsAbsent ? 1u : 0u, selected ? 1u : 0u, actionExecuted ? 1u : 0u,
        static_cast<unsigned long long>(engineActionId),
        terminalValid ? 1u : 0u, auraApplied ? 1u : 0u,
        agilityBeforeUse, agilityAfterUse, countAfterUse,
        mutationValid ? 1u : 0u, terminalValid ? 1u : 0u,
        cooldownBlocked ? 1u : 0u, repeatRejected ? 1u : 0u,
        sameItem ? sameItem->GetCount() : 0u, noRepeatRootOrMutation ? 1u : 0u);

    bool const passed = useSucceeded && cooldownSafe &&
        player->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F49 selected an item-backed self-utility candidate through PartyBotEngine, revalidated exact inventory identity before Core item use, correlated the Engine action ID with ItemUseTerminal, applied the real Core aura/stat and stack mutation, rejected a stale removed-item race without side effects, and suppressed a cooldown-blocked repeat before dispatch.");

    if (player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        player->InterruptSpell(CURRENT_GENERIC_SPELL, true);
    player->SetAI(nullptr);
    session.LogoutPlayer(false);
    bool const playerCleaned = session.GetPlayer() == nullptr;
    if (!playerCleaned)
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PartyBotSimulator] F49 item utility Player cleanup failed.");
    return passed && playerCleaned;
}


} // anonymous namespace



bool RunEngineCoreFollowPartyMemberActionSmokeTest(Map& map)
{
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    WorldSession followerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    WorldSession firstLeaderSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    WorldSession nextLeaderSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* follower = nullptr;
    Player* firstLeader = nullptr;
    Player* nextLeader = nullptr;

    bool const playersCreated =
        CreateSimulatorPlayer(map, partyFaction, followerSession, follower,
                              0.0f, 30.0f, 0.0f) &&
        CreateSimulatorPlayer(map, partyFaction, firstLeaderSession, firstLeader,
                              8.0f, 30.0f, 0.0f) &&
        CreateSimulatorPlayer(map, partyFaction, nextLeaderSession, nextLeader,
                              14.0f, 30.0f, 0.0f);

    ObjectGuid const followerGuid = follower
        ? follower->GetObjectGuid() : ObjectGuid();
    ObjectGuid const firstLeaderGuid = firstLeader
        ? firstLeader->GetObjectGuid() : ObjectGuid();
    ObjectGuid const nextLeaderGuid = nextLeader
        ? nextLeader->GetObjectGuid() : ObjectGuid();

    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;
    std::unique_ptr<PartyBotAI> actionRuntime;

    auto cleanup = [&]() -> bool
    {
        if (follower)
        {
            follower->GetMotionMaster()->MoveIdle();
            follower->SetAI(nullptr);
        }
        actionRuntime.reset();

        bool ungrouped = true;
        if (group && group->GetId())
        {
            // Group::Create converts the follower's temporary solo instance bind
            // into a group bind. Remove only that synthetic bind before Disband:
            // Group::Disband otherwise resets the shared test map, preventing
            // later Core Player fixtures from being created on the same instance.
            group->UnbindInstance(map.GetId(), false);
            group->Disband(true, group->GetLeaderGuid());
            for (Player* player : {follower, firstLeader, nextLeader})
                if (player && player->GetGroup())
                    ungrouped = false;
            if (groupRegistered)
            {
                sObjectMgr.RemoveGroup(group.get());
                groupRegistered = false;
            }
            group.reset();
        }

        if (followerSession.GetPlayer())
            followerSession.LogoutPlayer(false);
        if (firstLeaderSession.GetPlayer())
            firstLeaderSession.LogoutPlayer(false);
        if (nextLeaderSession.GetPlayer())
            nextLeaderSession.LogoutPlayer(false);

        bool const sessionsCleaned =
            !followerSession.GetPlayer() &&
            !firstLeaderSession.GetPlayer() &&
            !nextLeaderSession.GetPlayer();
        bool const playersDetached =
            (!followerGuid || !map.GetUnit(followerGuid)) &&
            (!firstLeaderGuid || !map.GetUnit(firstLeaderGuid)) &&
            (!nextLeaderGuid || !map.GetUnit(nextLeaderGuid));

        follower = nullptr;
        firstLeader = nullptr;
        nextLeader = nullptr;
        return ungrouped && sessionsCleaned && playersDetached;
    };

    if (!playersCreated || !follower || !firstLeader || !nextLeader)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] FollowPartyMember Player creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    bool const groupCreated = group->Create(
        firstLeader->GetObjectGuid(), firstLeader->GetName());
    if (groupCreated)
    {
        sObjectMgr.AddGroup(group.get());
        groupRegistered = true;
    }
    bool const membersAdded = groupCreated &&
        group->AddMember(follower->GetObjectGuid(), follower->GetName()) &&
        group->AddMember(nextLeader->GetObjectGuid(), nextLeader->GetName());
    if (!membersAdded)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] FollowPartyMember Group setup failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    actionRuntime.reset(new PartyBotAI(
        firstLeader, map.GetId(), map.GetInstanceId(),
        follower->GetPositionX(), follower->GetPositionY(),
        follower->GetPositionZ(), follower->GetOrientation()));
    actionRuntime->SetPlayer(follower);
    actionRuntime->m_initialized = true;
    actionRuntime->m_updateTimer.Reset(600000);
    follower->SetAI(actionRuntime.get());

    PartyCoordinator initialCoordinator;
    PartySnapshot const* initialSnapshot = PartySnapshotBuilder::GetOrBuild(
        follower, initialCoordinator, WorldTimer::getMSTime(), 1000);
    PartyMemberSnapshot const* initialBot =
        initialSnapshot ? initialSnapshot->FindMember(followerGuid.GetRawValue()) : nullptr;
    bool const rosterValid = initialSnapshot &&
        initialSnapshot->groupLeaderGuid == firstLeaderGuid.GetRawValue() &&
        initialSnapshot->members.size() == 3 && initialBot &&
        initialSnapshot->FindMember(firstLeaderGuid.GetRawValue()) &&
        initialSnapshot->FindMember(nextLeaderGuid.GetRawValue()) &&
        initialCoordinator.GetFormationSlot(followerGuid.GetRawValue()) == 0;
    if (!rosterValid)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] FollowPartyMember initial roster snapshot failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }
    PartySnapshot const beforeFollow = *initialSnapshot;
    Guid const firstGuid = firstLeaderGuid.GetRawValue();
    Guid const nextGuid = nextLeaderGuid.GetRawValue();
    Guid const botGuid = followerGuid.GetRawValue();

    PartyBotEngine engine;
    std::shared_ptr<FollowPartyMemberAction> action(
        new FollowPartyMemberAction());
    bool const registered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> firstStrategy(
        new Strategy("simulator_follow_party_member_first"));
    firstStrategy->AddTrigger(
        std::make_shared<SimulatorFollowPartyMemberTrigger>(firstGuid));
    engine.AddStrategy(firstStrategy);

    PartyMemberSnapshot const* botSnapshot = beforeFollow.FindMember(botGuid);
    DecisionContext firstContext(
        beforeFollow, *botSnapshot, initialCoordinator, WorldTimer::getMSTime(),
        nullptr, nullptr, nullptr, nullptr, actionRuntime.get());
    DecisionReport firstReport = engine.Evaluate(firstContext);
    bool const firstSelected = registered &&
        firstReport.selectedIndices.size() == 1 &&
        firstReport.candidates[firstReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (firstSelected)
        engine.ExecuteSelected(firstContext, firstReport);

    MovementGenerator const* firstMovement = follower->GetMotionMaster()->GetCurrent();
    FollowMovementGenerator<Player> const* firstFollow =
        firstMovement &&
        firstMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
            ? static_cast<FollowMovementGenerator<Player> const*>(firstMovement)
            : nullptr;
    bool const firstStarted = firstSelected &&
        firstReport.candidates[firstReport.selectedIndices[0]].status ==
            CandidateStatus::Executed &&
        firstReport.candidates[firstReport.selectedIndices[0]].actionResult ==
            ActionResult::Success &&
        firstFollow && firstFollow->GetTarget() == firstLeader &&
        follower->HasUnitState(UNIT_STATE_FOLLOW);

    PartyCoordinator observedCoordinator;
    PartySnapshot const* observedSnapshot = PartySnapshotBuilder::GetOrBuild(
        follower, observedCoordinator, WorldTimer::getMSTime(), 1000);
    PartyMemberSnapshot const* observedBot = observedSnapshot
        ? observedSnapshot->FindMember(botGuid) : nullptr;
    bool const followTargetCaptured = observedBot &&
        observedBot->followTargetGuid == firstGuid &&
        observedBot->followFormationSlot == 0;

    bool sameTargetNotRestarted = false;
    bool sameTargetFormationReapplied = false;
    bool retargeted = false;
    bool staleGroupRejected = false;
    bool retargetRegistered = false;
    bool staleSelected = false;
    bool firstLeaderRemoved = false;
    bool stopFollowingSnapshotCaptured = false;
    bool stopFollowingRegistered = false;
    bool staleStopTargetRejected = false;
    bool stoppedFollowing = false;
    bool pointMovementPreserved = false;
    bool productionFollowPassed = false;
    bool f51CombatFollowStopped = false;
    bool f51CombatFollowResumed = false;
    bool f52bLeaderHandoffRetargeted = false;
    bool f52bOwnerLeaderRestored = false;

    if (observedSnapshot && observedBot)
    {
        DecisionContext sameTargetContext(
            *observedSnapshot, *observedBot, observedCoordinator,
            WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
            actionRuntime.get());
        DecisionReport sameTargetReport = engine.Evaluate(sameTargetContext);
        sameTargetNotRestarted = sameTargetReport.selectedIndices.empty() &&
            !sameTargetReport.candidates.empty() &&
            sameTargetReport.candidates[0].status == CandidateStatus::RejectedUseless;

        // A same-target request with a different assigned slot must replace
        // the native Follow generator so existing Bots adopt the new formation.
        PartyBotEngine formationEngine;
        std::shared_ptr<FollowPartyMemberAction> formationAction(
            new FollowPartyMemberAction());
        bool const formationRegistered = formationEngine.RegisterAction(formationAction);
        std::shared_ptr<Strategy> formationStrategy(
            new Strategy("simulator_follow_party_member_formation"));
        formationStrategy->AddTrigger(
            std::make_shared<SimulatorFollowPartyMemberTrigger>(firstGuid, 1));
        formationEngine.AddStrategy(formationStrategy);
        DecisionContext formationContext(
            *observedSnapshot, *observedBot, observedCoordinator,
            WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
            actionRuntime.get());
        DecisionReport formationReport = formationEngine.Evaluate(formationContext);
        bool const formationSelected = formationRegistered &&
            formationReport.selectedIndices.size() == 1 &&
            formationReport.candidates[formationReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (formationSelected)
            formationEngine.ExecuteSelected(formationContext, formationReport);
        CandidateEvaluation const* formationEvaluation = formationSelected
            ? &formationReport.candidates[formationReport.selectedIndices[0]] : nullptr;
        MovementGenerator const* formationMovement =
            follower->GetMotionMaster()->GetCurrent();
        FollowMovementGenerator<Player> const* formationFollow =
            formationMovement &&
            formationMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                ? static_cast<FollowMovementGenerator<Player> const*>(formationMovement)
                : nullptr;
        FollowFormationOffset const slotOneOffset = GetFollowFormationOffset(1);
        sameTargetFormationReapplied = formationEvaluation &&
            formationEvaluation->status == CandidateStatus::Executed &&
            formationEvaluation->actionResult == ActionResult::Success &&
            formationFollow && formationFollow->GetTarget() == firstLeader &&
            formationFollow->GetFollowDistance() == slotOneOffset.distance &&
            formationFollow->GetFollowAngle() == slotOneOffset.angle;

        PartyBotEngine retargetEngine;
        std::shared_ptr<FollowPartyMemberAction> retargetAction(
            new FollowPartyMemberAction());
        retargetRegistered = retargetEngine.RegisterAction(retargetAction);
        std::shared_ptr<Strategy> retargetStrategy(
            new Strategy("simulator_follow_party_member_retarget"));
        retargetStrategy->AddTrigger(
            std::make_shared<SimulatorFollowPartyMemberTrigger>(nextGuid));
        retargetEngine.AddStrategy(retargetStrategy);
        DecisionContext retargetContext(
            *observedSnapshot, *observedBot, observedCoordinator,
            WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
            actionRuntime.get());
        DecisionReport retargetReport = retargetEngine.Evaluate(retargetContext);
        bool const retargetSelected = retargetRegistered &&
            retargetReport.selectedIndices.size() == 1 &&
            retargetReport.candidates[retargetReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (retargetSelected)
            retargetEngine.ExecuteSelected(retargetContext, retargetReport);
        MovementGenerator const* retargetMovement =
            follower->GetMotionMaster()->GetCurrent();
        FollowMovementGenerator<Player> const* retargetFollow =
            retargetMovement &&
            retargetMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                ? static_cast<FollowMovementGenerator<Player> const*>(retargetMovement)
                : nullptr;
        retargeted = retargetSelected &&
            retargetReport.candidates[retargetReport.selectedIndices[0]].status ==
                CandidateStatus::Executed &&
            retargetReport.candidates[retargetReport.selectedIndices[0]].actionResult ==
                ActionResult::Success &&
            retargetFollow && retargetFollow->GetTarget() == nextLeader;

        // F50: run the real PartyBotAI::UpdateAI under explicit NewEngine
        // authority and require the production policy to select the Group leader.
        if (retargeted)
        {
            follower->GetMotionMaster()->MoveIdle();
            actionRuntime->m_simulatorExecutionAuthorityOverride =
                static_cast<int32>(ExecutionAuthorityMode::NewEngine);
            actionRuntime->m_updateTimer.Reset(1000);
            actionRuntime->UpdateAI(1000);

            MovementGenerator const* productionMovement =
                follower->GetMotionMaster()->GetCurrent();
            FollowMovementGenerator<Player> const* productionFollow =
                productionMovement &&
                productionMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                    ? static_cast<FollowMovementGenerator<Player> const*>(productionMovement)
                    : nullptr;
            productionFollowPassed =
                actionRuntime->m_simulatorNewEngineTickCount == 1 &&
                actionRuntime->m_simulatorLegacyPolicyEntryCount == 0 &&
                actionRuntime->m_simulatorLegacyPolicyDispatchCount == 0 &&
                actionRuntime->m_simulatorEngineDecisionCycleCount == 1 &&
                actionRuntime->m_simulatorEngineCandidateCount == 1 &&
                actionRuntime->m_newEngineStrategiesRegistered &&
                !actionRuntime->m_engineShadowStrategiesRegistered &&
                actionRuntime->m_partyCoordinator &&
                actionRuntime->m_partyCoordinator->GetFormationSlot(botGuid) == 0 &&
                productionFollow && productionFollow->GetTarget() == firstLeader &&
                follower->HasUnitState(UNIT_STATE_FOLLOW);

            // F51: stop the native Follow as soon as combat begins, then
            // resume Group-leader Follow after Core reports combat has ended.
            if (productionFollowPassed)
            {
                follower->SetInCombatState();
                if (actionRuntime->m_partyCoordinator)
                    actionRuntime->m_partyCoordinator->InvalidateSnapshot();
                actionRuntime->m_updateTimer.Reset(1000);
                actionRuntime->UpdateAI(1000);
                MovementGenerator const* combatMovement =
                    follower->GetMotionMaster()->GetCurrent();
                FollowMovementGenerator<Player> const* combatFollow =
                    combatMovement &&
                    combatMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                        ? static_cast<FollowMovementGenerator<Player> const*>(combatMovement)
                        : nullptr;
                f51CombatFollowStopped =
                    actionRuntime->m_simulatorNewEngineTickCount == 2 &&
                    actionRuntime->m_simulatorEngineDecisionCycleCount == 2 &&
                    actionRuntime->m_simulatorEngineCandidateCount == 1 &&
                    actionRuntime->m_simulatorLegacyPolicyEntryCount == 0 &&
                    actionRuntime->m_simulatorLegacyPolicyDispatchCount == 0 &&
                    !combatFollow &&
                    !follower->HasUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);

                follower->ClearInCombat();
                if (actionRuntime->m_partyCoordinator)
                    actionRuntime->m_partyCoordinator->InvalidateSnapshot();
                actionRuntime->m_updateTimer.Reset(1000);
                actionRuntime->UpdateAI(1000);
                MovementGenerator const* recoveredMovement =
                    follower->GetMotionMaster()->GetCurrent();
                FollowMovementGenerator<Player> const* recoveredFollow =
                    recoveredMovement &&
                    recoveredMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                        ? static_cast<FollowMovementGenerator<Player> const*>(recoveredMovement)
                        : nullptr;
                f51CombatFollowResumed =
                    actionRuntime->m_simulatorNewEngineTickCount == 3 &&
                    actionRuntime->m_simulatorEngineDecisionCycleCount == 3 &&
                    actionRuntime->m_simulatorEngineCandidateCount == 1 &&
                    actionRuntime->m_simulatorLegacyPolicyEntryCount == 0 &&
                    actionRuntime->m_simulatorLegacyPolicyDispatchCount == 0 &&
                    recoveredFollow && recoveredFollow->GetTarget() == firstLeader &&
                    follower->HasUnitState(UNIT_STATE_FOLLOW);
            }

            // Start F52b from the original owner/Group Leader. The Bot follows
            // this target before another live member takes Group leadership.
            follower->GetMotionMaster()->MoveFollow(
                firstLeader, 2.5f, 3.14159265358979323846f);
        }

        // F52b: another live Group member becomes leader while the Bot
        // still follows the original owner. Production NewEngine must retarget
        // the existing follower on its next map-thread decision cycle.
        if (productionFollowPassed && f51CombatFollowStopped &&
            f51CombatFollowResumed)
        {
            bool const handoffRequested =
                group->IsMember(nextLeaderGuid) &&
                group->GetLeaderGuid() != nextLeaderGuid;
            if (handoffRequested)
                group->ChangeLeader(nextLeaderGuid);
            bool const handedOff = group->GetLeaderGuid() == nextLeaderGuid;
            if (handedOff)
            {
                if (actionRuntime->m_partyCoordinator)
                    actionRuntime->m_partyCoordinator->InvalidateSnapshot();
                actionRuntime->m_updateTimer.Reset(1000);
                actionRuntime->UpdateAI(1000);
            }

            MovementGenerator const* handoffMovement =
                follower->GetMotionMaster()->GetCurrent();
            FollowMovementGenerator<Player> const* handoffFollow =
                handoffMovement &&
                handoffMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                    ? static_cast<FollowMovementGenerator<Player> const*>(handoffMovement)
                    : nullptr;
            f52bLeaderHandoffRetargeted =
                handoffRequested && handedOff &&
                actionRuntime->m_simulatorNewEngineTickCount == 4 &&
                actionRuntime->m_simulatorEngineDecisionCycleCount == 4 &&
                actionRuntime->m_simulatorEngineCandidateCount == 1 &&
                actionRuntime->m_simulatorLegacyPolicyEntryCount == 0 &&
                actionRuntime->m_simulatorLegacyPolicyDispatchCount == 0 &&
                handoffFollow && handoffFollow->GetTarget() == nextLeader;

            // Now promote the Bot itself. Its original owner is online and
            // remains in the Group, so PartyBotAI::GetPartyLeader restores that
            // owner as Group Leader before the NewEngine decision cycle.
            bool const promotionRequested =
                group->IsMember(followerGuid) &&
                group->GetLeaderGuid() != followerGuid;
            if (promotionRequested)
                group->ChangeLeader(followerGuid);
            bool const promotedToLeader = group->GetLeaderGuid() == followerGuid;
            if (promotedToLeader)
            {
                if (actionRuntime->m_partyCoordinator)
                    actionRuntime->m_partyCoordinator->InvalidateSnapshot();
                actionRuntime->m_updateTimer.Reset(1000);
                actionRuntime->UpdateAI(1000);
            }

            MovementGenerator const* ownerRestoreMovement =
                follower->GetMotionMaster()->GetCurrent();
            FollowMovementGenerator<Player> const* ownerRestoreFollow =
                ownerRestoreMovement &&
                ownerRestoreMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                    ? static_cast<FollowMovementGenerator<Player> const*>(ownerRestoreMovement)
                    : nullptr;
            f52bOwnerLeaderRestored =
                promotionRequested && promotedToLeader &&
                group->GetLeaderGuid() == firstLeaderGuid &&
                actionRuntime->m_simulatorNewEngineTickCount == 5 &&
                actionRuntime->m_simulatorEngineDecisionCycleCount == 5 &&
                actionRuntime->m_simulatorEngineCandidateCount == 1 &&
                actionRuntime->m_simulatorLegacyPolicyEntryCount == 0 &&
                actionRuntime->m_simulatorLegacyPolicyDispatchCount == 0 &&
                ownerRestoreFollow && ownerRestoreFollow->GetTarget() == firstLeader;

            // Restore fixture state for the following stale-Group target test.
            if (group->IsMember(firstLeaderGuid) &&
                group->GetLeaderGuid() != firstLeaderGuid)
                group->ChangeLeader(firstLeaderGuid);
            if (actionRuntime->m_partyCoordinator)
                actionRuntime->m_partyCoordinator->InvalidateSnapshot();
            follower->GetMotionMaster()->MoveFollow(
                nextLeader, 2.5f, 3.14159265358979323846f);
        }

        // The old snapshot still includes firstLeader, but the live Group no longer does.
        firstLeaderRemoved =
            group->RemoveMember(firstLeaderGuid, 0) == 2 &&
            firstLeader->GetGroup() == nullptr;
        PartyCoordinator staleCoordinator;
        PartyMemberSnapshot const* staleBot = beforeFollow.FindMember(botGuid);
        DecisionContext staleContext(
            beforeFollow, *staleBot, staleCoordinator, WorldTimer::getMSTime(),
            nullptr, nullptr, nullptr, nullptr, actionRuntime.get());
        PartyBotEngine staleEngine;
        std::shared_ptr<FollowPartyMemberAction> staleAction(
            new FollowPartyMemberAction());
        bool const staleRegistered = staleEngine.RegisterAction(staleAction);
        std::shared_ptr<Strategy> staleStrategy(
            new Strategy("simulator_follow_party_member_stale_group"));
        staleStrategy->AddTrigger(
            std::make_shared<SimulatorFollowPartyMemberTrigger>(firstGuid));
        staleEngine.AddStrategy(staleStrategy);
        DecisionReport staleReport = staleEngine.Evaluate(staleContext);
        staleSelected = staleRegistered &&
            staleReport.selectedIndices.size() == 1 &&
            staleReport.candidates[staleReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (staleSelected)
            staleEngine.ExecuteSelected(staleContext, staleReport);
        CandidateEvaluation const* staleEvaluation = staleSelected
            ? &staleReport.candidates[staleReport.selectedIndices[0]] : nullptr;
        MovementGenerator const* afterStaleMovement =
            follower->GetMotionMaster()->GetCurrent();
        FollowMovementGenerator<Player> const* afterStaleFollow =
            afterStaleMovement &&
            afterStaleMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                ? static_cast<FollowMovementGenerator<Player> const*>(afterStaleMovement)
                : nullptr;
        staleGroupRejected = firstLeaderRemoved && staleEvaluation &&
            staleEvaluation->status == CandidateStatus::ExecutionFailed &&
            staleEvaluation->actionResult == ActionResult::Impossible &&
            staleEvaluation->reason ==
                "follow_party_member_live_group_or_target_invalid" &&
            afterStaleFollow && afterStaleFollow->GetTarget() == nextLeader;
    }

    // F43: stop only the currently active native FollowMovementGenerator.
    // A changed live target must reject a stale snapshot; a matching snapshot
    // may clear FOLLOW, and a later Point move must remain untouched.
    PartyCoordinator stopSnapshotCoordinator;
    PartySnapshot const* stopSnapshotSource = PartySnapshotBuilder::GetOrBuild(
        follower, stopSnapshotCoordinator, WorldTimer::getMSTime(), 1000);
    PartyMemberSnapshot const* stopSnapshotBot = stopSnapshotSource
        ? stopSnapshotSource->FindMember(botGuid) : nullptr;
    PartySnapshot stopSnapshot;
    PartyMemberSnapshot stopBotSnapshot;
    if (stopSnapshotSource && stopSnapshotBot)
    {
        stopSnapshot = *stopSnapshotSource;
        stopBotSnapshot = *stopSnapshotBot;
        stopFollowingSnapshotCaptured =
            stopBotSnapshot.followTargetGuid == nextGuid;
    }

    PartyBotEngine stopEngine;
    std::shared_ptr<StopFollowingAction> stopAction(
        new StopFollowingAction());
    stopFollowingRegistered = stopEngine.RegisterAction(stopAction);
    std::shared_ptr<Strategy> stopStrategy(
        new Strategy("simulator_stop_following"));
    stopStrategy->AddTrigger(std::make_shared<SimulatorStopFollowingTrigger>());
    stopEngine.AddStrategy(stopStrategy);

    if (stopFollowingSnapshotCaptured && stopFollowingRegistered)
    {
        // Change the live target after the value snapshot was captured.
        follower->GetMotionMaster()->MoveFollow(
            firstLeader, 2.5f, 3.14159265358979323846f);
        DecisionContext staleStopContext(
            stopSnapshot, stopBotSnapshot, stopSnapshotCoordinator,
            WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
            actionRuntime.get());
        DecisionReport staleStopReport = stopEngine.Evaluate(staleStopContext);
        bool const staleStopSelected = staleStopReport.selectedIndices.size() == 1 &&
            staleStopReport.candidates[staleStopReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (staleStopSelected)
            stopEngine.ExecuteSelected(staleStopContext, staleStopReport);
        CandidateEvaluation const* staleStopEvaluation = staleStopSelected
            ? &staleStopReport.candidates[staleStopReport.selectedIndices[0]] : nullptr;
        MovementGenerator const* changedFollowMovement =
            follower->GetMotionMaster()->GetCurrent();
        FollowMovementGenerator<Player> const* changedFollow =
            changedFollowMovement &&
            changedFollowMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                ? static_cast<FollowMovementGenerator<Player> const*>(changedFollowMovement)
                : nullptr;
        staleStopTargetRejected = staleStopEvaluation &&
            staleStopEvaluation->status == CandidateStatus::ExecutionFailed &&
            staleStopEvaluation->actionResult == ActionResult::Impossible &&
            staleStopEvaluation->reason == "stop_following_live_target_mismatch" &&
            changedFollow && changedFollow->GetTarget() == firstLeader &&
            follower->HasUnitState(UNIT_STATE_FOLLOW);

        // Restore the target, capture a fresh value snapshot, and stop that follow.
        follower->GetMotionMaster()->MoveFollow(
            nextLeader, 2.5f, 3.14159265358979323846f);
        PartyCoordinator freshStopCoordinator;
        PartySnapshot const* freshStopSource = PartySnapshotBuilder::GetOrBuild(
            follower, freshStopCoordinator, WorldTimer::getMSTime(), 1000);
        PartyMemberSnapshot const* freshStopBot = freshStopSource
            ? freshStopSource->FindMember(botGuid) : nullptr;
        if (freshStopSource && freshStopBot &&
            freshStopBot->followTargetGuid == nextGuid)
        {
            PartySnapshot freshStopSnapshot = *freshStopSource;
            PartyMemberSnapshot freshStopBotSnapshot = *freshStopBot;
            DecisionContext stopContext(
                freshStopSnapshot, freshStopBotSnapshot, freshStopCoordinator,
                WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
                actionRuntime.get());
            DecisionReport stopReport = stopEngine.Evaluate(stopContext);
            bool const stopSelected = stopReport.selectedIndices.size() == 1 &&
                stopReport.candidates[stopReport.selectedIndices[0]].status ==
                    CandidateStatus::Selected;
            if (stopSelected)
                stopEngine.ExecuteSelected(stopContext, stopReport);
            CandidateEvaluation const* stopEvaluation = stopSelected
                ? &stopReport.candidates[stopReport.selectedIndices[0]] : nullptr;

            MotionMaster* motion = follower->GetMotionMaster();
            bool followGeneratorRemains = false;
            for (MotionMaster::const_iterator it = motion->begin();
                 it != motion->end(); ++it)
            {
                if ((*it)->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                {
                    followGeneratorRemains = true;
                    break;
                }
            }
            stoppedFollowing = stopEvaluation &&
                stopEvaluation->status == CandidateStatus::Executed &&
                stopEvaluation->actionResult == ActionResult::Success &&
                !followGeneratorRemains &&
                !follower->HasUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);

            // A stale positive follow snapshot must not replace a new Point move.
            motion->MovePoint(77, follower->GetPositionX() + 5.0f,
                              follower->GetPositionY(), follower->GetPositionZ());
            DecisionContext staleAfterStopContext(
                freshStopSnapshot, freshStopBotSnapshot, freshStopCoordinator,
                WorldTimer::getMSTime(), nullptr, nullptr, nullptr, nullptr,
                actionRuntime.get());
            DecisionReport staleAfterStopReport =
                stopEngine.Evaluate(staleAfterStopContext);
            bool const staleAfterStopSelected =
                staleAfterStopReport.selectedIndices.size() == 1 &&
                staleAfterStopReport.candidates[staleAfterStopReport.selectedIndices[0]].status ==
                    CandidateStatus::Selected;
            if (staleAfterStopSelected)
                stopEngine.ExecuteSelected(staleAfterStopContext, staleAfterStopReport);
            CandidateEvaluation const* staleAfterStopEvaluation = staleAfterStopSelected
                ? &staleAfterStopReport.candidates[
                    staleAfterStopReport.selectedIndices[0]] : nullptr;
            pointMovementPreserved = staleAfterStopEvaluation &&
                staleAfterStopEvaluation->status == CandidateStatus::ExecutionFailed &&
                staleAfterStopEvaluation->actionResult == ActionResult::Useless &&
                staleAfterStopEvaluation->reason ==
                    "stop_following_not_currently_following" &&
                motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
                !follower->HasUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
            motion->MovementExpired(false);
        }
    }

    sLog.Out(LOG_BASIC, productionFollowPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] NewEngine production follow: leaderSnapshot=%u newEngineTicks=%u legacyEntries=%u legacyDispatches=%u decisionCycles=%u candidates=%u policiesRegistered=%u shadowPolicies=%u followStarted=%u.",
        rosterValid && initialSnapshot->groupLeaderGuid == firstLeaderGuid.GetRawValue() ? 1u : 0u,
        actionRuntime->m_simulatorNewEngineTickCount,
        actionRuntime->m_simulatorLegacyPolicyEntryCount,
        actionRuntime->m_simulatorLegacyPolicyDispatchCount,
        actionRuntime->m_simulatorEngineDecisionCycleCount,
        actionRuntime->m_simulatorEngineCandidateCount,
        actionRuntime->m_newEngineStrategiesRegistered ? 1u : 0u,
        actionRuntime->m_engineShadowStrategiesRegistered ? 1u : 0u,
        productionFollowPassed ? 1u : 0u);
    if (productionFollowPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F50 real PartyBotAI::UpdateAI registered the production FollowPartyLeader strategy, selected the value-only Group leader GUID, and executed the Core FollowMovementGenerator without entering Legacy policy or registering Shadow strategies.");
    sLog.Out(LOG_BASIC, f51CombatFollowStopped && f51CombatFollowResumed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F51 behavior state: combatFollowStopped=%u combatFollowResumed=%u newEngineTicks=%u legacyEntries=%u legacyDispatches=%u.",
        f51CombatFollowStopped ? 1u : 0u, f51CombatFollowResumed ? 1u : 0u,
        actionRuntime->m_simulatorNewEngineTickCount,
        actionRuntime->m_simulatorLegacyPolicyEntryCount,
        actionRuntime->m_simulatorLegacyPolicyDispatchCount);
    sLog.Out(LOG_BASIC,
        f52bLeaderHandoffRetargeted && f52bOwnerLeaderRestored
            ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F52b leader handoff: retargeted=%u ownerLeaderRestored=%u newEngineTicks=%u decisionCycles=%u candidates=%u legacyEntries=%u legacyDispatches=%u.",
        f52bLeaderHandoffRetargeted ? 1u : 0u,
        f52bOwnerLeaderRestored ? 1u : 0u,
        actionRuntime->m_simulatorNewEngineTickCount,
        actionRuntime->m_simulatorEngineDecisionCycleCount,
        actionRuntime->m_simulatorEngineCandidateCount,
        actionRuntime->m_simulatorLegacyPolicyEntryCount,
        actionRuntime->m_simulatorLegacyPolicyDispatchCount);
    if (f52bLeaderHandoffRetargeted && f52bOwnerLeaderRestored)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F52b retargeted an existing Bot follower after Group::ChangeLeader handed leadership to another live member, then restored the original owner as Group Leader when the Bot itself was promoted and followed the restored leader through NewEngine with zero Legacy policy entry/dispatch.");
    if (f51CombatFollowStopped && f51CombatFollowResumed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F51 production state transitions stopped the native Follow when combat began and resumed Group-leader Follow after combat ended, using only NewEngine actions with zero Legacy policy entry/dispatch.");

    bool const stopFollowingPassed = stopFollowingSnapshotCaptured &&
        stopFollowingRegistered && staleStopTargetRejected &&
        stoppedFollowing && pointMovementPreserved;
    sLog.Out(LOG_BASIC, stopFollowingPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] StopFollowingAction: registered=%u snapshot=%u staleTargetRejected=%u stopped=%u pointMovementPreserved=%u.",
        stopFollowingRegistered ? 1u : 0u, stopFollowingSnapshotCaptured ? 1u : 0u,
        staleStopTargetRejected ? 1u : 0u, stoppedFollowing ? 1u : 0u,
        pointMovementPreserved ? 1u : 0u);

    bool const passed = registered && firstStarted && followTargetCaptured &&
        sameTargetNotRestarted && sameTargetFormationReapplied &&
        retargetRegistered && retargeted &&
        productionFollowPassed && f51CombatFollowStopped && f51CombatFollowResumed &&
        f52bLeaderHandoffRetargeted && f52bOwnerLeaderRestored &&
        firstLeaderRemoved && staleSelected && staleGroupRejected &&
        stopFollowingPassed;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] Core FollowPartyMember action: registered=%u firstStarted=%u followTargetCaptured=%u sameTargetNotRestarted=%u sameTargetFormationReapplied=%u retargetRegistered=%u retargeted=%u leaderHandoffRetargeted=%u ownerLeaderRestored=%u firstLeaderRemoved=%u staleSelected=%u staleGroupRejected=%u.",
        registered ? 1u : 0u, firstStarted ? 1u : 0u,
        followTargetCaptured ? 1u : 0u, sameTargetNotRestarted ? 1u : 0u,
        sameTargetFormationReapplied ? 1u : 0u,
        retargetRegistered ? 1u : 0u, retargeted ? 1u : 0u,
        f52bLeaderHandoffRetargeted ? 1u : 0u,
        f52bOwnerLeaderRestored ? 1u : 0u,
        firstLeaderRemoved ? 1u : 0u, staleSelected ? 1u : 0u,
        staleGroupRejected ? 1u : 0u);

    bool const cleaned = cleanup();
    if (!passed || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] FollowPartyMember Core fixture failed (passed=%u cleanup=%u).",
                 passed ? 1u : 0u, cleaned ? 1u : 0u);
        return false;
    }

    // Preserve the F43/F50 legacy marker consumed by the established CI harness.
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] PASS: FollowPartyMemberAction used a GUID-only current-follow snapshot to avoid restarting the same target, retargeted the native Player FollowMovementGenerator to a new group member, and rejected stale live-Group membership without disturbing movement.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] PASS: F52a reapplied the native FollowMovementGenerator when the same live Group leader required a different stable formation slot and verified the new Core offset/angle.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] PASS: StopFollowingAction rejected a stale follow-target snapshot, expired only the matching native FollowMovementGenerator through MovementExpired(false), and preserved an unrelated PointMovementGenerator.");
    return true;
}


bool RunEngineCoreFiveBotFormationSmokeTest(Map& map)
{
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    std::vector<std::unique_ptr<WorldSession>> sessions;
    std::vector<Player*> players;
    std::vector<ObjectGuid> playerGuids;
    sessions.reserve(5);
    players.reserve(5);
    playerGuids.reserve(5);

    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;
    std::vector<std::unique_ptr<PartyBotAI>> botAIs;
    botAIs.reserve(4);

    auto cleanup = [&]() -> bool
    {
        for (std::size_t index = 0; index < botAIs.size(); ++index)
        {
            if (index + 1 < players.size() && players[index + 1])
            {
                players[index + 1]->GetMotionMaster()->MoveIdle();
                players[index + 1]->SetAI(nullptr);
            }
        }
        botAIs.clear();

        // A failed lifecycle assertion must not leave a simulator Player as a
        // corpse/ghost in the shared Map or leak a resurrectable Corpse object.
        for (Player* player : players)
            if (player && !player->IsAlive())
            {
                player->ResurrectPlayer(1.0f, false);
                player->SpawnCorpseBones();
            }

        bool ungrouped = true;
        if (group && group->GetId())
        {
            // Group::Create converts the leader's temporary solo instance bind
            // into a group bind. Remove only that synthetic bind before Disband
            // so the shared simulator Map remains available to later fixtures.
            group->UnbindInstance(map.GetId(), false);
            group->Disband(true, group->GetLeaderGuid());
            for (Player* player : players)
                if (player && player->GetGroup())
                    ungrouped = false;
            if (groupRegistered)
            {
                sObjectMgr.RemoveGroup(group.get());
                groupRegistered = false;
            }
            group.reset();
        }

        bool sessionsCleaned = true;
        for (std::unique_ptr<WorldSession>& session : sessions)
        {
            if (session && session->GetPlayer())
                session->LogoutPlayer(false);
            if (session && session->GetPlayer())
                sessionsCleaned = false;
        }

        bool playersDetached = true;
        for (ObjectGuid const& guid : playerGuids)
            if (map.GetUnit(guid))
                playersDetached = false;

        players.clear();
        sessions.clear();
        return ungrouped && sessionsCleaned && playersDetached;
    };

    bool playersCreated = true;
    for (uint32 index = 0; index < 5; ++index)
    {
        std::unique_ptr<WorldSession> session(
            new WorldSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS));
        Player* player = nullptr;
        bool const created = CreateSimulatorPlayer(
            map, partyFaction, *session, player,
            static_cast<float>(index) * 2.5f, 34.0f, 0.0f);
        sessions.push_back(std::move(session));
        if (player)
        {
            players.push_back(player);
            playerGuids.push_back(player->GetObjectGuid());
        }
        playersCreated = playersCreated && created && player && player->IsInWorld();
        if (!playersCreated)
            break;
    }

    if (!playersCreated || players.size() != 5)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] F52c five-Player creation failed (created=%u count=%u cleanup=%u).",
            playersCreated ? 1u : 0u, static_cast<uint32>(players.size()),
            cleaned ? 1u : 0u);
        return false;
    }

    bool const groupCreated = group->Create(
        players[0]->GetObjectGuid(), players[0]->GetName());
    if (groupCreated)
    {
        sObjectMgr.AddGroup(group.get());
        groupRegistered = true;
    }
    bool membersAdded = groupCreated && group->GetMembersCount() == 1 &&
        players[0]->GetGroup() == group.get();
    for (std::size_t index = 1; index < players.size() && membersAdded; ++index)
        membersAdded = group->AddMember(
            players[index]->GetObjectGuid(), players[index]->GetName()) &&
            players[index]->GetGroup() == group.get();

    if (!membersAdded || group->GetMembersCount() != 5 ||
        group->GetLeaderGuid() != players[0]->GetObjectGuid())
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] F52c five-Player Group setup failed (created=%u members=%u cleanup=%u).",
            groupCreated ? 1u : 0u, group ? group->GetMembersCount() : 0u,
            cleaned ? 1u : 0u);
        return false;
    }

    Player* const leader = players[0];
    for (std::size_t index = 1; index < players.size(); ++index)
    {
        Player* const bot = players[index];
        std::unique_ptr<PartyBotAI> ai(new PartyBotAI(
            leader, map.GetId(), map.GetInstanceId(),
            bot->GetPositionX(), bot->GetPositionY(),
            bot->GetPositionZ(), bot->GetOrientation()));
        ai->SetPlayer(bot);
        ai->m_initialized = true;
        ai->m_updateTimer.Reset(600000);
        ai->m_simulatorExecutionAuthorityOverride =
            static_cast<int32>(ExecutionAuthorityMode::NewEngine);
        bot->SetAI(ai.get());
        botAIs.push_back(std::move(ai));
    }

    bool usedSlots[MaxPartyFormationSlots] = {};
    std::uint8_t slotByBot[4] = {
        InvalidFormationSlot, InvalidFormationSlot,
        InvalidFormationSlot, InvalidFormationSlot
    };
    std::shared_ptr<PartyCoordinator> sharedCoordinator;
    bool sharedCoordinatorForAll = true;
    bool allBotsStartedFollow = true;
    bool allSlotsUnique = true;
    bool allOffsetsMatch = true;
    bool allNewEngineOnly = true;
    bool allSnapshotsAreFiveMembers = true;

    // First pass in roster order. All four AIs already exist before the first
    // snapshot, so the shared coordinator assigns the complete five-member roster.
    for (std::size_t index = 0; index < botAIs.size(); ++index)
    {
        PartyBotAI& ai = *botAIs[index];
        Player* const bot = players[index + 1];
        ai.m_updateTimer.Reset(1000);
        ai.UpdateAI(1000);

        std::shared_ptr<PartyCoordinator> coordinator = ai.m_partyCoordinator;
        if (!sharedCoordinator)
            sharedCoordinator = coordinator;
        else
            sharedCoordinatorForAll = sharedCoordinatorForAll &&
                coordinator && coordinator.get() == sharedCoordinator.get();

        if (!coordinator)
        {
            allBotsStartedFollow = false;
            allSlotsUnique = false;
            allOffsetsMatch = false;
            allNewEngineOnly = false;
            allSnapshotsAreFiveMembers = false;
            continue;
        }

        Guid const botGuid = bot->GetObjectGuid().GetRawValue();
        std::uint8_t const slot = coordinator->GetFormationSlot(botGuid);
        slotByBot[index] = slot;
        bool const slotValid = slot < MaxPartyFormationSlots;
        if (slotValid)
        {
            if (usedSlots[slot])
                allSlotsUnique = false;
            usedSlots[slot] = true;
        }
        else
            allSlotsUnique = false;

        MovementGenerator const* current =
            bot->GetMotionMaster()->GetCurrent();
        FollowMovementGenerator<Player> const* follow =
            current && current->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                ? static_cast<FollowMovementGenerator<Player> const*>(current)
                : nullptr;
        allBotsStartedFollow = allBotsStartedFollow && follow &&
            follow->GetTarget() == leader &&
            bot->HasUnitState(UNIT_STATE_FOLLOW);

        if (follow && slotValid)
        {
            FollowFormationOffset const expected = GetFollowFormationOffset(slot);
            allOffsetsMatch = allOffsetsMatch &&
                std::fabs(follow->GetFollowDistance() - expected.distance) < 0.0001f &&
                std::fabs(follow->GetFollowAngle() - expected.angle) < 0.0001f;
        }
        else
            allOffsetsMatch = false;

        allNewEngineOnly = allNewEngineOnly &&
            ai.m_simulatorNewEngineTickCount == 1 &&
            ai.m_simulatorEngineDecisionCycleCount == 1 &&
            ai.m_simulatorEngineCandidateCount == 1 &&
            ai.m_simulatorLegacyPolicyEntryCount == 0 &&
            ai.m_simulatorLegacyPolicyDispatchCount == 0 &&
            ai.m_newEngineStrategiesRegistered &&
            !ai.m_engineShadowStrategiesRegistered;

        PartySnapshot const* snapshot = coordinator->GetSnapshotIfFresh(
            WorldTimer::getMSTime(), 1000);
        allSnapshotsAreFiveMembers = allSnapshotsAreFiveMembers &&
            snapshot && snapshot->groupId == group->GetId() &&
            snapshot->groupLeaderGuid == leader->GetObjectGuid().GetRawValue() &&
            snapshot->members.size() == 5;
    }

    bool const allFourSlotsOccupied =
        usedSlots[0] && usedSlots[1] && usedSlots[2] && usedSlots[3];

    // Second pass in reverse Bot order. Each Bot must observe the same stable
    // assignment and leave its already-correct native Follow generator untouched.
    bool reverseTickNoOps = sharedCoordinatorForAll && sharedCoordinator;
    for (std::size_t offset = 0; offset < botAIs.size(); ++offset)
    {
        std::size_t const index = botAIs.size() - 1 - offset;
        PartyBotAI& ai = *botAIs[index];
        Player* const bot = players[index + 1];
        sharedCoordinator->InvalidateSnapshot();
        MovementGenerator const* before = bot->GetMotionMaster()->GetCurrent();
        std::uint8_t const slotBefore = sharedCoordinator->GetFormationSlot(
            bot->GetObjectGuid().GetRawValue());
        ai.m_updateTimer.Reset(1000);
        ai.UpdateAI(1000);
        MovementGenerator const* after = bot->GetMotionMaster()->GetCurrent();
        FollowMovementGenerator<Player> const* follow =
            after && after->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                ? static_cast<FollowMovementGenerator<Player> const*>(after)
                : nullptr;
        reverseTickNoOps = reverseTickNoOps &&
            ai.m_partyCoordinator &&
            ai.m_partyCoordinator.get() == sharedCoordinator.get() &&
            ai.m_simulatorNewEngineTickCount == 2 &&
            ai.m_simulatorEngineDecisionCycleCount == 2 &&
            ai.m_simulatorEngineCandidateCount == 0 &&
            ai.m_simulatorLegacyPolicyEntryCount == 0 &&
            ai.m_simulatorLegacyPolicyDispatchCount == 0 &&
            before == after && slotBefore == slotByBot[index] &&
            follow && follow->GetTarget() == leader &&
            bot->HasUnitState(UNIT_STATE_FOLLOW);
    }

    // F52d: exercise real Group departure/rejoin against the shared production
    // coordinator. Removing one Bot must release only its slot; the remaining
    // three assignments survive. Re-adding that same Player must reclaim the
    // lowest vacant slot on the next NewEngine snapshot.
    bool departureReleasedSlot = false;
    bool incumbentsKeptSlots = false;
    bool rejoinReclaimedSlot = false;
    Player* const departingBot = players[1];
    ObjectGuid const departingGuid = departingBot->GetObjectGuid();
    std::uint8_t const departingSlot = slotByBot[0];
    bool const removedFromGroup = departingSlot == 0 &&
        group->RemoveMember(departingGuid, 0) == 4 &&
        departingBot->GetGroup() == nullptr;
    if (removedFromGroup)
    {
        sharedCoordinator->InvalidateSnapshot();
        PartyBotAI& remainingAI = *botAIs[1];
        remainingAI.m_updateTimer.Reset(1000);
        remainingAI.UpdateAI(1000);
        departureReleasedSlot =
            sharedCoordinator->GetFormationSlot(departingGuid.GetRawValue()) ==
                InvalidFormationSlot;
        incumbentsKeptSlots =
            sharedCoordinator->GetFormationSlot(players[2]->GetObjectGuid().GetRawValue()) == slotByBot[1] &&
            sharedCoordinator->GetFormationSlot(players[3]->GetObjectGuid().GetRawValue()) == slotByBot[2] &&
            sharedCoordinator->GetFormationSlot(players[4]->GetObjectGuid().GetRawValue()) == slotByBot[3] &&
            remainingAI.m_simulatorLegacyPolicyEntryCount == 0 &&
            remainingAI.m_simulatorLegacyPolicyDispatchCount == 0;

        bool const addedBack = group->AddMember(departingGuid, departingBot->GetName()) &&
            departingBot->GetGroup() == group.get() && group->GetMembersCount() == 5;
        if (addedBack)
        {
            // Group roster mutations do not directly notify this shared cache.
            // Force the next NewEngine tick to build a fresh five-member snapshot;
            // otherwise the 250 ms cache can still contain the just-built
            // four-member roster inside this synchronous simulator fixture.
            sharedCoordinator->InvalidateSnapshot();
            PartyBotAI& returningAI = *botAIs[0];
            returningAI.m_updateTimer.Reset(1000);
            returningAI.UpdateAI(1000);
            MovementGenerator const* current = departingBot->GetMotionMaster()->GetCurrent();
            FollowMovementGenerator<Player> const* follow =
                current && current->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                    ? static_cast<FollowMovementGenerator<Player> const*>(current)
                    : nullptr;
            rejoinReclaimedSlot =
                returningAI.m_partyCoordinator &&
                returningAI.m_partyCoordinator.get() == sharedCoordinator.get() &&
                sharedCoordinator->GetFormationSlot(departingGuid.GetRawValue()) == departingSlot &&
                sharedCoordinator->GetFormationSlot(players[2]->GetObjectGuid().GetRawValue()) == slotByBot[1] &&
                sharedCoordinator->GetFormationSlot(players[3]->GetObjectGuid().GetRawValue()) == slotByBot[2] &&
                sharedCoordinator->GetFormationSlot(players[4]->GetObjectGuid().GetRawValue()) == slotByBot[3] &&
                follow && follow->GetTarget() == leader &&
                returningAI.m_simulatorLegacyPolicyEntryCount == 0 &&
                returningAI.m_simulatorLegacyPolicyDispatchCount == 0;
        }
    }

    // F52e: move the same real five-Player Group to a different Vanilla
    // dungeon Map and drive all four production NewEngine PartyBotAI ticks.
    // This is deliberately stronger than the older F27 Shadow regroup fixture:
    // every Bot must discard its source shard, share one fresh destination
    // coordinator/snapshot, receive a unique slot and resume native Follow.
    static uint32 constexpr F52E_DESTINATION_MAP_ID = 34; // Stockades.
    MapEntry const* regroupEntry =
        sMapStorage.LookupEntry<MapEntry>(F52E_DESTINATION_MAP_ID);
    bool const regroupMapAvailable = regroupEntry && regroupEntry->IsDungeon() &&
        group->GetBoundInstance(F52E_DESTINATION_MAP_ID) == nullptr;
    Map* regroupMap = regroupMapAvailable
        ? sMapMgr.CreateTestMap(F52E_DESTINATION_MAP_ID, true, 0.0f, 0.0f)
        : nullptr;
    bool regroupSourceReleased = false;
    bool regroupDestinationReady = false;
    bool regroupSharedCoordinator = false;
    bool regroupFreshSnapshot = false;
    bool regroupSlotsUnique = false;
    bool regroupFollowsRestored = false;
    bool regroupNewEngineOnly = false;
    bool regroupPlayersRestored = false;
    bool regroupBindReleased = false;
    if (regroupMap && regroupMap->GetId() != map.GetId() &&
        regroupMap->GetInstanceId() && regroupMap->GetInstanceId() != map.GetInstanceId())
    {
        bool allOnSource = std::all_of(players.begin(), players.end(),
            [&map](Player const* member)
            {
                return member && member->IsInWorld() && member->GetMap() == &map;
            });
        if (allOnSource)
        {
            for (Player* member : players)
                map.Remove(member, false);
            bool allDetached = std::all_of(players.begin(), players.end(),
                [](Player const* member) { return member && !member->IsInWorld(); });
            regroupSourceReleased = allDetached;
            for (std::size_t index = 0; index < botAIs.size(); ++index)
                regroupSourceReleased = regroupSourceReleased &&
                    !botAIs[index]->m_partyCoordinator &&
                    botAIs[index]->m_partyCoordinatorMapId == 0 &&
                    botAIs[index]->m_partyCoordinatorInstanceId == 0;

            bool allAdded = allDetached;
            if (allDetached)
                for (Player* member : players)
                    if (!regroupMap->Add(member))
                        allAdded = false;
            regroupDestinationReady = allAdded &&
                std::all_of(players.begin(), players.end(),
                    [regroupMap](Player const* member)
                    {
                        return member && member->IsInWorld() &&
                            member->GetMap() == regroupMap &&
                            member->GetMapId() == regroupMap->GetId() &&
                            member->GetInstanceId() == regroupMap->GetInstanceId();
                    });

            std::shared_ptr<PartyCoordinator> destinationCoordinator;
            bool usedDestinationSlots[MaxPartyFormationSlots] = {};
            regroupSharedCoordinator = regroupDestinationReady;
            regroupSlotsUnique = regroupDestinationReady;
            regroupFollowsRestored = regroupDestinationReady;
            regroupNewEngineOnly = regroupDestinationReady;
            if (regroupDestinationReady)
            {
                for (std::size_t index = 0; index < botAIs.size(); ++index)
                {
                    PartyBotAI& ai = *botAIs[index];
                    Player* const bot = players[index + 1];
                    ai.m_updateTimer.Reset(1000);
                    ai.UpdateAI(1000);
                    if (!destinationCoordinator)
                        destinationCoordinator = ai.m_partyCoordinator;
                    regroupSharedCoordinator = regroupSharedCoordinator &&
                        ai.m_partyCoordinator &&
                        ai.m_partyCoordinator.get() == destinationCoordinator.get() &&
                        destinationCoordinator.get() != sharedCoordinator.get() &&
                        ai.m_partyCoordinatorMapId == regroupMap->GetId() &&
                        ai.m_partyCoordinatorInstanceId == regroupMap->GetInstanceId();

                    std::uint8_t const slot = destinationCoordinator
                        ? destinationCoordinator->GetFormationSlot(
                            bot->GetObjectGuid().GetRawValue())
                        : InvalidFormationSlot;
                    bool const slotValid = slot < MaxPartyFormationSlots;
                    if (slotValid)
                    {
                        if (usedDestinationSlots[slot])
                            regroupSlotsUnique = false;
                        usedDestinationSlots[slot] = true;
                    }
                    else
                        regroupSlotsUnique = false;

                    MovementGenerator const* current = bot->GetMotionMaster()->GetCurrent();
                    FollowMovementGenerator<Player> const* follow =
                        current && current->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                            ? static_cast<FollowMovementGenerator<Player> const*>(current)
                            : nullptr;
                    regroupFollowsRestored = regroupFollowsRestored && follow &&
                        follow->GetTarget() == players[0] &&
                        bot->HasUnitState(UNIT_STATE_FOLLOW);
                    if (follow && slotValid)
                    {
                        FollowFormationOffset const expected = GetFollowFormationOffset(slot);
                        regroupFollowsRestored = regroupFollowsRestored &&
                            std::fabs(follow->GetFollowDistance() - expected.distance) < 0.0001f &&
                            std::fabs(follow->GetFollowAngle() - expected.angle) < 0.0001f;
                    }
                    else
                        regroupFollowsRestored = false;

                    regroupNewEngineOnly = regroupNewEngineOnly &&
                        ai.m_simulatorLegacyPolicyEntryCount == 0 &&
                        ai.m_simulatorLegacyPolicyDispatchCount == 0 &&
                        ai.m_newEngineStrategiesRegistered &&
                        !ai.m_engineShadowStrategiesRegistered;
                }

                regroupSlotsUnique = regroupSlotsUnique &&
                    usedDestinationSlots[0] && usedDestinationSlots[1] &&
                    usedDestinationSlots[2] && usedDestinationSlots[3];
                PartySnapshot const* destinationSnapshot = destinationCoordinator
                    ? destinationCoordinator->GetSnapshotIfFresh(WorldTimer::getMSTime(), 1000)
                    : nullptr;
                regroupFreshSnapshot = destinationSnapshot &&
                    destinationSnapshot->groupId == group->GetId() &&
                    destinationSnapshot->mapId == regroupMap->GetId() &&
                    destinationSnapshot->instanceId == regroupMap->GetInstanceId() &&
                    destinationSnapshot->groupLeaderGuid == leader->GetObjectGuid().GetRawValue() &&
                    destinationSnapshot->members.size() == 5;
                if (regroupFreshSnapshot)
                    for (Player* member : players)
                    {
                        PartyMemberSnapshot const* row = destinationSnapshot->FindMember(
                            member->GetObjectGuid().GetRawValue());
                        regroupFreshSnapshot = regroupFreshSnapshot && row &&
                            row->online && row->mapId == regroupMap->GetId() &&
                            row->instanceId == regroupMap->GetInstanceId();
                    }
            }

            // Always restore every Player before releasing the temporary dungeon
            // bind or deleting the destination map, including partial-transfer cases.
            for (Player* member : players)
                if (member && member->IsInWorld() && member->GetMap() == regroupMap)
                    regroupMap->Remove(member, false);
            bool allDetachedFromDestination = std::all_of(
                players.begin(), players.end(),
                [regroupMap](Player const* member)
                {
                    return member && (!member->IsInWorld() || member->GetMap() != regroupMap);
                });
            if (allDetachedFromDestination &&
                group->GetBoundInstance(regroupMap->GetId()))
                group->UnbindInstance(regroupMap->GetId());
            regroupBindReleased = group->GetBoundInstance(regroupMap->GetId()) == nullptr;

            regroupPlayersRestored = allDetachedFromDestination;
            for (Player* member : players)
            {
                if (!member)
                {
                    regroupPlayersRestored = false;
                    continue;
                }
                if (!member->IsInWorld())
                {
                    if (!map.Add(member))
                        regroupPlayersRestored = false;
                }
                else if (member->GetMap() != &map)
                    regroupPlayersRestored = false;
            }
            regroupPlayersRestored = regroupPlayersRestored &&
                std::all_of(players.begin(), players.end(),
                    [&map](Player const* member)
                    {
                        return member && member->IsInWorld() && member->GetMap() == &map &&
                            member->GetMapId() == map.GetId() &&
                            member->GetInstanceId() == map.GetInstanceId();
                    });
        }
    }
    if (regroupMap)
    {
        bool const mapEmpty = std::all_of(players.begin(), players.end(),
            [regroupMap](Player const* member)
            {
                return !member || !member->IsInWorld() || member->GetMap() != regroupMap;
            });
        if (mapEmpty && group->GetBoundInstance(regroupMap->GetId()))
            group->UnbindInstance(regroupMap->GetId());
        regroupBindReleased = regroupBindReleased &&
            group->GetBoundInstance(regroupMap->GetId()) == nullptr;
        if (mapEmpty && regroupBindReleased)
            sMapMgr.DeleteTestMap(regroupMap);
        else
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[PartyBotSimulator] Refusing to delete F52e destination Map %u instance %u (empty=%u bindReleased=%u).",
                regroupMap->GetId(), regroupMap->GetInstanceId(),
                mapEmpty ? 1u : 0u, regroupBindReleased ? 1u : 0u);
    }
    bool const f52ePassed = regroupSourceReleased && regroupDestinationReady &&
        regroupSharedCoordinator && regroupFreshSnapshot && regroupSlotsUnique &&
        regroupFollowsRestored && regroupNewEngineOnly && regroupPlayersRestored &&
        regroupBindReleased;
    sLog.Out(LOG_BASIC, f52ePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F52e five-Bot cross-map regroup: sourceReleased=%u destinationReady=%u sharedCoordinator=%u freshFiveMemberSnapshot=%u uniqueSlots=%u nativeFollows=%u newEngineOnly=%u playersRestored=%u groupBindReleased=%u.",
        regroupSourceReleased ? 1u : 0u, regroupDestinationReady ? 1u : 0u,
        regroupSharedCoordinator ? 1u : 0u, regroupFreshSnapshot ? 1u : 0u,
        regroupSlotsUnique ? 1u : 0u, regroupFollowsRestored ? 1u : 0u,
        regroupNewEngineOnly ? 1u : 0u, regroupPlayersRestored ? 1u : 0u,
        regroupBindReleased ? 1u : 0u);
    if (f52ePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F52e transferred a real five-Player Group to a distinct Vanilla dungeon Map; all four PartyBotAI instances released the source shard, reacquired one shared destination coordinator and five-member snapshot, received unique formation slots, resumed exact native Follow offsets through NewEngine only, and the fixture restored all Players and released its temporary GroupBind.");

    // F52f: drive party-wide wipe detection and recovery through the production
    // PartyBotAI -> PartySnapshotBuilder -> PartyCoordinator update path using
    // the same five real Core Players and four native Follow generators.
    bool const f52fSourceReady = std::all_of(players.begin(), players.end(),
        [&map](Player const* member)
        {
            return member && member->IsInWorld() && member->GetMap() == &map &&
                member->GetMapId() == map.GetId() &&
                member->GetInstanceId() == map.GetInstanceId();
        });
    std::shared_ptr<PartyCoordinator> wipeCoordinator;
    bool f52fEncounterStarted = false;
    bool f52fReservationsSeeded = false;
    bool f52fPartialDeathSafe = false;
    bool f52fWipeClosed = false;
    bool f52fReservationsCleared = false;
    bool f52fRepeatedWipeIdempotent = false;
    bool f52fFollowStoppedOnDeath = false;
    bool f52fDeadStateObserved = false;
    bool f52fResurrectionRecovered = false;
    bool f52fNoFalseEncounterRestart = false;
    bool partialSnapshotAvailable = false;
    uint32 partialOnline = 0;
    uint32 partialLiving = 0;
    uint32 partialNormalReservationCount = 0;
    uint32 partialThreatReservationCount = 0;
    if (f52fSourceReady && sharedCoordinator &&
        !sharedCoordinator->HasActiveEncounter())
    {
        // Resolve the actual AI attached to the selected death victim and
        // choose a different living Bot as the observer/driver. After the
        // cross-map fixture, every AI must reacquire the source shard first;
        // a single driver tick must not be mistaken for party-wide ownership.
        Player* const deadBot = players[4];
        PartyBotAI* const deadBotAI =
            deadBot ? dynamic_cast<PartyBotAI*>(deadBot->AI()) : nullptr;
        PartyBotAI* driverAI = nullptr;
        Player* driverBot = nullptr;
        for (std::size_t index = 0; index < botAIs.size(); ++index)
        {
            if (botAIs[index].get() == deadBotAI)
                continue;
            driverAI = botAIs[index].get();
            driverBot = players[index + 1];
            break;
        }

        // Retain the source coordinator while every Bot reacquires it after
        // the cross-map transfer, then drive the encounter using a Bot other
        // than the selected death victim.
        wipeCoordinator = sharedCoordinator;
        leader->SetInCombatState();
        bool allSourceCoordinatorsReacquired = deadBotAI && driverAI && driverBot;
        for (std::size_t index = 0; index < botAIs.size(); ++index)
        {
            PartyBotAI& ai = *botAIs[index];
            wipeCoordinator->InvalidateSnapshot();
            ai.m_updateTimer.Reset(1000);
            ai.UpdateAI(1000);
            allSourceCoordinatorsReacquired = allSourceCoordinatorsReacquired &&
                ai.m_partyCoordinator &&
                ai.m_partyCoordinator.get() == sharedCoordinator.get() &&
                ai.m_partyCoordinatorMapId == map.GetId() &&
                ai.m_partyCoordinatorInstanceId == map.GetInstanceId();
        }
        f52fEncounterStarted = allSourceCoordinatorsReacquired && wipeCoordinator &&
            wipeCoordinator.get() == sharedCoordinator.get() &&
            wipeCoordinator->HasActiveEncounter();

        if (f52fEncounterStarted)
        {
            uint32 const nowMs = WorldTimer::getMSTime();
            Guid const survivorGuid = driverBot->GetObjectGuid().GetRawValue();
            Guid const deadBotGuid = deadBot->GetObjectGuid().GetRawValue();
            Guid const leaderGuid = leader->GetObjectGuid().GetRawValue();
            bool ordinaryReserved = true;
            // Seed one reservation for the Bot about to die, one for a survivor,
            // and one orphaned logical owner which only the party-wide wipe may
            // remove. This distinguishes per-Bot death cleanup from shard wipe.
            Reservation reservation;
            reservation.targetGuid = leaderGuid;
            reservation.ownerGuid = deadBotGuid;
            reservation.expiresAtMs = nowMs + 60000u;
            ordinaryReserved = wipeCoordinator->TryReserve(reservation, nowMs) && ordinaryReserved;
            reservation.targetGuid = players[2]->GetObjectGuid().GetRawValue();
            reservation.ownerGuid = survivorGuid;
            ordinaryReserved = wipeCoordinator->TryReserve(reservation, nowMs) && ordinaryReserved;
            reservation.targetGuid = players[3]->GetObjectGuid().GetRawValue();
            reservation.ownerGuid = leaderGuid;
            ordinaryReserved = wipeCoordinator->TryReserve(reservation, nowMs) && ordinaryReserved;

            bool threatReserved = true;
            auto reserveThreat = [&](Guid actionOwner, Guid threatOwner)
            {
                ThreatReservation threatReservation;
                threatReservation.ownerGuid = actionOwner;
                threatReservation.threatOwnerGuid = threatOwner;
                threatReservation.hostileGuid = leaderGuid;
                threatReservation.actionId = wipeCoordinator->NextActionId();
                threatReservation.threatPoints = 25.0f;
                threatReservation.expiresAtMs = nowMs + 60000u;
                return wipeCoordinator->TryReserveThreatImpact(threatReservation, nowMs);
            };
            threatReserved = reserveThreat(deadBotGuid, deadBotGuid) && threatReserved;
            threatReserved = reserveThreat(survivorGuid, survivorGuid) && threatReserved;
            threatReserved = reserveThreat(leaderGuid, leaderGuid) && threatReserved;
            f52fReservationsSeeded = ordinaryReserved && threatReserved &&
                wipeCoordinator->GetReservations().size() == 3 &&
                wipeCoordinator->GetThreatReservationCount() == 3;

            // One dead Bot is not a wipe: the other four online members live.
            deadBot->SetDeathState(JUST_DIED);
            wipeCoordinator->InvalidateSnapshot();
            driverAI->m_updateTimer.Reset(1000);
            driverAI->UpdateAI(1000);
            PartySnapshot const* partialSnapshot = wipeCoordinator->GetSnapshotIfFresh(
                WorldTimer::getMSTime(), 1000);
            partialSnapshotAvailable = partialSnapshot != nullptr;
            if (partialSnapshot)
                for (PartyMemberSnapshot const& member : partialSnapshot->members)
                {
                    if (!member.online)
                        continue;
                    ++partialOnline;
                    if (member.alive)
                        ++partialLiving;
                }
            partialNormalReservationCount =
                static_cast<uint32>(wipeCoordinator->GetReservations().size());
            partialThreatReservationCount =
                static_cast<uint32>(wipeCoordinator->GetThreatReservationCount());
            bool const survivingReservationPresent =
                wipeCoordinator->GetReservations().size() == 2 &&
                std::any_of(wipeCoordinator->GetReservations().begin(),
                    wipeCoordinator->GetReservations().end(),
                    [survivorGuid](Reservation const& row) { return row.ownerGuid == survivorGuid; }) &&
                std::any_of(wipeCoordinator->GetReservations().begin(),
                    wipeCoordinator->GetReservations().end(),
                    [leaderGuid](Reservation const& row) { return row.ownerGuid == leaderGuid; }) &&
                std::none_of(wipeCoordinator->GetReservations().begin(),
                    wipeCoordinator->GetReservations().end(),
                    [deadBotGuid](Reservation const& row) { return row.ownerGuid == deadBotGuid; });
            uint32 const nowAfterPartial = WorldTimer::getMSTime();
            bool const survivingThreatReservationPresent =
                wipeCoordinator->GetThreatReservationCount() == 2 &&
                wipeCoordinator->GetReservedThreatImpact(leaderGuid, survivorGuid, 0,
                    nowAfterPartial) > 0.0f &&
                wipeCoordinator->GetReservedThreatImpact(leaderGuid, leaderGuid, 0,
                    nowAfterPartial) > 0.0f &&
                wipeCoordinator->GetReservedThreatImpact(leaderGuid, deadBotGuid, 0,
                    nowAfterPartial) == 0.0f;
            f52fPartialDeathSafe = partialSnapshotAvailable && partialOnline == 5 &&
                partialLiving == 4 && wipeCoordinator->HasActiveEncounter() &&
                survivingReservationPresent && survivingThreatReservationPresent;

            // The remaining four deaths create a true party wipe. Core's
            // JUST_DIED transition also clears every bot's native movement.
            for (Player* member : players)
                if (member && member->IsAlive())
                    member->SetDeathState(JUST_DIED);
            bool allDead = std::all_of(players.begin(), players.end(),
                [](Player const* member) { return member && !member->IsAlive(); });
            wipeCoordinator->InvalidateSnapshot();
            driverAI->m_updateTimer.Reset(1000);
            driverAI->UpdateAI(1000);
            f52fWipeClosed = allDead && !wipeCoordinator->HasActiveEncounter();
            f52fReservationsCleared = f52fWipeClosed &&
                wipeCoordinator->GetReservations().empty() &&
                wipeCoordinator->GetThreatReservationCount() == 0;

            // Tick every dead Bot so its behavior state machine observes Dead;
            // further dead snapshots must not recreate or re-close an encounter.
            f52fDeadStateObserved = allDead;
            for (std::size_t index = 0; index < botAIs.size(); ++index)
            {
                PartyBotAI& ai = *botAIs[index];
                Player* const bot = players[index + 1];
                wipeCoordinator->InvalidateSnapshot();
                ai.m_updateTimer.Reset(1000);
                ai.UpdateAI(1000);
                bool const noFollowGenerator =
                    bot->GetMotionMaster()->GetCurrentMovementGeneratorType() !=
                        FOLLOW_MOTION_TYPE &&
                    !bot->HasUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
                f52fDeadStateObserved = f52fDeadStateObserved &&
                    ai.m_partyCoordinator &&
                    ai.m_partyCoordinator.get() == wipeCoordinator.get() &&
                    ai.m_botBehaviorStateMachine.GetState() == BotBehaviorState::Corpse &&
                    noFollowGenerator &&
                    ai.m_simulatorLegacyPolicyEntryCount == 0 &&
                    ai.m_simulatorLegacyPolicyDispatchCount == 0;
            }
            f52fFollowStoppedOnDeath = f52fDeadStateObserved &&
                std::all_of(botAIs.begin(), botAIs.end(),
                    [](std::unique_ptr<PartyBotAI> const& ai)
                    {
                        return ai->m_partyBotEngine.GetPendingActionCount() == 0 &&
                            ai->m_partyBotEngine.GetPendingContinuationCount() == 0 &&
                            ai->m_partyBotEngine.GetPendingItemUseCount() == 0;
                    });
            f52fRepeatedWipeIdempotent = f52fWipeClosed &&
                !wipeCoordinator->HasActiveEncounter() &&
                wipeCoordinator->GetReservations().empty() &&
                wipeCoordinator->GetThreatReservationCount() == 0;

            // Resurrect the whole Group. With no member in combat, recovery
            // must rebuild the same four slots and native leader Follow only.
            for (Player* member : players)
                member->ResurrectPlayer(0.5f, false);
            f52fResurrectionRecovered = true;
            bool recoveredSlots[MaxPartyFormationSlots] = {};
            for (std::size_t index = 0; index < botAIs.size(); ++index)
            {
                PartyBotAI& ai = *botAIs[index];
                Player* const bot = players[index + 1];
                wipeCoordinator->InvalidateSnapshot();
                ai.m_updateTimer.Reset(1000);
                ai.UpdateAI(1000);
                // A second tick observes the native Follow created by the first
                // recovery decision and must settle the state machine at FollowingLeader.
                wipeCoordinator->InvalidateSnapshot();
                ai.m_updateTimer.Reset(1000);
                ai.UpdateAI(1000);

                uint8 const slot = wipeCoordinator->GetFormationSlot(
                    bot->GetObjectGuid().GetRawValue());
                bool const slotValid = slot < MaxPartyFormationSlots;
                if (slotValid)
                {
                    if (recoveredSlots[slot])
                        f52fResurrectionRecovered = false;
                    recoveredSlots[slot] = true;
                }
                MovementGenerator const* current = bot->GetMotionMaster()->GetCurrent();
                FollowMovementGenerator<Player> const* follow =
                    current && current->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                        ? static_cast<FollowMovementGenerator<Player> const*>(current)
                        : nullptr;
                f52fResurrectionRecovered = f52fResurrectionRecovered &&
                    bot->IsAlive() && ai.m_partyCoordinator &&
                    ai.m_partyCoordinator.get() == wipeCoordinator.get() &&
                    slotValid && slot == slotByBot[index] &&
                    follow && follow->GetTarget() == leader &&
                    bot->HasUnitState(UNIT_STATE_FOLLOW) &&
                    ai.m_botBehaviorStateMachine.GetState() == BotBehaviorState::FollowingLeader;
                if (follow && slotValid)
                {
                    FollowFormationOffset const expected = GetFollowFormationOffset(slot);
                    f52fResurrectionRecovered = f52fResurrectionRecovered &&
                        std::fabs(follow->GetFollowDistance() - expected.distance) < 0.0001f &&
                        std::fabs(follow->GetFollowAngle() - expected.angle) < 0.0001f;
                }
                else
                    f52fResurrectionRecovered = false;
                f52fResurrectionRecovered = f52fResurrectionRecovered &&
                    ai.m_simulatorLegacyPolicyEntryCount == 0 &&
                    ai.m_simulatorLegacyPolicyDispatchCount == 0 &&
                    ai.m_newEngineStrategiesRegistered &&
                    !ai.m_engineShadowStrategiesRegistered;
            }
            f52fResurrectionRecovered = f52fResurrectionRecovered &&
                recoveredSlots[0] && recoveredSlots[1] &&
                recoveredSlots[2] && recoveredSlots[3];

            f52fNoFalseEncounterRestart =
                !wipeCoordinator->HasActiveEncounter() &&
                wipeCoordinator->GetReservations().empty() &&
                wipeCoordinator->GetThreatReservationCount() == 0;
            PartySnapshot const* recoveredSnapshot = wipeCoordinator->GetSnapshotIfFresh(
                WorldTimer::getMSTime(), 1000);
            f52fNoFalseEncounterRestart = f52fNoFalseEncounterRestart &&
                recoveredSnapshot && recoveredSnapshot->members.size() == 5 &&
                std::all_of(recoveredSnapshot->members.begin(), recoveredSnapshot->members.end(),
                    [](PartyMemberSnapshot const& member)
                    {
                        return member.online && member.alive && !member.inCombat;
                    });
        }
    }

    bool const f52fPassed = f52fEncounterStarted && f52fReservationsSeeded &&
        f52fPartialDeathSafe && f52fWipeClosed && f52fReservationsCleared &&
        f52fRepeatedWipeIdempotent && f52fFollowStoppedOnDeath &&
        f52fDeadStateObserved && f52fResurrectionRecovered &&
        f52fNoFalseEncounterRestart;
    sLog.Out(LOG_BASIC, f52fPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F52f five-Player wipe recovery: encounterStarted=%u reservationsSeeded=%u partialDeathSafe=%u partialSnapshot=%u partialOnline=%u partialLiving=%u partialNormalReservations=%u partialThreatReservations=%u wipeClosed=%u reservationsCleared=%u repeatedWipeIdempotent=%u followStoppedOnDeath=%u deadStateObserved=%u resurrectionRecovered=%u noFalseEncounterRestart=%u.",
        f52fEncounterStarted ? 1u : 0u, f52fReservationsSeeded ? 1u : 0u,
        f52fPartialDeathSafe ? 1u : 0u, partialSnapshotAvailable ? 1u : 0u,
        partialOnline, partialLiving, partialNormalReservationCount,
        partialThreatReservationCount, f52fWipeClosed ? 1u : 0u,
        f52fReservationsCleared ? 1u : 0u,
        f52fRepeatedWipeIdempotent ? 1u : 0u,
        f52fFollowStoppedOnDeath ? 1u : 0u,
        f52fDeadStateObserved ? 1u : 0u,
        f52fResurrectionRecovered ? 1u : 0u,
        f52fNoFalseEncounterRestart ? 1u : 0u);
    if (f52fPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F52f closed a real five-Player Group encounter immediately on a full wipe, preserved it through a single Bot death, cleared encounter reservations once, verified Core death cleanup and Dead-state observation, then resurrected all members and restored four unique native NewEngine Follow offsets without restarting combat or entering Legacy policy.");

    // F52g-A: exercise the NewEngine self-resurrection action against a
    // real Core corpse/ghost transition. The existing Core repop lifecycle
    // creates the corpse and released-ghost state; the Engine only decides
    // when to invoke Player::ResurrectPlayer and then restores native Follow.
    bool f52gCorpseCreated = false;
    bool f52gGhostReleased = false;
    bool f52gActionCandidate = false;
    bool f52gSelfResurrected = false;
    bool f52gCorpseConvertedToBones = false;
    bool f52gFollowRestored = false;
    bool f52gNewEngineOnly = false;
    if (f52fPassed && !botAIs.empty() && players.size() == 5)
    {
        PartyBotAI& reviveAI = *botAIs[0];
        Player* const reviveBot = players[1];
        uint32 const ticksBefore = reviveAI.m_simulatorNewEngineTickCount;
        uint32 const cyclesBefore = reviveAI.m_simulatorEngineDecisionCycleCount;
        uint32 const legacyEntriesBefore = reviveAI.m_simulatorLegacyPolicyEntryCount;
        uint32 const legacyDispatchesBefore = reviveAI.m_simulatorLegacyPolicyDispatchCount;

        // Match the real lethal transition before the native corpse/repop path.
        reviveBot->SetHealth(0);
        reviveBot->SetDeathState(JUST_DIED);
        reviveBot->KillPlayer();
        f52gCorpseCreated = reviveBot->GetDeathState() == CORPSE &&
            !reviveBot->IsAlive();

        if (f52gCorpseCreated)
            reviveBot->BuildPlayerRepop();
        f52gGhostReleased = f52gCorpseCreated &&
            reviveBot->GetDeathState() == DEAD && !reviveBot->IsAlive() &&
            reviveBot->GetCorpse() != nullptr;

        if (f52gGhostReleased)
        {
            wipeCoordinator->InvalidateSnapshot();
            reviveAI.m_updateTimer.Reset(1000);
            reviveAI.UpdateAI(1000);

            f52gActionCandidate =
                reviveAI.m_simulatorEngineDecisionCycleCount == cyclesBefore + 1 &&
                reviveAI.m_simulatorNewEngineTickCount == ticksBefore + 1 &&
                reviveAI.m_simulatorEngineCandidateCount == 1;
            f52gSelfResurrected = reviveBot->IsAlive() &&
                reviveBot->GetDeathState() == ALIVE;
            f52gCorpseConvertedToBones =
                f52gSelfResurrected && reviveBot->GetCorpse() == nullptr;

            // The next tick chooses the native Follow again; a further tick
            // observes the resulting generator and settles at FollowingLeader.
            for (uint32 tick = 0; tick < 2; ++tick)
            {
                wipeCoordinator->InvalidateSnapshot();
                reviveAI.m_updateTimer.Reset(1000);
                reviveAI.UpdateAI(1000);
            }
            MovementGenerator const* recoveredMovement =
                reviveBot->GetMotionMaster()->GetCurrent();
            FollowMovementGenerator<Player> const* recoveredFollow =
                recoveredMovement &&
                recoveredMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE
                    ? static_cast<FollowMovementGenerator<Player> const*>(recoveredMovement)
                    : nullptr;
            uint8 const recoveredSlot = wipeCoordinator->GetFormationSlot(
                reviveBot->GetObjectGuid().GetRawValue());
            f52gFollowRestored = recoveredFollow &&
                recoveredFollow->GetTarget() == leader &&
                reviveBot->HasUnitState(UNIT_STATE_FOLLOW) &&
                recoveredSlot == slotByBot[0] &&
                reviveAI.m_botBehaviorStateMachine.GetState() ==
                    BotBehaviorState::FollowingLeader;
        }

        f52gNewEngineOnly =
            reviveAI.m_simulatorNewEngineTickCount == ticksBefore + 3 &&
            reviveAI.m_simulatorLegacyPolicyEntryCount == legacyEntriesBefore &&
            reviveAI.m_simulatorLegacyPolicyDispatchCount == legacyDispatchesBefore &&
            reviveAI.m_newEngineStrategiesRegistered &&
            !reviveAI.m_engineShadowStrategiesRegistered &&
            !wipeCoordinator->HasActiveEncounter();
    }
    bool const f52gSelfResurrectionPassed =
        f52gCorpseCreated && f52gGhostReleased && f52gActionCandidate &&
        f52gSelfResurrected && f52gCorpseConvertedToBones &&
        f52gFollowRestored && f52gNewEngineOnly;
    sLog.Out(LOG_BASIC, f52gSelfResurrectionPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F52g NewEngine ghost self-resurrection: corpseCreated=%u ghostReleased=%u actionCandidate=%u selfResurrected=%u corpseConvertedToBones=%u followRestored=%u newEngineOnly=%u.",
        f52gCorpseCreated ? 1u : 0u, f52gGhostReleased ? 1u : 0u,
        f52gActionCandidate ? 1u : 0u, f52gSelfResurrected ? 1u : 0u,
        f52gCorpseConvertedToBones ? 1u : 0u,
        f52gFollowRestored ? 1u : 0u, f52gNewEngineOnly ? 1u : 0u);
    if (f52gSelfResurrectionPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F52g Core created a real resurrectable corpse and released ghost; NewEngine selected its self-resurrection action, Core Player::ResurrectPlayer restored the Bot and converted the corpse to bones, then NewEngine restored the original formation Follow without Legacy policy entry/dispatch.");

    // F52g-B: run the production group-resurrection Strategy/CoreAction path
    // against a real five-Player Group. Vanilla resurrection is a two-step
    // lifecycle: Spell::finish creates an offer; the recipient accepts later.
    bool f52gBSpellAvailable = false;
    bool f52gBTerminalListenerAdded = false;
    bool f52gBGhostRejected = false;
    bool f52gBForeignGroupRejected = false;
    bool f52gBOutOfRangeRejected = false;
    bool f52gBActionSelected = false;
    bool f52gBCastStarted = false;
    bool f52gBRequestPending = false;
    bool f52gBTerminalMatched = false;
    bool f52gBReservationReleased = false;
    bool f52gBTargetAccepted = false;
    bool f52gBNewEngineOnly = false;
    uint32 f52gBSpellId = 0;
    ActionId f52gBActionId = 0;
    if (f52gSelfResurrectionPassed && botAIs.size() == 4 && players.size() == 5)
    {
        PartyBotAI& resurrectionAI = *botAIs[0];
        Player* const caster = players[1];
        Player* const ghostTarget = players[0];
        Player* const corpseTarget = players[2];
        Guid const casterGuid = caster->GetObjectGuid().GetRawValue();
        Guid const ghostTargetGuid = ghostTarget->GetObjectGuid().GetRawValue();
        Guid const corpseTargetGuid = corpseTarget->GetObjectGuid().GetRawValue();
        uint32 const legacyEntriesBefore = resurrectionAI.m_simulatorLegacyPolicyEntryCount;
        uint32 const legacyDispatchesBefore = resurrectionAI.m_simulatorLegacyPolicyDispatchCount;
        uint32 const ticksBefore = resurrectionAI.m_simulatorNewEngineTickCount;
        uint32 const cyclesBefore = resurrectionAI.m_simulatorEngineDecisionCycleCount;

        // Simulator Players start with only their minimal default spell set.
        // Learn the real Vanilla Priest Resurrection chain through Core, then
        // let the production CombatBotBaseAI spell scanner choose its best rank.
        caster->LearnSpellHighRank(2006); // Priest Resurrection, Rank 1
        resurrectionAI.PopulateSpellData();
        SpellEntry const* const resurrectionSpell = resurrectionAI.m_resurrectionSpell;
        if (resurrectionSpell)
        {
            for (uint32 effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
                if (resurrectionSpell->Effect[effect] == SPELL_EFFECT_RESURRECT ||
                    resurrectionSpell->Effect[effect] == SPELL_EFFECT_RESURRECT_NEW)
                    f52gBSpellAvailable = true;
            if (f52gBSpellAvailable)
                f52gBSpellId = resurrectionSpell->Id;
        }

        std::shared_ptr<CaptureCoreSpellTerminalListener> resurrectionTerminal(
            new CaptureCoreSpellTerminalListener());
        f52gBTerminalListenerAdded =
            resurrectionAI.m_partyBotEngine.AddCoreSpellTerminalListener(resurrectionTerminal);

        auto captureGroupResurrectionDecision =
            [&](Player* expectedTarget, PartySnapshot& capturedSnapshot,
                DecisionReport& report, std::size_t& selectedIndex) -> bool
        {
            if (!wipeCoordinator || !expectedTarget)
                return false;
            wipeCoordinator->InvalidateSnapshot();
            PartySnapshot const* const fresh = PartySnapshotBuilder::GetOrBuild(
                caster, *wipeCoordinator, WorldTimer::getMSTime(), 0);
            if (!fresh)
                return false;
            capturedSnapshot = *fresh;
            PartyMemberSnapshot const* const botSnapshot =
                capturedSnapshot.FindMember(casterGuid);
            if (!botSnapshot)
                return false;
            DecisionContext context(
                capturedSnapshot, *botSnapshot, *wipeCoordinator,
                WorldTimer::getMSTime(), nullptr, nullptr,
                &resurrectionAI, nullptr, &resurrectionAI);
            report = resurrectionAI.m_partyBotEngine.Evaluate(context);
            selectedIndex = report.candidates.size();
            for (std::size_t index : report.selectedIndices)
                if (index < report.candidates.size() &&
                    report.candidates[index].actionName == "core_resurrect_group_member" &&
                    report.candidates[index].targetGuid ==
                        expectedTarget->GetObjectGuid().GetRawValue())
                {
                    selectedIndex = index;
                    break;
                }
            return selectedIndex < report.candidates.size();
        };

        // Build a valid Corpse candidate first, then advance the real Core
        // target to ReleasedGhost. Executing the stale decision must fail closed.
        if (f52gBSpellAvailable && f52gBTerminalListenerAdded &&
            caster->IsAlive() && caster->GetGroup() == group.get())
        {
            ghostTarget->SetHealth(0);
            ghostTarget->SetDeathState(JUST_DIED);
            ghostTarget->KillPlayer();
            bool const ghostCorpseCreated =
                ghostTarget->GetDeathState() == CORPSE && !ghostTarget->IsAlive();
            PartySnapshot staleGhostSnapshot;
            DecisionReport staleGhostReport;
            std::size_t staleGhostIndex = 0;
            bool const staleGhostSelected = ghostCorpseCreated &&
                captureGroupResurrectionDecision(
                    ghostTarget, staleGhostSnapshot, staleGhostReport, staleGhostIndex);
            if (staleGhostSelected)
            {
                ghostTarget->BuildPlayerRepop();
                bool const releasedGhost =
                    ghostTarget->GetDeathState() == DEAD && !ghostTarget->IsAlive();
                PartyMemberSnapshot const* const botSnapshot =
                    staleGhostSnapshot.FindMember(casterGuid);
                DecisionContext staleGhostContext(
                    staleGhostSnapshot, *botSnapshot, *wipeCoordinator,
                    WorldTimer::getMSTime(), nullptr, nullptr,
                    &resurrectionAI, nullptr, &resurrectionAI);
                resurrectionAI.m_partyBotEngine.ExecuteSelected(
                    staleGhostContext, staleGhostReport);
                CandidateEvaluation const& evaluation =
                    staleGhostReport.candidates[staleGhostIndex];
                f52gBGhostRejected = releasedGhost &&
                    evaluation.status == CandidateStatus::ExecutionFailed &&
                    evaluation.actionResult == ActionResult::Impossible &&
                    wipeCoordinator->GetReservations().empty() &&
                    caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
            }

            // Restore the Group leader through Core before the other stale
            // target races; no network/client action is implied by this fixture.
            if (!ghostTarget->IsAlive())
            {
                ghostTarget->ResurrectPlayer(0.5f, false);
                ghostTarget->SpawnCorpseBones();
                ghostTarget->ClearResurrectRequestData();
            }

            // Create a second valid snapshot, then remove the target from its
            // Group before dispatch. Core's live Group check must veto it.
            if (f52gBGhostRejected && ghostTarget->IsAlive())
            {
                corpseTarget->SetHealth(0);
                corpseTarget->SetDeathState(JUST_DIED);
                corpseTarget->KillPlayer();
                bool const groupCorpseCreated =
                    corpseTarget->GetDeathState() == CORPSE && !corpseTarget->IsAlive();
                PartySnapshot staleGroupSnapshot;
                DecisionReport staleGroupReport;
                std::size_t staleGroupIndex = 0;
                bool const staleGroupSelected = groupCorpseCreated &&
                    captureGroupResurrectionDecision(
                        corpseTarget, staleGroupSnapshot, staleGroupReport, staleGroupIndex);
                if (staleGroupSelected)
                {
                    group->RemoveMember(corpseTarget->GetObjectGuid(), GROUP_LEAVE);
                    bool const removedFromGroup =
                        corpseTarget->GetGroup() != group.get() &&
                        group->GetMembersCount() == 4;
                    PartyMemberSnapshot const* const botSnapshot =
                        staleGroupSnapshot.FindMember(casterGuid);
                    DecisionContext staleGroupContext(
                        staleGroupSnapshot, *botSnapshot, *wipeCoordinator,
                        WorldTimer::getMSTime(), nullptr, nullptr,
                        &resurrectionAI, nullptr, &resurrectionAI);
                    resurrectionAI.m_partyBotEngine.ExecuteSelected(
                        staleGroupContext, staleGroupReport);
                    CandidateEvaluation const& evaluation =
                        staleGroupReport.candidates[staleGroupIndex];
                    f52gBForeignGroupRejected = removedFromGroup &&
                        evaluation.status == CandidateStatus::ExecutionFailed &&
                        evaluation.actionResult == ActionResult::Impossible &&
                        wipeCoordinator->GetReservations().empty() &&
                        caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
                    bool const rejoined =
                        group->AddMember(corpseTarget->GetObjectGuid(), corpseTarget->GetName()) &&
                        corpseTarget->GetGroup() == group.get() &&
                        group->GetMembersCount() == 5;
                    f52gBForeignGroupRejected =
                        f52gBForeignGroupRejected && rejoined;
                }

                // Build another fresh decision, move the still-live Corpse
                // beyond the actual spell range, and dispatch the stale data.
                if (f52gBForeignGroupRejected)
                {
                    PartySnapshot staleRangeSnapshot;
                    DecisionReport staleRangeReport;
                    std::size_t staleRangeIndex = 0;
                    bool const staleRangeSelected = captureGroupResurrectionDecision(
                        corpseTarget, staleRangeSnapshot, staleRangeReport, staleRangeIndex);
                    if (staleRangeSelected)
                    {
                        float const targetX = corpseTarget->GetPositionX();
                        float const targetY = corpseTarget->GetPositionY();
                        float const targetZ = corpseTarget->GetPositionZ();
                        float const targetO = corpseTarget->GetOrientation();
                        corpseTarget->Relocate(1000.0f, targetY, targetZ, targetO);
                        bool const outsideSpellRange =
                            !resurrectionSpell->IsTargetInRange(caster, corpseTarget);
                        PartyMemberSnapshot const* const botSnapshot =
                            staleRangeSnapshot.FindMember(casterGuid);
                        DecisionContext staleRangeContext(
                            staleRangeSnapshot, *botSnapshot, *wipeCoordinator,
                            WorldTimer::getMSTime(), nullptr, nullptr,
                            &resurrectionAI, nullptr, &resurrectionAI);
                        resurrectionAI.m_partyBotEngine.ExecuteSelected(
                            staleRangeContext, staleRangeReport);
                        CandidateEvaluation const& evaluation =
                            staleRangeReport.candidates[staleRangeIndex];
                        f52gBOutOfRangeRejected = outsideSpellRange &&
                            evaluation.status == CandidateStatus::ExecutionFailed &&
                            evaluation.actionResult == ActionResult::Impossible &&
                            wipeCoordinator->GetReservations().empty() &&
                            caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
                        corpseTarget->Relocate(targetX, targetY, targetZ, targetO);
                    }
                }

                // Finally, select and execute against the fresh real Corpse.
                if (f52gBOutOfRangeRejected)
                {
                    PartySnapshot liveSnapshot;
                    DecisionReport liveReport;
                    std::size_t liveIndex = 0;
                    bool const selected = captureGroupResurrectionDecision(
                        corpseTarget, liveSnapshot, liveReport, liveIndex);
                    if (selected)
                    {
                        f52gBActionSelected = true;
                        f52gBActionId = liveReport.candidates[liveIndex].actionId;
                        PartyMemberSnapshot const* const botSnapshot =
                            liveSnapshot.FindMember(casterGuid);
                        DecisionContext liveContext(
                            liveSnapshot, *botSnapshot, *wipeCoordinator,
                            WorldTimer::getMSTime(), nullptr, nullptr,
                            &resurrectionAI, nullptr, &resurrectionAI);
                        resurrectionAI.m_partyBotEngine.ExecuteSelected(
                            liveContext, liveReport);
                        CandidateEvaluation const& evaluation =
                            liveReport.candidates[liveIndex];
                        f52gBCastStarted =
                            evaluation.status == CandidateStatus::Executed &&
                            evaluation.actionResult == ActionResult::Success &&
                            f52gBActionId != 0 &&
                            caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr &&
                            std::any_of(
                                wipeCoordinator->GetReservations().begin(),
                                wipeCoordinator->GetReservations().end(),
                                [casterGuid, corpseTargetGuid](Reservation const& row)
                                {
                                    return row.type == ReservationType::Resurrection &&
                                        row.targetGuid == corpseTargetGuid &&
                                        row.ownerGuid == casterGuid;
                                });
                    }

                    // Freeze all Bot decision timers while the real Core Spell
                    // cast advances; Map::Update still delivers Spell terminals.
                    for (std::unique_ptr<PartyBotAI> const& botAI : botAIs)
                        botAI->m_updateTimer.Reset(600000);
                    for (uint32 tick = 0; tick < 60 &&
                        (resurrectionTerminal->events.empty() ||
                         caster->GetCurrentSpell(CURRENT_GENERIC_SPELL)); ++tick)
                        map.Update(250);

                    f52gBRequestPending = f52gBCastStarted &&
                        corpseTarget->IsRessurectRequested() &&
                        corpseTarget->GetResurrector() == caster->GetObjectGuid();
                    f52gBTerminalMatched = f52gBCastStarted &&
                        resurrectionTerminal->events.size() == 1 &&
                        resurrectionTerminal->events[0].actionId == f52gBActionId &&
                        resurrectionTerminal->events[0].targetGuid == corpseTargetGuid &&
                        resurrectionTerminal->events[0].spellId == f52gBSpellId &&
                        resurrectionTerminal->events[0].status ==
                            CoreSpellTerminalStatus::Succeeded;
                    f52gBReservationReleased = f52gBTerminalMatched &&
                        std::none_of(
                            wipeCoordinator->GetReservations().begin(),
                            wipeCoordinator->GetReservations().end(),
                            [casterGuid, corpseTargetGuid](Reservation const& row)
                            {
                                return row.type == ReservationType::Resurrection &&
                                    row.targetGuid == corpseTargetGuid &&
                                    row.ownerGuid == casterGuid;
                            });
                    if (f52gBRequestPending && f52gBTerminalMatched &&
                        f52gBReservationReleased)
                    {
                        // No simulator client can send SMSG_RESURRECT_RESPONSE.
                        // Model explicit recipient consent at Core Player level,
                        // then verify Core's normal resurrection/corpse cleanup.
                        corpseTarget->ResurrectPlayer(0.5f, false);
                        corpseTarget->SpawnCorpseBones();
                        corpseTarget->ClearResurrectRequestData();
                        f52gBTargetAccepted = corpseTarget->IsAlive() &&
                            corpseTarget->GetDeathState() == ALIVE &&
                            corpseTarget->GetCorpse() == nullptr &&
                            !corpseTarget->IsRessurectRequested();
                    }
                }
            }
        }

        f52gBNewEngineOnly =
            resurrectionAI.m_newEngineStrategiesRegistered &&
            !resurrectionAI.m_engineShadowStrategiesRegistered &&
            resurrectionAI.m_simulatorLegacyPolicyEntryCount == legacyEntriesBefore &&
            resurrectionAI.m_simulatorLegacyPolicyDispatchCount == legacyDispatchesBefore &&
            resurrectionAI.m_simulatorNewEngineTickCount == ticksBefore &&
            resurrectionAI.m_simulatorEngineDecisionCycleCount == cyclesBefore &&
            wipeCoordinator && !wipeCoordinator->HasActiveEncounter();
    }
    bool const f52gBPassed =
        f52gBSpellAvailable && f52gBTerminalListenerAdded &&
        f52gBGhostRejected && f52gBForeignGroupRejected &&
        f52gBOutOfRangeRejected && f52gBActionSelected &&
        f52gBCastStarted && f52gBRequestPending &&
        f52gBTerminalMatched && f52gBReservationReleased &&
        f52gBTargetAccepted && f52gBNewEngineOnly;
    sLog.Out(LOG_BASIC, f52gBPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F52g-B group resurrection: spellAvailable=%u spellId=%u ghostRejected=%u foreignGroupRejected=%u outOfRangeRejected=%u selected=%u castStarted=%u actionId=%llu requestPending=%u terminalMatched=%u reservationReleased=%u targetAccepted=%u newEngineOnly=%u.",
        f52gBSpellAvailable ? 1u : 0u, f52gBSpellId,
        f52gBGhostRejected ? 1u : 0u,
        f52gBForeignGroupRejected ? 1u : 0u,
        f52gBOutOfRangeRejected ? 1u : 0u,
        f52gBActionSelected ? 1u : 0u, f52gBCastStarted ? 1u : 0u,
        static_cast<unsigned long long>(f52gBActionId),
        f52gBRequestPending ? 1u : 0u,
        f52gBTerminalMatched ? 1u : 0u,
        f52gBReservationReleased ? 1u : 0u,
        f52gBTargetAccepted ? 1u : 0u,
        f52gBNewEngineOnly ? 1u : 0u);
    if (f52gBPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F52g-B NewEngine selected a same-Group corpse, Core Spell::finish created one resurrection offer and emitted the matching Engine action terminal, and the exact reservation was released; live released-ghost, departed-member and out-of-range snapshots were rejected before casting.");

    bool const passed = groupCreated && membersAdded &&
        sharedCoordinatorForAll && allSnapshotsAreFiveMembers &&
        allBotsStartedFollow && allFourSlotsOccupied && allSlotsUnique &&
        allOffsetsMatch && allNewEngineOnly && reverseTickNoOps &&
        removedFromGroup && departureReleasedSlot && incumbentsKeptSlots &&
        rejoinReclaimedSlot && f52ePassed && f52fPassed &&
        f52gSelfResurrectionPassed && f52gBPassed;

    bool const cleaned = cleanup();
    sLog.Out(LOG_BASIC, passed && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] F52c/F52d five-Bot formation: group=%u members=%u sharedCoordinator=%u snapshots=%u follows=%u occupiedSlots=%u uniqueSlots=%u offsets=%u newEngineOnly=%u reverseTickNoOps=%u removed=%u departureReleased=%u incumbentsStable=%u rejoinReclaimed=%u cleanup=%u slots=%u/%u/%u/%u.",
        groupCreated ? 1u : 0u, membersAdded ? 1u : 0u,
        sharedCoordinatorForAll ? 1u : 0u, allSnapshotsAreFiveMembers ? 1u : 0u,
        allBotsStartedFollow ? 1u : 0u, allFourSlotsOccupied ? 1u : 0u,
        allSlotsUnique ? 1u : 0u, allOffsetsMatch ? 1u : 0u,
        allNewEngineOnly ? 1u : 0u, reverseTickNoOps ? 1u : 0u,
        removedFromGroup ? 1u : 0u, departureReleasedSlot ? 1u : 0u,
        incumbentsKeptSlots ? 1u : 0u, rejoinReclaimedSlot ? 1u : 0u,
        cleaned ? 1u : 0u,
        static_cast<unsigned>(slotByBot[0]), static_cast<unsigned>(slotByBot[1]),
        static_cast<unsigned>(slotByBot[2]), static_cast<unsigned>(slotByBot[3]));
    if (passed && cleaned)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: F52d removed and re-added a real PartyBot Player through Group::RemoveMember/AddMember; the shared production snapshot released only its slot, preserved the other three assignments, and the returning Bot reclaimed the vacant slot and resumed native Follow through NewEngine with zero Legacy policy entry/dispatch.");
    return passed && cleaned;
}

bool RunEngineCoreSelectHostileTargetSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;


    SimulatorActor hostile;
    SimulatorActor friendly;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 10.0f, 6.0f, hostile) &&
        CreateSimulatorActor(map, *info, partyFaction, 3.0f, 6.0f, friendly);

    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = actorsCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot, 0.0f, 6.0f, 4.0f);
    if (!botCreated)
    {
        bool cleaned = true;
        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        else if (bot)
            delete bot;
        cleaned = DestroySimulatorActor(map, friendly) && cleaned;
        cleaned = DestroySimulatorActor(map, hostile) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Select-hostile target fixture creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    PartyBotAI actionRuntime(
        bot, map.GetId(), map.GetInstanceId(),
        bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetOrientation());
    actionRuntime.SetPlayer(bot);
    actionRuntime.m_initialized = true;
    actionRuntime.m_updateTimer.Reset(600000);

    PartySnapshot snapshot;
    snapshot.groupId = 93;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 93;
    snapshot.capturedAtMs = WorldTimer::getMSTime();

    PartyMemberSnapshot member;
    member.guid = bot->GetObjectGuid().GetRawValue();
    member.mapId = map.GetId();
    member.instanceId = map.GetInstanceId();
    member.health = bot->GetHealth();
    member.maxHealth = bot->GetMaxHealth();
    member.selectionGuid = bot->GetSelectionGuid().GetRawValue();
    member.online = bot->IsInWorld();
    member.alive = bot->IsAlive();
    snapshot.members.push_back(member);

    HostileSnapshot hostileSnapshot;
    hostileSnapshot.guid = hostile.creature->GetObjectGuid().GetRawValue();
    hostileSnapshot.health = hostile.creature->GetHealth();
    hostileSnapshot.maxHealth = hostile.creature->GetMaxHealth();
    hostileSnapshot.x = hostile.creature->GetPositionX();
    hostileSnapshot.y = hostile.creature->GetPositionY();
    hostileSnapshot.z = hostile.creature->GetPositionZ();
    hostileSnapshot.positionValid = true;
    hostileSnapshot.alive = hostile.creature->IsAlive();
    member.x = bot->GetPositionX();
    member.y = bot->GetPositionY();
    member.z = bot->GetPositionZ();
    member.orientation = bot->GetOrientation();
    snapshot.hostiles.push_back(hostileSnapshot);

    PartyCoordinator coordinator;
    DecisionContext context(
        snapshot, snapshot.members[0], coordinator, WorldTimer::getMSTime(),
        nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);

    // First clear a deliberately stale, non-resolving selection GUID. The
    // live action must prove that the Player is truly idle before clearing it.
    ObjectGuid const staleSelectionGuid(static_cast<uint64>(0xF430000000000001ull));
    bot->SetSelectionGuid(staleSelectionGuid);
    snapshot.members[0].selectionGuid = staleSelectionGuid.GetRawValue();
    PartyCoordinator clearCoordinator;
    DecisionContext clearContext(
        snapshot, snapshot.members[0], clearCoordinator, WorldTimer::getMSTime(),
        nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine clearEngine;
    std::shared_ptr<ClearIdleTargetAction> clearAction(new ClearIdleTargetAction());
    bool const clearRegistered = clearEngine.RegisterAction(clearAction);
    std::shared_ptr<Strategy> clearStrategy(new Strategy("simulator_clear_idle_target"));
    clearStrategy->AddTrigger(std::make_shared<SimulatorClearIdleTargetTrigger>());
    clearEngine.AddStrategy(clearStrategy);
    DecisionReport clearReport = clearEngine.Evaluate(clearContext);
    bool const clearSelected = clearRegistered &&
        clearReport.selectedIndices.size() == 1 &&
        clearReport.candidates[clearReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (clearSelected)
        clearEngine.ExecuteSelected(clearContext, clearReport);
    bool const staleSelectionCleared = clearSelected &&
        clearReport.candidates[clearReport.selectedIndices[0]].status ==
            CandidateStatus::Executed &&
        clearReport.candidates[clearReport.selectedIndices[0]].actionResult ==
            ActionResult::Success &&
        bot->GetSelectionGuid().IsEmpty() && bot->GetTargetGuid().IsEmpty() &&
        bot->GetVictim() == nullptr && !bot->IsInCombat();
    snapshot.members[0].selectionGuid = 0;

    PartyBotEngine engine;
    std::shared_ptr<SelectHostileTargetAction> action(
        new SelectHostileTargetAction());
    bool const actionRegistered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> hostileStrategy(new Strategy("simulator_select_hostile_target"));
    hostileStrategy->AddTrigger(std::make_shared<SimulatorSelectHostileTargetTrigger>(
        hostileSnapshot.guid));
    engine.AddStrategy(hostileStrategy);

    DecisionReport hostileReport = engine.Evaluate(context);
    bool const hostileSelected = actionRegistered &&
        hostileReport.selectedIndices.size() == 1 &&
        hostileReport.candidates[hostileReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (hostileSelected)
        engine.ExecuteSelected(context, hostileReport);
    bool const hostileAccepted = hostileSelected &&
        hostileReport.candidates[hostileReport.selectedIndices[0]].status ==
            CandidateStatus::Executed &&
        hostileReport.candidates[hostileReport.selectedIndices[0]].actionResult ==
            ActionResult::Success &&
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid();

    Guid const hostileGuid = hostile.creature->GetObjectGuid().GetRawValue();

    // F43: dismount only for a selected, live hostile. Use a DBC-backed display
    // ID and the real Player::Mount/Unmount Core APIs; the fixture must preserve
    // selection, victim/combat, resources, target health and threat.
    uint32 mountDisplayId = 0;
    for (uint32 displayId = 1;
         displayId < sCreatureDisplayInfoStore.GetNumRows(); ++displayId)
    {
        if (sCreatureDisplayInfoStore.LookupEntry(displayId))
        {
            mountDisplayId = displayId;
            break;
        }
    }
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bool const dismountMountStarted =
        mountDisplayId != 0 &&
        bot->Mount(mountDisplayId, 1) == MOUNTRESULT_OK &&
        bot->IsMounted();
    PartySnapshot dismountSnapshot = snapshot;
    dismountSnapshot.epoch++;
    dismountSnapshot.members[0].selectionGuid = hostileGuid;
    dismountSnapshot.members[0].mounted = true;
    dismountSnapshot.members[0].taxiFlying = false;
    dismountSnapshot.members[0].inCombat = bot->IsInCombat();
    PartyCoordinator dismountCoordinator;
    DecisionContext dismountContext(
        dismountSnapshot, dismountSnapshot.members[0], dismountCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine dismountEngine;
    std::shared_ptr<DismountForCombatAction> dismountAction(
        new DismountForCombatAction());
    bool const dismountRegistered =
        dismountEngine.RegisterAction(dismountAction);
    std::shared_ptr<Strategy> dismountStrategy(
        new Strategy("simulator_dismount_for_combat"));
    dismountStrategy->AddTrigger(
        std::make_shared<SimulatorDismountForCombatTrigger>(hostileGuid));
    dismountEngine.AddStrategy(dismountStrategy);

    Unit* const dismountVictimBefore = bot->GetVictim();
    bool const dismountCombatBefore = bot->IsInCombat();
    uint32 const dismountHealthBefore = bot->GetHealth();
    uint32 const dismountPowerBefore = bot->GetPower(bot->GetPowerType());
    uint32 const dismountTargetHealthBefore = hostile.creature->GetHealth();
    float const dismountThreatBefore =
        hostile.creature->GetThreatManager().getThreat(bot);
    DecisionReport dismountReport = dismountEngine.Evaluate(dismountContext);
    bool const dismountSelected = dismountMountStarted && dismountRegistered &&
        dismountReport.selectedIndices.size() == 1 &&
        dismountReport.candidates[
            dismountReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (dismountSelected)
        dismountEngine.ExecuteSelected(dismountContext, dismountReport);
    CandidateEvaluation const* dismountEvaluation = dismountSelected
        ? &dismountReport.candidates[dismountReport.selectedIndices[0]] : nullptr;
    float const dismountThreatAfter =
        hostile.creature->GetThreatManager().getThreat(bot);
    bool const dismountAccepted = dismountEvaluation &&
        dismountEvaluation->status == CandidateStatus::Executed &&
        dismountEvaluation->actionResult == ActionResult::Success &&
        dismountEvaluation->reason ==
            "dismount_for_combat_unmounted_without_combat_side_effects" &&
        !bot->IsMounted() && bot->GetMountID() == 0 &&
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetVictim() == dismountVictimBefore &&
        bot->IsInCombat() == dismountCombatBefore &&
        bot->GetHealth() == dismountHealthBefore &&
        bot->GetPower(bot->GetPowerType()) == dismountPowerBefore &&
        hostile.creature->GetHealth() == dismountTargetHealthBefore &&
        std::fabs(dismountThreatAfter - dismountThreatBefore) <= 0.001f;

    // Replaying the same stale mounted snapshot must not send a second
    // dismount request or claim success after the Player is already unmounted.
    DecisionReport alreadyDismountedReport =
        dismountEngine.Evaluate(dismountContext);
    bool const alreadyDismountedSelected =
        alreadyDismountedReport.selectedIndices.size() == 1 &&
        alreadyDismountedReport.candidates[
            alreadyDismountedReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
    if (alreadyDismountedSelected)
        dismountEngine.ExecuteSelected(dismountContext, alreadyDismountedReport);
    CandidateEvaluation const* alreadyDismountedEvaluation =
        alreadyDismountedSelected
            ? &alreadyDismountedReport.candidates[
                alreadyDismountedReport.selectedIndices[0]] : nullptr;
    bool const alreadyDismountedRejected = alreadyDismountedEvaluation &&
        alreadyDismountedEvaluation->status == CandidateStatus::ExecutionFailed &&
        alreadyDismountedEvaluation->actionResult == ActionResult::Useless &&
        alreadyDismountedEvaluation->reason ==
            "dismount_for_combat_actor_not_mounted_or_on_taxi" &&
        !bot->IsMounted();

    // A live selection race after evaluation must reject before unmounting.
    bool const dismountRaceMountStarted =
        bot->Mount(mountDisplayId, 1) == MOUNTRESULT_OK && bot->IsMounted();
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    PartySnapshot dismountRaceSnapshot = dismountSnapshot;
    dismountRaceSnapshot.epoch++;
    PartyCoordinator dismountRaceCoordinator;
    DecisionContext dismountRaceContext(
        dismountRaceSnapshot, dismountRaceSnapshot.members[0],
        dismountRaceCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    DecisionReport dismountRaceReport =
        dismountEngine.Evaluate(dismountRaceContext);
    bool const dismountRaceSelected = dismountRaceMountStarted &&
        dismountRaceReport.selectedIndices.size() == 1 &&
        dismountRaceReport.candidates[
            dismountRaceReport.selectedIndices[0]].status == CandidateStatus::Selected;
    bot->SetSelectionGuid(friendly.creature->GetObjectGuid());
    if (dismountRaceSelected)
        dismountEngine.ExecuteSelected(dismountRaceContext, dismountRaceReport);
    CandidateEvaluation const* dismountRaceEvaluation = dismountRaceSelected
        ? &dismountRaceReport.candidates[
            dismountRaceReport.selectedIndices[0]] : nullptr;
    bool const dismountSelectionDriftRejected = dismountRaceEvaluation &&
        dismountRaceEvaluation->status == CandidateStatus::ExecutionFailed &&
        dismountRaceEvaluation->actionResult == ActionResult::Impossible &&
        dismountRaceEvaluation->reason ==
            "dismount_for_combat_live_selection_mismatch" &&
        bot->IsMounted() &&
        bot->GetSelectionGuid() == friendly.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == friendly.creature->GetObjectGuid();
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bool const dismountRaceCleanup =
        bot->Unmount() == DISMOUNTRESULT_OK && !bot->IsMounted();

    // Taxi-flight state is explicitly excluded at snapshot eligibility time.
    PartySnapshot taxiDismountSnapshot = dismountSnapshot;
    taxiDismountSnapshot.epoch++;
    taxiDismountSnapshot.members[0].taxiFlying = true;
    PartyCoordinator taxiDismountCoordinator;
    DecisionContext taxiDismountContext(
        taxiDismountSnapshot, taxiDismountSnapshot.members[0],
        taxiDismountCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine taxiDismountEngine;
    std::shared_ptr<DismountForCombatAction> taxiDismountAction(
        new DismountForCombatAction());
    bool const taxiDismountRegistered =
        taxiDismountEngine.RegisterAction(taxiDismountAction);
    std::shared_ptr<Strategy> taxiDismountStrategy(
        new Strategy("simulator_dismount_during_taxi"));
    taxiDismountStrategy->AddTrigger(
        std::make_shared<SimulatorDismountForCombatTrigger>(hostileGuid));
    taxiDismountEngine.AddStrategy(taxiDismountStrategy);
    DecisionReport taxiDismountReport =
        taxiDismountEngine.Evaluate(taxiDismountContext);
    bool const taxiDismountNotProposed = taxiDismountRegistered &&
        taxiDismountReport.selectedIndices.empty() &&
        taxiDismountReport.candidates.empty() && !bot->IsMounted();

    bool const dismountFixturePassed =
        dismountMountStarted && dismountRegistered && dismountSelected &&
        dismountAccepted && alreadyDismountedRejected &&
        dismountRaceSelected && dismountSelectionDriftRejected &&
        dismountRaceCleanup && taxiDismountNotProposed;
    sLog.Out(LOG_BASIC, dismountFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] DismountForCombatAction: display=%u mountStarted=%u registered=%u selected=%u accepted=%u alreadyDismountedRejected=%u selectionDriftRejected=%u raceCleanup=%u taxiNotProposed=%u.",
        mountDisplayId, dismountMountStarted ? 1u : 0u,
        dismountRegistered ? 1u : 0u, dismountSelected ? 1u : 0u,
        dismountAccepted ? 1u : 0u, alreadyDismountedRejected ? 1u : 0u,
        dismountSelectionDriftRejected ? 1u : 0u,
        dismountRaceCleanup ? 1u : 0u, taxiDismountNotProposed ? 1u : 0u);
    if (!dismountFixturePassed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] DismountForCombatAction accepted/rejected fixture failed.");
        return false;
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] PASS: DismountForCombatAction used Player::Unmount only for a selected live hostile, preserved combat/selection/health/threat, rejected stale mount and target facts, and skipped taxi flight.");

    // F43: execute a generic CoreAction against an already-selected hostile.
    // The fixture observes the synchronous Core request, stale-snapshot rejection
    // and a live selection race. It does not use PartyBotAI::AttackStart.
    PartySnapshot attackSnapshot = snapshot;
    attackSnapshot.epoch++;
    attackSnapshot.members[0].selectionGuid = hostileGuid;
    attackSnapshot.members[0].victimGuid = 0;
    attackSnapshot.members[0].inCombat = bot->IsInCombat();
    PartyCoordinator attackCoordinator;
    DecisionContext attackContext(
        attackSnapshot, attackSnapshot.members[0], attackCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine attackEngine;
    std::shared_ptr<AttackTargetAction> attackAction(new AttackTargetAction());
    bool const attackRegistered = attackEngine.RegisterAction(attackAction);
    std::shared_ptr<Strategy> attackStrategy(new Strategy("simulator_attack_target"));
    attackStrategy->AddTrigger(
        std::make_shared<SimulatorAttackTargetTrigger>(hostileGuid));
    attackEngine.AddStrategy(attackStrategy);

    uint32 const attackTargetHealthBefore = hostile.creature->GetHealth();
    float const attackThreatBefore = hostile.creature->GetThreatManager().getThreat(bot);
    DecisionReport attackReport = attackEngine.Evaluate(attackContext);
    bool const attackSelected = attackReport.selectedIndices.size() == 1 &&
        attackReport.candidates[attackReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (attackSelected)
        attackEngine.ExecuteSelected(attackContext, attackReport);
    CandidateEvaluation const* attackEvaluation = attackSelected
        ? &attackReport.candidates[attackReport.selectedIndices[0]] : nullptr;
    float const attackThreatAfter = hostile.creature->GetThreatManager().getThreat(bot);
    bool const attackAccepted = attackEvaluation &&
        attackEvaluation->status == CandidateStatus::Executed &&
        attackEvaluation->actionResult == ActionResult::Success &&
        attackEvaluation->reason == "attack_target_auto_attack_request_accepted" &&
        bot->GetVictim() == hostile.creature &&
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid();
    bool const attackNoImmediateDamageOrThreat =
        hostile.creature->GetHealth() == attackTargetHealthBefore &&
        std::fabs(attackThreatAfter - attackThreatBefore) <= 0.001f;
    bot->AttackStop();

    // A stale positive hostile snapshot must not make a same-faction Creature
    // attackable; the final live CanAttack preflight must veto it.
    Guid const friendlyGuid = friendly.creature->GetObjectGuid().GetRawValue();
    bot->SetSelectionGuid(friendly.creature->GetObjectGuid());
    PartySnapshot attackFriendlySnapshot = attackSnapshot;
    attackFriendlySnapshot.epoch++;
    attackFriendlySnapshot.members[0].selectionGuid = friendlyGuid;
    attackFriendlySnapshot.hostiles.clear();
    HostileSnapshot staleAttackFriendly;
    staleAttackFriendly.guid = friendlyGuid;
    staleAttackFriendly.health = friendly.creature->GetHealth();
    staleAttackFriendly.maxHealth = friendly.creature->GetMaxHealth();
    staleAttackFriendly.alive = friendly.creature->IsAlive();
    attackFriendlySnapshot.hostiles.push_back(staleAttackFriendly);
    PartyCoordinator attackFriendlyCoordinator;
    DecisionContext attackFriendlyContext(
        attackFriendlySnapshot, attackFriendlySnapshot.members[0],
        attackFriendlyCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine attackFriendlyEngine;
    std::shared_ptr<AttackTargetAction> attackFriendlyAction(
        new AttackTargetAction());
    bool const attackFriendlyRegistered =
        attackFriendlyEngine.RegisterAction(attackFriendlyAction);
    std::shared_ptr<Strategy> attackFriendlyStrategy(
        new Strategy("simulator_attack_stale_friendly"));
    attackFriendlyStrategy->AddTrigger(
        std::make_shared<SimulatorAttackTargetTrigger>(friendlyGuid));
    attackFriendlyEngine.AddStrategy(attackFriendlyStrategy);
    DecisionReport attackFriendlyReport =
        attackFriendlyEngine.Evaluate(attackFriendlyContext);
    bool const attackFriendlySelected = attackFriendlyRegistered &&
        attackFriendlyReport.selectedIndices.size() == 1 &&
        attackFriendlyReport.candidates[
            attackFriendlyReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (attackFriendlySelected)
        attackFriendlyEngine.ExecuteSelected(
            attackFriendlyContext, attackFriendlyReport);
    CandidateEvaluation const* attackFriendlyEvaluation = attackFriendlySelected
        ? &attackFriendlyReport.candidates[
            attackFriendlyReport.selectedIndices[0]] : nullptr;
    bool const attackFriendlyRejected = attackFriendlyEvaluation &&
        attackFriendlyEvaluation->status == CandidateStatus::ExecutionFailed &&
        attackFriendlyEvaluation->actionResult == ActionResult::Impossible &&
        attackFriendlyEvaluation->reason == "attack_target_not_attackable" &&
        bot->GetVictim() == nullptr &&
        bot->GetSelectionGuid() == friendly.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == friendly.creature->GetObjectGuid();

    // Change selection after evaluation. The adapter must detect target drift
    // before invoking Unit::Attack and leave the live selection untouched.
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    PartySnapshot attackRaceSnapshot = attackSnapshot;
    attackRaceSnapshot.epoch++;
    PartyCoordinator attackRaceCoordinator;
    DecisionContext attackRaceContext(
        attackRaceSnapshot, attackRaceSnapshot.members[0], attackRaceCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine attackRaceEngine;
    std::shared_ptr<AttackTargetAction> attackRaceAction(new AttackTargetAction());
    bool const attackRaceRegistered = attackRaceEngine.RegisterAction(attackRaceAction);
    std::shared_ptr<Strategy> attackRaceStrategy(
        new Strategy("simulator_attack_selection_drift"));
    attackRaceStrategy->AddTrigger(
        std::make_shared<SimulatorAttackTargetTrigger>(hostileGuid));
    attackRaceEngine.AddStrategy(attackRaceStrategy);
    DecisionReport attackRaceReport = attackRaceEngine.Evaluate(attackRaceContext);
    bool const attackRaceSelected = attackRaceRegistered &&
        attackRaceReport.selectedIndices.size() == 1 &&
        attackRaceReport.candidates[attackRaceReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    bot->SetSelectionGuid(friendly.creature->GetObjectGuid());
    if (attackRaceSelected)
        attackRaceEngine.ExecuteSelected(attackRaceContext, attackRaceReport);
    CandidateEvaluation const* attackRaceEvaluation = attackRaceSelected
        ? &attackRaceReport.candidates[attackRaceReport.selectedIndices[0]] : nullptr;
    bool const attackSelectionDriftRejected = attackRaceEvaluation &&
        attackRaceEvaluation->status == CandidateStatus::ExecutionFailed &&
        attackRaceEvaluation->actionResult == ActionResult::Impossible &&
        attackRaceEvaluation->reason == "attack_target_live_selection_mismatch" &&
        bot->GetVictim() == nullptr &&
        bot->GetSelectionGuid() == friendly.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == friendly.creature->GetObjectGuid();
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());

    bool const attackFixturePassed = attackRegistered && attackSelected &&
        attackAccepted && attackNoImmediateDamageOrThreat &&
        attackFriendlyRegistered && attackFriendlySelected &&
        attackFriendlyRejected && attackRaceRegistered && attackRaceSelected &&
        attackSelectionDriftRejected;
    sLog.Out(LOG_BASIC, attackFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] AttackTargetAction: registered=%u selected=%u accepted=%u noImmediateDamageOrThreat=%u staleFriendlyRejected=%u selectionDriftRejected=%u.",
        attackRegistered ? 1u : 0u, attackSelected ? 1u : 0u,
        attackAccepted ? 1u : 0u, attackNoImmediateDamageOrThreat ? 1u : 0u,
        attackFriendlyRejected ? 1u : 0u,
        attackSelectionDriftRejected ? 1u : 0u);

    // F43: stop a current melee auto-attack by its captured victim GUID.
    // This is deliberately narrower than CombatStop: Core AttackStop must leave
    // selection/combat and non-melee spell state alone.
    bool const stopAutoAttackStarted = bot->Attack(hostile.creature, true);
    bot->SetInCombatWith(hostile.creature);
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    PartySnapshot stopAutoAttackSnapshot = snapshot;
    stopAutoAttackSnapshot.epoch++;
    stopAutoAttackSnapshot.members[0].selectionGuid = hostileGuid;
    stopAutoAttackSnapshot.members[0].victimGuid = hostileGuid;
    stopAutoAttackSnapshot.members[0].inCombat = bot->IsInCombat();
    stopAutoAttackSnapshot.members[0].currentSpellId = 0;

    PartyCoordinator stopAutoAttackCoordinator;
    DecisionContext stopAutoAttackContext(
        stopAutoAttackSnapshot, stopAutoAttackSnapshot.members[0],
        stopAutoAttackCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine stopAutoAttackEngine;
    std::shared_ptr<StopAutoAttackAction> stopAutoAttackAction(
        new StopAutoAttackAction());
    bool const stopAutoAttackRegistered =
        stopAutoAttackEngine.RegisterAction(stopAutoAttackAction);
    std::shared_ptr<Strategy> stopAutoAttackStrategy(
        new Strategy("simulator_stop_auto_attack"));
    stopAutoAttackStrategy->AddTrigger(
        std::make_shared<SimulatorStopAutoAttackTrigger>());
    stopAutoAttackEngine.AddStrategy(stopAutoAttackStrategy);

    uint32 const stopAutoAttackHealthBefore = hostile.creature->GetHealth();
    float const stopAutoAttackThreatBefore =
        hostile.creature->GetThreatManager().getThreat(bot);
    ObjectGuid const stopAutoAttackSelectionBefore = bot->GetSelectionGuid();
    bool const stopAutoAttackCombatBefore = bot->IsInCombat();
    bool const stopAutoAttackReferenceBefore =
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
        hostile.creature->GetAttackers().end();
    bool const stopAutoAttackMeleeBefore =
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);

    DecisionReport stopAutoAttackReport =
        stopAutoAttackEngine.Evaluate(stopAutoAttackContext);
    bool const stopAutoAttackSelected = stopAutoAttackReport.selectedIndices.size() == 1 &&
        stopAutoAttackReport.candidates[
            stopAutoAttackReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (stopAutoAttackSelected)
        stopAutoAttackEngine.ExecuteSelected(
            stopAutoAttackContext, stopAutoAttackReport);
    CandidateEvaluation const* stopAutoAttackEvaluation = stopAutoAttackSelected
        ? &stopAutoAttackReport.candidates[
            stopAutoAttackReport.selectedIndices[0]] : nullptr;
    float const stopAutoAttackThreatAfter =
        hostile.creature->GetThreatManager().getThreat(bot);
    bool const stopAutoAttackReferenceAfter =
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
        hostile.creature->GetAttackers().end();
    bool const stopAutoAttackAccepted = stopAutoAttackStarted &&
        stopAutoAttackMeleeBefore && stopAutoAttackReferenceBefore &&
        stopAutoAttackEvaluation &&
        stopAutoAttackEvaluation->status == CandidateStatus::Executed &&
        stopAutoAttackEvaluation->actionResult == ActionResult::Success &&
        stopAutoAttackEvaluation->reason ==
            "stop_auto_attack_stopped_expected_victim" &&
        bot->GetVictim() == nullptr && bot->GetTargetGuid().IsEmpty() &&
        !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
        bot->GetSelectionGuid() == stopAutoAttackSelectionBefore &&
        bot->IsInCombat() == stopAutoAttackCombatBefore &&
        !stopAutoAttackReferenceAfter &&
        hostile.creature->GetHealth() == stopAutoAttackHealthBefore &&
        std::fabs(stopAutoAttackThreatAfter - stopAutoAttackThreatBefore) <= 0.001f;

    // With no current victim, the trigger must not propose a stop action.
    PartySnapshot idleStopSnapshot = stopAutoAttackSnapshot;
    idleStopSnapshot.epoch++;
    idleStopSnapshot.members[0].victimGuid = 0;
    PartyCoordinator idleStopCoordinator;
    DecisionContext idleStopContext(
        idleStopSnapshot, idleStopSnapshot.members[0], idleStopCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine idleStopEngine;
    bool const idleStopRegistered =
        idleStopEngine.RegisterAction(std::shared_ptr<StopAutoAttackAction>(
            new StopAutoAttackAction()));
    std::shared_ptr<Strategy> idleStopStrategy(
        new Strategy("simulator_stop_auto_attack_idle"));
    idleStopStrategy->AddTrigger(
        std::make_shared<SimulatorStopAutoAttackTrigger>());
    idleStopEngine.AddStrategy(idleStopStrategy);
    DecisionReport idleStopReport = idleStopEngine.Evaluate(idleStopContext);
    bool const noVictimNotProposed = idleStopRegistered &&
        idleStopReport.selectedIndices.empty() &&
        idleStopReport.candidates.empty() &&
        bot->GetVictim() == nullptr && bot->IsInCombat() == stopAutoAttackCombatBefore;

    // Re-target to a different live hostile after evaluation. The captured old
    // victim GUID must not let the action detach the new attack.
    friendly.creature->SetFactionTemplateId(hostileFaction);
    bool const driftAttackStarted = bot->Attack(friendly.creature, true);
    bot->SetInCombatWith(friendly.creature);
    PartySnapshot driftStopSnapshot = stopAutoAttackSnapshot;
    driftStopSnapshot.epoch++;
    PartyCoordinator driftStopCoordinator;
    DecisionContext driftStopContext(
        driftStopSnapshot, driftStopSnapshot.members[0], driftStopCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine driftStopEngine;
    bool const driftStopRegistered =
        driftStopEngine.RegisterAction(std::shared_ptr<StopAutoAttackAction>(
            new StopAutoAttackAction()));
    std::shared_ptr<Strategy> driftStopStrategy(
        new Strategy("simulator_stop_auto_attack_victim_drift"));
    driftStopStrategy->AddTrigger(
        std::make_shared<SimulatorStopAutoAttackTrigger>());
    driftStopEngine.AddStrategy(driftStopStrategy);
    DecisionReport driftStopReport = driftStopEngine.Evaluate(driftStopContext);
    bool const driftStopSelected = driftStopRegistered &&
        driftStopReport.selectedIndices.size() == 1 &&
        driftStopReport.candidates[
            driftStopReport.selectedIndices[0]].status == CandidateStatus::Selected;
    bool const driftStopCombatBefore = bot->IsInCombat();
    bool const driftFriendlyReferenceBefore =
        std::find(friendly.creature->GetAttackers().begin(),
                  friendly.creature->GetAttackers().end(), bot) !=
        friendly.creature->GetAttackers().end();
    if (driftStopSelected)
        driftStopEngine.ExecuteSelected(driftStopContext, driftStopReport);
    CandidateEvaluation const* driftStopEvaluation = driftStopSelected
        ? &driftStopReport.candidates[
            driftStopReport.selectedIndices[0]] : nullptr;
    bool const stopAutoAttackVictimDriftRejected = driftAttackStarted &&
        driftFriendlyReferenceBefore && driftStopEvaluation &&
        driftStopEvaluation->status == CandidateStatus::ExecutionFailed &&
        driftStopEvaluation->actionResult == ActionResult::Impossible &&
        driftStopEvaluation->reason == "stop_auto_attack_live_victim_mismatch" &&
        bot->GetVictim() == friendly.creature &&
        bot->GetTargetGuid() == friendly.creature->GetObjectGuid() &&
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
        bot->GetSelectionGuid() == stopAutoAttackSelectionBefore &&
        bot->IsInCombat() == driftStopCombatBefore &&
        std::find(friendly.creature->GetAttackers().begin(),
                  friendly.creature->GetAttackers().end(), bot) !=
            friendly.creature->GetAttackers().end();
    bot->AttackStop();
    friendly.creature->SetFactionTemplateId(partyFaction);

    // A stale snapshot must not let StopAutoAttackAction run while a real
    // non-melee Player cast is active. Keep currentSpellId=0 in the value
    // snapshot deliberately so evaluation selects the candidate; the final
    // CoreAction preflight must observe the live Spell and reject side effects.
    bool const activeCastAttackStarted = bot->Attack(hostile.creature, true);
    bot->SetInCombatWith(hostile.creature);
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bot->SetHealth(bot->GetMaxHealth() / 2);
    SpellEntry const* const activeStopProbeSpell =
        sSpellMgr.GetSpellEntry(SIMULATOR_HEAL_SPELL_ID);
    SpellCastResult const activeStopCastResult = activeStopProbeSpell
        ? bot->CastSpell(bot, activeStopProbeSpell, false)
        : SPELL_FAILED_DONT_REPORT;
    Spell* const activeStopCast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);

    PartySnapshot activeCastStopSnapshot = stopAutoAttackSnapshot;
    activeCastStopSnapshot.epoch++;
    activeCastStopSnapshot.members[0].currentSpellId = 0;
    PartyCoordinator activeCastStopCoordinator;
    DecisionContext activeCastStopContext(
        activeCastStopSnapshot, activeCastStopSnapshot.members[0],
        activeCastStopCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine activeCastStopEngine;
    bool const activeCastStopRegistered =
        activeCastStopEngine.RegisterAction(std::shared_ptr<StopAutoAttackAction>(
            new StopAutoAttackAction()));
    std::shared_ptr<Strategy> activeCastStopStrategy(
        new Strategy("simulator_stop_auto_attack_active_cast"));
    activeCastStopStrategy->AddTrigger(
        std::make_shared<SimulatorStopAutoAttackTrigger>());
    activeCastStopEngine.AddStrategy(activeCastStopStrategy);
    DecisionReport activeCastStopReport =
        activeCastStopEngine.Evaluate(activeCastStopContext);
    bool const activeCastStopSelected =
        activeCastStopRegistered &&
        activeCastStopReport.selectedIndices.size() == 1 &&
        activeCastStopReport.candidates[
            activeCastStopReport.selectedIndices[0]].status == CandidateStatus::Selected;
    bool const activeCastStopCombatBefore = bot->IsInCombat();
    ObjectGuid const activeCastStopSelectionBefore = bot->GetSelectionGuid();
    bool const activeCastStopReferenceBefore =
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
        hostile.creature->GetAttackers().end();
    if (activeCastStopSelected)
        activeCastStopEngine.ExecuteSelected(activeCastStopContext, activeCastStopReport);
    CandidateEvaluation const* activeCastStopEvaluation = activeCastStopSelected
        ? &activeCastStopReport.candidates[
            activeCastStopReport.selectedIndices[0]] : nullptr;
    bool const activeCastStopRejected = activeCastAttackStarted &&
        activeStopCastResult == SPELL_CAST_OK && activeStopCast &&
        activeStopCast->m_spellInfo &&
        activeStopCast->m_spellInfo->Id == SIMULATOR_HEAL_SPELL_ID &&
        activeCastStopReferenceBefore && activeCastStopEvaluation &&
        activeCastStopEvaluation->status == CandidateStatus::ExecutionFailed &&
        activeCastStopEvaluation->actionResult == ActionResult::Useless &&
        activeCastStopEvaluation->reason ==
            "stop_auto_attack_non_melee_spell_in_progress" &&
        bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) == activeStopCast &&
        bot->GetVictim() == hostile.creature &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid() &&
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
        bot->GetSelectionGuid() == activeCastStopSelectionBefore &&
        bot->IsInCombat() == activeCastStopCombatBefore &&
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
            hostile.creature->GetAttackers().end();
    if (activeStopCast)
        bot->InterruptNonMeleeSpells(true, SIMULATOR_HEAL_SPELL_ID);
    bool const activeCastCleanup = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr;
    bot->AttackStop();

    // AttackStop deliberately preserves the combat flag. Close the preceding
    // auto-attack fixture's residual combat timer explicitly so the facing
    // fixture starts from an isolated out-of-combat baseline; its Unit::Update
    // advances the facing spline and may also naturally expire combat timers.
    bot->CombatStop(false);
    bool const faceFixtureCombatReset =
        !bot->IsInCombat() && bot->GetVictim() == nullptr;

    bool const stopAutoAttackFixturePassed = stopAutoAttackRegistered &&
        stopAutoAttackSelected && stopAutoAttackAccepted && noVictimNotProposed &&
        driftStopRegistered && driftStopSelected && stopAutoAttackVictimDriftRejected &&
        activeCastStopRegistered && activeCastStopSelected &&
        activeCastStopRejected && activeCastCleanup;
    sLog.Out(LOG_BASIC, stopAutoAttackFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] StopAutoAttackAction: registered=%u selected=%u stoppedOnly=%u noVictimNotProposed=%u victimDriftRejected=%u activeCastSelected=%u activeCastRejected=%u activeCastCleanup=%u.",
        stopAutoAttackRegistered ? 1u : 0u, stopAutoAttackSelected ? 1u : 0u,
        stopAutoAttackAccepted ? 1u : 0u, noVictimNotProposed ? 1u : 0u,
        stopAutoAttackVictimDriftRejected ? 1u : 0u,
        activeCastStopSelected ? 1u : 0u, activeCastStopRejected ? 1u : 0u,
        activeCastCleanup ? 1u : 0u);

    // AttackStop correctly clears TargetGuid while preserving selection.
    // Re-establish the next fixture's independent selected-target precondition
    // explicitly instead of inheriting state from StopAutoAttackAction.
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bot->SetTargetGuid(hostile.creature->GetObjectGuid());
    bool const faceFixtureSetupReady = faceFixtureCombatReset &&
        bot->GetVictim() == nullptr && !bot->IsInCombat() &&
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid();

    // F43: face the selected hostile only when the value snapshot says the
    // target is outside Core's 180-degree front arc. Live Core state still
    // owns hostility, movement/cast/control validation and the final outcome.
    bot->SetOrientation(3.14159265358979323846f);
    PartySnapshot faceSnapshot = snapshot;
    faceSnapshot.epoch++;
    faceSnapshot.members[0].selectionGuid = hostileGuid;
    faceSnapshot.members[0].orientation = bot->GetOrientation();
    faceSnapshot.members[0].x = bot->GetPositionX();
    faceSnapshot.members[0].y = bot->GetPositionY();
    faceSnapshot.members[0].z = bot->GetPositionZ();
    PartyCoordinator faceCoordinator;
    DecisionContext faceContext(
        faceSnapshot, faceSnapshot.members[0], faceCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine faceEngine;
    std::shared_ptr<FaceTargetAction> faceAction(new FaceTargetAction());
    bool const faceRegistered = faceEngine.RegisterAction(faceAction);
    std::shared_ptr<Strategy> faceStrategy(new Strategy("simulator_face_target"));
    faceStrategy->AddTrigger(
        std::make_shared<SimulatorFaceTargetTrigger>(hostileGuid));
    faceEngine.AddStrategy(faceStrategy);

    float const faceXBefore = bot->GetPositionX();
    float const faceYBefore = bot->GetPositionY();
    float const faceZBefore = bot->GetPositionZ();
    bool const faceCombatBefore = bot->IsInCombat();
    DecisionReport faceReport = faceEngine.Evaluate(faceContext);
    bool const faceSelected = faceRegistered &&
        faceReport.selectedIndices.size() == 1 &&
        faceReport.candidates[faceReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (faceSelected)
        faceEngine.ExecuteSelected(faceContext, faceReport);
    CandidateEvaluation const* faceEvaluation = faceSelected
        ? &faceReport.candidates[faceReport.selectedIndices[0]] : nullptr;
    bool const faceRequestAccepted = faceEvaluation &&
        faceEvaluation->status == CandidateStatus::Executed &&
        faceEvaluation->actionResult == ActionResult::Success &&
        faceEvaluation->reason == "face_target_facing_request_accepted" &&
        bot->GetPositionX() == faceXBefore &&
        bot->GetPositionY() == faceYBefore &&
        bot->GetPositionZ() == faceZBefore &&
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetVictim() == nullptr && bot->IsInCombat() == faceCombatBefore;

    // Unit::Update invokes UpdateSplineMovement as part of its ordinary Core
    // update path. Drive that same public Unit update for this 1 ms facing spline;
    // the fixture's local-session Player is not advanced by Map::Update here.
    if (faceRequestAccepted)
        bot->Update(10, 10);
    bool const faceFacingComplete = faceRequestAccepted &&
        bot->IsFacingTarget(hostile.creature);
    bool const facePositionPreserved =
        std::fabs(bot->GetPositionX() - faceXBefore) <= 0.001f &&
        std::fabs(bot->GetPositionY() - faceYBefore) <= 0.001f &&
        std::fabs(bot->GetPositionZ() - faceZBefore) <= 0.001f;
    bool const faceTargetStatePreserved =
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetVictim() == nullptr && bot->IsInCombat() == faceCombatBefore;
    bool const faceAccepted = faceRequestAccepted && faceFacingComplete &&
        facePositionPreserved && faceTargetStatePreserved;

    // After refreshing only the value orientation, the already-facing target
    // must not be proposed again.
    faceSnapshot.members[0].orientation = bot->GetOrientation();
    PartyCoordinator faceRepeatCoordinator;
    DecisionContext faceRepeatContext(
        faceSnapshot, faceSnapshot.members[0], faceRepeatCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport faceRepeatReport = faceEngine.Evaluate(faceRepeatContext);
    bool const alreadyFacingNoOp = faceRepeatReport.selectedIndices.empty() &&
        !faceRepeatReport.candidates.empty() &&
        faceRepeatReport.candidates[0].status == CandidateStatus::RejectedUseless &&
        faceRepeatReport.candidates[0].reason == "face_target_already_in_front";

    // A positive but stale friendly snapshot can nominate this action; the
    // final CanAttack preflight must reject it without changing orientation.
    bot->SetSelectionGuid(friendly.creature->GetObjectGuid());
    bot->SetTargetGuid(friendly.creature->GetObjectGuid());
    bot->SetOrientation(3.14159265358979323846f);
    PartySnapshot faceFriendlySnapshot = faceSnapshot;
    faceFriendlySnapshot.epoch++;
    faceFriendlySnapshot.members[0].selectionGuid =
        friendly.creature->GetObjectGuid().GetRawValue();
    faceFriendlySnapshot.members[0].orientation = bot->GetOrientation();
    faceFriendlySnapshot.hostiles.clear();
    HostileSnapshot staleFaceFriendly;
    staleFaceFriendly.guid = friendly.creature->GetObjectGuid().GetRawValue();
    staleFaceFriendly.health = friendly.creature->GetHealth();
    staleFaceFriendly.maxHealth = friendly.creature->GetMaxHealth();
    staleFaceFriendly.x = friendly.creature->GetPositionX();
    staleFaceFriendly.y = friendly.creature->GetPositionY();
    staleFaceFriendly.z = friendly.creature->GetPositionZ();
    staleFaceFriendly.positionValid = true;
    staleFaceFriendly.alive = friendly.creature->IsAlive();
    faceFriendlySnapshot.hostiles.push_back(staleFaceFriendly);
    PartyCoordinator faceFriendlyCoordinator;
    DecisionContext faceFriendlyContext(
        faceFriendlySnapshot, faceFriendlySnapshot.members[0],
        faceFriendlyCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    // Use a trigger bound to the friendly GUID. Reusing faceEngine here
    // would still emit the original hostile GUID and never exercise the
    // stale-friendly CoreAction preflight.
    PartyBotEngine faceFriendlyEngine;
    std::shared_ptr<FaceTargetAction> faceFriendlyAction(
        new FaceTargetAction());
    bool const faceFriendlyRegistered =
        faceFriendlyEngine.RegisterAction(faceFriendlyAction);
    std::shared_ptr<Strategy> faceFriendlyStrategy(
        new Strategy("simulator_face_stale_friendly"));
    faceFriendlyStrategy->AddTrigger(
        std::make_shared<SimulatorFaceTargetTrigger>(
            friendly.creature->GetObjectGuid().GetRawValue()));
    faceFriendlyEngine.AddStrategy(faceFriendlyStrategy);
    DecisionReport staleFaceFriendlyReport =
        faceFriendlyEngine.Evaluate(faceFriendlyContext);
    bool const staleFaceFriendlySelected = faceFriendlyRegistered &&
        staleFaceFriendlyReport.selectedIndices.size() == 1 &&
        staleFaceFriendlyReport.candidates[
            staleFaceFriendlyReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
    if (staleFaceFriendlySelected)
        faceFriendlyEngine.ExecuteSelected(
            faceFriendlyContext, staleFaceFriendlyReport);
    CandidateEvaluation const* staleFaceFriendlyEvaluation =
        staleFaceFriendlySelected
            ? &staleFaceFriendlyReport.candidates[
                staleFaceFriendlyReport.selectedIndices[0]] : nullptr;
    std::string const staleFaceFriendlyReason = staleFaceFriendlyEvaluation
        ? staleFaceFriendlyEvaluation->reason : "candidate_not_selected";
    uint32 const staleFaceFriendlyStatus = staleFaceFriendlyEvaluation
        ? static_cast<uint32>(staleFaceFriendlyEvaluation->status) : 0xffffffffu;
    uint32 const staleFaceFriendlyResult = staleFaceFriendlyEvaluation
        ? static_cast<uint32>(staleFaceFriendlyEvaluation->actionResult) : 0xffffffffu;
    bool const staleFaceFriendlySelectionMatches =
        bot->GetSelectionGuid() == friendly.creature->GetObjectGuid();
    bool const staleFaceFriendlyTargetMatches =
        bot->GetTargetGuid() == friendly.creature->GetObjectGuid();
    bool const staleFaceFriendlyNotAttackable =
        !bot->CanAttack(friendly.creature);
    bool const staleFaceFriendlyRejected = staleFaceFriendlyEvaluation &&
        staleFaceFriendlyEvaluation->status == CandidateStatus::ExecutionFailed &&
        staleFaceFriendlyEvaluation->actionResult == ActionResult::Impossible &&
        staleFaceFriendlyEvaluation->reason == "face_target_not_attackable" &&
        std::fabs(bot->GetOrientation() - 3.14159265358979323846f) <= 0.001f &&
        staleFaceFriendlySelectionMatches && staleFaceFriendlyTargetMatches &&
        staleFaceFriendlyNotAttackable &&
        bot->GetVictim() == nullptr && bot->IsInCombat() == faceCombatBefore;
    sLog.Out(LOG_BASIC, staleFaceFriendlyRejected ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] FaceTarget stale-friendly detail: selected=%u status=%u result=%u reason=%s selectionMatch=%u targetMatch=%u canAttack=%u orientation=%.6f victim=%u combat=%u.",
        staleFaceFriendlySelected ? 1u : 0u, staleFaceFriendlyStatus,
        staleFaceFriendlyResult, staleFaceFriendlyReason.c_str(),
        staleFaceFriendlySelectionMatches ? 1u : 0u,
        staleFaceFriendlyTargetMatches ? 1u : 0u,
        staleFaceFriendlyNotAttackable ? 1u : 0u,
        bot->GetOrientation(), bot->GetVictim() ? 1u : 0u,
        bot->IsInCombat() ? 1u : 0u);

    // The native facing API intentionally ignores moving actors. Verify the
    // adapter returns Useless and leaves an active Core PointMovementGenerator intact.
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bot->SetTargetGuid(hostile.creature->GetObjectGuid());
    bot->SetOrientation(3.14159265358979323846f);
    faceSnapshot.members[0].selectionGuid = hostileGuid;
    faceSnapshot.members[0].orientation = bot->GetOrientation();
    faceSnapshot.members[0].x = bot->GetPositionX();
    faceSnapshot.members[0].y = bot->GetPositionY();
    faceSnapshot.members[0].z = bot->GetPositionZ();
    bot->GetMotionMaster()->MovePoint(
        77, bot->GetPositionX() + 10.0f, bot->GetPositionY(), bot->GetPositionZ());
    PartyCoordinator faceMovingCoordinator;
    DecisionContext faceMovingContext(
        faceSnapshot, faceSnapshot.members[0], faceMovingCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport faceMovingReport = faceEngine.Evaluate(faceMovingContext);
    bool const faceMovingSelected = faceMovingReport.selectedIndices.size() == 1 &&
        faceMovingReport.candidates[faceMovingReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (faceMovingSelected)
        faceEngine.ExecuteSelected(faceMovingContext, faceMovingReport);
    CandidateEvaluation const* faceMovingEvaluation = faceMovingSelected
        ? &faceMovingReport.candidates[faceMovingReport.selectedIndices[0]] : nullptr;
    bool const movingRejectedWithoutMutation = faceMovingEvaluation &&
        faceMovingEvaluation->status == CandidateStatus::ExecutionFailed &&
        faceMovingEvaluation->actionResult == ActionResult::Useless &&
        faceMovingEvaluation->reason == "face_target_actor_moving" &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    bot->GetMotionMaster()->MovementExpired(false);

    bool const faceFixturePassed = faceFixtureSetupReady &&
        faceRegistered && faceSelected && faceAccepted && alreadyFacingNoOp &&
        staleFaceFriendlyRejected && movingRejectedWithoutMutation;
    sLog.Out(LOG_BASIC, faceFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] FaceTargetAction: setupRestored=%u registered=%u selected=%u requestAccepted=%u facingComplete=%u positionPreserved=%u targetStatePreserved=%u alreadyFacingNoOp=%u staleFriendlyRejected=%u movingRejected=%u.",
        faceFixtureSetupReady ? 1u : 0u, faceRegistered ? 1u : 0u,
        faceSelected ? 1u : 0u, faceRequestAccepted ? 1u : 0u,
        faceFacingComplete ? 1u : 0u, facePositionPreserved ? 1u : 0u,
        faceTargetStatePreserved ? 1u : 0u, alreadyFacingNoOp ? 1u : 0u,
        staleFaceFriendlyRejected ? 1u : 0u,
        movingRejectedWithoutMutation ? 1u : 0u);

    // Deliberately retain a stale positive HostileSnapshot for the friendly
    // unit. The final CoreAction preflight must re-resolve its live faction and
    // reject it without changing the Player's current selection/target.
    bot->SetSelectionGuid(ObjectGuid());
    PartySnapshot staleSnapshot = snapshot;
    staleSnapshot.epoch++;
    staleSnapshot.members[0].selectionGuid = 0;
    HostileSnapshot staleFriendlySnapshot;
    staleFriendlySnapshot.guid = friendly.creature->GetObjectGuid().GetRawValue();
    staleFriendlySnapshot.health = friendly.creature->GetHealth();
    staleFriendlySnapshot.maxHealth = friendly.creature->GetMaxHealth();
    staleFriendlySnapshot.alive = friendly.creature->IsAlive();
    staleSnapshot.hostiles.clear();
    staleSnapshot.hostiles.push_back(staleFriendlySnapshot);

    PartyCoordinator staleCoordinator;
    DecisionContext staleContext(
        staleSnapshot, staleSnapshot.members[0], staleCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    PartyBotEngine staleEngine;
    std::shared_ptr<SelectHostileTargetAction> staleAction(
        new SelectHostileTargetAction());
    bool const staleRegistered = staleEngine.RegisterAction(staleAction);
    std::shared_ptr<Strategy> staleStrategy(new Strategy("simulator_stale_hostile_snapshot"));
    staleStrategy->AddTrigger(std::make_shared<SimulatorSelectHostileTargetTrigger>(
        staleFriendlySnapshot.guid));
    staleEngine.AddStrategy(staleStrategy);

    DecisionReport friendlyReport = staleEngine.Evaluate(staleContext);
    bool const friendlyCandidateSelected = staleRegistered &&
        friendlyReport.selectedIndices.size() == 1 &&
        friendlyReport.candidates[friendlyReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (friendlyCandidateSelected)
        staleEngine.ExecuteSelected(staleContext, friendlyReport);
    CandidateEvaluation const* friendlyEvaluation =
        friendlyCandidateSelected
            ? &friendlyReport.candidates[friendlyReport.selectedIndices[0]] : nullptr;
    bool const friendlyRejected = friendlyEvaluation &&
        friendlyEvaluation->status == CandidateStatus::ExecutionFailed &&
        friendlyEvaluation->actionResult == ActionResult::Impossible &&
        friendlyEvaluation->reason == "select_hostile_target_not_attackable" &&
        bot->GetSelectionGuid().IsEmpty() && bot->GetTargetGuid().IsEmpty() &&
        bot->GetVictim() == nullptr;

    // F43: Stop only an actually unattackable current victim. AttackStop must
    // preserve the Player's combat flag; a stale negative snapshot against a
    // still-hostile live victim must be rejected without detaching the attack.
    bool const attackStarted = bot->Attack(hostile.creature, true);
    bot->SetInCombatWith(hostile.creature);
    Guid const currentVictimGuid = hostile.creature->GetObjectGuid().GetRawValue();
    snapshot.members[0].victimGuid = currentVictimGuid;
    snapshot.members[0].victimAttackable =
        attackStarted && bot->CanAttack(hostile.creature);
    bool const attackableBefore = snapshot.members[0].victimAttackable;
    bool const attackerReferenceBefore =
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
        hostile.creature->GetAttackers().end();
    bool const combatBeforeStop = bot->IsInCombat();

    PartyCoordinator stopCoordinator;
    DecisionContext stopContext(
        snapshot, snapshot.members[0], stopCoordinator, WorldTimer::getMSTime(),
        nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine stopEngine;
    std::shared_ptr<StopUnattackableTargetAction> stopAction(
        new StopUnattackableTargetAction());
    bool const stopRegistered = stopEngine.RegisterAction(stopAction);
    std::shared_ptr<Strategy> stopStrategy(
        new Strategy("simulator_stop_unattackable_target"));
    stopStrategy->AddTrigger(
        std::make_shared<SimulatorStopUnattackableTargetTrigger>());
    stopEngine.AddStrategy(stopStrategy);

    DecisionReport attackableReport = stopEngine.Evaluate(stopContext);
    bool const attackableNotProposed = attackStarted && attackableBefore &&
        attackableReport.selectedIndices.empty() &&
        attackableReport.candidates.empty() &&
        bot->GetVictim() == hostile.creature && attackerReferenceBefore;

    // Deliberately make the snapshot stale in the negative direction. The
    // action is proposed, but final Core preflight must preserve a live attack.
    snapshot.members[0].victimAttackable = false;
    DecisionReport staleVictimReport = stopEngine.Evaluate(stopContext);
    bool const staleVictimSelected = stopRegistered &&
        staleVictimReport.selectedIndices.size() == 1 &&
        staleVictimReport.candidates[staleVictimReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (staleVictimSelected)
        stopEngine.ExecuteSelected(stopContext, staleVictimReport);
    CandidateEvaluation const* staleVictimEvaluation = staleVictimSelected
        ? &staleVictimReport.candidates[staleVictimReport.selectedIndices[0]] : nullptr;
    bool const staleVictimRejected = staleVictimEvaluation &&
        staleVictimEvaluation->status == CandidateStatus::ExecutionFailed &&
        staleVictimEvaluation->actionResult == ActionResult::Useless &&
        staleVictimEvaluation->reason ==
            "stop_unattackable_target_live_victim_still_attackable" &&
        bot->GetVictim() == hostile.creature &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid() &&
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
        bot->IsInCombat() == combatBeforeStop &&
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
            hostile.creature->GetAttackers().end();

    // Change only the live faction relation. The next snapshot-guided attempt
    // must now use Core AttackStop, not CombatStop, and detach one back-reference.
    hostile.creature->SetFactionTemplateId(partyFaction);
    bool const liveVictimUnattackable = !bot->CanAttack(hostile.creature);
    DecisionReport stopReport = stopEngine.Evaluate(stopContext);
    bool const stopSelected = stopReport.selectedIndices.size() == 1 &&
        stopReport.candidates[stopReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (stopSelected)
        stopEngine.ExecuteSelected(stopContext, stopReport);
    CandidateEvaluation const* stopEvaluation = stopSelected
        ? &stopReport.candidates[stopReport.selectedIndices[0]] : nullptr;
    bool const attackerReferenceAfter =
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
        hostile.creature->GetAttackers().end();
    bool const attackStoppedWithoutCombatStop = liveVictimUnattackable &&
        stopEvaluation && stopEvaluation->status == CandidateStatus::Executed &&
        stopEvaluation->actionResult == ActionResult::Success &&
        bot->GetVictim() == nullptr && bot->GetTargetGuid().IsEmpty() &&
        !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
        !attackerReferenceAfter && bot->IsInCombat() == combatBeforeStop;
    bool const stopFixturePassed = stopRegistered && attackableNotProposed &&
        staleVictimSelected && staleVictimRejected && liveVictimUnattackable &&
        stopSelected && attackStoppedWithoutCombatStop;
    sLog.Out(LOG_BASIC, stopFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] Core StopUnattackableTarget: registered=%u attackableNotProposed=%u staleSelected=%u staleRejected=%u liveUnattackable=%u selected=%u stoppedOnly=%u.",
        stopRegistered ? 1u : 0u, attackableNotProposed ? 1u : 0u,
        staleVictimSelected ? 1u : 0u, staleVictimRejected ? 1u : 0u,
        liveVictimUnattackable ? 1u : 0u, stopSelected ? 1u : 0u,
        attackStoppedWithoutCombatStop ? 1u : 0u);
    hostile.creature->SetFactionTemplateId(hostileFaction);
    bot->CombatStop(false);

    // F43: clear a departed/dead selection without interrupting a different
    // live victim or ending the encounter. Core resolution is actor-only
    // (candidate.targetGuid=0), so a missing selected Unit is expected here.
    bool const clearStaleAttackStarted = bot->Attack(hostile.creature, true);
    bot->SetInCombatWith(hostile.creature);
    ObjectGuid const combatStaleSelectionGuid(
        HIGHGUID_UNIT, 0x00fffffeu, 0x00fffffeu);
    bot->SetSelectionGuid(combatStaleSelectionGuid);
    Guid const clearStaleVictimGuid =
        hostile.creature->GetObjectGuid().GetRawValue();
    bool const clearStaleSelectionMissing =
        combatStaleSelectionGuid.IsUnit() &&
        map.GetUnit(combatStaleSelectionGuid) == nullptr;
    bool const clearStaleAttackerBefore =
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
        hostile.creature->GetAttackers().end();
    bool const clearStaleCombatBefore = bot->IsInCombat();
    bool const clearStaleMeleeBefore =
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
    float const clearStaleThreatBefore =
        hostile.creature->GetThreatManager().getThreat(bot);
    uint32 const clearStaleVictimHealthBefore = hostile.creature->GetHealth();

    PartySnapshot clearStaleSnapshot = snapshot;
    clearStaleSnapshot.epoch++;
    clearStaleSnapshot.members[0].selectionGuid =
        combatStaleSelectionGuid.GetRawValue();
    clearStaleSnapshot.members[0].victimGuid = clearStaleVictimGuid;
    clearStaleSnapshot.members[0].inCombat = clearStaleCombatBefore;
    PartyCoordinator clearStaleCoordinator;
    DecisionContext clearStaleContext(
        clearStaleSnapshot, clearStaleSnapshot.members[0],
        clearStaleCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine clearStaleEngine;
    std::shared_ptr<ClearStaleTargetAction> clearStaleAction(
        new ClearStaleTargetAction());
    bool const clearStaleRegistered =
        clearStaleEngine.RegisterAction(clearStaleAction);
    std::shared_ptr<Strategy> clearStaleStrategy(
        new Strategy("simulator_clear_stale_target"));
    clearStaleStrategy->AddTrigger(
        std::make_shared<SimulatorClearStaleTargetTrigger>());
    clearStaleEngine.AddStrategy(clearStaleStrategy);

    DecisionReport clearStaleReport =
        clearStaleEngine.Evaluate(clearStaleContext);
    bool const clearStaleSelected =
        clearStaleReport.selectedIndices.size() == 1 &&
        clearStaleReport.candidates[
            clearStaleReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (clearStaleSelected)
    {
        // Simulate a selection race after evaluation. A live selection change
        // must be rejected before the actor's different victim is disturbed.
        bot->SetSelectionGuid(friendly.creature->GetObjectGuid());
        clearStaleEngine.ExecuteSelected(clearStaleContext, clearStaleReport);
    }
    CandidateEvaluation const* clearStaleDriftEvaluation = clearStaleSelected
        ? &clearStaleReport.candidates[
            clearStaleReport.selectedIndices[0]] : nullptr;
    bool const clearStaleDriftRejected = clearStaleDriftEvaluation &&
        clearStaleDriftEvaluation->status == CandidateStatus::ExecutionFailed &&
        clearStaleDriftEvaluation->actionResult == ActionResult::Impossible &&
        clearStaleDriftEvaluation->reason ==
            "clear_stale_target_actor_or_selection_mismatch" &&
        bot->GetSelectionGuid() == friendly.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == friendly.creature->GetObjectGuid() &&
        bot->GetVictim() == hostile.creature &&
        bot->IsInCombat() == clearStaleCombatBefore;

    // Restore the exact stale selection snapshot and run the accepted path.
    bot->SetSelectionGuid(combatStaleSelectionGuid);
    PartyCoordinator clearStaleAcceptedCoordinator;
    DecisionContext clearStaleAcceptedContext(
        clearStaleSnapshot, clearStaleSnapshot.members[0],
        clearStaleAcceptedCoordinator, WorldTimer::getMSTime(), nullptr, nullptr,
        &actionRuntime, nullptr, &actionRuntime);
    DecisionReport clearStaleAcceptedReport =
        clearStaleEngine.Evaluate(clearStaleAcceptedContext);
    bool const clearStaleAcceptedSelected =
        clearStaleAcceptedReport.selectedIndices.size() == 1 &&
        clearStaleAcceptedReport.candidates[
            clearStaleAcceptedReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
    if (clearStaleAcceptedSelected)
        clearStaleEngine.ExecuteSelected(
            clearStaleAcceptedContext, clearStaleAcceptedReport);
    CandidateEvaluation const* clearStaleAcceptedEvaluation =
        clearStaleAcceptedSelected
            ? &clearStaleAcceptedReport.candidates[
                clearStaleAcceptedReport.selectedIndices[0]] : nullptr;
    bool const clearStaleAccepted =
        clearStaleAcceptedEvaluation &&
        clearStaleAcceptedEvaluation->status == CandidateStatus::Executed &&
        clearStaleAcceptedEvaluation->actionResult == ActionResult::Success &&
        clearStaleAcceptedEvaluation->reason ==
            "clear_stale_target_cleared_selection_preserving_combat" &&
        bot->GetSelectionGuid().IsEmpty() && bot->GetTargetGuid().IsEmpty();

    bool const clearStaleVictimPreserved =
        clearStaleAttackStarted && clearStaleSelectionMissing &&
        bot->GetVictim() == hostile.creature &&
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == clearStaleMeleeBefore;
    bool const clearStaleCombatPreserved =
        bot->IsInCombat() == clearStaleCombatBefore;
    bool const clearStaleAttackerPreserved =
        clearStaleAttackerBefore &&
        std::find(hostile.creature->GetAttackers().begin(),
                  hostile.creature->GetAttackers().end(), bot) !=
            hostile.creature->GetAttackers().end();
    float const clearStaleThreatAfter =
        hostile.creature->GetThreatManager().getThreat(bot);
    uint32 const clearStaleVictimHealthAfter = hostile.creature->GetHealth();
    bool const clearStaleNoThreatMutation =
        std::fabs(clearStaleThreatAfter - clearStaleThreatBefore) < 0.001f &&
        clearStaleVictimHealthAfter == clearStaleVictimHealthBefore;
    bool const clearStaleFixturePassed =
        clearStaleRegistered && clearStaleSelected && clearStaleDriftRejected &&
        clearStaleAccepted && clearStaleVictimPreserved &&
        clearStaleCombatPreserved && clearStaleAttackerPreserved &&
        clearStaleNoThreatMutation;
    sLog.Out(LOG_BASIC, clearStaleFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] ClearStaleTargetAction: registered=%u selected=%u driftRejected=%u cleared=%u victimPreserved=%u combatPreserved=%u attackersPreserved=%u threatUnchanged=%u.",
        clearStaleRegistered ? 1u : 0u, clearStaleSelected ? 1u : 0u,
        clearStaleDriftRejected ? 1u : 0u, clearStaleAccepted ? 1u : 0u,
        clearStaleVictimPreserved ? 1u : 0u, clearStaleCombatPreserved ? 1u : 0u,
        clearStaleAttackerPreserved ? 1u : 0u,
        clearStaleNoThreatMutation ? 1u : 0u);
    bot->CombatStop(false);

    bot->SetAI(nullptr);
    actionRuntime.Remove();
    botSession.LogoutPlayer(false);
    bool const playerCleaned = botSession.GetPlayer() == nullptr;
    bot = nullptr;
    bool const friendlyCleaned = DestroySimulatorActor(map, friendly);
    bool const hostileCleaned = DestroySimulatorActor(map, hostile);
    bool const cleaned = playerCleaned && friendlyCleaned && hostileCleaned;

    sLog.Out(LOG_BASIC, (staleSelectionCleared && hostileAccepted &&
                         attackFixturePassed && stopAutoAttackFixturePassed &&
                         faceFixturePassed && clearStaleFixturePassed && dismountFixturePassed &&
                         friendlyRejected && cleaned)
        ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] Core generic target actions: clearRegistered=%u staleSelectionCleared=%u hostileRegistered=%u hostileAccepted=%u faceFixture=%u clearStaleFixture=%u dismountFixture=%u friendlyCandidate=%u friendlyRejected=%u unchangedOnReject=%u cleanup=%u.",
        clearRegistered ? 1u : 0u, staleSelectionCleared ? 1u : 0u,
        actionRegistered ? 1u : 0u, hostileAccepted ? 1u : 0u,
        faceFixturePassed ? 1u : 0u, clearStaleFixturePassed ? 1u : 0u,
        dismountFixturePassed ? 1u : 0u, friendlyCandidateSelected ? 1u : 0u, friendlyRejected ? 1u : 0u,
        (botSession.GetPlayer() == nullptr && friendlyRejected) ? 1u : 0u,
        cleaned ? 1u : 0u);

    if (!clearRegistered || !staleSelectionCleared || !actionRegistered ||
        !hostileAccepted || !attackFixturePassed || !stopAutoAttackFixturePassed ||
        !faceFixturePassed || !clearStaleFixturePassed || !dismountFixturePassed ||
        !friendlyCandidateSelected || !friendlyRejected ||
        !stopFixturePassed || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core select-hostile target fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: generic Core target maintenance cleared an idle stale selection, selected a live hostile and rejected a stale friendly target without disturbing Player combat state.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F43 StopUnattackableTarget used live Core preflight to reject a stale negative snapshot, then AttackStop detached only the invalid victim while preserving combat state.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: StopAutoAttackAction used the snapshot victim GUID and live Core AttackStop, preserved selection/combat and unchanged health/threat, and rejected a different live victim without calling CombatStop.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: AttackTargetAction used the CoreAction live resolver and CanAttack preflight, accepted only the selected hostile, rejected stale-friendly and selection-drift requests, and created no synchronous damage/threat or Legacy fallback.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: FaceTargetAction used snapshot geometry to face a live selected hostile, skipped an already-facing target, and rejected stale-friendly and moving actors without disturbing position, movement or combat state.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: ClearStaleTargetAction cleared a missing selection during a live encounter while preserving a different victim, melee state, attacker reference, combat flag and ThreatManager values.");
    return true;
}


bool RunEngineCoreChaseTargetSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=begin template=%u.", info->entry);

    SimulatorActor hostile;
    bool const hostileCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 10.0f, 6.0f, hostile);
    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = hostileCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot, 0.0f, 6.0f, 4.0f);
    WorldSession followLeaderSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* followLeader = nullptr;
    bool const followLeaderCreated = botCreated &&
        CreateSimulatorPlayer(map, partyFaction, followLeaderSession, followLeader,
                              -3.0f, 6.0f, 4.0f);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=actors hostileCreated=%u botCreated=%u followLeaderCreated=%u bot=%p.",
        hostileCreated ? 1u : 0u, botCreated ? 1u : 0u,
        followLeaderCreated ? 1u : 0u, static_cast<void*>(bot));
    if (!botCreated || !followLeaderCreated)
    {
        if (followLeaderSession.GetPlayer())
            followLeaderSession.LogoutPlayer(false);
        else if (followLeader)
            delete followLeader;
        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        else if (bot)
            delete bot;
        DestroySimulatorActor(map, hostile);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] ChaseTargetAction fixture creation failed.");
        return false;
    }

    PartyBotAI actionRuntime(
        bot, map.GetId(), map.GetInstanceId(),
        bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetOrientation());
    actionRuntime.SetPlayer(bot);
    actionRuntime.m_initialized = true;
    actionRuntime.m_updateTimer.Reset(600000);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=runtime-ready player=%u map=%u instance=%u.",
        bot->GetObjectGuid().GetCounter(), map.GetId(), map.GetInstanceId());

    Guid const hostileGuid = hostile.creature->GetObjectGuid().GetRawValue();
    PartySnapshot snapshot;
    snapshot.groupId = 0xF43u;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 0xF430u;
    snapshot.capturedAtMs = WorldTimer::getMSTime();

    PartyMemberSnapshot member;
    member.guid = bot->GetObjectGuid().GetRawValue();
    member.mapId = map.GetId();
    member.instanceId = map.GetInstanceId();
    member.health = bot->GetHealth();
    member.maxHealth = bot->GetMaxHealth();
    member.power = bot->GetPower(bot->GetPowerType());
    member.maxPower = bot->GetMaxPower(bot->GetPowerType());
    member.online = bot->IsInWorld();
    member.alive = bot->IsAlive();
    member.selectionGuid = hostileGuid;
    member.mounted = bot->IsMounted();
    member.taxiFlying = bot->IsTaxiFlying();
    member.chaseTargetGuid = 0;
    Guid const followLeaderGuid = followLeader->GetObjectGuid().GetRawValue();
    member.followTargetGuid = followLeaderGuid;
    snapshot.members.push_back(member);

    PartyMemberSnapshot followLeaderMember;
    followLeaderMember.guid = followLeaderGuid;
    followLeaderMember.mapId = map.GetId();
    followLeaderMember.instanceId = map.GetInstanceId();
    followLeaderMember.health = followLeader->GetHealth();
    followLeaderMember.maxHealth = followLeader->GetMaxHealth();
    followLeaderMember.online = followLeader->IsInWorld();
    followLeaderMember.alive = followLeader->IsAlive();
    snapshot.members.push_back(followLeaderMember);

    HostileSnapshot hostileSnapshot;
    hostileSnapshot.guid = hostileGuid;
    hostileSnapshot.health = hostile.creature->GetHealth();
    hostileSnapshot.maxHealth = hostile.creature->GetMaxHealth();
    hostileSnapshot.x = hostile.creature->GetPositionX();
    hostileSnapshot.y = hostile.creature->GetPositionY();
    hostileSnapshot.z = hostile.creature->GetPositionZ();
    hostileSnapshot.positionValid = true;
    hostileSnapshot.alive = hostile.creature->IsAlive();
    snapshot.hostiles.push_back(hostileSnapshot);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=snapshot-ready target=%u.",
        hostile.creature->GetObjectGuid().GetCounter());

    bool const attackStarted = bot->Attack(hostile.creature, true);
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bot->SetInCombatWith(hostile.creature);
    snapshot.members[0].victimGuid = hostileGuid;
    snapshot.members[0].inCombat = bot->IsInCombat();
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=attack-started=%u victim=%u targetGuid=%llu.",
        attackStarted ? 1u : 0u, bot->GetVictim() == hostile.creature ? 1u : 0u,
        static_cast<unsigned long long>(bot->GetTargetGuid().GetRawValue()));

    PartyCoordinator coordinator;
    DecisionContext context(
        snapshot, snapshot.members[0], coordinator, WorldTimer::getMSTime(),
        nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    PartyBotEngine engine;
    std::shared_ptr<ChaseTargetAction> action(new ChaseTargetAction(1.0f, 0.0f));
    bool const registered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_chase_target"));
    strategy->AddTrigger(std::make_shared<SimulatorChaseTargetTrigger>(hostileGuid));
    engine.AddStrategy(strategy);

    uint32 const healthBefore = bot->GetHealth();
    uint32 const powerBefore = bot->GetPower(bot->GetPowerType());
    uint32 const targetHealthBefore = hostile.creature->GetHealth();
    float const threatBefore = hostile.creature->GetThreatManager().getThreat(bot);

    DecisionReport report = engine.Evaluate(context);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=evaluated candidates=%u selected=%u.",
        static_cast<uint32>(report.candidates.size()),
        static_cast<uint32>(report.selectedIndices.size()));
    bool const selected = attackStarted && bot->GetVictim() == hostile.creature &&
        registered && report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    if (selected)
        engine.ExecuteSelected(context, report);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=dispatched selected=%u status=%u result=%u reason=%s.",
        selected ? 1u : 0u,
        selected ? static_cast<uint32>(report.candidates[report.selectedIndices[0]].status) : 0xffffffffu,
        selected ? static_cast<uint32>(report.candidates[report.selectedIndices[0]].actionResult) : 0xffffffffu,
        selected ? report.candidates[report.selectedIndices[0]].reason.c_str() : "not-selected");
    CandidateEvaluation const* evaluation = selected
        ? &report.candidates[report.selectedIndices[0]] : nullptr;
    ChaseMovementGenerator<Player> const* activeChase =
        bot->GetMotionMaster() &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE
            ? static_cast<ChaseMovementGenerator<Player> const*>(
                bot->GetMotionMaster()->GetCurrent()) : nullptr;
    float const threatAfter = hostile.creature->GetThreatManager().getThreat(bot);
    bool const accepted = evaluation &&
        evaluation->status == CandidateStatus::Executed &&
        evaluation->actionResult == ActionResult::Success &&
        evaluation->reason == "chase_target_core_movement_request_accepted" &&
        activeChase && activeChase->GetTarget() == hostile.creature &&
        bot->GetVictim() == hostile.creature &&
        bot->GetSelectionGuid() == hostile.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == hostile.creature->GetObjectGuid() &&
        bot->IsInCombat() && bot->GetHealth() == healthBefore &&
        bot->GetPower(bot->GetPowerType()) == powerBefore &&
        hostile.creature->GetHealth() == targetHealthBefore &&
        std::fabs(threatAfter - threatBefore) <= 0.001f;

    // Builder-level same-target fact prevents repeated decision proposals.
    PartySnapshot sameChaseSnapshot = snapshot;
    sameChaseSnapshot.epoch++;
    sameChaseSnapshot.members[0].chaseTargetGuid = hostileGuid;
    PartyCoordinator sameCoordinator;
    DecisionContext sameContext(
        sameChaseSnapshot, sameChaseSnapshot.members[0], sameCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport sameReport = engine.Evaluate(sameContext);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=same-chase-evaluated candidates=%u selected=%u.",
        static_cast<uint32>(sameReport.candidates.size()),
        static_cast<uint32>(sameReport.selectedIndices.size()));
    bool const sameChaseNotProposed =
        sameReport.selectedIndices.empty() && !sameReport.candidates.empty() &&
        sameReport.candidates[0].status == CandidateStatus::RejectedUseless &&
        sameReport.candidates[0].reason ==
            "chase_target_already_chasing_current_victim" &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;

    // Combat engagement may supersede a valid party Follow. Core MoveChase
    // stacks above Follow, but Core MovementExpired intentionally drops stored
    // targeted generators under a completed Chase; policy must reissue Follow
    // after combat rather than expecting the old generator to resume.
    bot->GetMotionMaster()->MovementExpired(false);
    bot->StopMoving();
    bot->GetMotionMaster()->MoveFollow(followLeader, 2.5f, 3.14159265358979323846f);
    bool const followStarted =
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
    PartySnapshot staleFollowSnapshot = snapshot;
    staleFollowSnapshot.epoch++;
    staleFollowSnapshot.members[0].followTargetGuid = 0;
    PartyCoordinator staleFollowCoordinator;
    DecisionContext staleFollowContext(
        staleFollowSnapshot, staleFollowSnapshot.members[0], staleFollowCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport staleFollowReport = engine.Evaluate(staleFollowContext);
    bool const staleFollowSelected = followStarted &&
        staleFollowReport.selectedIndices.size() == 1 &&
        staleFollowReport.candidates[
            staleFollowReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (staleFollowSelected)
        engine.ExecuteSelected(staleFollowContext, staleFollowReport);
    CandidateEvaluation const* staleFollowEvaluation = staleFollowSelected
        ? &staleFollowReport.candidates[staleFollowReport.selectedIndices[0]] : nullptr;
    bool const followStaleRejected = staleFollowEvaluation &&
        staleFollowEvaluation->status == CandidateStatus::ExecutionFailed &&
        staleFollowEvaluation->actionResult == ActionResult::Useless &&
        staleFollowEvaluation->reason == "chase_target_follow_target_stale" &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
    PartySnapshot followSnapshot = snapshot;
    followSnapshot.epoch++;
    followSnapshot.members[0].followTargetGuid = followLeaderGuid;
    followSnapshot.members[0].chaseTargetGuid = 0;
    PartyCoordinator followCoordinator;
    DecisionContext followContext(
        followSnapshot, followSnapshot.members[0], followCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport followReport = engine.Evaluate(followContext);
    bool const followSelected = followStarted &&
        followReport.selectedIndices.size() == 1 &&
        followReport.candidates[followReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    if (followSelected)
        engine.ExecuteSelected(followContext, followReport);
    CandidateEvaluation const* followEvaluation = followSelected
        ? &followReport.candidates[followReport.selectedIndices[0]] : nullptr;
    ChaseMovementGenerator<Player> const* followChase =
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE
            ? static_cast<ChaseMovementGenerator<Player> const*>(
                bot->GetMotionMaster()->GetCurrent()) : nullptr;
    bool const followChaseAccepted = followEvaluation &&
        followEvaluation->status == CandidateStatus::Executed &&
        followEvaluation->actionResult == ActionResult::Success &&
        followEvaluation->reason == "chase_target_core_movement_request_accepted" &&
        followChase && followChase->GetTarget() == hostile.creature;
    if (followChaseAccepted)
        bot->GetMotionMaster()->MovementExpired(false);
    bool const followDroppedOnChaseExpiry = followChaseAccepted &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE &&
        !bot->HasUnitState(UNIT_STATE_FOLLOW);
    // Defensive cleanup if Core's stack behavior changes and leaves Follow on top.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        bot->GetMotionMaster()->MovementExpired(false);
    bot->StopMoving();

    // A stale snapshot must not restart chasing once live Core geometry says
    // the victim is already in melee reach. Remove only the current Chase
    // generator, move the Player close through the native relocation API, and
    // require the action's final live preflight to return Useless.
    bot->GetMotionMaster()->MovementExpired(false);
    bot->StopMoving();
    float const meleeTestX = hostile.creature->GetPositionX() - 0.1f;
    float const meleeTestY = hostile.creature->GetPositionY();
    float const meleeTestZ = hostile.creature->GetPositionZ();
    bool const nearVictimPositioned =
        bot->SetPosition(meleeTestX, meleeTestY, meleeTestZ, bot->GetOrientation()) &&
        std::fabs(bot->GetPositionX() - meleeTestX) < 0.01f &&
        std::fabs(bot->GetPositionY() - meleeTestY) < 0.01f &&
        std::fabs(bot->GetPositionZ() - meleeTestZ) < 0.01f;
    bool const coreMeleeReachable = nearVictimPositioned &&
        bot->CanReachWithMeleeAutoAttack(hostile.creature);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction melee-fixture: positioned=%u coreReachable=%u bot=(%.3f,%.3f,%.3f) hostile=(%.3f,%.3f,%.3f).",
        nearVictimPositioned ? 1u : 0u, coreMeleeReachable ? 1u : 0u,
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
        hostile.creature->GetPositionX(), hostile.creature->GetPositionY(),
        hostile.creature->GetPositionZ());
    PartySnapshot reachableSnapshot = snapshot;
    reachableSnapshot.epoch++;
    reachableSnapshot.members[0].x = bot->GetPositionX();
    reachableSnapshot.members[0].y = bot->GetPositionY();
    reachableSnapshot.members[0].z = bot->GetPositionZ();
    reachableSnapshot.members[0].chaseTargetGuid = 0;
    PartyCoordinator reachableCoordinator;
    DecisionContext reachableContext(
        reachableSnapshot, reachableSnapshot.members[0], reachableCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport reachableReport = engine.Evaluate(reachableContext);
    bool const reachableSelected = coreMeleeReachable &&
        reachableReport.selectedIndices.size() == 1 &&
        reachableReport.candidates[
            reachableReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (reachableSelected)
        engine.ExecuteSelected(reachableContext, reachableReport);
    CandidateEvaluation const* reachableEvaluation = reachableSelected
        ? &reachableReport.candidates[reachableReport.selectedIndices[0]] : nullptr;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction melee-dispatch: selected=%u dispatched=%u status=%u result=%u reason=%s.",
        reachableSelected ? 1u : 0u, reachableEvaluation ? 1u : 0u,
        reachableEvaluation ? static_cast<uint32>(reachableEvaluation->status) : 0xffffffffu,
        reachableEvaluation ? static_cast<uint32>(reachableEvaluation->actionResult) : 0xffffffffu,
        reachableEvaluation ? reachableEvaluation->reason.c_str() : "not-dispatched");
    bool const alreadyReachableNotChased = reachableEvaluation &&
        reachableEvaluation->status == CandidateStatus::ExecutionFailed &&
        reachableEvaluation->actionResult == ActionResult::Useless &&
        reachableEvaluation->reason == "chase_target_already_melee_reachable" &&
        bot->GetVictim() == hostile.creature &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE;
    bool const originalPositionRestored =
        bot->SetPosition(0.0f, 6.0f, 4.0f, bot->GetOrientation());

    // The snapshot can remain positive while Core movement/control state
    // changes after evaluation. The final adapter preflight must veto Chase.
    bot->AddUnitState(UNIT_STATE_CAN_NOT_MOVE);
    PartySnapshot immobileSnapshot = snapshot;
    immobileSnapshot.epoch++;
    immobileSnapshot.members[0].chaseTargetGuid = 0;
    PartyCoordinator immobileCoordinator;
    DecisionContext immobileContext(
        immobileSnapshot, immobileSnapshot.members[0], immobileCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport immobileReport = engine.Evaluate(immobileContext);
    bool const immobileSelected = immobileReport.selectedIndices.size() == 1 &&
        immobileReport.candidates[
            immobileReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (immobileSelected)
        engine.ExecuteSelected(immobileContext, immobileReport);
    CandidateEvaluation const* immobileEvaluation = immobileSelected
        ? &immobileReport.candidates[immobileReport.selectedIndices[0]] : nullptr;
    bool const immobileRejected = immobileEvaluation &&
        immobileEvaluation->status == CandidateStatus::ExecutionFailed &&
        immobileEvaluation->actionResult == ActionResult::Useless &&
        immobileEvaluation->reason == "chase_target_actor_cannot_move_or_react" &&
        bot->GetVictim() == hostile.creature &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE;
    bot->ClearUnitState(UNIT_STATE_CAN_NOT_MOVE);

    // Evaluate against a stale victim snapshot, then stop the real Core attack.
    DecisionReport staleReport = engine.Evaluate(context);
    bool const staleSelected = staleReport.selectedIndices.size() == 1 &&
        staleReport.candidates[staleReport.selectedIndices[0]].status ==
            CandidateStatus::Selected;
    bot->GetMotionMaster()->MovementExpired(false);
    bot->StopMoving();
    bot->AttackStop();
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=stale-attack-stopped selected=%u.",
        staleSelected ? 1u : 0u);
    if (staleSelected)
        engine.ExecuteSelected(context, staleReport);
    CandidateEvaluation const* staleEvaluation = staleSelected
        ? &staleReport.candidates[staleReport.selectedIndices[0]] : nullptr;
    bool const staleVictimRejected = staleEvaluation &&
        staleEvaluation->status == CandidateStatus::ExecutionFailed &&
        staleEvaluation->actionResult == ActionResult::Impossible &&
        staleEvaluation->reason == "chase_target_live_victim_mismatch" &&
        bot->GetVictim() == nullptr &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE;

    // An unrelated Point generator remains untouched by a chase request.
    bool const pointAttackStarted = bot->Attack(hostile.creature, true);
    bot->SetInCombatWith(hostile.creature);
    bot->SetSelectionGuid(hostile.creature->GetObjectGuid());
    bot->GetMotionMaster()->MovePoint(
        0xF430u, 2.0f, 6.0f, 4.0f);
    MovementGenerator const* pointBefore = bot->GetMotionMaster()->GetCurrent();
    PartySnapshot pointSnapshot = snapshot;
    pointSnapshot.epoch++;
    pointSnapshot.members[0].inCombat = bot->IsInCombat();
    PartyCoordinator pointCoordinator;
    DecisionContext pointContext(
        pointSnapshot, pointSnapshot.members[0], pointCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport pointReport = engine.Evaluate(pointContext);
    bool const pointSelected = pointAttackStarted && pointBefore &&
        pointBefore->GetMovementGeneratorType() == POINT_MOTION_TYPE &&
        pointReport.selectedIndices.size() == 1 &&
        pointReport.candidates[pointReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (pointSelected)
        engine.ExecuteSelected(pointContext, pointReport);
    CandidateEvaluation const* pointEvaluation = pointSelected
        ? &pointReport.candidates[pointReport.selectedIndices[0]] : nullptr;
    bool const pointMovementPreserved = pointEvaluation &&
        pointEvaluation->status == CandidateStatus::ExecutionFailed &&
        pointEvaluation->actionResult == ActionResult::Useless &&
        pointEvaluation->reason == "chase_target_unrelated_movement_active" &&
        bot->GetMotionMaster()->GetCurrent() == pointBefore &&
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

    bot->GetMotionMaster()->MovementExpired(false);
    bot->StopMoving();
    bot->AttackStop();
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=point-cleaned.");

    bool const fixturePassed = attackStarted && registered && selected && accepted &&
        sameChaseNotProposed && followStarted && followStaleRejected &&
        followSelected && followChaseAccepted && followDroppedOnChaseExpiry && nearVictimPositioned &&
        alreadyReachableNotChased && originalPositionRestored &&
        immobileSelected && immobileRejected &&
        staleSelected && staleVictimRejected && pointSelected &&
        pointMovementPreserved;

    // Capture every diagnostic fact while the Player and hostile are still
    // alive. LogoutPlayer(false) destroys the Player; no raw Player/Unit pointer
    // may be dereferenced after that boundary.
    bool const detailVictimMatch = bot->GetVictim() == hostile.creature;
    bool const detailCanAttack = bot->CanAttack(hostile.creature);
    bool const detailMeleeReachable =
        bot->CanReachWithMeleeAutoAttack(hostile.creature);
    uint32 const detailMovementType = bot->GetMotionMaster()
        ? static_cast<uint32>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType())
        : static_cast<uint32>(IDLE_MOTION_TYPE);
    uint64 const detailVictimGuid = bot->GetVictim()
        ? bot->GetVictim()->GetObjectGuid().GetRawValue() : 0;
    bool const detailMounted = bot->IsMounted();
    bool const detailTaxi = bot->IsTaxiFlying();

    bot->SetAI(nullptr);
    actionRuntime.Remove();
    botSession.LogoutPlayer(false);
    bool const playerCleaned = botSession.GetPlayer() == nullptr;
    bot = nullptr;
    followLeaderSession.LogoutPlayer(false);
    bool const followLeaderCleaned = followLeaderSession.GetPlayer() == nullptr;
    followLeader = nullptr;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction stage=players-logged-out botCleaned=%u followLeaderCleaned=%u.",
        playerCleaned ? 1u : 0u, followLeaderCleaned ? 1u : 0u);
    bool const hostileCleaned = DestroySimulatorActor(map, hostile);
    bool const cleaned = playerCleaned && followLeaderCleaned && hostileCleaned;

    bool const passed = fixturePassed && cleaned;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] ChaseTargetAction detail: result=%u actionResult=%u reason=%s victimMatch=%u canAttack=%u meleeReachable=%u movementType=%u targetGuid=%llu victimGuid=%llu mounted=%u taxi=%u.",
        evaluation ? static_cast<uint32>(evaluation->status) : 0xffffffffu,
        evaluation ? static_cast<uint32>(evaluation->actionResult) : 0xffffffffu,
        evaluation ? evaluation->reason.c_str() : "no-evaluation",
        detailVictimMatch ? 1u : 0u, detailCanAttack ? 1u : 0u,
        detailMeleeReachable ? 1u : 0u, detailMovementType,
        static_cast<unsigned long long>(hostileGuid),
        static_cast<unsigned long long>(detailVictimGuid),
        detailMounted ? 1u : 0u, detailTaxi ? 1u : 0u);
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] ChaseTargetAction: attackStarted=%u registered=%u selected=%u accepted=%u sameChaseNotProposed=%u followStarted=%u followStaleRejected=%u followChaseAccepted=%u followDroppedOnChaseExpiry=%u alreadyReachableNotChased=%u immobileSelected=%u immobileRejected=%u staleSelected=%u staleVictimRejected=%u pointMovementPreserved=%u cleanup=%u.",
        attackStarted ? 1u : 0u, registered ? 1u : 0u, selected ? 1u : 0u,
        accepted ? 1u : 0u, sameChaseNotProposed ? 1u : 0u,
        followStarted ? 1u : 0u, followStaleRejected ? 1u : 0u,
        followChaseAccepted ? 1u : 0u, followDroppedOnChaseExpiry ? 1u : 0u, alreadyReachableNotChased ? 1u : 0u,
        immobileSelected ? 1u : 0u, immobileRejected ? 1u : 0u,
        staleSelected ? 1u : 0u, staleVictimRejected ? 1u : 0u,
        pointMovementPreserved ? 1u : 0u, cleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: ChaseTargetAction used Core MoveChase for the live victim, safely superseded a valid party Follow with Core's targeted-generator expiry semantics, preserved combat and threat state, skipped redundant chases, rejected a stale victim, and preserved unrelated Point movement.");
    return passed;
}


bool RunEngineCoreStopStaleChaseActionSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor staleTarget;
    SimulatorActor currentVictim;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 10.0f, 6.0f, staleTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 18.0f, 6.0f, currentVictim);

    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = actorsCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot, 0.0f, 6.0f, 4.0f);
    WorldSession leaderSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* leader = nullptr;
    bool const leaderCreated = botCreated &&
        CreateSimulatorPlayer(map, partyFaction, leaderSession, leader,
                              -3.0f, 6.0f, 4.0f);

    if (!leaderCreated)
    {
        if (leaderSession.GetPlayer())
            leaderSession.LogoutPlayer(false);
        else if (leader)
            delete leader;
        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        else if (bot)
            delete bot;
        bool cleaned = DestroySimulatorActor(map, currentVictim);
        cleaned = DestroySimulatorActor(map, staleTarget) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] StopStaleChaseAction fixture creation failed (cleanup=%u).",
            cleaned ? 1u : 0u);
        return false;
    }

    PartyBotAI actionRuntime(
        bot, map.GetId(), map.GetInstanceId(),
        bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetOrientation());
    actionRuntime.SetPlayer(bot);
    actionRuntime.m_initialized = true;
    actionRuntime.m_updateTimer.Reset(600000);

    Guid const staleGuid = staleTarget.creature->GetObjectGuid().GetRawValue();
    Guid const currentGuid = currentVictim.creature->GetObjectGuid().GetRawValue();
    MotionMaster* const motion = bot->GetMotionMaster();

    motion->MoveFollow(leader, 2.5f, 3.14159265358979323846f);
    bool const followStarted =
        motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
    bool const firstAttackStarted = bot->Attack(staleTarget.creature, true);
    bot->SetInCombatWith(staleTarget.creature);
    bot->SetSelectionGuid(staleTarget.creature->GetObjectGuid());
    bot->SetTargetGuid(staleTarget.creature->GetObjectGuid());
    motion->MoveChase(staleTarget.creature);
    ChaseMovementGenerator<Player> const* const initialChase =
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE
            ? static_cast<ChaseMovementGenerator<Player> const*>(motion->GetCurrent())
            : nullptr;
    bool const initialChaseStarted =
        initialChase && initialChase->GetTarget() == staleTarget.creature;

    auto makeSnapshot = [&](Guid victimGuid, Guid chaseGuid, uint32 epoch)
    {
        PartySnapshot snapshot;
        snapshot.groupId = 0xF44u;
        snapshot.mapId = map.GetId();
        snapshot.instanceId = map.GetInstanceId();
        snapshot.epoch = epoch;
        snapshot.capturedAtMs = WorldTimer::getMSTime();

        PartyMemberSnapshot member;
        member.guid = bot->GetObjectGuid().GetRawValue();
        member.mapId = map.GetId();
        member.instanceId = map.GetInstanceId();
        member.health = bot->GetHealth();
        member.maxHealth = bot->GetMaxHealth();
        member.power = bot->GetPower(bot->GetPowerType());
        member.maxPower = bot->GetMaxPower(bot->GetPowerType());
        member.online = bot->IsInWorld();
        member.alive = bot->IsAlive();
        member.victimGuid = victimGuid;
        member.chaseTargetGuid = chaseGuid;
        member.selectionGuid = bot->GetSelectionGuid().GetRawValue();
        member.inCombat = bot->IsInCombat();
        snapshot.members.push_back(member);
        return snapshot;
    };

    PartyBotEngine engine;
    std::shared_ptr<StopStaleChaseAction> action(new StopStaleChaseAction());
    bool const registered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_stop_stale_chase"));
    strategy->AddTrigger(std::make_shared<SimulatorStopStaleChaseTrigger>());
    engine.AddStrategy(strategy);

    PartySnapshot sameVictimSnapshot = makeSnapshot(staleGuid, staleGuid, 0xF440u);
    PartyCoordinator sameVictimCoordinator;
    DecisionContext sameVictimContext(
        sameVictimSnapshot, sameVictimSnapshot.members[0], sameVictimCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport sameVictimReport = engine.Evaluate(sameVictimContext);
    bool const sameTargetNotProposed = firstAttackStarted && initialChaseStarted &&
        sameVictimReport.candidates.empty() &&
        sameVictimReport.selectedIndices.empty() &&
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;

    bool const switchedToNewVictim = bot->Attack(currentVictim.creature, true);
    bot->SetInCombatWith(currentVictim.creature);
    bot->SetSelectionGuid(currentVictim.creature->GetObjectGuid());
    bot->SetTargetGuid(currentVictim.creature->GetObjectGuid());
    bool const oldChaseStillCurrent =
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
        static_cast<ChaseMovementGenerator<Player> const*>(motion->GetCurrent())->GetTarget() ==
            staleTarget.creature;
    PartySnapshot staleSnapshot = makeSnapshot(currentGuid, staleGuid, 0xF441u);
    PartyCoordinator staleCoordinator;
    DecisionContext staleContext(
        staleSnapshot, staleSnapshot.members[0], staleCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport staleReport = engine.Evaluate(staleContext);
    bool const staleSelected = registered && switchedToNewVictim &&
        oldChaseStillCurrent && staleReport.selectedIndices.size() == 1 &&
        staleReport.candidates[staleReport.selectedIndices[0]].status == CandidateStatus::Selected;

    uint32 const healthBefore = bot->GetHealth();
    Powers const powerType = bot->GetPowerType();
    uint32 const powerBefore = bot->GetPower(powerType);
    uint32 const staleHealthBefore = staleTarget.creature->GetHealth();
    uint32 const victimHealthBefore = currentVictim.creature->GetHealth();
    float const staleThreatBefore = staleTarget.creature->GetThreatManager().getThreat(bot);
    float const victimThreatBefore = currentVictim.creature->GetThreatManager().getThreat(bot);
    Spell* spellsBefore[CURRENT_MAX_SPELL] = {};
    for (uint32 spellIndex = 0; spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        spellsBefore[spellIndex] = bot->GetCurrentSpell(CurrentSpellTypes(spellIndex));

    if (staleSelected)
        engine.ExecuteSelected(staleContext, staleReport);
    CandidateEvaluation const* staleEvaluation = staleSelected
        ? &staleReport.candidates[staleReport.selectedIndices[0]] : nullptr;

    bool targetedMovementRemains = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
    {
        MovementGeneratorType const type = (*it)->GetMovementGeneratorType();
        if (type == CHASE_MOTION_TYPE || type == FOLLOW_MOTION_TYPE)
            targetedMovementRemains = true;
    }
    bool spellsPreserved = true;
    for (uint32 spellIndex = 0; spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
    {
        if (bot->GetCurrentSpell(CurrentSpellTypes(spellIndex)) != spellsBefore[spellIndex])
        {
            spellsPreserved = false;
            break;
        }
    }
    bool const combatStatePreserved =
        bot->GetVictim() == currentVictim.creature &&
        bot->GetSelectionGuid() == currentVictim.creature->GetObjectGuid() &&
        bot->GetTargetGuid() == currentVictim.creature->GetObjectGuid() &&
        bot->IsInCombat() && bot->GetHealth() == healthBefore &&
        bot->GetPower(powerType) == powerBefore && spellsPreserved &&
        currentVictim.creature->GetHealth() == victimHealthBefore &&
        staleTarget.creature->GetHealth() == staleHealthBefore &&
        std::fabs(currentVictim.creature->GetThreatManager().getThreat(bot) -
                  victimThreatBefore) <= 0.001f &&
        std::fabs(staleTarget.creature->GetThreatManager().getThreat(bot) -
                  staleThreatBefore) <= 0.001f;
    bool const staleAccepted = staleEvaluation &&
        staleEvaluation->status == CandidateStatus::Executed &&
        staleEvaluation->actionResult == ActionResult::Success &&
        staleEvaluation->reason ==
            "stop_stale_chase_expired_targeted_generators_preserving_combat" &&
        !targetedMovementRemains &&
        motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE &&
        combatStatePreserved &&
        !bot->HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE |
                           UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    bool const followDroppedOnExpiry = staleAccepted &&
        !bot->HasUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);

    // A Point below a stale Chase is unrelated movement; fail closed before
    // MovementExpired(false) can expose or disturb that Point generator.
    motion->MovePoint(0xF442u, bot->GetPositionX() + 8.0f,
                      bot->GetPositionY(), bot->GetPositionZ());
    MovementGenerator const* const pointBefore = motion->GetCurrent();
    motion->MoveChase(staleTarget.creature);
    MovementGenerator const* const stackedChase = motion->GetCurrent();
    bool const pointChaseStarted = pointBefore &&
        pointBefore->GetMovementGeneratorType() == POINT_MOTION_TYPE &&
        stackedChase && stackedChase->GetMovementGeneratorType() == CHASE_MOTION_TYPE;
    PartySnapshot pointSnapshot = makeSnapshot(currentGuid, staleGuid, 0xF443u);
    PartyCoordinator pointCoordinator;
    DecisionContext pointContext(
        pointSnapshot, pointSnapshot.members[0], pointCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport pointReport = engine.Evaluate(pointContext);
    bool const pointSelected = pointChaseStarted &&
        pointReport.selectedIndices.size() == 1 &&
        pointReport.candidates[pointReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (pointSelected)
        engine.ExecuteSelected(pointContext, pointReport);
    CandidateEvaluation const* pointEvaluation = pointSelected
        ? &pointReport.candidates[pointReport.selectedIndices[0]] : nullptr;
    bool pointStillStacked = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
        if (*it == pointBefore)
            pointStillStacked = true;
    bool const unrelatedMovementRejected = pointEvaluation &&
        pointEvaluation->status == CandidateStatus::ExecutionFailed &&
        pointEvaluation->actionResult == ActionResult::Useless &&
        pointEvaluation->reason == "stop_stale_chase_unrelated_movement_stacked" &&
        motion->GetCurrent() == stackedChase &&
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
        pointStillStacked && bot->GetVictim() == currentVictim.creature;
    motion->MovementExpired(false);
    bool const pointRestored = motion->GetCurrent() == pointBefore &&
        motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    motion->MovementExpired(false);
    bot->StopMoving();

    // Snapshot-to-dispatch race: another system replaces the old Chase with a
    // current-victim Chase after evaluation. The old candidate must not cancel it.
    motion->MoveChase(staleTarget.creature);
    MovementGenerator const* const oldRaceChase = motion->GetCurrent();
    PartySnapshot raceSnapshot = makeSnapshot(currentGuid, staleGuid, 0xF444u);
    PartyCoordinator raceCoordinator;
    DecisionContext raceContext(
        raceSnapshot, raceSnapshot.members[0], raceCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport raceReport = engine.Evaluate(raceContext);
    bool const raceSelected = raceReport.selectedIndices.size() == 1 &&
        raceReport.candidates[raceReport.selectedIndices[0]].status == CandidateStatus::Selected;
    motion->MoveChase(currentVictim.creature);
    MovementGenerator const* const replacementChase = motion->GetCurrent();
    bool const replacementStarted = replacementChase && replacementChase != oldRaceChase &&
        replacementChase->GetMovementGeneratorType() == CHASE_MOTION_TYPE &&
        static_cast<ChaseMovementGenerator<Player> const*>(replacementChase)->GetTarget() ==
            currentVictim.creature;
    if (raceSelected)
        engine.ExecuteSelected(raceContext, raceReport);
    CandidateEvaluation const* raceEvaluation = raceSelected
        ? &raceReport.candidates[raceReport.selectedIndices[0]] : nullptr;
    bool const staleSnapshotRejected = raceEvaluation &&
        raceEvaluation->status == CandidateStatus::ExecutionFailed &&
        raceEvaluation->actionResult == ActionResult::Impossible &&
        raceEvaluation->reason == "stop_stale_chase_live_generator_mismatch" &&
        motion->GetCurrent() == replacementChase &&
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
        bot->GetVictim() == currentVictim.creature;

    motion->MovementExpired(false);
    bot->StopMoving();
    bot->AttackStop();
    bot->CombatStop(false);

    bot->SetAI(nullptr);
    actionRuntime.Remove();
    botSession.LogoutPlayer(false);
    bool const botCleaned = botSession.GetPlayer() == nullptr;
    leaderSession.LogoutPlayer(false);
    bool const leaderCleaned = leaderSession.GetPlayer() == nullptr;
    bool const actorsCleaned =
        DestroySimulatorActor(map, currentVictim) &&
        DestroySimulatorActor(map, staleTarget);
    bool const cleaned = botCleaned && leaderCleaned && actorsCleaned;

    bool const passed = followStarted && firstAttackStarted && initialChaseStarted &&
        sameTargetNotProposed && switchedToNewVictim && oldChaseStillCurrent &&
        staleSelected && staleAccepted && followDroppedOnExpiry &&
        unrelatedMovementRejected && pointRestored && raceSelected &&
        replacementStarted && staleSnapshotRejected && cleaned;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] StopStaleChaseAction: registered=%u followStarted=%u firstAttack=%u chaseStarted=%u sameTargetNotProposed=%u switchedVictim=%u oldChaseCurrent=%u staleSelected=%u staleAccepted=%u followDroppedOnExpiry=%u unrelatedMovementRejected=%u pointPreserved=%u raceSelected=%u replacementStarted=%u staleSnapshotRejected=%u cleanup=%u.",
        registered ? 1u : 0u, followStarted ? 1u : 0u,
        firstAttackStarted ? 1u : 0u, initialChaseStarted ? 1u : 0u,
        sameTargetNotProposed ? 1u : 0u, switchedToNewVictim ? 1u : 0u,
        oldChaseStillCurrent ? 1u : 0u, staleSelected ? 1u : 0u,
        staleAccepted ? 1u : 0u, followDroppedOnExpiry ? 1u : 0u,
        unrelatedMovementRejected ? 1u : 0u, pointRestored ? 1u : 0u,
        raceSelected ? 1u : 0u, replacementStarted ? 1u : 0u,
        staleSnapshotRejected ? 1u : 0u, cleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: StopStaleChaseAction expired only a snapshot-matched stale Core Chase, allowed Core to drop its underlying Follow, refused unrelated Point movement, and rejected a stale snapshot after a replacement Chase became current.");
    return passed;
}


bool RunEngineCoreStopChaseSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor target, replacement;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 14.0f, 6.0f, target) &&
        CreateSimulatorActor(map, *info, hostileFaction, 20.0f, 6.0f, replacement);
    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = actorsCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot, 0.0f, 6.0f, 4.0f);
    WorldSession leaderSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* leader = nullptr;
    bool const leaderCreated = botCreated &&
        CreateSimulatorPlayer(map, partyFaction, leaderSession, leader, -3.0f, 6.0f, 4.0f);
    if (!leaderCreated)
    {
        if (leaderSession.GetPlayer()) leaderSession.LogoutPlayer(false);
        else if (leader) delete leader;
        if (botSession.GetPlayer()) botSession.LogoutPlayer(false);
        else if (bot) delete bot;
        bool cleaned = DestroySimulatorActor(map, replacement);
        cleaned = DestroySimulatorActor(map, target) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] StopChaseAction fixture creation failed (cleanup=%u).",
            cleaned ? 1u : 0u);
        return false;
    }

    PartyBotAI actionRuntime(bot, map.GetId(), map.GetInstanceId(),
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
    actionRuntime.SetPlayer(bot);
    actionRuntime.m_initialized = true;
    actionRuntime.m_updateTimer.Reset(600000);
    MotionMaster* const motion = bot->GetMotionMaster();
    Guid const targetGuid = target.creature->GetObjectGuid().GetRawValue();

    motion->MoveFollow(leader, 2.5f, 3.14159265358979323846f);
    bool const followStarted = motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
    bool const attackStarted = bot->Attack(target.creature, true);
    bot->SetInCombatWith(target.creature);
    bot->SetSelectionGuid(target.creature->GetObjectGuid());
    bot->SetTargetGuid(target.creature->GetObjectGuid());
    motion->MoveChase(target.creature);
    ChaseMovementGenerator<Player> const* chase =
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE
            ? static_cast<ChaseMovementGenerator<Player> const*>(motion->GetCurrent()) : nullptr;
    bool const chaseStarted = chase && chase->GetTarget() == target.creature;
    bool followStackedBefore = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
        followStackedBefore |= (*it)->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE;

    auto makeSnapshot = [&](uint32 epoch)
    {
        PartySnapshot value;
        value.groupId = 0xF450u;
        value.mapId = map.GetId();
        value.instanceId = map.GetInstanceId();
        value.epoch = epoch;
        value.capturedAtMs = WorldTimer::getMSTime();
        PartyMemberSnapshot member;
        member.guid = bot->GetObjectGuid().GetRawValue();
        member.mapId = map.GetId();
        member.instanceId = map.GetInstanceId();
        member.online = bot->IsInWorld();
        member.alive = bot->IsAlive();
        member.health = bot->GetHealth();
        member.maxHealth = bot->GetMaxHealth();
        member.power = bot->GetPower(bot->GetPowerType());
        member.maxPower = bot->GetMaxPower(bot->GetPowerType());
        member.inCombat = bot->IsInCombat();
        member.victimGuid = targetGuid;
        member.chaseTargetGuid = targetGuid;
        member.selectionGuid = bot->GetSelectionGuid().GetRawValue();
        value.members.push_back(member);
        HostileSnapshot hostile;
        hostile.guid = targetGuid;
        hostile.entry = target.creature->GetEntry();
        hostile.health = target.creature->GetHealth();
        hostile.maxHealth = target.creature->GetMaxHealth();
        hostile.x = target.creature->GetPositionX();
        hostile.y = target.creature->GetPositionY();
        hostile.z = target.creature->GetPositionZ();
        hostile.positionValid = true;
        hostile.alive = target.creature->IsAlive();
        value.hostiles.push_back(hostile);
        return value;
    };

    PartyBotEngine engine;
    std::shared_ptr<StopChaseAction> action(new StopChaseAction());
    bool const registered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_stop_chase"));
    strategy->AddTrigger(std::make_shared<SimulatorStopChaseTrigger>());
    engine.AddStrategy(strategy);
    PartySnapshot firstSnapshot = makeSnapshot(0xF450u);
    PartyCoordinator firstCoordinator;
    DecisionContext firstContext(firstSnapshot, firstSnapshot.members[0], firstCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport firstReport = engine.Evaluate(firstContext);
    bool const selected = registered && followStarted && attackStarted && chaseStarted &&
        followStackedBefore && firstReport.selectedIndices.size() == 1 &&
        firstReport.candidates[firstReport.selectedIndices[0]].status == CandidateStatus::Selected;

    uint32 const health = bot->GetHealth();
    Powers const powerType = bot->GetPowerType();
    uint32 const power = bot->GetPower(powerType);
    uint32 const targetHealth = target.creature->GetHealth();
    float const threat = target.creature->GetThreatManager().getThreat(bot);
    ObjectGuid const selection = bot->GetSelectionGuid();
    ObjectGuid const selectedTarget = bot->GetTargetGuid();
    bool const combat = bot->IsInCombat();
    bool const melee = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
    bool const attackerReference =
        std::find(target.creature->GetAttackers().begin(),
                  target.creature->GetAttackers().end(), bot) !=
        target.creature->GetAttackers().end();
    Spell* spells[CURRENT_MAX_SPELL] = {};
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
        spells[i] = bot->GetCurrentSpell(CurrentSpellTypes(i));

    if (selected) engine.ExecuteSelected(firstContext, firstReport);
    CandidateEvaluation const* accepted = selected
        ? &firstReport.candidates[firstReport.selectedIndices[0]] : nullptr;
    bool targetedRemains = false, followRemains = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
    {
        MovementGeneratorType const type = (*it)->GetMovementGeneratorType();
        targetedRemains |= type == CHASE_MOTION_TYPE || type == FOLLOW_MOTION_TYPE;
        followRemains |= type == FOLLOW_MOTION_TYPE;
    }
    bool spellsPreserved = true;
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
        spellsPreserved &= bot->GetCurrentSpell(CurrentSpellTypes(i)) == spells[i];
    bool const attackerAfter =
        std::find(target.creature->GetAttackers().begin(),
                  target.creature->GetAttackers().end(), bot) !=
        target.creature->GetAttackers().end();
    bool const statePreserved =
        bot->GetVictim() == target.creature &&
        bot->GetSelectionGuid() == selection && bot->GetTargetGuid() == selectedTarget &&
        bot->IsInCombat() == combat &&
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == melee &&
        bot->GetHealth() == health && bot->GetPower(powerType) == power &&
        target.creature->GetHealth() == targetHealth &&
        std::fabs(target.creature->GetThreatManager().getThreat(bot) - threat) <= 0.001f &&
        attackerAfter == attackerReference && spellsPreserved;
    bool const stopped = accepted && accepted->status == CandidateStatus::Executed &&
        accepted->actionResult == ActionResult::Success &&
        accepted->reason == "stop_chase_expired_targeted_movement_preserving_combat" &&
        !targetedRemains &&
        !bot->HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE |
                           UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    bool const followDropped = stopped && !followRemains;

    // A stale snapshot may select, but the final live preflight must reject
    // the already-expired generator without a second movement mutation.
    PartySnapshot staleSnapshot = makeSnapshot(0xF451u);
    PartyCoordinator staleCoordinator;
    DecisionContext staleContext(staleSnapshot, staleSnapshot.members[0], staleCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport staleReport = engine.Evaluate(staleContext);
    bool const staleSelected = staleReport.selectedIndices.size() == 1;
    if (staleSelected) engine.ExecuteSelected(staleContext, staleReport);
    CandidateEvaluation const* stale = staleSelected
        ? &staleReport.candidates[staleReport.selectedIndices[0]] : nullptr;
    bool const staleRejected = stale && stale->status == CandidateStatus::ExecutionFailed &&
        stale->actionResult == ActionResult::Useless &&
        stale->reason == "stop_chase_no_current_chase";

    // An unrelated Point under Chase must survive; reject before Core expiry.
    motion->MovePoint(0xF452u, bot->GetPositionX() + 8.0f,
                      bot->GetPositionY(), bot->GetPositionZ());
    MovementGenerator const* point = motion->GetCurrent();
    motion->MoveChase(target.creature);
    MovementGenerator const* stackedChase = motion->GetCurrent();
    PartySnapshot pointSnapshot = makeSnapshot(0xF453u);
    PartyCoordinator pointCoordinator;
    DecisionContext pointContext(pointSnapshot, pointSnapshot.members[0], pointCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport pointReport = engine.Evaluate(pointContext);
    bool const pointSelected = point && stackedChase && pointReport.selectedIndices.size() == 1;
    if (pointSelected) engine.ExecuteSelected(pointContext, pointReport);
    CandidateEvaluation const* pointResult = pointSelected
        ? &pointReport.candidates[pointReport.selectedIndices[0]] : nullptr;
    bool pointStillStacked = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
        pointStillStacked |= *it == point;
    bool const pointRejected = pointResult &&
        pointResult->status == CandidateStatus::ExecutionFailed &&
        pointResult->actionResult == ActionResult::Useless &&
        pointResult->reason == "stop_chase_unrelated_movement_stacked" &&
        motion->GetCurrent() == stackedChase && pointStillStacked;
    motion->MovementExpired(false);
    bool const pointRestored = motion->GetCurrent() == point &&
        motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    motion->MovementExpired(false);
    bot->StopMoving();

    // Change the live victim after evaluation; the old stop cannot expire Chase.
    motion->MoveChase(target.creature);
    PartySnapshot raceSnapshot = makeSnapshot(0xF454u);
    PartyCoordinator raceCoordinator;
    DecisionContext raceContext(raceSnapshot, raceSnapshot.members[0], raceCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport raceReport = engine.Evaluate(raceContext);
    bool const raceSelected = raceReport.selectedIndices.size() == 1;
    bool const replacementAttack = bot->Attack(replacement.creature, true);
    bot->SetInCombatWith(replacement.creature);
    bot->SetSelectionGuid(replacement.creature->GetObjectGuid());
    bot->SetTargetGuid(replacement.creature->GetObjectGuid());
    MovementGenerator const* oldChase = motion->GetCurrent();
    bool const oldChaseCurrent = oldChase && oldChase->GetMovementGeneratorType() == CHASE_MOTION_TYPE &&
        static_cast<ChaseMovementGenerator<Player> const*>(oldChase)->GetTarget() == target.creature;
    if (raceSelected) engine.ExecuteSelected(raceContext, raceReport);
    CandidateEvaluation const* raced = raceSelected
        ? &raceReport.candidates[raceReport.selectedIndices[0]] : nullptr;
    bool const victimRaceRejected = raced &&
        raced->status == CandidateStatus::ExecutionFailed &&
        raced->actionResult == ActionResult::Impossible &&
        raced->reason == "stop_chase_live_victim_mismatch" &&
        motion->GetCurrent() == oldChase && oldChaseCurrent &&
        bot->GetVictim() == replacement.creature;

    motion->MovementExpired(false);
    bot->StopMoving();
    bot->AttackStop();
    bot->CombatStop(false);
    bot->SetAI(nullptr);
    actionRuntime.Remove();
    botSession.LogoutPlayer(false);
    bool const botCleaned = botSession.GetPlayer() == nullptr;
    leaderSession.LogoutPlayer(false);
    bool const leaderCleaned = leaderSession.GetPlayer() == nullptr;
    bool const actorsCleaned =
        DestroySimulatorActor(map, replacement) && DestroySimulatorActor(map, target);
    bool const cleaned = botCleaned && leaderCleaned && actorsCleaned;
    bool const passed = selected && stopped && followDropped && statePreserved &&
        staleRejected && pointRejected && pointRestored && raceSelected &&
        replacementAttack && victimRaceRejected && cleaned;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] StopChaseAction: registered=%u selected=%u followStackedBefore=%u stopped=%u followDropped=%u statePreserved=%u staleRejected=%u pointRejected=%u pointRestored=%u victimRaceRejected=%u cleanup=%u.",
        registered ? 1u : 0u, selected ? 1u : 0u, followStackedBefore ? 1u : 0u,
        stopped ? 1u : 0u, followDropped ? 1u : 0u, statePreserved ? 1u : 0u,
        staleRejected ? 1u : 0u, pointRejected ? 1u : 0u,
        pointRestored ? 1u : 0u, victimRaceRejected ? 1u : 0u, cleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: StopChaseAction expired only a current-victim Core Chase, allowed Core to remove its underlying Follow, preserved victim/selection/combat/melee/spell/health/power/threat state, and rejected stale or unrelated movement before side effects.");
    return passed;
}



bool RunEngineCoreMoveDistanceSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor target, replacement;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 5.0f, 6.0f, target) &&
        CreateSimulatorActor(map, *info, hostileFaction, 18.0f, 6.0f, replacement);
    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = actorsCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot, 0.0f, 6.0f, 4.0f);
    WorldSession leaderSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* leader = nullptr;
    bool const leaderCreated = botCreated &&
        CreateSimulatorPlayer(map, partyFaction, leaderSession, leader, -3.0f, 6.0f, 4.0f);
    if (!leaderCreated)
    {
        if (leaderSession.GetPlayer()) leaderSession.LogoutPlayer(false);
        else if (leader) delete leader;
        if (botSession.GetPlayer()) botSession.LogoutPlayer(false);
        else if (bot) delete bot;
        bool cleaned = DestroySimulatorActor(map, replacement);
        cleaned = DestroySimulatorActor(map, target) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] MoveDistanceAction fixture creation failed (cleanup=%u).",
            cleaned ? 1u : 0u);
        return false;
    }

    PartyBotAI actionRuntime(bot, map.GetId(), map.GetInstanceId(),
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
    actionRuntime.SetPlayer(bot);
    actionRuntime.m_initialized = true;
    actionRuntime.m_updateTimer.Reset(600000);
    MotionMaster* const motion = bot->GetMotionMaster();

    // Establish the victim first. Unit::Attack may start a Chase; explicitly
    // issue Follow afterwards so this first matrix exercises the current
    // FOLLOW generator under Distancing, not a Follow hidden under Chase.
    bool const attackStarted = bot->Attack(target.creature, true);
    bot->SetInCombatWith(target.creature);
    bot->SetSelectionGuid(target.creature->GetObjectGuid());
    bot->SetTargetGuid(target.creature->GetObjectGuid());
    motion->MoveFollow(leader, 2.5f, 3.14159265358979323846f);
    bool const followStarted = motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
    FollowMovementGenerator<Player> const* const originalFollow = followStarted
        ? static_cast<FollowMovementGenerator<Player> const*>(motion->GetCurrent()) : nullptr;

    auto makeSnapshot = [&](uint32 epoch)
    {
        PartySnapshot value;
        value.groupId = 0xF460u;
        value.mapId = map.GetId();
        value.instanceId = map.GetInstanceId();
        value.epoch = epoch;
        value.capturedAtMs = WorldTimer::getMSTime();
        PartyMemberSnapshot member;
        member.guid = bot->GetObjectGuid().GetRawValue();
        member.mapId = map.GetId();
        member.instanceId = map.GetInstanceId();
        member.health = bot->GetHealth();
        member.maxHealth = bot->GetMaxHealth();
        member.power = bot->GetPower(bot->GetPowerType());
        member.maxPower = bot->GetMaxPower(bot->GetPowerType());
        member.online = bot->IsInWorld();
        member.alive = bot->IsAlive();
        member.inCombat = bot->IsInCombat();
        Unit* victim = bot->GetVictim();
        member.victimGuid = victim ? victim->GetObjectGuid().GetRawValue() : 0;
        member.selectionGuid = bot->GetSelectionGuid().GetRawValue();
        MovementGenerator const* current = motion->GetCurrent();
        if (current && current->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            member.followTargetGuid =
                static_cast<FollowMovementGenerator<Player> const*>(current)
                    ->GetTarget()->GetObjectGuid().GetRawValue();
        if (current && current->GetMovementGeneratorType() == CHASE_MOTION_TYPE)
            member.chaseTargetGuid =
                static_cast<ChaseMovementGenerator<Player> const*>(current)
                    ->GetTarget()->GetObjectGuid().GetRawValue();
        member.x = bot->GetPositionX();
        member.y = bot->GetPositionY();
        member.z = bot->GetPositionZ();
        value.members.push_back(member);

        // The follow preflight requires the same value-only party-roster fact
        // PartySnapshotBuilder would copy for a live Group member.
        PartyMemberSnapshot leaderMember;
        leaderMember.guid = leader->GetObjectGuid().GetRawValue();
        leaderMember.mapId = map.GetId();
        leaderMember.instanceId = map.GetInstanceId();
        leaderMember.health = leader->GetHealth();
        leaderMember.maxHealth = leader->GetMaxHealth();
        leaderMember.power = leader->GetPower(leader->GetPowerType());
        leaderMember.maxPower = leader->GetMaxPower(leader->GetPowerType());
        leaderMember.online = leader->IsInWorld();
        leaderMember.alive = leader->IsAlive();
        leaderMember.x = leader->GetPositionX();
        leaderMember.y = leader->GetPositionY();
        leaderMember.z = leader->GetPositionZ();
        value.members.push_back(leaderMember);

        auto addHostile = [&](Creature* creature)
        {
            HostileSnapshot hostile;
            hostile.guid = creature->GetObjectGuid().GetRawValue();
            hostile.entry = creature->GetEntry();
            hostile.health = creature->GetHealth();
            hostile.maxHealth = creature->GetMaxHealth();
            hostile.x = creature->GetPositionX();
            hostile.y = creature->GetPositionY();
            hostile.z = creature->GetPositionZ();
            hostile.positionValid = true;
            hostile.alive = creature->IsAlive();
            value.hostiles.push_back(hostile);
        };
        addHostile(target.creature);
        addHostile(replacement.creature);
        return value;
    };

    PartyBotEngine engine;
    std::shared_ptr<MoveDistanceAction> action(new MoveDistanceAction(8.0f));
    bool const registered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_move_distance"));
    strategy->AddTrigger(std::make_shared<SimulatorMoveDistanceTrigger>());
    engine.AddStrategy(strategy);

    uint32 const healthBefore = bot->GetHealth();
    Powers const powerType = bot->GetPowerType();
    uint32 const powerBefore = bot->GetPower(powerType);
    uint32 const targetHealthBefore = target.creature->GetHealth();
    float const targetThreatBefore = target.creature->GetThreatManager().getThreat(bot);
    ObjectGuid const selectionBefore = bot->GetSelectionGuid();
    ObjectGuid const selectedTargetBefore = bot->GetTargetGuid();
    bool const combatBefore = bot->IsInCombat();
    bool const meleeBefore = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);

    PartySnapshot followSnapshot = makeSnapshot(0xF460u);
    PartyCoordinator followCoordinator;
    DecisionContext followContext(followSnapshot, followSnapshot.members[0], followCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport followReport = engine.Evaluate(followContext);
    bool const followRosterCaptured =
        followSnapshot.FindMember(followSnapshot.members[0].followTargetGuid) != nullptr;
    bool const followSelected = registered && followStarted && originalFollow &&
        attackStarted && followRosterCaptured &&
        followReport.selectedIndices.size() == 1 &&
        followReport.candidates[followReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (followSelected) engine.ExecuteSelected(followContext, followReport);
    CandidateEvaluation const* followResult = followSelected
        ? &followReport.candidates[followReport.selectedIndices[0]] : nullptr;
    bool const followDistanceStarted = followResult &&
        followResult->status == CandidateStatus::Executed &&
        followResult->actionResult == ActionResult::Success &&
        followResult->reason ==
            "move_distance_core_request_accepted_over_preserved_targeted_movement" &&
        motion->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE;
    bool followPreservedUnderDistance = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
        followPreservedUnderDistance |= *it == originalFollow;
    bool const followStatePreserved =
        bot->GetVictim() == target.creature &&
        bot->GetSelectionGuid() == selectionBefore &&
        bot->GetTargetGuid() == selectedTargetBefore &&
        bot->IsInCombat() == combatBefore &&
        bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == meleeBefore &&
        bot->GetHealth() == healthBefore && bot->GetPower(powerType) == powerBefore &&
        target.creature->GetHealth() == targetHealthBefore &&
        std::fabs(target.creature->GetThreatManager().getThreat(bot) -
                  targetThreatBefore) <= 0.001f;
    motion->MovementExpired(true);
    bool const followRestored = motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
        motion->GetCurrent() == originalFollow && originalFollow->GetTarget() == leader;
    bot->StopMoving();

    motion->MoveChase(target.creature);
    ChaseMovementGenerator<Player> const* const originalChase =
        motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE
            ? static_cast<ChaseMovementGenerator<Player> const*>(motion->GetCurrent()) : nullptr;
    PartySnapshot chaseSnapshot = makeSnapshot(0xF461u);
    PartyCoordinator chaseCoordinator;
    DecisionContext chaseContext(chaseSnapshot, chaseSnapshot.members[0], chaseCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport chaseReport = engine.Evaluate(chaseContext);
    bool const chaseSelected = originalChase &&
        chaseReport.selectedIndices.size() == 1 &&
        chaseReport.candidates[chaseReport.selectedIndices[0]].status == CandidateStatus::Selected;
    if (chaseSelected) engine.ExecuteSelected(chaseContext, chaseReport);
    CandidateEvaluation const* chaseResult = chaseSelected
        ? &chaseReport.candidates[chaseReport.selectedIndices[0]] : nullptr;
    bool const chaseDistanceStarted = chaseResult &&
        chaseResult->status == CandidateStatus::Executed &&
        chaseResult->actionResult == ActionResult::Success &&
        motion->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE;
    bool chasePreservedUnderDistance = false, followStillPresent = false;
    for (MotionMaster::const_iterator it = motion->begin(); it != motion->end(); ++it)
    {
        chasePreservedUnderDistance |= *it == originalChase;
        followStillPresent |= *it == originalFollow;
    }
    motion->MovementExpired(true);
    bool const chaseRestored = motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
        motion->GetCurrent() == originalChase && originalChase->GetTarget() == target.creature;

    // Point is unrelated movement and must remain untouched by the action.
    motion->MovementExpired(false);
    bot->StopMoving();
    motion->MovePoint(0xF462u, bot->GetPositionX() + 8.0f,
                      bot->GetPositionY(), bot->GetPositionZ());
    MovementGenerator const* const point = motion->GetCurrent();
    PartySnapshot pointSnapshot = makeSnapshot(0xF462u);
    PartyCoordinator pointCoordinator;
    DecisionContext pointContext(pointSnapshot, pointSnapshot.members[0], pointCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport pointReport = engine.Evaluate(pointContext);
    bool const pointSelected = point &&
        point->GetMovementGeneratorType() == POINT_MOTION_TYPE &&
        pointReport.selectedIndices.size() == 1;
    if (pointSelected) engine.ExecuteSelected(pointContext, pointReport);
    CandidateEvaluation const* pointResult = pointSelected
        ? &pointReport.candidates[pointReport.selectedIndices[0]] : nullptr;
    bool const pointRejected = pointResult &&
        pointResult->status == CandidateStatus::ExecutionFailed &&
        pointResult->actionResult == ActionResult::Useless &&
        pointResult->reason == "move_distance_unrelated_movement_active" &&
        motion->GetCurrent() == point &&
        motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    motion->MovementExpired(false);
    bot->StopMoving();

    // A victim switch after Evaluate must leave the old chase untouched.
    motion->MoveChase(target.creature);
    PartySnapshot raceSnapshot = makeSnapshot(0xF463u);
    PartyCoordinator raceCoordinator;
    DecisionContext raceContext(raceSnapshot, raceSnapshot.members[0], raceCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr, &actionRuntime);
    DecisionReport raceReport = engine.Evaluate(raceContext);
    bool const raceSelected = raceReport.selectedIndices.size() == 1;
    bool const replacementAttack = bot->Attack(replacement.creature, true);
    bot->SetInCombatWith(replacement.creature);
    bot->SetSelectionGuid(replacement.creature->GetObjectGuid());
    bot->SetTargetGuid(replacement.creature->GetObjectGuid());
    MovementGenerator const* const oldChase = motion->GetCurrent();
    bool const oldChaseCurrent = oldChase &&
        oldChase->GetMovementGeneratorType() == CHASE_MOTION_TYPE &&
        static_cast<ChaseMovementGenerator<Player> const*>(oldChase)->GetTarget() == target.creature;
    if (raceSelected) engine.ExecuteSelected(raceContext, raceReport);
    CandidateEvaluation const* raceResult = raceSelected
        ? &raceReport.candidates[raceReport.selectedIndices[0]] : nullptr;
    bool const victimRaceRejected = raceResult &&
        raceResult->status == CandidateStatus::ExecutionFailed &&
        raceResult->actionResult == ActionResult::Impossible &&
        raceResult->reason == "move_distance_live_victim_mismatch" &&
        motion->GetCurrent() == oldChase && oldChaseCurrent &&
        bot->GetVictim() == replacement.creature;

    motion->MovementExpired(false);
    bot->StopMoving();
    bot->AttackStop();
    bot->CombatStop(false);
    bot->SetAI(nullptr);
    actionRuntime.Remove();
    botSession.LogoutPlayer(false);
    bool const botCleaned = botSession.GetPlayer() == nullptr;
    leaderSession.LogoutPlayer(false);
    bool const leaderCleaned = leaderSession.GetPlayer() == nullptr;
    bool const actorsCleaned =
        DestroySimulatorActor(map, replacement) && DestroySimulatorActor(map, target);
    bool const cleaned = botCleaned && leaderCleaned && actorsCleaned;
    bool const passed = followDistanceStarted && followPreservedUnderDistance &&
        followStatePreserved && followRestored && chaseDistanceStarted &&
        chasePreservedUnderDistance && followStillPresent && chaseRestored &&
        pointRejected && raceSelected && replacementAttack && victimRaceRejected && cleaned;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] MoveDistanceAction: registered=%u followStarted=%u followRosterCaptured=%u followSelected=%u followDistanceStarted=%u followPreserved=%u followRestored=%u chaseSelected=%u chaseStarted=%u chasePreserved=%u followStillPresent=%u chaseRestored=%u pointRejected=%u victimRaceRejected=%u cleanup=%u followResult=%s followActionResult=%u.",
        registered ? 1u : 0u, followStarted ? 1u : 0u,
        followRosterCaptured ? 1u : 0u, followSelected ? 1u : 0u,
        followDistanceStarted ? 1u : 0u, followPreservedUnderDistance ? 1u : 0u,
        followRestored ? 1u : 0u, chaseSelected ? 1u : 0u,
        chaseDistanceStarted ? 1u : 0u, chasePreservedUnderDistance ? 1u : 0u,
        followStillPresent ? 1u : 0u, chaseRestored ? 1u : 0u,
        pointRejected ? 1u : 0u, victimRaceRejected ? 1u : 0u, cleaned ? 1u : 0u,
        followResult ? followResult->reason.c_str() : "no-follow-candidate",
        followResult ? static_cast<uint32>(followResult->actionResult) : 0xffffffffu);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: MoveDistanceAction accepted Core MoveDistance over live Follow and Chase, preserved and restored the underlying targeted movement on Distancing expiry, preserved combat/threat state, and rejected unrelated Point and stale-victim requests.");
    return passed;
}




bool RunEngineCoreDpsAssistSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor mainTankTarget;
    SimulatorActor healerPeelTarget;
    SimulatorActor crowdControlTarget;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 12.0f, 6.0f,
                             mainTankTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 18.0f, 6.0f,
                             healerPeelTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 24.0f, 6.0f,
                             crowdControlTarget);

    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    bool const botCreated = actorsCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot,
                              0.0f, 6.0f, 4.0f);
    WorldSession mainTankSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* mainTank = nullptr;
    bool const mainTankCreated = botCreated &&
        CreateSimulatorPlayer(map, partyFaction, mainTankSession, mainTank,
                              -3.0f, 6.0f, 4.0f);
    WorldSession healerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* healer = nullptr;
    bool const healerCreated = mainTankCreated &&
        CreateSimulatorPlayer(map, partyFaction, healerSession, healer,
                              -5.0f, 6.0f, 4.0f);

    if (!healerCreated)
    {
        if (healerSession.GetPlayer())
            healerSession.LogoutPlayer(false);
        else if (healer)
            delete healer;
        if (mainTankSession.GetPlayer())
            mainTankSession.LogoutPlayer(false);
        else if (mainTank)
            delete mainTank;
        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        else if (bot)
            delete bot;

        bool cleaned = DestroySimulatorActor(map, crowdControlTarget);
        cleaned = DestroySimulatorActor(map, healerPeelTarget) && cleaned;
        cleaned = DestroySimulatorActor(map, mainTankTarget) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[PartyBotSimulator] DpsAssist fixture creation failed (cleanup=%u).",
            cleaned ? 1u : 0u);
        return false;
    }

    PartyBotAI actionRuntime(
        bot, map.GetId(), map.GetInstanceId(),
        bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetOrientation());
    actionRuntime.SetPlayer(bot);
    actionRuntime.m_initialized = true;
    actionRuntime.m_updateTimer.Reset(600000);

    Guid const botGuid = bot->GetObjectGuid().GetRawValue();
    Guid const mainTankGuid = mainTank->GetObjectGuid().GetRawValue();
    Guid const healerGuid = healer->GetObjectGuid().GetRawValue();
    Guid const mainTankTargetGuid =
        mainTankTarget.creature->GetObjectGuid().GetRawValue();
    Guid const healerPeelTargetGuid =
        healerPeelTarget.creature->GetObjectGuid().GetRawValue();
    Guid const crowdControlTargetGuid =
        crowdControlTarget.creature->GetObjectGuid().GetRawValue();

    auto makeSnapshot = [&](uint32 epoch)
    {
        PartySnapshot snapshot;
        snapshot.groupId = 0xF46u;
        snapshot.mapId = map.GetId();
        snapshot.instanceId = map.GetInstanceId();
        snapshot.epoch = epoch;
        snapshot.capturedAtMs = WorldTimer::getMSTime();

        PartyMemberSnapshot botMember;
        botMember.guid = botGuid;
        botMember.roleId = static_cast<uint8>(PartyRole::RangedDamage);
        botMember.mapId = map.GetId();
        botMember.instanceId = map.GetInstanceId();
        botMember.health = bot->GetHealth();
        botMember.maxHealth = bot->GetMaxHealth();
        botMember.power = bot->GetPower(bot->GetPowerType());
        botMember.maxPower = bot->GetMaxPower(bot->GetPowerType());
        botMember.powerType = static_cast<uint8>(bot->GetPowerType());
        botMember.online = bot->IsInWorld();
        botMember.alive = bot->IsAlive();
        Unit* const liveBotVictim = bot->GetVictim();
        botMember.victimGuid = liveBotVictim
            ? liveBotVictim->GetObjectGuid().GetRawValue() : 0;
        botMember.selectionGuid = bot->GetSelectionGuid().GetRawValue();
        botMember.inCombat = bot->IsInCombat();
        snapshot.members.push_back(botMember);

        PartyMemberSnapshot tankMember;
        tankMember.guid = mainTankGuid;
        tankMember.roleId = static_cast<uint8>(PartyRole::MainTank);
        tankMember.mapId = map.GetId();
        tankMember.instanceId = map.GetInstanceId();
        tankMember.health = mainTank->GetHealth();
        tankMember.maxHealth = mainTank->GetMaxHealth();
        tankMember.online = mainTank->IsInWorld();
        tankMember.alive = mainTank->IsAlive();
        tankMember.isMainTank = true;
        tankMember.victimGuid = mainTankTargetGuid;
        snapshot.members.push_back(tankMember);

        PartyMemberSnapshot healerMember;
        healerMember.guid = healerGuid;
        healerMember.roleId = static_cast<uint8>(PartyRole::Healer);
        healerMember.mapId = map.GetId();
        healerMember.instanceId = map.GetInstanceId();
        healerMember.health = healer->GetHealth();
        healerMember.maxHealth = healer->GetMaxHealth();
        healerMember.online = healer->IsInWorld();
        healerMember.alive = healer->IsAlive();
        healerMember.victimGuid = healerPeelTargetGuid;
        snapshot.members.push_back(healerMember);

        HostileSnapshot mainTankHostile;
        mainTankHostile.guid = mainTankTargetGuid;
        mainTankHostile.entry = mainTankTarget.creature->GetEntry();
        mainTankHostile.health = mainTankTarget.creature->GetHealth();
        mainTankHostile.maxHealth = mainTankTarget.creature->GetMaxHealth();
        mainTankHostile.x = mainTankTarget.creature->GetPositionX();
        mainTankHostile.y = mainTankTarget.creature->GetPositionY();
        mainTankHostile.z = mainTankTarget.creature->GetPositionZ();
        mainTankHostile.positionValid = true;
        mainTankHostile.alive = mainTankTarget.creature->IsAlive();
        snapshot.hostiles.push_back(mainTankHostile);

        HostileSnapshot healerPeelHostile;
        healerPeelHostile.guid = healerPeelTargetGuid;
        healerPeelHostile.entry = healerPeelTarget.creature->GetEntry();
        healerPeelHostile.victimGuid = healerGuid;
        healerPeelHostile.health = healerPeelTarget.creature->GetHealth();
        healerPeelHostile.maxHealth = healerPeelTarget.creature->GetMaxHealth();
        healerPeelHostile.x = healerPeelTarget.creature->GetPositionX();
        healerPeelHostile.y = healerPeelTarget.creature->GetPositionY();
        healerPeelHostile.z = healerPeelTarget.creature->GetPositionZ();
        healerPeelHostile.positionValid = true;
        healerPeelHostile.alive = healerPeelTarget.creature->IsAlive();
        snapshot.hostiles.push_back(healerPeelHostile);

        HostileSnapshot ccHostile;
        ccHostile.guid = crowdControlTargetGuid;
        ccHostile.entry = crowdControlTarget.creature->GetEntry();
        ccHostile.health = crowdControlTarget.creature->GetHealth();
        ccHostile.maxHealth = crowdControlTarget.creature->GetMaxHealth();
        ccHostile.x = crowdControlTarget.creature->GetPositionX();
        ccHostile.y = crowdControlTarget.creature->GetPositionY();
        ccHostile.z = crowdControlTarget.creature->GetPositionZ();
        ccHostile.positionValid = true;
        ccHostile.alive = crowdControlTarget.creature->IsAlive();
        ccHostile.crowdControlProtected = true;
        snapshot.hostiles.push_back(ccHostile);
        return snapshot;
    };

    PartyBotEngine engine;
    std::shared_ptr<SelectHostileTargetAction> selectAction(
        new SelectHostileTargetAction());
    std::shared_ptr<AttackTargetAction> attackAction(
        new AttackTargetAction());
    bool const selectRegistered = engine.RegisterAction(selectAction);
    bool const attackRegistered = engine.RegisterAction(attackAction);

    std::shared_ptr<Strategy> strategy(new Strategy("dps_assist"));
    strategy->AddTrigger(std::make_shared<DpsAssistTrigger>());
    engine.AddStrategy(strategy);

    PartySnapshot firstSnapshot = makeSnapshot(0xF460u);
    PartyCoordinator firstCoordinator;
    DecisionContext firstContext(
        firstSnapshot, firstSnapshot.members[0], firstCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport firstReport = engine.Evaluate(firstContext);

    std::size_t mainTankSelectIndex = firstReport.candidates.size();
    std::size_t healerPeelIndex = firstReport.candidates.size();
    bool crowdControlProposed = false;
    for (std::size_t i = 0; i < firstReport.candidates.size(); ++i)
    {
        CandidateEvaluation const& evaluation = firstReport.candidates[i];
        if (evaluation.candidate.targetGuid == mainTankTargetGuid &&
            evaluation.candidate.actionName == "core_select_hostile_target")
            mainTankSelectIndex = i;
        if (evaluation.candidate.targetGuid == healerPeelTargetGuid)
            healerPeelIndex = i;
        if (evaluation.candidate.targetGuid == crowdControlTargetGuid)
            crowdControlProposed = true;
    }

    bool const targetPrioritiesCorrect =
        mainTankSelectIndex < firstReport.candidates.size() &&
        healerPeelIndex < firstReport.candidates.size() &&
        firstReport.candidates[mainTankSelectIndex].candidate.basePriority >
            firstReport.candidates[healerPeelIndex].candidate.basePriority;
    bool const mainTankSelectionChosen = selectRegistered && attackRegistered &&
        targetPrioritiesCorrect && !crowdControlProposed &&
        firstReport.selectedIndices.size() == 1 &&
        firstReport.selectedIndices[0] == mainTankSelectIndex &&
        firstReport.candidates[mainTankSelectIndex].status == CandidateStatus::Selected;

    if (mainTankSelectionChosen)
        engine.ExecuteSelected(firstContext, firstReport);
    CandidateEvaluation const* selectionEvaluation = mainTankSelectionChosen
        ? &firstReport.candidates[mainTankSelectIndex] : nullptr;
    bool const mainTankTargetSelected = selectionEvaluation &&
        selectionEvaluation->status == CandidateStatus::Executed &&
        selectionEvaluation->actionResult == ActionResult::Success &&
        selectionEvaluation->reason == "select_hostile_target_selected" &&
        bot->GetSelectionGuid() == ObjectGuid(mainTankTargetGuid) &&
        bot->GetTargetGuid() == ObjectGuid(mainTankTargetGuid) &&
        bot->GetVictim() == nullptr;

    PartySnapshot secondSnapshot = makeSnapshot(0xF461u);
    PartyCoordinator secondCoordinator;
    DecisionContext secondContext(
        secondSnapshot, secondSnapshot.members[0], secondCoordinator,
        WorldTimer::getMSTime(), nullptr, nullptr, &actionRuntime, nullptr,
        &actionRuntime);
    DecisionReport secondReport = engine.Evaluate(secondContext);

    std::size_t mainTankAttackIndex = secondReport.candidates.size();
    std::size_t secondPeelIndex = secondReport.candidates.size();
    bool secondCrowdControlProposed = false;
    for (std::size_t i = 0; i < secondReport.candidates.size(); ++i)
    {
        CandidateEvaluation const& evaluation = secondReport.candidates[i];
        if (evaluation.candidate.targetGuid == mainTankTargetGuid &&
            evaluation.candidate.actionName == "core_attack_target")
            mainTankAttackIndex = i;
        if (evaluation.candidate.targetGuid == healerPeelTargetGuid)
            secondPeelIndex = i;
        if (evaluation.candidate.targetGuid == crowdControlTargetGuid)
            secondCrowdControlProposed = true;
    }

    bool const attackSelectedOnFreshSnapshot = mainTankTargetSelected &&
        secondPeelIndex < secondReport.candidates.size() &&
        mainTankAttackIndex < secondReport.candidates.size() &&
        secondReport.selectedIndices.size() == 1 &&
        secondReport.selectedIndices[0] == mainTankAttackIndex &&
        secondReport.candidates[mainTankAttackIndex].status == CandidateStatus::Selected &&
        secondReport.candidates[mainTankAttackIndex].candidate.finalPriority >
            secondReport.candidates[secondPeelIndex].candidate.finalPriority &&
        !secondCrowdControlProposed;

    uint32 const mainTargetHealthBefore = mainTankTarget.creature->GetHealth();
    uint32 const peelTargetHealthBefore = healerPeelTarget.creature->GetHealth();
    float const mainTargetThreatBefore =
        mainTankTarget.creature->GetThreatManager().getThreat(bot);
    float const peelTargetThreatBefore =
        healerPeelTarget.creature->GetThreatManager().getThreat(bot);

    if (attackSelectedOnFreshSnapshot)
        engine.ExecuteSelected(secondContext, secondReport);
    CandidateEvaluation const* attackEvaluation = attackSelectedOnFreshSnapshot
        ? &secondReport.candidates[mainTankAttackIndex] : nullptr;
    bool const attackStarted = attackEvaluation &&
        attackEvaluation->status == CandidateStatus::Executed &&
        attackEvaluation->actionResult == ActionResult::Success &&
        attackEvaluation->reason == "attack_target_auto_attack_request_accepted" &&
        bot->GetVictim() == mainTankTarget.creature &&
        bot->GetSelectionGuid() == ObjectGuid(mainTankTargetGuid) &&
        bot->GetTargetGuid() == ObjectGuid(mainTankTargetGuid) &&
        mainTankTarget.creature->GetHealth() == mainTargetHealthBefore &&
        healerPeelTarget.creature->GetHealth() == peelTargetHealthBefore &&
        std::fabs(mainTankTarget.creature->GetThreatManager().getThreat(bot) -
                  mainTargetThreatBefore) <= 0.001f &&
        std::fabs(healerPeelTarget.creature->GetThreatManager().getThreat(bot) -
                  peelTargetThreatBefore) <= 0.001f;

    bot->AttackStop();
    bot->CombatStop(false);
    bot->SetAI(nullptr);
    actionRuntime.Remove();
    botSession.LogoutPlayer(false);
    bool const botCleaned = botSession.GetPlayer() == nullptr;
    mainTankSession.LogoutPlayer(false);
    bool const tankCleaned = mainTankSession.GetPlayer() == nullptr;
    healerSession.LogoutPlayer(false);
    bool const healerCleaned = healerSession.GetPlayer() == nullptr;
    bool const actorsCleaned =
        DestroySimulatorActor(map, crowdControlTarget) &&
        DestroySimulatorActor(map, healerPeelTarget) &&
        DestroySimulatorActor(map, mainTankTarget);
    bool const cleaned = botCleaned && tankCleaned && healerCleaned && actorsCleaned;

    bool const passed = selectRegistered && attackRegistered &&
        targetPrioritiesCorrect && mainTankTargetSelected &&
        attackSelectedOnFreshSnapshot && attackStarted && cleaned;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] DpsAssistTargetPolicy: registered=%u actionsRegistered=%u targetPrioritiesCorrect=%u mainTankSelected=%u ccExcluded=%u stagedAttackSelected=%u attackStarted=%u statePreserved=%u cleanup=%u.",
        (selectRegistered && attackRegistered) ? 1u : 0u,
        (selectRegistered && attackRegistered) ? 1u : 0u,
        targetPrioritiesCorrect ? 1u : 0u,
        mainTankTargetSelected ? 1u : 0u,
        (!crowdControlProposed && !secondCrowdControlProposed) ? 1u : 0u,
        attackSelectedOnFreshSnapshot ? 1u : 0u,
        attackStarted ? 1u : 0u,
        attackStarted ? 1u : 0u,
        cleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: DpsAssistTrigger selected the Main Tank's target ahead of a healer-peel candidate, excluded a crowd-control-protected hostile, and on a fresh snapshot dispatched the existing Core AttackTargetAction.");
    return passed;
}


bool RunEngineCoreDpsAssistGroupSnapshotSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor mainTankTarget;
    SimulatorActor nextMainTankTarget;
    SimulatorActor healerPeelTarget;
    SimulatorActor crowdControlTarget;
    bool const actorsCreated =
        CreateSimulatorActor(map, *info, hostileFaction, 12.0f, 6.0f,
                             mainTankTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 15.0f, 10.0f,
                             nextMainTankTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 18.0f, 6.0f,
                             healerPeelTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 24.0f, 6.0f,
                             crowdControlTarget);

    WorldSession botSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    WorldSession mainTankSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    WorldSession healerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    WorldSession memberFourSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    WorldSession memberFiveSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* bot = nullptr;
    Player* mainTank = nullptr;
    Player* healer = nullptr;
    Player* memberFour = nullptr;
    Player* memberFive = nullptr;

    bool playersCreated = actorsCreated &&
        CreateSimulatorPlayer(map, partyFaction, botSession, bot,
                              0.0f, 6.0f, 4.0f) &&
        CreateSimulatorPlayer(map, partyFaction, mainTankSession, mainTank,
                              -3.0f, 6.0f, 4.0f) &&
        CreateSimulatorPlayer(map, partyFaction, healerSession, healer,
                              -5.0f, 6.0f, 4.0f) &&
        CreateSimulatorPlayer(map, partyFaction, memberFourSession, memberFour,
                              -7.0f, 6.0f, 4.0f) &&
        CreateSimulatorPlayer(map, partyFaction, memberFiveSession, memberFive,
                              -9.0f, 6.0f, 4.0f);

    Player* roster[MAX_GROUP_SIZE] = {
        bot, mainTank, healer, memberFour, memberFive
    };
    ObjectGuid const botGuid = bot ? bot->GetObjectGuid() : ObjectGuid();
    ObjectGuid const mainTankGuid = mainTank ? mainTank->GetObjectGuid() : ObjectGuid();
    ObjectGuid const healerGuid = healer ? healer->GetObjectGuid() : ObjectGuid();
    ObjectGuid const memberFourGuid = memberFour
        ? memberFour->GetObjectGuid() : ObjectGuid();
    ObjectGuid const memberFiveGuid = memberFive
        ? memberFive->GetObjectGuid() : ObjectGuid();
    Guid const mainTankTargetGuid = mainTankTarget.creature
        ? mainTankTarget.creature->GetObjectGuid().GetRawValue() : 0;
    Guid const nextMainTankTargetGuid = nextMainTankTarget.creature
        ? nextMainTankTarget.creature->GetObjectGuid().GetRawValue() : 0;
    Guid const healerPeelTargetGuid = healerPeelTarget.creature
        ? healerPeelTarget.creature->GetObjectGuid().GetRawValue() : 0;
    Guid const crowdControlTargetGuid = crowdControlTarget.creature
        ? crowdControlTarget.creature->GetObjectGuid().GetRawValue() : 0;

    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;
    std::unique_ptr<PartyBotAI> actionRuntime;
    std::unique_ptr<PartyBotAI> healerRoleAI;
    PlayerAI* previousBotAI = nullptr;
    PlayerAI* previousHealerAI = nullptr;

    auto cleanup = [&]() -> bool
    {
        if (bot)
        {
            bot->AttackStop();
            bot->CombatStop(false);
        }
        if (mainTank)
        {
            mainTank->AttackStop();
            mainTank->CombatStop(false);
        }
        if (healer)
        {
            healer->AttackStop();
            healer->CombatStop(false);
        }
        if (healerPeelTarget.creature)
        {
            healerPeelTarget.creature->AttackStop();
            healerPeelTarget.creature->CombatStop(false);
        }

        // Restore borrowed PlayerAI pointers before their owned adapters die.
        if (actionRuntime && bot)
            bot->SetAI(previousBotAI);
        if (healerRoleAI && healer)
            healer->SetAI(previousHealerAI);
        actionRuntime.reset();
        healerRoleAI.reset();

        bool ungrouped = true;
        if (group && group->GetId())
        {
            // Remove Group::Create's synthetic instance bind before Disband so
            // later simulator fixtures can continue using this same test Map.
            group->UnbindInstance(map.GetId(), false);
            group->Disband(true, botGuid);
            for (Player* member : roster)
                if (member && member->GetGroup())
                    ungrouped = false;
            if (groupRegistered)
            {
                sObjectMgr.RemoveGroup(group.get());
                groupRegistered = false;
            }
            group.reset();
        }

        bool actorsCleaned = DestroySimulatorActor(map, crowdControlTarget);
        actorsCleaned = DestroySimulatorActor(map, healerPeelTarget) && actorsCleaned;
        actorsCleaned = DestroySimulatorActor(map, nextMainTankTarget) && actorsCleaned;
        actorsCleaned = DestroySimulatorActor(map, mainTankTarget) && actorsCleaned;

        if (botSession.GetPlayer())
            botSession.LogoutPlayer(false);
        if (mainTankSession.GetPlayer())
            mainTankSession.LogoutPlayer(false);
        if (healerSession.GetPlayer())
            healerSession.LogoutPlayer(false);
        if (memberFourSession.GetPlayer())
            memberFourSession.LogoutPlayer(false);
        if (memberFiveSession.GetPlayer())
            memberFiveSession.LogoutPlayer(false);

        bool const sessionsCleaned =
            !botSession.GetPlayer() && !mainTankSession.GetPlayer() &&
            !healerSession.GetPlayer() && !memberFourSession.GetPlayer() &&
            !memberFiveSession.GetPlayer();
        bool const playersDetached =
            (!botGuid || !map.GetUnit(botGuid)) &&
            (!mainTankGuid || !map.GetUnit(mainTankGuid)) &&
            (!healerGuid || !map.GetUnit(healerGuid)) &&
            (!memberFourGuid || !map.GetUnit(memberFourGuid)) &&
            (!memberFiveGuid || !map.GetUnit(memberFiveGuid));
        bot = nullptr;
        mainTank = nullptr;
        healer = nullptr;
        memberFour = nullptr;
        memberFive = nullptr;
        return ungrouped && actorsCleaned && sessionsCleaned && playersDetached;
    };

    bool groupCreated = false;
    bool groupMembersAdded = false;
    bool mainTankAssigned = false;
    if (playersCreated && bot && mainTank && healer && memberFour && memberFive)
    {
        groupCreated = group->Create(botGuid, bot->GetName());
        if (groupCreated)
        {
            sObjectMgr.AddGroup(group.get());
            groupRegistered = true;
            groupMembersAdded = group->GetMembersCount() == 1;
            for (uint32 index = 1; index < MAX_GROUP_SIZE; ++index)
            {
                Player* member = roster[index];
                bool const added = member && group->AddMember(
                    member->GetObjectGuid(), member->GetName());
                groupMembersAdded = groupMembersAdded && added;
            }
            if (groupMembersAdded)
            {
                group->ConvertToRaid();
                group->SetMainTank(mainTankGuid);
                mainTankAssigned = group->GetMainTankGuid() == mainTankGuid;
            }
        }
    }

    bool rolesInstalled = false;
    if (groupMembersAdded && mainTankAssigned)
    {
        previousBotAI = bot->AI();
        previousHealerAI = healer->AI();
        actionRuntime.reset(new PartyBotAI(
            bot, nullptr, ROLE_RANGE_DPS, bot->GetRace(), bot->GetClass(),
            bot->GetLevel(), map.GetId(), map.GetInstanceId(),
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            bot->GetOrientation()));
        actionRuntime->SetPlayer(bot);
        actionRuntime->m_initialized = true;
        actionRuntime->m_updateTimer.Reset(600000);
        bot->SetAI(actionRuntime.get());

        healerRoleAI.reset(new PartyBotAI(
            bot, nullptr, ROLE_HEALER, healer->GetRace(), healer->GetClass(),
            healer->GetLevel(), map.GetId(), map.GetInstanceId(),
            healer->GetPositionX(), healer->GetPositionY(), healer->GetPositionZ(),
            healer->GetOrientation()));
        healerRoleAI->SetPlayer(healer);
        healer->SetAI(healerRoleAI.get());

        rolesInstalled =
            dynamic_cast<CombatBotBaseAI*>(bot->AI()) == actionRuntime.get() &&
            dynamic_cast<CombatBotBaseAI*>(healer->AI()) == healerRoleAI.get();
    }

    bool mainTankTargetStarted = false;
    bool healerPeelStarted = false;
    bool groupSnapshotValid = false;
    bool initialMainTankPriorityCorrect = false;
    bool initialCrowdControlExcluded = false;
    bool healerRoleSnapshotValid = false;
    bool firstSelectionExecuted = false;
    bool firstSelectionInvalidatedCache = false;
    bool firstAttackExecuted = false;
    bool firstAttackInvalidatedCache = false;
    bool staleCacheObservedBeforeExplicitInvalidation = false;
    bool mainTankRetargeted = false;
    bool explicitRetargetInvalidationWorked = false;
    bool freshHandoffSnapshotValid = false;
    bool handoffSelectionExecuted = false;
    bool handoffSelectionInvalidatedCache = false;
    bool handoffAttackExecuted = false;
    bool handoffAttackInvalidatedCache = false;

    if (rolesInstalled)
    {
        mainTankTargetStarted = mainTank->Attack(mainTankTarget.creature, true) &&
            mainTank->GetVictim() == mainTankTarget.creature;
        healerPeelTarget.creature->AI()->AttackStart(healer);
        healerPeelStarted = healerPeelTarget.creature->GetVictim() == healer;

        PartyCoordinator coordinator;
        PartyBotEngine engine;
        std::shared_ptr<SelectHostileTargetAction> selectAction(
            new SelectHostileTargetAction());
        std::shared_ptr<AttackTargetAction> attackAction(
            new AttackTargetAction());
        bool const selectRegistered = engine.RegisterAction(selectAction);
        bool const attackRegistered = engine.RegisterAction(attackAction);
        std::shared_ptr<Strategy> strategy(new Strategy("dps_assist_group_snapshot"));
        strategy->AddTrigger(std::make_shared<DpsAssistTrigger>());
        engine.AddStrategy(strategy);

        auto buildPolicySnapshot = [&](uint32 nowMs) -> PartySnapshot
        {
            PartySnapshot snapshot;
            PartySnapshot const* built = PartySnapshotBuilder::GetOrBuild(
                bot, coordinator, nowMs, 250);
            if (!built)
                return snapshot;

            snapshot = *built;
            // Builder does not yet infer CC ownership from live auras. Keep
            // one transparent CC veto row as a test-only policy overlay; the
            // roster and assist targets below are all real Core snapshot data.
            HostileSnapshot ccProtected;
            ccProtected.guid = crowdControlTargetGuid;
            ccProtected.entry = crowdControlTarget.creature->GetEntry();
            ccProtected.health = crowdControlTarget.creature->GetHealth();
            ccProtected.maxHealth = crowdControlTarget.creature->GetMaxHealth();
            ccProtected.x = crowdControlTarget.creature->GetPositionX();
            ccProtected.y = crowdControlTarget.creature->GetPositionY();
            ccProtected.z = crowdControlTarget.creature->GetPositionZ();
            ccProtected.positionValid = true;
            ccProtected.alive = crowdControlTarget.creature->IsAlive();
            ccProtected.crowdControlProtected = true;
            snapshot.hostiles.push_back(ccProtected);
            return snapshot;
        };

        auto findCandidate = [](DecisionReport const& report, Guid targetGuid,
                                char const* actionName) -> std::size_t
        {
            for (std::size_t index = 0; index < report.candidates.size(); ++index)
            {
                ActionCandidate const& candidate = report.candidates[index].candidate;
                if (candidate.targetGuid == targetGuid &&
                    candidate.actionName == actionName)
                    return index;
            }
            return report.candidates.size();
        };

        auto hasCandidateForTarget = [](DecisionReport const& report,
                                        Guid targetGuid) -> bool
        {
            for (CandidateEvaluation const& evaluation : report.candidates)
                if (evaluation.candidate.targetGuid == targetGuid)
                    return true;
            return false;
        };

        PartySnapshot firstSnapshot = buildPolicySnapshot(WorldTimer::getMSTime());
        PartyMemberSnapshot const* firstBot = firstSnapshot.FindMember(botGuid.GetRawValue());
        PartyMemberSnapshot const* firstTank = firstSnapshot.FindMember(mainTankGuid.GetRawValue());
        PartyMemberSnapshot const* firstHealer = firstSnapshot.FindMember(healerGuid.GetRawValue());
        HostileSnapshot const* firstTankTarget =
            firstSnapshot.FindHostile(mainTankTargetGuid);
        HostileSnapshot const* firstHealerPeel =
            firstSnapshot.FindHostile(healerPeelTargetGuid);
        healerRoleSnapshotValid = firstHealer && firstHealer->isPlayerBot &&
            firstHealer->roleId == static_cast<uint8>(PartyRole::Healer);
        groupSnapshotValid = selectRegistered && attackRegistered &&
            firstSnapshot.groupId == group->GetId() &&
            firstSnapshot.mapId == map.GetId() &&
            firstSnapshot.instanceId == map.GetInstanceId() &&
            firstSnapshot.members.size() == MAX_GROUP_SIZE &&
            firstBot && firstBot->isPlayerBot &&
            firstBot->roleId == static_cast<uint8>(PartyRole::RangedDamage) &&
            firstTank && firstTank->isMainTank &&
            firstTank->victimGuid == mainTankTargetGuid &&
            firstTankTarget && firstTankTarget->alive &&
            healerRoleSnapshotValid &&
            firstHealerPeel && firstHealerPeel->alive &&
            firstHealerPeel->victimGuid == healerGuid.GetRawValue() &&
            mainTankTargetStarted && healerPeelStarted;

        if (groupSnapshotValid)
        {
            DecisionContext firstContext(
                firstSnapshot, *firstBot, coordinator, WorldTimer::getMSTime(),
                nullptr, nullptr, actionRuntime.get(), nullptr, actionRuntime.get());
            DecisionReport firstReport = engine.Evaluate(firstContext);
            std::size_t const mainTankSelectIndex = findCandidate(
                firstReport, mainTankTargetGuid, "core_select_hostile_target");
            std::size_t const healerPeelIndex = findCandidate(
                firstReport, healerPeelTargetGuid, "core_select_hostile_target");
            initialMainTankPriorityCorrect =
                mainTankSelectIndex < firstReport.candidates.size() &&
                healerPeelIndex < firstReport.candidates.size() &&
                firstReport.candidates[mainTankSelectIndex].candidate.basePriority >
                    firstReport.candidates[healerPeelIndex].candidate.basePriority;
            initialCrowdControlExcluded =
                !hasCandidateForTarget(firstReport, crowdControlTargetGuid);

            bool const chosen = initialMainTankPriorityCorrect &&
                initialCrowdControlExcluded &&
                firstReport.selectedIndices.size() == 1 &&
                firstReport.selectedIndices[0] == mainTankSelectIndex;
            if (chosen)
                engine.ExecuteSelected(firstContext, firstReport);
            firstSelectionExecuted = chosen &&
                firstReport.candidates[mainTankSelectIndex].status == CandidateStatus::Executed &&
                firstReport.candidates[mainTankSelectIndex].actionResult == ActionResult::Success &&
                firstReport.candidates[mainTankSelectIndex].reason == "select_hostile_target_selected" &&
                bot->GetSelectionGuid() == ObjectGuid(mainTankTargetGuid) &&
                bot->GetTargetGuid() == ObjectGuid(mainTankTargetGuid) &&
                bot->GetVictim() == nullptr;
            firstSelectionInvalidatedCache =
                coordinator.GetSnapshotIfFresh(firstContext.nowMs, 250) == nullptr;

            if (firstSelectionExecuted && firstSelectionInvalidatedCache)
            {
                PartySnapshot selectedSnapshot =
                    buildPolicySnapshot(WorldTimer::getMSTime());
                PartyMemberSnapshot const* selectedBot =
                    selectedSnapshot.FindMember(botGuid.GetRawValue());
                bool const selectionCaptured = selectedBot &&
                    selectedSnapshot.epoch != firstSnapshot.epoch &&
                    selectedBot->selectionGuid == mainTankTargetGuid &&
                    selectedSnapshot.members.size() == MAX_GROUP_SIZE;
                if (selectionCaptured)
                {
                    DecisionContext selectedContext(
                        selectedSnapshot, *selectedBot, coordinator,
                        WorldTimer::getMSTime(), nullptr, nullptr,
                        actionRuntime.get(), nullptr, actionRuntime.get());
                    DecisionReport attackReport = engine.Evaluate(selectedContext);
                    std::size_t const mainTankAttackIndex = findCandidate(
                        attackReport, mainTankTargetGuid, "core_attack_target");
                    std::size_t const peelIndex = findCandidate(
                        attackReport, healerPeelTargetGuid, "core_select_hostile_target");
                    bool const chosenAttack =
                        mainTankAttackIndex < attackReport.candidates.size() &&
                        peelIndex < attackReport.candidates.size() &&
                        attackReport.selectedIndices.size() == 1 &&
                        attackReport.selectedIndices[0] == mainTankAttackIndex;
                    if (chosenAttack)
                        engine.ExecuteSelected(selectedContext, attackReport);
                    firstAttackExecuted = chosenAttack &&
                        attackReport.candidates[mainTankAttackIndex].status == CandidateStatus::Executed &&
                        attackReport.candidates[mainTankAttackIndex].actionResult == ActionResult::Success &&
                        bot->GetVictim() == mainTankTarget.creature;
                    firstAttackInvalidatedCache =
                        coordinator.GetSnapshotIfFresh(selectedContext.nowMs, 250) == nullptr;
                }
            }

            if (firstAttackExecuted && firstAttackInvalidatedCache)
            {
                PartySnapshot cachedBeforeRetarget =
                    buildPolicySnapshot(WorldTimer::getMSTime());
                PartyMemberSnapshot const* cachedTank =
                    cachedBeforeRetarget.FindMember(mainTankGuid.GetRawValue());
                PartyMemberSnapshot const* cachedBot =
                    cachedBeforeRetarget.FindMember(botGuid.GetRawValue());
                PartySnapshot const* cachedPointer = coordinator.GetSnapshotIfFresh(
                    cachedBeforeRetarget.capturedAtMs, 250);
                bool const capturedOldTarget = cachedPointer && cachedTank && cachedBot &&
                    cachedTank->victimGuid == mainTankTargetGuid &&
                    cachedBot->victimGuid == mainTankTargetGuid;

                mainTankRetargeted = mainTank->Attack(nextMainTankTarget.creature, true) &&
                    mainTank->GetVictim() == nextMainTankTarget.creature &&
                    bot->GetVictim() == mainTankTarget.creature;
                PartySnapshot const* stillCached = coordinator.GetSnapshotIfFresh(
                    cachedBeforeRetarget.capturedAtMs, 250);
                PartyMemberSnapshot const* stillCachedTank = stillCached
                    ? stillCached->FindMember(mainTankGuid.GetRawValue()) : nullptr;
                staleCacheObservedBeforeExplicitInvalidation =
                    capturedOldTarget && stillCached == cachedPointer &&
                    stillCachedTank && stillCachedTank->victimGuid == mainTankTargetGuid;

                // Human Main Tank target changes do not pass through these
                // Engine actions. Simulate the host's explicit invalidation
                // signal; otherwise the normal 250ms freshness limit bounds it.
                coordinator.InvalidateSnapshot();
                explicitRetargetInvalidationWorked =
                    coordinator.GetSnapshotIfFresh(cachedBeforeRetarget.capturedAtMs, 250) == nullptr;

                if (mainTankRetargeted && explicitRetargetInvalidationWorked)
                {
                    PartySnapshot handoffSnapshot =
                        buildPolicySnapshot(WorldTimer::getMSTime());
                    PartyMemberSnapshot const* handoffBot =
                        handoffSnapshot.FindMember(botGuid.GetRawValue());
                    PartyMemberSnapshot const* handoffTank =
                        handoffSnapshot.FindMember(mainTankGuid.GetRawValue());
                    HostileSnapshot const* oldHostile =
                        handoffSnapshot.FindHostile(mainTankTargetGuid);
                    HostileSnapshot const* nextHostile =
                        handoffSnapshot.FindHostile(nextMainTankTargetGuid);
                    freshHandoffSnapshotValid = handoffSnapshot.epoch !=
                            cachedBeforeRetarget.epoch &&
                        handoffSnapshot.members.size() == MAX_GROUP_SIZE &&
                        handoffBot && handoffTank &&
                        handoffTank->isMainTank &&
                        handoffTank->victimGuid == nextMainTankTargetGuid &&
                        handoffBot->victimGuid == mainTankTargetGuid &&
                        handoffBot->selectionGuid == mainTankTargetGuid &&
                        oldHostile && nextHostile && nextHostile->alive;

                    if (freshHandoffSnapshotValid)
                    {
                        DecisionContext handoffContext(
                            handoffSnapshot, *handoffBot, coordinator,
                            WorldTimer::getMSTime(), nullptr, nullptr,
                            actionRuntime.get(), nullptr, actionRuntime.get());
                        DecisionReport handoffReport = engine.Evaluate(handoffContext);
                        std::size_t const newTankSelectIndex = findCandidate(
                            handoffReport, nextMainTankTargetGuid,
                            "core_select_hostile_target");
                        std::size_t const handoffPeelIndex = findCandidate(
                            handoffReport, healerPeelTargetGuid,
                            "core_select_hostile_target");
                        bool const chosenHandoff = newTankSelectIndex <
                                handoffReport.candidates.size() &&
                            handoffPeelIndex < handoffReport.candidates.size() &&
                            handoffReport.candidates[newTankSelectIndex].candidate.basePriority >
                                handoffReport.candidates[handoffPeelIndex].candidate.basePriority &&
                            handoffReport.selectedIndices.size() == 1 &&
                            handoffReport.selectedIndices[0] == newTankSelectIndex;
                        if (chosenHandoff)
                            engine.ExecuteSelected(handoffContext, handoffReport);
                        handoffSelectionExecuted = chosenHandoff &&
                            handoffReport.candidates[newTankSelectIndex].status == CandidateStatus::Executed &&
                            handoffReport.candidates[newTankSelectIndex].actionResult == ActionResult::Success &&
                            bot->GetSelectionGuid() == ObjectGuid(nextMainTankTargetGuid);
                        handoffSelectionInvalidatedCache =
                            coordinator.GetSnapshotIfFresh(handoffContext.nowMs, 250) == nullptr;

                        if (handoffSelectionExecuted && handoffSelectionInvalidatedCache)
                        {
                            PartySnapshot retargetSelectedSnapshot =
                                buildPolicySnapshot(WorldTimer::getMSTime());
                            PartyMemberSnapshot const* retargetBot =
                                retargetSelectedSnapshot.FindMember(botGuid.GetRawValue());
                            bool const targetSelectionCaptured = retargetBot &&
                                retargetSelectedSnapshot.epoch != handoffSnapshot.epoch &&
                                retargetBot->selectionGuid == nextMainTankTargetGuid &&
                                retargetBot->victimGuid == mainTankTargetGuid;
                            if (targetSelectionCaptured)
                            {
                                DecisionContext retargetContext(
                                    retargetSelectedSnapshot, *retargetBot, coordinator,
                                    WorldTimer::getMSTime(), nullptr, nullptr,
                                    actionRuntime.get(), nullptr, actionRuntime.get());
                                DecisionReport retargetReport = engine.Evaluate(retargetContext);
                                std::size_t const retargetAttackIndex = findCandidate(
                                    retargetReport, nextMainTankTargetGuid,
                                    "core_attack_target");
                                bool const chosenRetargetAttack =
                                    retargetAttackIndex < retargetReport.candidates.size() &&
                                    retargetReport.selectedIndices.size() == 1 &&
                                    retargetReport.selectedIndices[0] == retargetAttackIndex;
                                if (chosenRetargetAttack)
                                    engine.ExecuteSelected(retargetContext, retargetReport);
                                handoffAttackExecuted = chosenRetargetAttack &&
                                    retargetReport.candidates[retargetAttackIndex].status == CandidateStatus::Executed &&
                                    retargetReport.candidates[retargetAttackIndex].actionResult == ActionResult::Success &&
                                    bot->GetVictim() == nextMainTankTarget.creature &&
                                    bot->GetSelectionGuid() == ObjectGuid(nextMainTankTargetGuid) &&
                                    mainTank->GetVictim() == nextMainTankTarget.creature;
                                handoffAttackInvalidatedCache =
                                    coordinator.GetSnapshotIfFresh(retargetContext.nowMs, 250) == nullptr;
                            }
                        }
                    }
                }
            }
        }
    }

    bool const cleaned = cleanup();
    bool const passed = actorsCreated && playersCreated && groupCreated &&
        groupMembersAdded && mainTankAssigned && rolesInstalled &&
        groupSnapshotValid && initialMainTankPriorityCorrect &&
        initialCrowdControlExcluded && firstSelectionExecuted &&
        firstSelectionInvalidatedCache && firstAttackExecuted &&
        firstAttackInvalidatedCache && staleCacheObservedBeforeExplicitInvalidation &&
        mainTankRetargeted && explicitRetargetInvalidationWorked &&
        freshHandoffSnapshotValid && handoffSelectionExecuted &&
        handoffSelectionInvalidatedCache && handoffAttackExecuted &&
        handoffAttackInvalidatedCache && cleaned;

    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] DpsAssistGroupSnapshot: actors=%u players=%u group=%u members=%u mainTank=%u rolesInstalled=%u healerRoleSnapshot=%u firstSelection=%u selectionInvalidated=%u firstAttack=%u attackInvalidated=%u staleCacheObserved=%u explicitRetargetInvalidation=%u handoffSnapshot=%u handoffSelection=%u handoffSelectionInvalidated=%u handoffAttack=%u handoffAttackInvalidated=%u cleanup=%u.",
        actorsCreated ? 1u : 0u, playersCreated ? 1u : 0u,
        groupCreated ? 1u : 0u, groupMembersAdded ? 1u : 0u,
        mainTankAssigned ? 1u : 0u, rolesInstalled ? 1u : 0u,
        healerRoleSnapshotValid ? 1u : 0u, firstSelectionExecuted ? 1u : 0u,
        firstSelectionInvalidatedCache ? 1u : 0u,
        firstAttackExecuted ? 1u : 0u, firstAttackInvalidatedCache ? 1u : 0u,
        staleCacheObservedBeforeExplicitInvalidation ? 1u : 0u,
        explicitRetargetInvalidationWorked ? 1u : 0u,
        freshHandoffSnapshotValid ? 1u : 0u,
        handoffSelectionExecuted ? 1u : 0u,
        handoffSelectionInvalidatedCache ? 1u : 0u,
        handoffAttackExecuted ? 1u : 0u,
        handoffAttackInvalidatedCache ? 1u : 0u,
        cleaned ? 1u : 0u);
    if (passed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: real five-Player Group PartySnapshotBuilder drove DPS Main Tank assist, healer peel priority and CC veto; Core selection and attack invalidated the shared cache, and a Main Tank target handoff refreshed the five-member snapshot and retargeted the DPS bot from its old victim to the new target with complete Group/session cleanup.");
    return passed;
}


bool RunEngineCorePlayerSpellHealSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, unusedHostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, unusedHostileFaction))
        return false;

    SimulatorActor patient;
    bool const patientCreated = CreateSimulatorActor(map, *info, partyFaction, 1.0f, 6.0f, patient);
    if (!patientCreated)
    {
        DestroySimulatorActor(map, patient);
        return false;
    }

    WorldSession healerSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS);
    Player* healer = nullptr;
    bool const healerCreated = CreateSimulatorPlayer(map, partyFaction, healerSession, healer);
    if (!healerCreated)
    {
        bool cleaned = true;
        if (healerSession.GetPlayer())
            healerSession.LogoutPlayer(false);
        else if (healer)
            delete healer;
        cleaned = DestroySimulatorActor(map, patient) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Player spell/heal fixture creation failed (cleanup=%u).", cleaned ? 1u : 0u);
        return false;
    }

    PartyBotAI engineActionAI(
        healer, map.GetId(), map.GetInstanceId(),
        healer->GetPositionX(), healer->GetPositionY(),
        healer->GetPositionZ(), healer->GetOrientation());
    engineActionAI.SetPlayer(healer);
    // This fixture exercises PartyBotAI's PlayerAI observer hooks, not its
    // Legacy startup path. The first normal AI tick would initialize the bot
    // and refill Mana to 100%, masking the Core Spell::finish power deduction.
    engineActionAI.m_initialized = true;
    engineActionAI.m_updateTimer.Reset(600000);

    patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);
    uint32 const healthBefore = patient.creature->GetHealth();
    uint32 const manaBefore = healer->GetPower(POWER_MANA);

    PartySnapshot snapshot;
    snapshot.groupId = 3;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 3;
    uint32 const engineActionNowMs = WorldTimer::getMSTime();
    snapshot.capturedAtMs = engineActionNowMs;

    PartyMemberSnapshot healerMember;
    healerMember.guid = healer->GetObjectGuid().GetRawValue();
    healerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    healerMember.mapId = map.GetId();
    healerMember.instanceId = map.GetInstanceId();
    healerMember.health = healer->GetHealth();
    healerMember.maxHealth = healer->GetMaxHealth();
    healerMember.online = true;
    healerMember.alive = healer->IsAlive();
    healerMember.inCombat = true; // Fixture-owned encounter scope for action trace correlation.
    snapshot.members.push_back(healerMember);

    PartyMemberSnapshot patientMember;
    patientMember.guid = patient.creature->GetObjectGuid().GetRawValue();
    patientMember.roleId = static_cast<std::uint8_t>(PartyRole::Tank);
    patientMember.mapId = map.GetId();
    patientMember.instanceId = map.GetInstanceId();
    patientMember.health = healthBefore;
    patientMember.maxHealth = patient.creature->GetMaxHealth();
    patientMember.online = true;
    patientMember.alive = patient.creature->IsAlive();
    patientMember.isMainTank = true;
    snapshot.members.push_back(patientMember);

    std::shared_ptr<PartyCoordinator> coordinator(new PartyCoordinator());
    coordinator->ConfigureEncounterTraceBudget(32, 16);
    EncounterTransition const engineTraceEncounter =
        coordinator->ObserveEncounter(snapshot, engineActionNowMs, 30000);
    engineActionAI.m_partyCoordinator = coordinator;
    engineActionAI.m_partyCoordinatorMapId = map.GetId();
    engineActionAI.m_partyCoordinatorInstanceId = map.GetInstanceId();
    engineActionAI.m_decisionTrace.BeginTick();
    healer->SetAI(&engineActionAI);
    DecisionContext context(snapshot, snapshot.members[0], *coordinator, engineActionNowMs,
                            &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
    PartyBotEngine& engine = engineActionAI.m_partyBotEngine;
    std::shared_ptr<CaptureCoreSpellTerminalListener> coreTerminalListener(
        new CaptureCoreSpellTerminalListener());
    bool const coreTerminalListenerRegistered =
        engine.AddCoreSpellTerminalListener(coreTerminalListener);
    uint32 continuationExecutionCount = 0;
    std::shared_ptr<SimulatorCoreSpellContinuationAction> continuationAction(
        new SimulatorCoreSpellContinuationAction(continuationExecutionCount));
    bool const continuationActionRegistered = engine.RegisterAction(continuationAction);
    uint32 actionResultAndSpellFollowupCount = 0;
    std::shared_ptr<SimulatorCoreSpellContinuationAction> actionResultAndSpellFollowup(
        new SimulatorCoreSpellContinuationAction(
            actionResultAndSpellFollowupCount, "core_action_result_and_spell_followup"));
    bool const actionResultAndSpellFollowupRegistered =
        engine.RegisterAction(actionResultAndSpellFollowup);
    std::shared_ptr<SimulatorCorePlayerHealAction> healAction(new SimulatorCorePlayerHealAction(map));
    ActionCandidate continuationCandidate;
    continuationCandidate.actionName = "core_spell_terminal_followup";
    continuationCandidate.targetGuid = patientMember.guid;
    continuationCandidate.basePriority = 25.0f;
    continuationCandidate.finalPriority = 25.0f;
    continuationCandidate.channel = ActionChannel::Utility;
    healAction->SetContinuationCandidate(continuationCandidate);
    bool const registered = engine.RegisterAction(healAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_healer_triage"));
    strategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>("core_player_heal"));
    engine.AddStrategy(strategy);
    std::shared_ptr<Strategy> traceOverflowStrategy(
        new Strategy("simulator_candidate_trace_overflow"));
    traceOverflowStrategy->AddTrigger(
        std::make_shared<SimulatorCandidateTraceOverflowTrigger>());
    engine.AddStrategy(traceOverflowStrategy);

    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == patientMember.guid &&
        report.candidates[report.selectedIndices[0]].candidate.spellId == SIMULATOR_HEAL_SPELL_ID &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(context, report);
    ActionId const engineActionId = report.selectedIndices.empty() ? 0 :
        report.candidates[report.selectedIndices[0]].actionId;
    bool const engineActionTraceSetup = engineTraceEncounter.kind ==
        EncounterTransitionKind::Started && engineActionAI.m_decisionTrace.IsEnabled() &&
        engineActionId != 0;

    bool const accepted = !report.selectedIndices.empty() &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed &&
        report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Success &&
        healAction->GetLastCastResult() == SPELL_CAST_OK &&
        healAction->WasCastPendingAfterAccept() && healAction->HadGcdAfterAccept();

    // Map::Update's Player update cadence is driven by WorldTimer's real
    // monotonic clock, not only by the synthetic t_diff argument. Sleep so
    // the normal Player::Update / Unit::Update spell-event path actually runs.
    for (uint32 tick = 0; tick < 20 && healer->GetCurrentSpell(CURRENT_GENERIC_SPELL); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    uint32 const healthAfter = patient.creature->GetHealth();
    uint32 const manaAfter = healer->GetPower(POWER_MANA);
    uint32 const manaAfterAccept = healAction->GetManaAfterAccept();
    // Core applies mana in Spell::finish, not at CastSpell acceptance. The
    // LastManaUse timer is authoritative even if natural regeneration restores
    // the numeric pool before this fixture reads it.
    bool const coreManaSpendObserved = healer->IsUnderLastManaUseEffect();
    bool const completed = healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                           healthAfter > healthBefore && coreManaSpendObserved;
    bool const engineActionTracePassed = engineActionTraceSetup && accepted &&
        report.candidates[report.selectedIndices[0]].actionId == engineActionId &&
        report.candidates[report.selectedIndices[0]].candidate.actionId == engineActionId &&
        engineActionAI.m_activeEngineActionId == 0 &&
        engineActionAI.m_pendingCastTraceActionId == 0;
    bool const coreSpellTerminalPassed = coreTerminalListenerRegistered &&
        coreTerminalListener->events.size() == 1 &&
        coreTerminalListener->events[0].actionId == engineActionId &&
        coreTerminalListener->events[0].encounterId == coordinator->GetEncounterId() &&
        coreTerminalListener->events[0].targetGuid == patientMember.guid &&
        coreTerminalListener->events[0].spellId == SIMULATOR_HEAL_SPELL_ID &&
        coreTerminalListener->events[0].status == CoreSpellTerminalStatus::Succeeded;
    bool const continuationEnqueued = healAction->GetContinuationResult() ==
        ActionContinuationEnqueueResult::Queued;
    PendingActionDispatchSummary continuationDispatch;
    std::size_t const pendingBeforeContinuationDispatch = engine.GetPendingActionCount();
    std::size_t const continuationWaitersBeforeDispatch = engine.GetPendingContinuationCount();
    bool continuationDispatched = false;
    if (coreSpellTerminalPassed && continuationEnqueued && continuationActionRegistered)
    {
        DecisionContext continuationContext(
            snapshot, snapshot.members[0], *coordinator,
            coreTerminalListener->events[0].occurredAtMs + 1,
            &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
        continuationDispatch = engine.ExecutePendingActions(continuationContext);
        continuationDispatched = continuationDispatch.selected == 1 &&
            continuationDispatch.executed == 1 && continuationExecutionCount == 1 &&
            engine.GetPendingContinuationCount() == 0 && engine.GetPendingActionCount() == 0;
    }

    uint32 const f34ContinuationExecutionCount = continuationExecutionCount;

    // F35: generate a second independent Core Spell::finish terminal from
    // the same Player/Engine, then register an AND dependency after both casts
    // have completed. This validates the real terminal-history late-registration
    // seam rather than manually injecting synthetic terminal events.
    SpellEntry const* f35HealSpell = sSpellMgr.GetSpellEntry(SIMULATOR_HEAL_SPELL_ID);
    for (uint32 tick = 0; tick < 20 && f35HealSpell &&
         (healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) || healer->HasGCD(f35HealSpell));
         ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }
    bool const f35SecondCastReady = f35HealSpell &&
        !healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
        !healer->HasGCD(f35HealSpell) && healer->IsSpellReady(f35HealSpell);
    bool f35SecondCastSelected = false;
    bool f35SecondCastAccepted = false;
    bool f35SecondCastCompleted = false;
    ActionId f35SecondActionId = 0;
    bool f35MultiCoreTerminalsPassed = false;
    ActionContinuationEnqueueResult f35DependencyResult =
        ActionContinuationEnqueueResult::Invalid;
    PendingActionDispatchSummary f35DependencyDispatch;
    bool f35MultiDependencyDispatched = false;
    uint32 f35AlternativeSelectedExecutions = 0;
    uint32 f35AlternativeRejectedExecutions = 0;
    ActionContinuationEnqueueResult f35AlternativeFirstResult =
        ActionContinuationEnqueueResult::Invalid;
    ActionContinuationEnqueueResult f35AlternativeSecondResult =
        ActionContinuationEnqueueResult::Invalid;
    PendingActionDispatchSummary f35AlternativeDispatch;
    bool f35AlternativeGroupPassed = false;
    ActionContinuationEnqueueResult f36MixedDependencyResult =
        ActionContinuationEnqueueResult::Invalid;
    PendingActionDispatchSummary f36MixedDependencyDispatch;
    bool f36MixedDependencyPassed = false;
    if (f35SecondCastReady)
    {
        // Keep a damaged target for the second ordinary Core heal. The first
        // cast above already proved its own health/Mana side effects.
        patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);
        healAction->DisableContinuationRegistration();
        ActionCandidate actionResultAndSpellCandidate;
        actionResultAndSpellCandidate.actionName =
            "core_action_result_and_spell_followup";
        actionResultAndSpellCandidate.targetGuid = patientMember.guid;
        actionResultAndSpellCandidate.basePriority = 25.0f;
        actionResultAndSpellCandidate.finalPriority = 25.0f;
        actionResultAndSpellCandidate.channel = ActionChannel::Utility;
        healAction->SetActionResultAndSpellDependencyCandidate(
            actionResultAndSpellCandidate);
        engineActionAI.m_decisionTrace.BeginTick();
        DecisionContext secondCastContext(
            snapshot, snapshot.members[0], *coordinator, WorldTimer::getMSTime(),
            &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
        DecisionReport secondReport = engine.Evaluate(secondCastContext);
        f35SecondCastSelected = secondReport.selectedIndices.size() == 1 &&
            secondReport.candidates[secondReport.selectedIndices[0]].candidate.targetGuid ==
                patientMember.guid &&
            secondReport.candidates[secondReport.selectedIndices[0]].status ==
                CandidateStatus::Selected;
        if (f35SecondCastSelected)
        {
            engine.ExecuteSelected(secondCastContext, secondReport);
            CandidateEvaluation const& secondEvaluation =
                secondReport.candidates[secondReport.selectedIndices[0]];
            f35SecondActionId = secondEvaluation.actionId;
            f35SecondCastAccepted =
                secondEvaluation.status == CandidateStatus::Executed &&
                secondEvaluation.actionResult == ActionResult::Success &&
                healAction->GetLastCastResult() == SPELL_CAST_OK &&
                f35SecondActionId != 0;
        }

        for (uint32 tick = 0; tick < 20 &&
             (healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) ||
              coreTerminalListener->events.size() < 2);
             ++tick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            map.Update(250);
        }
        f35SecondCastCompleted = f35SecondCastAccepted &&
            healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
            coreTerminalListener->events.size() == 2 &&
            coreTerminalListener->events[1].actionId == f35SecondActionId &&
            coreTerminalListener->events[1].status == CoreSpellTerminalStatus::Succeeded;

        f36MixedDependencyResult =
            healAction->GetActionResultAndSpellDependencyResult();
        if (f35SecondCastCompleted && actionResultAndSpellFollowupRegistered &&
            f36MixedDependencyResult == ActionContinuationEnqueueResult::Queued &&
            engine.GetPendingContinuationCount() == 0 &&
            engine.GetPendingActionCount() == 1)
        {
            DecisionContext f36DispatchContext(
                snapshot, snapshot.members[0], *coordinator, WorldTimer::getMSTime(),
                &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
            f36MixedDependencyDispatch =
                engine.ExecutePendingActions(f36DispatchContext);
            f36MixedDependencyPassed =
                f36MixedDependencyDispatch.selected == 1 &&
                f36MixedDependencyDispatch.executed == 1 &&
                actionResultAndSpellFollowupCount == 1 &&
                engine.GetPendingContinuationCount() == 0 &&
                engine.GetPendingActionCount() == 0;
        }

        f35MultiCoreTerminalsPassed = f35SecondCastCompleted &&
            coreTerminalListener->events[0].actionId == engineActionId &&
            coreTerminalListener->events[0].actionId !=
                coreTerminalListener->events[1].actionId &&
            coreTerminalListener->events[0].encounterId == coordinator->GetEncounterId() &&
            coreTerminalListener->events[1].encounterId == coordinator->GetEncounterId() &&
            coreTerminalListener->events[0].targetGuid == patientMember.guid &&
            coreTerminalListener->events[1].targetGuid == patientMember.guid &&
            coreTerminalListener->events[0].spellId == SIMULATOR_HEAL_SPELL_ID &&
            coreTerminalListener->events[1].spellId == SIMULATOR_HEAL_SPELL_ID &&
            coreTerminalListener->events[0].status == CoreSpellTerminalStatus::Succeeded &&
            coreTerminalListener->events[1].status == CoreSpellTerminalStatus::Succeeded;

        if (f35MultiCoreTerminalsPassed)
        {
            ActionDependencyRequest dependency;
            dependency.lifetimeMs = 4000;
            for (CoreSpellTerminalEvent const& terminal : coreTerminalListener->events)
            {
                ActionTerminalPrerequisite prerequisite;
                prerequisite.actionId = terminal.actionId;
                prerequisite.encounterId = terminal.encounterId;
                prerequisite.targetGuid = terminal.targetGuid;
                prerequisite.spellId = terminal.spellId;
                prerequisite.requiredStatus = CoreSpellTerminalStatus::Succeeded;
                dependency.prerequisites.push_back(prerequisite);
            }

            DecisionContext lateDependencyContext(
                snapshot, snapshot.members[0], *coordinator, WorldTimer::getMSTime(),
                &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
            f35DependencyResult = engine.QueueActionDependency(
                lateDependencyContext, dependency, continuationCandidate);
            if (f35DependencyResult == ActionContinuationEnqueueResult::ReleasedImmediately &&
                engine.GetPendingContinuationCount() == 0 &&
                engine.GetPendingActionCount() == 1)
            {
                DecisionContext dependencyDispatchContext(
                    snapshot, snapshot.members[0], *coordinator, WorldTimer::getMSTime(),
                    &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
                f35DependencyDispatch =
                    engine.ExecutePendingActions(dependencyDispatchContext);
                f35MultiDependencyDispatched =
                    f35DependencyDispatch.selected == 1 &&
                    f35DependencyDispatch.executed == 1 &&
                    continuationExecutionCount == 2 &&
                    engine.GetPendingContinuationCount() == 0 &&
                    engine.GetPendingActionCount() == 0;
            }
        }

        // Reuse the real second Core terminal to prove atomic one-of-N
        // selection. The lower-priority branch requires Success; a higher-
        // priority sibling requires Failed and must be rejected by the actual
        // Core Spell::finish result before the group chooses a winner.
        if (f35MultiCoreTerminalsPassed && f35MultiDependencyDispatched)
        {
            std::shared_ptr<SimulatorCoreSpellContinuationAction> alternativeSelectedAction(
                new SimulatorCoreSpellContinuationAction(
                    f35AlternativeSelectedExecutions,
                    "core_spell_alternative_selected"));
            std::shared_ptr<SimulatorCoreSpellContinuationAction> alternativeRejectedAction(
                new SimulatorCoreSpellContinuationAction(
                    f35AlternativeRejectedExecutions,
                    "core_spell_alternative_rejected"));
            bool const alternativesRegistered =
                engine.RegisterAction(alternativeSelectedAction) &&
                engine.RegisterAction(alternativeRejectedAction);

            ActionCandidate selectedAlternative = continuationCandidate;
            selectedAlternative.actionName = "core_spell_alternative_selected";
            selectedAlternative.basePriority = 10.0f;
            selectedAlternative.finalPriority = 10.0f;
            ActionCandidate rejectedAlternative = continuationCandidate;
            rejectedAlternative.actionName = "core_spell_alternative_rejected";
            rejectedAlternative.basePriority = 1000.0f;
            rejectedAlternative.finalPriority = 1000.0f;

            ActionDependencyRequest selectedAlternativeRequest;
            selectedAlternativeRequest.lifetimeMs = 4000;
            selectedAlternativeRequest.alternativeGroupId = 3501;
            selectedAlternativeRequest.alternativeIndex = 0;
            selectedAlternativeRequest.alternativeCount = 2;
            CoreSpellTerminalEvent const& realParent =
                coreTerminalListener->events[1];
            ActionTerminalPrerequisite selectedParent;
            selectedParent.actionId = realParent.actionId;
            selectedParent.encounterId = realParent.encounterId;
            selectedParent.targetGuid = realParent.targetGuid;
            selectedParent.spellId = realParent.spellId;
            selectedParent.requiredStatus = CoreSpellTerminalStatus::Succeeded;
            selectedAlternativeRequest.prerequisites.push_back(selectedParent);

            ActionDependencyRequest rejectedAlternativeRequest =
                selectedAlternativeRequest;
            rejectedAlternativeRequest.alternativeIndex = 1;
            rejectedAlternativeRequest.prerequisites[0].requiredStatus =
                CoreSpellTerminalStatus::Failed;

            if (alternativesRegistered)
            {
                DecisionContext alternativeContext(
                    snapshot, snapshot.members[0], *coordinator, WorldTimer::getMSTime(),
                    &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
                f35AlternativeFirstResult = engine.QueueActionDependency(
                    alternativeContext, selectedAlternativeRequest, selectedAlternative);
                f35AlternativeSecondResult = engine.QueueActionDependency(
                    alternativeContext, rejectedAlternativeRequest, rejectedAlternative);

                bool const groupResolved = 
                    f35AlternativeFirstResult == ActionContinuationEnqueueResult::Queued &&
                    f35AlternativeSecondResult == ActionContinuationEnqueueResult::Queued &&
                    engine.GetPendingContinuationCount() == 0 &&
                    engine.GetPendingActionCount() == 1;
                if (groupResolved)
                {
                    DecisionContext alternativeDispatchContext(
                        snapshot, snapshot.members[0], *coordinator,
                        WorldTimer::getMSTime(),
                        &engineActionAI.m_decisionTrace, nullptr, &engineActionAI);
                    f35AlternativeDispatch =
                        engine.ExecutePendingActions(alternativeDispatchContext);
                    f35AlternativeGroupPassed =
                        f35AlternativeDispatch.selected == 1 &&
                        f35AlternativeDispatch.executed == 1 &&
                        f35AlternativeSelectedExecutions == 1 &&
                        f35AlternativeRejectedExecutions == 0 &&
                        engine.GetPendingContinuationCount() == 0 &&
                        engine.GetPendingActionCount() == 0;
                }
            }
        }
    }
    // F39: exercise the Engine-only PointMovementGenerator correlation path on
    // a real Core Player, then prove forced generator cleanup is not success.
    uint32 movementFollowupExecutions = 0;
    std::shared_ptr<SimulatorCoreSpellContinuationAction> movementFollowup(
        new SimulatorCoreSpellContinuationAction(
            movementFollowupExecutions, "core_player_move_followup"));
    bool const movementFollowupRegistered = engine.RegisterAction(movementFollowup);
    std::shared_ptr<CapturePointMovementTerminalListener> movementTerminalListener(
        new CapturePointMovementTerminalListener());
    bool const movementTerminalListenerRegistered =
        engine.AddPointMovementTerminalListener(movementTerminalListener);

    uint32 const enginePointId = 0xF390u;
    float const movementArrivalTolerance = 0.75f;
    ActionCandidate movementFollowupCandidate;
    movementFollowupCandidate.actionName = "core_player_move_followup";
    movementFollowupCandidate.targetGuid = healerMember.guid;
    movementFollowupCandidate.basePriority = 10.0f;
    movementFollowupCandidate.finalPriority = 10.0f;
    movementFollowupCandidate.channel = ActionChannel::Utility;
    std::shared_ptr<SimulatorCorePlayerPointMoveAction> playerPointMoveAction(
        new SimulatorCorePlayerPointMoveAction(
            enginePointId, movementArrivalTolerance, movementFollowupCandidate));
    bool const playerPointMoveActionRegistered = engine.RegisterAction(playerPointMoveAction);

    float const pointDestinationX = healer->GetPositionX() + 3.0f;
    float const pointDestinationY = healer->GetPositionY();
    float const pointDestinationZ = healer->GetPositionZ();
    ActionCandidate playerPointMoveCandidate;
    playerPointMoveCandidate.actionName = "core_player_point_move_engine";
    playerPointMoveCandidate.strategyName = "simulator_player_movement";
    playerPointMoveCandidate.triggerName = "simulator_player_point_move";
    playerPointMoveCandidate.targetGuid = healerMember.guid;
    playerPointMoveCandidate.basePriority = 90.0f;
    playerPointMoveCandidate.finalPriority = 90.0f;
    playerPointMoveCandidate.channel = ActionChannel::Movement;
    playerPointMoveCandidate.hasDestination = true;
    playerPointMoveCandidate.destinationX = pointDestinationX;
    playerPointMoveCandidate.destinationY = pointDestinationY;
    playerPointMoveCandidate.destinationZ = pointDestinationZ;

    uint32 const movementNowMs = WorldTimer::getMSTime();
    DecisionContext movementContext(
        snapshot, snapshot.members[0], *coordinator, movementNowMs,
        &engineActionAI.m_decisionTrace, nullptr, &engineActionAI, &engine,
        &engineActionAI);
    CoreActionResolution const resolvedCoreContext =
        engineActionAI.ResolveUnitContext(movementContext, healerMember.guid);
    CoreActionResolution const missingCoreTarget =
        engineActionAI.ResolveUnitContext(movementContext, 0xFFFFFFFFFFFFFFFFull);

    PartySnapshot staleMapSnapshot = snapshot;
    ++staleMapSnapshot.mapId;
    DecisionContext staleMapContext(
        staleMapSnapshot, staleMapSnapshot.members[0], *coordinator, movementNowMs,
        &engineActionAI.m_decisionTrace, nullptr, &engineActionAI, &engine,
        &engineActionAI);
    CoreActionResolution const staleMapResolution =
        engineActionAI.ResolveUnitContext(staleMapContext, healerMember.guid);

    PartySnapshot staleInstanceSnapshot = snapshot;
    ++staleInstanceSnapshot.instanceId;
    DecisionContext staleInstanceContext(
        staleInstanceSnapshot, staleInstanceSnapshot.members[0], *coordinator,
        movementNowMs, &engineActionAI.m_decisionTrace, nullptr,
        &engineActionAI, &engine, &engineActionAI);
    CoreActionResolution const staleInstanceResolution =
        engineActionAI.ResolveUnitContext(staleInstanceContext, healerMember.guid);

    bool const f41CoreActionPreflightPassed =
        resolvedCoreContext.IsResolved() &&
        resolvedCoreContext.actor == healer &&
        resolvedCoreContext.target == healer &&
        resolvedCoreContext.map == &map &&
        missingCoreTarget.status == CoreActionResolveStatus::TargetNotFound &&
        staleMapResolution.status == CoreActionResolveStatus::SnapshotMapMismatch &&
        staleInstanceResolution.status == CoreActionResolveStatus::SnapshotInstanceMismatch;
    sLog.Out(LOG_BASIC, f41CoreActionPreflightPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] F41 Core action preflight: resolved=%u selfTarget=%u missingTargetRejected=%u staleMapRejected=%u staleInstanceRejected=%u.",
             resolvedCoreContext.IsResolved() ? 1u : 0u,
             resolvedCoreContext.target == healer ? 1u : 0u,
             missingCoreTarget.status == CoreActionResolveStatus::TargetNotFound ? 1u : 0u,
             staleMapResolution.status == CoreActionResolveStatus::SnapshotMapMismatch ? 1u : 0u,
             staleInstanceResolution.status == CoreActionResolveStatus::SnapshotInstanceMismatch ? 1u : 0u);
    if (f41CoreActionPreflightPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: F41 CoreAction required a fresh map-thread actor/target resolution and rejected missing targets plus stale map/instance snapshots before the real movement adapter ran.");

    CandidateEvaluation movementEvaluation;
    movementEvaluation.candidate = playerPointMoveCandidate;
    movementEvaluation.status = CandidateStatus::Selected;
    movementEvaluation.threatImpact = playerPointMoveAction->EstimateThreatImpact(
        movementContext, playerPointMoveCandidate);
    DecisionReport movementReport;
    movementReport.candidates.push_back(movementEvaluation);
    movementReport.selectedIndices.push_back(0);
    // Earlier simulator scenarios can leave a legacy point generator active;
    // clear that fixture state so this test measures the new Engine action.
    healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);

    bool const movementSetupPassed = f41CoreActionPreflightPassed &&
        movementFollowupRegistered && movementTerminalListenerRegistered &&
        playerPointMoveActionRegistered;
    if (movementSetupPassed)
        engine.ExecuteSelected(movementContext, movementReport);

    ActionId const engineMovementActionId =
        movementReport.candidates.empty() ? 0 : movementReport.candidates[0].actionId;
    bool const engineMovementStarted = movementSetupPassed &&
        movementReport.candidates[0].status == CandidateStatus::Executed &&
        movementReport.candidates[0].actionResult == ActionResult::Success &&
        playerPointMoveAction->DidStartMovement() && engineMovementActionId != 0;

    for (uint32 tick = 0; tick < 20 && movementTerminalListener->events.empty(); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        map.Update(250);
    }

    bool const naturalMovementReached = movementTerminalListener->events.size() == 1 &&
        movementTerminalListener->events[0].actionId == engineMovementActionId &&
        movementTerminalListener->events[0].encounterId == coordinator->GetEncounterId() &&
        movementTerminalListener->events[0].moverGuid == healerMember.guid &&
        movementTerminalListener->events[0].pointId == enginePointId &&
        movementTerminalListener->events[0].status == PointMovementTerminalStatus::Reached &&
        movementTerminalListener->events[0].finalDistance <= movementArrivalTolerance;
    bool const movementContinuationReleased =
        engine.GetPendingContinuationCount() == 0 && engine.GetPendingActionCount() == 1;
    DecisionContext movementContinuationContext(
        snapshot, snapshot.members[0], *coordinator, WorldTimer::getMSTime(),
        &engineActionAI.m_decisionTrace, nullptr, &engineActionAI, &engine,
        &engineActionAI);
    PendingActionDispatchSummary const movementFollowupDispatch =
        engine.ExecutePendingActions(movementContinuationContext);
    bool const movementFollowupDispatched = movementContinuationReleased &&
        movementFollowupDispatch.selected == 1 &&
        movementFollowupDispatch.executed == 1 &&
        movementFollowupExecutions == 1;

    ActionId const forcedCleanupActionId = coordinator->NextActionId();
    float const forcedDestinationX = healer->GetPositionX() + 12.0f;
    bool const forcedMoveStarted = forcedCleanupActionId &&
        healer->GetMotionMaster()->MovePointForEngine(
            enginePointId + 1, forcedDestinationX, healer->GetPositionY(),
            healer->GetPositionZ(), MOVE_NONE, forcedCleanupActionId,
            coordinator->GetEncounterId(), movementArrivalTolerance);
    if (forcedMoveStarted)
        healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);
    bool const forcedCleanupInterrupted = movementTerminalListener->events.size() == 2 &&
        movementTerminalListener->events[1].actionId == forcedCleanupActionId &&
        movementTerminalListener->events[1].pointId == enginePointId + 1 &&
        movementTerminalListener->events[1].status == PointMovementTerminalStatus::Interrupted;

    bool const f39MovementTerminalPassed = engineMovementStarted &&
        naturalMovementReached && movementContinuationReleased &&
        movementFollowupDispatched && forcedMoveStarted && forcedCleanupInterrupted;
    sLog.Out(LOG_BASIC, f39MovementTerminalPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] F39 Player movement terminal: setup=%u started=%u action=%llu events=%zu reached=%u continuationReleased=%u followup=%u forcedStarted=%u forcedInterrupted=%u distance=%.3f cleanup=%.3f.",
             movementSetupPassed ? 1u : 0u, engineMovementStarted ? 1u : 0u,
             static_cast<unsigned long long>(engineMovementActionId),
             movementTerminalListener->events.size(), naturalMovementReached ? 1u : 0u,
             movementContinuationReleased ? 1u : 0u,
             movementFollowupDispatched ? 1u : 0u,
             forcedMoveStarted ? 1u : 0u, forcedCleanupInterrupted ? 1u : 0u,
             movementTerminalListener->events.empty() ? -1.0f
                 : movementTerminalListener->events[0].finalDistance,
             movementTerminalListener->events.size() < 2 ? -1.0f
                 : movementTerminalListener->events[1].finalDistance);

    if (forcedMoveStarted && !forcedCleanupInterrupted)
        healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);

    // F43: exercise the reusable actor-only MovePointAction. It must never
    // overwrite an unrelated active generator, and request acceptance is not
    // considered arrival until the correlated Core terminal says Reached.
    PartySnapshot movePointSnapshot = snapshot;
    movePointSnapshot.epoch = snapshot.epoch + 1;
    PartyMemberSnapshot& movePointBot = movePointSnapshot.members[0];
    movePointBot.x = healer->GetPositionX();
    movePointBot.y = healer->GetPositionY();
    movePointBot.z = healer->GetPositionZ();
    movePointBot.online = true;
    movePointBot.alive = healer->IsAlive();
    movePointBot.inCombat = false;
    movePointBot.victimGuid = 0;
    movePointBot.currentSpellId = 0;
    movePointBot.mapId = map.GetId();
    movePointBot.instanceId = map.GetInstanceId();

    DecisionContext movePointContext(
        movePointSnapshot, movePointBot, *coordinator, WorldTimer::getMSTime(),
        &engineActionAI.m_decisionTrace, nullptr, &engineActionAI, nullptr,
        &engineActionAI);
    std::shared_ptr<MovePointAction> movePointAction(new MovePointAction(
        movementArrivalTolerance));
    bool const movePointRegistered = engine.RegisterAction(movePointAction);

    ActionCandidate movePointCandidate;
    movePointCandidate.actionName = "core_move_point";
    movePointCandidate.strategyName = "simulator_move_point";
    movePointCandidate.triggerName = "simulator_move_point";
    movePointCandidate.basePriority = 90.0f;
    movePointCandidate.finalPriority = 90.0f;
    movePointCandidate.channel = ActionChannel::Movement;
    movePointCandidate.hasDestination = true;
    movePointCandidate.destinationX = movePointBot.x + 3.0f;
    movePointCandidate.destinationY = movePointBot.y;
    movePointCandidate.destinationZ = movePointBot.z;

    std::string movePointReason;
    bool const movePointActorOnlyResolved =
        engineActionAI.ResolveUnitContext(movePointContext, 0).IsResolved() &&
        engineActionAI.ResolveUnitContext(movePointContext, 0).target == nullptr;
    bool const movePointSnapshotEligible = movePointAction->IsPossible(
        movePointContext, movePointCandidate, movePointReason);
    bool const movePointUseful = movePointAction->IsUseful(
        movePointContext, movePointCandidate, movePointReason);
    ActionCandidate alreadyThereCandidate = movePointCandidate;
    alreadyThereCandidate.destinationX = movePointBot.x;
    alreadyThereCandidate.destinationY = movePointBot.y;
    alreadyThereCandidate.destinationZ = movePointBot.z;
    std::string alreadyThereReason;
    bool const alreadyTherePossible = movePointAction->IsPossible(
        movePointContext, alreadyThereCandidate, alreadyThereReason);
    bool const alreadyThereNoOp = alreadyTherePossible &&
        !movePointAction->IsUseful(
            movePointContext, alreadyThereCandidate, alreadyThereReason);
    ActionCandidate outOfBoundsCandidate = movePointCandidate;
    outOfBoundsCandidate.destinationX = MAP_HALFSIZE + 1.0f;
    std::string outOfBoundsReason;
    bool const outOfBoundsDestinationRejected =
        !movePointAction->IsPossible(
            movePointContext, outOfBoundsCandidate, outOfBoundsReason);

    // A currently active Point generator is unrelated movement and must not
    // be replaced by this action, even when the requested destination differs.
    healer->GetMotionMaster()->MovePoint(
        0xF431u, healer->GetPositionX() + 8.0f, healer->GetPositionY(),
        healer->GetPositionZ(), MOVE_NONE);
    MovementGenerator const* const unrelatedMovementBefore =
        healer->GetMotionMaster()->GetCurrent();
    bool const unrelatedMovementStarted = unrelatedMovementBefore &&
        healer->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
            POINT_MOTION_TYPE;
    CandidateEvaluation unrelatedMoveEvaluation;
    unrelatedMoveEvaluation.candidate = movePointCandidate;
    unrelatedMoveEvaluation.status = CandidateStatus::Selected;
    unrelatedMoveEvaluation.threatImpact =
        movePointAction->EstimateThreatImpact(
            movePointContext, movePointCandidate);
    DecisionReport unrelatedMoveReport;
    unrelatedMoveReport.candidates.push_back(unrelatedMoveEvaluation);
    unrelatedMoveReport.selectedIndices.push_back(0);
    if (movePointRegistered && unrelatedMovementStarted)
        engine.ExecuteSelected(movePointContext, unrelatedMoveReport);
    CandidateEvaluation const* unrelatedMoveResult =
        unrelatedMoveReport.candidates.empty()
            ? nullptr : &unrelatedMoveReport.candidates[0];
    bool const unrelatedMovementRejected = unrelatedMoveResult &&
        unrelatedMoveResult->status == CandidateStatus::ExecutionFailed &&
        unrelatedMoveResult->actionResult == ActionResult::Useless;
    bool const unrelatedMovementPreserved = unrelatedMovementStarted &&
        healer->GetMotionMaster()->GetCurrent() == unrelatedMovementBefore &&
        healer->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
            POINT_MOTION_TYPE;
    if (unrelatedMovementPreserved)
        healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);

    // The rejected request left the Player idle. Refresh the value snapshot
    // coordinates before issuing a new, independently correlated move.
    movePointBot.x = healer->GetPositionX();
    movePointBot.y = healer->GetPositionY();
    movePointBot.z = healer->GetPositionZ();
    movePointCandidate.destinationX = movePointBot.x + 3.0f;
    movePointCandidate.destinationY = movePointBot.y;
    movePointCandidate.destinationZ = movePointBot.z;
    movementTerminalListener->events.clear();

    CandidateEvaluation movePointEvaluation;
    movePointEvaluation.candidate = movePointCandidate;
    movePointEvaluation.status = CandidateStatus::Selected;
    movePointEvaluation.threatImpact =
        movePointAction->EstimateThreatImpact(
            movePointContext, movePointCandidate);
    DecisionReport movePointReport;
    movePointReport.candidates.push_back(movePointEvaluation);
    movePointReport.selectedIndices.push_back(0);

    ObjectGuid const movePointSelectionBefore = healer->GetSelectionGuid();
    ObjectGuid const movePointTargetBefore = healer->GetTargetGuid();
    Unit* const movePointVictimBefore = healer->GetVictim();
    bool const movePointCombatBefore = healer->IsInCombat();
    uint32 const movePointHealthBefore = healer->GetHealth();
    uint32 const movePointPowerBefore = healer->GetPower(healer->GetPowerType());

    if (movePointRegistered && movePointActorOnlyResolved &&
        movePointSnapshotEligible && movePointUseful &&
        alreadyThereNoOp && unrelatedMovementRejected &&
        unrelatedMovementPreserved)
        engine.ExecuteSelected(movePointContext, movePointReport);

    ActionId const movePointActionId = movePointReport.candidates.empty()
        ? 0 : movePointReport.candidates[0].actionId;
    bool const movePointAccepted = movePointReport.candidates.size() == 1 &&
        movePointReport.candidates[0].status == CandidateStatus::Executed &&
        movePointReport.candidates[0].actionResult == ActionResult::Success &&
        movePointActionId != 0 &&
        healer->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
            POINT_MOTION_TYPE;

    // This Player is a local-session fixture actor and is not advanced by
    // Map::Update. Drive only its public Core Unit::Update path so the movement
    // terminal is delivered without ticking unrelated actors in the shared Map.
    for (uint32 tick = 0;
         tick < 20 && movementTerminalListener->events.empty(); ++tick)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        healer->Update(250, 250);
    }

    float const movePointDistance = std::sqrt(
        std::pow(healer->GetPositionX() - movePointCandidate.destinationX, 2.0f) +
        std::pow(healer->GetPositionY() - movePointCandidate.destinationY, 2.0f) +
        std::pow(healer->GetPositionZ() - movePointCandidate.destinationZ, 2.0f));
    bool const movePointReached =
        movementTerminalListener->events.size() == 1 &&
        movementTerminalListener->events[0].actionId == movePointActionId &&
        movementTerminalListener->events[0].encounterId ==
            coordinator->GetEncounterId() &&
        movementTerminalListener->events[0].moverGuid == healerMember.guid &&
        movementTerminalListener->events[0].pointId ==
            MovePointAction::PointIdForAction(movePointActionId) &&
        movementTerminalListener->events[0].status ==
            PointMovementTerminalStatus::Reached &&
        movementTerminalListener->events[0].finalDistance <=
            movementArrivalTolerance &&
        movePointDistance <= movementArrivalTolerance;
    bool const movePointStatePreserved =
        healer->GetSelectionGuid() == movePointSelectionBefore &&
        healer->GetTargetGuid() == movePointTargetBefore &&
        healer->GetVictim() == movePointVictimBefore &&
        healer->IsInCombat() == movePointCombatBefore &&
        healer->GetHealth() == movePointHealthBefore &&
        healer->GetPower(healer->GetPowerType()) == movePointPowerBefore;
    bool const movePointFixturePassed =
        movePointRegistered && movePointActorOnlyResolved &&
        movePointSnapshotEligible && movePointUseful && alreadyThereNoOp &&
        outOfBoundsDestinationRejected &&
        unrelatedMovementRejected && unrelatedMovementPreserved &&
        movePointAccepted && movePointReached && movePointStatePreserved;

    if (!movePointReached &&
        healer->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
            POINT_MOTION_TYPE)
        healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);

    sLog.Out(LOG_BASIC, movePointFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] MovePointAction: registered=%u actorOnly=%u snapshotEligible=%u alreadyThereNoOp=%u outOfBoundsRejected=%u unrelatedMovementRejected=%u sameGeneratorPreserved=%u accepted=%u reached=%u statePreserved=%u distance=%.3f.",
        movePointRegistered ? 1u : 0u, movePointActorOnlyResolved ? 1u : 0u,
        movePointSnapshotEligible ? 1u : 0u, alreadyThereNoOp ? 1u : 0u,
        outOfBoundsDestinationRejected ? 1u : 0u,
        unrelatedMovementRejected ? 1u : 0u,
        unrelatedMovementPreserved ? 1u : 0u,
        movePointAccepted ? 1u : 0u, movePointReached ? 1u : 0u,
        movePointStatePreserved ? 1u : 0u, movePointDistance);
    if (movePointFixturePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: MovePointAction used an actor-only Core resolver, rejected out-of-bounds destinations, preserved the exact unrelated movement generator, and treated its matching typed Reached terminal—not request acceptance—as proof of arrival.");

    // F43: cancellation is bound to the exact Engine-owned point movement
    // correlation in the value snapshot. A replaced live generator must survive
    // a stale stop request; successful cancellation must publish Interrupted.
    PartySnapshot stopPointBaseSnapshot = movePointSnapshot;
    stopPointBaseSnapshot.epoch = movePointSnapshot.epoch + 1;
    PartyMemberSnapshot& stopPointBaseBot = stopPointBaseSnapshot.members[0];
    stopPointBaseBot.x = healer->GetPositionX();
    stopPointBaseBot.y = healer->GetPositionY();
    stopPointBaseBot.z = healer->GetPositionZ();
    stopPointBaseBot.online = healer->IsInWorld();
    stopPointBaseBot.alive = healer->IsAlive();
    stopPointBaseBot.inCombat = healer->IsInCombat();
    stopPointBaseBot.victimGuid = healer->GetVictim()
        ? healer->GetVictim()->GetObjectGuid().GetRawValue() : 0;
    stopPointBaseBot.pointMovementActionId = 0;
    stopPointBaseBot.pointMovementEncounterId = 0;
    stopPointBaseBot.pointMovementPointId = 0;

    auto capturePointMovementSnapshot = [](
        PartyMemberSnapshot& member, Player* player) -> bool
    {
        if (!player || !player->GetMotionMaster() ||
            player->GetMotionMaster()->empty())
            return false;
        MovementGenerator const* movement = player->GetMotionMaster()->GetCurrent();
        if (!movement || movement->GetMovementGeneratorType() != POINT_MOTION_TYPE)
            return false;
        PointMovementGenerator<Player> const* point =
            static_cast<PointMovementGenerator<Player> const*>(movement);
        member.pointMovementActionId = point->GetEngineActionId();
        member.pointMovementEncounterId = point->GetEngineEncounterId();
        member.pointMovementPointId = point->GetPointId();
        return member.pointMovementActionId != 0 &&
            member.pointMovementEncounterId != 0 &&
            member.pointMovementPointId != 0;
    };

    std::shared_ptr<StopPointMovementAction> stopPointAction(
        new StopPointMovementAction());
    PartyBotEngine stopPointEngine;
    bool const stopPointRegistered =
        stopPointEngine.RegisterAction(stopPointAction);
    std::shared_ptr<Strategy> stopPointStrategy(
        new Strategy("simulator_stop_point_movement"));
    stopPointStrategy->AddTrigger(
        std::make_shared<SimulatorStopPointMovementTrigger>());
    stopPointEngine.AddStrategy(stopPointStrategy);

    float const stopPointDestinationY = healer->GetPositionY();
    float const stopPointDestinationZ = healer->GetPositionZ();
    uint64 const stopPointEncounterId = coordinator->GetEncounterId();
    movementTerminalListener->events.clear();

    ActionId const moveToStopActionA = coordinator->NextActionId();
    uint32 const moveToStopPointA =
        MovePointAction::PointIdForAction(moveToStopActionA);
    bool const moveToStopAStarted = moveToStopActionA && stopPointEncounterId &&
        healer->GetMotionMaster()->MovePointForEngine(
            moveToStopPointA, healer->GetPositionX() + 12.0f,
            stopPointDestinationY, stopPointDestinationZ, MOVE_NONE,
            moveToStopActionA, stopPointEncounterId, movementArrivalTolerance);
    MovementGenerator const* const moveAGenerator =
        healer->GetMotionMaster()->GetCurrent();
    PointMovementGenerator<Player> const* const moveAPoint =
        moveAGenerator &&
        moveAGenerator->GetMovementGeneratorType() == POINT_MOTION_TYPE
            ? static_cast<PointMovementGenerator<Player> const*>(moveAGenerator) : nullptr;
    bool const moveAIdentityCaptured = moveToStopAStarted && moveAPoint &&
        moveAPoint->GetEngineActionId() == moveToStopActionA &&
        moveAPoint->GetEngineEncounterId() == stopPointEncounterId &&
        moveAPoint->GetPointId() == moveToStopPointA;
    bool const moveToStopASnapshotCaptured = moveAIdentityCaptured &&
        capturePointMovementSnapshot(stopPointBaseBot, healer);
    PartySnapshot stopPointSnapshotA = stopPointBaseSnapshot;
    stopPointSnapshotA.epoch++;
    DecisionContext stopPointContextA(
        stopPointSnapshotA, stopPointSnapshotA.members[0], *coordinator,
        WorldTimer::getMSTime(), &engineActionAI.m_decisionTrace, nullptr,
        &engineActionAI, nullptr, &engineActionAI);
    DecisionReport stopPointReportA = stopPointEngine.Evaluate(stopPointContextA);
    bool const stopPointASelected = moveToStopASnapshotCaptured &&
        stopPointReportA.selectedIndices.size() == 1 &&
        stopPointReportA.candidates[
            stopPointReportA.selectedIndices[0]].status == CandidateStatus::Selected;

    // Core MotionMaster::Mutate stacks a new Point generator and calls
    // Interrupt() on the old generator; it does not Finalize the old one.
    // Therefore B becomes current while A remains underneath, and neither
    // receives a terminal until each is explicitly expired.
    ActionId const replacementMoveActionB = coordinator->NextActionId();
    uint32 const replacementMovePointB =
        MovePointAction::PointIdForAction(replacementMoveActionB);
    bool const replacementBStarted = replacementMoveActionB &&
        healer->GetMotionMaster()->MovePointForEngine(
            replacementMovePointB, healer->GetPositionX() + 8.0f,
            stopPointDestinationY, stopPointDestinationZ, MOVE_NONE,
            replacementMoveActionB, stopPointEncounterId, movementArrivalTolerance);
    MovementGenerator const* const replacementBGenerator =
        healer->GetMotionMaster()->GetCurrent();

    bool moveAGeneratorStillStacked = false;
    for (MotionMaster::const_iterator it = healer->GetMotionMaster()->begin();
         it != healer->GetMotionMaster()->end(); ++it)
        if (*it == moveAGenerator)
            moveAGeneratorStillStacked = true;
    bool const noPrematureTerminalForA = movementTerminalListener->events.empty();

    if (stopPointASelected)
        stopPointEngine.ExecuteSelected(stopPointContextA, stopPointReportA);
    CandidateEvaluation const* staleStopPointResult = stopPointASelected
        ? &stopPointReportA.candidates[
            stopPointReportA.selectedIndices[0]] : nullptr;
    PointMovementGenerator<Player> const* replacementBPoint =
        replacementBGenerator &&
        replacementBGenerator->GetMovementGeneratorType() == POINT_MOTION_TYPE
            ? static_cast<PointMovementGenerator<Player> const*>(
                replacementBGenerator) : nullptr;
    bool const staleStopPointRejected = staleStopPointResult &&
        staleStopPointResult->status == CandidateStatus::ExecutionFailed &&
        staleStopPointResult->actionResult == ActionResult::Impossible &&
        staleStopPointResult->reason ==
            "stop_point_movement_live_generator_identity_mismatch";
    bool const replacementBPreserved = replacementBStarted &&
        healer->GetMotionMaster()->GetCurrent() == replacementBGenerator &&
        replacementBPoint &&
        replacementBPoint->GetEngineActionId() == replacementMoveActionB &&
        replacementBPoint->GetEngineEncounterId() == stopPointEncounterId &&
        replacementBPoint->GetPointId() == replacementMovePointB;

    // Expire B first. Its own correlated Interrupted event must arrive, and
    // the previously stacked A must resume as the current generator.
    if (replacementBStarted)
        healer->GetMotionMaster()->MovementExpired(false);
    bool const replacementBInterrupted = movementTerminalListener->events.size() == 1 &&
        movementTerminalListener->events[0].actionId == replacementMoveActionB &&
        movementTerminalListener->events[0].encounterId == stopPointEncounterId &&
        movementTerminalListener->events[0].moverGuid == healerMember.guid &&
        movementTerminalListener->events[0].pointId == replacementMovePointB &&
        movementTerminalListener->events[0].status ==
            PointMovementTerminalStatus::Interrupted;
    PointMovementGenerator<Player> const* resumedAPoint =
        healer->GetMotionMaster()->GetCurrent() &&
        healer->GetMotionMaster()->GetCurrent()->GetMovementGeneratorType() ==
            POINT_MOTION_TYPE
            ? static_cast<PointMovementGenerator<Player> const*>(
                healer->GetMotionMaster()->GetCurrent()) : nullptr;
    bool const moveAResumed = moveAGeneratorStillStacked &&
        healer->GetMotionMaster()->GetCurrent() == moveAGenerator &&
        resumedAPoint &&
        resumedAPoint->GetEngineActionId() == moveToStopActionA &&
        resumedAPoint->GetEngineEncounterId() == stopPointEncounterId &&
        resumedAPoint->GetPointId() == moveToStopPointA;

    // Expire the resurfaced A as fixture cleanup and require its own terminal.
    if (moveAResumed)
        healer->GetMotionMaster()->MovementExpired(false);
    bool const moveAInterrupted = movementTerminalListener->events.size() == 2 &&
        movementTerminalListener->events[1].actionId == moveToStopActionA &&
        movementTerminalListener->events[1].encounterId == stopPointEncounterId &&
        movementTerminalListener->events[1].moverGuid == healerMember.guid &&
        movementTerminalListener->events[1].pointId == moveToStopPointA &&
        movementTerminalListener->events[1].status ==
            PointMovementTerminalStatus::Interrupted;

    // A legacy point move has zero Engine action/encounter IDs and must not be
    // proposed or expired by this adapter.
    healer->GetMotionMaster()->MovePoint(
        0xF443u, healer->GetPositionX() + 4.0f,
        stopPointDestinationY, stopPointDestinationZ, MOVE_NONE);
    MovementGenerator const* const legacyPointGenerator =
        healer->GetMotionMaster()->GetCurrent();
    PartySnapshot legacyPointSnapshot = stopPointBaseSnapshot;
    legacyPointSnapshot.epoch += 2;
    legacyPointSnapshot.members[0].pointMovementActionId = 0;
    legacyPointSnapshot.members[0].pointMovementEncounterId = 0;
    legacyPointSnapshot.members[0].pointMovementPointId = 0;
    DecisionContext legacyPointContext(
        legacyPointSnapshot, legacyPointSnapshot.members[0], *coordinator,
        WorldTimer::getMSTime(), &engineActionAI.m_decisionTrace, nullptr,
        &engineActionAI, nullptr, &engineActionAI);
    DecisionReport legacyPointReport = stopPointEngine.Evaluate(legacyPointContext);
    bool const legacyPointNotProposed =
        legacyPointReport.selectedIndices.empty() &&
        legacyPointReport.candidates.empty();
    bool const legacyPointPreserved =
        legacyPointGenerator &&
        healer->GetMotionMaster()->GetCurrent() == legacyPointGenerator;
    healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);

    // Start point C and stop it using C's fresh value snapshot.
    movementTerminalListener->events.clear();
    ActionId const stopPointActionC = coordinator->NextActionId();
    uint32 const stopPointIdC = MovePointAction::PointIdForAction(stopPointActionC);
    bool const stopPointCStarted = stopPointActionC &&
        healer->GetMotionMaster()->MovePointForEngine(
            stopPointIdC, healer->GetPositionX() + 10.0f,
            stopPointDestinationY, stopPointDestinationZ, MOVE_NONE,
            stopPointActionC, stopPointEncounterId, movementArrivalTolerance);
    PartySnapshot stopPointSnapshotC = stopPointBaseSnapshot;
    stopPointSnapshotC.epoch += 3;
    bool const stopPointCSnapshotCaptured = stopPointCStarted &&
        capturePointMovementSnapshot(stopPointSnapshotC.members[0], healer);
    DecisionContext stopPointContextC(
        stopPointSnapshotC, stopPointSnapshotC.members[0], *coordinator,
        WorldTimer::getMSTime(), &engineActionAI.m_decisionTrace, nullptr,
        &engineActionAI, nullptr, &engineActionAI);
    std::string stopPointReasonC;
    bool const stopPointSnapshotPossible = stopPointAction->IsPossible(
        stopPointContextC, ActionCandidate(), stopPointReasonC);
    bool const stopPointUseful = stopPointAction->IsUseful(
        stopPointContextC, ActionCandidate(), stopPointReasonC);
    DecisionReport stopPointReportC = stopPointEngine.Evaluate(stopPointContextC);
    bool const stopPointCSelected = stopPointCSnapshotCaptured &&
        stopPointSnapshotPossible && stopPointUseful &&
        stopPointReportC.selectedIndices.size() == 1 &&
        stopPointReportC.candidates[
            stopPointReportC.selectedIndices[0]].status == CandidateStatus::Selected;

    ObjectGuid const stopPointSelectionBefore = healer->GetSelectionGuid();
    ObjectGuid const stopPointTargetBefore = healer->GetTargetGuid();
    Unit* const stopPointVictimBefore = healer->GetVictim();
    bool const stopPointCombatBefore = healer->IsInCombat();
    uint32 const stopPointHealthBefore = healer->GetHealth();
    Powers const stopPointPowerType = healer->GetPowerType();
    uint32 const stopPointPowerBefore = healer->GetPower(stopPointPowerType);

    if (stopPointCSelected)
        stopPointEngine.ExecuteSelected(stopPointContextC, stopPointReportC);
    CandidateEvaluation const* stopPointCEvaluation = stopPointCSelected
        ? &stopPointReportC.candidates[stopPointReportC.selectedIndices[0]] : nullptr;
    bool const stopPointCExecuted = stopPointCEvaluation &&
        stopPointCEvaluation->status == CandidateStatus::Executed &&
        stopPointCEvaluation->actionResult == ActionResult::Success &&
        stopPointCEvaluation->reason ==
            "stop_point_movement_expired_matching_engine_generator";
    bool const stopPointCInterrupted = movementTerminalListener->events.size() == 1 &&
        movementTerminalListener->events[0].actionId == stopPointActionC &&
        movementTerminalListener->events[0].encounterId == stopPointEncounterId &&
        movementTerminalListener->events[0].moverGuid == healerMember.guid &&
        movementTerminalListener->events[0].pointId == stopPointIdC &&
        movementTerminalListener->events[0].status ==
            PointMovementTerminalStatus::Interrupted;
    bool const stopPointStatePreserved =
        healer->GetSelectionGuid() == stopPointSelectionBefore &&
        healer->GetTargetGuid() == stopPointTargetBefore &&
        healer->GetVictim() == stopPointVictimBefore &&
        healer->IsInCombat() == stopPointCombatBefore &&
        healer->GetHealth() == stopPointHealthBefore &&
        healer->GetPower(stopPointPowerType) == stopPointPowerBefore;
    bool const stopPointFixturePassed =
        stopPointRegistered && moveToStopAStarted && moveToStopASnapshotCaptured &&
        stopPointASelected && moveAGeneratorStillStacked &&
        noPrematureTerminalForA && replacementBStarted &&
        staleStopPointRejected && replacementBPreserved && replacementBInterrupted &&
        moveAResumed && moveAInterrupted &&
        legacyPointNotProposed && legacyPointPreserved &&
        stopPointCStarted && stopPointCSnapshotCaptured &&
        stopPointSnapshotPossible && stopPointUseful && stopPointCSelected &&
        stopPointCExecuted && stopPointCInterrupted && stopPointStatePreserved;

    if (healer->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
        POINT_MOTION_TYPE)
        healer->GetMotionMaster()->ClearType(POINT_MOTION_TYPE);

    sLog.Out(LOG_BASIC, stopPointFixturePassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
        "[PartyBotSimulator] StopPointMovementAction: registered=%u moveAStarted=%u snapshotCaptured=%u selected=%u moveAStacked=%u noPrematureTerminal=%u replacementBStarted=%u staleRejected=%u replacementPreserved=%u replacementInterrupted=%u moveAResumed=%u moveAInterrupted=%u legacyNotProposed=%u legacyPreserved=%u moveCStarted=%u snapshotPossible=%u selectedC=%u stopExecuted=%u stopInterrupted=%u statePreserved=%u.",
        stopPointRegistered ? 1u : 0u, moveToStopAStarted ? 1u : 0u,
        moveToStopASnapshotCaptured ? 1u : 0u, stopPointASelected ? 1u : 0u,
        moveAGeneratorStillStacked ? 1u : 0u, noPrematureTerminalForA ? 1u : 0u,
        replacementBStarted ? 1u : 0u, staleStopPointRejected ? 1u : 0u,
        replacementBPreserved ? 1u : 0u, replacementBInterrupted ? 1u : 0u,
        moveAResumed ? 1u : 0u, moveAInterrupted ? 1u : 0u,
        legacyPointNotProposed ? 1u : 0u, legacyPointPreserved ? 1u : 0u,
        stopPointCStarted ? 1u : 0u,
        stopPointSnapshotPossible ? 1u : 0u, stopPointCSelected ? 1u : 0u,
        stopPointCExecuted ? 1u : 0u, stopPointCInterrupted ? 1u : 0u,
        stopPointStatePreserved ? 1u : 0u);
    if (stopPointFixturePassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[PartyBotSimulator] PASS: StopPointMovementAction rejected a stale A snapshot while stacked Engine move B remained current, observed B then A Interrupted terminals in Core expiry order, ignored a legacy point generator, and cancelled exact Engine move C with its matching Interrupted terminal.");

    DecisionSummary const decisionSummary = SummarizeDecisionReport(report);
    std::vector<std::size_t> const candidateDetailIndices =
        SelectPriorityCandidateEvaluationIndices(
            report, DecisionReport::MaxCandidateTraceRows);
    bool candidateDetailTracePassed = engineActionTracePassed &&
        candidateDetailIndices.size() == DecisionReport::MaxCandidateTraceRows &&
        candidateDetailIndices[0] == report.selectedIndices[0] &&
        report.candidates.size() == 5 &&
        report.candidates[candidateDetailIndices[0]].status == CandidateStatus::Executed &&
        decisionSummary.candidateDetailOmitted == 2;
    uint32 candidateDetailRowsEmitted = 0;
    if (candidateDetailTracePassed && coordinator->HasActiveEncounter())
    {
        for (std::size_t index : candidateDetailIndices)
        {
            if (coordinator->TryAdmitEncounterTraceEvent(
                    coordinator->GetEncounterId(), EncounterTraceEventClass::Detail) !=
                EncounterTraceAdmission::Accepted)
            {
                candidateDetailTracePassed = false;
                break;
            }
            uint32 const sequence = coordinator->NextEncounterSequence();
            CandidateEvaluation const& evaluation = report.candidates[index];
            ActionCandidate const& candidate = evaluation.candidate;
            char const* status = evaluation.status == CandidateStatus::Executed
                ? "executed" : (evaluation.status == CandidateStatus::RejectedMissingAction
                    ? "rejected_missing_action" : "candidate");
            engineActionAI.m_decisionTrace.RecordEncounterCandidate(
                healer, snapshot.groupId, snapshot.mapId, snapshot.instanceId,
                snapshot.epoch, coordinator->GetEncounterId(), sequence,
                coordinator->GetEncounterElapsedMs(3000), "healer",
                candidate.strategyName.c_str(), candidate.triggerName.c_str(),
                candidate.actionName.c_str(), candidate.targetGuid, candidate.spellId,
                static_cast<uint32>(candidate.channel), candidate.finalPriority,
                status, evaluation.reason.empty() ? "none" : evaluation.reason.c_str());
            if (sequence)
                ++candidateDetailRowsEmitted;
        }
    }

    bool decisionSummaryTracePassed = engineActionTracePassed &&
        decisionSummary.candidateCount == 5 &&
        decisionSummary.selectedCount == 1 &&
        decisionSummary.missingActionCount == 4 &&
        decisionSummary.executedCount == 1 &&
        decisionSummary.executionFailedCount == 0 &&
        decisionSummary.candidateDetailLimit == 3 &&
        decisionSummary.candidateDetailOmitted == 2 &&
        candidateDetailTracePassed && candidateDetailRowsEmitted == 3;
    if (decisionSummaryTracePassed && coordinator->HasActiveEncounter())
    {
        EncounterTraceAdmission const admission = coordinator->TryAdmitEncounterTraceEvent(
            coordinator->GetEncounterId(), EncounterTraceEventClass::Detail);
        uint32 const sequence = admission == EncounterTraceAdmission::Accepted
            ? coordinator->NextEncounterSequence() : 0;
        if (sequence)
            engineActionAI.m_decisionTrace.RecordEncounterDecisionSummary(
                healer, snapshot.groupId, snapshot.mapId, snapshot.instanceId,
                snapshot.epoch, coordinator->GetEncounterId(), sequence,
                coordinator->GetEncounterElapsedMs(3000), "healer", decisionSummary);
        else
            decisionSummaryTracePassed = false;
    }
    else
        decisionSummaryTracePassed = false;

    healer->SetAI(nullptr);
    engineActionAI.Remove();
    healerSession.LogoutPlayer(false);
    bool const playerCleaned = healerSession.GetPlayer() == nullptr;
    healer = nullptr; // LogoutPlayer(true/false) removes and deletes the in-world Player.
    bool const patientCleaned = DestroySimulatorActor(map, patient);
    bool const cleaned = playerCleaned && patientCleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine Player spell/heal: known=%u selected=%u accepted=%u pending=%u gcd=%u castResult=%u manaAtAccept=%u>%u manaAtFinish=%u>%u lastManaUse=%u health=%u>%u completed=%u engineActionTrace=%u decisionSummaryTrace=%u candidateDetailTrace=%u candidateRows=%u actionId=%llu continuationResult=%u coreTerminal=%u continuationWaiters=%zu pendingBeforeDispatch=%zu dispatchSelected=%zu dispatchExecuted=%zu dispatchRejected=%zu dispatchStale=%zu continuationExecuted=%u continuationEnqueued=%u continuationDispatched=%u cleanup=%u.",
             1u, selected ? 1u : 0u, accepted ? 1u : 0u,
             healAction->WasCastPendingAfterAccept() ? 1u : 0u,
             healAction->HadGcdAfterAccept() ? 1u : 0u,
             static_cast<uint32>(healAction->GetLastCastResult()),
             manaBefore, manaAfterAccept, manaBefore, manaAfter,
             coreManaSpendObserved ? 1u : 0u,
             healthBefore, healthAfter,
             completed ? 1u : 0u,
             engineActionTracePassed ? 1u : 0u,
             decisionSummaryTracePassed ? 1u : 0u,
             candidateDetailTracePassed ? 1u : 0u,
             candidateDetailRowsEmitted,
             static_cast<unsigned long long>(engineActionId),
             static_cast<uint32>(healAction->GetContinuationResult()),
             coreSpellTerminalPassed ? 1u : 0u,
             continuationWaitersBeforeDispatch, pendingBeforeContinuationDispatch,
             continuationDispatch.selected, continuationDispatch.executed,
             continuationDispatch.rejected, continuationDispatch.staleContext,
             f34ContinuationExecutionCount, continuationEnqueued ? 1u : 0u,
             continuationDispatched ? 1u : 0u, cleaned ? 1u : 0u);

    if (!registered || !continuationActionRegistered || !selected || !accepted || !completed ||
        !engineActionTracePassed || !coreSpellTerminalPassed || !continuationEnqueued ||
        !continuationDispatched || !f35MultiDependencyDispatched ||
        !f35MultiCoreTerminalsPassed || !f35AlternativeGroupPassed ||
        !f36MixedDependencyPassed || !f39MovementTerminalPassed ||
        !decisionSummaryTracePassed || !movePointFixturePassed ||
        !stopPointFixturePassed || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine-to-Player normal spell/heal smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Engine action ID flowed through PartyBotAI into Core Spell::prepare/finish; one action_id correlated the adapter attempt/result and the later successful Core cast terminal, with real healing and mana effects.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F32 Core Spell::finish delivered a value-only terminal event to the registered Engine listener with the same action/encounter/spell/target correlation as the adapter attempt; Core success was distinct from SPELL_CAST_OK acceptance.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F34 released the registered continuation only after the matching successful Core terminal and executed it through the next Engine pending dispatch.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine Player spell/heal F35/F36: secondReady=%u secondSelected=%u secondAccepted=%u secondCompleted=%u coreEvents=%zu dependencyResult=%u pending=%zu dispatched=%u continuationExecutions=%u altFirst=%u altSecond=%u altSelected=%u altRejected=%u altDispatched=%u altSelectedExec=%u altRejectedExec=%u mixedResult=%u mixedSelected=%zu mixedExecuted=%zu mixedFollowups=%u mixedPassed=%u.",
             f35SecondCastReady ? 1u : 0u, f35SecondCastSelected ? 1u : 0u,
             f35SecondCastAccepted ? 1u : 0u, f35SecondCastCompleted ? 1u : 0u,
             coreTerminalListener->events.size(),
             static_cast<uint32>(f35DependencyResult), engine.GetPendingActionCount(),
             f35MultiDependencyDispatched ? 1u : 0u, continuationExecutionCount,
             static_cast<uint32>(f35AlternativeFirstResult),
             static_cast<uint32>(f35AlternativeSecondResult),
             f35AlternativeDispatch.selected, f35AlternativeDispatch.rejected,
             f35AlternativeGroupPassed ? 1u : 0u,
             f35AlternativeSelectedExecutions, f35AlternativeRejectedExecutions,
             static_cast<uint32>(f36MixedDependencyResult),
             f36MixedDependencyDispatch.selected, f36MixedDependencyDispatch.executed,
             actionResultAndSpellFollowupCount, f36MixedDependencyPassed ? 1u : 0u);
    if (!f35MultiDependencyDispatched || !f35MultiCoreTerminalsPassed ||
        !f35AlternativeGroupPassed || !f36MixedDependencyPassed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] F35 multi-prerequisite / alternative Core terminal fixture failed.");
        return false;
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F35 gated one follow-up on two distinct successful Core Spell::finish terminals and dispatched it through the normal Engine pending queue.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F35 resolved a real Core terminal alternative group, rejected the higher-priority wrong-status branch, and dispatched the valid branch.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F36 required both the synchronous successful ActionResult and the later successful Core Spell::finish for the same action ID before dispatching its child.");
    sLog.Out(LOG_BASIC, f39MovementTerminalPassed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] %s: F39 emitted a correlated Reached terminal only after natural PointMovementGenerator completion and destination tolerance check; the movement prerequisite released its continuation through the next pending dispatch, while ClearType produced Interrupted.",
             f39MovementTerminalPassed ? "PASS" : "FAIL");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F20 DecisionSummary aggregated the complete executed Core Player heal report and emitted the bounded encounter summary.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: F24 candidate detail cap reported five complete candidates, retained the selected Core heal first after execution, emitted three rows and counted two omitted alternatives.");
    return true;
}

bool RunEngineCoreExecutorSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor party[5];
    SimulatorActor enemy;
    bool created = true;
    for (uint32 i = 0; i < 5 && created; ++i)
        created = CreateSimulatorActor(map, *info, partyFaction,
                                       static_cast<float>(i), 2.0f, party[i]);
    if (created)
        created = CreateSimulatorActor(map, *info, hostileFaction, 5.0f, 2.0f, enemy);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, enemy);
        for (uint32 i = 0; i < 5; ++i)
            cleaned = DestroySimulatorActor(map, party[i]) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine executor fixture creation failed (cleanup=%u).", cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120; ++tick)
    {
        bool ready = enemy.updateCount != 0;
        for (uint32 i = 0; i < 5; ++i)
            ready = ready && party[i].updateCount != 0;
        if (ready)
            break;
        map.Update(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    PartySnapshot snapshot;
    snapshot.groupId = 1;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 1;
    snapshot.capturedAtMs = 1000;
    for (uint32 i = 0; i < 5; ++i)
    {
        PartyMemberSnapshot member;
        member.guid = party[i].creature->GetObjectGuid().GetRawValue();
        member.roleId = static_cast<std::uint8_t>(
            i == 0 ? PartyRole::Tank :
            i == 1 ? PartyRole::Healer :
            i == 2 ? PartyRole::MeleeDamage :
            i == 3 ? PartyRole::RangedDamage : PartyRole::Support);
        member.mapId = map.GetId();
        member.instanceId = map.GetInstanceId();
        member.health = party[i].creature->GetHealth();
        member.maxHealth = party[i].creature->GetMaxHealth();
        member.online = true;
        member.alive = party[i].creature->IsAlive();
        member.isMainTank = i == 0;
        snapshot.members.push_back(member);
    }

    HostileSnapshot hostile;
    hostile.guid = enemy.creature->GetObjectGuid().GetRawValue();
    hostile.health = enemy.creature->GetHealth();
    hostile.maxHealth = enemy.creature->GetMaxHealth();
    hostile.alive = enemy.creature->IsAlive();
    snapshot.hostiles.push_back(hostile);

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 1000);
    PartyBotEngine engine;
    bool const registered = engine.RegisterAction(std::make_shared<SimulatorCoreAttackAction>(map));
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_tank_assist"));
    strategy->AddTrigger(std::make_shared<SimulatorTankAssistTrigger>());
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == hostile.guid &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(context, report);
    bool const executed = !report.selectedIndices.empty() &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed &&
        report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Success &&
        party[0].creature->GetVictim() == enemy.creature;

    party[0].creature->AttackStop();
    bool cleaned = DestroySimulatorActor(map, enemy);
    for (uint32 i = 0; i < 5; ++i)
        cleaned = DestroySimulatorActor(map, party[i]) && cleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine executor: party=5 hostile=1 aiReady=%u registered=%u candidates=%u selected=%u executed=%u cleanup=%u.",
             (enemy.updateCount != 0 && party[0].updateCount != 0 && party[1].updateCount != 0 &&
              party[2].updateCount != 0 && party[3].updateCount != 0 && party[4].updateCount != 0) ? 1u : 0u,
             registered ? 1u : 0u, static_cast<uint32>(report.candidates.size()),
             selected ? 1u : 0u, executed ? 1u : 0u, cleaned ? 1u : 0u);

    if (!registered || !selected || !executed || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine-to-Core executor smoke test failed.");
        return false;
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine selected a tank-assist action and executed real Unit::Attack against a hostile Core Creature.");
    return true;
}

bool RunCoreCombatThreatSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    if (!info)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] No safe creature_template found (requires a normal-rank template with a display and no spawn spell, spell list or script).");
        return false;
    }

    uint32 bossFaction = 0;
    uint32 attackerFaction = 0;
    if (!FindMutuallyHostileFactions(bossFaction, attackerFaction))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Could not find a mutually hostile pair in the loaded faction_template data.");
        return false;
    }

    SimulatorActor boss;
    SimulatorActor tank;
    SimulatorActor damageDealer;

    bool created = CreateSimulatorActor(map, *info, bossFaction, 0.0f, 0.0f, boss) &&
                   CreateSimulatorActor(map, *info, attackerFaction, 1.0f, 0.0f, tank) &&
                   CreateSimulatorActor(map, *info, attackerFaction, 2.0f, 0.0f, damageDealer);

    if (!created)
    {
        bool const cleanedBoss = DestroySimulatorActor(map, boss);
        bool const cleanedTank = DestroySimulatorActor(map, tank);
        bool const cleanedDamageDealer = DestroySimulatorActor(map, damageDealer);
        if (!cleanedBoss || !cleanedTank || !cleanedDamageDealer)
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PartyBotSimulator] Actor cleanup failed after a partial create.");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Failed to create and insert all three template-backed Creature actors.");
        return false;
    }

    bool const hostilePair = boss.creature->IsHostileTo(tank.creature) &&
                             tank.creature->IsHostileTo(boss.creature) &&
                             boss.creature->IsHostileTo(damageDealer.creature) &&
                             damageDealer.creature->IsHostileTo(boss.creature);

    // Map cell updates are paced by the Core's wall-clock update-cells
    // interval. Drive bounded real map ticks and require each custom AI to
    // prove that the active-object update path reached it.
    for (uint32 tick = 0; tick < 120 &&
         (!boss.updateCount || !tank.updateCount || !damageDealer.updateCount); ++tick)
    {
        map.Update(50);
        if (!boss.updateCount || !tank.updateCount || !damageDealer.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    bool const aiUpdated = boss.updateCount && tank.updateCount && damageDealer.updateCount;
    bool damageAndThreat = false;
    bool targetSelection = false;
    uint32 initialHealth = 0;
    uint32 healthAfterTank = 0;
    uint32 healthAfterDps = 0;
    uint32 healthAfterFinalHit = 0;
    uint32 tankDamage = 0;
    uint32 dpsDamage = 0;
    uint32 additionalDamage = 0;
    float tankThreat = 0.0f;
    float dpsThreat = 0.0f;
    float estimatedTankThreat = 0.0f;
    bool tankImpactMatchesCore = false;
    uint32 selectedAfterTankGuid = 0;
    uint32 selectedBelowThresholdGuid = 0;
    uint32 selectedAboveThresholdGuid = 0;

    if (hostilePair && aiUpdated)
    {
        initialHealth = boss.creature->GetHealth();
        ThreatImpactEstimate const tankDamageImpact = EstimateAppliedDirectDamageComponent(
            tank.creature, boss.creature, 100, SPELL_SCHOOL_MASK_NORMAL, 1,
            ThreatImpactBound::Exact);
        bool const tankImpactShapeValid =
            tankDamageImpact.completeness == ThreatImpactCompleteness::Unknown &&
            tankDamageImpact.snapshotEpoch == 1 && tankDamageImpact.impacts.size() == 1 &&
            tankDamageImpact.impacts[0].source == ThreatImpactSource::DirectDamage &&
            tankDamageImpact.impacts[0].bound == ThreatImpactBound::Exact;
        estimatedTankThreat = tankImpactShapeValid
            ? tankDamageImpact.impacts[0].threatPoints : -1.0f;
        tankDamage = tank.creature->DealDamage(
            boss.creature, 100, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        tankThreat = boss.creature->GetThreatManager().getThreat(tank.creature);
        tankImpactMatchesCore = tankImpactShapeValid && tankDamage == 100 &&
            std::fabs(estimatedTankThreat - tankThreat) < 0.01f;
        healthAfterTank = boss.creature->GetHealth();

        Unit* selectedAfterTank = boss.creature->GetThreatManager().getHostileTarget();
        selectedAfterTankGuid = selectedAfterTank ? selectedAfterTank->GetGUIDLow() : 0;

        dpsDamage = damageDealer.creature->DealDamage(
            boss.creature, 105, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        dpsThreat = boss.creature->GetThreatManager().getThreat(damageDealer.creature);
        healthAfterDps = boss.creature->GetHealth();
        Unit* selectedBelowThreshold = boss.creature->GetThreatManager().getHostileTarget();
        selectedBelowThresholdGuid = selectedBelowThreshold ? selectedBelowThreshold->GetGUIDLow() : 0;

        additionalDamage = damageDealer.creature->DealDamage(
            boss.creature, 10, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        dpsThreat = boss.creature->GetThreatManager().getThreat(damageDealer.creature);
        healthAfterFinalHit = boss.creature->GetHealth();
        Unit* selectedAboveThreshold = boss.creature->GetThreatManager().getHostileTarget();
        selectedAboveThresholdGuid = selectedAboveThreshold ? selectedAboveThreshold->GetGUIDLow() : 0;

        damageAndThreat = tankDamage > 0 && dpsDamage > 0 && additionalDamage > 0 &&
                          healthAfterTank < initialHealth && healthAfterDps < healthAfterTank &&
                          healthAfterFinalHit < healthAfterDps && tankThreat > 0.0f && dpsThreat > 0.0f &&
                          tankImpactMatchesCore;

        targetSelection = selectedAfterTank == tank.creature &&
                          selectedBelowThreshold == tank.creature &&
                          dpsThreat > 1.1f * tankThreat &&
                          selectedAboveThreshold == damageDealer.creature;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Creature probe: template=%u factions=%u/%u aiUpdates=%u/%u/%u hostile=%u health=%u>%u>%u>%u damage=%u/%u/%u threat=%.1f/%.1f switchThreshold=%.1f selectedGUID=%u/%u/%u damageThreat=%u impactThreat=%.1f impactMatches=%u targetSwitch=%u.",
             info->entry, bossFaction, attackerFaction,
             boss.updateCount, tank.updateCount, damageDealer.updateCount,
             hostilePair ? 1u : 0u, initialHealth, healthAfterTank, healthAfterDps, healthAfterFinalHit,
             tankDamage, dpsDamage, additionalDamage, tankThreat, dpsThreat, 1.1f * tankThreat,
             selectedAfterTankGuid, selectedBelowThresholdGuid, selectedAboveThresholdGuid,
             damageAndThreat ? 1u : 0u, estimatedTankThreat,
             tankImpactMatchesCore ? 1u : 0u, targetSelection ? 1u : 0u);

    // Clear the boss-owned threat references before destroying their targets.
    bool const cleanedBoss = DestroySimulatorActor(map, boss);
    bool const cleanedTank = DestroySimulatorActor(map, tank);
    bool const cleanedDamageDealer = DestroySimulatorActor(map, damageDealer);
    bool const actorsCleaned = cleanedBoss && cleanedTank && cleanedDamageDealer;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Actor cleanup: boss=%u tank=%u damageDealer=%u.",
             cleanedBoss ? 1u : 0u, cleanedTank ? 1u : 0u, cleanedDamageDealer ? 1u : 0u);

    if (!hostilePair || !aiUpdated || !damageAndThreat || !targetSelection || !actorsCleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core combat/threat smoke test failed (actorsCleaned=%u).", actorsCleaned ? 1u : 0u);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: real Creature updates, a Core-calculated applied-damage threat component matched the real ThreatManager increment, and Core retained its 110%% melee target switching.");
    return true;
}


bool RunPeriodicDamageThreatFixture(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor caster;
    SimulatorActor hostile;
    bool const created =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 24.0f, caster) &&
        CreateSimulatorActor(map, *info, hostileFaction, 2.0f, 24.0f, hostile);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, hostile);
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic damage fixture actor creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    // Give the spell a deterministic hit advantage and remove template-provided
    // Shadow resistance. The simulator still exercises the real Core spell-hit
    // and Aura application path; it must not fail intermittently because the
    // generic creature template happened to resist a periodic tick's initial cast.
    caster.creature->SetLevel(63);
    hostile.creature->SetLevel(60);
    hostile.creature->SetInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_SHADOW, 0);

    // Shadow Word: Pain (rank 8) is an ordinary one-effect periodic-damage aura
    // in the pinned 5875 data. Reject data drift instead of silently exercising
    // a different aura family or a scripted special case.
    uint32 const spellId = 10894;
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    bool const spellValid = spell &&
        spell->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
        spell->EffectApplyAuraName[0] == SPELL_AURA_PERIODIC_DAMAGE &&
        !spell->Effect[1] && !spell->Effect[2] &&
        !spell->HasAttribute(SPELL_ATTR_EX4_NO_HARMFUL_THREAT) &&
        !spell->HasAttribute(SPELL_ATTR_EX_NO_THREAT);
    if (!spellValid)
    {
        bool cleaned = DestroySimulatorActor(map, hostile);
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Expected simple periodic-damage spell %u was not available (cleanup=%u).",
                 spellId, cleaned ? 1u : 0u);
        return false;
    }

    // Keep the victim in combat so passive Creature regeneration cannot hide
    // the effective tick damage. The simulator AI intentionally does not attack.
    hostile.creature->SetInCombatWith(caster.creature);
    SpellCastResult castResult = SPELL_FAILED_DONT_REPORT;
    Aura* aura = nullptr;
    uint32 castAttempts = 0;
    // A triggered cast can be accepted while its unit target independently
    // resists the spell. Retry only that real Core cast path, and require the
    // resulting Aura object before proceeding to the periodic-tick assertion.
    for (; castAttempts < 4 && !aura; ++castAttempts)
    {
        castResult = caster.creature->CastSpell(hostile.creature, spell, true);
        if (castResult == SPELL_CAST_OK)
            aura = hostile.creature->GetAura(spellId, EFFECT_INDEX_0);
    }
    bool const auraIsOrdinary = aura && aura->IsPeriodic() &&
        aura->GetAuraScript() == nullptr && aura->GetAuraPeriodicTimer() > 0;
    if (!auraIsOrdinary)
    {
        bool cleaned = DestroySimulatorActor(map, hostile);
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic damage aura did not apply as a simple Core aura (attempts=%u cast=%u aura=%u script=%u timer=%d shadowResistance=%d casterLevel=%u targetLevel=%u cleanup=%u).",
                 castAttempts, static_cast<uint32>(castResult), aura ? 1u : 0u,
                 aura && aura->GetAuraScript() ? 1u : 0u,
                 aura ? aura->GetAuraPeriodicTimer() : -1,
                 hostile.creature->GetResistance(SPELL_SCHOOL_SHADOW),
                 caster.creature->GetLevel(), hostile.creature->GetLevel(),
                 cleaned ? 1u : 0u);
        return false;
    }

    uint32 const healthBefore = hostile.creature->GetHealth();
    uint32 const ticksBefore = aura->GetAuraTicks();
    int32 const durationBefore = aura->GetAuraDuration();
    int32 const maxDuration = aura->GetAuraMaxDuration();
    int32 const timerBefore = aura->GetAuraPeriodicTimer();
    float const threatBefore = hostile.creature->GetThreatManager().getThreat(caster.creature);

    // Advance the ordinary active Creature/Aura update path. Duration can be
    // zero for a permanent/otherwise non-expiring aura, so it must not gate
    // ticking. Re-resolve the Aura after each map update in case Core removes it.
    uint32 updates = 0;
    for (; updates < 64; ++updates)
    {
        map.Update(250);
        // Map/Unit active-object updates are wall-clock throttled; tiny sleeps
        // do not advance Aura's periodic timer by the synthetic Map::Update diff.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        aura = hostile.creature->GetAura(spellId, EFFECT_INDEX_0);
        if (!aura || aura->GetAuraTicks() > ticksBefore)
            break;
    }

    uint32 const ticksAfter = aura ? aura->GetAuraTicks() : 0;
    int32 const timerAfter = aura ? aura->GetAuraPeriodicTimer() : -1;
    int32 const durationAfter = aura ? aura->GetAuraDuration() : -1;
    uint32 const healthAfter = hostile.creature->GetHealth();
    float const threatAfter = hostile.creature->GetThreatManager().getThreat(caster.creature);
    uint32 const appliedDamage = healthBefore > healthAfter
        ? healthBefore - healthAfter : 0;
    float const actualThreatDelta = threatAfter - threatBefore;

    ThreatImpactEstimate const estimate = EstimateAppliedPeriodicDamageComponent(
        caster.creature, hostile.creature, appliedDamage, spell, 88001);
    bool const estimateShape = estimate.completeness == ThreatImpactCompleteness::Unknown &&
        estimate.impacts.size() == 1 &&
        estimate.impacts[0].threatOwnerGuid == caster.creature->GetObjectGuid().GetRawValue() &&
        estimate.impacts[0].hostileGuid == hostile.creature->GetObjectGuid().GetRawValue() &&
        estimate.impacts[0].source == ThreatImpactSource::PeriodicDamage &&
        estimate.impacts[0].bound == ThreatImpactBound::Exact;
    float const estimatedThreat = estimateShape ? estimate.impacts[0].threatPoints : -1.0f;
    bool const matched = ticksAfter == ticksBefore + 1 && appliedDamage > 0 &&
        estimateShape && std::fabs(actualThreatDelta - estimatedThreat) < 0.01f;

    bool cleaned = DestroySimulatorActor(map, hostile);
    cleaned = DestroySimulatorActor(map, caster) && cleaned;
    sLog.Out(LOG_BASIC, matched && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] Periodic damage tick: spell=%u ticks=%u>%u updates=%u duration=%d>%d maxDuration=%d timer=%d>%d appliedDamage=%u estimated=%.3f actualDelta=%.3f estimateShape=%u matched=%u cleanup=%u.",
             spellId, ticksBefore, ticksAfter, updates, durationBefore, durationAfter,
             maxDuration, timerBefore, timerAfter, appliedDamage, estimatedThreat,
             actualThreatDelta, estimateShape ? 1u : 0u, matched ? 1u : 0u,
             cleaned ? 1u : 0u);

    if (!matched || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core periodic-damage tick parity fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: a real Core Aura update delivered one periodic damage tick through Aura::PeriodicTick and Unit::DealDamage(DOT); the applied-tick threat component matched the actual ThreatManager delta.");
    return true;
}


bool RunPeriodicHealingAssistThreatFixture(Map& map)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic healing fixture: begin.");
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor healer;
    SimulatorActor healedTarget;
    SimulatorActor hostileA;
    SimulatorActor hostileB;
    bool created =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 36.0f, healer) &&
        CreateSimulatorActor(map, *info, partyFaction, 2.0f, 36.0f, healedTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 4.0f, 36.0f, hostileA) &&
        CreateSimulatorActor(map, *info, hostileFaction, 6.0f, 36.0f, hostileB);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, healedTarget) && cleaned;
        cleaned = DestroySimulatorActor(map, healer) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic healing fixture actor creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    healer.creature->SetLevel(60);
    healedTarget.creature->SetLevel(60);
    hostileA.creature->SetLevel(60);
    hostileB.creature->SetLevel(60);
    healedTarget.creature->SetHealth(50000);

    // Create two real hostile references on the healed Unit. The hostile
    // Creature ThreatManagers must both contain the healed target before the
    // periodic heal so Core's HostileRefManager::threatAssist divides by 2.
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic healing fixture: actors ready; seeding hostile references.");
    uint32 const seedDamageA = healedTarget.creature->DealDamage(
        hostileA.creature, 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    uint32 const seedDamageB = healedTarget.creature->DealDamage(
        hostileB.creature, 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    healedTarget.creature->SetInCombatWith(hostileA.creature);
    healedTarget.creature->SetInCombatWith(hostileB.creature);
    hostileA.creature->SetInCombatWith(healedTarget.creature);
    hostileB.creature->SetInCombatWith(healedTarget.creature);

    uint32 hostileReferenceCount = 0;
    for (HostileReference* reference = healedTarget.creature->GetHostileRefManager().getFirst();
         reference; reference = reference->next())
        ++hostileReferenceCount;

    // Rejuvenation rank 10 is an ordinary single-effect periodic heal in the
    // pinned Vanilla 5875 data. Reject DB/script drift, rather than substituting
    // a different effect family or broadening this fixture silently.
    uint32 const spellId = 9841;
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    bool const spellValid = spell &&
        spell->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
        spell->EffectApplyAuraName[0] == SPELL_AURA_PERIODIC_HEAL &&
        !spell->Effect[1] && !spell->Effect[2] &&
        spell->IsPositiveEffect(EFFECT_INDEX_0) &&
        spell->SpellVisual != 163 &&
        !spell->HasAttribute(SPELL_ATTR_EX4_NO_HELPFUL_THREAT) &&
        !spell->HasAttribute(SPELL_ATTR_EX_NO_THREAT);
    if (!spellValid || seedDamageA != 1 || seedDamageB != 1 ||
        hostileReferenceCount != 2)
    {
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, healedTarget) && cleaned;
        cleaned = DestroySimulatorActor(map, healer) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic healing fixture preconditions failed (spellValid=%u seed=%u/%u references=%u cleanup=%u).",
                 spellValid ? 1u : 0u, seedDamageA, seedDamageB,
                 hostileReferenceCount, cleaned ? 1u : 0u);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic healing fixture: refs=%u; applying Rejuvenation.", hostileReferenceCount);
    SpellCastResult const castResult = healer.creature->CastSpell(
        healedTarget.creature, spell, true);
    Aura* aura = castResult == SPELL_CAST_OK
        ? healedTarget.creature->GetAura(spellId, EFFECT_INDEX_0) : nullptr;
    bool const auraIsOrdinary = aura && aura->IsPeriodic() &&
        aura->GetAuraScript() == nullptr && aura->GetAuraPeriodicTimer() > 0;
    if (!auraIsOrdinary)
    {
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, healedTarget) && cleaned;
        cleaned = DestroySimulatorActor(map, healer) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic healing aura did not apply as a simple Core aura (cast=%u aura=%u script=%u timer=%d cleanup=%u).",
                 static_cast<uint32>(castResult), aura ? 1u : 0u,
                 aura && aura->GetAuraScript() ? 1u : 0u,
                 aura ? aura->GetAuraPeriodicTimer() : -1, cleaned ? 1u : 0u);
        return false;
    }

    uint32 const healthBefore = healedTarget.creature->GetHealth();
    uint32 const ticksBefore = aura->GetAuraTicks();
    float const threatABefore = hostileA.creature->GetThreatManager().getThreat(healer.creature);
    float const threatBBefore = hostileB.creature->GetThreatManager().getThreat(healer.creature);

    // Drive the ordinary Core active-object/Aura scheduler until exactly one
    // real tick has completed. Resolve the Aura pointer again after each update.
    uint32 updates = 0;
    for (; updates < 64; ++updates)
    {
        map.Update(250);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        aura = healedTarget.creature->GetAura(spellId, EFFECT_INDEX_0);
        if (!aura || aura->GetAuraTicks() > ticksBefore)
            break;
    }

    uint32 const ticksAfter = aura ? aura->GetAuraTicks() : 0;
    uint32 const healthAfter = healedTarget.creature->GetHealth();
    uint32 const effectiveHealing = healthAfter > healthBefore
        ? healthAfter - healthBefore : 0;
    float const threatAAfter = hostileA.creature->GetThreatManager().getThreat(healer.creature);
    float const threatBAfter = hostileB.creature->GetThreatManager().getThreat(healer.creature);
    float const actualDeltaA = threatAAfter - threatABefore;
    float const actualDeltaB = threatBAfter - threatBBefore;

    ThreatImpactEstimate const estimate = EstimateAppliedPeriodicHealingAssistComponent(
        healer.creature, healedTarget.creature, effectiveHealing, spell, 88002);
    bool rowA = false, rowB = false;
    float estimatedA = -1.0f, estimatedB = -1.0f;
    for (ThreatImpactEntry const& impact : estimate.impacts)
    {
        if (impact.threatOwnerGuid != healer.creature->GetObjectGuid().GetRawValue() ||
            impact.source != ThreatImpactSource::PeriodicHealAssist ||
            impact.bound != ThreatImpactBound::Exact)
            continue;
        if (impact.hostileGuid == hostileA.creature->GetObjectGuid().GetRawValue())
        {
            rowA = true;
            estimatedA = impact.threatPoints;
        }
        else if (impact.hostileGuid == hostileB.creature->GetObjectGuid().GetRawValue())
        {
            rowB = true;
            estimatedB = impact.threatPoints;
        }
    }

    bool const estimateShape =
        estimate.completeness == ThreatImpactCompleteness::Unknown &&
        estimate.impacts.size() == 2 && rowA && rowB;
    bool const matched = ticksAfter == ticksBefore + 1 &&
        effectiveHealing > 0 && estimateShape &&
        std::fabs(actualDeltaA - estimatedA) < 0.01f &&
        std::fabs(actualDeltaB - estimatedB) < 0.01f &&
        std::fabs(actualDeltaA - actualDeltaB) < 0.01f;

    bool cleaned = DestroySimulatorActor(map, hostileB);
    cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
    cleaned = DestroySimulatorActor(map, healedTarget) && cleaned;
    cleaned = DestroySimulatorActor(map, healer) && cleaned;

    sLog.Out(LOG_BASIC, matched && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] Periodic healing tick: spell=%u ticks=%u>%u updates=%u effectiveHealing=%u refs=%u estimateShape=%u estimated=%.3f/%.3f actualDelta=%.3f/%.3f matched=%u cleanup=%u.",
             spellId, ticksBefore, ticksAfter, updates, effectiveHealing,
             hostileReferenceCount, estimateShape ? 1u : 0u, estimatedA,
             estimatedB, actualDeltaA, actualDeltaB, matched ? 1u : 0u,
             cleaned ? 1u : 0u);

    if (!matched || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core periodic-healing assist tick parity fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: a real Core periodic-healing Aura tick distributed half the effective healing across both live hostile references; the component matched each actual ThreatManager delta.");
    return true;
}


bool RunPeriodicPowerAssistThreatFixture(Map& map)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic power fixture: begin.");
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor energizedTarget;
    SimulatorActor hostileA;
    SimulatorActor hostileB;
    bool created =
        CreateSimulatorActor(map, *info, partyFaction, 2.0f, 48.0f, energizedTarget) &&
        CreateSimulatorActor(map, *info, hostileFaction, 4.0f, 48.0f, hostileA) &&
        CreateSimulatorActor(map, *info, hostileFaction, 6.0f, 48.0f, hostileB);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, energizedTarget) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic power fixture actor creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    energizedTarget.creature->SetLevel(60);
    hostileA.creature->SetLevel(60);
    hostileB.creature->SetLevel(60);

    // Both real hostile references are attached to the Unit that will cast
    // the helpful aura on itself; HostileRefManager::threatAssist therefore
    // distributes the tick's assist event to both hostile ThreatManagers.
    uint32 const seedDamageA = energizedTarget.creature->DealDamage(
        hostileA.creature, 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    uint32 const seedDamageB = energizedTarget.creature->DealDamage(
        hostileB.creature, 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    energizedTarget.creature->SetInCombatWith(hostileA.creature);
    energizedTarget.creature->SetInCombatWith(hostileB.creature);
    hostileA.creature->SetInCombatWith(energizedTarget.creature);
    hostileB.creature->SetInCombatWith(energizedTarget.creature);

    uint32 hostileReferenceCount = 0;
    for (HostileReference* reference = energizedTarget.creature->GetHostileRefManager().getFirst();
         reference; reference = reference->next())
        ++hostileReferenceCount;

    // The pinned 5875 DBC set contains mana-periodic auras but no ordinary
    // PERIODIC_ENERGIZE/ENERGY aura. Use a real SpellMgr-owned Rejuvenation
    // entry and temporarily override only its Aura type/resource metadata.
    // Spell::Spell's native-pointer invariant is preserved; an RAII guard
    // restores the loaded DBC fields after each cast/tick/cleanup.
    uint32 const spellId = 9841; // Rejuvenation rank 10, one ordinary helpful aura
    SpellEntry const* nativeSpell = sSpellMgr.GetSpellEntry(spellId);
    bool const spellValid = nativeSpell &&
        nativeSpell->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
        !nativeSpell->Effect[1] && !nativeSpell->Effect[2] &&
        nativeSpell->IsPositiveEffect(EFFECT_INDEX_0) &&
        !nativeSpell->ScriptId &&
        nativeSpell->SpellVisual != 163 &&
        !nativeSpell->HasAttribute(SPELL_ATTR_EX4_NO_HELPFUL_THREAT) &&
        !nativeSpell->HasAttribute(SPELL_ATTR_EX_NO_THREAT);

    if (!spellValid || seedDamageA != 1 || seedDamageB != 1 ||
        hostileReferenceCount != 2)
    {
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, energizedTarget) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic power fixture preconditions failed (spellValid=%u seed=%u/%u references=%u cleanup=%u).",
                 spellValid ? 1u : 0u, seedDamageA, seedDamageB,
                 hostileReferenceCount, cleaned ? 1u : 0u);
        return false;
    }

    struct ScopedAuraEffectOverride
    {
        SpellEntry* spell;
        uint32 originalAuraType;
        int32 originalMiscValue;

        ScopedAuraEffectOverride(SpellEntry* entry, uint32 auraType, int32 miscValue)
            : spell(entry),
              originalAuraType(entry->EffectApplyAuraName[0]),
              originalMiscValue(entry->EffectMiscValue[0])
        {
            spell->EffectApplyAuraName[0] = auraType;
            spell->EffectMiscValue[0] = miscValue;
        }

        ~ScopedAuraEffectOverride()
        {
            spell->EffectApplyAuraName[0] = originalAuraType;
            spell->EffectMiscValue[0] = originalMiscValue;
        }

        ScopedAuraEffectOverride(ScopedAuraEffectOverride const&) = delete;
        ScopedAuraEffectOverride& operator=(ScopedAuraEffectOverride const&) = delete;
    };

    struct PeriodicPowerVariantResult
    {
        bool matched = false;
        uint32 spellId = 0;
        uint32 effectiveGain = 0;
        float estimatedA = -1.0f;
        float estimatedB = -1.0f;
        float actualA = 0.0f;
        float actualB = 0.0f;
    };

    auto runVariant = [&](char const* label, uint32 auraType,
                          Powers powerType, uint32 maxPower,
                          bool expectThreat) -> PeriodicPowerVariantResult
    {
        PeriodicPowerVariantResult result;
        energizedTarget.creature->SetPowerType(powerType);
        energizedTarget.creature->SetMaxPower(powerType, maxPower);
        energizedTarget.creature->SetPower(powerType, 0);

        SpellEntry* mutableNativeSpell = const_cast<SpellEntry*>(nativeSpell);
        ScopedAuraEffectOverride auraOverride(
            mutableNativeSpell, auraType,
            auraType == SPELL_AURA_PERIODIC_ENERGIZE
                ? static_cast<int32>(powerType) : static_cast<int32>(POWER_MANA));

        // The SpellEntry pointer remains the exact object owned by SpellMgr.
        // Only its two effect metadata fields are overridden for this bounded
        // isolated scenario; the Aura and ThreatManager are normal Core code.
        SpellEntry const* selectedSpell = sSpellMgr.GetSpellEntry(spellId);
        SpellCastResult const castResult = energizedTarget.creature->CastSpell(
            energizedTarget.creature, selectedSpell, true);
        Aura* aura = castResult == SPELL_CAST_OK
            ? energizedTarget.creature->GetAura(spellId, EFFECT_INDEX_0) : nullptr;
        bool const auraIsOrdinary = aura && aura->IsPeriodic() &&
            aura->GetAuraScript() == nullptr && aura->GetAuraPeriodicTimer() > 0;
        if (!auraIsOrdinary)
        {
            energizedTarget.creature->RemoveAurasDueToSpell(spellId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[PartyBotSimulator] Periodic power variant %s failed to apply an ordinary Core Aura (cast=%u aura=%u script=%u timer=%d).",
                     label, static_cast<uint32>(castResult), aura ? 1u : 0u,
                     aura && aura->GetAuraScript() ? 1u : 0u,
                     aura ? aura->GetAuraPeriodicTimer() : -1);
            return result;
        }

        result.spellId = spellId;
        uint32 const powerBefore = energizedTarget.creature->GetPower(powerType);
        uint32 const ticksBefore = aura->GetAuraTicks();
        float const threatABefore =
            hostileA.creature->GetThreatManager().getThreat(energizedTarget.creature);
        float const threatBBefore =
            hostileB.creature->GetThreatManager().getThreat(energizedTarget.creature);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] Periodic power variant %s: real SpellMgr spell %u applied; awaiting Core Aura tick.",
                 label, spellId);

        uint32 updates = 0;
        for (; updates < 64; ++updates)
        {
            map.Update(250);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            aura = energizedTarget.creature->GetAura(spellId, EFFECT_INDEX_0);
            if (!aura || aura->GetAuraTicks() > ticksBefore)
                break;
        }

        uint32 const ticksAfter = aura ? aura->GetAuraTicks() : 0;
        uint32 const powerAfter = energizedTarget.creature->GetPower(powerType);
        result.effectiveGain = powerAfter > powerBefore ? powerAfter - powerBefore : 0;
        result.actualA =
            hostileA.creature->GetThreatManager().getThreat(energizedTarget.creature) - threatABefore;
        result.actualB =
            hostileB.creature->GetThreatManager().getThreat(energizedTarget.creature) - threatBBefore;

        ThreatImpactEstimate const estimate = EstimateAppliedPeriodicPowerAssistComponent(
            energizedTarget.creature, energizedTarget.creature, result.effectiveGain,
            powerType, selectedSpell, EFFECT_INDEX_0, 88003);
        bool rowA = false, rowB = false;
        for (ThreatImpactEntry const& impact : estimate.impacts)
        {
            if (impact.threatOwnerGuid != energizedTarget.creature->GetObjectGuid().GetRawValue() ||
                impact.source != ThreatImpactSource::PeriodicPowerAssist ||
                impact.bound != ThreatImpactBound::Exact)
                continue;
            if (impact.hostileGuid == hostileA.creature->GetObjectGuid().GetRawValue())
            {
                rowA = true;
                result.estimatedA = impact.threatPoints;
            }
            else if (impact.hostileGuid == hostileB.creature->GetObjectGuid().GetRawValue())
            {
                rowB = true;
                result.estimatedB = impact.threatPoints;
            }
        }

        bool estimateValid = false;
        if (expectThreat)
        {
            estimateValid = estimate.completeness == ThreatImpactCompleteness::Unknown &&
                estimate.impacts.size() == 2 && rowA && rowB;
            result.matched = ticksAfter == ticksBefore + 1 &&
                result.effectiveGain > 0 && estimateValid &&
                std::fabs(result.actualA - result.estimatedA) < 0.01f &&
                std::fabs(result.actualB - result.estimatedB) < 0.01f &&
                std::fabs(result.actualA - result.actualB) < 0.01f;
        }
        else
        {
            estimateValid = estimate.completeness == ThreatImpactCompleteness::NoThreat &&
                estimate.impacts.empty();
            result.matched = ticksAfter == ticksBefore + 1 &&
                result.effectiveGain > 0 && estimateValid &&
                std::fabs(result.actualA) < 0.01f &&
                std::fabs(result.actualB) < 0.01f;
        }

        sLog.Out(LOG_BASIC, result.matched ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic power variant: name=%s spell=%u auraType=%u power=%u ticks=%u>%u updates=%u gain=%u expectedThreat=%u estimateValid=%u estimated=%.3f/%.3f actualDelta=%.3f/%.3f matched=%u.",
                 label, result.spellId, auraType, static_cast<uint32>(powerType),
                 ticksBefore, ticksAfter, updates, result.effectiveGain,
                 expectThreat ? 1u : 0u, estimateValid ? 1u : 0u,
                 result.estimatedA, result.estimatedB, result.actualA,
                 result.actualB, result.matched ? 1u : 0u);

        energizedTarget.creature->RemoveAurasDueToSpell(spellId);
        return result;
    };

    PeriodicPowerVariantResult const energy = runVariant(
        "periodic-energy", SPELL_AURA_PERIODIC_ENERGIZE, POWER_ENERGY, 100, true);
    PeriodicPowerVariantResult const manaEnergize = runVariant(
        "periodic-mana-exempt", SPELL_AURA_PERIODIC_ENERGIZE, POWER_MANA, 1000, false);
    PeriodicPowerVariantResult const observedMana = runVariant(
        "observed-mana", SPELL_AURA_OBS_MOD_MANA, POWER_MANA, 1000, true);

    bool const matched = energy.matched && manaEnergize.matched && observedMana.matched;
    bool cleaned = DestroySimulatorActor(map, hostileB);
    cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
    cleaned = DestroySimulatorActor(map, energizedTarget) && cleaned;

    sLog.Out(LOG_BASIC, matched && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] Periodic power summary: energyGain=%u actual=%.3f/%.3f manaEnergizeGain=%u actual=%.3f/%.3f observedManaGain=%u actual=%.3f/%.3f matched=%u cleanup=%u.",
             energy.effectiveGain, energy.actualA, energy.actualB,
             manaEnergize.effectiveGain, manaEnergize.actualA, manaEnergize.actualB,
             observedMana.effectiveGain, observedMana.actualA, observedMana.actualB,
             matched ? 1u : 0u, cleaned ? 1u : 0u);

    if (!matched || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core periodic-power assist fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Core PERIODIC_ENERGIZE granted assist threat for energy but not mana, while OBS_MOD_MANA did grant mana assist threat; the component matched each real two-hostile ThreatManager result.");
    return true;
}


bool RunPeriodicManaLeechThreatFixture(Map& map)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic mana leech fixture: begin.");
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor caster;
    SimulatorActor hostile;
    bool created =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 60.0f, caster) &&
        CreateSimulatorActor(map, *info, hostileFaction, 2.0f, 60.0f, hostile);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, hostile);
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic mana leech fixture actor creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    caster.creature->SetLevel(60);
    hostile.creature->SetLevel(60);
    caster.creature->SetPowerType(POWER_MANA);
    caster.creature->SetMaxPower(POWER_MANA, 1000);
    caster.creature->SetPower(POWER_MANA, 0);
    hostile.creature->SetPowerType(POWER_MANA);
    hostile.creature->SetMaxPower(POWER_MANA, 1000);
    hostile.creature->SetPower(POWER_MANA, 1000);
    hostile.creature->SetInCombatWith(caster.creature);
    caster.creature->SetInCombatWith(hostile.creature);

    // Select a real native 5875 single-effect periodic mana-leech aura. Avoid
    // Mark of Kazzak (its depletion explosion is a separate action) and any
    // scripted/suppressed spell so this fixture isolates the direct mana-gain
    // threat row only.
    std::vector<SpellEntry const*> candidates;
    uint32 const maxSpellId = sSpellMgr.GetMaxSpellId();
    for (uint32 spellId = 1; spellId < maxSpellId; ++spellId)
    {
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
        if (!spell || spell->Id == 21056 ||
            spell->Effect[0] != SPELL_EFFECT_APPLY_AURA ||
            spell->EffectApplyAuraName[0] != SPELL_AURA_PERIODIC_MANA_LEECH ||
            spell->EffectMiscValue[0] != POWER_MANA ||
            spell->Effect[1] || spell->Effect[2] || spell->ScriptId ||
            spell->HasAttribute(SPELL_ATTR_EX4_NO_HARMFUL_THREAT) ||
            spell->HasAttribute(SPELL_ATTR_EX_NO_THREAT))
            continue;
        candidates.push_back(spell);
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic mana leech fixture: native candidates=%u.",
             static_cast<uint32>(candidates.size()));

    SpellEntry const* selectedSpell = nullptr;
    Aura* aura = nullptr;
    SpellCastResult castResult = SPELL_FAILED_SPELL_UNAVAILABLE;
    for (SpellEntry const* candidate : candidates)
    {
        // Preserve Core's exact SpellMgr-owned pointer invariant.
        castResult = caster.creature->CastSpell(hostile.creature, candidate, true);
        aura = castResult == SPELL_CAST_OK
            ? hostile.creature->GetAura(candidate->Id, EFFECT_INDEX_0) : nullptr;
        Modifier const* modifier = aura ? aura->GetModifier() : nullptr;
        bool const hasEffectiveDrain = modifier && modifier->m_amount > 0.0f;
        bool const transfersMana = candidate->EffectMultipleValue[0] > 0.0f;
        bool const ordinaryManaLeech = aura && aura->IsPeriodic() &&
            !aura->GetAuraScript() && aura->GetAuraPeriodicTimer() > 0 &&
            hasEffectiveDrain && transfersMana;
        if (ordinaryManaLeech)
        {
            selectedSpell = candidate;
            break;
        }

        // Some 5875 rows use the PERIODIC_MANA_LEECH aura type for a zero
        // amount or a zero transfer multiplier. They apply and tick normally,
        // but cannot drain or produce threat. Do not let one masquerade as the
        // representative mana-leech fixture.
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
                 "[PartyBotSimulator] Periodic mana leech candidate skipped: spell=%u cast=%u aura=%u amount=%.3f multiplier=%.3f timer=%d script=%u.",
                 candidate->Id, static_cast<uint32>(castResult), aura ? 1u : 0u,
                 modifier ? modifier->m_amount : 0.0f,
                 candidate->EffectMultipleValue[0],
                 aura ? aura->GetAuraPeriodicTimer() : -1,
                 aura && aura->GetAuraScript() ? 1u : 0u);
        hostile.creature->RemoveAurasDueToSpell(candidate->Id);
    }

    bool const auraFound = aura != nullptr;
    bool const hostileCanHaveThreatList =
        hostile.creature && hostile.creature->CanHaveThreatList();
    bool const improvedDrainMana1 =
        caster.creature && caster.creature->HasAura(17864, EFFECT_INDEX_0);
    bool const improvedDrainMana2 =
        caster.creature && caster.creature->HasAura(18393, EFFECT_INDEX_0);
    if (!selectedSpell || !aura || !hostileCanHaveThreatList ||
        improvedDrainMana1 || improvedDrainMana2)
    {
        bool cleaned = DestroySimulatorActor(map, hostile);
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic mana leech fixture preconditions failed (cast=%u aura=%u candidates=%u threatList=%u improvedA=%u improvedB=%u cleanup=%u).",
                 static_cast<uint32>(castResult), auraFound ? 1u : 0u,
                 static_cast<uint32>(candidates.size()),
                 hostileCanHaveThreatList ? 1u : 0u,
                 improvedDrainMana1 ? 1u : 0u,
                 improvedDrainMana2 ? 1u : 0u, cleaned ? 1u : 0u);
        return false;
    }

    uint32 const targetPowerBefore = hostile.creature->GetPower(POWER_MANA);
    uint32 const casterPowerBefore = caster.creature->GetPower(POWER_MANA);
    uint32 const ticksBefore = aura->GetAuraTicks();
    uint32 const targetHealthBefore = hostile.creature->GetHealth();
    float const threatBefore =
        hostile.creature->GetThreatManager().getThreat(caster.creature);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic mana leech fixture: selected native spell %u; awaiting one Core Aura tick.",
             selectedSpell->Id);

    uint32 updates = 0;
    for (; updates < 64; ++updates)
    {
        map.Update(250);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        aura = hostile.creature->GetAura(selectedSpell->Id, EFFECT_INDEX_0);
        if (!aura || aura->GetAuraTicks() > ticksBefore)
            break;
    }

    uint32 const ticksAfter = aura ? aura->GetAuraTicks() : 0;
    uint32 const targetPowerAfter = hostile.creature->GetPower(POWER_MANA);
    uint32 const casterPowerAfter = caster.creature->GetPower(POWER_MANA);
    uint32 const drainedPower = targetPowerBefore > targetPowerAfter
        ? targetPowerBefore - targetPowerAfter : 0;
    uint32 const effectiveCasterGain = casterPowerAfter > casterPowerBefore
        ? casterPowerAfter - casterPowerBefore : 0;
    uint32 const appliedHealthDamage = targetHealthBefore > hostile.creature->GetHealth()
        ? targetHealthBefore - hostile.creature->GetHealth() : 0;
    float const actualThreatDelta =
        hostile.creature->GetThreatManager().getThreat(caster.creature) - threatBefore;

    ThreatImpactEstimate const estimate = EstimateAppliedPeriodicManaLeechComponent(
        caster.creature, hostile.creature, effectiveCasterGain, POWER_MANA,
        selectedSpell, EFFECT_INDEX_0, 88004);
    bool const estimateShape =
        estimate.completeness == ThreatImpactCompleteness::Unknown &&
        estimate.impacts.size() == 1 &&
        estimate.impacts[0].threatOwnerGuid == caster.creature->GetObjectGuid().GetRawValue() &&
        estimate.impacts[0].hostileGuid == hostile.creature->GetObjectGuid().GetRawValue() &&
        estimate.impacts[0].source == ThreatImpactSource::PeriodicManaLeech &&
        estimate.impacts[0].bound == ThreatImpactBound::Exact;
    float const estimatedThreat = estimateShape ? estimate.impacts[0].threatPoints : -1.0f;
    bool const matched = ticksAfter == ticksBefore + 1 &&
        drainedPower > 0 && effectiveCasterGain > 0 && estimateShape &&
        std::fabs(actualThreatDelta - estimatedThreat) < 0.01f;

    bool cleaned = true;
    if (hostile.creature->IsInWorld())
        hostile.creature->RemoveAurasDueToSpell(selectedSpell->Id);
    cleaned = DestroySimulatorActor(map, hostile);
    cleaned = DestroySimulatorActor(map, caster) && cleaned;

    sLog.Out(LOG_BASIC, matched && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] Periodic mana leech tick: spell=%u ticks=%u>%u updates=%u drained=%u casterGain=%u extraDamage=%u estimated=%.3f actualDelta=%.3f estimateShape=%u matched=%u cleanup=%u.",
             selectedSpell->Id, ticksBefore, ticksAfter, updates, drainedPower,
             effectiveCasterGain, appliedHealthDamage, estimatedThreat,
             actualThreatDelta, estimateShape ? 1u : 0u, matched ? 1u : 0u,
             cleaned ? 1u : 0u);

    if (!matched || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core periodic mana leech threat fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: a real Core periodic mana-leech tick drained target Mana, credited the caster's effective Mana gain and matched the target ThreatManager delta with the direct 50-percent assist component.");
    return true;
}


bool RunPeriodicLeechThreatFixture(Map& map)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic life leech fixture: begin.");
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor caster;
    SimulatorActor hostileA;
    SimulatorActor hostileB;
    bool created =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 72.0f, caster) &&
        CreateSimulatorActor(map, *info, hostileFaction, 2.0f, 72.0f, hostileA) &&
        CreateSimulatorActor(map, *info, hostileFaction, 4.0f, 72.0f, hostileB);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic life leech fixture actor creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    caster.creature->SetLevel(60);
    hostileA.creature->SetLevel(60);
    hostileB.creature->SetLevel(60);
    caster.creature->SetHealth(50000);

    // Put two real hostile references on the self-healing caster. The damage
    // portion of the same tick hits hostile A, while the heal-assist portion
    // must fan out across both A and B.
    uint32 const seedDamageA = caster.creature->DealDamage(
        hostileA.creature, 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    uint32 const seedDamageB = caster.creature->DealDamage(
        hostileB.creature, 1, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    caster.creature->SetInCombatWith(hostileA.creature);
    caster.creature->SetInCombatWith(hostileB.creature);
    hostileA.creature->SetInCombatWith(caster.creature);
    hostileB.creature->SetInCombatWith(caster.creature);

    uint32 hostileReferenceCount = 0;
    for (HostileReference* reference = caster.creature->GetHostileRefManager().getFirst();
         reference; reference = reference->next())
        ++hostileReferenceCount;

    std::vector<SpellEntry const*> candidates;
    uint32 const maxSpellId = sSpellMgr.GetMaxSpellId();
    for (uint32 spellId = 1; spellId < maxSpellId; ++spellId)
    {
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
        if (!spell || spell->Effect[0] != SPELL_EFFECT_APPLY_AURA ||
            spell->EffectApplyAuraName[0] != SPELL_AURA_PERIODIC_LEECH ||
            spell->Effect[1] || spell->Effect[2] || spell->ScriptId ||
            spell->SpellVisual == 163 ||
            spell->HasAttribute(SPELL_ATTR_EX4_NO_HARMFUL_THREAT) ||
            spell->HasAttribute(SPELL_ATTR_EX4_NO_HELPFUL_THREAT) ||
            spell->HasAttribute(SPELL_ATTR_EX_NO_THREAT))
            continue;
        candidates.push_back(spell);
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic life leech fixture: refs=%u native candidates=%u.",
             hostileReferenceCount, static_cast<uint32>(candidates.size()));

    SpellEntry const* selectedSpell = nullptr;
    Aura* aura = nullptr;
    SpellCastResult castResult = SPELL_FAILED_SPELL_UNAVAILABLE;
    for (SpellEntry const* candidate : candidates)
    {
        castResult = caster.creature->CastSpell(hostileA.creature, candidate, true);
        aura = castResult == SPELL_CAST_OK
            ? hostileA.creature->GetAura(candidate->Id, EFFECT_INDEX_0) : nullptr;
        if (aura && aura->IsPeriodic() && !aura->GetAuraScript() &&
            aura->GetAuraPeriodicTimer() > 0)
        {
            selectedSpell = candidate;
            break;
        }
        hostileA.creature->RemoveAurasDueToSpell(candidate->Id);
    }

    if (!selectedSpell || !aura || seedDamageA != 1 || seedDamageB != 1 ||
        hostileReferenceCount != 2)
    {
        bool const auraFound = aura != nullptr;
        bool cleaned = DestroySimulatorActor(map, hostileB);
        cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Periodic life leech fixture preconditions failed (cast=%u aura=%u seed=%u/%u refs=%u candidates=%u cleanup=%u).",
                 static_cast<uint32>(castResult), auraFound ? 1u : 0u,
                 seedDamageA, seedDamageB, hostileReferenceCount,
                 static_cast<uint32>(candidates.size()), cleaned ? 1u : 0u);
        return false;
    }

    uint32 const casterHealthBefore = caster.creature->GetHealth();
    uint32 const hostileHealthBefore = hostileA.creature->GetHealth();
    uint32 const ticksBefore = aura->GetAuraTicks();
    float const threatABefore =
        hostileA.creature->GetThreatManager().getThreat(caster.creature);
    float const threatBBefore =
        hostileB.creature->GetThreatManager().getThreat(caster.creature);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Periodic life leech fixture: selected native spell %u; awaiting Core Aura tick.",
             selectedSpell->Id);

    uint32 updates = 0;
    for (; updates < 64; ++updates)
    {
        map.Update(250);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        aura = hostileA.creature->GetAura(selectedSpell->Id, EFFECT_INDEX_0);
        if (!aura || aura->GetAuraTicks() > ticksBefore)
            break;
    }

    uint32 const ticksAfter = aura ? aura->GetAuraTicks() : 0;
    uint32 const casterHealthAfter = caster.creature->GetHealth();
    uint32 const hostileHealthAfter = hostileA.creature->GetHealth();
    uint32 const effectiveHealing = casterHealthAfter > casterHealthBefore
        ? casterHealthAfter - casterHealthBefore : 0;
    uint32 const appliedDamage = hostileHealthBefore > hostileHealthAfter
        ? hostileHealthBefore - hostileHealthAfter : 0;
    float const actualDeltaA =
        hostileA.creature->GetThreatManager().getThreat(caster.creature) - threatABefore;
    float const actualDeltaB =
        hostileB.creature->GetThreatManager().getThreat(caster.creature) - threatBBefore;

    ThreatImpactEstimate const damageEstimate = EstimateAppliedPeriodicDamageComponent(
        caster.creature, hostileA.creature, appliedDamage, selectedSpell, 88005);
    ThreatImpactEstimate const healEstimate = EstimateAppliedPeriodicHealingAssistComponent(
        caster.creature, caster.creature, effectiveHealing, selectedSpell, 88006);

    bool damageShape =
        damageEstimate.completeness == ThreatImpactCompleteness::Unknown &&
        damageEstimate.impacts.size() == 1 &&
        damageEstimate.impacts[0].threatOwnerGuid == caster.creature->GetObjectGuid().GetRawValue() &&
        damageEstimate.impacts[0].hostileGuid == hostileA.creature->GetObjectGuid().GetRawValue() &&
        damageEstimate.impacts[0].source == ThreatImpactSource::PeriodicDamage &&
        damageEstimate.impacts[0].bound == ThreatImpactBound::Exact;
    bool healRowA = false, healRowB = false;
    float estimatedDamageA = damageShape ? damageEstimate.impacts[0].threatPoints : -1.0f;
    float estimatedHealA = -1.0f, estimatedHealB = -1.0f;
    for (ThreatImpactEntry const& impact : healEstimate.impacts)
    {
        if (impact.threatOwnerGuid != caster.creature->GetObjectGuid().GetRawValue() ||
            impact.source != ThreatImpactSource::PeriodicHealAssist ||
            impact.bound != ThreatImpactBound::Exact)
            continue;
        if (impact.hostileGuid == hostileA.creature->GetObjectGuid().GetRawValue())
        {
            healRowA = true;
            estimatedHealA = impact.threatPoints;
        }
        else if (impact.hostileGuid == hostileB.creature->GetObjectGuid().GetRawValue())
        {
            healRowB = true;
            estimatedHealB = impact.threatPoints;
        }
    }

    bool const healShape =
        healEstimate.completeness == ThreatImpactCompleteness::Unknown &&
        healEstimate.impacts.size() == 2 && healRowA && healRowB;
    bool const matched = ticksAfter == ticksBefore + 1 &&
        appliedDamage > 0 && effectiveHealing > 0 &&
        damageShape && healShape &&
        std::fabs(actualDeltaA - (estimatedDamageA + estimatedHealA)) < 0.01f &&
        std::fabs(actualDeltaB - estimatedHealB) < 0.01f;

    bool cleaned = true;
    if (hostileA.creature->IsInWorld())
        hostileA.creature->RemoveAurasDueToSpell(selectedSpell->Id);
    cleaned = DestroySimulatorActor(map, hostileB);
    cleaned = DestroySimulatorActor(map, hostileA) && cleaned;
    cleaned = DestroySimulatorActor(map, caster) && cleaned;

    sLog.Out(LOG_BASIC, matched && cleaned ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] Periodic life leech tick: spell=%u ticks=%u>%u updates=%u appliedDamage=%u effectiveHealing=%u damageEstimate=%.3f healAssistEstimate=%.3f/%.3f actualDelta=%.3f/%.3f damageShape=%u healShape=%u matched=%u cleanup=%u.",
             selectedSpell->Id, ticksBefore, ticksAfter, updates, appliedDamage,
             effectiveHealing, estimatedDamageA, estimatedHealA, estimatedHealB,
             actualDeltaA, actualDeltaB, damageShape ? 1u : 0u,
             healShape ? 1u : 0u, matched ? 1u : 0u, cleaned ? 1u : 0u);

    if (!matched || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core periodic life-leech threat fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: a real Core periodic life-leech tick matched both the hostile damage threat and the self-heal assist fan-out across two hostile ThreatManagers.");
    return true;
}

bool RunNegativeFlatSpellThreatFixture(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    SimulatorActor caster;
    SimulatorActor hostile;
    bool const created =
        CreateSimulatorActor(map, *info, partyFaction, 0.0f, 20.0f, caster) &&
        CreateSimulatorActor(map, *info, hostileFaction, 2.0f, 20.0f, hostile);
    if (!created)
    {
        bool cleaned = DestroySimulatorActor(map, hostile);
        cleaned = DestroySimulatorActor(map, caster) && cleaned;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Negative flat spell-threat fixture actor creation failed (cleanup=%u).",
                 cleaned ? 1u : 0u);
        return false;
    }

    caster.creature->SetLevel(60);
    hostile.creature->SetLevel(60);
    for (uint32 tick = 0; tick < 120 &&
         (!caster.updateCount || !hostile.updateCount); ++tick)
    {
        map.Update(50);
        if (!caster.updateCount || !hostile.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    bool matched = false;
    bool mismatch = false;
    bool failClosedChecksPassed = false;
    uint32 selectedSpellId = 0;
    uint32 attemptedSpells = 0;
    float estimatedThreat = 0.0f;
    float actualThreatDelta = 0.0f;
    uint32 const healthBefore = hostile.creature->GetHealth();

    // Pick a real database-backed harmful, single-effect aura spell with a
    // flat spell_threat row. Aura-only effects avoid mixing direct-damage
    // threat into the flat-threat parity assertion.
    for (uint32 spellId = 1; spellId < 50000 && !matched && !mismatch; ++spellId)
    {
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
        SpellThreatEntry const* threatEntry = sSpellMgr.GetSpellThreatEntry(spellId);
        if (!spell || !threatEntry || !threatEntry->threat ||
            spell->Effect[0] != SPELL_EFFECT_APPLY_AURA ||
            !spell->EffectApplyAuraName[0] ||
            spell->Effect[1] || spell->Effect[2] ||
            spell->EffectImplicitTargetA[0] == TARGET_UNIT_CASTER ||
            spell->IsPositiveEffect(SpellEffectIndex(0)) ||
            spell->HasAttribute(SPELL_ATTR_EX4_NO_HARMFUL_THREAT) ||
            spell->HasAttribute(SPELL_ATTR_EX_NO_THREAT) ||
            !threatEntry->CanCauseThreatOnMask(0x01))
            continue;

        std::vector<NegativeFlatSpellThreatTarget> successfulTargets;
        successfulTargets.push_back({hostile.creature, 0x01});
        ThreatImpactEstimate const estimate = EstimateNegativeFlatSpellThreatComponent(
            caster.creature, spell, threatEntry, successfulTargets, true, 77001);
        if (estimate.completeness != ThreatImpactCompleteness::Unknown ||
            estimate.impacts.size() != 1 ||
            estimate.impacts[0].threatOwnerGuid != caster.creature->GetObjectGuid().GetRawValue() ||
            estimate.impacts[0].hostileGuid != hostile.creature->GetObjectGuid().GetRawValue() ||
            estimate.impacts[0].source != ThreatImpactSource::FlatSpellThreat ||
            estimate.impacts[0].bound != ThreatImpactBound::Exact)
            continue;

        // Fail-closed contract tests use the same loaded spell/target as the
        // parity case. Invalid or incomplete caller facts must never produce
        // partial impact rows that a caller could mistake for a complete set.
        auto isUnknownWithoutRows = [](ThreatImpactEstimate const& value)
        {
            return value.completeness == ThreatImpactCompleteness::Unknown &&
                   value.impacts.empty();
        };
        std::vector<NegativeFlatSpellThreatTarget> incompleteTargets;
        incompleteTargets.push_back({hostile.creature, 0x01});
        bool const incompleteRejected = isUnknownWithoutRows(
            EstimateNegativeFlatSpellThreatComponent(
                caster.creature, spell, threatEntry, incompleteTargets, false, 77001));

        std::vector<NegativeFlatSpellThreatTarget> invalidMaskTargets;
        invalidMaskTargets.push_back({hostile.creature, 0x80});
        bool const invalidMaskRejected = isUnknownWithoutRows(
            EstimateNegativeFlatSpellThreatComponent(
                caster.creature, spell, threatEntry, invalidMaskTargets, true, 77001));

        std::vector<NegativeFlatSpellThreatTarget> duplicateTargets;
        duplicateTargets.push_back({hostile.creature, 0x01});
        duplicateTargets.push_back({hostile.creature, 0x01});
        bool const duplicateRejected = isUnknownWithoutRows(
            EstimateNegativeFlatSpellThreatComponent(
                caster.creature, spell, threatEntry, duplicateTargets, true, 77001));

        std::vector<NegativeFlatSpellThreatTarget> unresolvedTargets;
        unresolvedTargets.push_back({nullptr, 0x01});
        bool const unresolvedRejected = isUnknownWithoutRows(
            EstimateNegativeFlatSpellThreatComponent(
                caster.creature, spell, threatEntry, unresolvedTargets, true, 77001));

        SpellEntry mixedSignSpell = *spell;
        mixedSignSpell.Effect[1] = SPELL_EFFECT_HEAL;
        mixedSignSpell.EffectImplicitTargetA[1] = TARGET_UNIT_CASTER;
        std::vector<NegativeFlatSpellThreatTarget> mixedSignTargets;
        mixedSignTargets.push_back({hostile.creature, 0x03});
        ThreatImpactEstimate const mixedSignEstimate =
            EstimateNegativeFlatSpellThreatComponent(
                caster.creature, &mixedSignSpell, threatEntry,
                mixedSignTargets, true, 77001);
        bool const mixedSignRejected =
            mixedSignEstimate.completeness == ThreatImpactCompleteness::NoThreat &&
            mixedSignEstimate.impacts.empty();

        failClosedChecksPassed = incompleteRejected && invalidMaskRejected &&
            duplicateRejected && unresolvedRejected && mixedSignRejected;
        if (!failClosedChecksPassed)
        {
            mismatch = true;
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[PartyBotSimulator] Negative flat spell-threat fail-closed checks failed (incomplete=%u invalidMask=%u duplicate=%u unresolved=%u mixedSign=%u).",
                     incompleteRejected ? 1u : 0u, invalidMaskRejected ? 1u : 0u,
                     duplicateRejected ? 1u : 0u, unresolvedRejected ? 1u : 0u,
                     mixedSignRejected ? 1u : 0u);
            break;
        }

        ++attemptedSpells;
        float const before = hostile.creature->GetThreatManager().getThreat(caster.creature);
        SpellCastResult const castResult = caster.creature->CastSpell(
            hostile.creature, spell, true);
        if (castResult != SPELL_CAST_OK)
            continue;

        map.Update(50);
        float const after = hostile.creature->GetThreatManager().getThreat(caster.creature);
        float const delta = after - before;
        if (delta <= 0.0f)
            continue;

        selectedSpellId = spellId;
        estimatedThreat = estimate.impacts[0].threatPoints;
        actualThreatDelta = delta;
        matched = std::fabs(delta - estimatedThreat) < 0.01f &&
                  hostile.creature->GetHealth() == healthBefore;
        mismatch = !matched;
    }

    bool cleaned = DestroySimulatorActor(map, hostile);
    cleaned = DestroySimulatorActor(map, caster) && cleaned;
    bool const passed = matched && failClosedChecksPassed && !mismatch && cleaned;
    sLog.Out(LOG_BASIC, passed ? LOG_LVL_MINIMAL : LOG_LVL_ERROR,
             "[PartyBotSimulator] Negative flat spell threat: spell=%u attempts=%u estimated=%.3f actualDelta=%.3f matched=%u cleanup=%u.",
             selectedSpellId, attemptedSpells, estimatedThreat, actualThreatDelta,
             matched ? 1u : 0u, cleaned ? 1u : 0u);
    if (!passed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Negative flat spell-threat Core cast parity fixture failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: negative flat spell-threat estimator rejected incomplete target sets, invalid masks, duplicate/unresolved targets and mixed-sign spells without emitting impacts.");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: a real harmful Core aura spell routed negative flat spell_threat to its affected hostile and matched the estimator against the actual ThreatManager delta.");
    return true;
}

} // namespace

bool RunCoreMapLifecycleSmokeTest(uint32 mapId)
{
    MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(mapId);
    if (!entry || !entry->IsDungeon())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Map %u is not a valid dungeon/raid MapEntry; refusing to create a test map.",
                 mapId);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Creating isolated test map %u.", mapId);

    Map* map = sMapMgr.CreateTestMap(mapId, true, 0.0f, 0.0f);
    if (!map)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] MapManager::CreateTestMap(%u) returned null.", mapId);
        return false;
    }

    uint32 const instanceId = map->GetInstanceId();
    bool const registered = instanceId != 0 && sMapMgr.FindMap(mapId, instanceId) == map;

    // Keep Stage A limited to map registration/registry cleanup. An empty
    // Map::Update can execute instance-script work before simulator actors
    // exist; the first map tick belongs to Stage B, after actors have completed
    // the normal Creature::AIM_Initialize lifecycle above.
    bool const mapRetained = sMapMgr.FindMap(mapId, instanceId) == map;

    // Run every stage independently so one failure does not suppress the
    // remaining diagnostics; the final result still requires every stage.
    bool const creatureCombatPassed = RunCoreCombatThreatSmokeTest(*map);
    bool const negativeFlatSpellThreatPassed = RunNegativeFlatSpellThreatFixture(*map);
    bool const periodicDamagePassed = RunPeriodicDamageThreatFixture(*map);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Stage completed: periodic damage=%u; starting periodic healing.",
             periodicDamagePassed ? 1u : 0u);
    bool const periodicHealingPassed = RunPeriodicHealingAssistThreatFixture(*map);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Stage completed: periodic healing=%u; starting periodic power.",
             periodicHealingPassed ? 1u : 0u);
    bool const periodicPowerPassed = RunPeriodicPowerAssistThreatFixture(*map);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Stage completed: periodic power=%u; starting periodic mana leech.",
             periodicPowerPassed ? 1u : 0u);
    bool const periodicManaLeechPassed = RunPeriodicManaLeechThreatFixture(*map);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Stage completed: periodic mana leech=%u.",
             periodicManaLeechPassed ? 1u : 0u);
    bool const periodicLifeLeechPassed = RunPeriodicLeechThreatFixture(*map);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Stage completed: periodic life leech=%u.",
             periodicLifeLeechPassed ? 1u : 0u);
    bool const engineExecutorPassed = RunEngineCoreExecutorSmokeTest(*map);
    bool const creatureHealPassed = RunEngineCoreSpellHealSmokeTest(*map);
    bool const playerHealPassed = RunEngineCorePlayerSpellHealSmokeTest(*map);
    bool const cancelOwnCastPassed = RunEngineCoreCancelOwnCastActionSmokeTest(*map);
    bool const selectHostileTargetPassed = RunEngineCoreSelectHostileTargetSmokeTest(*map);
    bool const chaseTargetPassed = RunEngineCoreChaseTargetSmokeTest(*map);
    bool const moveDistancePassed = RunEngineCoreMoveDistanceSmokeTest(*map);
    bool const stopStaleChasePassed = RunEngineCoreStopStaleChaseActionSmokeTest(*map);
    bool const stopChasePassed = RunEngineCoreStopChaseSmokeTest(*map);
    bool const dpsAssistPassed = RunEngineCoreDpsAssistSmokeTest(*map);
    bool const dpsAssistGroupSnapshotPassed = RunEngineCoreDpsAssistGroupSnapshotSmokeTest(*map);
    bool const followPartyMemberPassed = RunEngineCoreFollowPartyMemberActionSmokeTest(*map);
    bool const fiveBotFormationPassed = RunEngineCoreFiveBotFormationSmokeTest(*map);
    bool const itemUsePassed = RunEngineCoreItemUseSmokeTest(*map);
    bool const countBasedItemMutationPassed =
        RunEngineCoreCountBasedItemMutationSmokeTest(*map);
    bool const utilitySpellAuditPassed =
        RunEngineCoreUtilitySpellAuditSmokeTest(*map);
    bool const itemUtilityAuditPassed =
        RunEngineCoreItemUtilityAuditSmokeTest();
    bool const itemUtilityUsePassed =
        RunEngineCoreItemUtilityUseSmokeTest(*map);
    bool const itemUtilityActionPassed =
        RunEngineCoreItemUtilityActionSmokeTest(*map);
    bool const movementPassed = RunEngineCoreMovementSmokeTest(*map);
    bool const followPassed = RunEngineCoreFollowSmokeTest(*map);
    bool const playerFollowCastSafetyPassed = RunEngineCorePlayerFollowCastSafetySmokeTest(*map);
    bool const followRetargetPassed = RunEngineCoreFollowRetargetSmokeTest(*map);
    bool const playerHealRevalidationPassed = RunEngineCorePlayerHealRevalidationSmokeTest(*map);
    bool const playerHealLosPassed = RunEngineCorePlayerHealLosSmokeTest(*map);
    bool const movePointControlSafetyPassed = RunEngineCoreMovePointControlSafetySmokeTest(*map);
    bool const movementInterruptPassed = RunEngineCoreMovementInterruptSmokeTest(*map);
    // Run the basic Group lifecycle fixture before the more demanding
    // multi-add encounter fixture. F9 is the final Player/Group scenario.
    // One five-Player Group fixture covers both the F8 roster/cache lifecycle
    // and F9 multi-add threat path; do not create two Groups on the same Map.
    bool const realGroupThreatSnapshotPassed = RunEngineCoreRealGroupThreatSnapshotSmokeTest(*map);
    bool const combatPassed = registered && mapRetained &&
                              creatureCombatPassed && negativeFlatSpellThreatPassed && periodicDamagePassed && periodicHealingPassed && periodicPowerPassed && periodicManaLeechPassed && periodicLifeLeechPassed && engineExecutorPassed &&
                              creatureHealPassed && playerHealPassed &&
                              cancelOwnCastPassed &&
                              selectHostileTargetPassed && chaseTargetPassed &&
                              moveDistancePassed && stopStaleChasePassed && stopChasePassed &&
                              dpsAssistPassed && dpsAssistGroupSnapshotPassed &&
                              followPartyMemberPassed && fiveBotFormationPassed && itemUsePassed &&
                              countBasedItemMutationPassed && utilitySpellAuditPassed &&
                              itemUtilityAuditPassed &&
                              itemUtilityUsePassed && itemUtilityActionPassed &&
                              movementPassed && followPassed &&
                              playerFollowCastSafetyPassed && followRetargetPassed &&
                              playerHealRevalidationPassed && playerHealLosPassed &&
                              movePointControlSafetyPassed && movementInterruptPassed &&
                              realGroupThreatSnapshotPassed;

    sMapMgr.DeleteTestMap(map);

    bool const removed = sMapMgr.FindMap(mapId, instanceId) == nullptr;
    if (!registered || !mapRetained || !combatPassed || !removed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Test-map/combat assertions failed (registered=%u retained=%u combat=%u removed=%u instance=%u).",
                 registered ? 1u : 0u, mapRetained ? 1u : 0u, combatPassed ? 1u : 0u,
                 removed ? 1u : 0u, instanceId);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: created, ran a bounded Core Creature combat/threat scenario and deleted map %u instance %u.",
             mapId, instanceId);
    return true;
}
}
