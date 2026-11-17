#include "CoreCombatSimulator.h"

#include "AI/CreatureAI.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "PlayerBots/Engine/Action.h"
#include "PlayerBots/Engine/PartyBotEngine.h"
#include "PlayerBots/Engine/PartyCoordinator.h"
#include "PlayerBots/Engine/PartySnapshot.h"
#include "PlayerBots/Engine/Strategy.h"
#include "PlayerBots/Engine/Trigger.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellMgr.h"

#include <algorithm>
#include <chrono>
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


class SimulatorCoreAttackAction final : public Action
{
public:
    explicit SimulatorCoreAttackAction(Map& map) : m_map(map) {}
    std::string const& GetName() const override { return m_name; }
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
bool CreateSimulatorPlayer(Map& map, uint32 factionId, WorldSession& session, Player*& player)
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
    player->Relocate(0.0f, 6.0f, 4.0f, 0.0f);
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
    uint32 selectedAfterTankGuid = 0;
    uint32 selectedBelowThresholdGuid = 0;
    uint32 selectedAboveThresholdGuid = 0;

    if (hostilePair && aiUpdated)
    {
        initialHealth = boss.creature->GetHealth();
        tankDamage = tank.creature->DealDamage(
            boss.creature, 100, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        tankThreat = boss.creature->GetThreatManager().getThreat(tank.creature);
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
                          healthAfterFinalHit < healthAfterDps && tankThreat > 0.0f && dpsThreat > 0.0f;

        targetSelection = selectedAfterTank == tank.creature &&
                          selectedBelowThreshold == tank.creature &&
                          dpsThreat > 1.1f * tankThreat &&
                          selectedAboveThreshold == damageDealer.creature;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBotSimulator] Creature probe: template=%u factions=%u/%u aiUpdates=%u/%u/%u hostile=%u health=%u>%u>%u>%u damage=%u/%u/%u threat=%.1f/%.1f switchThreshold=%.1f selectedGUID=%u/%u/%u damageThreat=%u targetSwitch=%u.",
             info->entry, bossFaction, attackerFaction,
             boss.updateCount, tank.updateCount, damageDealer.updateCount,
             hostilePair ? 1u : 0u, initialHealth, healthAfterTank, healthAfterDps, healthAfterFinalHit,
             tankDamage, dpsDamage, additionalDamage, tankThreat, dpsThreat, 1.1f * tankThreat,
             selectedAfterTankGuid, selectedBelowThresholdGuid, selectedAboveThresholdGuid,
             damageAndThreat ? 1u : 0u, targetSelection ? 1u : 0u);

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
             "[PartyBotSimulator] PASS: real Creature updates, direct damage, damage-generated threat and 110%% melee target switching.");
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

    bool const combatPassed = registered && mapRetained &&
                             RunCoreCombatThreatSmokeTest(*map) &&
                             RunEngineCoreExecutorSmokeTest(*map);
                             RunEngineCoreSpellHealSmokeTest(*map);
                             RunEngineCorePlayerSpellHealSmokeTest(*map);

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
