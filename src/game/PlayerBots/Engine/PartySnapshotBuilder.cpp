#include "PartySnapshotBuilder.h"
#include "CombatBotBaseAI.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MovementGenerator.h"
#include "PointMovementGenerator.h"
#include "MotionMaster.h"
#include "TargetedMovementGenerator.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "ThreatManager.h"
#include "Unit.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace PartyBotEngine {

namespace {
PartyRole ToPartyRole(CombatBotRoles role)
{
    switch (role)
    {
        case ROLE_TANK: return PartyRole::Tank;
        case ROLE_HEALER: return PartyRole::Healer;
        case ROLE_RANGE_DPS: return PartyRole::RangedDamage;
        case ROLE_MELEE_DPS: return PartyRole::MeleeDamage;
        default: return PartyRole::Unknown;
    }
}

bool IsSingleUnitHealTarget(std::uint32_t target)
{
    return target == TARGET_UNIT_CASTER ||
        target == TARGET_UNIT_FRIEND || target == TARGET_UNIT;
}

// These Vanilla Power Word: Shield ranks share the registered
// spell_priest_power_word_shield SpellScript. Its only OnHit side effect is
// applying Weakened Soul after a successful shield hit; it does not alter the
// already-created absorb Aura or its remaining amount. AuraScript is still
// rejected independently below. Keep this allowlist limited to the exact
// ranks registered by spell_priest.cpp.
bool IsKnownPowerWordShieldSpellScript(std::uint32_t spellId)
{
    switch (spellId)
    {
        case 17:
        case 592:
        case 600:
        case 3747:
        case 6065:
        case 6066:
        case 10898:
        case 10899:
        case 10900:
        case 10901:
        case 27607:
            return true;
        default:
            return false;
    }
}

std::uint32_t EstimateInFlightDirectHeal(
    Spell* currentSpell, Unit* target)
{
    if (!currentSpell || !currentSpell->m_spellInfo || !target ||
        currentSpell->IsChanneled() ||
        (currentSpell->getState() != SPELL_STATE_PREPARING &&
         currentSpell->getState() != SPELL_STATE_CASTING))
        return 0;

    SpellEntry const* spellInfo = currentSpell->m_spellInfo;
    // Only a plain one-effect direct heal is predictable here. Scripted,
    // group/chain, mixed-effect and implicit-area heals stay uncredited.
    if (spellInfo->ScriptId)
        return 0;

    std::uint32_t healEffect = MAX_SPELL_EFFECTS;
    std::uint32_t healEffectCount = 0;
    for (std::uint32_t i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (spellInfo->Effect[i] == SPELL_EFFECT_NONE)
            continue;
        if (spellInfo->Effect[i] != SPELL_EFFECT_HEAL)
            return 0;

        ++healEffectCount;
        if (healEffectCount != 1)
            return 0;

        std::uint32_t const targetA = spellInfo->EffectImplicitTargetA[i];
        std::uint32_t const targetB = spellInfo->EffectImplicitTargetB[i];
        std::uint32_t const targetCount =
            (targetA != TARGET_NONE ? 1u : 0u) +
            (targetB != TARGET_NONE ? 1u : 0u);
        if (targetCount != 1 ||
            !IsSingleUnitHealTarget(targetA ? targetA : targetB))
            return 0;

        healEffect = i;
    }

    if (healEffectCount != 1 || healEffect >= MAX_SPELL_EFFECTS)
        return 0;

    float const amount = currentSpell->CalculateDamage(
        SpellEffectIndex(healEffect), target);
    if (!std::isfinite(amount) || amount <= 0.0f)
        return 0;

    double const rounded = std::floor(static_cast<double>(amount));
    if (rounded >= static_cast<double>(
            std::numeric_limits<std::uint32_t>::max()))
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(rounded);
}

void AddSaturatedHealingAmount(
    std::uint32_t& total, std::uint64_t amount)
{
    std::uint64_t const maximum =
        std::numeric_limits<std::uint32_t>::max();
    std::uint64_t const combined =
        static_cast<std::uint64_t>(total) + amount;
    total = static_cast<std::uint32_t>(
        combined > maximum ? maximum : combined);
}

void CaptureHealingForecast(Player* member, Map* map,
                            PartyMemberSnapshot& memberData)
{
    if (!member || !map || member->GetMap() != map ||
        !member->IsInWorld())
        return;

    // F54c: the current Aura amount already includes the 1.11+ caster-side
    // healing bonus. Apply Core's current target-side bonus and count only
    // finite remaining ticks. Scripted or otherwise unresolvable HoTs are
    // intentionally omitted rather than assigned a guessed value.
    auto const& periodicHeals =
        member->GetAurasByType(SPELL_AURA_PERIODIC_HEAL);
    for (Aura* aura : periodicHeals)
    {
        if (!aura || aura->GetAuraScript())
            continue;
        SpellEntry const* spellInfo = aura->GetSpellProto();
        Unit* caster = aura->GetCaster();
        Modifier const* modifier = aura->GetModifier();
        if (!spellInfo || spellInfo->ScriptId || !caster || !modifier ||
            aura->GetAuraDuration() <= 0 || modifier->periodictime <= 0)
            continue;

        std::uint32_t const maxTicks = aura->GetAuraMaxTicks();
        std::uint32_t const completedTicks = aura->GetAuraTicks();
        if (!maxTicks || completedTicks >= maxTicks ||
            !std::isfinite(modifier->m_amount) || modifier->m_amount <= 0.0f)
            continue;

        float const targetAdjusted = member->SpellHealingBonusTaken(
            caster, spellInfo, aura->GetEffIndex(), modifier->m_amount,
            DOT, aura->GetStackAmount());
        if (!std::isfinite(targetAdjusted) || targetAdjusted <= 0.0f)
            continue;

        double const flooredPerTick =
            std::floor(static_cast<double>(targetAdjusted));
        std::uint64_t const perTick = static_cast<std::uint64_t>(
            std::min(flooredPerTick, static_cast<double>(
                std::numeric_limits<std::uint32_t>::max())));
        std::uint64_t const remainingTicks = maxTicks - completedTicks;
        AddSaturatedHealingAmount(
            memberData.periodicHealRemainingAmount,
            perTick * remainingTicks);
        if (memberData.periodicHealForecasts.size() <
            MAX_PERIODIC_HEAL_FORECASTS_PER_MEMBER)
        {
            PeriodicHealForecastSnapshot forecast;
            forecast.casterGuid = caster->GetObjectGuid().GetRawValue();
            forecast.spellId = spellInfo->Id;
            int32 const firstTickTimer = aura->GetAuraPeriodicTimer();
            forecast.firstTickMs = firstTickTimer > 0
                ? static_cast<std::uint32_t>(firstTickTimer) : 0u;
            forecast.periodMs = static_cast<std::uint32_t>(
                modifier->periodictime);
            forecast.remainingTicks = remainingTicks;
            forecast.healingPerTick = static_cast<std::uint32_t>(perTick);
            memberData.periodicHealForecasts.push_back(forecast);
        }
        else
            memberData.periodicHealForecastsTruncated = true;
    }

    // Only non-scripted, all-seven-school absorbs are treated as generally
    // applicable effective health. School-specific shields and Mana Shield
    // have resource/school constraints and are not credited by this baseline.
    auto const& absorbs =
        member->GetAurasByType(SPELL_AURA_SCHOOL_ABSORB);
    for (Aura* aura : absorbs)
    {
        if (!aura || aura->GetAuraScript())
            continue;
        SpellEntry const* spellInfo = aura->GetSpellProto();
        Modifier const* modifier = aura->GetModifier();
        if (!spellInfo || !modifier ||
            (spellInfo->ScriptId &&
             !IsKnownPowerWordShieldSpellScript(spellInfo->Id)) ||
            modifier->m_miscvalue != 0x7f ||
            !std::isfinite(modifier->m_amount) || modifier->m_amount <= 0.0f)
            continue;

        double const flooredAbsorb =
            std::floor(static_cast<double>(modifier->m_amount));
        std::uint64_t const remainingAbsorb = static_cast<std::uint64_t>(
            std::min(flooredAbsorb, static_cast<double>(
                std::numeric_limits<std::uint32_t>::max())));
        AddSaturatedHealingAmount(
            memberData.allSchoolAbsorbRemainingAmount, remainingAbsorb);
    }

    std::sort(memberData.periodicHealForecasts.begin(),
        memberData.periodicHealForecasts.end(),
        [](PeriodicHealForecastSnapshot const& left,
           PeriodicHealForecastSnapshot const& right)
        {
            if (left.firstTickMs != right.firstTickMs)
                return left.firstTickMs < right.firstTickMs;
            if (left.casterGuid != right.casterGuid)
                return left.casterGuid < right.casterGuid;
            if (left.spellId != right.spellId)
                return left.spellId < right.spellId;
            if (left.remainingTicks != right.remainingTicks)
                return left.remainingTicks < right.remainingTicks;
            return left.healingPerTick < right.healingPerTick;
        });
}
}

PartySnapshot const* PartySnapshotBuilder::GetOrBuild(Player* bot, PartyCoordinator& coordinator,
                                                       std::uint32_t nowMs, std::uint32_t maxAgeMs)
{
    if (PartySnapshot const* cached = coordinator.GetSnapshotIfFresh(nowMs, maxAgeMs))
        return cached;
    PartySnapshot snapshot;
    if (!Build(bot, snapshot, coordinator.NextSnapshotEpoch(), nowMs))
        return nullptr;
    coordinator.ReconcileFormationAssignments(snapshot);
    coordinator.SetSnapshot(std::move(snapshot));
    return coordinator.GetSnapshotIfFresh(nowMs, maxAgeMs);
}

bool PartySnapshotBuilder::Build(Player* bot, PartySnapshot& snapshot,
                                  std::uint32_t epoch, std::uint32_t nowMs)
{
    if (!bot || !bot->IsInWorld())
        return false;
    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    if (!group || !map || group->isBGGroup() || !group->GetId())
        return false;

    snapshot.groupId = group->GetId();
    snapshot.groupLeaderGuid = group->GetLeaderGuid().GetRawValue();
    snapshot.mapId = bot->GetMapId();
    snapshot.instanceId = bot->GetInstanceId();
    snapshot.epoch = epoch;
    snapshot.capturedAtMs = nowMs;

    ObjectGuid const mainTankGuid = group->GetMainTankGuid();
    std::map<Guid, HostileSnapshot> hostiles;

    // All data is copied into GUID/scalar values. Live pointers remain local to
    // this map update and never enter the cached PartySnapshot.
    auto addHostile = [&](Unit* hostileUnit) -> HostileSnapshot*
    {
        if (!hostileUnit || !hostileUnit->IsInWorld() ||
            hostileUnit->GetMap() != map ||
            group->IsMember(hostileUnit->GetObjectGuid()))
            return nullptr;

        Guid const hostileGuid = hostileUnit->GetObjectGuid().GetRawValue();
        auto inserted = hostiles.emplace(hostileGuid, HostileSnapshot());
        if (!inserted.second)
            return &inserted.first->second;

        HostileSnapshot& hostile = inserted.first->second;
        hostile.guid = hostileGuid;
        hostile.victimGuid = hostileUnit->GetVictim()
            ? hostileUnit->GetVictim()->GetObjectGuid().GetRawValue() : 0;
        if (Creature* creature = hostileUnit->ToCreature())
        {
            CreatureInfo const* info = creature->GetCreatureInfo();
            if (info)
            {
                hostile.entry = info->entry;
                hostile.rank = info->rank;
                hostile.worldBoss = creature->IsWorldBoss();
            }
        }
        hostile.health = hostileUnit->GetHealth();
        hostile.maxHealth = hostileUnit->GetMaxHealth();
        hostile.x = hostileUnit->GetPositionX();
        hostile.y = hostileUnit->GetPositionY();
        hostile.z = hostileUnit->GetPositionZ();
        hostile.positionValid = true;
        hostile.alive = hostileUnit->IsAlive();

        ThreatList const& threatList = hostileUnit->GetThreatManager().getThreatList();
        for (HostileReference* reference : threatList)
        {
            if (!reference)
                continue;
            ThreatEntrySnapshot threat;
            threat.unitGuid = reference->getUnitGuid().GetRawValue();
            threat.threat = reference->getThreat();
            threat.online = reference->isOnline();
            threat.accessible = reference->isAccessable();
            Unit* threatTarget = reference->getTarget();
            threat.meleeReachable = threatTarget &&
                hostileUnit->CanReachWithMeleeAutoAttack(threatTarget);
            hostile.threat.push_back(threat);
        }
        return &hostile;
    };

    auto addHostileReference = [&](HostileReference* reference)
    {
        if (!reference || !reference->getSource() || !reference->getTarget())
            return;

        ThreatManager* source = reference->getSource();
        Unit* hostileUnit = source->getOwner();
        HostileSnapshot* hostile = addHostile(hostileUnit);
        if (!hostile)
            return;

        Guid const targetGuid = reference->getUnitGuid().GetRawValue();
        if (reference->getTarget()->GetObjectGuid().GetRawValue() != targetGuid)
            return;

        // The online threat list was copied by addHostile. This path also
        // preserves references held by the party member when Core has moved
        // them to the hostile's offline threat container.
        for (ThreatEntrySnapshot const& existing : hostile->threat)
            if (existing.unitGuid == targetGuid)
                return;

        ThreatEntrySnapshot threat;
        threat.unitGuid = targetGuid;
        threat.threat = reference->getThreat();
        threat.online = reference->isOnline();
        threat.accessible = reference->isAccessable();
        Unit* threatTarget = reference->getTarget();
        threat.meleeReachable = threatTarget &&
            hostileUnit->CanReachWithMeleeAutoAttack(threatTarget);
        hostile->threat.push_back(threat);
    };

    // Group reads are safe in PACKET_PROCESS_MAP. Resolve only GUIDs on this
    // Map; never dereference a GroupReference Player* from another map.
    for (Group::MemberSlotList::const_iterator itr = group->GetMemberSlots().begin();
         itr != group->GetMemberSlots().end(); ++itr)
    {
        Unit* unit = map->GetUnit(itr->guid);
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
            continue;
        Player* member = static_cast<Player*>(unit);
        if (member->GetMapId() != snapshot.mapId || member->GetInstanceId() != snapshot.instanceId)
            continue;

        PartyMemberSnapshot memberData;
        memberData.guid = member->GetObjectGuid().GetRawValue();
        memberData.classId = member->GetClass();
        memberData.roleId = static_cast<std::uint8_t>(PartyRole::Unknown);
        CombatBotBaseAI* combatAI = dynamic_cast<CombatBotBaseAI*>(member->AI());
        memberData.isPlayerBot = combatAI != nullptr;
        if (combatAI)
        {
            memberData.roleId = static_cast<std::uint8_t>(ToPartyRole(combatAI->GetRole()));
            memberData.resurrectionSpellId = combatAI->m_resurrectionSpell
                ? combatAI->m_resurrectionSpell->Id : 0;
            memberData.inDuel = combatAI->IsInDuel();

            // F53a: expose the Priest Healer's learned direct-heal spell ranks
            // as scalar values. The immutable snapshot never retains SpellEntry
            // pointers; live castability and threat closure remain action-time
            // Core responsibilities.
            if (member->GetClass() == CLASS_PRIEST &&
                combatAI->GetRole() == ROLE_HEALER)
            {
                for (SpellEntry const* spell : combatAI->m_spellListDirectHeal)
                {
                    if (!spell || !spell->Id)
                        continue;

                    std::uint64_t baseHealing = 0;
                    for (std::uint32_t effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
                    {
                        if (spell->Effect[effect] == SPELL_EFFECT_HEAL &&
                            spell->EffectBasePoints[effect] > 0)
                            baseHealing += static_cast<std::uint32_t>(
                                spell->EffectBasePoints[effect]);
                    }

                    if (!baseHealing ||
                        baseHealing > std::numeric_limits<std::uint32_t>::max())
                        continue;

                    DirectHealSpellSnapshot healSpell;
                    healSpell.spellId = spell->Id;
                    healSpell.baseHealing = static_cast<std::uint32_t>(baseHealing);
                    // Reuse Core's cost calculation with charge consumption
                    // disabled; the pure selector then sees the exact current
                    // mana cost, including level and aura modifiers.
                    healSpell.manaCost =
                        Spell::CalculatePowerCost(spell, member, nullptr, nullptr, false);
                    memberData.directHealSpells.push_back(healSpell);
                }

                std::sort(
                    memberData.directHealSpells.begin(),
                    memberData.directHealSpells.end(),
                    [](DirectHealSpellSnapshot const& lhs,
                       DirectHealSpellSnapshot const& rhs)
                    {
                        if (lhs.baseHealing != rhs.baseHealing)
                            return lhs.baseHealing > rhs.baseHealing;
                        return lhs.spellId < rhs.spellId;
                    });
            }
        }
        memberData.resurrectionPending = member->IsRessurectRequested();
        memberData.isMainTank = member->GetObjectGuid() == mainTankGuid;
        memberData.level = member->GetLevel();
        memberData.powerType = static_cast<std::uint8_t>(member->GetPowerType());
        memberData.mapId = member->GetMapId();
        memberData.instanceId = member->GetInstanceId();
        memberData.health = member->GetHealth();
        memberData.maxHealth = member->GetMaxHealth();
        memberData.power = member->GetPower(member->GetPowerType());
        memberData.maxPower = member->GetMaxPower(member->GetPowerType());
        Unit* currentVictim = member->GetVictim();
        memberData.victimGuid = currentVictim
            ? currentVictim->GetObjectGuid().GetRawValue() : 0;
        memberData.victimAttackable = currentVictim &&
            currentVictim->IsInWorld() && currentVictim->GetMap() == map &&
            currentVictim->GetMapId() == snapshot.mapId &&
            currentVictim->GetInstanceId() == snapshot.instanceId &&
            currentVictim->IsAlive() &&
            !(currentVictim->IsPlayer() && currentVictim->ToPlayer()->IsGameMaster()) &&
            !(currentVictim->ToCreature() && currentVictim->ToCreature()->IsInEvadeMode()) &&
            member->CanAttack(currentVictim);
        memberData.selectionGuid = member->GetSelectionGuid().GetRawValue();
        MotionMaster* motionMaster = member->GetMotionMaster();
        MovementGenerator const* currentMovement =
            motionMaster && !motionMaster->empty() ? motionMaster->GetCurrent() : nullptr;
        if (currentMovement &&
            currentMovement->GetMovementGeneratorType() == CHASE_MOTION_TYPE)
        {
            // ChaseMovementGenerator owns a live Core reference. Capture only
            // the current target GUID for snapshot-only no-op decisions.
            ChaseMovementGenerator<Player> const* chase =
                static_cast<ChaseMovementGenerator<Player> const*>(currentMovement);
            Unit* chaseTarget = chase->GetTarget();
            if (chaseTarget && chaseTarget->IsInWorld() &&
                chaseTarget->GetMap() == map &&
                chaseTarget->GetMapId() == snapshot.mapId &&
                chaseTarget->GetInstanceId() == snapshot.instanceId)
                memberData.chaseTargetGuid =
                    chaseTarget->GetObjectGuid().GetRawValue();
        }

        if (currentMovement &&
            currentMovement->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            // FollowMovementGenerator<Player> owns a live Core reference.
            // Copy only its target GUID into the value snapshot.
            FollowMovementGenerator<Player> const* follow =
                static_cast<FollowMovementGenerator<Player> const*>(currentMovement);
            Unit* followTarget = follow->GetTarget();
            if (followTarget && followTarget->IsInWorld() &&
                followTarget->GetMap() == map &&
                followTarget->GetMapId() == snapshot.mapId &&
                followTarget->GetInstanceId() == snapshot.instanceId)
            {
                memberData.followTargetGuid =
                    followTarget->GetObjectGuid().GetRawValue();
                for (std::uint8_t slot = 0; slot < MaxPartyFormationSlots; ++slot)
                {
                    FollowFormationOffset const expected = GetFollowFormationOffset(slot);
                    if (follow->GetFollowDistance() == expected.distance &&
                        follow->GetFollowAngle() == expected.angle)
                    {
                        memberData.followFormationSlot = slot;
                        break;
                    }
                }
            }
        }

        if (currentMovement &&
            currentMovement->GetMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            // PointMovementGenerator<Player> may be legacy or Engine-owned.
            // Copy only scalar correlation; zero action/encounter IDs leave
            // ordinary Core point movement deliberately ineligible.
            PointMovementGenerator<Player> const* point =
                static_cast<PointMovementGenerator<Player> const*>(currentMovement);
            memberData.pointMovementActionId = point->GetEngineActionId();
            memberData.pointMovementEncounterId = point->GetEngineEncounterId();
            memberData.pointMovementPointId = point->GetPointId();
        }
        memberData.x = member->GetPositionX();
        memberData.y = member->GetPositionY();
        memberData.z = member->GetPositionZ();
        memberData.orientation = member->GetOrientation();
        memberData.shapeshiftForm = static_cast<std::uint8_t>(member->GetShapeshiftForm());

        Spell* currentSpell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        std::uint8_t currentSpellType = CURRENT_GENERIC_SPELL;
        if (!currentSpell)
        {
            currentSpell = member->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            currentSpellType = CURRENT_CHANNELED_SPELL;
        }
        if (!currentSpell)
        {
            currentSpell = member->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
            currentSpellType = CURRENT_AUTOREPEAT_SPELL;
        }
        if (currentSpell && currentSpell->m_spellInfo)
        {
            memberData.currentSpellId = currentSpell->m_spellInfo->Id;
            memberData.currentSpellState = currentSpell->getState();
            memberData.currentSpellType = currentSpellType;
            memberData.currentSpellTargetGuid =
                currentSpell->m_targets.getUnitTargetGuid().GetRawValue();
            memberData.currentSpellActionId =
                currentSpell->GetPartyBotTraceActionId();
            memberData.currentSpellEncounterId =
                currentSpell->GetPartyBotTraceEncounterId();
            memberData.currentSpellTraceTargetGuid =
                currentSpell->GetPartyBotTraceTargetGuid();
            memberData.currentSpellEngineAction =
                currentSpell->IsPartyBotTraceEngineAction() &&
                memberData.currentSpellActionId != 0;

            // F54b reservations already account for Engine-owned casts. Only
            // estimate a non-Engine direct heal with an exact same-Group Player
            // target; this avoids double counting the Bot's own reservation.
            if (!memberData.currentSpellEngineAction)
            {
                Unit* healTarget = currentSpell->m_targets.getUnitTarget();
                if (healTarget && healTarget->IsInWorld() &&
                    healTarget->GetTypeId() == TYPEID_PLAYER &&
                    healTarget->GetMap() == map &&
                    healTarget->GetMapId() == snapshot.mapId &&
                    healTarget->GetInstanceId() == snapshot.instanceId &&
                    group->IsMember(healTarget->GetObjectGuid()) &&
                    healTarget->GetObjectGuid().GetRawValue() ==
                        memberData.currentSpellTargetGuid)
                {
                    memberData.currentSpellHealingAmount =
                        EstimateInFlightDirectHeal(currentSpell, healTarget);
                    if (memberData.currentSpellHealingAmount)
                        memberData.currentSpellRemainingCastMs =
                            currentSpell->GetCastedTime();
                }
            }
        }

        CaptureHealingForecast(member, map, memberData);
        memberData.online = member->IsInWorld();
        memberData.alive = member->IsAlive();
        switch (member->GetDeathState())
        {
            case ALIVE:
                memberData.deathState = PartyMemberDeathState::Alive;
                break;
            case JUST_DIED:
                memberData.deathState = PartyMemberDeathState::Dying;
                break;
            case CORPSE:
                memberData.deathState = PartyMemberDeathState::Corpse;
                break;
            case DEAD:
                memberData.deathState = PartyMemberDeathState::ReleasedGhost;
                break;
            case JUST_ALIVED:
                memberData.deathState = PartyMemberDeathState::Resurrecting;
                break;
            case CORPSE_FALLING:
                memberData.deathState = PartyMemberDeathState::CorpseFalling;
                break;
            default:
                memberData.deathState = PartyMemberDeathState::Unknown;
                break;
        }
        memberData.inCombat = member->IsInCombat();
        memberData.mounted = member->IsMounted();
        memberData.taxiFlying = member->IsTaxiFlying();
        snapshot.members.push_back(memberData);

        // Record the Player's primary controlled pet as a distinct threat actor.
        // Keep its GUID as the actual Core threat owner; ownerGuid proves party
        // membership and is used only for coordinator reservation cleanup.
        if (Pet* pet = member->GetPet()) {
            if (pet->IsInWorld() && pet->GetMap() == map &&
                pet->GetMapId() == snapshot.mapId &&
                pet->GetInstanceId() == snapshot.instanceId &&
                pet->GetOwnerGuid() == member->GetObjectGuid()) {
                PartyCompanionSnapshot companion;
                companion.guid = pet->GetObjectGuid().GetRawValue();
                companion.ownerGuid = memberData.guid;
                companion.mapId = pet->GetMapId();
                companion.instanceId = pet->GetInstanceId();
                companion.health = pet->GetHealth();
                companion.maxHealth = pet->GetMaxHealth();
                companion.victimGuid = pet->GetVictim()
                    ? pet->GetVictim()->GetObjectGuid().GetRawValue() : 0;
                companion.x = pet->GetPositionX();
                companion.y = pet->GetPositionY();
                companion.z = pet->GetPositionZ();
                companion.online = pet->IsInWorld();
                companion.alive = pet->IsAlive();
                companion.inCombat = pet->IsInCombat();
                snapshot.companions.push_back(companion);

                // Discover hostile references held by or targeting the pet too.
                addHostile(pet->GetVictim());
                for (Unit* attacker : pet->GetAttackers())
                    addHostile(attacker);
                for (HostileReference* reference = pet->GetHostileRefManager().getFirst();
                     reference; reference = reference->next())
                    addHostileReference(reference);
            }
        }

        // Include current attackers and targets, plus every hostile that
        // holds a reference to this member. Healing/assist threat traverses
        // HostileRefManager, so the current victim alone is not a complete set.
        addHostile(member->GetVictim());
        for (Unit* attacker : member->GetAttackers())
            addHostile(attacker);
        for (HostileReference* reference = member->GetHostileRefManager().getFirst();
             reference; reference = reference->next())
            addHostileReference(reference);
    }

    if (snapshot.members.empty())
        return false;

    snapshot.hostiles.reserve(hostiles.size());
    for (auto& entry : hostiles)
        snapshot.hostiles.push_back(std::move(entry.second));
    return true;
}

} // namespace PartyBotEngine
