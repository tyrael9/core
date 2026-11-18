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
#include "PlayerBots/Engine/Action.h"
#include "PlayerBots/Engine/PartyBotEngine.h"
#include "PlayerBots/Engine/PartySnapshotBuilder.h"
#include "PlayerBots/Engine/PartyCoordinator.h"
#include "PlayerBots/Engine/PartySnapshot.h"
#include "PlayerBots/Engine/CoreThreatImpactEstimator.h"
#include "PlayerBots/Engine/ThreatBudgetPolicy.h"
#include "PlayerBots/Engine/Strategy.h"
#include "PlayerBots/Engine/Trigger.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"

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

class SimulatorThreatBudgetProbeAction final : public Action
{
public:
    explicit SimulatorThreatBudgetProbeAction(std::vector<ThreatImpactEntry> impacts)
        : m_impacts(std::move(impacts)) {}

    std::string const& GetName() const override { return m_name; }

    bool IsPossible(DecisionContext const&, ActionCandidate const&, std::string&) const override
    {
        return true;
    }

    bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string&) const override
    {
        return true;
    }

    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::Complete;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reservationDurationMs = 2000;
        estimate.impacts = m_impacts;
        estimate.reason = "simulator_core_snapshot_threat_budget_probe";
        return estimate;
    }

    ActionResult Execute(DecisionContext&, ActionCandidate const&) override
    {
        ++m_executeCount;
        return ActionResult::Success;
    }

    uint32 GetExecuteCount() const { return m_executeCount; }

private:
    std::vector<ThreatImpactEntry> m_impacts;
    std::string m_name = "simulator_threat_budget_probe";
    uint32 m_executeCount = 0;
};

class SimulatorThreatBudgetProbeTrigger final : public Trigger
{
public:
    explicit SimulatorThreatBudgetProbeTrigger(Guid hostileGuid)
        : m_hostileGuid(hostileGuid) {}

    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const&) const override { return true; }

    void CollectCandidates(DecisionContext const&,
                           std::vector<ActionCandidate>& candidates) const override
    {
        ActionCandidate candidate;
        candidate.actionName = "simulator_threat_budget_probe";
        candidate.targetGuid = m_hostileGuid;
        candidate.basePriority = 100.0f;
        candidate.channel = ActionChannel::GlobalCooldown;
        candidates.push_back(candidate);
    }

private:
    Guid m_hostileGuid = 0;
    std::string m_name = "simulator_threat_budget_probe";
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

class SimulatorCorePlayerHealAction final : public Action
{
public:
    explicit SimulatorCorePlayerHealAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }

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
             "[PartyBotSimulator] Movement interrupts Player cast: healRegistered=%u healSelected=%u castAccepted=%u moveRegistered=%u moveSelected=%u moveExecuted=%u castCancelled=%u health=%u>%u movementCompleted=%u cleanup=%u.",
             healRegistered ? 1u : 0u, healSelected ? 1u : 0u, castAccepted ? 1u : 0u,
             moveRegistered ? 1u : 0u, moveSelected ? 1u : 0u, moveExecuted ? 1u : 0u,
             castCancelled ? 1u : 0u, healthBefore, healthAfter,
             movementCompleted ? 1u : 0u, cleaned ? 1u : 0u);

    if (!healRegistered || !healSelected || !castAccepted ||
        !moveRegistered || !moveSelected || !moveExecuted ||
        !castCancelled || !movementCompleted || !cleaned)
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
    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;

    auto cleanup = [&]() -> bool
    {
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
        return actorsCleaned && ungrouped && sessionsCleaned && playersDetachedFromMap;
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
    if (membersAdded && mainTankAssigned && allPlayersOnSameMap)
        initialSnapshot = PartySnapshotBuilder::GetOrBuild(players[0], coordinator, 15000, 250);

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
                member->z == player->GetPositionZ();
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

    // Give the Healer a real, deliberately tiny assist-threat reference on
    // each add. The resulting snapshot has a current victim plus a non-victim
    // threat owner, so the Engine can assess both 110%/130% Core headrooms.
    float healerThreatBeforeBudget0 = 0.0f;
    float healerThreatBeforeBudget1 = 0.0f;
    bool threatBudgetSnapshotValid = false;
    bool overBudgetRejected = false;
    bool safeBudgetSelectedAndReserved = false;
    bool enginePreservedCoreThreatState = false;
    bool enginePreservedCoreVictims = false;
    bool reservationReleased = false;

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
            std::vector<ThreatImpactEntry> overBudgetImpacts;
            ThreatImpactEntry overFirst;
            overFirst.threatOwnerGuid = healerGuid.GetRawValue();
            overFirst.hostileGuid = hostiles[0].creature->GetObjectGuid().GetRawValue();
            overFirst.threatPoints = firstAssessment.rawHeadroom + 1.0f;
            overFirst.source = ThreatImpactSource::DirectDamage;
            overFirst.bound = ThreatImpactBound::UpperBound;
            ThreatImpactEntry overSecond = overFirst;
            overSecond.hostileGuid = hostiles[1].creature->GetObjectGuid().GetRawValue();
            overSecond.threatPoints = secondAssessment.rawHeadroom + 1.0f;
            overBudgetImpacts = {overFirst, overSecond};

            DecisionContext budgetContext(*budgetSnapshot, *healerForBudget, coordinator, 15500);
            PartyBotEngine overBudgetEngine;
            std::shared_ptr<SimulatorThreatBudgetProbeAction> overBudgetAction(
                new SimulatorThreatBudgetProbeAction(overBudgetImpacts));
            bool const overRegistered = overBudgetEngine.RegisterAction(overBudgetAction);
            std::shared_ptr<Strategy> overStrategy(new Strategy("simulator_over_budget_probe"));
            overStrategy->AddTrigger(std::make_shared<SimulatorThreatBudgetProbeTrigger>(
                hostiles[0].creature->GetObjectGuid().GetRawValue()));
            overBudgetEngine.AddStrategy(overStrategy);
            DecisionReport overReport = overBudgetEngine.Evaluate(budgetContext);
            overBudgetRejected = overRegistered && overReport.selectedIndices.empty() &&
                overReport.candidates.size() == 1 &&
                overReport.candidates[0].status == CandidateStatus::RejectedThreatBudget &&
                overBudgetAction->GetExecuteCount() == 0;

            float const safeFirstPoints = std::min(1.0f, firstAssessment.rawHeadroom * 0.25f);
            float const safeSecondPoints = std::min(1.0f, secondAssessment.rawHeadroom * 0.25f);
            std::vector<ThreatImpactEntry> safeImpacts;
            ThreatImpactEntry safeFirst = overFirst;
            safeFirst.threatPoints = safeFirstPoints;
            ThreatImpactEntry safeSecond = overSecond;
            safeSecond.threatPoints = safeSecondPoints;
            safeImpacts = {safeFirst, safeSecond};

            PartyBotEngine safeBudgetEngine;
            std::shared_ptr<SimulatorThreatBudgetProbeAction> safeBudgetAction(
                new SimulatorThreatBudgetProbeAction(safeImpacts));
            bool const safeRegistered = safeBudgetEngine.RegisterAction(safeBudgetAction);
            std::shared_ptr<Strategy> safeStrategy(new Strategy("simulator_safe_budget_probe"));
            safeStrategy->AddTrigger(std::make_shared<SimulatorThreatBudgetProbeTrigger>(
                hostiles[0].creature->GetObjectGuid().GetRawValue()));
            safeBudgetEngine.AddStrategy(safeStrategy);
            DecisionReport safeReport = safeBudgetEngine.Evaluate(budgetContext);
            bool const safeSelected = safeRegistered && safeReport.selectedIndices.size() == 1 &&
                safeReport.candidates[safeReport.selectedIndices[0]].status == CandidateStatus::Selected;
            safeBudgetEngine.ExecuteSelected(budgetContext, safeReport);
            CandidateEvaluation const* safeEvaluation = safeReport.candidates.empty()
                ? nullptr : &safeReport.candidates[0];
            safeBudgetSelectedAndReserved = safeSelected && safeEvaluation &&
                safeEvaluation->status == CandidateStatus::Executed &&
                safeBudgetAction->GetExecuteCount() == 1 &&
                safeEvaluation->threatActionId != 0 &&
                safeEvaluation->candidate.threatActionId == safeEvaluation->threatActionId &&
                std::fabs(coordinator.GetReservedThreatImpact(
                    safeFirst.hostileGuid, safeFirst.threatOwnerGuid, 0, 15500) - safeFirstPoints) < 0.01f &&
                std::fabs(coordinator.GetReservedThreatImpact(
                    safeSecond.hostileGuid, safeSecond.threatOwnerGuid, 0, 15500) - safeSecondPoints) < 0.01f;

            if (safeEvaluation && safeEvaluation->threatActionId)
            {
                coordinator.ReleaseThreatAction(safeEvaluation->threatActionId);
                reservationReleased = coordinator.GetReservedThreatImpact(
                    safeFirst.hostileGuid, safeFirst.threatOwnerGuid, 0, 15500) == 0.0f &&
                    coordinator.GetReservedThreatImpact(
                    safeSecond.hostileGuid, safeSecond.threatOwnerGuid, 0, 15500) == 0.0f;
            }

            float const healerThreatAfterBudget0 =
                hostiles[0].creature->GetThreatManager().getThreat(healer);
            float const healerThreatAfterBudget1 =
                hostiles[1].creature->GetThreatManager().getThreat(healer);
            enginePreservedCoreThreatState =
                std::fabs(healerThreatAfterBudget0 - healerThreatBeforeBudget0) < 0.01f &&
                std::fabs(healerThreatAfterBudget1 - healerThreatBeforeBudget1) < 0.01f &&
                std::fabs(hostiles[0].creature->GetThreatManager().getThreat(tank) - tankThreat0) < 0.01f &&
                std::fabs(hostiles[1].creature->GetThreatManager().getThreat(tank) - tankThreat1) < 0.01f;
            enginePreservedCoreVictims =
                hostiles[0].creature->GetThreatManager().getHostileTarget() == tank &&
                hostiles[1].creature->GetThreatManager().getHostileTarget() == tank;
        }
    }

    // The healer performs a normal non-triggered Lesser Heal. The follow-up
    // assist is an explicit scripted event through Core's native
    // HostileRefManager::threatAssist API; the simulator does not implement
    // healing-threat arithmetic or mutate either ThreatManager directly.
    uint32 const patientHealthBeforeDamage = tank->GetHealth();
    uint32 const scriptedTankDamage = std::max<uint32>(1u, tank->GetMaxHealth() / 5u);
    hostiles[0].creature->DealDamage(
        tank, scriptedTankDamage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    uint32 const patientHealthBeforeHeal = tank->GetHealth();
    bool const tankAliveAndInjured = tank->IsAlive() && patientHealthBeforeHeal > 0 &&
        patientHealthBeforeHeal < patientHealthBeforeDamage;
    uint32 const healerManaBefore = healer->GetPower(POWER_MANA);
    SpellEntry const* const healSpell = sSpellMgr.GetSpellEntry(SIMULATOR_HEAL_SPELL_ID);
    healer->SetFacingToObject(tank);
    SpellCastResult const healCastResult = healSpell && tankAliveAndInjured
        ? healer->CastSpell(tank, healSpell, false) : SPELL_FAILED_UNKNOWN;
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
        initialValuesCopied && initialRolesUnknown && initialCacheRetained &&
        registeredBeforeCleanup && bothHostilesInitiallyTargetTank &&
        threatBudgetSnapshotValid && overBudgetRejected && safeBudgetSelectedAndReserved &&
        enginePreservedCoreThreatState && enginePreservedCoreVictims && reservationReleased &&
        tankAliveAndInjured && healCompleted && healerPulledBothAdds &&
        snapshotValid && rosterRoleStateValid && bothAddsResolved && threatValuesCopied && coreReachabilityCopied;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Real Group multi-add healer threat: aiReady=%u members=%u mainTank=%u sameMap=%u initialSnapshot=%u initialRoster=%u initialValues=%u rolesUnknown=%u cache=%u registered=%u tankDamage=%u/%u tankHP=%u-%u/%u tankThreat=%.1f/%.1f initialTankVictim=%u/%u budgetSnapshot=%u overBudgetRejected=%u safeSelectedReserved=%u threatUnchanged=%u victimUnchanged=%u reservationReleased=%u hostileRefsOnTank=%u healResult=%u heal=%u>%u mana=%u>%u assistInput=%.1f healerThreat=%.1f/%.1f selectedHealer=%u/%u finalSnapshot=%u finalRoster=%u adds=%u threatCopied=%u meleeReachability=%u/4 reachable=%u unreachable=%u.",
             hostileAIReady ? 1u : 0u, membersAdded ? 1u : 0u,
             mainTankAssigned ? 1u : 0u, allPlayersOnSameMap ? 1u : 0u,
             initialSnapshotValid ? 1u : 0u, initialRosterResolved ? 1u : 0u,
             initialValuesCopied ? 1u : 0u, initialRolesUnknown ? 1u : 0u,
             initialCacheRetained ? 1u : 0u, registeredBeforeCleanup ? 1u : 0u,
             tankDamage0, tankDamage1, patientHealthBeforeDamage,
             scriptedTankDamage, patientHealthBeforeHeal, tankThreat0, tankThreat1,
             initialVictim0 == tank ? 1u : 0u, initialVictim1 == tank ? 1u : 0u,
             threatBudgetSnapshotValid ? 1u : 0u, overBudgetRejected ? 1u : 0u,
             safeBudgetSelectedAndReserved ? 1u : 0u,
             enginePreservedCoreThreatState ? 1u : 0u,
             enginePreservedCoreVictims ? 1u : 0u, reservationReleased ? 1u : 0u,
             hostileRefsOnTank, static_cast<uint32>(healCastResult),
             patientHealthBeforeHeal, patientHealthAfterHeal,
             healerManaBefore, healerManaAfter, kScriptedAssistThreat,
             healerThreat0, healerThreat1,
             selectedAfterAssist0 == healer ? 1u : 0u,
             selectedAfterAssist1 == healer ? 1u : 0u,
             snapshotValid ? 1u : 0u, rosterRoleStateValid ? 1u : 0u,
             bothAddsResolved ? 1u : 0u, threatValuesCopied ? 1u : 0u,
             meleeReachabilityChecks, meleeReachableChecks, meleeUnreachableChecks);

    if (f9ChecksPassed)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBotSimulator] PASS: one real five-Player Core Group passed initial roster/cache validation, two Core hostile adds switched from Main Tank to healer through assist-threat, final threat values and both reachable/unreachable per-reference melee states matched Core, and all Group/session cleanup succeeded.");

    // F12: a hostile can still hold threat references to a party member
    // without currently attacking any group member. It must be discovered
    // through that member's HostileRefManager, including offline references.
    bool const f12CanProbe = f9ChecksPassed && outsideGroupVictim.creature &&
        outsideGroupVictim.creature->IsInWorld();
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
    uint32 const tankHealthAtDeath = tank->GetHealth();
    uint32 const tankDeathDamage = f9ChecksPassed && healerHoldsAggroOverDps
        ? hostiles[0].creature->DealDamage(
            tank, tankHealthAtDeath, nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    bool const tankDied = tankDeathDamage > 0 && !tank->IsAlive() && tank->GetHealth() == 0;
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

    bool const f10ChecksPassed = f9ChecksPassed && f12ChecksPassed && healerHoldsAggroOverDps &&
        tankDied && tankThreatReferencesRemoved && healerRetargetedAfterTankDeath &&
        tankDeathSnapshotValid && tankDeathRosterValid && tankDeathThreatSnapshotValid &&
        healerDied && healerThreatReferencesRemoved && dpsRetargetedAfterHealerDeath &&
        healerDeathSnapshotValid && healerDeathRosterValid && healerDeathThreatSnapshotValid;

    uint32 const groupIdAfterEvents = group->GetId();
    bool const groupStillRegistered = registeredBeforeCleanup &&
        sObjectMgr.GetGroupById(groupIdAfterEvents) == group.get();
    bool const cleaned = cleanup();
    bool const groupRemoved = sObjectMgr.GetGroupById(groupIdAfterEvents) == nullptr;
    bool const cleanupPassed = cleaned && groupRemoved;

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

    if (!f9ChecksPassed || !f12ChecksPassed || !groupStillRegistered)
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

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: Core death cleanup removed Main Tank and healer references from both adds; SelectHostileTarget retargeted first to the surviving healer and then to the surviving DPS, and PartySnapshotBuilder copied both post-death states.");
    return true;
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

    patient.creature->SetHealth(patient.creature->GetMaxHealth() / 2);
    uint32 const healthBefore = patient.creature->GetHealth();
    uint32 const manaBefore = healer->GetPower(POWER_MANA);

    PartySnapshot snapshot;
    snapshot.groupId = 3;
    snapshot.mapId = map.GetId();
    snapshot.instanceId = map.GetInstanceId();
    snapshot.epoch = 3;
    snapshot.capturedAtMs = 3000;

    PartyMemberSnapshot healerMember;
    healerMember.guid = healer->GetObjectGuid().GetRawValue();
    healerMember.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    healerMember.mapId = map.GetId();
    healerMember.instanceId = map.GetInstanceId();
    healerMember.health = healer->GetHealth();
    healerMember.maxHealth = healer->GetMaxHealth();
    healerMember.online = true;
    healerMember.alive = healer->IsAlive();
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
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 3000);
    PartyBotEngine engine;
    std::shared_ptr<SimulatorCorePlayerHealAction> healAction(new SimulatorCorePlayerHealAction(map));
    bool const registered = engine.RegisterAction(healAction);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_healer_triage"));
    strategy->AddTrigger(std::make_shared<SimulatorHealerTriageTrigger>("core_player_heal"));
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].candidate.targetGuid == patientMember.guid &&
        report.candidates[report.selectedIndices[0]].candidate.spellId == SIMULATOR_HEAL_SPELL_ID &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected;
    engine.ExecuteSelected(context, report);

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
    bool const completed = healer->GetCurrentSpell(CURRENT_GENERIC_SPELL) == nullptr &&
                           healthAfter > healthBefore && manaAfter < manaBefore;

    healerSession.LogoutPlayer(false);
    bool const playerCleaned = healerSession.GetPlayer() == nullptr;
    healer = nullptr; // LogoutPlayer(true/false) removes and deletes the in-world Player.
    bool const patientCleaned = DestroySimulatorActor(map, patient);
    bool const cleaned = playerCleaned && patientCleaned;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Engine Player spell/heal: known=%u selected=%u accepted=%u pending=%u gcd=%u castResult=%u mana=%u>%u health=%u>%u completed=%u cleanup=%u.",
             1u, selected ? 1u : 0u, accepted ? 1u : 0u,
             healAction->WasCastPendingAfterAccept() ? 1u : 0u,
             healAction->HadGcdAfterAccept() ? 1u : 0u,
             static_cast<uint32>(healAction->GetLastCastResult()),
             manaBefore, manaAfter, healthBefore, healthAfter,
             completed ? 1u : 0u, cleaned ? 1u : 0u);

    if (!registered || !selected || !accepted || !completed || !cleaned)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Engine-to-Player normal spell/heal smoke test failed.");
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] PASS: PartyBotEngine selected a known Player spell, Core accepted a non-triggered cast with GCD, and the cast completed with real healing and mana cost.");
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

bool RunEngineCoreThreatBudgetAdmissionSmokeTest(Map& map)
{
    CreatureInfo const* info = FindSafeCreatureTemplate();
    uint32 partyFaction = 0, hostileFaction = 0;
    if (!info || !FindMutuallyHostileFactions(partyFaction, hostileFaction))
        return false;

    std::vector<std::unique_ptr<WorldSession>> sessions;
    std::vector<Player*> players;
    std::unique_ptr<Group> group(new Group());
    bool groupRegistered = false;
    SimulatorActor hostileActor;

    auto cleanup = [&]() -> bool
    {
        bool actorsCleaned = DestroySimulatorActor(map, hostileActor);
        bool ungrouped = true;
        if (group && group->GetId())
        {
            ObjectGuid initiator = players.empty()
                ? ObjectGuid() : players.front()->GetObjectGuid();
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
        bool playersDetached = true;
        for (Player* player : players)
            if (player && map.GetUnit(player->GetObjectGuid()))
                playersDetached = false;
        players.clear();
        sessions.clear();
        return actorsCleaned && ungrouped && sessionsCleaned && playersDetached;
    };

    // Three real Core Players: Main Tank, damage bot and a second bot that
    // owns an independent in-flight action reservation.
    for (uint32 index = 0; index < 3; ++index)
    {
        std::unique_ptr<WorldSession> session(
            new WorldSession(0, nullptr, SEC_PLAYER, 0, LOCALE_enUS));
        Player* player = nullptr;
        bool const created = CreateSimulatorPlayer(
            map, partyFaction, *session, player,
            static_cast<float>(index) * 2.0f, 6.0f, 0.0f);
        sessions.push_back(std::move(session));
        if (player)
            players.push_back(player);
        if (!created || !player || !player->IsInWorld())
        {
            bool const cleaned = cleanup();
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[PartyBotSimulator] Threat budget Player creation failed at %u (cleanup=%u).",
                     index, cleaned ? 1u : 0u);
            return false;
        }
    }

    bool groupCreated = group->Create(players[0]->GetObjectGuid(), players[0]->GetName());
    if (groupCreated)
    {
        sObjectMgr.AddGroup(group.get());
        groupRegistered = true;
    }
    bool membersAdded = groupCreated && group->GetMembersCount() == 1;
    for (std::size_t index = 1; membersAdded && index < players.size(); ++index)
    {
        bool const added = group->AddMember(players[index]->GetObjectGuid(), players[index]->GetName());
        membersAdded = membersAdded && added && players[index]->GetGroup() == group.get();
    }
    if (membersAdded)
    {
        group->ConvertToRaid();
        group->SetMainTank(players[0]->GetObjectGuid());
    }
    bool const mainTankAssigned = membersAdded &&
        group->GetMainTankGuid() == players[0]->GetObjectGuid();

    bool const hostileCreated = CreateSimulatorActor(
        map, *info, hostileFaction, 1.0f, 6.0f, hostileActor);
    if (!mainTankAssigned || !hostileCreated)
    {
        bool const cleaned = cleanup();
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Threat budget fixture setup failed (group=%u tank=%u hostile=%u cleanup=%u).",
                 membersAdded ? 1u : 0u, mainTankAssigned ? 1u : 0u,
                 hostileCreated ? 1u : 0u, cleaned ? 1u : 0u);
        return false;
    }

    for (uint32 tick = 0; tick < 120 && !hostileActor.updateCount; ++tick)
    {
        map.Update(50);
        if (!hostileActor.updateCount)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    Player* tank = players[0];
    Player* dps = players[1];
    Player* reservationOwner = players[2];
    ObjectGuid const tankGuid = tank->GetObjectGuid();
    ObjectGuid const dpsGuid = dps->GetObjectGuid();
    Guid const hostileGuid = hostileActor.creature->GetObjectGuid().GetRawValue();

    if (hostileActor.updateCount)
        hostileActor.creature->AI()->AttackStart(tank);
    uint32 const tankDamage = hostileActor.updateCount
        ? tank->DealDamage(hostileActor.creature, 400, nullptr, DIRECT_DAMAGE,
                           SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    uint32 const dpsDamage = hostileActor.updateCount
        ? dps->DealDamage(hostileActor.creature, 100, nullptr, DIRECT_DAMAGE,
                           SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;

    float const tankThreat = hostileActor.creature->GetThreatManager().getThreat(tank);
    float const dpsThreatBefore = hostileActor.creature->GetThreatManager().getThreat(dps);
    Unit* const victimBefore = hostileActor.creature->GetThreatManager().getHostileTarget();

    PartyCoordinator coordinator;
    uint32 constexpr SNAPSHOT_TIME = 30000;
    PartySnapshot const* builtSnapshot =
        PartySnapshotBuilder::GetOrBuild(tank, coordinator, SNAPSHOT_TIME, 250);
    PartySnapshot snapshot = builtSnapshot ? *builtSnapshot : PartySnapshot();
    HostileSnapshot const* hostileSnapshot = snapshot.FindHostile(hostileGuid);
    PartyMemberSnapshot const* dpsSnapshot = snapshot.FindMember(dpsGuid.GetRawValue());
    PartyMemberSnapshot const* tankSnapshot = snapshot.FindMember(tankGuid.GetRawValue());
    bool const snapshotMatchesCore = builtSnapshot &&
        snapshot.groupId == group->GetId() && snapshot.members.size() == players.size() &&
        hostileSnapshot && hostileSnapshot->alive && hostileSnapshot->victimGuid == tankGuid.GetRawValue() &&
        tankSnapshot && dpsSnapshot &&
        std::fabs(hostileActor.creature->GetThreatManager().getThreat(tank) - tankSnapshot->guid * 0.0f -
                  tankThreat) < 0.01f;

    ThreatBudgetAssessment const initialBudget = snapshotMatchesCore
        ? AssessThreatBudget(snapshot, hostileGuid, dpsGuid.GetRawValue(), SNAPSHOT_TIME, 250)
        : ThreatBudgetAssessment();
    float const threatPerDamage = EstimateAppliedDirectDamageComponent(
        dps, hostileActor.creature, 1, SPELL_SCHOOL_MASK_NORMAL,
        snapshot.epoch, ThreatImpactBound::Exact).impacts.empty()
            ? 0.0f
            : EstimateAppliedDirectDamageComponent(
                dps, hostileActor.creature, 1, SPELL_SCHOOL_MASK_NORMAL,
                snapshot.epoch, ThreatImpactBound::Exact).impacts[0].threatPoints;

    bool const budgetAvailable = initialBudget.status == ThreatBudgetStatus::BudgetAvailable &&
        std::isfinite(threatPerDamage) && threatPerDamage > 0.0f;
    uint32 const initialExternalReservation = 10;
    ThreatActionId externalActionId = 0;
    if (budgetAvailable)
    {
        externalActionId = coordinator.NextThreatActionId();
        ThreatReservation external;
        external.ownerGuid = reservationOwner->GetObjectGuid().GetRawValue();
        external.threatOwnerGuid = dpsGuid.GetRawValue();
        external.hostileGuid = hostileGuid;
        external.actionId = externalActionId;
        external.threatPoints = static_cast<float>(initialExternalReservation);
        external.expiresAtMs = SNAPSHOT_TIME + 5000;
        coordinator.TryReserveThreatImpact(external, SNAPSHOT_TIME);
    }

    double const initialAvailableAfterReservation =
        static_cast<double>(initialBudget.rawHeadroom) - initialExternalReservation;
    uint32 safeDamage = budgetAvailable && initialAvailableAfterReservation > 0.0
        ? static_cast<uint32>(std::floor(initialAvailableAfterReservation / threatPerDamage * 0.25))
        : 0;
    if (safeDamage == 0)
        safeDamage = 1;

    PartyBotEngine engine;
    std::shared_ptr<SimulatorThreatBudgetProbeAction> action(
        new SimulatorThreatBudgetProbeAction(map));
    bool const actionRegistered = engine.RegisterAction(action);
    std::shared_ptr<Strategy> strategy(new Strategy("simulator_threat_budget"));
    strategy->AddTrigger(std::make_shared<SimulatorThreatBudgetProbeTrigger>(hostileGuid, safeDamage));
    engine.AddStrategy(strategy);

    DecisionContext context(snapshot, dpsSnapshot ? *dpsSnapshot : snapshot.members.front(),
                            coordinator, SNAPSHOT_TIME);
    DecisionReport report = engine.Evaluate(context);
    bool const selected = report.selectedIndices.size() == 1 &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected &&
        report.candidates[report.selectedIndices[0]].threatImpact.completeness ==
            ThreatImpactCompleteness::Complete;
    bool const evaluateDidNotMutateCore =
        std::fabs(hostileActor.creature->GetThreatManager().getThreat(dps) - dpsThreatBefore) < 0.01f &&
        hostileActor.creature->GetThreatManager().getHostileTarget() == victimBefore &&
        coordinator.GetThreatReservationCount() == 1;

    engine.ExecuteSelected(context, report);
    bool const actionAccepted = selected && !report.selectedIndices.empty() &&
        report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed &&
        action->GetExecuteCount() == 1 && action->GetLastThreatActionId() != 0;
    bool const executionDidNotMutateCore =
        std::fabs(hostileActor.creature->GetThreatManager().getThreat(dps) - dpsThreatBefore) < 0.01f &&
        hostileActor.creature->GetThreatManager().getHostileTarget() == victimBefore;
    float const forecastThreat = !report.selectedIndices.empty()
        ? report.candidates[report.selectedIndices[0]].threatImpact.impacts[0].threatPoints
        : 0.0f;
    float const reservedAfterAccept = coordinator.GetReservedThreatImpact(
        hostileGuid, dpsGuid.GetRawValue(), 0, SNAPSHOT_TIME);

    uint32 const appliedDamage = actionAccepted
        ? dps->DealDamage(hostileActor.creature, safeDamage, nullptr, DIRECT_DAMAGE,
                          SPELL_SCHOOL_MASK_NORMAL, nullptr, false) : 0;
    float const dpsThreatAfterDamage = hostileActor.creature->GetThreatManager().getThreat(dps);
    float const appliedThreatDelta = dpsThreatAfterDamage - dpsThreatBefore;
    coordinator.ReleaseThreatAction(action->GetLastThreatActionId());
    float const reservationAfterCompletion = coordinator.GetReservedThreatImpact(
        hostileGuid, dpsGuid.GetRawValue(), 0, SNAPSHOT_TIME + 1);
    bool const coreThreatMatchedForecast = appliedDamage == safeDamage &&
        std::fabs(appliedThreatDelta - forecastThreat) < 0.01f &&
        std::fabs(reservedAfterAccept - (initialExternalReservation + forecastThreat)) < 0.01f &&
        std::fabs(reservationAfterCompletion - initialExternalReservation) < 0.01f;

    // Refresh the production snapshot after the actual Core damage event.
    // The next action is sized one threat point beyond the remaining headroom
    // after the independent pending reservation.
    uint32 constexpr REFRESHED_SNAPSHOT_TIME = SNAPSHOT_TIME + 300;
    PartySnapshot const* refreshedBuilt =
        PartySnapshotBuilder::GetOrBuild(tank, coordinator, REFRESHED_SNAPSHOT_TIME, 250);
    PartySnapshot refreshed = refreshedBuilt ? *refreshedBuilt : PartySnapshot();
    PartyMemberSnapshot const* refreshedDps = refreshed.FindMember(dpsGuid.GetRawValue());
    HostileSnapshot const* refreshedHostile = refreshed.FindHostile(hostileGuid);
    ThreatBudgetAssessment const refreshedBudget = refreshedBuilt && refreshedDps && refreshedHostile
        ? AssessThreatBudget(refreshed, hostileGuid, dpsGuid.GetRawValue(),
                             REFRESHED_SNAPSHOT_TIME, 250)
        : ThreatBudgetAssessment();
    float const remainingReservation = coordinator.GetReservedThreatImpact(
        hostileGuid, dpsGuid.GetRawValue(), 0, REFRESHED_SNAPSHOT_TIME);
    uint32 overBudgetDamage = 0;
    if (refreshedBudget.status == ThreatBudgetStatus::BudgetAvailable &&
        std::isfinite(threatPerDamage) && threatPerDamage > 0.0f)
    {
        double const remainingHeadroom =
            static_cast<double>(refreshedBudget.rawHeadroom) - remainingReservation;
        if (remainingHeadroom >= 0.0)
            overBudgetDamage = static_cast<uint32>(std::floor(remainingHeadroom / threatPerDamage)) + 1;
    }

    PartyBotEngine overBudgetEngine;
    std::shared_ptr<SimulatorThreatBudgetProbeAction> overBudgetAction(
        new SimulatorThreatBudgetProbeAction(map));
    bool const overBudgetRegistered = overBudgetEngine.RegisterAction(overBudgetAction);
    std::shared_ptr<Strategy> overBudgetStrategy(new Strategy("simulator_over_budget_threat"));
    overBudgetStrategy->AddTrigger(
        std::make_shared<SimulatorThreatBudgetProbeTrigger>(hostileGuid, overBudgetDamage));
    overBudgetEngine.AddStrategy(overBudgetStrategy);
    DecisionContext refreshedContext(
        refreshed, refreshedDps ? *refreshedDps : refreshed.members.front(),
        coordinator, REFRESHED_SNAPSHOT_TIME);
    float const threatBeforeRejectedCandidate =
        hostileActor.creature->GetThreatManager().getThreat(dps);
    Unit* const victimBeforeRejectedCandidate =
        hostileActor.creature->GetThreatManager().getHostileTarget();
    DecisionReport overBudgetReport = overBudgetEngine.Evaluate(refreshedContext);
    bool const overBudgetRejected = overBudgetReport.selectedIndices.empty() &&
        overBudgetReport.candidates.size() == 1 &&
        overBudgetReport.candidates[0].status == CandidateStatus::RejectedThreatBudget &&
        overBudgetAction->GetExecuteCount() == 0 &&
        std::fabs(hostileActor.creature->GetThreatManager().getThreat(dps) -
                  threatBeforeRejectedCandidate) < 0.01f &&
        hostileActor.creature->GetThreatManager().getHostileTarget() ==
            victimBeforeRejectedCandidate;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] Core threat budget admission: group=%u snapshot=%u victimTank=%u tankThreat=%.1f dpsThreat=%.1f headroom=%.1f melee=%u reserved=%.1f safeDamage=%u selected=%u evalReadOnly=%u accepted=%u executeReadOnly=%u forecast=%.1f applied=%u actualDelta=%.1f reservationAfter=%0.1f completionRelease=%0.1f overDamage=%u overBudgetRejected=%u.",
        group->GetId(), snapshot.epoch, victimBefore == tank ? 1u : 0u,
        tankThreat, dpsThreatBefore, initialBudget.rawHeadroom,
        initialBudget.usesMeleeThreshold ? 1u : 0u,
        initialExternalReservation, safeDamage, selected ? 1u : 0u,
        evaluateDidNotMutateCore ? 1u : 0u, actionAccepted ? 1u : 0u,
        executionDidNotMutateCore ? 1u : 0u, forecastThreat, appliedDamage,
        appliedThreatDelta, reservedAfterAccept, reservationAfterCompletion,
        overBudgetDamage, overBudgetRejected ? 1u : 0u);

    bool const cleaned = cleanup();
    bool const passed = hostileActor.updateCount && tankDamage == 400 && dpsDamage == 100 &&
        tankThreat > dpsThreatBefore && victimBefore == tank &&
        snapshotMatchesCore && budgetAvailable && actionRegistered && selected &&
        evaluateDidNotMutateCore && actionAccepted && executionDidNotMutateCore &&
        coreThreatMatchedForecast && refreshedBudget.status == ThreatBudgetStatus::BudgetAvailable &&
        overBudgetRegistered && overBudgetDamage > 0 && overBudgetRejected && cleaned;

    if (!passed)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[PartyBotSimulator] Core threat budget admission failed (group=%u cleanup=%u).",
                 group->GetId(), cleaned ? 1u : 0u);
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[PartyBotSimulator] PASS: PartyBot admission matched the pinned Core threat calculation, reserved before action dispatch, preserved Core state during Evaluate/Execute, released after Core damage completion, and rejected a threshold-crossing candidate without Core side effects.");
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
    bool const engineExecutorPassed = RunEngineCoreExecutorSmokeTest(*map);
    bool const creatureHealPassed = RunEngineCoreSpellHealSmokeTest(*map);
    bool const playerHealPassed = RunEngineCorePlayerSpellHealSmokeTest(*map);
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
    bool const threatBudgetAdmissionPassed = RunEngineCoreThreatBudgetAdmissionSmokeTest(*map);
    bool const combatPassed = registered && mapRetained &&
                              creatureCombatPassed && engineExecutorPassed &&
                              creatureHealPassed && playerHealPassed &&
                              movementPassed && followPassed &&
                              playerFollowCastSafetyPassed && followRetargetPassed &&
                              playerHealRevalidationPassed && playerHealLosPassed &&
                              movePointControlSafetyPassed && movementInterruptPassed &&
                              realGroupThreatSnapshotPassed && threatBudgetAdmissionPassed;

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
