#ifndef MANGOS_PARTYBOT_STOP_POINT_MOVEMENT_ACTION_H
#define MANGOS_PARTYBOT_STOP_POINT_MOVEMENT_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "MotionMaster.h"
#include "MovementGenerator.h"
#include "PointMovementGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "Unit.h"

namespace PartyBotEngine
{
// Cancel only the exact Engine-owned point move copied into the value snapshot.
// Ordinary/legacy PointMovementGenerator instances have no Engine correlation
// and are never eligible for this action.
class StopPointMovementAction final : public CoreAction
{
public:
    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "stop_point_movement_only_expires_engine_generator";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        if (!context.bot.online || !context.bot.alive || candidate.targetGuid ||
            !context.bot.pointMovementActionId ||
            !context.bot.pointMovementEncounterId ||
            !context.bot.pointMovementPointId)
        {
            reason = "stop_point_movement_missing_engine_snapshot";
            return false;
        }

        if (!context.coordinator.HasActiveEncounter() ||
            context.coordinator.GetEncounterId() !=
                context.bot.pointMovementEncounterId)
        {
            reason = "stop_point_movement_snapshot_encounter_inactive";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (candidate.targetGuid || !context.bot.pointMovementActionId ||
            !context.bot.pointMovementEncounterId ||
            !context.bot.pointMovementPointId)
        {
            reason = "stop_point_movement_no_engine_owned_move";
            return false;
        }
        return true;
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const override
    {
        return CheckLiveMovement(context, candidate, resolved);
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result =
            CheckLiveMovement(context, candidate, resolved);
        if (result.result != ActionResult::Success)
            return result;

        Player* actor = resolved.actor;
        MotionMaster* motion = actor->GetMotionMaster();
        PointMovementGenerator<Player> const* point =
            static_cast<PointMovementGenerator<Player> const*>(motion->GetCurrent());
        ActionId const expectedActionId = point->GetEngineActionId();
        std::uint64_t const expectedEncounterId = point->GetEngineEncounterId();
        std::uint32_t const expectedPointId = point->GetPointId();

        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetBefore = actor->GetTargetGuid();
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        uint32 const healthBefore = actor->GetHealth();
        Powers const powerType = actor->GetPowerType();
        uint32 const powerBefore = actor->GetPower(powerType);
        Spell* currentSpellsBefore[CURRENT_MAX_SPELL] = {};
        for (uint32 spellIndex = 0; spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
            currentSpellsBefore[spellIndex] =
                actor->GetCurrentSpell(CurrentSpellTypes(spellIndex));

        // Core owns expiry, deferred deletion during MotionMaster updates, and
        // the correlated Interrupted terminal emitted by Finalize.
        motion->MovementExpired(false);

        bool expectedGeneratorStillActive = false;
        for (MotionMaster::const_iterator it = motion->begin();
             it != motion->end(); ++it)
        {
            if ((*it)->GetMovementGeneratorType() != POINT_MOTION_TYPE)
                continue;
            PointMovementGenerator<Player> const* activePoint =
                static_cast<PointMovementGenerator<Player> const*>(*it);
            if (activePoint->GetEngineActionId() == expectedActionId &&
                activePoint->GetEngineEncounterId() == expectedEncounterId &&
                activePoint->GetPointId() == expectedPointId)
            {
                expectedGeneratorStillActive = true;
                break;
            }
        }

        bool spellsPreserved = true;
        for (uint32 spellIndex = 0; spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)) !=
                currentSpellsBefore[spellIndex])
            {
                spellsPreserved = false;
                break;
            }
        }

        bool const actorStatePreserved =
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetBefore &&
            actor->GetVictim() == victimBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->GetHealth() == healthBefore &&
            actor->GetPower(powerType) == powerBefore &&
            spellsPreserved;

        bool const stopped = !expectedGeneratorStillActive && actorStatePreserved;
        result.result = stopped ? ActionResult::Success : ActionResult::Failed;
        result.reason = stopped
            ? "stop_point_movement_expired_matching_engine_generator"
            : "stop_point_movement_expiry_postcondition_failed";
        return result;
    }

private:
    static ActionExecutionResult CheckLiveMovement(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved)
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            context.bot.guid != actor->GetObjectGuid().GetRawValue() ||
            !context.bot.online || !context.bot.alive ||
            actor->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !context.bot.pointMovementActionId ||
            !context.bot.pointMovementEncounterId ||
            !context.bot.pointMovementPointId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_point_movement_actor_or_snapshot_invalid";
            return result;
        }

        std::uint64_t const encounterId = context.coordinator.GetEncounterId();
        if (!context.coordinator.HasActiveEncounter() || !encounterId ||
            encounterId != context.bot.pointMovementEncounterId ||
            context.coordinator.GetEncounterMapId() != context.snapshot.mapId ||
            context.coordinator.GetEncounterInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_point_movement_encounter_scope_mismatch";
            return result;
        }

        MotionMaster* motion = actor->GetMotionMaster();
        MovementGenerator* current = motion ? motion->GetCurrent() : nullptr;
        if (!current || current->GetMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            result.result = ActionResult::Useless;
            result.reason = "stop_point_movement_no_current_point_generator";
            return result;
        }

        PointMovementGenerator<Player> const* point =
            static_cast<PointMovementGenerator<Player> const*>(current);
        if (!point->GetEngineActionId() || !point->GetEngineEncounterId() ||
            point->GetEngineActionId() != context.bot.pointMovementActionId ||
            point->GetEngineEncounterId() != context.bot.pointMovementEncounterId ||
            point->GetPointId() != context.bot.pointMovementPointId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "stop_point_movement_live_generator_identity_mismatch";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "stop_point_movement_live_generator_matches_snapshot";
        return result;
    }

    std::string m_name = "core_stop_point_movement";
};
} // namespace PartyBotEngine

#endif
