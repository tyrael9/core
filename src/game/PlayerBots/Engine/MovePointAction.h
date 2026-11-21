#ifndef MANGOS_PARTYBOT_MOVE_POINT_ACTION_H
#define MANGOS_PARTYBOT_MOVE_POINT_ACTION_H

#include "CoreAction.h"
#include "PartySnapshot.h"

#include "Movement/MotionMaster.h"
#include "Player.h"
#include "Spell.h"

#include <cmath>
#include <cstdint>

namespace PartyBotEngine
{
// Start a bounded, actor-only point move through Core's Engine-correlated
// MotionMaster entry point. A successful Execute means the request was
// accepted; only the later typed PointMovementTerminalEvent can prove arrival.
class MovePointAction final : public CoreAction
{
public:
    explicit MovePointAction(float arrivalTolerance = 1.5f,
                             float speed = 0.0f,
                             float finalOrientation = -10.0f)
        : m_arrivalTolerance(arrivalTolerance), m_speed(speed),
          m_finalOrientation(finalOrientation) {}

    std::string const& GetName() const override { return m_name; }

    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override
    {
        // A point move has no direct damage/heal effect, but its path can enter
        // hostile aggro range. Do not mislabel that positional risk as NoThreat.
        ThreatImpactEstimate estimate;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "move_point_path_may_enter_hostile_aggro_radius";
        return estimate;
    }

    bool IsPossible(DecisionContext const& context,
                    ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        if (!context.bot.online || !context.bot.alive ||
            context.bot.inCombat || context.bot.victimGuid ||
            context.bot.currentSpellId || candidate.targetGuid ||
            !candidate.hasDestination ||
            !std::isfinite(context.bot.x) ||
            !std::isfinite(context.bot.y) ||
            !std::isfinite(context.bot.z) ||
            !std::isfinite(candidate.destinationX) ||
            !std::isfinite(candidate.destinationY) ||
            !std::isfinite(candidate.destinationZ) ||
            !IsValidConfiguration())
        {
            reason = "move_point_invalid_actor_or_destination_snapshot";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context,
                  ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (!candidate.hasDestination ||
            !std::isfinite(candidate.destinationX) ||
            !std::isfinite(candidate.destinationY) ||
            !std::isfinite(candidate.destinationZ) ||
            !IsValidConfiguration())
        {
            reason = "move_point_destination_unavailable";
            return false;
        }

        float const dx = context.bot.x - candidate.destinationX;
        float const dy = context.bot.y - candidate.destinationY;
        float const dz = context.bot.z - candidate.destinationZ;
        if (dx * dx + dy * dy + dz * dz <=
            m_arrivalTolerance * m_arrivalTolerance)
        {
            reason = "move_point_destination_already_reached";
            return false;
        }
        return true;
    }

    static std::uint32_t PointIdForAction(ActionId actionId)
    {
        std::uint32_t pointId = static_cast<std::uint32_t>(
            actionId & static_cast<ActionId>(0xffffffffu));
        return pointId ? pointId : 1u;
    }

protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const override
    {
        return CheckLiveState(context, candidate, resolved);
    }

    ActionExecutionResult ExecuteCore(
        DecisionContext& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) override
    {
        ActionExecutionResult result =
            CheckLiveState(context, candidate, resolved);
        if (result.result != ActionResult::Success)
            return result;

        Player* actor = resolved.actor;
        MotionMaster* motion = actor->GetMotionMaster();
        ObjectGuid const selectionBefore = actor->GetSelectionGuid();
        ObjectGuid const targetGuidBefore = actor->GetTargetGuid();
        Unit* const victimBefore = actor->GetVictim();
        bool const combatBefore = actor->IsInCombat();
        std::uint32_t const healthBefore = actor->GetHealth();
        std::uint32_t const powerBefore = actor->GetPower(actor->GetPowerType());
        std::uint32_t const pointId = PointIdForAction(candidate.actionId);
        std::uint64_t const encounterId = context.coordinator.GetEncounterId();

        bool const accepted = motion->MovePointForEngine(
            pointId, candidate.destinationX, candidate.destinationY,
            candidate.destinationZ, MOVE_NONE, candidate.actionId, encounterId,
            m_arrivalTolerance, m_speed, m_finalOrientation);

        bool const statePreserved =
            actor->GetSelectionGuid() == selectionBefore &&
            actor->GetTargetGuid() == targetGuidBefore &&
            actor->GetVictim() == victimBefore &&
            actor->IsInCombat() == combatBefore &&
            actor->GetHealth() == healthBefore &&
            actor->GetPower(actor->GetPowerType()) == powerBefore;

        bool const pointMoveActive =
            motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
        bool const started = accepted && pointMoveActive && statePreserved;
        result.result = started ? ActionResult::Success : ActionResult::Failed;
        result.reason = started
            ? "move_point_core_request_accepted"
            : "move_point_core_request_or_postcondition_failed";
        return result;
    }

private:
    bool IsValidConfiguration() const
    {
        return std::isfinite(m_arrivalTolerance) && m_arrivalTolerance > 0.0f &&
            std::isfinite(m_speed) && m_speed >= 0.0f &&
            std::isfinite(m_finalOrientation);
    }

    static bool HasNonMeleeSpell(Player* actor)
    {
        if (!actor)
            return true;

        for (std::uint32_t spellIndex = CURRENT_FIRST_NON_MELEE_SPELL;
             spellIndex < CURRENT_MAX_SPELL; ++spellIndex)
        {
            if (actor->GetCurrentSpell(CurrentSpellTypes(spellIndex)))
                return true;
        }
        return false;
    }

    ActionExecutionResult CheckLiveState(
        DecisionContext const& context, ActionCandidate const& candidate,
        CoreActionResolution const& resolved) const
    {
        ActionExecutionResult result;
        Player* actor = resolved.actor;
        if (!actor || !resolved.map || resolved.target || candidate.targetGuid ||
            !candidate.actionId || !candidate.hasDestination ||
            context.bot.guid != actor->GetObjectGuid().GetRawValue() ||
            !context.bot.online || !context.bot.alive ||
            actor->GetMap() != resolved.map ||
            actor->GetMapId() != context.snapshot.mapId ||
            actor->GetInstanceId() != context.snapshot.instanceId ||
            !actor->IsInWorld() || !actor->IsAlive() ||
            !IsValidConfiguration() ||
            !std::isfinite(candidate.destinationX) ||
            !std::isfinite(candidate.destinationY) ||
            !std::isfinite(candidate.destinationZ))
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_point_live_actor_or_destination_invalid";
            return result;
        }

        std::uint64_t const encounterId = context.coordinator.GetEncounterId();
        if (!context.coordinator.HasActiveEncounter() || !encounterId ||
            context.coordinator.GetEncounterMapId() != context.snapshot.mapId ||
            context.coordinator.GetEncounterInstanceId() != context.snapshot.instanceId)
        {
            result.result = ActionResult::Impossible;
            result.reason = "move_point_encounter_scope_invalid";
            return result;
        }

        if (actor->GetVictim() || actor->IsInCombat())
        {
            result.result = ActionResult::Useless;
            result.reason = "move_point_actor_in_combat";
            return result;
        }

        if (HasNonMeleeSpell(actor))
        {
            result.result = ActionResult::Useless;
            result.reason = "move_point_actor_casting";
            return result;
        }

        if (actor->HasUnitState(
                UNIT_STATE_LOST_CONTROL | UNIT_STATE_CAN_NOT_MOVE |
                UNIT_STATE_POSSESSED))
        {
            result.result = ActionResult::Useless;
            result.reason = "move_point_actor_cannot_move";
            return result;
        }

        MotionMaster* motion = actor->GetMotionMaster();
        if (!motion || !actor->IsStopped() ||
            !motion->IsUsingIdleOrDefaultMovement())
        {
            result.result = ActionResult::Useless;
            result.reason = "move_point_would_replace_unrelated_movement";
            return result;
        }

        float const dx = actor->GetPositionX() - candidate.destinationX;
        float const dy = actor->GetPositionY() - candidate.destinationY;
        float const dz = actor->GetPositionZ() - candidate.destinationZ;
        if (dx * dx + dy * dy + dz * dz <=
            m_arrivalTolerance * m_arrivalTolerance)
        {
            result.result = ActionResult::Useless;
            result.reason = "move_point_destination_already_reached";
            return result;
        }

        result.result = ActionResult::Success;
        result.reason = "move_point_live_preflight_passed";
        return result;
    }

    float m_arrivalTolerance;
    float m_speed;
    float m_finalOrientation;
    std::string m_name = "core_move_point";
};
} // namespace PartyBotEngine

#endif
