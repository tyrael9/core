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
bool PartyBotEngine::AddActionResultListener(
    std::shared_ptr<ActionResultListener> listener) {
    if (!listener || m_actionResultListeners.size() >= 32)
        return false;
    for (auto const& registered : m_actionResultListeners)
        if (registered == listener)
            return false;
    m_actionResultListeners.push_back(std::move(listener));
    return true;
}
bool PartyBotEngine::AddCoreSpellTerminalListener(
    std::shared_ptr<CoreSpellTerminalListener> listener) {
    if (!listener || m_coreSpellTerminalListeners.size() >= 32)
        return false;
    for (auto const& registered : m_coreSpellTerminalListeners)
        if (registered == listener)
            return false;
    m_coreSpellTerminalListeners.push_back(std::move(listener));
    return true;
}

void PartyBotEngine::NotifyActionResult(
    DecisionContext const& context, CandidateEvaluation const& evaluation,
    ActionId actionId, bool attempted, ActionResult result,
    char const* reason) const {
    if (m_actionResultListeners.empty())
        return;

    ActionResultEvent event;
    event.candidate = evaluation.candidate;
    event.candidate.actionId = actionId;
    event.actionId = actionId;
    event.result = result;
    event.attempted = attempted;
    event.snapshotEpoch = context.snapshot.epoch;
    event.occurredAtMs = context.nowMs;
    event.reason = reason ? reason : "none";

    // Snapshot listeners before invoking callbacks. A listener may register
    // another listener without invalidating this dispatch's iteration.
    std::vector<std::shared_ptr<ActionResultListener>> const listeners =
        m_actionResultListeners;
    for (auto const& listener : listeners)
        if (listener)
            listener->OnActionResult(context, event);
}

void PartyBotEngine::RememberCoreSpellTerminal(
    CoreSpellTerminalEvent const& event) const {
    if (!event.actionId || !event.encounterId)
        return;
    for (CoreSpellTerminalEvent const& existing : m_recentCoreSpellTerminals)
        if (existing.actionId == event.actionId &&
            existing.encounterId == event.encounterId)
            return;
    static std::size_t const Capacity = 128;
    if (m_recentCoreSpellTerminals.size() >= Capacity)
        m_recentCoreSpellTerminals.erase(m_recentCoreSpellTerminals.begin());
    m_recentCoreSpellTerminals.push_back(event);
}

bool PartyBotEngine::ReleaseContinuation(
    ActionContinuationQueue::Entry const& entry, std::uint32_t nowMs) const {
    if (ActionContinuationQueue::IsExpired(nowMs, entry.expiresAtMs))
        return false;
    std::uint32_t const remainingMs = entry.expiresAtMs - nowMs;
    if (static_cast<std::int32_t>(remainingMs) <= 0)
        return false;
    std::shared_ptr<Action> action = FindAction(entry.candidate.actionName);
    if (!action)
        return false;
    std::uint32_t const actionLifetimeMs = action->GetPendingLifetimeMs();
    if (!actionLifetimeMs || actionLifetimeMs > PendingActionQueue::MaxLifetimeMs)
        return false;
    std::uint32_t const boundedLifetimeMs = std::min(remainingMs, actionLifetimeMs);
    PendingActionEnqueueResult const result = m_pendingActionQueue.Enqueue(
        entry.candidate, nowMs, boundedLifetimeMs, entry.context);
    return result == PendingActionEnqueueResult::Queued ||
        result == PendingActionEnqueueResult::UpdatedExisting;
}

bool PartyBotEngine::ReleaseReadyActionDependencies(
    std::uint32_t nowMs, ActionCandidate const* registeringCandidate) const {
    std::vector<ActionContinuationQueue::Entry> ready;
    m_actionContinuationQueue.CollectReady(ready);
    bool registeringCandidateReleased = false;
    auto sameIdentity = [](ActionCandidate const& left, ActionCandidate const& right) {
        return left.actionName == right.actionName &&
            left.targetGuid == right.targetGuid &&
            left.spellId == right.spellId &&
            left.channel == right.channel &&
            left.hasDestination == right.hasDestination &&
            (!left.hasDestination ||
                (left.destinationX == right.destinationX &&
                 left.destinationY == right.destinationY &&
                 left.destinationZ == right.destinationZ));
    };
    for (ActionContinuationQueue::Entry const& entry : ready) {
        bool const released = ReleaseContinuation(entry, nowMs);
        if (released && registeringCandidate &&
            sameIdentity(entry.candidate, *registeringCandidate))
            registeringCandidateReleased = true;
    }
    return registeringCandidateReleased;
}

ActionContinuationEnqueueResult PartyBotEngine::QueueActionDependency(
    DecisionContext const& context, ActionDependencyRequest const& request,
    ActionCandidate const& continuationCandidate) const {
    if (!context.coordinator.HasActiveEncounter() ||
        !context.coordinator.GetEncounterId() ||
        context.snapshot.groupId != context.coordinator.GetEncounterGroupId() ||
        context.snapshot.mapId != context.coordinator.GetEncounterMapId() ||
        context.snapshot.instanceId != context.coordinator.GetEncounterInstanceId())
        return ActionContinuationEnqueueResult::ParentEncounterInactive;
    if (!context.snapshot.groupId || !context.bot.guid ||
        context.snapshot.mapId != context.bot.mapId ||
        context.snapshot.instanceId != context.bot.instanceId ||
        request.prerequisites.empty() ||
        request.prerequisites.size() > ActionContinuationQueue::MaxPrerequisites ||
        !request.lifetimeMs ||
        request.lifetimeMs > ActionContinuationQueue::MaxLifetimeMs)
        return ActionContinuationEnqueueResult::Invalid;

    std::shared_ptr<Action> continuationAction = FindAction(continuationCandidate.actionName);
    if (!continuationAction)
        return ActionContinuationEnqueueResult::ChildActionNotRegistered;
    std::uint32_t const actionLifetimeMs = continuationAction->GetPendingLifetimeMs();
    if (!actionLifetimeMs || actionLifetimeMs > PendingActionQueue::MaxLifetimeMs)
        return ActionContinuationEnqueueResult::ChildLifetimeDisabled;

    ActionDependencyRequest boundedRequest = request;
    boundedRequest.lifetimeMs = std::min(request.lifetimeMs, actionLifetimeMs);
    for (ActionTerminalPrerequisite const& prerequisite : boundedRequest.prerequisites)
        if (prerequisite.encounterId != context.coordinator.GetEncounterId())
            return ActionContinuationEnqueueResult::ParentEncounterInactive;

    PendingActionQueue::Context pendingContext;
    pendingContext.groupId = context.snapshot.groupId;
    pendingContext.mapId = context.snapshot.mapId;
    pendingContext.instanceId = context.snapshot.instanceId;
    pendingContext.botGuid = context.bot.guid;
    pendingContext.encounterId = context.coordinator.GetEncounterId();
    pendingContext.encounterActive = context.coordinator.HasActiveEncounter();
    pendingContext.scoped = true;

    // Validate before consulting terminal history so malformed requests cannot
    // be reported as a parent-result rejection.
    if (!ActionContinuationQueue::IsValidRequest(
            boundedRequest, continuationCandidate, pendingContext))
        return ActionContinuationEnqueueResult::Invalid;

    // If every prerequisite already has a terminal, late registration
    // inherits its deadline from the latest prerequisite terminal, not from
    // the adapter call that happened after completion. Otherwise the normal
    // TTL starts now and is never extended by later prerequisites.
    bool allPrerequisitesAlreadyTerminal = true;
    bool hasLatestTerminal = false;
    std::uint32_t latestTerminalMs = context.nowMs;
    for (ActionTerminalPrerequisite const& prerequisite : boundedRequest.prerequisites) {
        CoreSpellTerminalEvent const* matchedTerminal = nullptr;
        for (CoreSpellTerminalEvent const& terminal : m_recentCoreSpellTerminals)
            if (terminal.actionId == prerequisite.actionId &&
                terminal.encounterId == prerequisite.encounterId) {
                matchedTerminal = &terminal;
                break;
            }
        if (!matchedTerminal) {
            allPrerequisitesAlreadyTerminal = false;
            continue;
        }
        // The caller may have captured a context timestamp just before this
        // terminal was observed. Do not use a future event to anchor its TTL.
        if (static_cast<std::int32_t>(context.nowMs - matchedTerminal->occurredAtMs) < 0) {
            allPrerequisitesAlreadyTerminal = false;
            continue;
        }

        bool const matchesPrerequisite =
            matchedTerminal->targetGuid == prerequisite.targetGuid &&
            matchedTerminal->spellId == prerequisite.spellId &&
            matchedTerminal->status == prerequisite.requiredStatus;
        // A failed parent invalidates an ordinary AND dependency immediately.
        // For alternatives, a rejected branch must remain registered so the
        // whole one-of-N group can settle and choose another eligible branch.
        if (!matchesPrerequisite && !boundedRequest.alternativeGroupId)
            return ActionContinuationEnqueueResult::ParentTerminalRejected;

        if (!hasLatestTerminal ||
            static_cast<std::int32_t>(matchedTerminal->occurredAtMs - latestTerminalMs) > 0) {
            latestTerminalMs = matchedTerminal->occurredAtMs;
            hasLatestTerminal = true;
        }
    }
    std::uint32_t const ttlStartMs =
        allPrerequisitesAlreadyTerminal && hasLatestTerminal
            ? latestTerminalMs : context.nowMs;
    if (ActionContinuationQueue::IsExpired(
            context.nowMs, ttlStartMs + boundedRequest.lifetimeMs))
        return ActionContinuationEnqueueResult::Expired;

    ActionContinuationEnqueueResult const result = m_actionContinuationQueue.Enqueue(
        boundedRequest, continuationCandidate, context.nowMs, pendingContext, ttlStartMs);
    if (result != ActionContinuationEnqueueResult::Queued)
        return result;

    // Instant casts can finish before an adapter registers a dependency. Seed
    // every prerequisite from the bounded terminal ledger before deciding.
    for (CoreSpellTerminalEvent const& event : m_recentCoreSpellTerminals)
        m_actionContinuationQueue.ObserveTerminal(event);
    return ReleaseReadyActionDependencies(context.nowMs, &continuationCandidate)
        ? ActionContinuationEnqueueResult::ReleasedImmediately
        : ActionContinuationEnqueueResult::Queued;
}

ActionContinuationEnqueueResult PartyBotEngine::QueueActionContinuation(
    DecisionContext const& context, ActionCandidate const& parentCandidate,
    ActionCandidate const& continuationCandidate, std::uint32_t lifetimeMs) const {
    ActionDependencyRequest request;
    request.lifetimeMs = lifetimeMs;
    ActionTerminalPrerequisite prerequisite;
    prerequisite.actionId = parentCandidate.actionId;
    prerequisite.encounterId = context.coordinator.GetEncounterId();
    prerequisite.targetGuid = parentCandidate.targetGuid;
    prerequisite.spellId = parentCandidate.spellId;
    prerequisite.requiredStatus = CoreSpellTerminalStatus::Succeeded;
    request.prerequisites.push_back(prerequisite);
    return QueueActionDependency(context, request, continuationCandidate);
}

void PartyBotEngine::NotifyCoreSpellTerminal(CoreSpellTerminalEvent const& event) const {
    RememberCoreSpellTerminal(event);
    m_actionContinuationQueue.PruneExpired(event.occurredAtMs);
    m_actionContinuationQueue.ObserveTerminal(event);
    ReleaseReadyActionDependencies(event.occurredAtMs);

    std::vector<std::shared_ptr<CoreSpellTerminalListener>> const listeners =
        m_coreSpellTerminalListeners;
    for (auto const& listener : listeners)
        if (listener)
            listener->OnCoreSpellTerminal(event);
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
void PartyBotEngine::InvalidateDecisionState() const {
    m_valueCache.InvalidateAll();
    m_triggerScheduler.InvalidateAll();
}
void PartyBotEngine::ResetDecisionState() const {
    m_valueCache.InvalidateAll();
    m_triggerScheduler.Clear();
    m_pendingActionQueue.Clear();
    m_actionContinuationQueue.Clear();
    m_recentCoreSpellTerminals.clear();
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
CandidateEvaluation PartyBotEngine::EvaluateCandidate(
    DecisionContext const& context, ActionCandidate candidate) const {
    CandidateEvaluation evaluation;
    evaluation.candidate = std::move(candidate);
    ActionCandidate const& evaluatedCandidate = evaluation.candidate;
    std::shared_ptr<Action> action = FindAction(evaluatedCandidate.actionName);
    if (!action) {
        evaluation.status = CandidateStatus::RejectedMissingAction;
        evaluation.reason = "action_not_registered";
        return evaluation;
    }
    if (!std::isfinite(evaluatedCandidate.basePriority)) {
        evaluation.status = CandidateStatus::RejectedInvalidPriority;
        evaluation.reason = "non_finite_base_priority";
        return evaluation;
    }
    if (!action->IsPossible(context, evaluatedCandidate, evaluation.reason)) {
        evaluation.status = CandidateStatus::RejectedImpossible;
        if (evaluation.reason.empty()) evaluation.reason = "action_not_possible";
        return evaluation;
    }
    evaluation.reason.clear();
    if (!action->IsUseful(context, evaluatedCandidate, evaluation.reason)) {
        evaluation.status = CandidateStatus::RejectedUseless;
        if (evaluation.reason.empty()) evaluation.reason = "action_not_useful";
        return evaluation;
    }
    evaluation.threatImpact = action->EstimateThreatImpact(context, evaluatedCandidate);
    if (!IsWellFormedThreatImpactEstimate(evaluation.threatImpact) ||
        evaluation.threatImpact.snapshotEpoch != context.snapshot.epoch) {
        evaluation.status = CandidateStatus::RejectedThreatUnknown;
        evaluation.reason = "threat_impact_estimate_invalid_or_stale";
        return evaluation;
    }

    float priority = evaluatedCandidate.basePriority;
    for (auto const& multiplier : m_multipliers) {
        if (!multiplier) continue;
        float const factor = multiplier->GetValue(context, evaluatedCandidate);
        if (!std::isfinite(factor) || factor <= 0.0f) {
            evaluation.status = CandidateStatus::RejectedMultiplier;
            evaluation.reason = multiplier->GetName();
            if (evaluation.reason.empty()) evaluation.reason = "multiplier_veto";
            return evaluation;
        }
        priority *= factor;
    }
    if (!std::isfinite(priority)) {
        evaluation.status = CandidateStatus::RejectedInvalidPriority;
        evaluation.reason = "non_finite_final_priority";
        return evaluation;
    }
    evaluation.candidate.finalPriority = priority;
    evaluation.status = CandidateStatus::NotSelected;
    evaluation.reason.clear();
    return evaluation;
}

PendingActionEnqueueResult PartyBotEngine::QueuePendingAction(
    DecisionContext const& context, ActionCandidate const& candidate) const {
    std::shared_ptr<Action> action = FindAction(candidate.actionName);
    if (!action)
        return PendingActionEnqueueResult::ActionNotRegistered;
    std::uint32_t const lifetimeMs = action->GetPendingLifetimeMs();
    if (!lifetimeMs || lifetimeMs > PendingActionQueue::MaxLifetimeMs)
        return PendingActionEnqueueResult::LifetimeDisabled;
    if (!context.snapshot.groupId || !context.bot.guid ||
        context.snapshot.mapId != context.bot.mapId ||
        context.snapshot.instanceId != context.bot.instanceId)
        return PendingActionEnqueueResult::Invalid;

    DecisionContext evaluationContext(
        context.snapshot, context.bot, context.coordinator, context.nowMs,
        context.reservationTrace,
        context.valueCache ? context.valueCache : &m_valueCache,
        context.engineActionObserver, this);
    CandidateEvaluation evaluation = EvaluateCandidate(evaluationContext, candidate);
    if (evaluation.status != CandidateStatus::NotSelected)
        return PendingActionEnqueueResult::NotEligible;

    PendingActionQueue::Context pendingContext;
    pendingContext.groupId = context.snapshot.groupId;
    pendingContext.mapId = context.snapshot.mapId;
    pendingContext.instanceId = context.snapshot.instanceId;
    pendingContext.botGuid = context.bot.guid;
    pendingContext.encounterId = context.coordinator.GetEncounterId();
    pendingContext.encounterActive = context.coordinator.HasActiveEncounter();
    pendingContext.scoped = true;
    return m_pendingActionQueue.Enqueue(
        evaluation.candidate, context.nowMs, lifetimeMs, pendingContext);
}

std::size_t PartyBotEngine::QueueSelectedActions(
    DecisionContext const& context, DecisionReport& report) const {
    std::size_t queued = 0;
    for (std::size_t index : report.selectedIndices) {
        if (index >= report.candidates.size())
            continue;
        CandidateEvaluation& evaluation = report.candidates[index];
        if (evaluation.status != CandidateStatus::Selected)
            continue;
        std::shared_ptr<Action> action = FindAction(evaluation.candidate.actionName);
        if (!action || !action->GetPendingLifetimeMs())
            continue;

        PendingActionEnqueueResult const result =
            QueuePendingAction(context, evaluation.candidate);
        if (result == PendingActionEnqueueResult::Queued ||
            result == PendingActionEnqueueResult::UpdatedExisting) {
            evaluation.status = CandidateStatus::Queued;
            evaluation.actionResult = ActionResult::NotAttempted;
            evaluation.reason = result == PendingActionEnqueueResult::Queued
                ? "pending_action_queued" : "pending_action_refreshed";
            ++queued;
        } else {
            evaluation.status = CandidateStatus::ExecutionFailed;
            evaluation.actionResult = ActionResult::Failed;
            switch (result) {
                case PendingActionEnqueueResult::Full:
                    evaluation.reason = "pending_queue_full";
                    break;
                case PendingActionEnqueueResult::Invalid:
                    evaluation.reason = "pending_queue_invalid";
                    break;
                case PendingActionEnqueueResult::ActionNotRegistered:
                    evaluation.reason = "pending_action_not_registered";
                    break;
                case PendingActionEnqueueResult::LifetimeDisabled:
                    evaluation.reason = "pending_action_lifetime_invalid";
                    break;
                case PendingActionEnqueueResult::NotEligible:
                    evaluation.reason = "pending_action_not_eligible";
                    break;
                case PendingActionEnqueueResult::Queued:
                case PendingActionEnqueueResult::UpdatedExisting:
                    evaluation.reason = "pending_queue_unexpected_result";
                    break;
            }
        }
    }
    return queued;
}

PendingActionDispatchSummary PartyBotEngine::ExecutePendingActions(
    DecisionContext& context, std::size_t maxActionsPerDispatch,
    std::uint32_t blockedChannelMask) const {
    PendingActionDispatchSummary summary;
    summary.expiredContinuations = m_actionContinuationQueue.PruneExpired(context.nowMs);
    if (!maxActionsPerDispatch)
        return summary;
    maxActionsPerDispatch = std::min<std::size_t>(maxActionsPerDispatch, 8);
    DecisionContext executionContext(
        context.snapshot, context.bot, context.coordinator, context.nowMs,
        context.reservationTrace,
        context.valueCache ? context.valueCache : &m_valueCache,
        context.engineActionObserver, this);
    PendingActionQueue::Context currentContext;
    currentContext.groupId = executionContext.snapshot.groupId;
    currentContext.mapId = executionContext.snapshot.mapId;
    currentContext.instanceId = executionContext.snapshot.instanceId;
    currentContext.botGuid = executionContext.bot.guid;
    currentContext.encounterId = executionContext.coordinator.GetEncounterId();
    currentContext.encounterActive = executionContext.coordinator.HasActiveEncounter();
    currentContext.scoped = true;

    struct PendingWork {
        PendingActionQueue::Entry entry;
        CandidateEvaluation evaluation;
    };
    std::vector<PendingWork> work;
    work.reserve(PendingActionQueue::Capacity);
    summary.expired = m_pendingActionQueue.PruneExpired(executionContext.nowMs);
    PendingActionQueue::Entry entry;
    while (m_pendingActionQueue.PopNext(executionContext.nowMs, entry)) {
        ++summary.dequeued;
        PendingActionQueue::Context const& oldContext = entry.context;
        if (!oldContext.scoped ||
            oldContext.groupId != currentContext.groupId ||
            oldContext.mapId != currentContext.mapId ||
            oldContext.instanceId != currentContext.instanceId ||
            oldContext.botGuid != currentContext.botGuid ||
            oldContext.encounterId != currentContext.encounterId ||
            oldContext.encounterActive != currentContext.encounterActive) {
            ++summary.staleContext;
            continue;
        }
        CandidateEvaluation evaluation =
            EvaluateCandidate(executionContext, entry.candidate);
        if (evaluation.status != CandidateStatus::NotSelected) {
            ++summary.rejected;
            summary.completedCandidates.push_back(evaluation);
            continue;
        }
        PendingWork item;
        item.entry = entry;
        item.evaluation = std::move(evaluation);
        // Fairness compares against the currently revalidated priority, not
        // the potentially stale value captured when the action was enqueued.
        item.entry.priority = item.evaluation.candidate.finalPriority;
        work.push_back(std::move(item));
    }

    std::stable_sort(work.begin(), work.end(),
        [&executionContext](PendingWork const& left, PendingWork const& right) {
            // Keep the same age-promotion ordering as PendingActionQueue::PopNext.
            // This post-revalidation sort must not undo the fairness decision.
            return PendingActionQueue::ComesBefore(
                left.entry, right.entry, executionContext.nowMs);
        });

    DecisionReport report;
    report.snapshotEpoch = executionContext.snapshot.epoch;
    std::set<ActionChannel> occupiedChannels;
    for (std::uint32_t channel = 0;
         channel <= static_cast<std::uint32_t>(ActionChannel::Utility); ++channel) {
        if (blockedChannelMask & (1u << channel))
            occupiedChannels.insert(static_cast<ActionChannel>(channel));
    }
    ThreatImpactTotals selectedThreatInReport;
    auto defer = [&](PendingWork& item) {
        item.entry.candidate = item.evaluation.candidate;
        item.entry.priority = item.evaluation.candidate.finalPriority;
        if (m_pendingActionQueue.Requeue(item.entry, executionContext.nowMs))
            ++summary.requeued;
        else
            ++summary.queueFull;
    };

    for (PendingWork& item : work) {
        if (report.selectedIndices.size() >= maxActionsPerDispatch ||
            occupiedChannels.find(item.evaluation.candidate.channel) !=
                occupiedChannels.end()) {
            defer(item);
            continue;
        }
        ThreatImpactBudgetResult const threatBudget = AssessThreatImpactBudget(
            executionContext.snapshot, item.evaluation.threatImpact,
            executionContext.coordinator, executionContext.nowMs, 250,
            &selectedThreatInReport);
        if (threatBudget.status != ThreatImpactBudgetStatus::Safe) {
            // Temporary headroom/reservation changes may make this safe later;
            // requeue only until the original TTL and never bypass the budget.
            defer(item);
            continue;
        }
        item.evaluation.status = CandidateStatus::Selected;
        std::size_t const index = report.candidates.size();
        report.candidates.push_back(std::move(item.evaluation));
        report.selectedIndices.push_back(index);
        occupiedChannels.insert(report.candidates.back().candidate.channel);
    }

    summary.selected = report.selectedIndices.size();
    if (!report.selectedIndices.empty())
        ExecuteSelected(context, report);
    for (CandidateEvaluation const& evaluation : report.candidates) {
        if (evaluation.status == CandidateStatus::Executed) {
            ++summary.executed;
            summary.completedCandidates.push_back(evaluation);
        } else if (evaluation.status == CandidateStatus::ExecutionFailed) {
            ++summary.failed;
            summary.completedCandidates.push_back(evaluation);
        }
    }
    return summary;
}

PendingActionDispatchSummary PartyBotEngine::ExecuteDecisionCycle(
    DecisionContext& context, DecisionReport& report) const {
    // TTL-enabled current decisions enter the pending queue first. Immediate
    // actions then reserve their channels and share the eight-action tick cap.
    PendingActionDispatchSummary summary;
    summary.expiredContinuations = m_actionContinuationQueue.PruneExpired(context.nowMs);
    summary.newlyQueued = QueueSelectedActions(context, report);

    std::uint32_t blockedChannelMask = 0;
    std::size_t immediateSelected = 0;
    for (std::size_t index : report.selectedIndices) {
        if (index >= report.candidates.size())
            continue;
        CandidateEvaluation const& evaluation = report.candidates[index];
        if (evaluation.status != CandidateStatus::Selected)
            continue;
        ++immediateSelected;
        blockedChannelMask |= 1u << static_cast<std::uint32_t>(
            evaluation.candidate.channel);
    }

    if (immediateSelected)
        ExecuteSelected(context, report);

    std::size_t const pendingBudget =
        8 - std::min<std::size_t>(immediateSelected, 8);
    if (pendingBudget) {
        PendingActionDispatchSummary pending =
            ExecutePendingActions(context, pendingBudget, blockedChannelMask);
        summary.expired += pending.expired;
        summary.expiredContinuations += pending.expiredContinuations;
        summary.dequeued += pending.dequeued;
        summary.staleContext += pending.staleContext;
        summary.rejected += pending.rejected;
        summary.requeued += pending.requeued;
        summary.queueFull += pending.queueFull;
        summary.selected += pending.selected;
        summary.executed += pending.executed;
        summary.failed += pending.failed;
        summary.completedCandidates.insert(summary.completedCandidates.end(),
            pending.completedCandidates.begin(), pending.completedCandidates.end());
    }

    auto sameIdentity = [](ActionCandidate const& left, ActionCandidate const& right) {
        return left.actionName == right.actionName &&
            left.targetGuid == right.targetGuid &&
            left.spellId == right.spellId &&
            left.channel == right.channel &&
            left.hasDestination == right.hasDestination &&
            (!left.hasDestination ||
                (left.destinationX == right.destinationX &&
                 left.destinationY == right.destinationY &&
                 left.destinationZ == right.destinationZ));
    };
    for (CandidateEvaluation const& completed : summary.completedCandidates) {
        for (std::size_t index : report.selectedIndices) {
            if (index >= report.candidates.size())
                continue;
            CandidateEvaluation& source = report.candidates[index];
            if (source.status != CandidateStatus::Queued ||
                !sameIdentity(source.candidate, completed.candidate))
                continue;
            source = completed;
            break;
        }
    }
    return summary;
}

DecisionReport PartyBotEngine::Evaluate(DecisionContext const& context) const {
    DecisionContext evaluationContext(
        context.snapshot, context.bot, context.coordinator, context.nowMs,
        context.reservationTrace,
        context.valueCache ? context.valueCache : &m_valueCache,
        context.engineActionObserver, this);
    DecisionReport report;
    report.snapshotEpoch = evaluationContext.snapshot.epoch;
    auto retainTriggerEvaluation = [&report](TriggerEvaluation&& observation) {
        if (report.triggerEvaluations.size() < DecisionReport::MaxTriggerEvaluations)
            report.triggerEvaluations.push_back(std::move(observation));
        else
            ++report.omittedTriggerEvaluations;
    };
    for (auto const& strategy : m_strategies) {
        if (!strategy) continue;
        for (auto const& trigger : strategy->GetTriggers()) {
            if (!trigger) continue;
            TriggerEvaluation observation;
            observation.strategyName = strategy->GetName();
            observation.triggerName = trigger->GetName();
            observation.intervalMs = trigger->GetCheckIntervalMs();
            std::string const triggerKey = strategy->GetName() + "/" + trigger->GetName();
            if (!m_triggerScheduler.ShouldEvaluate(
                    triggerKey, observation.intervalMs, evaluationContext.nowMs)) {
                observation.status = TriggerEvaluationStatus::CadenceSkipped;
                retainTriggerEvaluation(std::move(observation));
                continue;
            }
            if (!trigger->IsActive(evaluationContext)) {
                observation.status = TriggerEvaluationStatus::Inactive;
                retainTriggerEvaluation(std::move(observation));
                continue;
            }
            observation.status = TriggerEvaluationStatus::Active;
            std::vector<ActionCandidate> candidates;
            trigger->CollectCandidates(evaluationContext, candidates);
            observation.candidateCount = static_cast<std::uint32_t>(
                std::min<std::size_t>(candidates.size(),
                    std::numeric_limits<std::uint32_t>::max()));
            retainTriggerEvaluation(std::move(observation));
            for (auto const& candidate : candidates) {
                ActionCandidate evaluatedCandidate = candidate;
                if (evaluatedCandidate.strategyName.empty())
                    evaluatedCandidate.strategyName = strategy->GetName();
                evaluatedCandidate.triggerName = trigger->GetName();
                report.candidates.push_back(
                    EvaluateCandidate(evaluationContext, std::move(evaluatedCandidate)));
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
        context.valueCache ? context.valueCache : &m_valueCache,
        context.engineActionObserver, this);
    auto recordReservation = [&executionContext](ReservationTraceEvent const& event) {
        if (executionContext.reservationTrace)
            executionContext.reservationTrace->RecordReservationEvent(executionContext, event);
    };
    auto recordAction = [this, &executionContext](
        CandidateEvaluation& evaluation, ActionId actionId,
        EngineActionTraceStage stage, bool attempted,
        ActionResult result, char const* reason) {
        if (executionContext.reservationTrace ||
            executionContext.engineActionObserver) {
            EngineActionTraceEvent event;
            event.stage = stage;
            event.actionId = actionId;
            event.actionName = evaluation.candidate.actionName.c_str();
            event.targetGuid = evaluation.candidate.targetGuid;
            event.spellId = evaluation.candidate.spellId;
            event.channel = evaluation.candidate.channel;
            event.priority = evaluation.candidate.finalPriority;
            event.result = result;
            event.attempted = attempted;
            event.reason = reason ? reason : "none";
            if (executionContext.reservationTrace)
                executionContext.reservationTrace->RecordEngineActionEvent(executionContext, event);
            if (executionContext.engineActionObserver)
                executionContext.engineActionObserver->OnEngineActionEvent(executionContext, event);
        }

        // Result listeners are policy hooks, not trace consumers: they receive
        // the same immutable event even when all diagnostic sinks are disabled.
        if (stage == EngineActionTraceStage::Result)
            NotifyActionResult(executionContext, evaluation, actionId,
                               attempted, result, reason);
    };

    // Materialize selected decisions into a bounded queue. The queue contains
    // indices only; all Core-facing validity, range, LOS, resource and threat
    // checks remain immediately before dispatch below.
    ActionQueue queue;
    for (std::size_t index : report.selectedIndices) {
        if (index >= report.candidates.size())
            continue;
        CandidateEvaluation& queued = report.candidates[index];
        if (queued.status != CandidateStatus::Selected)
            continue;
        if (!std::isfinite(queued.candidate.finalPriority) ||
            !queue.Push(index, queued.candidate.finalPriority)) {
            queued.status = CandidateStatus::ExecutionFailed;
            queued.actionResult = ActionResult::Failed;
            queued.reason = "action_queue_capacity_or_priority_rejected";
            ActionId const rejectedActionId = executionContext.coordinator.NextActionId();
            queued.actionId = rejectedActionId;
            queued.candidate.actionId = rejectedActionId;
            recordAction(queued, rejectedActionId, EngineActionTraceStage::Result,
                         false, ActionResult::Failed, queued.reason.c_str());
        }
    }

    ActionQueue::Entry queuedEntry;
    std::size_t processed = 0;
    std::size_t const maxActionsPerDispatch = 8;
    while (processed < maxActionsPerDispatch && queue.PopNext(queuedEntry)) {
        ++processed;
        std::size_t const index = queuedEntry.reportIndex;
        if (index >= report.candidates.size())
            continue;
        CandidateEvaluation& evaluation = report.candidates[index];
        if (evaluation.status != CandidateStatus::Selected)
            continue;

        // One coordinator-local ID identifies the logical Engine action whether
        // or not it creates a threat reservation. Reservation rows, when any,
        // reuse the same ID.
        ActionId const actionId = executionContext.coordinator.NextActionId();
        evaluation.actionId = actionId;
        evaluation.candidate.actionId = actionId;
        if (!actionId) {
            evaluation.status = CandidateStatus::ExecutionFailed;
            evaluation.actionResult = ActionResult::Failed;
            evaluation.reason = "engine_action_id_unavailable";
            recordAction(evaluation, 0, EngineActionTraceStage::Result,
                         false, ActionResult::Failed, evaluation.reason.c_str());
            continue;
        }

        std::shared_ptr<Action> action = FindAction(evaluation.candidate.actionName);
        if (!action) {
            evaluation.status = CandidateStatus::ExecutionFailed;
            evaluation.actionResult = ActionResult::Failed;
            evaluation.reason = "action_unregistered_before_execution";
            recordAction(evaluation, actionId, EngineActionTraceStage::Result,
                         false, ActionResult::Failed, evaluation.reason.c_str());
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
            recordAction(evaluation, actionId, EngineActionTraceStage::Result,
                         false, ActionResult::Failed, evaluation.reason.c_str());
            continue;
        }

        bool hasThreatReservation = false;
        if (evaluation.threatImpact.completeness == ThreatImpactCompleteness::Complete) {
            ThreatImpactTotals totals;
            for (ThreatImpactEntry const& impact : evaluation.threatImpact.impacts) {
                if (impact.threatPoints > 0.0f)
                    totals[ThreatImpactKey(impact.hostileGuid, impact.threatOwnerGuid)] +=
                        static_cast<double>(impact.threatPoints);
            }

            if (!totals.empty()) {
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
                        executionContext.coordinator.TryReserveThreatImpact(
                            reservation, executionContext.nowMs);

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
                    recordAction(evaluation, actionId, EngineActionTraceStage::Result,
                                 false, ActionResult::Failed, evaluation.reason.c_str());
                    continue;
                }

                hasThreatReservation = true;
                evaluation.threatActionId = actionId;
                evaluation.candidate.threatActionId = actionId;
            }
        }

        // This event brackets the synchronous adapter call. A successful result
        // means Action::Execute returned Success, not that a later spell/movement
        // effect necessarily completed; Core terminal events remain separate.
        recordAction(evaluation, actionId, EngineActionTraceStage::Attempt,
                     true, ActionResult::NotAttempted, "adapter_dispatch");
        evaluation.actionResult = action->Execute(executionContext, evaluation.candidate);
        if (evaluation.actionResult == ActionResult::Success) {
            evaluation.status = CandidateStatus::Executed;
            evaluation.reason.clear();
            recordAction(evaluation, actionId, EngineActionTraceStage::Result,
                         true, evaluation.actionResult, "adapter_returned_success");
        } else {
            if (hasThreatReservation) {
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
            if (evaluation.reason.empty())
                evaluation.reason = "action_execution_not_successful";
            recordAction(evaluation, actionId, EngineActionTraceStage::Result,
                         true, evaluation.actionResult, evaluation.reason.c_str());
        }
    }
}
} // namespace PartyBotEngine
