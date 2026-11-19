#include "PartyBotEngine.h"
#include "ThreatBudgetPolicy.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <limits>
#include <utility>
namespace PartyBotEngine {
void PartyBotEngine::AddStrategy(std::shared_ptr<Strategy> strategy) {
    if (strategy) m_strategies.push_back(std::move(strategy));
}
bool PartyBotEngine::RegisterAction(std::shared_ptr<Action> action) {
    if (!action || action->GetName().empty()) return false;
    std::string const name = action->GetName();
    return m_actions.insert(std::make_pair(name, std::move(action))).second;
}
void PartyBotEngine::AddMultiplier(std::shared_ptr<Multiplier> multiplier) {
    if (multiplier) m_multipliers.push_back(std::move(multiplier));
}
bool PartyBotEngine::InvalidateValue(std::string const& name) const {
    return m_valueCache.Invalidate(name);
}
void PartyBotEngine::InvalidateAllValues() const {
    m_valueCache.InvalidateAll();
}
bool PartyBotEngine::InvalidateTrigger(std::string const& strategyName,
                                       std::string const& triggerName) const {
    if (strategyName.empty() || triggerName.empty()) return false;
    return m_triggerScheduler.Invalidate(strategyName + "/" + triggerName);
}
void PartyBotEngine::InvalidateAllTriggers() const {
    m_triggerScheduler.InvalidateAll();
}
std::shared_ptr<Action> PartyBotEngine::FindAction(std::string const& name) const {
    auto itr = m_actions.find(name);
    return itr == m_actions.end() ? std::shared_ptr<Action>() : itr->second;
}
DecisionReport PartyBotEngine::Evaluate(DecisionContext const& context) const {
    DecisionContext evaluationContext(
        context.snapshot, context.bot, context.coordinator, context.nowMs,
        context.reservationTrace,
        context.valueCache ? context.valueCache : &m_valueCache);
    DecisionReport report;
    report.snapshotEpoch = evaluationContext.snapshot.epoch;
    for (auto const& strategy : m_strategies) {
        if (!strategy) continue;
        for (auto const& trigger : strategy->GetTriggers()) {
            if (!trigger) continue;
            std::string const triggerKey = strategy->GetName() + "/" + trigger->GetName();
            if (!m_triggerScheduler.ShouldEvaluate(
                    triggerKey, trigger->GetCheckIntervalMs(), evaluationContext.nowMs))
                continue;
            if (!trigger->IsActive(evaluationContext)) continue;
            std::vector<ActionCandidate> candidates;
            trigger->CollectCandidates(evaluationContext, candidates);
            for (auto const& candidate : candidates) {
                CandidateEvaluation evaluation;
                evaluation.candidate = candidate;
                if (evaluation.candidate.strategyName.empty())
                    evaluation.candidate.strategyName = strategy->GetName();
                ActionCandidate const& evaluatedCandidate = evaluation.candidate;
                std::shared_ptr<Action> action = FindAction(evaluatedCandidate.actionName);
                if (!action) {
                    evaluation.status = CandidateStatus::RejectedMissingAction;
                    evaluation.reason = "action_not_registered";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                if (!std::isfinite(candidate.basePriority)) {
                    evaluation.status = CandidateStatus::RejectedInvalidPriority;
                    evaluation.reason = "non_finite_base_priority";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                if (!action->IsPossible(evaluationContext, evaluatedCandidate, evaluation.reason)) {
                    evaluation.status = CandidateStatus::RejectedImpossible;
                    if (evaluation.reason.empty()) evaluation.reason = "action_not_possible";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                evaluation.reason.clear();
                if (!action->IsUseful(evaluationContext, evaluatedCandidate, evaluation.reason)) {
                    evaluation.status = CandidateStatus::RejectedUseless;
                    if (evaluation.reason.empty()) evaluation.reason = "action_not_useful";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }

                evaluation.threatImpact = action->EstimateThreatImpact(evaluationContext, evaluatedCandidate);
                if (!IsWellFormedThreatImpactEstimate(evaluation.threatImpact) ||
                    evaluation.threatImpact.snapshotEpoch != evaluationContext.snapshot.epoch) {
                    evaluation.status = CandidateStatus::RejectedThreatUnknown;
                    evaluation.reason = "threat_impact_estimate_invalid_or_stale";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }

                float priority = candidate.basePriority;
                bool vetoed = false;
                for (auto const& multiplier : m_multipliers) {
                    if (!multiplier) continue;
                    float const factor = multiplier->GetValue(evaluationContext, evaluatedCandidate);
                    if (!std::isfinite(factor) || factor <= 0.0f) {
                        evaluation.status = CandidateStatus::RejectedMultiplier;
                        evaluation.reason = multiplier->GetName();
                        if (evaluation.reason.empty()) evaluation.reason = "multiplier_veto";
                        vetoed = true;
                        break;
                    }
                    priority *= factor;
                }
                if (vetoed) {
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                if (!std::isfinite(priority)) {
                    evaluation.status = CandidateStatus::RejectedInvalidPriority;
                    evaluation.reason = "non_finite_final_priority";
                    report.candidates.push_back(std::move(evaluation));
                    continue;
                }
                evaluation.candidate.finalPriority = priority;
                evaluation.status = CandidateStatus::NotSelected;
                evaluation.reason.clear();
                report.candidates.push_back(std::move(evaluation));
            }
        }
    }
    std::vector<std::size_t> eligible;
    for (std::size_t i = 0; i < report.candidates.size(); ++i)
        if (report.candidates[i].status == CandidateStatus::NotSelected) eligible.push_back(i);
    std::stable_sort(eligible.begin(), eligible.end(), [&report](std::size_t a, std::size_t b) {
        return report.candidates[a].candidate.finalPriority > report.candidates[b].candidate.finalPriority;
    });
    std::set<ActionChannel> occupiedChannels;
    ThreatImpactTotals selectedThreatInReport;
    for (std::size_t index : eligible) {
        CandidateEvaluation& evaluation = report.candidates[index];
        if (occupiedChannels.find(evaluation.candidate.channel) != occupiedChannels.end()) {
            evaluation.reason = "lower_priority_same_channel";
            continue;
        }

        ThreatImpactBudgetResult const threatBudget = AssessThreatImpactBudget(
            evaluationContext.snapshot, evaluation.threatImpact, evaluationContext.coordinator,
            evaluationContext.nowMs, 250, &selectedThreatInReport);
        if (threatBudget.status == ThreatImpactBudgetStatus::Unknown) {
            evaluation.status = CandidateStatus::RejectedThreatUnknown;
            evaluation.reason = threatBudget.reason;
            continue;
        }
        if (threatBudget.status == ThreatImpactBudgetStatus::Exceeded) {
            evaluation.status = CandidateStatus::RejectedThreatBudget;
            evaluation.reason = threatBudget.reason;
            continue;
        }

        occupiedChannels.insert(evaluation.candidate.channel);
        evaluation.status = CandidateStatus::Selected;
        report.selectedIndices.push_back(index);
    }
    return report;
}
void PartyBotEngine::ExecuteSelected(DecisionContext& context, DecisionReport& report) const {
    DecisionContext executionContext(
        context.snapshot, context.bot, context.coordinator, context.nowMs,
        context.reservationTrace,
        context.valueCache ? context.valueCache : &m_valueCache);
    auto recordReservation = [&executionContext](ReservationTraceEvent const& event) {
        if (executionContext.reservationTrace)
            executionContext.reservationTrace->RecordReservationEvent(executionContext, event);
    };

    // Materialize selected decisions into a bounded queue. The queue contains
    // indices only; all Core-facing validity, range, LOS, resource and threat
    // checks remain immediately before dispatch below.
    ActionQueue queue;
    for (std::size_t index : report.selectedIndices) {
        if (index >= report.candidates.size())
            continue;
        CandidateEvaluation const& queued = report.candidates[index];
        if (queued.status != CandidateStatus::Selected)
            continue;
        if (!std::isfinite(queued.candidate.finalPriority) ||
            !queue.Push(index, queued.candidate.finalPriority)) {
            CandidateEvaluation& rejected = report.candidates[index];
            rejected.status = CandidateStatus::ExecutionFailed;
            rejected.actionResult = ActionResult::Failed;
            rejected.reason = "action_queue_capacity_or_priority_rejected";
        }
    }

    ActionQueue::Entry queuedEntry;
    std::size_t processed = 0;
    std::size_t const maxActionsPerDispatch = 8;
    while (processed < maxActionsPerDispatch && queue.PopNext(queuedEntry)) {
        ++processed;
        std::size_t const index = queuedEntry.reportIndex;
        if (index >= report.candidates.size()) continue;
        CandidateEvaluation& evaluation = report.candidates[index];
        if (evaluation.status != CandidateStatus::Selected) continue;
        std::shared_ptr<Action> action = FindAction(evaluation.candidate.actionName);
        if (!action) {
            evaluation.status = CandidateStatus::ExecutionFailed;
            evaluation.actionResult = ActionResult::Failed;
            evaluation.reason = "action_unregistered_before_execution";
            continue;
        }
        // Recheck the action's exact estimate against current coordinator reservations
        // immediately before execution. New Engine mode never falls back to Legacy.
        ThreatImpactBudgetResult const threatBudget = AssessThreatImpactBudget(
            executionContext.snapshot, evaluation.threatImpact, executionContext.coordinator,
            executionContext.nowMs, 250, nullptr);
        if (threatBudget.status != ThreatImpactBudgetStatus::Safe) {
            if (threatBudget.status == ThreatImpactBudgetStatus::Exceeded) {
                ReservationTraceEvent event;
                event.stage = ReservationTraceStage::BudgetVeto;
                event.ownerGuid = executionContext.bot.guid;
                event.threatOwnerGuid = threatBudget.threatOwnerGuid;
                event.hostileGuid = threatBudget.hostileGuid;
                event.threatPoints = static_cast<float>(threatBudget.candidateThreat);
                event.rawHeadroom = threatBudget.rawHeadroom;
                event.alreadyReserved = threatBudget.alreadyReserved;
                event.reason = threatBudget.reason.c_str();
                recordReservation(event);
            }
            evaluation.status = CandidateStatus::ExecutionFailed;
            evaluation.actionResult = ActionResult::Failed;
            evaluation.reason = threatBudget.reason;
            continue;
        }

        ThreatActionId actionId = 0;
        if (evaluation.threatImpact.completeness == ThreatImpactCompleteness::Complete) {
            ThreatImpactTotals totals;
            for (ThreatImpactEntry const& impact : evaluation.threatImpact.impacts) {
                if (impact.threatPoints > 0.0f)
                    totals[ThreatImpactKey(impact.hostileGuid, impact.threatOwnerGuid)] +=
                        static_cast<double>(impact.threatPoints);
            }

            if (!totals.empty()) {
                actionId = executionContext.coordinator.NextThreatActionId();
                if (!actionId) {
                    ReservationTraceEvent event;
                    event.stage = ReservationTraceStage::Result;
                    event.ownerGuid = executionContext.bot.guid;
                    event.accepted = false;
                    event.reason = "threat_action_id_unavailable";
                    recordReservation(event);
                    evaluation.status = CandidateStatus::ExecutionFailed;
                    evaluation.actionResult = ActionResult::Failed;
                    evaluation.reason = "threat_action_id_unavailable";
                    continue;
                }

                std::uint32_t const expiresAtMs =
                    executionContext.nowMs + evaluation.threatImpact.reservationDurationMs;
                bool reservationAccepted = true;
                std::uint32_t reservedRows = 0;
                for (auto const& entry : totals) {
                    ThreatReservation reservation;
                    reservation.ownerGuid = executionContext.bot.guid;
                    reservation.threatOwnerGuid = entry.first.second;
                    reservation.hostileGuid = entry.first.first;
                    reservation.actionId = actionId;
                    reservation.threatPoints = std::isfinite(entry.second) &&
                        entry.second <= static_cast<double>(std::numeric_limits<float>::max())
                        ? static_cast<float>(entry.second) : 0.0f;
                    reservation.expiresAtMs = expiresAtMs;

                    ReservationTraceEvent attempt;
                    attempt.stage = ReservationTraceStage::Attempt;
                    attempt.actionId = actionId;
                    attempt.ownerGuid = reservation.ownerGuid;
                    attempt.threatOwnerGuid = reservation.threatOwnerGuid;
                    attempt.hostileGuid = reservation.hostileGuid;
                    attempt.threatPoints = reservation.threatPoints;
                    attempt.expiresAtMs = expiresAtMs;
                    attempt.reason = "execute_selected";
                    recordReservation(attempt);

                    bool const validThreat = std::isfinite(entry.second) &&
                        entry.second > 0.0 &&
                        entry.second <= static_cast<double>(std::numeric_limits<float>::max());
                    bool const accepted = validThreat &&
                        executionContext.coordinator.TryReserveThreatImpact(reservation, executionContext.nowMs);

                    ReservationTraceEvent result;
                    result.stage = ReservationTraceStage::Result;
                    result.actionId = actionId;
                    result.ownerGuid = reservation.ownerGuid;
                    result.threatOwnerGuid = reservation.threatOwnerGuid;
                    result.hostileGuid = reservation.hostileGuid;
                    result.threatPoints = reservation.threatPoints;
                    result.expiresAtMs = expiresAtMs;
                    result.accepted = accepted;
                    result.reason = !validThreat ? "invalid_threat_reservation_row" :
                        (accepted ? "coordinator_accepted" : "coordinator_rejected");
                    recordReservation(result);

                    if (!accepted) {
                        reservationAccepted = false;
                        break;
                    }
                    ++reservedRows;
                }

                if (!reservationAccepted) {
                    executionContext.coordinator.ReleaseThreatAction(actionId);
                    ReservationTraceEvent release;
                    release.stage = ReservationTraceStage::Release;
                    release.actionId = actionId;
                    release.ownerGuid = executionContext.bot.guid;
                    release.reservationRows = reservedRows;
                    release.reason = "partial_acquire_rollback";
                    recordReservation(release);
                    evaluation.status = CandidateStatus::ExecutionFailed;
                    evaluation.actionResult = ActionResult::Failed;
                    evaluation.reason = "threat_reservation_failed";
                    continue;
                }
            }
        }

        evaluation.threatActionId = actionId;
        evaluation.candidate.threatActionId = actionId;
        evaluation.actionResult = action->Execute(executionContext, evaluation.candidate);
        if (evaluation.actionResult == ActionResult::Success)
            evaluation.status = CandidateStatus::Executed;
        else {
            if (actionId) {
                executionContext.coordinator.ReleaseThreatAction(actionId);
                ReservationTraceEvent release;
                release.stage = ReservationTraceStage::Release;
                release.actionId = actionId;
                release.ownerGuid = executionContext.bot.guid;
                release.reason = "action_execution_failed";
                release.reservationRows = static_cast<std::uint32_t>(
                    evaluation.threatImpact.impacts.size());
                recordReservation(release);
            }
            evaluation.status = CandidateStatus::ExecutionFailed;
            if (evaluation.reason.empty()) evaluation.reason = "action_execution_not_successful";
        }
    }
}
}
