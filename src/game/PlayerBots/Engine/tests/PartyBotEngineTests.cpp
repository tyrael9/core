#include "PartyBotEngine.h"
#include "ItemUseAggregate.h"
#include "CoreAction.h"
#include "ExecutionAuthority.h"
#include "Value.h"
#include "ShadowStrategies.h"
#include "DpsAssistTrigger.h"
#include "HealerPolicy.h"
#include "PriestHealingPolicy.h"
#include "HealingForecast.h"
#include "PriestDirectHealAction.h"
#include "DirectHealThreatComponents.h"
#include "FollowPartyLeaderTrigger.h"
#include "BotBehaviorState.h"
#include "AutoResurrectSelfTrigger.h"
#include "ResurrectionPolicy.h"
#include "ResurrectGroupMemberTrigger.h"
#include "ResurrectGroupMemberAction.h"
#include "StopFollowingUnsafeTrigger.h"
#include "ThreatBudget.h"
#include "ThreatBudgetPolicy.h"
#include "ThreatReservationTerminalListener.h"
#include "HealReservationTerminalListener.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace PartyBotEngine;
namespace {
class TestAction : public Action {
public:
    explicit TestAction(std::string name) : m_name(std::move(name)) {}
    std::string const& GetName() const override { return m_name; }
    bool IsPossible(DecisionContext const&, ActionCandidate const&, std::string& reason) const override {
        ++possibleChecks;
        if (!possible) reason = "test_impossible";
        return possible;
    }
    bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string& reason) const override {
        ++usefulChecks;
        if (!useful) reason = "test_useless";
        return useful;
    }
    std::uint32_t GetPendingLifetimeMs() const override { return pendingLifetimeMs; }
    ActionResult Execute(DecisionContext&, ActionCandidate const& candidate) override {
        ++executeCount;
        lastThreatActionId = candidate.threatActionId;
        if (executionOrder)
            executionOrder->push_back(m_name);
        return result;
    }
    ThreatImpactEstimate EstimateThreatImpact(DecisionContext const& context,
                                               ActionCandidate const& candidate) const override {
        if (returnDefaultUnknown)
            return Action::EstimateThreatImpact(context, candidate);
        if (useThreatImpact) {
            ThreatImpactEstimate estimate = threatImpact;
            estimate.snapshotEpoch = context.snapshot.epoch;
            return estimate;
        }
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "test_action_has_no_threat";
        return estimate;
    }
    bool possible = true, useful = true, useThreatImpact = false;
    bool returnDefaultUnknown = false;
    std::uint32_t pendingLifetimeMs = 0;
    mutable std::uint32_t possibleChecks = 0;
    mutable std::uint32_t usefulChecks = 0;
    ActionResult result = ActionResult::Success;
    ThreatImpactEstimate threatImpact;
    ThreatActionId lastThreatActionId = 0;
    std::uint32_t executeCount = 0;
    std::vector<std::string>* executionOrder = nullptr;
private:
    std::string m_name;
};
class DependencyRegisteringTestAction final : public Action {
public:
    DependencyRegisteringTestAction(
        std::string name, std::string childName,
        ActionContinuationEnqueueResult& enqueueResult,
        ActionResult executionResult = ActionResult::Success)
        : m_name(std::move(name)), m_childName(std::move(childName)),
          m_enqueueResult(enqueueResult), m_executionResult(executionResult) {}

    std::string const& GetName() const override { return m_name; }
    bool IsPossible(DecisionContext const&, ActionCandidate const&, std::string&) const override {
        return true;
    }
    bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string&) const override {
        return true;
    }
    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "test_dependency_parent_no_threat";
        return estimate;
    }
    ActionResult Execute(DecisionContext& context, ActionCandidate const& candidate) override {
        ActionDependencyRequest request;
        request.lifetimeMs = 1500;

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

        ActionCandidate child;
        child.actionName = m_childName;
        child.targetGuid = candidate.targetGuid;
        child.basePriority = 25.0f;
        child.finalPriority = 25.0f;
        child.channel = ActionChannel::Utility;
        m_enqueueResult = context.continuationSink
            ? context.continuationSink->QueueActionDependency(context, request, child)
            : ActionContinuationEnqueueResult::Invalid;
        return m_executionResult;
    }

private:
    std::string m_name;
    std::string m_childName;
    ActionContinuationEnqueueResult& m_enqueueResult;
    ActionResult m_executionResult;
};

class CaptureReservationTraceSink final : public ReservationTraceSink {
public:
    void RecordReservationEvent(DecisionContext const&,
                                ReservationTraceEvent const& event) override {
        reasonStorage.push_back(std::make_shared<std::string>(
            event.reason ? event.reason : "none"));
        ReservationTraceEvent captured = event;
        captured.reason = reasonStorage.back()->c_str();
        events.push_back(captured);
    }
    void RecordEngineActionEvent(DecisionContext const&,
                                 EngineActionTraceEvent const& event) override {
        actionNameStorage.push_back(std::make_shared<std::string>(
            event.actionName ? event.actionName : "none"));
        actionReasonStorage.push_back(std::make_shared<std::string>(
            event.reason ? event.reason : "none"));
        EngineActionTraceEvent captured = event;
        captured.actionName = actionNameStorage.back()->c_str();
        captured.reason = actionReasonStorage.back()->c_str();
        actionEvents.push_back(captured);
    }
    // Trace event strings are borrowed synchronously; retain backing strings
    // so assertions can inspect the captured values after dispatch.
    std::vector<std::shared_ptr<std::string>> reasonStorage;
    std::vector<std::shared_ptr<std::string>> actionNameStorage;
    std::vector<std::shared_ptr<std::string>> actionReasonStorage;
    std::vector<ReservationTraceEvent> events;
    std::vector<EngineActionTraceEvent> actionEvents;
};
class CaptureActionResultListener final : public ActionResultListener {
public:
    void OnActionResult(DecisionContext const&, ActionResultEvent const& event) override {
        events.push_back(event);
    }
    std::vector<ActionResultEvent> events;
};

class CaptureCoreSpellTerminalListener final : public CoreSpellTerminalListener {
public:
    void OnCoreSpellTerminal(CoreSpellTerminalEvent const& event) override { events.push_back(event); }
    std::vector<CoreSpellTerminalEvent> events;
};
class MockDirectHealRuntime final : public CoreActionRuntime
{
public:
    CoreActionResolution ResolveUnitContext(
        DecisionContext const&, Guid targetGuid) const override
    {
        CoreActionResolution result;
        result.status = CoreActionResolveStatus::Resolved;
        result.actor = reinterpret_cast<Player*>(const_cast<MockDirectHealRuntime*>(this));
        result.map = reinterpret_cast<Map*>(const_cast<MockDirectHealRuntime*>(this));
        result.target = targetGuid
            ? reinterpret_cast<Unit*>(const_cast<MockDirectHealRuntime*>(this))
            : nullptr;
        result.reason = "mock_resolved";
        return result;
    }
    bool CanCastDirectHeal(DecisionContext const&, Guid, std::uint32_t) const override
    {
        return canCast;
    }
    bool StartDirectHeal(
        DecisionContext const& context, Guid targetGuid, std::uint32_t) override
    {
        observedOwnReservation = context.coordinator.GetReservedAmount(
            ReservationType::Heal, targetGuid, otherOwnerGuid, context.nowMs);
        return startSucceeds;
    }
    Guid otherOwnerGuid = 0;
    std::uint32_t observedOwnReservation = 0;
    bool canCast = true;
    bool startSucceeds = true;
};

class CapturePointMovementTerminalListener final : public PointMovementTerminalListener {
public:
    void OnPointMovementTerminal(PointMovementTerminalEvent const& event) override {
        events.push_back(event);
    }
    std::vector<PointMovementTerminalEvent> events;
};
class CaptureItemUseTerminalListener final : public ItemUseTerminalListener {
public:
    void OnItemUseTerminal(ItemUseTerminalEvent const& event) override {
        events.push_back(event);
    }
    std::vector<ItemUseTerminalEvent> events;
};
class RegisterCoreSpellTerminalListenerDuringCallback final : public CoreSpellTerminalListener {
public:
    RegisterCoreSpellTerminalListenerDuringCallback(
        PartyBotEngine::PartyBotEngine& engine,
        std::shared_ptr<CoreSpellTerminalListener> next)
        : m_engine(engine), m_next(std::move(next)) {}
    void OnCoreSpellTerminal(CoreSpellTerminalEvent const& event) override {
        events.push_back(event);
        if (!registered) registered = m_engine.AddCoreSpellTerminalListener(m_next);
    }
    std::vector<CoreSpellTerminalEvent> events;
    bool registered = false;
private:
    PartyBotEngine::PartyBotEngine& m_engine;
    std::shared_ptr<CoreSpellTerminalListener> m_next;
};
class TestTrigger : public Trigger {
public:
    explicit TestTrigger(std::string name) : m_name(std::move(name)) {}
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetCheckIntervalMs() const override { return intervalMs; }
    bool IsActive(DecisionContext const&) const override { ++activeChecks; return active; }
    void CollectCandidates(DecisionContext const&, std::vector<ActionCandidate>& out) const override {
        out.insert(out.end(), generated.begin(), generated.end());
    }
    bool active = true;
    std::uint32_t intervalMs = 0;
    mutable std::uint32_t activeChecks = 0;
    std::vector<ActionCandidate> generated;
private:
    std::string m_name;
};
class TestMultiplier : public Multiplier {
public:
    TestMultiplier(std::string name, float factor) : m_name(std::move(name)), m_factor(factor) {}
    std::string const& GetName() const override { return m_name; }
    float GetValue(DecisionContext const&, ActionCandidate const&) const override { return m_factor; }
private:
    std::string m_name;
    float m_factor;
};
struct Fixture {
    PartySnapshot snapshot;
    PartyMemberSnapshot bot;
    PartyCoordinator coordinator;
    DecisionContext context;
    Fixture() : context(snapshot, bot, coordinator, 1000) {
        snapshot.epoch = 7; snapshot.groupId = 5; snapshot.mapId = 33;
        snapshot.instanceId = 101; bot.guid = 11; bot.mapId = 33;
        bot.instanceId = 101; bot.online = true; bot.alive = true;
    }
};
ActionCandidate Candidate(std::string name, float priority, ActionChannel channel) {
    ActionCandidate c; c.actionName = std::move(name); c.targetGuid = 22;
    c.basePriority = priority; c.channel = channel; return c;
}

ItemUseRequestDescriptor ItemUseRequest(
    std::uint32_t nowMs = 1000, std::uint32_t lifetimeMs = 5000)
{
    ItemUseRequestDescriptor request;
    request.actionId = 91;
    request.encounterId = 17;
    request.actorGuid = 100;
    request.itemGuid = 200;
    request.itemEntry = 118;
    request.requestedSpellSlot = ItemUseAggregateTracker::UnknownSlot;
    request.itemCountBefore = 1;
    request.startedAtMs = nowMs;
    request.lifetimeMs = lifetimeMs;
    return request;
}

void TestItemUseAggregateWaitsForFanoutClosure()
{
    ItemUseAggregateTracker tracker;
    ItemUseId root = 0;
    assert(tracker.Begin(ItemUseRequest(), root) == ItemUseTrackerResult::Accepted);
    assert(root != 0);

    ItemUseChildId first = 0, second = 0;
    assert(tracker.RegisterChild(root, 0, 439, false, first) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RegisterChild(root, 2, 440, true, second) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.MarkSlotSkipped(root, 1) == ItemUseTrackerResult::Accepted);

    ItemUseTerminalEvent terminal;
    assert(tracker.RecordChildTerminal(root, first,
        ItemUseChildStatus::Succeeded, 1010, terminal) ==
        ItemUseTrackerResult::Accepted);
    // A synchronous first child does not close the logical item request.
    assert(tracker.GetPendingRootCount() == 1);
    assert(tracker.RecordChildTerminal(root, second,
        ItemUseChildStatus::Succeeded, 1011, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.GetPendingRootCount() == 1);

    assert(tracker.CloseDispatch(root, 1012, terminal) ==
           ItemUseTrackerResult::Completed);
    assert(terminal.itemUseId == root);
    assert(terminal.actionId == 91);
    assert(terminal.encounterId == 17);
    assert(terminal.status == ItemUseAggregateStatus::AllSucceeded);
    assert(terminal.childCount == 2);
    assert(terminal.completedChildCount == 2);
    assert(terminal.succeededChildCount == 2);
    assert(terminal.skippedSlotCount == 1);
    assert(terminal.children[0].childId == first);
    assert(terminal.children[0].slotIndex == 0);
    assert(terminal.children[0].spellId == 439);
    assert(!terminal.children[0].triggered);
    assert(terminal.children[0].status == ItemUseChildStatus::Succeeded);
    assert(terminal.children[0].terminalAtMs == 1010);
    assert(terminal.children[1].childId == second);
    assert(terminal.children[1].slotIndex == 2);
    assert(terminal.children[1].spellId == 440);
    assert(terminal.children[1].triggered);
    assert(terminal.children[1].status == ItemUseChildStatus::Succeeded);
    assert(terminal.children[1].terminalAtMs == 1011);
    assert(tracker.GetPendingRootCount() == 0);
    assert(tracker.RecordChildTerminal(root, first,
        ItemUseChildStatus::Succeeded, 1013, terminal) ==
        ItemUseTrackerResult::AlreadySettled);
}

void TestItemUseAggregateDelayedChildAndPartialOutcomes()
{
    ItemUseAggregateTracker tracker;
    ItemUseId root = 0;
    assert(tracker.Begin(ItemUseRequest(), root) == ItemUseTrackerResult::Accepted);
    ItemUseChildId first = 0, delayed = 0, failed = 0;
    assert(tracker.RegisterChild(root, 0, 439, false, first) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RegisterChild(root, 1, 440, true, delayed) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RegisterChild(root, 3, 441, true, failed) ==
           ItemUseTrackerResult::Accepted);

    ItemUseTerminalEvent terminal;
    assert(tracker.RecordChildTerminal(root, first,
        ItemUseChildStatus::Succeeded, 1010, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.RecordChildTerminal(root, failed,
        ItemUseChildStatus::Failed, 1011, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.CloseDispatch(root, 1012, terminal) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.GetPendingRootCount() == 1);

    // Dispatch is closed, but the root stays pending until the delayed child
    // reaches its own Core Spell terminal.
    assert(tracker.RecordChildTerminal(root, delayed,
        ItemUseChildStatus::Succeeded, 2500, terminal) ==
        ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::PartialSuccess);
    assert(terminal.childCount == 3);
    assert(terminal.completedChildCount == 3);
    assert(terminal.succeededChildCount == 2);
    assert(terminal.failedChildCount == 1);
    assert(terminal.interruptedChildCount == 0);
}

void TestItemUseAggregateAllFailureAndInterruptStatuses()
{
    ItemUseAggregateTracker tracker;
    ItemUseTerminalEvent terminal;
    ItemUseId failedRoot = 0;
    assert(tracker.Begin(ItemUseRequest(), failedRoot) == ItemUseTrackerResult::Accepted);
    ItemUseChildId failedA = 0, failedB = 0;
    assert(tracker.RegisterChild(failedRoot, 0, 439, false, failedA) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RegisterChild(failedRoot, 1, 440, true, failedB) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RecordChildTerminal(failedRoot, failedA,
        ItemUseChildStatus::Failed, 1010, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.RecordChildTerminal(failedRoot, failedB,
        ItemUseChildStatus::Interrupted, 1011, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.CloseDispatch(failedRoot, 1012, terminal) ==
           ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::AllFailed);
    assert(terminal.failedChildCount == 1);
    assert(terminal.interruptedChildCount == 1);

    ItemUseId interruptedRoot = 0;
    assert(tracker.Begin(ItemUseRequest(2000), interruptedRoot) ==
           ItemUseTrackerResult::Accepted);
    ItemUseChildId interruptedA = 0, interruptedB = 0;
    assert(tracker.RegisterChild(interruptedRoot, 0, 439, false, interruptedA) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RegisterChild(interruptedRoot, 1, 440, true, interruptedB) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RecordChildTerminal(interruptedRoot, interruptedA,
        ItemUseChildStatus::Interrupted, 2010, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.RecordChildTerminal(interruptedRoot, interruptedB,
        ItemUseChildStatus::Interrupted, 2011, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.CloseDispatch(interruptedRoot, 2012, terminal) ==
           ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::AllInterrupted);
}

void TestItemUseAggregateRecordsMutationSeparately()
{
    ItemUseAggregateTracker tracker;
    ItemUseId root = 0;
    assert(tracker.Begin(ItemUseRequest(), root) == ItemUseTrackerResult::Accepted);
    ItemUseChildId child = 0;
    assert(tracker.RegisterChild(root, 0, 439, false, child) ==
           ItemUseTrackerResult::Accepted);

    ItemUseMutationEvent mutation;
    mutation.itemUseId = root;
    mutation.childId = child;
    mutation.itemGuid = 200;
    mutation.itemEntry = 118;
    mutation.kind = ItemUseMutationKind::ItemDestroyed;
    mutation.phase = ItemUseMutationPhase::BeforeEffects;
    mutation.beforeCount = 1;
    mutation.afterCount = 0;
    mutation.itemExistsAfter = false;
    mutation.occurredAtMs = 1010;
    assert(tracker.RecordMutation(mutation) == ItemUseTrackerResult::Accepted);

    ItemUseMutationEvent stackMutation = mutation;
    stackMutation.itemGuid = 201;
    stackMutation.kind = ItemUseMutationKind::CountChanged;
    stackMutation.phase = ItemUseMutationPhase::BeforeEffects;
    stackMutation.beforeCount = 2;
    stackMutation.afterCount = 1;
    stackMutation.itemExistsAfter = true;
    stackMutation.occurredAtMs = 1011;
    assert(tracker.RecordMutation(stackMutation) == ItemUseTrackerResult::Accepted);

    ItemUseMutationEvent reagentMutation = mutation;
    reagentMutation.itemGuid = 0;
    reagentMutation.kind = ItemUseMutationKind::ReagentConsumed;
    reagentMutation.phase = ItemUseMutationPhase::BeforeEffects;
    reagentMutation.beforeCount = 3;
    reagentMutation.afterCount = 0;
    reagentMutation.itemExistsAfter = false;
    reagentMutation.occurredAtMs = 1012;
    assert(tracker.RecordMutation(reagentMutation) == ItemUseTrackerResult::Accepted);

    ItemUseMutationEvent invalidStackMutation = stackMutation;
    invalidStackMutation.itemGuid = 0;
    assert(tracker.RecordMutation(invalidStackMutation) ==
           ItemUseTrackerResult::Invalid);

    ItemUseTerminalEvent terminal;
    assert(tracker.RecordChildTerminal(root, child,
        ItemUseChildStatus::Interrupted, 1013, terminal) ==
        ItemUseTrackerResult::Accepted);
    assert(tracker.CloseDispatch(root, 1014, terminal) ==
           ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::AllInterrupted);
    assert(terminal.observedMutationCount == 3);

    ItemUseMutationEvent emitted;
    assert(tracker.PopMutationEvent(emitted));
    assert(emitted.itemUseId == root);
    assert(emitted.childId == child);
    assert(emitted.kind == ItemUseMutationKind::ItemDestroyed);
    assert(emitted.beforeCount == 1 && emitted.afterCount == 0);
    assert(!emitted.itemExistsAfter);

    assert(tracker.PopMutationEvent(emitted));
    assert(emitted.itemGuid == 201);
    assert(emitted.kind == ItemUseMutationKind::CountChanged);
    assert(emitted.phase == ItemUseMutationPhase::BeforeEffects);
    assert(emitted.beforeCount == 2 && emitted.afterCount == 1);
    assert(emitted.itemExistsAfter);

    assert(tracker.PopMutationEvent(emitted));
    assert(emitted.itemGuid == 0);
    assert(emitted.itemEntry == 118);
    assert(emitted.kind == ItemUseMutationKind::ReagentConsumed);
    assert(emitted.phase == ItemUseMutationPhase::BeforeEffects);
    assert(emitted.beforeCount == 3 && emitted.afterCount == 0);
    assert(!emitted.itemExistsAfter);
    assert(!tracker.PopMutationEvent(emitted));
}

void TestItemUseAggregateNoChildrenDuplicateSlotsAndCapacity()
{
    ItemUseAggregateTracker tracker;
    ItemUseTerminalEvent terminal;
    ItemUseId empty = 0;
    assert(tracker.Begin(ItemUseRequest(), empty) == ItemUseTrackerResult::Accepted);
    assert(tracker.MarkSlotSkipped(empty, 0) == ItemUseTrackerResult::Accepted);
    assert(tracker.MarkSlotSkipped(empty, 0) == ItemUseTrackerResult::SlotAlreadyReported);
    assert(tracker.CloseDispatch(empty, 1001, terminal) ==
           ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::NoChildren);
    assert(terminal.skippedSlotCount == 1);

    ItemUseId active[ItemUseAggregateTracker::Capacity] = {};
    for (std::size_t i = 0; i < ItemUseAggregateTracker::Capacity; ++i)
        assert(tracker.Begin(ItemUseRequest(
            static_cast<std::uint32_t>(2000 + i)), active[i]) ==
            ItemUseTrackerResult::Accepted);
    ItemUseId overflow = 0;
    assert(tracker.Begin(ItemUseRequest(3000), overflow) == ItemUseTrackerResult::Full);
    for (std::size_t i = 0; i < ItemUseAggregateTracker::Capacity; ++i)
        assert(tracker.CloseDispatch(active[i], 4000, terminal) ==
            ItemUseTrackerResult::Completed);
}

void TestItemUseAggregateLateChildCannotBeatDeadline()
{
    ItemUseAggregateTracker tracker;
    ItemUseId root = 0;
    std::uint32_t const start = 0xfffffff0u;
    assert(tracker.Begin(ItemUseRequest(start, 40), root) ==
           ItemUseTrackerResult::Accepted);
    ItemUseChildId child = 0;
    assert(tracker.RegisterChild(root, 0, 439, false, child) ==
           ItemUseTrackerResult::Accepted);

    ItemUseTerminalEvent terminal;
    assert(tracker.CloseDispatch(root, 0x00000017u, terminal) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.GetPendingRootCount() == 1);

    // Exact expiry is wrap-safe. The delayed Core child did finish, but its
    // terminal arrived after the root deadline and cannot turn it into success.
    assert(tracker.RecordChildTerminal(root, child,
        ItemUseChildStatus::Succeeded, 0x00000018u, terminal) ==
        ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::Expired);
    assert(terminal.itemUseId == root);
    assert(terminal.childCount == 1);
    assert(terminal.completedChildCount == 1);
    assert(terminal.succeededChildCount == 1);
    assert(tracker.GetPendingRootCount() == 0);
    assert(tracker.RecordChildTerminal(root, child,
        ItemUseChildStatus::Succeeded, 0x00000019u, terminal) ==
        ItemUseTrackerResult::AlreadySettled);

    ItemUseId closeRoot = 0;
    assert(tracker.Begin(ItemUseRequest(3000, 100), closeRoot) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.RegisterChild(closeRoot, 0, 439, false, child) ==
           ItemUseTrackerResult::Accepted);
    assert(tracker.CloseDispatch(closeRoot, 3100, terminal) ==
           ItemUseTrackerResult::Completed);
    assert(terminal.status == ItemUseAggregateStatus::Expired);
    assert(terminal.completedChildCount == 0);
    assert(tracker.GetPendingRootCount() == 0);
}

void TestItemUseAggregateExpiryAndClockWrap()
{
    ItemUseAggregateTracker tracker;
    ItemUseId root = 0;
    std::uint32_t const start = 0xfffffff0u;
    assert(tracker.Begin(ItemUseRequest(start, 40), root) ==
           ItemUseTrackerResult::Accepted);
    ItemUseChildId child = 0;
    assert(tracker.RegisterChild(root, 0, 439, false, child) ==
           ItemUseTrackerResult::Accepted);
    assert(!ItemUseAggregateTracker::IsExpired(0x00000010u, 0x00000018u));
    std::vector<ItemUseTerminalEvent> expired;
    assert(tracker.PruneExpired(0x00000017u, expired) == 0);
    assert(tracker.PruneExpired(0x00000018u, expired) == 1);
    assert(expired.size() == 1);
    assert(expired[0].itemUseId == root);
    assert(expired[0].status == ItemUseAggregateStatus::Expired);
    assert(expired[0].completedChildCount == 0);
    ItemUseTerminalEvent lateTerminal;
    assert(tracker.CloseDispatch(root, 0x00000019u, lateTerminal) ==
           ItemUseTrackerResult::AlreadySettled);
}

class TestCoreActionRuntime final : public CoreActionRuntime {
public:
    CoreActionResolution ResolveUnitContext(
        DecisionContext const&, Guid targetGuid) const override {
        ++resolveCalls;
        CoreActionResolution result;
        result.status = CoreActionResolveStatus::Resolved;
        result.actor = reinterpret_cast<Player*>(const_cast<TestCoreActionRuntime*>(this));
        result.target = targetGuid
            ? reinterpret_cast<Unit*>(const_cast<TestCoreActionRuntime*>(this)) : nullptr;
        result.map = reinterpret_cast<Map*>(const_cast<TestCoreActionRuntime*>(this));
        result.reason = "test_core_context_resolved";
        return result;
    }
    mutable std::uint32_t resolveCalls = 0;
};

class TestCoreAction final : public CoreAction {
public:
    explicit TestCoreAction(std::string name) : m_name(std::move(name)) {}
    std::string const& GetName() const override { return m_name; }
    bool IsPossible(DecisionContext const& context, ActionCandidate const&,
                    std::string& reason) const override {
        ++possibleChecks;
        if (!context.coreActionRuntime) reason = "missing_core_runtime";
        return context.coreActionRuntime != nullptr;
    }
    bool IsUseful(DecisionContext const& context, ActionCandidate const&,
                  std::string& reason) const override {
        ++usefulChecks;
        if (!context.coreActionRuntime) reason = "missing_core_runtime";
        return context.coreActionRuntime != nullptr;
    }
    ThreatImpactEstimate EstimateThreatImpact(
        DecisionContext const& context, ActionCandidate const&) const override {
        ThreatImpactEstimate estimate;
        estimate.completeness = ThreatImpactCompleteness::NoThreat;
        estimate.snapshotEpoch = context.snapshot.epoch;
        estimate.reason = "test_core_action_no_threat";
        return estimate;
    }
    std::uint32_t GetPendingLifetimeMs() const override { return 1500; }
    mutable std::uint32_t possibleChecks = 0;
    mutable std::uint32_t usefulChecks = 0;
    std::uint32_t executeCoreCalls = 0;
    bool executeCoreSawResolvedContext = false;
    bool rejectPreflight = false;
    mutable std::uint32_t preflightChecks = 0;
protected:
    ActionExecutionResult ValidateCoreAction(
        DecisionContext const&, ActionCandidate const&,
        CoreActionResolution const& resolved) const override {
        ++preflightChecks;
        ActionExecutionResult result;
        if (!resolved.IsResolved() || !resolved.actor || !resolved.target || !resolved.map) {
            result.result = ActionResult::Impossible;
            result.reason = "test_core_action_context_incomplete";
        } else if (rejectPreflight) {
            result.result = ActionResult::Impossible;
            result.reason = "test_core_action_preflight_veto";
        } else {
            result.result = ActionResult::Success;
            result.reason = "test_core_action_preflight_passed";
        }
        return result;
    }
    ActionExecutionResult ExecuteCore(DecisionContext&, ActionCandidate const&,
                                      CoreActionResolution const& resolved) override {
        ++executeCoreCalls;
        executeCoreSawResolvedContext = resolved.IsResolved() &&
            resolved.actor && resolved.target && resolved.map;
        ActionExecutionResult result;
        result.result = executeCoreSawResolvedContext
            ? ActionResult::Success : ActionResult::Impossible;
        result.reason = executeCoreSawResolvedContext
            ? "test_core_action_executed" : "test_core_action_execute_missing_context";
        return result;
    }
private:
    std::string m_name;
};

void TestPartyBotEnginePublishesItemUseRootAndIndependentMutation()
{
    PartyBotEngine::PartyBotEngine engine;
    auto listener = std::make_shared<CaptureItemUseTerminalListener>();
    assert(engine.AddItemUseTerminalListener(listener));

    ItemUseId root = 0;
    assert(engine.BeginItemUse(ItemUseRequest(), root) ==
           ItemUseTrackerResult::Accepted);
    assert(root != 0);
    assert(engine.GetPendingItemUseCount() == 1);

    ItemUseChildId child = 0;
    assert(engine.RegisterItemUseChild(root, 0, 439, false, child) ==
           ItemUseTrackerResult::Accepted);

    ItemUseMutationEvent mutation;
    mutation.itemUseId = root;
    mutation.childId = child;
    mutation.itemGuid = 200;
    mutation.itemEntry = 118;
    mutation.kind = ItemUseMutationKind::ChargeChanged;
    mutation.phase = ItemUseMutationPhase::AfterEffects;
    mutation.slotIndex = 0;
    mutation.beforeCharges = -2;
    mutation.afterCharges = -1;
    mutation.itemExistsAfter = true;
    mutation.occurredAtMs = 1010;
    assert(engine.RecordItemUseMutation(mutation) == ItemUseTrackerResult::Accepted);

    ItemUseTerminalEvent terminal;
    assert(engine.RecordItemUseChildTerminal(
        root, child, ItemUseChildStatus::Succeeded, 1011) ==
        ItemUseTrackerResult::Accepted);
    assert(listener->events.empty());
    assert(engine.CloseItemUseDispatch(root, 1012) ==
           ItemUseTrackerResult::Completed);

    assert(engine.GetPendingItemUseCount() == 0);
    assert(listener->events.size() == 1);
    assert(listener->events[0].itemUseId == root);
    assert(listener->events[0].status == ItemUseAggregateStatus::AllSucceeded);
    assert(listener->events[0].observedMutationCount == 1);
    assert(listener->events[0].succeededChildCount == 1);

    ItemUseMutationEvent emitted;
    assert(engine.PopItemUseMutationEvent(emitted));
    assert(emitted.itemUseId == root);
    assert(emitted.childId == child);
    assert(emitted.kind == ItemUseMutationKind::ChargeChanged);
    assert(emitted.beforeCharges == -2 && emitted.afterCharges == -1);
    assert(!engine.PopItemUseMutationEvent(emitted));
}
void TestCoreActionRuntimeIsPreservedAcrossDecisionAndPendingContexts()
{
    Fixture f;
    TestCoreActionRuntime runtime;
    f.context.coreActionRuntime = &runtime;

    // Evaluate() and ExecuteSelected() each make an internal context copy.
    PartyBotEngine::PartyBotEngine immediateEngine;
    auto immediateAction = std::make_shared<TestCoreAction>("core_immediate");
    assert(immediateEngine.RegisterAction(immediateAction));
    auto trigger = std::make_shared<TestTrigger>("core_action_trigger");
    trigger->generated.push_back(
        Candidate("core_immediate", 20.0f, ActionChannel::Utility));
    auto strategy = std::make_shared<Strategy>("core_action_strategy");
    strategy->AddTrigger(trigger);
    immediateEngine.AddStrategy(strategy);

    DecisionReport report = immediateEngine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    assert(immediateAction->possibleChecks == 1);
    assert(immediateAction->usefulChecks == 1);
    assert(report.candidates[0].status == CandidateStatus::Selected);
    immediateEngine.ExecuteSelected(f.context, report);
    assert(report.candidates[0].status == CandidateStatus::Executed);
    assert(immediateAction->preflightChecks == 1);
    assert(immediateAction->executeCoreCalls == 1);
    assert(immediateAction->executeCoreSawResolvedContext);
    assert(report.candidates[0].reason == "test_core_action_executed");
    assert(runtime.resolveCalls == 1);

    // QueuePendingAction() and ExecutePendingActions() also create internal
    // copies. Eligibility must be revalidated and execution must re-resolve.
    PartyBotEngine::PartyBotEngine pendingEngine;
    auto pendingAction = std::make_shared<TestCoreAction>("core_pending");
    assert(pendingEngine.RegisterAction(pendingAction));
    ActionCandidate pendingCandidate =
        Candidate("core_pending", 15.0f, ActionChannel::Utility);
    assert(pendingEngine.QueuePendingAction(f.context, pendingCandidate) ==
           PendingActionEnqueueResult::Queued);
    assert(pendingAction->possibleChecks == 1);
    assert(pendingAction->usefulChecks == 1);
    assert(pendingEngine.GetPendingActionCount() == 1);

    PendingActionDispatchSummary pending =
        pendingEngine.ExecutePendingActions(f.context);
    assert(pending.dequeued == 1);
    assert(pending.executed == 1);
    assert(pending.rejected == 0);
    assert(pendingAction->possibleChecks == 2);
    assert(pendingAction->usefulChecks == 2);
    assert(pendingAction->preflightChecks == 1);
    assert(pendingAction->executeCoreCalls == 1);
    assert(pendingAction->executeCoreSawResolvedContext);
    assert(pending.completedCandidates.size() == 1);
    assert(pending.completedCandidates[0].reason == "test_core_action_executed");
    assert(runtime.resolveCalls == 2);
}

void TestCoreActionPreflightVetoPreventsSideEffectAndPreservesReason()
{
    Fixture f;
    TestCoreActionRuntime runtime;
    f.context.coreActionRuntime = &runtime;

    PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestCoreAction>("core_preflight_veto");
    action->rejectPreflight = true;
    assert(engine.RegisterAction(action));
    auto trigger = std::make_shared<TestTrigger>("core_preflight_veto_trigger");
    trigger->generated.push_back(
        Candidate("core_preflight_veto", 20.0f, ActionChannel::Utility));
    auto strategy = std::make_shared<Strategy>("core_preflight_veto_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    engine.ExecuteSelected(f.context, report);
    assert(report.candidates[0].status == CandidateStatus::ExecutionFailed);
    assert(report.candidates[0].actionResult == ActionResult::Impossible);
    assert(report.candidates[0].reason == "test_core_action_preflight_veto");
    assert(action->preflightChecks == 1);
    assert(action->executeCoreCalls == 0);
    assert(runtime.resolveCalls == 1);
}

void TestActionQueueIsBoundedPriorityOrderedAndStable()
{
    ActionQueue queue;
    ActionQueue::Entry entry;
    assert(queue.Empty());
    assert(!queue.PopNext(entry));

    assert(queue.Push(10, 10.0f));
    assert(queue.Push(11, 30.0f));
    assert(queue.Push(12, 30.0f));
    assert(queue.Push(13, 20.0f));
    assert(queue.Size() == 4);

    assert(queue.PopNext(entry) && entry.reportIndex == 11);
    assert(queue.PopNext(entry) && entry.reportIndex == 12); // Stable FIFO for equal priority.
    assert(queue.PopNext(entry) && entry.reportIndex == 13);
    assert(queue.PopNext(entry) && entry.reportIndex == 10);
    assert(queue.Empty());

    for (std::size_t i = 0; i < ActionQueue::Capacity; ++i)
        assert(queue.Push(i, static_cast<float>(i)));
    assert(queue.Size() == ActionQueue::Capacity);
    assert(!queue.Push(999, 1000.0f)); // Full queues fail closed; no queued item is evicted.
    assert(!queue.Push(1000, std::numeric_limits<float>::quiet_NaN()));
    assert(queue.Size() == ActionQueue::Capacity);
    queue.Clear();
    assert(queue.Empty());
}

void TestExecuteSelectedDispatchesByPriorityWithStableTies()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    DecisionReport report;
    report.snapshotEpoch = f.snapshot.epoch;
    std::vector<std::string> executionOrder;

    auto appendSelected = [&](std::string const& name, float priority) {
        auto action = std::make_shared<TestAction>(name);
        action->executionOrder = &executionOrder;
        assert(engine.RegisterAction(action));
        CandidateEvaluation evaluation;
        evaluation.candidate = Candidate(name, priority, ActionChannel::Utility);
        evaluation.candidate.finalPriority = priority;
        evaluation.status = CandidateStatus::Selected;
        evaluation.threatImpact.completeness = ThreatImpactCompleteness::NoThreat;
        evaluation.threatImpact.snapshotEpoch = f.snapshot.epoch;
        std::size_t const index = report.candidates.size();
        report.candidates.push_back(evaluation);
        report.selectedIndices.push_back(index);
    };

    // Deliberately make report order differ from execution priority.
    appendSelected("low", 10.0f);
    appendSelected("high", 30.0f);
    appendSelected("tie_first", 30.0f);
    appendSelected("tie_second", 30.0f);
    appendSelected("middle", 20.0f);

    engine.ExecuteSelected(f.context, report);

    std::vector<std::string> const expected = {
        "high", "tie_first", "tie_second", "middle", "low"
    };
    assert(executionOrder == expected);
    for (CandidateEvaluation const& evaluation : report.candidates) {
        assert(evaluation.status == CandidateStatus::Executed);
        assert(evaluation.actionResult == ActionResult::Success);
    }
}

void TestExecuteSelectedHonorsPerDispatchLimit()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    DecisionReport report;
    report.snapshotEpoch = f.snapshot.epoch;
    std::vector<std::string> executionOrder;

    for (std::size_t i = 0; i < 10; ++i) {
        std::string const name = "bounded_" + std::to_string(i);
        auto action = std::make_shared<TestAction>(name);
        action->executionOrder = &executionOrder;
        assert(engine.RegisterAction(action));
        CandidateEvaluation evaluation;
        evaluation.candidate = Candidate(name, static_cast<float>(i + 1), ActionChannel::Utility);
        evaluation.candidate.finalPriority = static_cast<float>(i + 1);
        evaluation.status = CandidateStatus::Selected;
        evaluation.threatImpact.completeness = ThreatImpactCompleteness::NoThreat;
        evaluation.threatImpact.snapshotEpoch = f.snapshot.epoch;
        report.candidates.push_back(evaluation);
        report.selectedIndices.push_back(i);
    }

    engine.ExecuteSelected(f.context, report);

    assert(executionOrder.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        std::string const expected = "bounded_" + std::to_string(9 - i);
        assert(executionOrder[i] == expected);
        assert(report.candidates[9 - i].status == CandidateStatus::Executed);
    }
    assert(report.candidates[0].status == CandidateStatus::Selected);
    assert(report.candidates[1].status == CandidateStatus::Selected);
}

void TestPendingActionQueueDeduplicatesAndPreservesOriginalExpiry()
{
    PendingActionQueue queue;
    ActionCandidate candidate = Candidate("heal", 10.0f, ActionChannel::GlobalCooldown);
    candidate.strategyName = "routine_heal";
    candidate.spellId = 2050;
    candidate.finalPriority = 10.0f;

    assert(queue.Enqueue(candidate, 1000, 200) == PendingActionEnqueueResult::Queued);
    assert(queue.Size() == 1);

    ActionCandidate refreshed = candidate;
    refreshed.strategyName = "emergency_heal";
    refreshed.finalPriority = 30.0f;
    // Same logical action identity updates its current payload/priority, but
    // does not get a new FIFO position or a later expiry deadline.
    assert(queue.Enqueue(refreshed, 1050, 500) ==
           PendingActionEnqueueResult::UpdatedExisting);
    assert(queue.Size() == 1);

    PendingActionQueue::Entry entry;
    assert(queue.PopNext(1199, entry));
    assert(entry.candidate.strategyName == "emergency_heal");
    assert(entry.candidate.spellId == 2050);
    assert(entry.priority == 30.0f);
    assert(entry.enqueuedAtMs == 1000);
    assert(entry.expiresAtMs == 1200);

    PendingActionQueue expiryQueue;
    assert(expiryQueue.Enqueue(candidate, 1000, 200) == PendingActionEnqueueResult::Queued);
    assert(expiryQueue.Enqueue(refreshed, 1050, 500) ==
           PendingActionEnqueueResult::UpdatedExisting);
    assert(!expiryQueue.PopNext(1200, entry));
    assert(expiryQueue.Empty());
}

void TestPendingActionQueuePriorityFifoExpiryAndClockWrap()
{
    PendingActionQueue queue;
    PendingActionQueue::Entry entry;
    ActionCandidate low = Candidate("low", 10.0f, ActionChannel::Utility);
    ActionCandidate high = Candidate("high", 30.0f, ActionChannel::Utility);
    ActionCandidate tie = Candidate("tie", 30.0f, ActionChannel::Utility);
    low.finalPriority = 10.0f;
    high.finalPriority = 30.0f;
    tie.finalPriority = 30.0f;

    assert(queue.Enqueue(low, 100, 1000) == PendingActionEnqueueResult::Queued);
    assert(queue.Enqueue(high, 100, 1000) == PendingActionEnqueueResult::Queued);
    assert(queue.Enqueue(tie, 100, 1000) == PendingActionEnqueueResult::Queued);
    assert(queue.PopNext(100, entry) && entry.candidate.actionName == "high");
    assert(queue.PopNext(100, entry) && entry.candidate.actionName == "tie");
    assert(queue.PopNext(100, entry) && entry.candidate.actionName == "low");

    assert(queue.Enqueue(low, 1000, 100) == PendingActionEnqueueResult::Queued);
    assert(queue.PruneExpired(1099) == 0);
    assert(queue.PruneExpired(1100) == 1);
    assert(queue.Empty());

    PendingActionQueue wrapped;
    std::uint32_t const start = 0xfffffff0u;
    assert(wrapped.Enqueue(low, start, 32) == PendingActionEnqueueResult::Queued);
    assert(wrapped.PruneExpired(0x0fu) == 0);
    assert(wrapped.PruneExpired(0x10u) == 1);
    assert(wrapped.Empty());
}

void TestPendingActionQueueAgePromotionPreventsPriorityStarvation()
{
    PendingActionQueue queue;
    PendingActionQueue::Entry entry;
    ActionCandidate oldest = Candidate("oldest", 1.0f, ActionChannel::Utility);
    ActionCandidate freshHigh = Candidate("fresh_high", 1000.0f, ActionChannel::Utility);
    ActionCandidate nextHigh = Candidate("next_high", 2000.0f, ActionChannel::Utility);
    oldest.finalPriority = 1.0f;
    freshHigh.finalPriority = 1000.0f;
    nextHigh.finalPriority = 2000.0f;

    assert(queue.Enqueue(oldest, 1000, 10000) == PendingActionEnqueueResult::Queued);
    assert(queue.Enqueue(freshHigh, 3999, 10000) == PendingActionEnqueueResult::Queued);
    // At 2,999 ms the oldest action has not reached the fairness boundary.
    assert(queue.PopNext(3999, entry) && entry.candidate.actionName == "fresh_high");

    assert(queue.Enqueue(nextHigh, 4000, 10000) == PendingActionEnqueueResult::Queued);
    // At exactly 3,000 ms, age promotion outranks a newly arrived higher priority action.
    assert(queue.PopNext(4000, entry) && entry.candidate.actionName == "oldest");
    assert(queue.PopNext(4000, entry) && entry.candidate.actionName == "next_high");

    PendingActionQueue promoted;
    ActionCandidate older = Candidate("older", 1.0f, ActionChannel::Utility);
    ActionCandidate newer = Candidate("newer", 5000.0f, ActionChannel::Utility);
    older.finalPriority = 1.0f;
    newer.finalPriority = 5000.0f;
    assert(promoted.Enqueue(older, 100, 10000) == PendingActionEnqueueResult::Queued);
    assert(promoted.Enqueue(newer, 1000, 10000) == PendingActionEnqueueResult::Queued);
    // When both are promoted, longest wait wins even if its priority is lower.
    assert(promoted.PopNext(3100, entry) && entry.candidate.actionName == "older");

    PendingActionQueue wrapped;
    std::uint32_t const start = 0xfffffff0u;
    assert(wrapped.Enqueue(oldest, start, 10000) == PendingActionEnqueueResult::Queued);
    assert(wrapped.Enqueue(freshHigh, start + 2999u, 10000) ==
           PendingActionEnqueueResult::Queued);
    assert(wrapped.PopNext(start + 2999u, entry) &&
           entry.candidate.actionName == "fresh_high");
    assert(wrapped.Enqueue(nextHigh, start + 3000u, 10000) ==
           PendingActionEnqueueResult::Queued);
    assert(wrapped.PopNext(start + 3000u, entry) &&
           entry.candidate.actionName == "oldest");
}

void BeginContinuationTestEncounter(Fixture& f)
{
    f.snapshot.epoch = 8;
    f.bot.inCombat = true;
    f.snapshot.members.clear();
    f.snapshot.members.push_back(f.bot);
    EncounterTransition const started =
        f.coordinator.ObserveEncounter(f.snapshot, f.context.nowMs, 10000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(started.encounterId != 0);
}

ActionCandidate MakeContinuationParent(ActionId actionId = 77)
{
    ActionCandidate parent = Candidate("parent_spell", 50.0f, ActionChannel::GlobalCooldown);
    parent.actionId = actionId;
    parent.targetGuid = 22;
    parent.spellId = 2050;
    return parent;
}

ActionCandidate MakeContinuationCandidate()
{
    ActionCandidate candidate = Candidate("followup_action", 30.0f, ActionChannel::Utility);
    candidate.finalPriority = 30.0f;
    return candidate;
}

void TestCoreSpellSuccessReleasesAndExecutesBoundedContinuation()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("followup_action");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));

    ActionCandidate const parent = MakeContinuationParent();
    ActionCandidate const continuation = MakeContinuationCandidate();
    assert(engine.QueueActionContinuation(f.context, parent, continuation, 1500) ==
           ActionContinuationEnqueueResult::Queued);
    assert(engine.GetPendingContinuationCount() == 1);
    assert(engine.GetPendingActionCount() == 0);

    CoreSpellTerminalEvent terminal;
    terminal.actionId = parent.actionId;
    terminal.encounterId = f.coordinator.GetEncounterId();
    terminal.targetGuid = parent.targetGuid;
    terminal.spellId = parent.spellId;
    terminal.occurredAtMs = 1200;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(terminal);

    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 1);
    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1201);
    PendingActionDispatchSummary const dispatched = engine.ExecutePendingActions(nextTick);
    assert(dispatched.dequeued == 1 && dispatched.selected == 1 && dispatched.executed == 1);
    assert(child->executeCount == 1);
    assert(engine.GetPendingActionCount() == 0);
}

void TestFailedInterruptedAndMismatchedTerminalsNeverReleaseContinuations()
{
    CoreSpellTerminalStatus const failures[] = {
        CoreSpellTerminalStatus::Failed,
        CoreSpellTerminalStatus::Interrupted
    };
    for (CoreSpellTerminalStatus status : failures)
    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        ActionCandidate const parent = MakeContinuationParent();
        assert(engine.QueueActionContinuation(
            f.context, parent, MakeContinuationCandidate(), 1500) ==
            ActionContinuationEnqueueResult::Queued);
        CoreSpellTerminalEvent terminal;
        terminal.actionId = parent.actionId;
        terminal.encounterId = f.coordinator.GetEncounterId();
        terminal.targetGuid = parent.targetGuid;
        terminal.spellId = parent.spellId;
        terminal.occurredAtMs = 1200;
        terminal.status = status;
        engine.NotifyCoreSpellTerminal(terminal);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);
        DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1201);
        engine.ExecutePendingActions(nextTick);
        assert(child->executeCount == 0);
    }

    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("followup_action");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));
    ActionCandidate const parent = MakeContinuationParent();
    assert(engine.QueueActionContinuation(
        f.context, parent, MakeContinuationCandidate(), 1500) ==
        ActionContinuationEnqueueResult::Queued);
    CoreSpellTerminalEvent mismatched;
    mismatched.actionId = parent.actionId;
    mismatched.encounterId = f.coordinator.GetEncounterId();
    mismatched.targetGuid = parent.targetGuid + 1;
    mismatched.spellId = parent.spellId;
    mismatched.occurredAtMs = 1200;
    mismatched.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(mismatched);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 0);
}

void TestContinuationExpiryEncounterIsolationCapacityAndLateTerminalRace()
{
    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        ActionCandidate const parent = MakeContinuationParent();
        assert(engine.QueueActionContinuation(
            f.context, parent, MakeContinuationCandidate(), 100) ==
            ActionContinuationEnqueueResult::Queued);
        CoreSpellTerminalEvent expired;
        expired.actionId = parent.actionId;
        expired.encounterId = f.coordinator.GetEncounterId();
        expired.targetGuid = parent.targetGuid;
        expired.spellId = parent.spellId;
        expired.occurredAtMs = 1100;
        expired.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(expired);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        ActionCandidate const parent = MakeContinuationParent();
        assert(engine.QueueActionContinuation(
            f.context, parent, MakeContinuationCandidate(), 1000) ==
            ActionContinuationEnqueueResult::Queued);
        CoreSpellTerminalEvent foreign;
        foreign.actionId = parent.actionId;
        foreign.encounterId = f.coordinator.GetEncounterId() + 1;
        foreign.targetGuid = parent.targetGuid;
        foreign.spellId = parent.spellId;
        foreign.occurredAtMs = 1100;
        foreign.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(foreign);
        assert(engine.GetPendingContinuationCount() == 1);
        assert(engine.GetPendingActionCount() == 0);
        engine.ResetDecisionState();
        assert(engine.GetPendingContinuationCount() == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        assert(engine.QueueActionContinuation(
            f.context, MakeContinuationParent(500), MakeContinuationCandidate(), 100) ==
            ActionContinuationEnqueueResult::Queued);
        DecisionContext afterExpiry(f.snapshot, f.bot, f.coordinator, 1100);
        PendingActionDispatchSummary const pruned = engine.ExecutePendingActions(afterExpiry);
        assert(pruned.expiredContinuations == 1);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0 && child->executeCount == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        ActionCandidate const parent = MakeContinuationParent(199);
        CoreSpellTerminalEvent alreadyFinished;
        alreadyFinished.actionId = parent.actionId;
        alreadyFinished.encounterId = f.coordinator.GetEncounterId();
        alreadyFinished.targetGuid = parent.targetGuid;
        alreadyFinished.spellId = parent.spellId;
        alreadyFinished.occurredAtMs = 1100;
        alreadyFinished.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(alreadyFinished);
        DecisionContext tooLate(f.snapshot, f.bot, f.coordinator, 2200);
        assert(engine.QueueActionContinuation(
            tooLate, parent, MakeContinuationCandidate(), 1000) ==
            ActionContinuationEnqueueResult::Expired);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 5000;
        assert(engine.RegisterAction(child));
        ActionCandidate const continuation = MakeContinuationCandidate();
        for (ActionId id = 1; id <= ActionContinuationQueue::Capacity; ++id) {
            ActionCandidate parent = MakeContinuationParent(id);
            assert(engine.QueueActionContinuation(
                f.context, parent, continuation, 4000) ==
                ActionContinuationEnqueueResult::Queued);
        }
        assert(engine.GetPendingContinuationCount() == ActionContinuationQueue::Capacity);
        assert(engine.QueueActionContinuation(
            f.context, MakeContinuationParent(33), continuation, 4000) ==
            ActionContinuationEnqueueResult::Full);
        assert(engine.QueueActionContinuation(
            f.context, MakeContinuationParent(1), continuation, 4000) ==
            ActionContinuationEnqueueResult::DuplicateParent);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("followup_action");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        ActionCandidate const parent = MakeContinuationParent(99);
        CoreSpellTerminalEvent alreadyFinished;
        alreadyFinished.actionId = parent.actionId;
        alreadyFinished.encounterId = f.coordinator.GetEncounterId();
        alreadyFinished.targetGuid = parent.targetGuid;
        alreadyFinished.spellId = parent.spellId;
        alreadyFinished.occurredAtMs = 1100;
        alreadyFinished.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(alreadyFinished);
        DecisionContext afterTerminal(f.snapshot, f.bot, f.coordinator, 1200);
        assert(engine.QueueActionContinuation(
            afterTerminal, parent, MakeContinuationCandidate(), 1000) ==
            ActionContinuationEnqueueResult::ReleasedImmediately);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 1);

        PartySnapshot foreignSnapshot = f.snapshot;
        PartyMemberSnapshot foreignBot = f.bot;
        foreignSnapshot.groupId += 1;
        foreignSnapshot.mapId += 1;
        foreignSnapshot.instanceId += 1;
        foreignBot.mapId = foreignSnapshot.mapId;
        foreignBot.instanceId = foreignSnapshot.instanceId;
        DecisionContext foreignContext(foreignSnapshot, foreignBot, f.coordinator, 1201);
        PendingActionDispatchSummary const stale =
            engine.ExecutePendingActions(foreignContext);
        assert(stale.staleContext == 1 && stale.executed == 0);
        assert(child->executeCount == 0 && engine.GetPendingActionCount() == 0);
    }
}

ActionTerminalPrerequisite MakePrerequisite(
    ActionId actionId, std::uint64_t encounterId, Guid targetGuid,
    std::uint32_t spellId, CoreSpellTerminalStatus status)
{
    ActionTerminalPrerequisite prerequisite;
    prerequisite.actionId = actionId;
    prerequisite.encounterId = encounterId;
    prerequisite.targetGuid = targetGuid;
    prerequisite.spellId = spellId;
    prerequisite.requiredStatus = status;
    return prerequisite;
}

ActionDependencyRequest MakeDependencyRequest(
    std::uint32_t lifetimeMs, ActionTerminalPrerequisite const& prerequisite)
{
    ActionDependencyRequest request;
    request.lifetimeMs = lifetimeMs;
    request.prerequisites.push_back(prerequisite);
    return request;
}

void TestRejectedContinuationIsTombstonedUntilQueueReset()
{
    ActionContinuationQueue queue;
    PendingActionQueue::Context scope;
    scope.groupId = 5;
    scope.mapId = 33;
    scope.instanceId = 101;
    scope.botGuid = 11;
    scope.encounterId = 77;
    scope.encounterActive = true;
    scope.scoped = true;

    ActionTerminalPrerequisite const parent = MakePrerequisite(
        2101, 77, 22, 2050, CoreSpellTerminalStatus::Succeeded);
    ActionDependencyRequest request = MakeDependencyRequest(1000, parent);
    ActionCandidate candidate = Candidate(
        "rejected_parent_child", 20.0f, ActionChannel::Utility);
    candidate.finalPriority = 20.0f;

    assert(queue.Enqueue(request, candidate, 1000, scope, 1000) ==
           ActionContinuationEnqueueResult::Queued);
    CoreSpellTerminalEvent failed;
    failed.actionId = parent.actionId;
    failed.encounterId = parent.encounterId;
    failed.targetGuid = parent.targetGuid;
    failed.spellId = parent.spellId;
    failed.occurredAtMs = 1100;
    failed.status = CoreSpellTerminalStatus::Failed;
    queue.ObserveTerminal(failed);

    std::vector<ActionContinuationQueue::Entry> ready;
    queue.CollectReady(ready);
    assert(ready.empty() && queue.Empty());
    assert(queue.Enqueue(request, candidate, 1200, scope, 1200) ==
           ActionContinuationEnqueueResult::DuplicateParent);

    // A reset starts a new lifecycle and clears the settled-key history.
    queue.Clear();
    assert(queue.Enqueue(request, candidate, 1200, scope, 1200) ==
           ActionContinuationEnqueueResult::Queued);
}

void TestCoreTerminalCannotSatisfyActionResultPrerequisite()
{
    ActionContinuationQueue queue;
    PendingActionQueue::Context scope;
    scope.groupId = 5;
    scope.mapId = 33;
    scope.instanceId = 101;
    scope.botGuid = 11;
    scope.encounterId = 77;
    scope.encounterActive = true;
    scope.scoped = true;

    ActionDependencyRequest request;
    request.lifetimeMs = 1000;
    ActionDependencyPrerequisite prerequisite;
    prerequisite.source = ActionDependencyEventSource::ActionResult;
    prerequisite.actionId = 2001;
    prerequisite.encounterId = 77;
    prerequisite.targetGuid = 22;
    prerequisite.spellId = 2050;
    prerequisite.expectedActionName = "action_result_parent";
    prerequisite.requiredActionResult = ActionResult::Success;
    prerequisite.requiredAttempted = true;
    request.prerequisites.push_back(prerequisite);

    ActionCandidate candidate = Candidate(
        "action_result_child", 20.0f, ActionChannel::Utility);
    candidate.finalPriority = 20.0f;
    assert(queue.Enqueue(request, candidate, 1000, scope, 1000) ==
           ActionContinuationEnqueueResult::Queued);

    CoreSpellTerminalEvent terminal;
    terminal.actionId = 2001;
    terminal.encounterId = 77;
    terminal.targetGuid = 22;
    terminal.spellId = 2050;
    terminal.occurredAtMs = 1100;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    queue.ObserveTerminal(terminal);

    std::vector<ActionContinuationQueue::Entry> ready;
    queue.CollectReady(ready);
    assert(ready.empty());
    assert(queue.Size() == 1);

    ActionResultEvent result;
    result.actionId = 2001;
    result.encounterId = 77;
    result.candidate = Candidate(
        "action_result_parent", 20.0f, ActionChannel::GlobalCooldown);
    result.candidate.spellId = 2050;
    result.candidate.actionId = 2001;
    result.result = ActionResult::Success;
    result.attempted = true;
    result.occurredAtMs = 1200;
    queue.ObserveActionResult(result);
    queue.CollectReady(ready);
    assert(ready.size() == 1);
    assert(queue.Empty());
}

void TestActionResultAndCoreSpellPrerequisitesAreDistinctAndAnded()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("mixed_result_and_spell_child");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));

    ActionContinuationEnqueueResult enqueueResult = ActionContinuationEnqueueResult::Invalid;
    auto parent = std::make_shared<DependencyRegisteringTestAction>(
        "mixed_result_and_spell_parent", "mixed_result_and_spell_child", enqueueResult);
    assert(engine.RegisterAction(parent));

    auto trigger = std::make_shared<TestTrigger>("mixed_result_and_spell_trigger");
    ActionCandidate parentCandidate = Candidate(
        "mixed_result_and_spell_parent", 50.0f, ActionChannel::GlobalCooldown);
    parentCandidate.spellId = 2050;
    trigger->generated.push_back(parentCandidate);
    auto strategy = std::make_shared<Strategy>("mixed_result_and_spell_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    engine.ExecuteSelected(f.context, report);
    ActionId const parentActionId =
        report.candidates[report.selectedIndices[0]].actionId;
    assert(parentActionId != 0);
    assert(enqueueResult == ActionContinuationEnqueueResult::Queued);

    // The ActionResult event is generated synchronously after Execute returns.
    // It must satisfy only the typed ActionResult prerequisite, not the Core
    // Spell::finish prerequisite, even though both share one action ID.
    assert(engine.GetPendingContinuationCount() == 1);
    assert(engine.GetPendingActionCount() == 0);

    CoreSpellTerminalEvent terminal;
    terminal.actionId = parentActionId;
    terminal.encounterId = f.coordinator.GetEncounterId();
    terminal.targetGuid = parentCandidate.targetGuid;
    terminal.spellId = parentCandidate.spellId;
    terminal.occurredAtMs = 1100;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(terminal);

    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 1);
    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1101);
    PendingActionDispatchSummary const dispatch = engine.ExecutePendingActions(nextTick);
    assert(dispatch.selected == 1 && dispatch.executed == 1);
    assert(child->executeCount == 1);
}

void TestLateActionResultDependencyRegistrationUsesRecentEventHistory()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto parent = std::make_shared<TestAction>("late_action_result_parent");
    auto child = std::make_shared<TestAction>("late_action_result_child");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(parent) && engine.RegisterAction(child));

    auto trigger = std::make_shared<TestTrigger>("late_action_result_trigger");
    ActionCandidate parentCandidate = Candidate(
        "late_action_result_parent", 50.0f, ActionChannel::GlobalCooldown);
    parentCandidate.spellId = 2050;
    trigger->generated.push_back(parentCandidate);
    auto strategy = std::make_shared<Strategy>("late_action_result_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    engine.ExecuteSelected(f.context, report);
    ActionId const parentActionId = report.candidates[report.selectedIndices[0]].actionId;
    assert(parentActionId != 0);

    CoreSpellTerminalEvent terminal;
    terminal.actionId = parentActionId;
    terminal.encounterId = f.coordinator.GetEncounterId();
    terminal.targetGuid = parentCandidate.targetGuid;
    terminal.spellId = parentCandidate.spellId;
    terminal.occurredAtMs = 1050;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(terminal);

    ActionDependencyRequest request;
    request.lifetimeMs = 1200;
    ActionDependencyPrerequisite actionResult;
    actionResult.source = ActionDependencyEventSource::ActionResult;
    actionResult.actionId = parentActionId;
    actionResult.encounterId = f.coordinator.GetEncounterId();
    actionResult.targetGuid = parentCandidate.targetGuid;
    actionResult.spellId = parentCandidate.spellId;
    actionResult.expectedActionName = parentCandidate.actionName;
    actionResult.requiredActionResult = ActionResult::Success;
    actionResult.requiredAttempted = true;
    request.prerequisites.push_back(actionResult);

    ActionDependencyPrerequisite spellTerminal;
    spellTerminal.source = ActionDependencyEventSource::CoreSpellTerminal;
    spellTerminal.actionId = parentActionId;
    spellTerminal.encounterId = f.coordinator.GetEncounterId();
    spellTerminal.targetGuid = parentCandidate.targetGuid;
    spellTerminal.spellId = parentCandidate.spellId;
    spellTerminal.requiredStatus = CoreSpellTerminalStatus::Succeeded;
    request.prerequisites.push_back(spellTerminal);

    ActionCandidate childCandidate = Candidate(
        "late_action_result_child", 25.0f, ActionChannel::Utility);
    childCandidate.finalPriority = 25.0f;
    DecisionContext lateContext(f.snapshot, f.bot, f.coordinator, 1100);
    assert(engine.QueueActionDependency(lateContext, request, childCandidate) ==
           ActionContinuationEnqueueResult::ReleasedImmediately);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 1);

    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1101);
    PendingActionDispatchSummary const dispatch = engine.ExecutePendingActions(nextTick);
    assert(dispatch.selected == 1 && dispatch.executed == 1);
    assert(child->executeCount == 1);
}

void TestPointMovementTerminalDependencyUsesCorrelationStatusAndHistory()
{
    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("movement_followup");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));
        auto listener = std::make_shared<CapturePointMovementTerminalListener>();
        assert(engine.AddPointMovementTerminalListener(listener));

        PointMovementTerminalEvent reached;
        reached.actionId = 8801;
        reached.encounterId = f.coordinator.GetEncounterId();
        reached.moverGuid = f.bot.guid;
        reached.pointId = 17;
        reached.status = PointMovementTerminalStatus::Reached;
        reached.occurredAtMs = 1100;
        reached.finalDistance = 0.25f;
        engine.NotifyPointMovementTerminal(reached);
        assert(listener->events.size() == 1);
        assert(listener->events[0].actionId == reached.actionId);
        assert(listener->events[0].status == PointMovementTerminalStatus::Reached);

        ActionDependencyRequest request;
        request.lifetimeMs = 1200;
        ActionDependencyPrerequisite prerequisite;
        prerequisite.source = ActionDependencyEventSource::PointMovementTerminal;
        prerequisite.actionId = reached.actionId;
        prerequisite.encounterId = reached.encounterId;
        prerequisite.targetGuid = reached.moverGuid;
        prerequisite.pointId = reached.pointId;
        prerequisite.requiredMovementStatus = PointMovementTerminalStatus::Reached;
        request.prerequisites.push_back(prerequisite);
        ActionCandidate continuation = Candidate(
            "movement_followup", 25.0f, ActionChannel::Utility);
        continuation.finalPriority = 25.0f;

        DecisionContext lateContext(
            f.snapshot, f.bot, f.coordinator, reached.occurredAtMs + 1);
        assert(engine.QueueActionDependency(lateContext, request, continuation) ==
               ActionContinuationEnqueueResult::ReleasedImmediately);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 1);

        DecisionContext nextTick(
            f.snapshot, f.bot, f.coordinator, reached.occurredAtMs + 2);
        PendingActionDispatchSummary const dispatched =
            engine.ExecutePendingActions(nextTick);
        assert(dispatched.selected == 1 && dispatched.executed == 1);
        assert(child->executeCount == 1);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("movement_interrupted_followup");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));

        ActionDependencyRequest request;
        request.lifetimeMs = 1200;
        ActionDependencyPrerequisite prerequisite;
        prerequisite.source = ActionDependencyEventSource::PointMovementTerminal;
        prerequisite.actionId = 8802;
        prerequisite.encounterId = f.coordinator.GetEncounterId();
        prerequisite.targetGuid = f.bot.guid;
        prerequisite.pointId = 18;
        prerequisite.requiredMovementStatus = PointMovementTerminalStatus::Reached;
        request.prerequisites.push_back(prerequisite);
        ActionCandidate continuation = Candidate(
            "movement_interrupted_followup", 25.0f, ActionChannel::Utility);
        continuation.finalPriority = 25.0f;
        assert(engine.QueueActionDependency(f.context, request, continuation) ==
               ActionContinuationEnqueueResult::Queued);

        PointMovementTerminalEvent interrupted;
        interrupted.actionId = prerequisite.actionId;
        interrupted.encounterId = prerequisite.encounterId;
        interrupted.moverGuid = prerequisite.targetGuid;
        interrupted.pointId = prerequisite.pointId;
        interrupted.status = PointMovementTerminalStatus::Interrupted;
        interrupted.occurredAtMs = 1100;
        interrupted.finalDistance = 4.0f;
        engine.NotifyPointMovementTerminal(interrupted);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);
        DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1101);
        engine.ExecutePendingActions(nextTick);
        assert(child->executeCount == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("movement_failed_arrival_followup");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));

        ActionDependencyRequest request;
        request.lifetimeMs = 1200;
        ActionDependencyPrerequisite prerequisite;
        prerequisite.source = ActionDependencyEventSource::PointMovementTerminal;
        prerequisite.actionId = 8803;
        prerequisite.encounterId = f.coordinator.GetEncounterId();
        prerequisite.targetGuid = f.bot.guid;
        prerequisite.pointId = 19;
        prerequisite.requiredMovementStatus = PointMovementTerminalStatus::Reached;
        request.prerequisites.push_back(prerequisite);
        ActionCandidate continuation = Candidate(
            "movement_failed_arrival_followup", 25.0f, ActionChannel::Utility);
        continuation.finalPriority = 25.0f;
        assert(engine.QueueActionDependency(f.context, request, continuation) ==
               ActionContinuationEnqueueResult::Queued);

        PointMovementTerminalEvent failedArrival;
        failedArrival.actionId = prerequisite.actionId;
        failedArrival.encounterId = prerequisite.encounterId;
        failedArrival.moverGuid = prerequisite.targetGuid;
        failedArrival.pointId = prerequisite.pointId;
        failedArrival.status = PointMovementTerminalStatus::Failed;
        failedArrival.occurredAtMs = 1100;
        failedArrival.finalDistance = 3.0f;
        engine.NotifyPointMovementTerminal(failedArrival);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);
        DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1101);
        engine.ExecutePendingActions(nextTick);
        assert(child->executeCount == 0);
    }
}
void TestItemUseTerminalDependencyUsesCorrelationStatusAndHistory()
{
    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("item_use_success_followup");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));

        ItemUseTerminalEvent terminal;
        terminal.actionId = 8804;
        terminal.encounterId = f.coordinator.GetEncounterId();
        terminal.itemUseId = 9001;
        terminal.actorGuid = f.bot.guid;
        terminal.status = ItemUseAggregateStatus::AllSucceeded;
        terminal.occurredAtMs = 1100;
        engine.NotifyItemUseTerminal(terminal);

        ActionDependencyRequest request;
        request.lifetimeMs = 1200;
        ActionDependencyPrerequisite prerequisite;
        prerequisite.source = ActionDependencyEventSource::ItemUseTerminal;
        prerequisite.actionId = terminal.actionId;
        prerequisite.encounterId = terminal.encounterId;
        prerequisite.itemUseId = terminal.itemUseId;
        prerequisite.requiredItemUseStatus = ItemUseAggregateStatus::AllSucceeded;
        request.prerequisites.push_back(prerequisite);

        ActionCandidate continuation = Candidate(
            "item_use_success_followup", 25.0f, ActionChannel::Utility);
        continuation.finalPriority = 25.0f;
        DecisionContext lateContext(
            f.snapshot, f.bot, f.coordinator, terminal.occurredAtMs + 1);
        assert(engine.QueueActionDependency(lateContext, request, continuation) ==
               ActionContinuationEnqueueResult::ReleasedImmediately);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 1);

        DecisionContext nextTick(
            f.snapshot, f.bot, f.coordinator, terminal.occurredAtMs + 2);
        PendingActionDispatchSummary const dispatched =
            engine.ExecutePendingActions(nextTick);
        assert(dispatched.selected == 1 && dispatched.executed == 1);
        assert(child->executeCount == 1);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("item_use_failure_followup");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));

        ActionDependencyRequest request;
        request.lifetimeMs = 1200;
        ActionDependencyPrerequisite prerequisite;
        prerequisite.source = ActionDependencyEventSource::ItemUseTerminal;
        prerequisite.actionId = 8805;
        prerequisite.encounterId = f.coordinator.GetEncounterId();
        prerequisite.itemUseId = 9002;
        prerequisite.requiredItemUseStatus = ItemUseAggregateStatus::AllSucceeded;
        request.prerequisites.push_back(prerequisite);
        ActionCandidate continuation = Candidate(
            "item_use_failure_followup", 25.0f, ActionChannel::Utility);
        continuation.finalPriority = 25.0f;
        assert(engine.QueueActionDependency(f.context, request, continuation) ==
               ActionContinuationEnqueueResult::Queued);
        assert(engine.GetPendingContinuationCount() == 1);

        // Same action/encounter but another itemUseId must not satisfy this
        // continuation. A matching root with a failed aggregate must reject it.
        ItemUseTerminalEvent unrelated;
        unrelated.actionId = prerequisite.actionId;
        unrelated.encounterId = prerequisite.encounterId;
        unrelated.itemUseId = 9003;
        unrelated.status = ItemUseAggregateStatus::AllSucceeded;
        unrelated.occurredAtMs = 1100;
        engine.NotifyItemUseTerminal(unrelated);
        assert(engine.GetPendingContinuationCount() == 1);
        assert(engine.GetPendingActionCount() == 0);

        ItemUseTerminalEvent failed;
        failed.actionId = prerequisite.actionId;
        failed.encounterId = prerequisite.encounterId;
        failed.itemUseId = prerequisite.itemUseId;
        failed.status = ItemUseAggregateStatus::AllFailed;
        failed.occurredAtMs = 1101;
        engine.NotifyItemUseTerminal(failed);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);

        DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1102);
        engine.ExecutePendingActions(nextTick);
        assert(child->executeCount == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("item_use_queued_success_followup");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));

        ActionDependencyRequest request;
        request.lifetimeMs = 1200;
        ActionDependencyPrerequisite prerequisite;
        prerequisite.source = ActionDependencyEventSource::ItemUseTerminal;
        prerequisite.actionId = 8806;
        prerequisite.encounterId = f.coordinator.GetEncounterId();
        prerequisite.itemUseId = 9004;
        prerequisite.requiredItemUseStatus = ItemUseAggregateStatus::AllSucceeded;
        request.prerequisites.push_back(prerequisite);
        ActionCandidate continuation = Candidate(
            "item_use_queued_success_followup", 25.0f, ActionChannel::Utility);
        continuation.finalPriority = 25.0f;

        assert(engine.QueueActionDependency(f.context, request, continuation) ==
               ActionContinuationEnqueueResult::Queued);
        assert(engine.GetPendingContinuationCount() == 1);
        assert(engine.GetPendingActionCount() == 0);

        ItemUseTerminalEvent terminal;
        terminal.actionId = prerequisite.actionId;
        terminal.encounterId = prerequisite.encounterId;
        terminal.itemUseId = prerequisite.itemUseId;
        terminal.status = ItemUseAggregateStatus::AllSucceeded;
        terminal.occurredAtMs = 1100;
        engine.NotifyItemUseTerminal(terminal);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 1);

        DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1101);
        PendingActionDispatchSummary const dispatched =
            engine.ExecutePendingActions(nextTick);
        assert(dispatched.selected == 1 && dispatched.executed == 1);
        assert(child->executeCount == 1);
    }
}

void TestFailedActionResultRejectsMixedDependency()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("failed_action_result_child");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));

    ActionContinuationEnqueueResult enqueueResult = ActionContinuationEnqueueResult::Invalid;
    auto parent = std::make_shared<DependencyRegisteringTestAction>(
        "failed_action_result_parent", "failed_action_result_child",
        enqueueResult, ActionResult::Failed);
    assert(engine.RegisterAction(parent));

    auto trigger = std::make_shared<TestTrigger>("failed_action_result_trigger");
    ActionCandidate parentCandidate = Candidate(
        "failed_action_result_parent", 50.0f, ActionChannel::GlobalCooldown);
    parentCandidate.spellId = 2050;
    trigger->generated.push_back(parentCandidate);
    auto strategy = std::make_shared<Strategy>("failed_action_result_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    engine.ExecuteSelected(f.context, report);
    assert(enqueueResult == ActionContinuationEnqueueResult::Queued);
    assert(report.candidates[report.selectedIndices[0]].actionResult == ActionResult::Failed);
    // A failed synchronous adapter result rejects the ActionResult branch;
    // a later successful Core Spell terminal cannot resurrect this dependency.
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 0);

    CoreSpellTerminalEvent terminal;
    terminal.actionId = report.candidates[report.selectedIndices[0]].actionId;
    terminal.encounterId = f.coordinator.GetEncounterId();
    terminal.targetGuid = parentCandidate.targetGuid;
    terminal.spellId = parentCandidate.spellId;
    terminal.occurredAtMs = 1100;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(terminal);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 0);
    assert(child->executeCount == 0);
}

void TestActionDependenciesRequireAllAndPrerequisites()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("followup_and");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));

    std::uint64_t const encounterId = f.coordinator.GetEncounterId();
    ActionCandidate candidate = Candidate("followup_and", 30.0f, ActionChannel::Utility);
    candidate.finalPriority = 30.0f;
    ActionDependencyRequest request;
    request.lifetimeMs = 1500;
    request.prerequisites.push_back(MakePrerequisite(
        1001, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded));
    request.prerequisites.push_back(MakePrerequisite(
        1002, encounterId, 23, 2061, CoreSpellTerminalStatus::Succeeded));

    assert(engine.QueueActionDependency(f.context, request, candidate) ==
           ActionContinuationEnqueueResult::Queued);
    assert(engine.GetPendingContinuationCount() == 1);

    CoreSpellTerminalEvent first;
    first.actionId = 1001;
    first.encounterId = encounterId;
    first.targetGuid = 22;
    first.spellId = 2050;
    first.occurredAtMs = 1100;
    first.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(first);
    assert(engine.GetPendingContinuationCount() == 1);
    assert(engine.GetPendingActionCount() == 0);

    CoreSpellTerminalEvent second;
    second.actionId = 1002;
    second.encounterId = encounterId;
    second.targetGuid = 23;
    second.spellId = 2061;
    second.occurredAtMs = 1200;
    second.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(second);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 1);

    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1201);
    PendingActionDispatchSummary const dispatch = engine.ExecutePendingActions(nextTick);
    assert(dispatch.selected == 1 && dispatch.executed == 1);
    assert(child->executeCount == 1);

    // The same completed parent set must not release a second child when a
    // caller retries after the original continuation has already dispatched.
    DecisionContext retryContext(f.snapshot, f.bot, f.coordinator, 1300);
    assert(engine.QueueActionDependency(retryContext, request, candidate) ==
           ActionContinuationEnqueueResult::DuplicateParent);
    ActionDependencyRequest reorderedRequest = request;
    std::reverse(reorderedRequest.prerequisites.begin(),
                 reorderedRequest.prerequisites.end());
    assert(engine.QueueActionDependency(retryContext, reorderedRequest, candidate) ==
           ActionContinuationEnqueueResult::DuplicateParent);
    assert(engine.GetPendingActionCount() == 0);
}

void TestLateDependencyRegistrationUsesLatestTerminalAndWrapSafeTime()
{
    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("late_multi_prerequisite");
        child->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(child));

        std::uint64_t const encounterId = f.coordinator.GetEncounterId();
        ActionDependencyRequest request;
        request.lifetimeMs = 300;
        request.prerequisites.push_back(MakePrerequisite(
            1601, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded));
        request.prerequisites.push_back(MakePrerequisite(
            1602, encounterId, 23, 2061, CoreSpellTerminalStatus::Succeeded));
        ActionCandidate candidate = Candidate(
            "late_multi_prerequisite", 30.0f, ActionChannel::Utility);
        candidate.finalPriority = 30.0f;

        // Completion notifications may arrive out of prerequisite order.
        CoreSpellTerminalEvent later;
        later.actionId = 1602;
        later.encounterId = encounterId;
        later.targetGuid = 23;
        later.spellId = 2061;
        later.occurredAtMs = 1500;
        later.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(later);

        CoreSpellTerminalEvent earlier;
        earlier.actionId = 1601;
        earlier.encounterId = encounterId;
        earlier.targetGuid = 22;
        earlier.spellId = 2050;
        earlier.occurredAtMs = 1100;
        earlier.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(earlier);

        DecisionContext lateContext(f.snapshot, f.bot, f.coordinator, 1700);
        assert(engine.QueueActionDependency(lateContext, request, candidate) ==
               ActionContinuationEnqueueResult::ReleasedImmediately);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 1);

        // The lifetime is anchored to the latest parent terminal (1500), not
        // to this late registration (1700) or the earlier terminal (1100).
        DecisionContext expiryContext(f.snapshot, f.bot, f.coordinator, 1800);
        PendingActionDispatchSummary const expired =
            engine.ExecutePendingActions(expiryContext);
        assert(expired.expired == 1 && expired.executed == 0);
        assert(engine.GetPendingActionCount() == 0);
        assert(child->executeCount == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto child = std::make_shared<TestAction>("wrap_safe_dependency");
        child->pendingLifetimeMs = 1000;
        assert(engine.RegisterAction(child));

        std::uint64_t const encounterId = f.coordinator.GetEncounterId();
        ActionDependencyRequest request;
        request.lifetimeMs = 40;
        request.prerequisites.push_back(MakePrerequisite(
            1701, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded));
        request.prerequisites.push_back(MakePrerequisite(
            1702, encounterId, 23, 2061, CoreSpellTerminalStatus::Succeeded));
        ActionCandidate candidate = Candidate(
            "wrap_safe_dependency", 30.0f, ActionChannel::Utility);
        candidate.finalPriority = 30.0f;

        CoreSpellTerminalEvent beforeWrap;
        beforeWrap.actionId = 1701;
        beforeWrap.encounterId = encounterId;
        beforeWrap.targetGuid = 22;
        beforeWrap.spellId = 2050;
        beforeWrap.occurredAtMs = 0xfffffff0u;
        beforeWrap.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(beforeWrap);

        CoreSpellTerminalEvent afterWrap;
        afterWrap.actionId = 1702;
        afterWrap.encounterId = encounterId;
        afterWrap.targetGuid = 23;
        afterWrap.spellId = 2061;
        afterWrap.occurredAtMs = 10u;
        afterWrap.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(afterWrap);

        DecisionContext afterWrapContext(f.snapshot, f.bot, f.coordinator, 40u);
        assert(engine.QueueActionDependency(afterWrapContext, request, candidate) ==
               ActionContinuationEnqueueResult::ReleasedImmediately);
        assert(engine.GetPendingActionCount() == 1);

        DecisionContext wrappedExpiry(f.snapshot, f.bot, f.coordinator, 50u);
        PendingActionDispatchSummary const expired =
            engine.ExecutePendingActions(wrappedExpiry);
        assert(expired.expired == 1 && expired.executed == 0);
        assert(engine.GetPendingActionCount() == 0);
        assert(child->executeCount == 0);
    }
}

void TestLateDependencyRegistrationRejectsKnownFailedParent()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("late_failed_parent_child");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));

    std::uint64_t const encounterId = f.coordinator.GetEncounterId();
    ActionDependencyRequest request;
    request.lifetimeMs = 1000;
    request.prerequisites.push_back(MakePrerequisite(
        1801, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded));
    request.prerequisites.push_back(MakePrerequisite(
        1802, encounterId, 23, 2061, CoreSpellTerminalStatus::Succeeded));
    ActionCandidate candidate = Candidate(
        "late_failed_parent_child", 30.0f, ActionChannel::Utility);
    candidate.finalPriority = 30.0f;

    // One AND prerequisite has already failed before the continuation is
    // registered; another prerequisite has not completed.
    CoreSpellTerminalEvent failed;
    failed.actionId = 1801;
    failed.encounterId = encounterId;
    failed.targetGuid = 22;
    failed.spellId = 2050;
    failed.occurredAtMs = 1100;
    failed.status = CoreSpellTerminalStatus::Failed;
    engine.NotifyCoreSpellTerminal(failed);

    DecisionContext lateContext(f.snapshot, f.bot, f.coordinator, 1200);
    assert(engine.QueueActionDependency(lateContext, request, candidate) ==
           ActionContinuationEnqueueResult::ParentTerminalRejected);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 0);
    assert(child->executeCount == 0);
}

void TestAlternativeDependencyKeepsRejectedBranchUntilGroupResolution()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto eligible = std::make_shared<TestAction>("alternative_eligible");
    auto rejected = std::make_shared<TestAction>("alternative_rejected");
    eligible->pendingLifetimeMs = rejected->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(eligible) && engine.RegisterAction(rejected));

    std::uint64_t const encounterId = f.coordinator.GetEncounterId();
    ActionTerminalPrerequisite const succeededParent = MakePrerequisite(
        1901, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded);
    ActionDependencyRequest eligibleRequest = MakeDependencyRequest(
        1000, succeededParent);
    eligibleRequest.alternativeGroupId = 703;
    eligibleRequest.alternativeIndex = 0;
    eligibleRequest.alternativeCount = 2;
    ActionDependencyRequest rejectedRequest = eligibleRequest;
    rejectedRequest.alternativeIndex = 1;
    rejectedRequest.prerequisites[0].requiredStatus = CoreSpellTerminalStatus::Failed;

    ActionCandidate eligibleCandidate = Candidate(
        "alternative_eligible", 10.0f, ActionChannel::Utility);
    ActionCandidate rejectedCandidate = Candidate(
        "alternative_rejected", 100.0f, ActionChannel::Utility);
    eligibleCandidate.finalPriority = 10.0f;
    rejectedCandidate.finalPriority = 100.0f;

    assert(engine.QueueActionDependency(f.context, eligibleRequest, eligibleCandidate) ==
           ActionContinuationEnqueueResult::Queued);

    CoreSpellTerminalEvent succeeded;
    succeeded.actionId = succeededParent.actionId;
    succeeded.encounterId = encounterId;
    succeeded.targetGuid = succeededParent.targetGuid;
    succeeded.spellId = succeededParent.spellId;
    succeeded.occurredAtMs = 1100;
    succeeded.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(succeeded);

    // A failed branch is normal in a one-of-N group. It must not be surfaced
    // as a globally rejected parent while another branch remains eligible.
    DecisionContext lateContext(f.snapshot, f.bot, f.coordinator, 1200);
    assert(engine.QueueActionDependency(lateContext, rejectedRequest, rejectedCandidate) ==
           ActionContinuationEnqueueResult::Queued);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 1);

    DecisionContext dispatchContext(f.snapshot, f.bot, f.coordinator, 1201);
    PendingActionDispatchSummary const dispatched =
        engine.ExecutePendingActions(dispatchContext);
    assert(dispatched.executed == 1);
    assert(eligible->executeCount == 1);
    assert(rejected->executeCount == 0);
}

void TestActionDependenciesRejectFailedAndInvalidAndPrerequisites()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    auto child = std::make_shared<TestAction>("followup_and_failure");
    child->pendingLifetimeMs = 2000;
    assert(engine.RegisterAction(child));

    std::uint64_t const encounterId = f.coordinator.GetEncounterId();
    ActionCandidate candidate = Candidate(
        "followup_and_failure", 20.0f, ActionChannel::Utility);
    candidate.finalPriority = 20.0f;
    ActionDependencyRequest request;
    request.lifetimeMs = 1500;
    request.prerequisites.push_back(MakePrerequisite(
        1101, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded));
    request.prerequisites.push_back(MakePrerequisite(
        1102, encounterId, 23, 2061, CoreSpellTerminalStatus::Succeeded));
    assert(engine.QueueActionDependency(f.context, request, candidate) ==
           ActionContinuationEnqueueResult::Queued);

    CoreSpellTerminalEvent success;
    success.actionId = 1101;
    success.encounterId = encounterId;
    success.targetGuid = 22;
    success.spellId = 2050;
    success.occurredAtMs = 1100;
    success.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(success);

    CoreSpellTerminalEvent failed;
    failed.actionId = 1102;
    failed.encounterId = encounterId;
    failed.targetGuid = 23;
    failed.spellId = 2061;
    failed.occurredAtMs = 1200;
    failed.status = CoreSpellTerminalStatus::Failed;
    engine.NotifyCoreSpellTerminal(failed);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 0);
    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1201);
    engine.ExecutePendingActions(nextTick);
    assert(child->executeCount == 0);

    ActionDependencyRequest duplicate = MakeDependencyRequest(
        1000, MakePrerequisite(1201, encounterId, 22, 2050,
                               CoreSpellTerminalStatus::Succeeded));
    duplicate.prerequisites.push_back(duplicate.prerequisites[0]);
    assert(engine.QueueActionDependency(f.context, duplicate, candidate) ==
           ActionContinuationEnqueueResult::Invalid);

    ActionDependencyRequest wrongEncounter = MakeDependencyRequest(
        1000, MakePrerequisite(1202, encounterId + 1, 22, 2050,
                               CoreSpellTerminalStatus::Succeeded));
    assert(engine.QueueActionDependency(f.context, wrongEncounter, candidate) ==
           ActionContinuationEnqueueResult::ParentEncounterInactive);
}

void TestActionAlternativeGroupsWaitForAllBranchesAndChoosePriority()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;
    std::vector<std::string> order;
    auto lower = std::make_shared<TestAction>("alternative_lower");
    auto higher = std::make_shared<TestAction>("alternative_higher");
    lower->pendingLifetimeMs = higher->pendingLifetimeMs = 2000;
    lower->executionOrder = higher->executionOrder = &order;
    assert(engine.RegisterAction(lower) && engine.RegisterAction(higher));

    std::uint64_t const encounterId = f.coordinator.GetEncounterId();
    ActionTerminalPrerequisite const parent = MakePrerequisite(
        1301, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded);
    ActionDependencyRequest lowerRequest = MakeDependencyRequest(1500, parent);
    lowerRequest.alternativeGroupId = 700;
    lowerRequest.alternativeIndex = 0;
    lowerRequest.alternativeCount = 2;
    ActionDependencyRequest higherRequest = lowerRequest;
    higherRequest.alternativeIndex = 1;

    ActionCandidate lowerCandidate = Candidate(
        "alternative_lower", 10.0f, ActionChannel::Utility);
    lowerCandidate.finalPriority = 10.0f;
    ActionCandidate higherCandidate = Candidate(
        "alternative_higher", 30.0f, ActionChannel::Utility);
    higherCandidate.finalPriority = 30.0f;

    assert(engine.QueueActionDependency(f.context, lowerRequest, lowerCandidate) ==
           ActionContinuationEnqueueResult::Queued);
    CoreSpellTerminalEvent terminal;
    terminal.actionId = parent.actionId;
    terminal.encounterId = encounterId;
    terminal.targetGuid = parent.targetGuid;
    terminal.spellId = parent.spellId;
    terminal.occurredAtMs = 1100;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(terminal);
    // One registered branch is not enough to settle an alternative group.
    assert(engine.GetPendingContinuationCount() == 1);
    assert(engine.GetPendingActionCount() == 0);

    // The second branch is registered after the Core terminal. Its prereq is
    // seeded from the recent-terminal ledger and the group resolves atomically.
    assert(engine.QueueActionDependency(f.context, higherRequest, higherCandidate) ==
           ActionContinuationEnqueueResult::ReleasedImmediately);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 1);
    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1200);
    PendingActionDispatchSummary const dispatch = engine.ExecutePendingActions(nextTick);
    assert(dispatch.selected == 1 && dispatch.executed == 1);
    assert(order.size() == 1 && order[0] == "alternative_higher");
    assert(lower->executeCount == 0 && higher->executeCount == 1);

    // A resolved alternative group ID is single-use within its owner context.
    DecisionContext duplicateContext(f.snapshot, f.bot, f.coordinator, 1201);
    assert(engine.QueueActionDependency(
        duplicateContext, higherRequest, higherCandidate) ==
        ActionContinuationEnqueueResult::DuplicateParent);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == 0);
}

void TestActionAlternativeGroupsUseFifoForPriorityTiesAndCancelOnExpiry()
{
    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        std::vector<std::string> order;
        auto first = std::make_shared<TestAction>("alternative_first");
        auto second = std::make_shared<TestAction>("alternative_second");
        first->pendingLifetimeMs = second->pendingLifetimeMs = 2000;
        first->executionOrder = second->executionOrder = &order;
        assert(engine.RegisterAction(first) && engine.RegisterAction(second));

        std::uint64_t const encounterId = f.coordinator.GetEncounterId();
        ActionTerminalPrerequisite const parent = MakePrerequisite(
            1401, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded);
        ActionDependencyRequest firstRequest = MakeDependencyRequest(1500, parent);
        firstRequest.alternativeGroupId = 701;
        firstRequest.alternativeIndex = 0;
        firstRequest.alternativeCount = 2;
        ActionDependencyRequest secondRequest = firstRequest;
        secondRequest.alternativeIndex = 1;
        ActionCandidate firstCandidate = Candidate(
            "alternative_first", 20.0f, ActionChannel::Utility);
        ActionCandidate secondCandidate = Candidate(
            "alternative_second", 20.0f, ActionChannel::Utility);
        firstCandidate.finalPriority = secondCandidate.finalPriority = 20.0f;

        assert(engine.QueueActionDependency(f.context, firstRequest, firstCandidate) ==
               ActionContinuationEnqueueResult::Queued);
        assert(engine.QueueActionDependency(f.context, secondRequest, secondCandidate) ==
               ActionContinuationEnqueueResult::Queued);
        CoreSpellTerminalEvent terminal;
        terminal.actionId = parent.actionId;
        terminal.encounterId = encounterId;
        terminal.targetGuid = parent.targetGuid;
        terminal.spellId = parent.spellId;
        terminal.occurredAtMs = 1100;
        terminal.status = CoreSpellTerminalStatus::Succeeded;
        engine.NotifyCoreSpellTerminal(terminal);
        DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 1200);
        engine.ExecutePendingActions(nextTick);
        assert(order.size() == 1 && order[0] == "alternative_first");
        assert(first->executeCount == 1 && second->executeCount == 0);
    }

    {
        Fixture f;
        BeginContinuationTestEncounter(f);
        PartyBotEngine::PartyBotEngine engine;
        auto first = std::make_shared<TestAction>("expired_alternative_first");
        auto second = std::make_shared<TestAction>("expired_alternative_second");
        first->pendingLifetimeMs = second->pendingLifetimeMs = 2000;
        assert(engine.RegisterAction(first) && engine.RegisterAction(second));
        std::uint64_t const encounterId = f.coordinator.GetEncounterId();
        ActionTerminalPrerequisite const parent = MakePrerequisite(
            1501, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded);
        ActionDependencyRequest firstRequest = MakeDependencyRequest(100, parent);
        firstRequest.alternativeGroupId = 702;
        firstRequest.alternativeIndex = 0;
        firstRequest.alternativeCount = 2;
        ActionDependencyRequest secondRequest = MakeDependencyRequest(500, parent);
        secondRequest.alternativeGroupId = 702;
        secondRequest.alternativeIndex = 1;
        secondRequest.alternativeCount = 2;
        ActionCandidate firstCandidate = Candidate(
            "expired_alternative_first", 20.0f, ActionChannel::Utility);
        ActionCandidate secondCandidate = Candidate(
            "expired_alternative_second", 10.0f, ActionChannel::Utility);
        firstCandidate.finalPriority = 20.0f;
        secondCandidate.finalPriority = 10.0f;
        assert(engine.QueueActionDependency(f.context, firstRequest, firstCandidate) ==
               ActionContinuationEnqueueResult::Queued);
        assert(engine.QueueActionDependency(f.context, secondRequest, secondCandidate) ==
               ActionContinuationEnqueueResult::Queued);
        DecisionContext expiryTick(f.snapshot, f.bot, f.coordinator, 1100);
        PendingActionDispatchSummary const pruned = engine.ExecutePendingActions(expiryTick);
        assert(pruned.expiredContinuations == 2);
        assert(engine.GetPendingContinuationCount() == 0);
        assert(engine.GetPendingActionCount() == 0);

        // Expiry settles and tombstones the group as a whole; a delayed retry
        // cannot register only one new branch under the old group ID.
        assert(engine.QueueActionDependency(
            expiryTick, secondRequest, secondCandidate) ==
            ActionContinuationEnqueueResult::DuplicateParent);
    }
}

void TestPendingActionQueueRejectsInvalidAndFullEntries()
{
    PendingActionQueue queue;
    ActionCandidate valid = Candidate("valid", 1.0f, ActionChannel::Utility);
    valid.finalPriority = 1.0f;

    assert(queue.Enqueue(valid, 100, 0) == PendingActionEnqueueResult::Invalid);
    assert(queue.Enqueue(valid, 100, 0x80000000u) == PendingActionEnqueueResult::Invalid);

    ActionCandidate noName = valid;
    noName.actionName.clear();
    assert(queue.Enqueue(noName, 100, 100) == PendingActionEnqueueResult::Invalid);

    ActionCandidate nanPriority = valid;
    nanPriority.finalPriority = std::numeric_limits<float>::quiet_NaN();
    assert(queue.Enqueue(nanPriority, 100, 100) == PendingActionEnqueueResult::Invalid);

    ActionCandidate invalidDestination = valid;
    invalidDestination.hasDestination = true;
    invalidDestination.destinationX = std::numeric_limits<float>::infinity();
    assert(queue.Enqueue(invalidDestination, 100, 100) == PendingActionEnqueueResult::Invalid);

    for (std::size_t i = 0; i < PendingActionQueue::Capacity; ++i) {
        ActionCandidate candidate = Candidate(
            "pending_" + std::to_string(i), static_cast<float>(i), ActionChannel::Utility);
        candidate.finalPriority = static_cast<float>(i);
        assert(queue.Enqueue(candidate, 100, 1000) == PendingActionEnqueueResult::Queued);
    }
    assert(queue.Size() == PendingActionQueue::Capacity);

    ActionCandidate overflow = Candidate("overflow", 1000.0f, ActionChannel::Utility);
    overflow.finalPriority = 1000.0f;
    assert(queue.Enqueue(overflow, 100, 1000) == PendingActionEnqueueResult::Full);
    assert(queue.Size() == PendingActionQueue::Capacity);

    queue.Clear();
    assert(queue.Empty());
}

void TestReadyContinuationRetriesAfterPendingQueueBackpressure()
{
    Fixture f;
    BeginContinuationTestEncounter(f);
    PartyBotEngine::PartyBotEngine engine;

    // Occupy every PendingActionQueue slot with distinct, eligible actions.
    for (std::size_t i = 0; i < PendingActionQueue::Capacity; ++i) {
        std::string const name = "backpressure_filler_" + std::to_string(i);
        auto filler = std::make_shared<TestAction>(name);
        filler->pendingLifetimeMs = 5000;
        assert(engine.RegisterAction(filler));
        ActionCandidate candidate = Candidate(
            name, static_cast<float>(i + 1), ActionChannel::Utility);
        candidate.finalPriority = static_cast<float>(i + 1);
        assert(engine.QueuePendingAction(f.context, candidate) ==
               PendingActionEnqueueResult::Queued);
    }
    assert(engine.GetPendingActionCount() == PendingActionQueue::Capacity);

    auto child = std::make_shared<TestAction>("backpressure_child");
    child->pendingLifetimeMs = 5000;
    assert(engine.RegisterAction(child));

    std::uint64_t const encounterId = f.coordinator.GetEncounterId();
    ActionTerminalPrerequisite const parent = MakePrerequisite(
        2201, encounterId, 22, 2050, CoreSpellTerminalStatus::Succeeded);
    ActionDependencyRequest request = MakeDependencyRequest(2000, parent);
    ActionCandidate childCandidate = Candidate(
        "backpressure_child", 1000.0f, ActionChannel::Utility);
    childCandidate.finalPriority = 1000.0f;

    // The parent completed before registration. The ready continuation cannot
    // enter the full pending queue and must remain in the bounded ready backlog.
    CoreSpellTerminalEvent terminal;
    terminal.actionId = parent.actionId;
    terminal.encounterId = parent.encounterId;
    terminal.targetGuid = parent.targetGuid;
    terminal.spellId = parent.spellId;
    terminal.occurredAtMs = 900;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    engine.NotifyCoreSpellTerminal(terminal);

    DecisionContext registrationContext(f.snapshot, f.bot, f.coordinator, 1000);
    assert(engine.QueueActionDependency(
        registrationContext, request, childCandidate) ==
        ActionContinuationEnqueueResult::Queued);
    assert(engine.GetPendingActionCount() == PendingActionQueue::Capacity);
    assert(engine.GetPendingContinuationCount() == 1);
    assert(child->executeCount == 0);

    // One dispatch frees a slot; end-of-dispatch retry transfers the ready
    // continuation to PendingActionQueue without bypassing its normal checks.
    DecisionContext firstDispatchContext(f.snapshot, f.bot, f.coordinator, 1001);
    PendingActionDispatchSummary const firstDispatch =
        engine.ExecutePendingActions(firstDispatchContext);
    assert(firstDispatch.executed == 1);
    assert(engine.GetPendingContinuationCount() == 0);
    assert(engine.GetPendingActionCount() == PendingActionQueue::Capacity);
    assert(child->executeCount == 0);

    // The deferred child is now in the ordinary pending queue and wins on its
    // current priority; no second registration or duplicate parent event is needed.
    DecisionContext secondDispatchContext(f.snapshot, f.bot, f.coordinator, 1002);
    PendingActionDispatchSummary const secondDispatch =
        engine.ExecutePendingActions(secondDispatchContext);
    assert(secondDispatch.executed == 1);
    assert(child->executeCount == 1);
}

void TestEnginePendingQueueRevalidatesCurrentEligibilityAndResetClearsIt()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestAction>("pending_heal");
    action->pendingLifetimeMs = 1000;
    assert(engine.RegisterAction(action));
    ActionCandidate candidate = Candidate(
        "pending_heal", 20.0f, ActionChannel::GlobalCooldown);
    assert(engine.QueuePendingAction(f.context, candidate) ==
        PendingActionEnqueueResult::Queued);
    assert(engine.GetPendingActionCount() == 1);
    assert(action->possibleChecks == 1 && action->usefulChecks == 1);

    action->useful = false;
    DecisionContext laterContext(f.snapshot, f.bot, f.coordinator, 1100);
    PendingActionDispatchSummary const rejected =
        engine.ExecutePendingActions(laterContext);
    assert(rejected.dequeued == 1 && rejected.rejected == 1);
    assert(rejected.executed == 0 && action->executeCount == 0);
    assert(action->possibleChecks == 2 && action->usefulChecks == 2);
    assert(engine.GetPendingActionCount() == 0);

    action->useful = true;
    assert(engine.QueuePendingAction(laterContext, candidate) ==
        PendingActionEnqueueResult::Queued);
    engine.InvalidateDecisionState();
    assert(engine.GetPendingActionCount() == 1);
    engine.ResetDecisionState();
    assert(engine.GetPendingActionCount() == 0);
}

void TestEnginePendingQueueDropsForeignGroupMapAndInstanceContext()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestAction>("pending_move");
    action->pendingLifetimeMs = 1000;
    assert(engine.RegisterAction(action));
    ActionCandidate candidate = Candidate(
        "pending_move", 15.0f, ActionChannel::Movement);
    assert(engine.QueuePendingAction(f.context, candidate) ==
        PendingActionEnqueueResult::Queued);

    PartySnapshot foreignSnapshot = f.snapshot;
    PartyMemberSnapshot foreignBot = f.bot;
    foreignSnapshot.groupId = 6;
    foreignSnapshot.mapId = 34;
    foreignSnapshot.instanceId = 202;
    foreignBot.mapId = 34;
    foreignBot.instanceId = 202;
    DecisionContext foreignContext(foreignSnapshot, foreignBot, f.coordinator, 1100);
    PendingActionDispatchSummary const stale =
        engine.ExecutePendingActions(foreignContext);
    assert(stale.dequeued == 1 && stale.staleContext == 1);
    assert(stale.executed == 0 && action->executeCount == 0);
    assert(engine.GetPendingActionCount() == 0);
}

void TestEnginePendingQueuePriorityChannelDeferralPreservesOriginalExpiry()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    std::vector<std::string> order;
    auto low = std::make_shared<TestAction>("pending_low");
    auto high = std::make_shared<TestAction>("pending_high");
    low->pendingLifetimeMs = 500;
    high->pendingLifetimeMs = 500;
    low->executionOrder = &order;
    high->executionOrder = &order;
    assert(engine.RegisterAction(low) && engine.RegisterAction(high));

    ActionCandidate lowCandidate = Candidate(
        "pending_low", 10.0f, ActionChannel::GlobalCooldown);
    ActionCandidate highCandidate = Candidate(
        "pending_high", 30.0f, ActionChannel::GlobalCooldown);
    assert(engine.QueuePendingAction(f.context, lowCandidate) ==
        PendingActionEnqueueResult::Queued);
    assert(engine.QueuePendingAction(f.context, highCandidate) ==
        PendingActionEnqueueResult::Queued);

    DecisionContext firstTick(f.snapshot, f.bot, f.coordinator, 1100);
    PendingActionDispatchSummary const first =
        engine.ExecutePendingActions(firstTick);
    assert(first.dequeued == 2 && first.selected == 1 && first.executed == 1);
    assert(first.requeued == 1 && engine.GetPendingActionCount() == 1);
    assert(order.size() == 1 && order[0] == "pending_high");

    DecisionContext expiredTick(f.snapshot, f.bot, f.coordinator, 1500);
    PendingActionDispatchSummary const expired =
        engine.ExecutePendingActions(expiredTick);
    assert(expired.expired == 1 && expired.dequeued == 0);
    assert(engine.GetPendingActionCount() == 0);
    assert(order.size() == 1);
}

void TestEnginePendingQueueFairnessSurvivesRevalidatedWorkSort()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    std::vector<std::string> order;
    auto oldest = std::make_shared<TestAction>("pending_oldest");
    auto freshHigh = std::make_shared<TestAction>("pending_fresh_high");
    oldest->pendingLifetimeMs = 10000;
    freshHigh->pendingLifetimeMs = 10000;
    oldest->executionOrder = &order;
    freshHigh->executionOrder = &order;
    assert(engine.RegisterAction(oldest) && engine.RegisterAction(freshHigh));

    ActionCandidate oldestCandidate = Candidate(
        "pending_oldest", 1.0f, ActionChannel::GlobalCooldown);
    oldestCandidate.finalPriority = 1.0f;
    ActionCandidate freshHighCandidate = Candidate(
        "pending_fresh_high", 1000.0f, ActionChannel::GlobalCooldown);
    freshHighCandidate.finalPriority = 1000.0f;

    DecisionContext oldestEnqueue(f.snapshot, f.bot, f.coordinator, 1000);
    DecisionContext highEnqueue(f.snapshot, f.bot, f.coordinator, 3999);
    assert(engine.QueuePendingAction(oldestEnqueue, oldestCandidate) ==
           PendingActionEnqueueResult::Queued);
    assert(engine.QueuePendingAction(highEnqueue, freshHighCandidate) ==
           PendingActionEnqueueResult::Queued);

    DecisionContext fairnessTick(f.snapshot, f.bot, f.coordinator, 4000);
    PendingActionDispatchSummary const first =
        engine.ExecutePendingActions(fairnessTick);
    // The executor revalidates and sorts its local work vector after PopNext.
    // It must use the same comparator or the queue's age promotion is lost.
    assert(first.selected == 1 && first.executed == 1 && first.requeued == 1);
    assert(order.size() == 1 && order[0] == "pending_oldest");
    assert(engine.GetPendingActionCount() == 1);

    DecisionContext nextTick(f.snapshot, f.bot, f.coordinator, 4100);
    PendingActionDispatchSummary const second =
        engine.ExecutePendingActions(nextTick);
    assert(second.selected == 1 && second.executed == 1);
    assert(order.size() == 2 && order[1] == "pending_fresh_high");
    assert(engine.GetPendingActionCount() == 0);
}

void TestDecisionCycleQueuesOptInActionsAndDispatchesPendingWork()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestAction>("deferred_cast");
    action->pendingLifetimeMs = 1000;
    assert(engine.RegisterAction(action));

    auto trigger = std::make_shared<TestTrigger>("deferred_trigger");
    trigger->generated.push_back(Candidate(
        "deferred_cast", 50.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("deferred_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    PendingActionDispatchSummary cycle = engine.ExecuteDecisionCycle(f.context, report);
    assert(cycle.newlyQueued == 1);
    assert(cycle.executed == 1);
    assert(action->executeCount == 1);
    assert(engine.GetPendingActionCount() == 0);
    assert(report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed);
    assert(SummarizeDecisionReport(report).executedCount == 1);
}

void TestDecisionCycleBlocksPendingChannelsAndHonorsOriginalExpiry()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto pending = std::make_shared<TestAction>("old_pending");
    auto immediate = std::make_shared<TestAction>("current_immediate");
    pending->pendingLifetimeMs = 500;
    assert(engine.RegisterAction(pending) && engine.RegisterAction(immediate));

    ActionCandidate oldCandidate = Candidate(
        "old_pending", 10.0f, ActionChannel::GlobalCooldown);
    assert(engine.QueuePendingAction(f.context, oldCandidate) ==
        PendingActionEnqueueResult::Queued);

    auto trigger = std::make_shared<TestTrigger>("immediate_trigger");
    trigger->generated.push_back(Candidate(
        "current_immediate", 30.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("immediate_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    PendingActionDispatchSummary first = engine.ExecuteDecisionCycle(f.context, report);
    assert(immediate->executeCount == 1);
    assert(pending->executeCount == 0);
    assert(first.requeued == 1);
    assert(engine.GetPendingActionCount() == 1);

    DecisionContext expiredContext(f.snapshot, f.bot, f.coordinator, 1500);
    DecisionReport emptyReport;
    emptyReport.snapshotEpoch = f.snapshot.epoch;
    PendingActionDispatchSummary expired =
        engine.ExecuteDecisionCycle(expiredContext, emptyReport);
    assert(expired.expired == 1);
    assert(pending->executeCount == 0);
    assert(engine.GetPendingActionCount() == 0);
}

void TestExecutionAuthorityModesAreExplicitAndFailSafe()
{
    assert(ResolveExecutionAuthorityMode(0) == ExecutionAuthorityMode::Legacy);
    assert(ResolveExecutionAuthorityMode(1) == ExecutionAuthorityMode::Shadow);
    assert(ResolveExecutionAuthorityMode(2) == ExecutionAuthorityMode::NewEngine);
    assert(ResolveExecutionAuthorityMode(-1) == ExecutionAuthorityMode::Legacy);
    assert(ResolveExecutionAuthorityMode(3) == ExecutionAuthorityMode::Legacy);

    assert(!ShouldEvaluateEngine(ExecutionAuthorityMode::Legacy, false));
    assert(ShouldEvaluateEngine(ExecutionAuthorityMode::Legacy, true));
    assert(ShouldEvaluateEngine(ExecutionAuthorityMode::Shadow, false));
    assert(ShouldEvaluateEngine(ExecutionAuthorityMode::NewEngine, false));

    assert(ShouldRunLegacyPolicy(ExecutionAuthorityMode::Legacy));
    assert(ShouldRunLegacyPolicy(ExecutionAuthorityMode::Shadow));
    assert(!ShouldRunLegacyPolicy(ExecutionAuthorityMode::NewEngine));
}

void TestValueCacheEpochExpiryInvalidationAndTypeSafety()
{
    ValueCache cache;
    std::uint32_t computeCount = 0;
    auto compute = [&computeCount]() { return ++computeCount; };
    assert(cache.GetOrCompute<int>("party.health", 7, 1000, 200, compute) == 1);
    assert(cache.GetOrCompute<int>("party.health", 7, 1100, 200, compute) == 1);
    assert(computeCount == 1 && cache.Size() == 1);
    assert(cache.GetOrCompute<int>("party.health", 8, 1150, 200, compute) == 2);
    assert(computeCount == 2);
    assert(cache.Invalidate("party.health"));
    assert(cache.GetOrCompute<int>("party.health", 8, 1160, 200, compute) == 3);
    assert(!cache.Invalidate("missing"));

    std::uint32_t stringComputes = 0;
    std::string const typed = cache.GetOrCompute<std::string>(
        "party.health", 8, 1170, 200,
        [&stringComputes]() { ++stringComputes; return std::string("typed"); });
    assert(typed == "typed" && stringComputes == 1);
    assert(cache.PruneExpired(1370) == 1);
    assert(cache.Size() == 0);
    cache.GetOrCompute<int>("wrapped", 1, 0xfffffff0u, 32, []() { return 42; });
    assert(cache.PruneExpired(0x0fu) == 0);
    assert(cache.PruneExpired(0x10u) == 1);
    assert(cache.GetOrCompute<int>("uncached", 1, 10, 0, compute) == 4);
    assert(cache.GetOrCompute<int>("uncached", 1, 10, 0x80000000u, compute) == 5);
}

void TestValueCacheCapacityFailsOpenWithoutEvictingLiveEntries()
{
    ValueCache cache;
    for (std::size_t i = 0; i < ValueCache::Capacity; ++i) {
        std::string const key = "value_" + std::to_string(i);
        assert(cache.GetOrCompute<std::uint32_t>(
            key, 1, 100, 1000, [i]() { return static_cast<std::uint32_t>(i); }) == i);
    }
    assert(cache.Size() == ValueCache::Capacity);
    std::uint32_t overflowComputes = 0;
    auto overflow = [&overflowComputes]() { return ++overflowComputes; };
    assert(cache.GetOrCompute<std::uint32_t>("overflow", 1, 100, 1000, overflow) == 1);
    assert(cache.GetOrCompute<std::uint32_t>("overflow", 1, 101, 1000, overflow) == 2);
    assert(overflowComputes == 2 && cache.Size() == ValueCache::Capacity);
    assert(cache.GetOrCompute<std::uint32_t>(
        "value_0", 1, 101, 1000, []() { return 999u; }) == 0);
}

void TestCachedValueUsesContextEpochAndExplicitInvalidation()
{
    Fixture f;
    ValueCache cache;
    DecisionContext context(f.snapshot, f.bot, f.coordinator, 1000, nullptr, &cache);
    std::uint32_t computeCount = 0;
    CachedValue<std::uint32_t> value(
        "party.member_count", 500,
        [&computeCount](DecisionContext const& decision) {
            ++computeCount;
            return static_cast<std::uint32_t>(decision.snapshot.members.size());
        });
    f.snapshot.members.resize(2);
    assert(value.Get(context) == 2);
    f.snapshot.members.resize(3);
    assert(value.Get(context) == 2);
    assert(computeCount == 1);
    f.snapshot.epoch++;
    assert(value.Get(context) == 3 && computeCount == 2);
    assert(cache.Invalidate(value.GetName()));
    f.snapshot.members.resize(4);
    assert(value.Get(context) == 4 && computeCount == 3);
}

void TestTriggerSchedulerCadenceInvalidationAndClockWrap()
{
    TriggerScheduler scheduler;
    assert(scheduler.ShouldEvaluate("healer/emergency", 500, 1000));
    assert(!scheduler.ShouldEvaluate("healer/emergency", 500, 1499));
    assert(scheduler.ShouldEvaluate("healer/emergency", 500, 1500));
    assert(scheduler.Invalidate("healer/emergency"));
    assert(scheduler.ShouldEvaluate("healer/emergency", 500, 1501));
    assert(!scheduler.ShouldEvaluate("healer/emergency", 500, 1502));
    assert(scheduler.ShouldEvaluate("always", 0, 10));
    assert(scheduler.ShouldEvaluate("always", 0, 11));

    TriggerScheduler wrapped;
    std::uint32_t const start = 0xfffffff0u;
    assert(wrapped.ShouldEvaluate("wrapped", 32, start));
    assert(!wrapped.ShouldEvaluate("wrapped", 32, 0x0fu));
    assert(wrapped.ShouldEvaluate("wrapped", 32, 0x10u));
}

void TestTriggerSchedulerCapacityFailsOpen()
{
    TriggerScheduler scheduler;
    for (std::size_t i = 0; i < TriggerScheduler::Capacity; ++i) {
        std::string const key = "trigger_" + std::to_string(i);
        assert(scheduler.ShouldEvaluate(key, 1000, 100));
        assert(!scheduler.ShouldEvaluate(key, 1000, 101));
    }
    assert(scheduler.Size() == TriggerScheduler::Capacity);
    assert(scheduler.ShouldEvaluate("overflow", 1000, 100));
    assert(scheduler.ShouldEvaluate("overflow", 1000, 101));
    assert(scheduler.Size() == TriggerScheduler::Capacity);
}

void TestEngineTriggerCadenceAndExplicitInvalidation()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestAction>("cadenced");
    assert(engine.RegisterAction(action));
    auto trigger = std::make_shared<TestTrigger>("periodic");
    trigger->intervalMs = 500;
    trigger->generated.push_back(Candidate("cadenced", 10.0f, ActionChannel::Utility));
    auto strategy = std::make_shared<Strategy>("test_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport first = engine.Evaluate(f.context);
    assert(!first.candidates.empty() && trigger->activeChecks == 1);
    assert(first.triggerEvaluations.size() == 1);
    assert(first.triggerEvaluations[0].strategyName == "test_strategy");
    assert(first.triggerEvaluations[0].triggerName == "periodic");
    assert(first.triggerEvaluations[0].intervalMs == 500);
    assert(first.triggerEvaluations[0].candidateCount == 1);
    assert(first.triggerEvaluations[0].status == TriggerEvaluationStatus::Active);
    assert(first.candidates[0].candidate.triggerName == "periodic");

    DecisionContext earlyContext(f.snapshot, f.bot, f.coordinator, 1499);
    DecisionReport early = engine.Evaluate(earlyContext);
    assert(early.candidates.empty() && trigger->activeChecks == 1);
    assert(early.triggerEvaluations.size() == 1);
    assert(early.triggerEvaluations[0].status == TriggerEvaluationStatus::CadenceSkipped);

    assert(engine.InvalidateTrigger("test_strategy", "periodic"));
    DecisionContext invalidatedContext(f.snapshot, f.bot, f.coordinator, 1500);
    DecisionReport invalidated = engine.Evaluate(invalidatedContext);
    assert(!invalidated.candidates.empty() && trigger->activeChecks == 2);
    assert(invalidated.triggerEvaluations[0].status == TriggerEvaluationStatus::Active);

    engine.InvalidateDecisionState();
    DecisionContext allInvalidatedContext(f.snapshot, f.bot, f.coordinator, 1501);
    DecisionReport allInvalidated = engine.Evaluate(allInvalidatedContext);
    assert(!allInvalidated.candidates.empty() && trigger->activeChecks == 3);
    assert(allInvalidated.triggerEvaluations[0].status == TriggerEvaluationStatus::Active);

    engine.ResetDecisionState();
    DecisionContext resetContext(f.snapshot, f.bot, f.coordinator, 1502);
    DecisionReport reset = engine.Evaluate(resetContext);
    assert(!reset.candidates.empty() && trigger->activeChecks == 4);
    assert(reset.triggerEvaluations[0].status == TriggerEvaluationStatus::Active);

    trigger->active = false;
    DecisionContext inactiveContext(f.snapshot, f.bot, f.coordinator, 2002);
    DecisionReport inactive = engine.Evaluate(inactiveContext);
    assert(inactive.candidates.empty() && trigger->activeChecks == 5);
    assert(inactive.triggerEvaluations.size() == 1);
    assert(inactive.triggerEvaluations[0].status == TriggerEvaluationStatus::Inactive);
    assert(inactive.triggerEvaluations[0].candidateCount == 0);
}

void TestTriggerTraceSelectionPrioritizesActionableRows()
{
    DecisionReport report;
    auto add = [&report](TriggerEvaluationStatus status, std::uint32_t candidateCount,
                         char const* name) {
        TriggerEvaluation evaluation;
        evaluation.status = status;
        evaluation.candidateCount = candidateCount;
        evaluation.triggerName = name;
        report.triggerEvaluations.push_back(evaluation);
    };

    // Registration order intentionally puts inactive rows before an active row.
    add(TriggerEvaluationStatus::Inactive, 0, "inactive_first");
    add(TriggerEvaluationStatus::Active, 1, "active_one");
    add(TriggerEvaluationStatus::CadenceSkipped, 0, "cadence_first");
    add(TriggerEvaluationStatus::Active, 0, "active_empty");
    add(TriggerEvaluationStatus::Active, 4, "active_four");
    add(TriggerEvaluationStatus::Inactive, 0, "inactive_second");
    add(TriggerEvaluationStatus::CadenceSkipped, 0, "cadence_second");

    std::vector<std::size_t> const selected =
        SelectPriorityTriggerEvaluationIndices(report, 4);
    assert(selected.size() == 4);
    assert(selected[0] == 4); // Most candidate-producing active trigger.
    assert(selected[1] == 1); // Next candidate-producing active trigger.
    assert(selected[2] == 3); // Active but did not produce candidates.
    assert(selected[3] == 0); // Inactive precedes cadence-skipped.
    assert(SelectPriorityTriggerEvaluationIndices(report, 0).empty());
    assert(SelectPriorityTriggerEvaluationIndices(report, 20).size() == 7);
}

void TestEngineTriggerObservationCapacityIsBounded()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    for (std::uint32_t i = 0; i < DecisionReport::MaxTriggerEvaluations + 6; ++i)
    {
        auto trigger = std::make_shared<TestTrigger>("trigger_" + std::to_string(i));
        auto strategy = std::make_shared<Strategy>("strategy_" + std::to_string(i));
        strategy->AddTrigger(trigger);
        engine.AddStrategy(strategy);
    }

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.triggerEvaluations.size() == DecisionReport::MaxTriggerEvaluations);
    assert(report.omittedTriggerEvaluations == 6);
}

void TestEngineActionAttemptAndResultTrace()
{
    Fixture f;
    f.snapshot.groupId = 91;
    f.snapshot.mapId = 33;
    f.snapshot.instanceId = 101;
    PartyMemberSnapshot inCombatBot = f.bot;
    inCombatBot.inCombat = true;
    f.snapshot.members.push_back(inCombatBot);
    EncounterTransition const started =
        f.coordinator.ObserveEncounter(f.snapshot, 1000, 30000);
    assert(started.kind == EncounterTransitionKind::Started);

    CaptureReservationTraceSink traceSink;
    f.context.reservationTrace = &traceSink;

    PartyBotEngine::PartyBotEngine engine;
    auto success = std::make_shared<TestAction>("engine_heal");
    auto failure = std::make_shared<TestAction>("engine_move");
    failure->result = ActionResult::Failed;
    assert(engine.RegisterAction(success));
    assert(engine.RegisterAction(failure));

    ActionCandidate heal = Candidate("engine_heal", 20.0f, ActionChannel::Utility);
    heal.spellId = 2050;
    ActionCandidate move = Candidate("engine_move", 10.0f, ActionChannel::Movement);
    move.targetGuid = 44;
    auto trigger = std::make_shared<TestTrigger>("adapter_outcomes");
    trigger->generated.push_back(heal);
    trigger->generated.push_back(move);
    auto strategy = std::make_shared<Strategy>("engine_adapter");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 2);
    engine.ExecuteSelected(f.context, report);

    assert(success->executeCount == 1 && failure->executeCount == 1);
    assert(traceSink.actionEvents.size() == 4);
    EngineActionTraceEvent const& firstAttempt = traceSink.actionEvents[0];
    EngineActionTraceEvent const& firstResult = traceSink.actionEvents[1];
    EngineActionTraceEvent const& secondAttempt = traceSink.actionEvents[2];
    EngineActionTraceEvent const& secondResult = traceSink.actionEvents[3];

    assert(firstAttempt.stage == EngineActionTraceStage::Attempt);
    assert(firstAttempt.actionId != 0);
    assert(firstAttempt.actionId == firstResult.actionId);
    assert(firstAttempt.actionName == std::string("engine_heal"));
    assert(firstAttempt.targetGuid == 22 && firstAttempt.spellId == 2050);
    assert(firstAttempt.attempted && firstAttempt.result == ActionResult::NotAttempted);
    assert(firstResult.stage == EngineActionTraceStage::Result);
    assert(firstResult.attempted && firstResult.result == ActionResult::Success);
    assert(firstResult.reason == std::string("adapter_returned_success"));

    assert(secondAttempt.stage == EngineActionTraceStage::Attempt);
    assert(secondAttempt.actionId != 0 && secondAttempt.actionId != firstAttempt.actionId);
    assert(secondAttempt.actionId == secondResult.actionId);
    assert(secondAttempt.actionName == std::string("engine_move"));
    assert(secondAttempt.targetGuid == 44);
    assert(secondAttempt.attempted);
    assert(secondResult.stage == EngineActionTraceStage::Result);
    assert(secondResult.attempted && secondResult.result == ActionResult::Failed);
    assert(secondResult.reason == std::string("action_execution_not_successful"));

    for (std::size_t index : report.selectedIndices)
    {
        CandidateEvaluation const& evaluation = report.candidates[index];
        assert(evaluation.actionId != 0);
        assert(evaluation.actionId == evaluation.candidate.actionId);
    }
    assert(report.candidates[report.selectedIndices[0]].actionId !=
           report.candidates[report.selectedIndices[1]].actionId);
    assert(report.candidates[report.selectedIndices[0]].threatActionId == 0);
}

void TestThreatReservationTerminalListenerReleasesOnlyMatchingOwnerAndEncounter()
{
    auto coordinator = std::make_shared<PartyCoordinator>();
    PartySnapshot snapshot;
    snapshot.groupId = 71;
    snapshot.mapId = 33;
    snapshot.instanceId = 101;
    snapshot.epoch = 1;
    PartyMemberSnapshot member;
    member.guid = 42;
    member.online = true;
    member.alive = true;
    member.inCombat = true;
    snapshot.members.push_back(member);
    EncounterTransition started = coordinator->ObserveEncounter(snapshot, 1000, 30000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(started.encounterId != 0);

    ThreatReservation reservation;
    reservation.ownerGuid = 42;
    reservation.threatOwnerGuid = 42;
    reservation.hostileGuid = 9001;
    reservation.actionId = coordinator->NextActionId();
    reservation.threatPoints = 17.0f;
    reservation.expiresAtMs = 5000;
    assert(coordinator->TryReserveThreatImpact(reservation, 1000));

    ThreatReservation otherOwner = reservation;
    otherOwner.ownerGuid = 43;
    otherOwner.threatOwnerGuid = 43;
    otherOwner.actionId = coordinator->NextActionId();
    otherOwner.threatPoints = 9.0f;
    assert(coordinator->TryReserveThreatImpact(otherOwner, 1000));

    ThreatReservationTerminalListener listener(42, coordinator);
    CoreSpellTerminalEvent terminal;
    terminal.actionId = reservation.actionId;
    terminal.encounterId = started.encounterId + 1;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetThreatReservationCount() == 2);

    terminal.encounterId = started.encounterId;
    terminal.actionId = otherOwner.actionId;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetThreatReservationCount() == 2);

    terminal.actionId = reservation.actionId;
    const CoreSpellTerminalStatus terminalStatuses[] = {
        CoreSpellTerminalStatus::Succeeded,
        CoreSpellTerminalStatus::Failed,
        CoreSpellTerminalStatus::Interrupted
    };
    for (CoreSpellTerminalStatus status : terminalStatuses)
    {
        // Reinsert the bot's row to verify every Core terminal outcome releases it.
        if (!coordinator->TryReserveThreatImpact(reservation, 1000))
            assert(coordinator->GetThreatReservationCount() == 2);
        terminal.status = status;
        listener.OnCoreSpellTerminal(terminal);
        assert(coordinator->GetReservedThreatImpact(
            9001, 42, 0, 1000) == 0.0f);
        assert(coordinator->GetReservedThreatImpact(
            9001, 42, 0, 1000) == 0.0f);
        assert(coordinator->GetThreatReservationCount() == 1);
    }

    // A terminal delivered after its coordinator has been destroyed is inert.
    terminal.actionId = reservation.actionId;
    coordinator.reset();
    listener.OnCoreSpellTerminal(terminal);
}

void TestThreatReservationTerminalListenerReleasesNoEncounterTerminalWithoutCrossShardCollision()
{
    auto sourceCoordinator = std::make_shared<PartyCoordinator>();
    auto destinationCoordinator = std::make_shared<PartyCoordinator>();
    Guid const ownerGuid = 4200;

    ActionId const sourceActionId = sourceCoordinator->NextActionId();
    ActionId const destinationActionId = destinationCoordinator->NextActionId();
    assert(sourceActionId != 0 && destinationActionId != 0);
    assert(sourceActionId != destinationActionId);
    assert(sourceCoordinator->GetEncounterId() == 0);
    assert(destinationCoordinator->GetEncounterId() == 0);

    ThreatReservation sourceReservation;
    sourceReservation.ownerGuid = ownerGuid;
    sourceReservation.threatOwnerGuid = ownerGuid;
    sourceReservation.hostileGuid = 9101;
    sourceReservation.actionId = sourceActionId;
    sourceReservation.threatPoints = 17.0f;
    sourceReservation.expiresAtMs = 5000;
    assert(sourceCoordinator->TryReserveThreatImpact(sourceReservation, 1000));

    ThreatReservation successorReservation = sourceReservation;
    successorReservation.hostileGuid = 9102;
    successorReservation.actionId = destinationActionId;
    successorReservation.threatPoints = 19.0f;
    assert(destinationCoordinator->TryReserveThreatImpact(successorReservation, 1000));

    ThreatReservationTerminalListener sourceListener(ownerGuid, sourceCoordinator);
    ThreatReservationTerminalListener destinationListener(ownerGuid, destinationCoordinator);
    CoreSpellTerminalEvent terminal;
    terminal.actionId = sourceActionId;
    terminal.encounterId = 0;
    terminal.status = CoreSpellTerminalStatus::Interrupted;

    // A terminal for an out-of-encounter action still releases its exact source
    // row. Rebinding the old event to a new coordinator must not release the
    // successor row owned by the same Bot.
    sourceListener.OnCoreSpellTerminal(terminal);
    assert(sourceCoordinator->GetThreatReservationCount() == 0);
    destinationListener.OnCoreSpellTerminal(terminal);
    assert(destinationCoordinator->GetThreatReservationCount() == 1);
    assert(destinationCoordinator->GetReservedThreatImpact(
        9102, ownerGuid, 0, 1000) == 19.0f);
}

void TestHealReservationsAggregateAcrossBotsAndReleaseOnlyExactCast()
{
    auto coordinator = std::make_shared<PartyCoordinator>();
    Guid const targetGuid = 700;
    Guid const healerGuid = 701;
    Guid const otherHealerGuid = 702;

    Reservation first;
    first.type = ReservationType::Heal;
    first.targetGuid = targetGuid;
    first.ownerGuid = healerGuid;
    first.expiresAtMs = 5000;
    first.amount = 450;
    first.spellId = 2050;
    first.actionId = coordinator->NextActionId();
    assert(first.actionId && coordinator->TryReserve(first, 1000));

    Reservation other = first;
    other.ownerGuid = otherHealerGuid;
    other.amount = 300;
    other.actionId = coordinator->NextActionId();
    assert(coordinator->TryReserve(other, 1000));
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1100) == 750);

    Reservation successor = first;
    successor.amount = 125;
    successor.spellId = 2052;
    successor.actionId = coordinator->NextActionId();
    assert(successor.actionId != first.actionId);
    assert(coordinator->TryReserve(successor, 1200));
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1200) == 425);

    HealReservationTerminalListener listener(healerGuid, coordinator);
    CoreSpellTerminalEvent terminal;
    terminal.actionId = first.actionId;
    terminal.encounterId = 0;
    terminal.targetGuid = targetGuid;
    terminal.spellId = first.spellId;
    terminal.status = CoreSpellTerminalStatus::Interrupted;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1200) == 425);

    terminal.actionId = successor.actionId;
    terminal.spellId = first.spellId;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1200) == 425);

    terminal.spellId = successor.spellId;
    terminal.targetGuid = targetGuid + 1;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1200) == 425);

    terminal.targetGuid = targetGuid;
    terminal.encounterId = 1;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1200) == 425);

    terminal.encounterId = 0;
    listener.OnCoreSpellTerminal(terminal);
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid, 0, 1200) == 300);

    Reservation huge = other;
    huge.targetGuid = targetGuid + 2;
    huge.amount = std::numeric_limits<std::uint32_t>::max();
    huge.actionId = coordinator->NextActionId();
    assert(coordinator->TryReserve(huge, 1300));
    Reservation overflow = huge;
    overflow.ownerGuid = otherHealerGuid + 1;
    overflow.amount = 1;
    overflow.actionId = coordinator->NextActionId();
    assert(coordinator->TryReserve(overflow, 1300));
    assert(coordinator->GetReservedAmount(
        ReservationType::Heal, targetGuid + 2, 0, 1300) ==
        std::numeric_limits<std::uint32_t>::max());
}

void TestPriestHealReservationStartRollbackAndTerminalRelease()
{
    Fixture successful;
    successful.bot.classId = PriestDirectHealAction::PriestClassId;
    successful.bot.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    successful.bot.isPlayerBot = true;
    successful.bot.power = 500;
    successful.bot.directHealSpells = {{2050, 120, 25}};
    PartyMemberSnapshot target;
    target.guid = 22;
    target.roleId = static_cast<std::uint8_t>(PartyRole::Tank);
    target.health = 30;
    target.maxHealth = 100;
    target.online = true;
    target.alive = true;
    target.isMainTank = true;
    target.mapId = successful.snapshot.mapId;
    target.instanceId = successful.snapshot.instanceId;
    successful.snapshot.members.push_back(target);

    MockDirectHealRuntime successRuntime;
    successRuntime.otherOwnerGuid = 33;
    Reservation other;
    other.type = ReservationType::Heal;
    other.targetGuid = 22;
    other.ownerGuid = 33;
    other.expiresAtMs = 10000;
    other.amount = 20;
    other.spellId = 2050;
    other.actionId = successful.coordinator.NextActionId();
    assert(successful.coordinator.TryReserve(other, 1000));

    DecisionContext successContext(
        successful.snapshot, successful.bot, successful.coordinator, 1000,
        nullptr, nullptr, nullptr, nullptr, &successRuntime);
    ActionCandidate candidate = Candidate(
        "core_priest_direct_heal", 80.0f, ActionChannel::GlobalCooldown);
    candidate.targetGuid = 22;
    candidate.spellId = 2050;
    candidate.actionId = successful.coordinator.NextActionId();

    PriestDirectHealAction action;
    std::string reason;
    assert(action.IsPossible(successContext, candidate, reason));
    assert(action.IsUseful(successContext, candidate, reason));
    ActionExecutionResult started = action.ExecuteDetailed(successContext, candidate);
    assert(started.result == ActionResult::Success);
    assert(started.reason == "priest_direct_heal_core_cast_started");
    assert(successRuntime.observedOwnReservation == 50);
    assert(successful.coordinator.GetReservedAmount(
        ReservationType::Heal, 22, 0, 1000) == 70);

    std::shared_ptr<PartyCoordinator> coordinator(
        &successful.coordinator, [](PartyCoordinator*) {});
    HealReservationTerminalListener listener(11, coordinator);
    CoreSpellTerminalEvent terminal;
    terminal.actionId = candidate.actionId;
    terminal.encounterId = 0;
    terminal.targetGuid = candidate.targetGuid;
    terminal.spellId = candidate.spellId;
    terminal.status = CoreSpellTerminalStatus::Succeeded;
    listener.OnCoreSpellTerminal(terminal);
    assert(successful.coordinator.GetReservedAmount(
        ReservationType::Heal, 22, 0, 1000) == 20);

    Fixture failed;
    failed.bot.classId = PriestDirectHealAction::PriestClassId;
    failed.bot.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    failed.bot.isPlayerBot = true;
    failed.bot.power = 500;
    failed.bot.directHealSpells = {{2050, 120, 25}};
    target.guid = 22;
    target.health = 30;
    target.maxHealth = 100;
    failed.snapshot.members.push_back(target);
    MockDirectHealRuntime failRuntime;
    failRuntime.otherOwnerGuid = 33;
    failRuntime.startSucceeds = false;
    other.actionId = failed.coordinator.NextActionId();
    assert(failed.coordinator.TryReserve(other, 1000));
    DecisionContext failedContext(
        failed.snapshot, failed.bot, failed.coordinator, 1000,
        nullptr, nullptr, nullptr, nullptr, &failRuntime);
    candidate.actionId = failed.coordinator.NextActionId();
    ActionExecutionResult rejected = action.ExecuteDetailed(failedContext, candidate);
    assert(rejected.result == ActionResult::Impossible);
    assert(rejected.reason == "priest_direct_heal_core_cast_rejected");
    assert(failRuntime.observedOwnReservation == 50);
    assert(failed.coordinator.GetReservedAmount(
        ReservationType::Heal, 22, 0, 1000) == 20);
}

void TestPriestDirectHealReservationSubtractsIncomingForecasts()
{
    Fixture f;
    f.bot.classId = PriestDirectHealAction::PriestClassId;
    f.bot.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    f.bot.isPlayerBot = true;
    f.bot.power = 500;
    f.bot.directHealSpells = {{2050, 120, 25}};

    PartyMemberSnapshot target;
    target.guid = 22;
    target.roleId = static_cast<std::uint8_t>(PartyRole::Tank);
    target.health = 30;
    target.maxHealth = 100;
    target.online = true;
    target.alive = true;
    target.isMainTank = true;
    target.mapId = f.snapshot.mapId;
    target.instanceId = f.snapshot.instanceId;
    target.periodicHealRemainingAmount = 10;
    target.allSchoolAbsorbRemainingAmount = 5;
    f.snapshot.members.push_back(target);

    PartyMemberSnapshot humanCaster;
    humanCaster.guid = 44;
    humanCaster.roleId = static_cast<std::uint8_t>(PartyRole::Unknown);
    humanCaster.health = 100;
    humanCaster.maxHealth = 100;
    humanCaster.online = true;
    humanCaster.alive = true;
    humanCaster.mapId = f.snapshot.mapId;
    humanCaster.instanceId = f.snapshot.instanceId;
    humanCaster.currentSpellTargetGuid = target.guid;
    humanCaster.currentSpellHealingAmount = 10;
    f.snapshot.members.push_back(humanCaster);

    Reservation other;
    other.type = ReservationType::Heal;
    other.targetGuid = target.guid;
    other.ownerGuid = 33;
    other.expiresAtMs = 10000;
    other.amount = 20;
    other.spellId = 2050;
    other.actionId = f.coordinator.NextActionId();
    assert(f.coordinator.TryReserve(other, 1000));

    MockDirectHealRuntime runtime;
    runtime.otherOwnerGuid = other.ownerGuid;
    DecisionContext context(
        f.snapshot, f.bot, f.coordinator, 1000,
        nullptr, nullptr, nullptr, nullptr, &runtime);
    ActionCandidate candidate = Candidate(
        "core_priest_direct_heal", 80.0f, ActionChannel::GlobalCooldown);
    candidate.targetGuid = target.guid;
    candidate.spellId = 2050;
    candidate.actionId = f.coordinator.NextActionId();

    PriestDirectHealAction action;
    std::string reason;
    assert(action.IsPossible(context, candidate, reason));
    assert(action.IsUseful(context, candidate, reason));
    ActionExecutionResult started = action.ExecuteDetailed(context, candidate);
    assert(started.result == ActionResult::Success);
    assert(runtime.observedOwnReservation == 25);
    assert(f.coordinator.GetReservedAmount(
        ReservationType::Heal, target.guid, 0, 1000) == 45);

    // Once the forecast and reservations cover the remaining deficit, the
    // action becomes useless and cannot create another Heal reservation.
    f.snapshot.members[0].periodicHealRemainingAmount = 40;
    f.snapshot.members[0].allSchoolAbsorbRemainingAmount = 15;
    f.snapshot.members[1].currentSpellHealingAmount = 10;
    // Keep the test's original second reservation and the Bot's own reservation:
    // the aggregate now covers 70 missing health exactly.
    assert(!action.IsUseful(context, candidate, reason));
}

void TestClosingEncounterClearsInFlightThreatReservations()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 72;
    snapshot.mapId = 33;
    snapshot.instanceId = 102;
    snapshot.epoch = 1;
    PartyMemberSnapshot member;
    member.guid = 52;
    member.online = true;
    member.alive = true;
    member.inCombat = true;
    snapshot.members.push_back(member);
    EncounterTransition started = coordinator.ObserveEncounter(snapshot, 1000, 30000);
    assert(started.kind == EncounterTransitionKind::Started);

    ThreatReservation reservation;
    reservation.ownerGuid = 52;
    reservation.threatOwnerGuid = 52;
    reservation.hostileGuid = 9002;
    reservation.actionId = coordinator.NextActionId();
    reservation.threatPoints = 12.0f;
    reservation.expiresAtMs = 10000;
    assert(coordinator.TryReserveThreatImpact(reservation, 1000));
    assert(coordinator.GetThreatReservationCount() == 1);

    EncounterTransition ended = coordinator.CloseEncounter(1500, "test_terminal");
    assert(ended.kind == EncounterTransitionKind::Ended);
    assert(coordinator.GetThreatReservationCount() == 0);
}

void TestCoreSpellTerminalListenersDeliverOwnedEventsAndAreBounded()
{
    PartyBotEngine::PartyBotEngine engine;
    auto first = std::make_shared<CaptureCoreSpellTerminalListener>();
    auto late = std::make_shared<CaptureCoreSpellTerminalListener>();
    auto registering = std::make_shared<RegisterCoreSpellTerminalListenerDuringCallback>(engine, late);
    assert(engine.AddCoreSpellTerminalListener(first));
    assert(!engine.AddCoreSpellTerminalListener(first));
    assert(!engine.AddCoreSpellTerminalListener(std::shared_ptr<CoreSpellTerminalListener>()));
    assert(engine.AddCoreSpellTerminalListener(registering));
    assert(engine.GetCoreSpellTerminalListenerCount() == 2);
    CoreSpellTerminalEvent event;
    event.actionId = 0x12345678u;
    event.encounterId = 0x87654321u;
    event.targetGuid = 0x1122334455667788ull;
    event.spellId = 2061;
    event.occurredAtMs = 4567;
    event.status = CoreSpellTerminalStatus::Succeeded;
    // No DecisionContext, trace sink or EngineActionObserver participates.
    engine.NotifyCoreSpellTerminal(event);
    assert(first->events.size() == 1 && registering->events.size() == 1);
    assert(late->events.empty() && registering->registered);
    assert(first->events[0].actionId == event.actionId);
    assert(first->events[0].encounterId == event.encounterId);
    assert(first->events[0].targetGuid == event.targetGuid);
    assert(first->events[0].spellId == event.spellId);
    assert(first->events[0].occurredAtMs == event.occurredAtMs);
    assert(first->events[0].status == CoreSpellTerminalStatus::Succeeded);
    event.actionId += 1;
    event.status = CoreSpellTerminalStatus::Interrupted;
    engine.NotifyCoreSpellTerminal(event);
    assert(first->events.size() == 2 && registering->events.size() == 2);
    assert(late->events.size() == 1);
    assert(late->events[0].actionId == event.actionId);
    assert(late->events[0].status == CoreSpellTerminalStatus::Interrupted);
    std::vector<std::shared_ptr<CaptureCoreSpellTerminalListener>> extras;
    while (engine.GetCoreSpellTerminalListenerCount() < 32) {
        auto extra = std::make_shared<CaptureCoreSpellTerminalListener>();
        assert(engine.AddCoreSpellTerminalListener(extra));
        extras.push_back(extra);
    }
    assert(engine.GetCoreSpellTerminalListenerCount() == 32);
    assert(!engine.AddCoreSpellTerminalListener(std::make_shared<CaptureCoreSpellTerminalListener>()));
}
void TestActionResultListenersReceiveDispatchOutcomesWithoutTraceSinks()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto success = std::make_shared<TestAction>("listener_success");
    auto failure = std::make_shared<TestAction>("listener_failure");
    failure->result = ActionResult::Failed;
    assert(engine.RegisterAction(success) && engine.RegisterAction(failure));

    auto listener = std::make_shared<CaptureActionResultListener>();
    assert(engine.AddActionResultListener(listener));
    assert(engine.GetActionResultListenerCount() == 1);
    assert(!engine.AddActionResultListener(listener));
    assert(!engine.AddActionResultListener(std::shared_ptr<ActionResultListener>()));

    auto trigger = std::make_shared<TestTrigger>("listener_outcomes");
    trigger->generated.push_back(Candidate(
        "listener_success", 20.0f, ActionChannel::Utility));
    trigger->generated.push_back(Candidate(
        "listener_failure", 10.0f, ActionChannel::Movement));
    auto strategy = std::make_shared<Strategy>("listener_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    // No ReservationTraceSink or EngineActionObserver is installed. Listener
    // delivery must not depend on diagnostics being enabled.
    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 2);
    engine.ExecuteSelected(f.context, report);

    assert(listener->events.size() == 2);
    ActionResultEvent const& first = listener->events[0];
    ActionResultEvent const& second = listener->events[1];
    assert(first.attempted && first.result == ActionResult::Success);
    assert(first.reason == "adapter_returned_success");
    assert(first.candidate.actionName == "listener_success");
    assert(first.actionId != 0 && first.actionId == first.candidate.actionId);
    assert(first.snapshotEpoch == f.snapshot.epoch && first.occurredAtMs == f.context.nowMs);
    assert(second.attempted && second.result == ActionResult::Failed);
    assert(second.reason == "action_execution_not_successful");
    assert(second.candidate.actionName == "listener_failure");
    assert(second.actionId != 0 && second.actionId == second.candidate.actionId);
}

void TestActionResultListenersAreBoundedAndSnapshotDuringDispatch()
{
    Fixture f;
    PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestAction>("listener_mutation");
    auto nextAction = std::make_shared<TestAction>("listener_mutation_next");
    assert(engine.RegisterAction(action) && engine.RegisterAction(nextAction));
    auto first = std::make_shared<CaptureActionResultListener>();
    auto second = std::make_shared<CaptureActionResultListener>();
    assert(engine.AddActionResultListener(first));

    class RegisterDuringCallback final : public ActionResultListener {
    public:
        RegisterDuringCallback(::PartyBotEngine::PartyBotEngine& engine,
                               std::shared_ptr<ActionResultListener> next)
            : engine(engine), next(std::move(next)) {}
        void OnActionResult(DecisionContext const&,
                            ActionResultEvent const&) override {
            if (!registered) {
                assert(engine.AddActionResultListener(next));
                registered = true;
            }
        }
        ::PartyBotEngine::PartyBotEngine& engine;
        std::shared_ptr<ActionResultListener> next;
        bool registered = false;
    };
    auto registering = std::make_shared<RegisterDuringCallback>(engine, second);
    assert(engine.AddActionResultListener(registering));

    auto trigger = std::make_shared<TestTrigger>("listener_mutation_trigger");
    trigger->generated.push_back(Candidate(
        "listener_mutation", 20.0f, ActionChannel::Utility));
    trigger->generated.push_back(Candidate(
        "listener_mutation_next", 10.0f, ActionChannel::Movement));
    auto strategy = std::make_shared<Strategy>("listener_mutation_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    engine.ExecuteSelected(f.context, report);
    assert(first->events.size() == 2);
    assert(second->events.size() == 1);
    assert(second->events[0].candidate.actionName == "listener_mutation_next");
    assert(engine.GetActionResultListenerCount() == 3);

    std::vector<std::shared_ptr<ActionResultListener>> extras;
    while (engine.GetActionResultListenerCount() < 32) {
        auto extra = std::make_shared<CaptureActionResultListener>();
        assert(engine.AddActionResultListener(extra));
        extras.push_back(extra);
    }
    assert(engine.GetActionResultListenerCount() == 32);
    assert(!engine.AddActionResultListener(
        std::make_shared<CaptureActionResultListener>()));
}

void TestCandidateTraceSelectionPreservesArbitrationSelectionAfterExecution()
{
    DecisionReport report;
    auto add = [&report](float priority, CandidateStatus status) {
        CandidateEvaluation evaluation;
        evaluation.candidate.finalPriority = priority;
        evaluation.status = status;
        report.candidates.push_back(evaluation);
    };

    add(70.0f, CandidateStatus::NotSelected);    // index 0
    add(10.0f, CandidateStatus::Executed);       // index 1, selected originally
    add(99.0f, CandidateStatus::RejectedMissingAction); // index 2
    add(90.0f, CandidateStatus::ExecutionFailed); // index 3, selected originally
    add(80.0f, CandidateStatus::RejectedImpossible); // index 4
    add(std::numeric_limits<float>::quiet_NaN(), CandidateStatus::RejectedInvalidPriority); // index 5
    report.selectedIndices = {1, 3};

    std::vector<std::size_t> const selected =
        SelectPriorityCandidateEvaluationIndices(report, 3);
    assert(selected.size() == 3);
    assert(selected[0] == 3); // A selected action remains first after execution failure.
    assert(selected[1] == 1); // Then the other selected action, despite lower priority.
    assert(selected[2] == 2); // Highest-priority unselected alternative.
    assert(SelectPriorityCandidateEvaluationIndices(report, 0).empty());
    assert(SelectPriorityCandidateEvaluationIndices(report, 20).size() == 6);

    DecisionSummary const summary = SummarizeDecisionReport(report);
    assert(summary.candidateCount == 6);
    assert(summary.selectedCount == 2);
    assert(summary.candidateDetailLimit == DecisionReport::MaxCandidateTraceRows);
    assert(summary.candidateDetailOmitted == 3);
}

void TestDecisionSummaryCountsAllArbitrationOutcomes()
{
    DecisionReport report;
    report.selectedIndices = {0, 1, 2};

    auto add = [&report](CandidateStatus status, char const* reason) {
        CandidateEvaluation evaluation;
        evaluation.status = status;
        evaluation.reason = reason ? reason : "";
        report.candidates.push_back(evaluation);
    };

    add(CandidateStatus::Selected, "");
    add(CandidateStatus::Executed, "");
    add(CandidateStatus::ExecutionFailed, "adapter_failed");
    add(CandidateStatus::NotSelected, "lower_priority_same_channel");
    add(CandidateStatus::RejectedMissingAction, "action_not_registered");
    add(CandidateStatus::RejectedImpossible, "out_of_range");
    add(CandidateStatus::RejectedUseless, "already_full_health");
    add(CandidateStatus::RejectedMultiplier, "hard_safety_veto");
    add(CandidateStatus::RejectedInvalidPriority, "non_finite_final_priority");
    add(CandidateStatus::RejectedThreatUnknown, "threat_impact_unknown");
    add(CandidateStatus::RejectedThreatBudget, "threat_budget_exceeded");
    // NotSelected without the channel-arbitration reason is not a channel loser.
    add(CandidateStatus::NotSelected, "other_not_selected");
    add(CandidateStatus::Queued, "pending_action_queued");

    DecisionSummary const summary = SummarizeDecisionReport(report);
    assert(summary.candidateCount == 13);
    assert(summary.selectedCount == 3);
    assert(summary.channelLoserCount == 1);
    assert(summary.missingActionCount == 1);
    assert(summary.impossibleCount == 1);
    assert(summary.uselessCount == 1);
    assert(summary.multiplierVetoCount == 1);
    assert(summary.invalidPriorityCount == 1);
    assert(summary.threatUnknownCount == 1);
    assert(summary.threatBudgetVetoCount == 1);
    assert(summary.executedCount == 1);
    assert(summary.executionFailedCount == 1);
    assert(summary.pendingQueuedCount == 1);
}

void TestPriorityAndChannelArbitration() {
    Fixture f; PartyBotEngine::PartyBotEngine engine;
    auto heal = std::make_shared<TestAction>("heal");
    auto shield = std::make_shared<TestAction>("shield");
    auto move = std::make_shared<TestAction>("move");
    assert(engine.RegisterAction(heal)); assert(engine.RegisterAction(shield)); assert(engine.RegisterAction(move));
    auto trigger = std::make_shared<TestTrigger>("party_needs_help");
    trigger->generated.push_back(Candidate("heal", 10.0f, ActionChannel::GlobalCooldown));
    trigger->generated.push_back(Candidate("shield", 8.0f, ActionChannel::GlobalCooldown));
    trigger->generated.push_back(Candidate("move", 4.0f, ActionChannel::Movement));
    auto strategy = std::make_shared<Strategy>("healer"); strategy->AddTrigger(trigger); engine.AddStrategy(strategy);
    engine.AddMultiplier(std::make_shared<TestMultiplier>("emergency_boost", 2.0f));
    DecisionReport report = engine.Evaluate(f.context);
    assert(report.snapshotEpoch == 7 && report.selectedIndices.size() == 2);
    assert(report.candidates[report.selectedIndices[0]].candidate.actionName == "heal");
    assert(report.candidates[report.selectedIndices[1]].candidate.actionName == "move");
    assert(report.candidates[1].reason == "lower_priority_same_channel");
    assert(heal->executeCount == 0 && shield->executeCount == 0 && move->executeCount == 0);
    engine.ExecuteSelected(f.context, report);
    assert(heal->executeCount == 1 && shield->executeCount == 0 && move->executeCount == 1);
    assert(report.candidates[report.selectedIndices[0]].status == CandidateStatus::Executed);
}
void TestEligibilityRejections() {
    Fixture f; PartyBotEngine::PartyBotEngine engine;
    auto impossible = std::make_shared<TestAction>("impossible"); impossible->possible = false;
    auto useless = std::make_shared<TestAction>("useless"); useless->useful = false;
    assert(engine.RegisterAction(impossible) && engine.RegisterAction(useless));
    auto trigger = std::make_shared<TestTrigger>("eligibility");
    trigger->generated.push_back(Candidate("impossible", 10.0f, ActionChannel::GlobalCooldown));
    trigger->generated.push_back(Candidate("useless", 9.0f, ActionChannel::GlobalCooldown));
    trigger->generated.push_back(Candidate("missing", 8.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("test"); strategy->AddTrigger(trigger); engine.AddStrategy(strategy);
    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.empty());
    assert(report.candidates[0].status == CandidateStatus::RejectedImpossible);
    assert(report.candidates[0].reason == "test_impossible");
    assert(report.candidates[1].status == CandidateStatus::RejectedUseless);
    assert(report.candidates[1].reason == "test_useless");
    assert(report.candidates[2].status == CandidateStatus::RejectedMissingAction);
}
void TestReservationOwnershipAndExpiry() {
    PartyCoordinator coordinator; Reservation r;
    r.type = ReservationType::CrowdControl; r.targetGuid = 101; r.ownerGuid = 201; r.expiresAtMs = 1500;
    assert(coordinator.TryReserve(r, 1000));
    assert(coordinator.HasReservation(ReservationType::CrowdControl, 101, 202, 1100));
    assert(!coordinator.HasReservation(ReservationType::CrowdControl, 101, 201, 1100));
    Reservation conflict = r; conflict.ownerGuid = 202;
    assert(!coordinator.TryReserve(conflict, 1200));
    coordinator.PruneExpired(1500);
    assert(!coordinator.HasReservation(ReservationType::CrowdControl, 101, 202, 1500));
    assert(coordinator.GetReservations().empty());
}
void TestResurrectionReservationOwnershipExpiryAndCompetition()
{
    PartyCoordinator coordinator;
    Reservation first;
    first.type = ReservationType::Resurrection;
    first.targetGuid = 7001;
    first.ownerGuid = 8001;
    first.spellId = 2010;
    first.expiresAtMs = 5000;
    assert(coordinator.TryReserve(first, 1000));
    assert(coordinator.HasReservation(ReservationType::Resurrection, 7001, 8002, 1100));

    // A second Bot cannot reserve the same corpse while the first cast is pending.
    Reservation competing = first;
    competing.ownerGuid = 8002;
    assert(!coordinator.TryReserve(competing, 1200));
    assert(!coordinator.ReleaseReservation(
        ReservationType::Resurrection, 7001, 8002));
    assert(coordinator.HasReservation(ReservationType::Resurrection, 7001, 8002, 1300));

    // Exact terminal cleanup for the first Bot leaves unrelated resurrection work intact.
    Reservation unrelated = first;
    unrelated.targetGuid = 7002;
    unrelated.ownerGuid = 8002;
    unrelated.expiresAtMs = 6000;
    assert(coordinator.TryReserve(unrelated, 1300));
    assert(coordinator.ReleaseReservation(
        ReservationType::Resurrection, 7001, 8001));
    assert(!coordinator.HasReservation(ReservationType::Resurrection, 7001, 0, 1400));
    assert(coordinator.HasReservation(ReservationType::Resurrection, 7002, 8001, 1400));
    assert(coordinator.GetReservations().size() == 1);

    // Missing Core terminal cannot strand a corpse indefinitely: the bounded
    // reservation expires and another Bot can claim it at the deadline.
    Reservation expiring = first;
    expiring.expiresAtMs = 2000;
    assert(coordinator.TryReserve(expiring, 1500));
    coordinator.PruneExpired(2000);
    assert(coordinator.GetReservations().size() == 1);
    assert(coordinator.TryReserve(competing, 2000));
    assert(coordinator.HasReservation(ReservationType::Resurrection, 7001, 8001, 2000));
}

void TestThreatReservationAggregationAndActionRelease() {
    PartyCoordinator coordinator;
    ThreatActionId const actionOne = coordinator.NextThreatActionId();
    ThreatActionId const actionTwo = coordinator.NextThreatActionId();
    assert(actionOne && actionTwo && actionOne != actionTwo);

    ThreatReservation botPetDamage;
    botPetDamage.ownerGuid = 1001;
    botPetDamage.threatOwnerGuid = 2001; // The pet, not the bot, receives Core threat.
    botPetDamage.hostileGuid = 3001;
    botPetDamage.actionId = actionOne;
    botPetDamage.threatPoints = 12.5f;
    botPetDamage.expiresAtMs = 1500;
    assert(coordinator.TryReserveThreatImpact(botPetDamage, 1000));
    assert(std::fabs(coordinator.GetReservedThreatImpact(3001, 2001, 0, 1100) - 12.5f) < 0.0001f);

    // Re-submitting one action/hostile/recipient row replaces it rather than double-counting.
    botPetDamage.threatPoints = 17.5f;
    assert(coordinator.TryReserveThreatImpact(botPetDamage, 1100));
    assert(std::fabs(coordinator.GetReservedThreatImpact(3001, 2001, 0, 1100) - 17.5f) < 0.0001f);

    // One logical action may fan out to another hostile and/or another actual threat owner.
    ThreatReservation secondHostile = botPetDamage;
    secondHostile.hostileGuid = 3002;
    secondHostile.threatPoints = 4.0f;
    assert(coordinator.TryReserveThreatImpact(secondHostile, 1100));
    ThreatReservation botSelfDamage = botPetDamage;
    botSelfDamage.threatOwnerGuid = 1001;
    botSelfDamage.threatPoints = 6.0f;
    assert(coordinator.TryReserveThreatImpact(botSelfDamage, 1100));

    ThreatReservation concurrentAction = botPetDamage;
    concurrentAction.ownerGuid = 1002;
    concurrentAction.actionId = actionTwo;
    concurrentAction.threatPoints = 2.5f;
    assert(coordinator.TryReserveThreatImpact(concurrentAction, 1100));

    assert(coordinator.GetThreatReservationCount() == 4);
    assert(std::fabs(coordinator.GetReservedThreatImpact(3001, 2001, 0, 1100) - 20.0f) < 0.0001f);
    assert(std::fabs(coordinator.GetReservedThreatImpact(3001, 2001, actionOne, 1100) - 2.5f) < 0.0001f);
    assert(std::fabs(coordinator.GetReservedThreatImpact(3002, 2001, 0, 1100) - 4.0f) < 0.0001f);
    assert(std::fabs(coordinator.GetReservedThreatImpact(3001, 1001, 0, 1100) - 6.0f) < 0.0001f);
    assert(coordinator.GetReservedThreatImpact(3001, 9999, 0, 1100) == 0.0f);

    // Completing one action releases every hostile/recipient row it created.
    coordinator.ReleaseThreatAction(actionOne);
    assert(coordinator.GetThreatReservationCount() == 1);
    assert(std::fabs(coordinator.GetReservedThreatImpact(3001, 2001, 0, 1100) - 2.5f) < 0.0001f);
    assert(coordinator.GetReservedThreatImpact(3002, 2001, 0, 1100) == 0.0f);

    // Owner cleanup is by the bot that owns the action, not by the pet GUID that receives threat.
    coordinator.ReleaseOwner(1002);
    assert(coordinator.GetThreatReservationCount() == 0);
    assert(coordinator.GetReservedThreatImpact(3001, 2001, 0, 1100) == 0.0f);
}

void TestThreatReservationExpiryAndClockWrap() {
    PartyCoordinator coordinator;
    ThreatReservation reservation;
    reservation.ownerGuid = 10;
    reservation.threatOwnerGuid = 20;
    reservation.hostileGuid = 30;
    reservation.actionId = coordinator.NextThreatActionId();
    reservation.threatPoints = 8.25f;
    reservation.expiresAtMs = 1500;
    assert(coordinator.TryReserveThreatImpact(reservation, 1000));
    assert(std::fabs(coordinator.GetReservedThreatImpact(30, 20, 0, 1499) - 8.25f) < 0.0001f);
    assert(coordinator.GetReservedThreatImpact(30, 20, 0, 1500) == 0.0f);
    // Read-only budget queries ignore expired rows without mutating the coordinator.
    assert(coordinator.GetThreatReservationCount() == 1);
    coordinator.PruneExpired(1500);
    assert(coordinator.GetThreatReservationCount() == 0);

    reservation.actionId = coordinator.NextThreatActionId();
    reservation.expiresAtMs = 0x10u;
    assert(coordinator.TryReserveThreatImpact(reservation, 0xfffffff0u));
    assert(std::fabs(coordinator.GetReservedThreatImpact(30, 20, 0, 0xffffffffu) - 8.25f) < 0.0001f);
    assert(coordinator.GetReservedThreatImpact(30, 20, 0, 0x10u) == 0.0f);
    assert(coordinator.GetThreatReservationCount() == 1);
    coordinator.PruneExpired(0x10u);
    assert(coordinator.GetThreatReservationCount() == 0);
}

void TestThreatReservationValidationAndBoundedCapacity() {
    PartyCoordinator coordinator;
    ThreatReservation invalid;
    // A missing logical action owner must be rejected even when all other fields are valid.
    invalid.ownerGuid = 0;
    invalid.threatOwnerGuid = 2;
    invalid.hostileGuid = 3;
    invalid.actionId = coordinator.NextThreatActionId();
    invalid.threatPoints = 1.0f;
    invalid.expiresAtMs = 2000;
    assert(!coordinator.TryReserveThreatImpact(invalid, 1000));

    ThreatReservation valid = invalid;
    valid.ownerGuid = 1;
    valid.threatOwnerGuid = 2;
    valid.hostileGuid = 3;
    valid.actionId = coordinator.NextThreatActionId();
    valid.expiresAtMs = 2000;
    assert(valid.actionId && coordinator.TryReserveThreatImpact(valid, 1000));

    ThreatReservation candidate = valid;
    candidate.actionId = coordinator.NextThreatActionId();
    candidate.ownerGuid = 0;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.threatOwnerGuid = 0;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.hostileGuid = 0;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = 0;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.threatPoints = 0.0f;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.threatPoints = -1.0f;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.threatPoints = std::nanf("");
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId();
    candidate.threatPoints = std::numeric_limits<float>::infinity();
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.expiresAtMs = 1000;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));
    candidate = valid; candidate.actionId = coordinator.NextThreatActionId(); candidate.expiresAtMs = 0x800003e8u;
    assert(!coordinator.TryReserveThreatImpact(candidate, 1000));

    coordinator.ReleaseOwner(1);
    assert(coordinator.GetThreatReservationCount() == 0);

    ThreatReservation first;
    for (std::uint32_t i = 0; i < 128; ++i) {
        ThreatReservation row;
        row.ownerGuid = 100;
        row.threatOwnerGuid = 1000 + i;
        row.hostileGuid = 2000 + i;
        row.actionId = coordinator.NextThreatActionId();
        row.threatPoints = 1.0f;
        row.expiresAtMs = 5000;
        assert(row.actionId && coordinator.TryReserveThreatImpact(row, 1000));
        if (i == 0)
            first = row;
    }
    assert(coordinator.GetThreatReservationCount() == 128);

    ThreatReservation overflow = first;
    overflow.hostileGuid = 99999;
    overflow.actionId = coordinator.NextThreatActionId();
    assert(!coordinator.TryReserveThreatImpact(overflow, 1000));
    assert(coordinator.GetThreatReservationCount() == 128);

    // An idempotent refresh remains legal at capacity.
    first.threatPoints = 2.0f;
    assert(coordinator.TryReserveThreatImpact(first, 1000));
    assert(coordinator.GetThreatReservationCount() == 128);
    assert(std::fabs(coordinator.GetReservedThreatImpact(first.hostileGuid,
        first.threatOwnerGuid, 0, 1000) - 2.0f) < 0.0001f);

    coordinator.ReleaseOwner(100);
    assert(coordinator.GetThreatReservationCount() == 0);
}

void TestInvalidPriorityAndMultiplierVeto() {
    Fixture f; PartyBotEngine::PartyBotEngine engine;
    auto action = std::make_shared<TestAction>("damage"); assert(engine.RegisterAction(action));
    auto trigger = std::make_shared<TestTrigger>("invalid");
    trigger->generated.push_back(Candidate("damage", std::nanf(""), ActionChannel::GlobalCooldown));
    trigger->generated.push_back(Candidate("damage", 5.0f, ActionChannel::Movement));
    auto strategy = std::make_shared<Strategy>("test"); strategy->AddTrigger(trigger); engine.AddStrategy(strategy);
    engine.AddMultiplier(std::make_shared<TestMultiplier>("encounter_veto", 0.0f));
    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.empty());
    assert(report.candidates[0].status == CandidateStatus::RejectedInvalidPriority);
    assert(report.candidates[1].status == CandidateStatus::RejectedMultiplier);
    assert(report.candidates[1].reason == "encounter_veto");
}
void TestSnapshotCacheFreshnessAndEpoch() {
    PartyCoordinator coordinator;
    assert(coordinator.GetSnapshotIfFresh(1000, 250) == nullptr);

    std::uint32_t const firstEpoch = coordinator.NextSnapshotEpoch();
    PartySnapshot snapshot;
    snapshot.groupId = 101;
    snapshot.capturedAtMs = 1000;
    snapshot.epoch = firstEpoch;
    coordinator.SetSnapshot(snapshot);

    PartySnapshot const* fresh = coordinator.GetSnapshotIfFresh(1250, 250);
    assert(fresh && fresh->groupId == 101 && fresh->epoch == firstEpoch);
    assert(coordinator.GetSnapshotIfFresh(1251, 250) == nullptr);
    assert(coordinator.NextSnapshotEpoch() == firstEpoch + 1);

    PartySnapshot wrapped;
    wrapped.capturedAtMs = 0xfffffff0u;
    coordinator.SetSnapshot(wrapped);
    assert(coordinator.GetSnapshotIfFresh(10u, 26u) != nullptr);
    assert(coordinator.GetSnapshotIfFresh(11u, 26u) == nullptr);

    coordinator.InvalidateSnapshot();
    assert(coordinator.GetSnapshotIfFresh(10u, 100u) == nullptr);
}

void TestCoordinatorRegistryIdentityAndBattlegroundBypass() {
    assert(!PartyCoordinatorRegistry::GetOrCreate(0, 1, 1));

    std::shared_ptr<PartyCoordinator> const first =
        PartyCoordinatorRegistry::GetOrCreate(700001, 1, 10);
    std::shared_ptr<PartyCoordinator> const same =
        PartyCoordinatorRegistry::GetOrCreate(700001, 1, 10);
    std::shared_ptr<PartyCoordinator> const otherMap =
        PartyCoordinatorRegistry::GetOrCreate(700001, 2, 10);
    std::shared_ptr<PartyCoordinator> const otherInstance =
        PartyCoordinatorRegistry::GetOrCreate(700001, 1, 11);

    assert(first && same && first == same);
    assert(otherMap && first != otherMap);
    assert(otherInstance && first != otherInstance);

    std::vector<std::shared_ptr<PartyCoordinator>> concurrent(8);
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < concurrent.size(); ++i) {
        workers.emplace_back([&concurrent, i]() {
            concurrent[i] = PartyCoordinatorRegistry::GetOrCreate(700002, 571, 42);
        });
    }
    for (auto& worker : workers)
        worker.join();

    for (auto const& shard : concurrent)
        assert(shard && shard == concurrent.front());

    PartyCoordinatorRegistry::PruneExpired();
}

void ConfigureMember(PartyMemberSnapshot& member, Guid guid, PartyRole role,
                     std::uint32_t health, std::uint32_t maxHealth, bool mainTank = false) {
    member.guid = guid;
    member.roleId = static_cast<std::uint8_t>(role);
    member.health = health;
    member.maxHealth = maxHealth;
    member.online = true;
    member.alive = true;
    member.isMainTank = mainTank;
}
void TestNonBotPartyMemberStateChangesAreSampledWithoutDuplicates() {
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 995;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 1;
    PartyMemberSnapshot human;
    human.guid = 996; human.online = true; human.alive = true;
    human.health = 100; human.maxHealth = 100; human.inCombat = true;
    PartyMemberSnapshot bot = human;
    bot.guid = 997; bot.isPlayerBot = true; bot.health = 80;
    snapshot.members.push_back(human);
    snapshot.members.push_back(bot);

    assert(coordinator.ObserveEncounter(snapshot, 1000, 30000).kind ==
           EncounterTransitionKind::Started);
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
    snapshot.members[0].health = 65;
    snapshot.members[1].health = 40;
    std::vector<PartyMemberStateChange> changes =
        coordinator.ObserveNonBotPartyMemberStateChanges(snapshot);
    assert(changes.size() == 1 && changes[0].memberGuid == human.guid);
    assert(changes[0].healthBefore == 100 && changes[0].healthAfter == 65);
    assert(changes[0].wasAlive && changes[0].isAlive);
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());

    snapshot.members[0].health = 0; snapshot.members[0].alive = false;
    changes = coordinator.ObserveNonBotPartyMemberStateChanges(snapshot);
    assert(changes.size() == 1 && changes[0].wasAlive && !changes[0].isAlive);
    assert(changes[0].healthBefore == 65 && changes[0].healthAfter == 0);
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());

    snapshot.members[0].health = 50; snapshot.members[0].alive = true;
    changes = coordinator.ObserveNonBotPartyMemberStateChanges(snapshot);
    assert(changes.size() == 1 && !changes[0].wasAlive && changes[0].isAlive);

    snapshot.members.erase(snapshot.members.begin());
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
    snapshot.members.insert(snapshot.members.begin(), human);
    snapshot.members[0].health = 25;
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());

    assert(coordinator.CloseEncounter(2000, "test_close").kind ==
           EncounterTransitionKind::Ended);
    snapshot.members[0].health = 10;
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
}

void TestNonBotPartyMemberStateSamplingRejectsForeignShardSnapshots() {
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 995;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 1;
    PartyMemberSnapshot human;
    human.guid = 996;
    human.online = true;
    human.alive = true;
    human.health = 100;
    human.maxHealth = 100;
    human.inCombat = true;
    snapshot.members.push_back(human);

    assert(coordinator.ObserveEncounter(snapshot, 1000, 30000).kind ==
           EncounterTransitionKind::Started);
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());

    // Each mismatched identity is ignored without destroying the valid
    // baseline. Returning to the owning shard must report the accumulated
    // state delta against the original observation.
    snapshot.members[0].health = 80;
    snapshot.groupId = 996;
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
    snapshot.groupId = 995;

    snapshot.members[0].health = 70;
    snapshot.mapId = 2;
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
    snapshot.mapId = 1;

    snapshot.members[0].health = 60;
    snapshot.instanceId = 10;
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
    snapshot.instanceId = 9;

    std::vector<PartyMemberStateChange> changes =
        coordinator.ObserveNonBotPartyMemberStateChanges(snapshot);
    assert(changes.size() == 1);
    assert(changes[0].memberGuid == human.guid);
    assert(changes[0].healthBefore == 100 && changes[0].healthAfter == 60);
    assert(coordinator.ObserveNonBotPartyMemberStateChanges(snapshot).empty());
}

void TestSupportedSingleTargetDirectHealSpellShape() {
    constexpr std::uint32_t kUnitCaster = 1;
    constexpr std::uint32_t kUnitFriendNearCaster = 3;
    constexpr std::uint32_t kUnitFriend = 21;
    constexpr std::uint32_t kUnit = 25;
    constexpr std::uint32_t kPartyAoeAtSource = 33;
    constexpr std::uint32_t kUnitParty = 35;
    constexpr std::uint32_t kUnitFriendChainHeal = 45;
    constexpr std::uint32_t kUnitRaid = 57;

    auto supportedMode = [](std::uint32_t mode)
    {
        return IsSupportedDirectHealTargetMode(
            mode, kUnitFriend, kUnitParty, kUnitRaid, kUnit, kUnitCaster);
    };

    assert(supportedMode(kUnitFriend));
    assert(supportedMode(kUnitParty));
    assert(supportedMode(kUnitRaid));
    assert(supportedMode(kUnit));
    assert(supportedMode(kUnitCaster));
    assert(IsSupportedDirectHealTargetRelation(true, true));
    assert(!IsSupportedDirectHealTargetRelation(true, false));
    assert(IsSupportedDirectHealTargetRelation(false, false));
    assert(!supportedMode(kUnitFriendNearCaster));
    assert(!supportedMode(kPartyAoeAtSource));
    assert(!supportedMode(kUnitFriendChainHeal));

    assert(IsSupportedSingleTargetDirectHealEffectShape(
        1, true, false, true, true, false));
    assert(IsSupportedSingleTargetDirectHealEffectShape(
        1, true, true, false, false, true));

    // Multiple effects, mixed effects, no selected target, two selected
    // targets and unsupported implicit target modes all remain outside scope.
    assert(!IsSupportedSingleTargetDirectHealEffectShape(
        2, true, false, true, true, false));
    assert(!IsSupportedSingleTargetDirectHealEffectShape(
        1, false, false, true, true, false));
    assert(!IsSupportedSingleTargetDirectHealEffectShape(
        1, true, true, true, false, false));
    assert(!IsSupportedSingleTargetDirectHealEffectShape(
        1, true, false, false, true, true));
    assert(!IsSupportedSingleTargetDirectHealEffectShape(
        1, true, false, true, false, false));
    assert(!IsSupportedSingleTargetDirectHealEffectShape(
        0, true, false, true, true, false));
}

void TestDirectHealHelpfulProcRiskClassifier() {
    constexpr std::uint32_t kDealHelpfulSpell = 1u << 4;
    constexpr std::uint32_t kTakeHelpfulSpell = 1u << 7;
    constexpr std::uint32_t kHarmfulSpell = 1u << 10;
    constexpr std::uint32_t kHelpfulSpellMask =
        kDealHelpfulSpell | kTakeHelpfulSpell;

    assert(!HasPotentialHelpfulSpellProc(
        false, false, 0, kDealHelpfulSpell));
    assert(HasPotentialHelpfulSpellProc(
        true, false, 0, kDealHelpfulSpell));
    assert(HasPotentialHelpfulSpellProc(
        false, true, 0, kDealHelpfulSpell));
    assert(HasPotentialHelpfulSpellProc(
        false, false, kDealHelpfulSpell, kDealHelpfulSpell));
    assert(HasPotentialHelpfulSpellProc(
        false, false, kTakeHelpfulSpell, kTakeHelpfulSpell));
    assert(!HasPotentialHelpfulSpellProc(
        false, false, kHarmfulSpell, kHelpfulSpellMask));
    assert(!HasPotentialHelpfulSpellProc(
        false, false, kDealHelpfulSpell, kTakeHelpfulSpell));

    // A configured SpellScript can issue nested casts outside the generic
    // proc-flag path; it is fail-closed until its callback is audited.
    assert(!HasUnboundedDirectHealSpellScript(false));
    assert(HasUnboundedDirectHealSpellScript(true));

    // TriggerSpell/Missile explicitly dispatch child casts, while Dummy and
    // ScriptEffect can issue hard-coded or ScriptMgr-driven nested casts.
    assert(!HasUnboundedDirectHealSecondarySpellEffect(
        false, false, false, false));
    assert(HasUnboundedDirectHealSecondarySpellEffect(
        true, false, false, false));
    assert(HasUnboundedDirectHealSecondarySpellEffect(
        false, true, false, false));
    assert(HasUnboundedDirectHealSecondarySpellEffect(
        false, false, true, false));
    assert(HasUnboundedDirectHealSecondarySpellEffect(
        false, false, false, true));

    // Add-target-trigger auras are checked at their actual active effect
    // index, including nonzero slots; inactive or nonmatching slots are safe.
    assert(!HasMatchingAddTargetTriggerAuraEffect(false, true, true));
    assert(!HasMatchingAddTargetTriggerAuraEffect(true, false, true));
    assert(!HasMatchingAddTargetTriggerAuraEffect(true, true, false));
    assert(HasMatchingAddTargetTriggerAuraEffect(true, true, true));
}

void TestDirectHealThreatComponentsSplitAcrossAllHostileReferences() {
    DirectHealThreatComponents components;

    // Core divides helpful healing assist and positive flat spell threat over
    // every HostileReference independently; only the assist path receives the
    // heal's spell threat multiplier before ThreatCalcHelper runs.
    assert(CalculateDirectHealThreatComponents(
        200.0f, 0.5f, 1.5f, 24.0f, 4, components));
    assert(std::fabs(components.assistThreatPerReference - 37.5f) < 0.0001f);
    assert(std::fabs(components.flatThreatPerReference - 6.0f) < 0.0001f);

    // Paladin's healing-assist coefficient is 25%; a zero flat row contributes
    // no second component and must not be mistaken for a missing recipient.
    assert(CalculateDirectHealThreatComponents(
        100.0f, 0.25f, 2.0f, 0.0f, 2, components));
    assert(std::fabs(components.assistThreatPerReference - 25.0f) < 0.0001f);
    assert(components.flatThreatPerReference == 0.0f);

    // No references cannot be treated as a completed estimate: callers must
    // preserve Unknown until the action's other threat/proc paths are closed.
    assert(!CalculateDirectHealThreatComponents(
        100.0f, 0.5f, 1.0f, 0.0f, 0, components));
    assert(components.assistThreatPerReference == 0.0f);
    assert(components.flatThreatPerReference == 0.0f);

    assert(!CalculateDirectHealThreatComponents(
        -1.0f, 0.5f, 1.0f, 0.0f, 1, components));
    assert(!CalculateDirectHealThreatComponents(
        100.0f, 1.1f, 1.0f, 0.0f, 1, components));
    assert(!CalculateDirectHealThreatComponents(
        100.0f, 0.5f, -1.0f, 0.0f, 1, components));
    assert(!CalculateDirectHealThreatComponents(
        100.0f, 0.5f, 1.0f, -1.0f, 1, components));
    assert(!CalculateDirectHealThreatComponents(
        std::numeric_limits<float>::max(), 1.0f, 2.0f,
        0.0f, 1, components));
}

void TestPriestDirectHealActionRemainsFailClosedUntilThreatClosure() {
    Fixture f;
    f.bot.classId = PriestDirectHealAction::PriestClassId;
    f.bot.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    f.bot.isPlayerBot = true;
    f.bot.power = 500;
    f.bot.directHealSpells = {{2050, 120, 25}};

    PartyMemberSnapshot target;
    ConfigureMember(target, 22, PartyRole::Tank, 30, 100, true);
    target.mapId = f.snapshot.mapId;
    target.instanceId = f.snapshot.instanceId;
    f.snapshot.members.push_back(target);

    ActionCandidate candidate = Candidate(
        "core_priest_direct_heal", 75.0f, ActionChannel::GlobalCooldown);
    candidate.targetGuid = target.guid;
    candidate.spellId = 2050;

    PriestDirectHealAction action;
    std::string reason;
    assert(action.IsPossible(f.context, candidate, reason));
    assert(action.IsUseful(f.context, candidate, reason));

    ThreatImpactEstimate const estimate =
        action.EstimateThreatImpact(f.context, candidate);
    assert(estimate.completeness == ThreatImpactCompleteness::Unknown);
    assert(estimate.snapshotEpoch == f.snapshot.epoch);
    assert(estimate.impacts.empty());
    assert(!estimate.reason.empty());

    // The action may be exercised in isolation, but the Engine must reject it
    // before any Core side effect until the entire heal threat envelope closes.
    PartyBotEngine::PartyBotEngine engine;
    assert(engine.RegisterAction(std::shared_ptr<Action>(
        new PriestDirectHealAction())));
    std::shared_ptr<TestTrigger> trigger(new TestTrigger("priest_heal_probe"));
    trigger->generated.push_back(candidate);
    std::shared_ptr<Strategy> strategy(new Strategy("priest_heal_probe"));
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);
    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.empty());
    assert(report.candidates.size() == 1);
    assert(report.candidates[0].status == CandidateStatus::RejectedThreatUnknown);
    assert(report.candidates[0].reason == "threat_impact_estimate_unknown");

    f.bot.power = 1;
    assert(!action.IsPossible(f.context, candidate, reason));
    f.bot.power = 500;
    f.snapshot.members[0].health = f.snapshot.members[0].maxHealth;
    assert(!action.IsPossible(f.context, candidate, reason));
    f.snapshot.members[0].health = 30;

    // Every boundary remains snapshot-local and fail-closed before Core.
    candidate.channel = ActionChannel::Utility;
    assert(!action.IsPossible(f.context, candidate, reason));
    candidate.channel = ActionChannel::GlobalCooldown;
    f.bot.currentSpellId = 2050;
    assert(!action.IsPossible(f.context, candidate, reason));
    f.bot.currentSpellId = 0;
    f.bot.directHealSpells.clear();
    assert(!action.IsPossible(f.context, candidate, reason));
    f.bot.directHealSpells = {{2050, 120, 25}};
    f.snapshot.members[0].alive = false;
    assert(!action.IsPossible(f.context, candidate, reason));
    f.snapshot.members[0].alive = true;
    f.snapshot.members[0].online = false;
    assert(!action.IsPossible(f.context, candidate, reason));
    f.snapshot.members[0].online = true;
    f.snapshot.members[0].mapId += 1;
    assert(!action.IsPossible(f.context, candidate, reason));
}

void TestPriestDirectHealSelectionUsesAffordableClosestRank() {
    PartyMemberSnapshot healer;
    ConfigureMember(healer, 11, PartyRole::Healer, 100, 100);
    healer.isPlayerBot = true;
    healer.power = 500;
    healer.directHealSpells = {
        {501, 1200, 250},
        {502, 800, 180},
        {503, 400, 90},
        {504, 200, 40}
    };

    PriestDirectHealSelection selection;
    assert(SelectPriestDirectHealSpell(healer, 700, selection));
    assert(selection.spellId == 502);
    assert(selection.baseHealing == 800);
    assert(selection.manaCost == 180);

    // When no affordable rank covers the deficit, use the strongest
    // affordable rank rather than selecting an unaffordable larger spell.
    healer.power = 100;
    assert(SelectPriestDirectHealSpell(healer, 700, selection));
    assert(selection.spellId == 503);
    assert(selection.baseHealing == 400);
    assert(selection.manaCost == 90);

    // If all affordable ranks under-heal, choose the strongest one.
    healer.power = 500;
    assert(SelectPriestDirectHealSpell(healer, 1300, selection));
    assert(selection.spellId == 501);
    assert(selection.baseHealing == 1200);

    // Equal healing chooses lower mana, then lower spell ID deterministically.
    healer.directHealSpells = {
        {602, 800, 180},
        {601, 800, 120},
        {600, 800, 120}
    };
    assert(SelectPriestDirectHealSpell(healer, 700, selection));
    assert(selection.spellId == 600);
    assert(selection.manaCost == 120);

    healer.directHealSpells.push_back({600, 1000, 100});
    assert(!SelectPriestDirectHealSpell(healer, 700, selection));
    assert(!selection.IsValid());

    healer.directHealSpells.clear();
    assert(!SelectPriestDirectHealSpell(healer, 700, selection));
    healer.directHealSpells = {{700, 800, 180}};
    healer.isPlayerBot = false;
    assert(!SelectPriestDirectHealSpell(healer, 700, selection));
    healer.isPlayerBot = true;
    healer.online = false;
    assert(!SelectPriestDirectHealSpell(healer, 700, selection));
    healer.online = true;
    healer.alive = false;
    assert(!SelectPriestDirectHealSpell(healer, 700, selection));
    healer.alive = true;
    healer.roleId = static_cast<std::uint8_t>(PartyRole::Tank);
    assert(!SelectPriestDirectHealSpell(healer, 700, selection));
}

void TestClassNeutralHealerPolicyAcceptsPaladinProfile()
{
    PartySnapshot snapshot;
    snapshot.groupId = 910;
    snapshot.mapId = 1;
    snapshot.instanceId = 12;
    snapshot.epoch = 91;

    PartyMemberSnapshot paladin, tank;
    ConfigureMember(paladin, 101, PartyRole::Healer, 100, 100);
    paladin.classId = 2; // Vanilla Paladin; the shared policy has no Priest gate.
    paladin.isPlayerBot = true;
    paladin.power = 40;
    paladin.directHealSpells = {
        {1501, 70, 20, 2500, true},
        {1502, 120, 35, 3500, true},
        // A larger spell with an unsupported fan-out shape must not win rank selection.
        {1503, 900, 0, 0, false}
    };
    paladin.mapId = snapshot.mapId;
    paladin.instanceId = snapshot.instanceId;

    ConfigureMember(tank, 202, PartyRole::Tank, 20, 100, true);
    tank.mapId = snapshot.mapId;
    tank.instanceId = snapshot.instanceId;
    snapshot.members = {paladin, tank};

    HealerTargetSelection target;
    assert(SelectHealerTarget(snapshot, paladin.guid, target));
    assert(target.targetGuid == tank.guid);
    assert(target.missingHealth == 80);
    assert(target.deficitBasisPoints == 8000);
    assert(GetUncoveredMissingHealth(
        snapshot, *snapshot.FindMember(tank.guid), paladin.guid, nullptr, 0) == 80);

    DirectHealSpellSelection spell;
    assert(SelectDirectHealSpell(*snapshot.FindMember(paladin.guid), 80, spell));
    assert(spell.spellId == 1502);
    assert(spell.baseHealing == 120);
    assert(spell.manaCost == 35);

    // Cast time is available to a class adapter for its rank policy, but does
    // not authorize dispatch or imply that the current threat gate accepts it.
    assert(snapshot.FindMember(paladin.guid)->directHealSpells[0].castTimeMs == 2500);

    // If no affordable supported rank covers the deficit, choose the strongest
    // affordable supported under-heal. The unsupported stronger row stays out.
    snapshot.members[0].power = 25;
    assert(SelectDirectHealSpell(snapshot.members[0], 80, spell));
    assert(spell.spellId == 1501);

    HealerDecisionSelection decision;
    assert(SelectHealerDecision(snapshot, paladin.guid, decision));
    assert(decision.IsValid());
    assert(decision.target.targetGuid == tank.guid);
    assert(decision.spell.spellId == 1501);

    // The legacy Priest entry point retains its explicit class restriction.
    PriestHealingTargetSelection priestTarget;
    assert(!SelectPriestHealingTarget(snapshot, paladin.guid, priestTarget));
    assert(!priestTarget.IsValid());

    snapshot.members[0].roleId =
        static_cast<std::uint8_t>(PartyRole::Tank);
    assert(!SelectHealerTarget(snapshot, paladin.guid, target));
    assert(!target.IsValid());
}

void TestPriestHealingTargetSelectionIsDeterministicAndSnapshotBounded()
{
    PartySnapshot snapshot;
    snapshot.groupId = 901;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 84;

    PartyMemberSnapshot healer, mainTank, ranged, melee, foreignMap, foreignInstance;
    ConfigureMember(healer, 11, PartyRole::Healer, 90, 100);
    healer.classId = 5;
    healer.isPlayerBot = true;
    healer.power = 60;
    healer.mapId = snapshot.mapId;
    healer.instanceId = snapshot.instanceId;
    healer.directHealSpells = {{2050, 100, 20}, {2052, 600, 55}};

    ConfigureMember(mainTank, 22, PartyRole::Tank, 20, 100, true);
    mainTank.mapId = snapshot.mapId;
    mainTank.instanceId = snapshot.instanceId;

    // Same 20% current health as the Main Tank; the exact-ratio tie goes to tank.
    ConfigureMember(ranged, 33, PartyRole::RangedDamage, 100, 500);
    ranged.mapId = snapshot.mapId;
    ranged.instanceId = snapshot.instanceId;

    ConfigureMember(melee, 44, PartyRole::MainTank, 21, 100);
    melee.mapId = snapshot.mapId;
    melee.instanceId = snapshot.instanceId;

    ConfigureMember(foreignMap, 55, PartyRole::RangedDamage, 1, 100);
    foreignMap.mapId = snapshot.mapId + 1;
    foreignMap.instanceId = snapshot.instanceId;

    ConfigureMember(foreignInstance, 66, PartyRole::RangedDamage, 1, 100);
    foreignInstance.mapId = snapshot.mapId;
    foreignInstance.instanceId = snapshot.instanceId + 1;

    PartyMemberSnapshot offline, dead, full, contradictory;
    ConfigureMember(offline, 77, PartyRole::RangedDamage, 1, 100);
    offline.mapId = snapshot.mapId; offline.instanceId = snapshot.instanceId;
    offline.online = false;
    ConfigureMember(dead, 88, PartyRole::RangedDamage, 1, 100);
    dead.mapId = snapshot.mapId; dead.instanceId = snapshot.instanceId;
    dead.alive = false;
    ConfigureMember(full, 99, PartyRole::RangedDamage, 100, 100);
    full.mapId = snapshot.mapId; full.instanceId = snapshot.instanceId;
    ConfigureMember(contradictory, 100, PartyRole::RangedDamage, 0, 100);
    contradictory.mapId = snapshot.mapId; contradictory.instanceId = snapshot.instanceId;

    snapshot.members = {healer, mainTank, ranged, melee, foreignMap,
                        foreignInstance, offline, dead, full, contradictory};

    PriestHealingTargetSelection target;
    assert(SelectPriestHealingTarget(snapshot, healer.guid, target));
    assert(target.targetGuid == mainTank.guid);
    assert(target.missingHealth == 80);
    assert(target.deficitBasisPoints == 8000);

    // A lower health fraction beats Main Tank preference.
    snapshot.members[1].health = 35;
    assert(SelectPriestHealingTarget(snapshot, healer.guid, target));
    assert(target.targetGuid == ranged.guid);
    assert(target.missingHealth == 400);

    // Equal percentages among non-tanks prefer the larger absolute deficit.
    snapshot.members[1].health = 50;
    snapshot.members[1].isMainTank = false;
    snapshot.members[2].health = 20;
    snapshot.members[2].maxHealth = 100;
    snapshot.members[2].roleId = static_cast<std::uint8_t>(PartyRole::RangedDamage);
    snapshot.members[2].isMainTank = false;
    snapshot.members[3].health = 200;
    snapshot.members[3].maxHealth = 1000;
    snapshot.members[3].roleId = static_cast<std::uint8_t>(PartyRole::RangedDamage);
    snapshot.members[3].isMainTank = false;
    assert(SelectPriestHealingTarget(snapshot, healer.guid, target));
    assert(target.targetGuid == melee.guid); // Same ratio, 800 missing beats 80.

    // A final exact tie falls back to stable, lower GUID.
    snapshot.members[2].health = 100;
    snapshot.members[2].maxHealth = 500;
    snapshot.members[3].health = 100;
    snapshot.members[3].maxHealth = 500;
    assert(SelectPriestHealingTarget(snapshot, healer.guid, target));
    assert(target.targetGuid == ranged.guid);

    // Roster order does not change the choice.
    std::reverse(snapshot.members.begin(), snapshot.members.end());
    assert(SelectPriestHealingTarget(snapshot, healer.guid, target));
    assert(target.targetGuid == ranged.guid);

    PriestHealingDecisionSelection decision;
    // The selected target has 400 missing health; rank 2052 covers it and
    // remains affordable, while rank 2050 under-heals.
    assert(SelectPriestHealingDecision(snapshot, healer.guid, decision));
    assert(decision.IsValid());
    assert(decision.target.targetGuid == ranged.guid);
    assert(decision.target.missingHealth == 400);
    assert(decision.spell.spellId == 2052);
    assert(decision.spell.manaCost == 55);

    // Without an affordable direct-heal rank, do not return a target-only plan.
    auto healerIt = std::find_if(
        snapshot.members.begin(), snapshot.members.end(),
        [&healer](PartyMemberSnapshot const& member)
        {
            return member.guid == healer.guid;
        });
    assert(healerIt != snapshot.members.end());
    healerIt->power = 10;
    assert(!SelectPriestHealingDecision(snapshot, healer.guid, decision));
    assert(!decision.IsValid());

    // Duplicate member GUIDs make identity ambiguous and fail closed.
    healerIt->power = 60;
    snapshot.members.push_back(snapshot.members[1]);
    assert(!SelectPriestHealingTarget(snapshot, healer.guid, target));
    assert(!target.IsValid());

    snapshot.members.pop_back();
    assert(!SelectPriestHealingTarget(snapshot, 0, target));
    assert(!target.IsValid());

    // A stale/non-healer actor must never generate a Priest heal decision.
    healerIt = std::find_if(
        snapshot.members.begin(), snapshot.members.end(),
        [&healer](PartyMemberSnapshot const& member)
        {
            return member.guid == healer.guid;
        });
    assert(healerIt != snapshot.members.end());
    healerIt->classId = 8;
    assert(!SelectPriestHealingTarget(snapshot, healer.guid, target));
    healerIt->classId = 5;
    healerIt->roleId = static_cast<std::uint8_t>(PartyRole::Tank);
    assert(!SelectPriestHealingDecision(snapshot, healer.guid, decision));
}

void TestPriestHealingTargetSelectionAccountsForOtherBotHealReservations()
{
    PartySnapshot snapshot;
    snapshot.groupId = 902;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 85;
    PartyMemberSnapshot healer, tank, ranged;
    ConfigureMember(healer, 11, PartyRole::Healer, 100, 100);
    healer.classId = 5;
    healer.isPlayerBot = true;
    healer.power = 100;
    healer.mapId = snapshot.mapId;
    healer.instanceId = snapshot.instanceId;
    healer.directHealSpells = {{2050, 100, 20}, {2052, 600, 55}};
    ConfigureMember(tank, 22, PartyRole::Tank, 100, 1000, true);
    tank.mapId = snapshot.mapId;
    tank.instanceId = snapshot.instanceId;
    ConfigureMember(ranged, 33, PartyRole::RangedDamage, 100, 500);
    ranged.mapId = snapshot.mapId;
    ranged.instanceId = snapshot.instanceId;
    snapshot.members = {healer, tank, ranged};

    PartyCoordinator coordinator;
    Reservation tankCovered;
    tankCovered.type = ReservationType::Heal;
    tankCovered.targetGuid = tank.guid;
    tankCovered.ownerGuid = 77;
    tankCovered.expiresAtMs = 5000;
    tankCovered.amount = 900;
    tankCovered.spellId = 2050;
    tankCovered.actionId = coordinator.NextActionId();
    assert(coordinator.TryReserve(tankCovered, 1000));

    PriestHealingTargetSelection selection;
    assert(SelectPriestHealingTarget(snapshot, healer.guid, selection));
    assert(selection.targetGuid == tank.guid && selection.missingHealth == 900);
    assert(SelectPriestHealingTarget(
        snapshot, healer.guid, coordinator, 1100, selection));
    assert(selection.targetGuid == ranged.guid && selection.missingHealth == 400);

    PriestHealingDecisionSelection decision;
    assert(SelectPriestHealingDecision(
        snapshot, healer.guid, coordinator, 1100, decision));
    assert(decision.IsValid() && decision.target.targetGuid == ranged.guid);
    assert(decision.target.missingHealth == 400 && decision.spell.spellId == 2052);

    Reservation rangedCovered = tankCovered;
    rangedCovered.targetGuid = ranged.guid;
    rangedCovered.amount = 400;
    rangedCovered.actionId = coordinator.NextActionId();
    assert(coordinator.TryReserve(rangedCovered, 1100));
    assert(!SelectPriestHealingTarget(
        snapshot, healer.guid, coordinator, 1200, selection));
    assert(!selection.IsValid());

    assert(coordinator.ReleaseReservation(
        ReservationType::Heal, ranged.guid, tankCovered.ownerGuid));
    rangedCovered.ownerGuid = healer.guid;
    rangedCovered.amount = 100;
    rangedCovered.actionId = coordinator.NextActionId();
    assert(coordinator.TryReserve(rangedCovered, 1200));
    assert(SelectPriestHealingTarget(
        snapshot, healer.guid, coordinator, 1300, selection));
    assert(selection.targetGuid == ranged.guid && selection.missingHealth == 400);
}

void TestPriestHealingTargetSelectionAccountsForIncomingHealsHotsAndAbsorbs()
{
    PartySnapshot snapshot;
    snapshot.groupId = 903;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 86;

    PartyMemberSnapshot healer, tank, ranged, humanCaster, engineCaster;
    ConfigureMember(healer, 11, PartyRole::Healer, 100, 100);
    healer.classId = 5;
    healer.isPlayerBot = true;
    healer.mapId = snapshot.mapId;
    healer.instanceId = snapshot.instanceId;

    ConfigureMember(tank, 22, PartyRole::Tank, 400, 1000, true);
    tank.mapId = snapshot.mapId;
    tank.instanceId = snapshot.instanceId;
    tank.periodicHealRemainingAmount = 180;
    tank.allSchoolAbsorbRemainingAmount = 120;

    ConfigureMember(ranged, 33, PartyRole::RangedDamage, 400, 500);
    ranged.mapId = snapshot.mapId;
    ranged.instanceId = snapshot.instanceId;

    // A human healer's non-Engine direct cast is visible through the caster's
    // target GUID and Core-derived amount. Engine casts are intentionally not
    // counted here because their shared reservation is authoritative.
    ConfigureMember(humanCaster, 44, PartyRole::Unknown, 100, 100);
    humanCaster.mapId = snapshot.mapId;
    humanCaster.instanceId = snapshot.instanceId;
    humanCaster.currentSpellTargetGuid = tank.guid;
    humanCaster.currentSpellHealingAmount = 200;

    ConfigureMember(engineCaster, 55, PartyRole::Healer, 100, 100);
    engineCaster.isPlayerBot = true;
    engineCaster.mapId = snapshot.mapId;
    engineCaster.instanceId = snapshot.instanceId;
    engineCaster.currentSpellTargetGuid = tank.guid;
    engineCaster.currentSpellHealingAmount = 900;
    engineCaster.currentSpellEngineAction = true;

    snapshot.members = {healer, tank, ranged, humanCaster, engineCaster};

    PartyCoordinator coordinator;
    Reservation engineReservation;
    engineReservation.type = ReservationType::Heal;
    engineReservation.targetGuid = tank.guid;
    engineReservation.ownerGuid = engineCaster.guid;
    engineReservation.expiresAtMs = 5000;
    engineReservation.amount = 100;
    engineReservation.spellId = 2050;
    engineReservation.actionId = coordinator.NextActionId();
    assert(coordinator.TryReserve(engineReservation, 1000));

    PriestHealingTargetSelection selection;
    // 200 pending cast + 180 remaining HoT + 120 all-school absorb +
    // 100 Engine reservation exactly cover the Tank's 600 health deficit.
    // The Engine cast's deliberately huge amount is not double counted.
    assert(SelectPriestHealingTarget(
        snapshot, healer.guid, coordinator, 1100, selection));
    assert(selection.targetGuid == ranged.guid);
    assert(selection.missingHealth == 100);

    // A Bot cast without its matching reservation is not credited twice:
    // the cast amount is ignored while currentSpellEngineAction is true.
    snapshot.members[1].periodicHealRemainingAmount = 0;
    snapshot.members[1].allSchoolAbsorbRemainingAmount = 0;
    snapshot.members[3].currentSpellHealingAmount = 0;
    assert(SelectPriestHealingTarget(
        snapshot, healer.guid, selection));
    assert(selection.targetGuid == tank.guid);
    assert(selection.missingHealth == 600);

    // Non-Engine concurrent casts are additive, but the aggregate is capped
    // at the target's current missing health and cannot wrap uint32 arithmetic.
    snapshot.members[3].currentSpellEngineAction = false;
    snapshot.members[3].currentSpellHealingAmount =
        std::numeric_limits<std::uint32_t>::max();
    snapshot.members[1].periodicHealRemainingAmount =
        std::numeric_limits<std::uint32_t>::max();
    snapshot.members[1].allSchoolAbsorbRemainingAmount =
        std::numeric_limits<std::uint32_t>::max();
    assert(SelectPriestHealingTarget(
        snapshot, healer.guid, selection));
    assert(selection.targetGuid == ranged.guid);
    assert(selection.missingHealth == 100);
}

void TestCoreRandomDitherBoundsMatchVanillaCore()
{
    std::uint32_t minimum = 99;
    std::uint32_t maximum = 99;
    assert(GetCoreRandomDitherBounds(9.25f, minimum, maximum));
    assert(minimum == 9 && maximum == 10);
    assert(GetCoreRandomDitherBounds(9.0f, minimum, maximum));
    assert(minimum == 9 && maximum == 9);
    assert(GetCoreRandomDitherBounds(0.25f, minimum, maximum));
    assert(minimum == 0 && maximum == 1);
    assert(!GetCoreRandomDitherBounds(0.0f, minimum, maximum));
    assert(minimum == 0 && maximum == 0);
    assert(!GetCoreRandomDitherBounds(-0.25f, minimum, maximum));
    assert(!GetCoreRandomDitherBounds(
        std::numeric_limits<float>::infinity(), minimum, maximum));
    assert(!GetCoreRandomDitherBounds(
        static_cast<float>(std::numeric_limits<std::int32_t>::max()),
        minimum, maximum));
}

void TestSchoolAwareAbsorbProjection()
{
    SchoolAbsorbForecastSnapshot fireShield;
    fireShield.spellId = 17;
    fireShield.schoolMask = 1u << 2; // Core SPELL_SCHOOL_MASK_FIRE
    fireShield.remainingAmount = 20;
    fireShield.remainingDurationMs = 1000;
    std::vector<SchoolAbsorbForecastSnapshot> shields(1, fireShield);

    HealingTimelineProjection projection;
    std::vector<HealingTimelineEvent> frostHit;
    assert(AppendDamageTimelineEvent(11, 133, 100, 15, frostHit, 1u << 4));
    assert(ProjectHealingTimeline(100, 100, frostHit, shields, false, projection));
    assert(projection.finalHealth == 85);
    assert(projection.appliedDamage == 15 && projection.absorbedDamage == 0);

    std::vector<HealingTimelineEvent> fireHit;
    assert(AppendDamageTimelineEvent(11, 133, 100, 15, fireHit, 1u << 2));
    assert(ProjectHealingTimeline(100, 100, fireHit, shields, false, projection));
    assert(projection.finalHealth == 100);
    assert(projection.appliedDamage == 0 && projection.absorbedDamage == 15);

    std::vector<HealingTimelineEvent> repeatedFire;
    assert(AppendDamageTimelineEvent(11, 133, 100, 15, repeatedFire, 1u << 2));
    assert(AppendDamageTimelineEvent(12, 133, 200, 15, repeatedFire, 1u << 2));
    assert(ProjectHealingTimeline(100, 100, repeatedFire, shields, false, projection));
    assert(projection.finalHealth == 90);
    assert(projection.absorbedDamage == 20 && projection.appliedDamage == 10);

    // School masks are bitsets: a Fire|Frost hit matches a Fire shield.
    std::vector<HealingTimelineEvent> combinedSchool;
    assert(AppendDamageTimelineEvent(
        11, 133, 100, 12, combinedSchool, (1u << 2) | (1u << 4)));
    assert(ProjectHealingTimeline(
        100, 100, combinedSchool, shields, false, projection));
    assert(projection.finalHealth == 100 && projection.absorbedDamage == 12);

    // A shield is unavailable at its exact expiration timestamp.
    std::vector<HealingTimelineEvent> expiredHit;
    assert(AppendDamageTimelineEvent(11, 133, 1000, 12, expiredHit, 1u << 2));
    assert(ProjectHealingTimeline(100, 100, expiredHit, shields, false, projection));
    assert(projection.finalHealth == 88 && projection.absorbedDamage == 0);

    // Unknown school cannot consume an active, school-specific shield safely.
    std::vector<HealingTimelineEvent> unknownHit;
    assert(AppendDamageTimelineEvent(11, 133, 100, 12, unknownHit));
    assert(!ProjectHealingTimeline(100, 100, unknownHit, shields, false, projection));
    assert(!projection.valid);

    // At one timestamp Core hit order is unknown. Different match sets veto.
    std::vector<HealingTimelineEvent> mixedSchoolHits;
    assert(AppendDamageTimelineEvent(11, 133, 100, 5, mixedSchoolHits, 1u << 2));
    assert(AppendDamageTimelineEvent(12, 133, 100, 5, mixedSchoolHits, 1u << 4));
    assert(!ProjectHealingTimeline(
        100, 100, mixedSchoolHits, shields, false, projection));
    assert(!projection.valid);

    // Same-time hits with identical school applicability may be aggregated.
    std::vector<HealingTimelineEvent> sameSchoolHits;
    assert(AppendDamageTimelineEvent(11, 133, 100, 15, sameSchoolHits, 1u << 2));
    assert(AppendDamageTimelineEvent(12, 133, 100, 15, sameSchoolHits, 1u << 2));
    assert(ProjectHealingTimeline(
        100, 100, sameSchoolHits, shields, false, projection));
    assert(projection.finalHealth == 90);
    assert(projection.absorbedDamage == 20 && projection.appliedDamage == 10);

    // A truncated snapshot never credits a partial shield set.
    assert(ProjectHealingTimeline(
        100, 100, fireHit, shields, true, projection));
    assert(projection.schoolAbsorbForecastIncomplete);
    assert(projection.absorbedDamage == 0 && projection.finalHealth == 85);

    SchoolAbsorbForecastSnapshot invalidShield = fireShield;
    invalidShield.schoolMask = 0x80;
    std::vector<SchoolAbsorbForecastSnapshot> malformed(1, invalidShield);
    assert(!ProjectHealingTimeline(
        100, 100, fireHit, malformed, false, projection));
}

void TestHealingTimelineProjectionOrdersHoTsAndCapsOverheal()
{
    PeriodicHealForecastSnapshot renew;
    renew.casterGuid = 11;
    renew.spellId = 139;
    renew.firstTickMs = 50;
    renew.periodMs = 100;
    renew.remainingTicks = 3;
    renew.healingPerTick = 15;

    std::vector<HealingTimelineEvent> events;
    assert(AppendPeriodicHealForecastEvents(renew, events));
    assert(events.size() == 3);
    assert(events[0].arrivalMs == 50);
    assert(events[1].arrivalMs == 150);
    assert(events[2].arrivalMs == 250);
    PartyMemberSnapshot directCaster;
    directCaster.guid = 22;
    directCaster.currentSpellId = 2052;
    directCaster.currentSpellState = 1; // Core snapshot says preparing/casting.
    directCaster.currentSpellTargetGuid = 99;
    directCaster.currentSpellHealingAmount = 20;
    directCaster.currentSpellRemainingCastMs = 100;
    assert(AppendInFlightDirectHealForecastEvent(directCaster, 99, events));
    assert(events.back().arrivalMs == 100);
    assert(events.back().sourceGuid == directCaster.guid);

    PeriodicHealForecastSnapshot fractionalRenew;
    fractionalRenew.casterGuid = 12;
    fractionalRenew.spellId = 139;
    fractionalRenew.firstTickMs = 25;
    fractionalRenew.periodMs = 100;
    fractionalRenew.remainingTicks = 3;
    fractionalRenew.healingPerTick = 9;
    fractionalRenew.healingPerTickUpperBound = 10;
    std::vector<HealingTimelineEvent> boundedHotEvents;
    assert(AppendPeriodicHealForecastEvents(fractionalRenew, boundedHotEvents));
    assert(boundedHotEvents.size() == 3);
    assert(boundedHotEvents[0].amount == 9 &&
           boundedHotEvents[0].maximumAmount == 10);
    assert(boundedHotEvents[1].maximumAmount == 10);
    assert(boundedHotEvents[2].maximumAmount == 10);

    HealingTimelineProjection projection;
    assert(ProjectHealingTimeline(50, 100, boundedHotEvents, projection));
    assert(projection.finalHealth == 77 && projection.maximumFinalHealth == 80);
    assert(projection.effectiveHealing == 27 &&
           projection.maximumEffectiveHealing == 30);
    assert(projection.overheal == 0 && projection.maximumOverheal == 0);

    PeriodicHealForecastSnapshot oneBoundedTick = fractionalRenew;
    oneBoundedTick.remainingTicks = 1;
    std::vector<HealingTimelineEvent> boundedOverheal;
    assert(AppendPeriodicHealForecastEvents(oneBoundedTick, boundedOverheal));
    assert(ProjectHealingTimeline(95, 100, boundedOverheal, projection));
    assert(projection.finalHealth == 100 && projection.maximumFinalHealth == 100);
    assert(projection.effectiveHealing == 5 &&
           projection.maximumEffectiveHealing == 5);
    assert(projection.overheal == 4 && projection.maximumOverheal == 5);

    PeriodicHealForecastSnapshot firstBoundedTick = fractionalRenew;
    firstBoundedTick.firstTickMs = 100;
    firstBoundedTick.remainingTicks = 1;
    PeriodicHealForecastSnapshot secondBoundedTick = fractionalRenew;
    secondBoundedTick.firstTickMs = 200;
    secondBoundedTick.remainingTicks = 1;
    std::vector<HealingTimelineEvent> boundedInterleaving;
    assert(AppendPeriodicHealForecastEvents(firstBoundedTick, boundedInterleaving));
    assert(AppendDamageTimelineEvent(55, 133, 150, 30, boundedInterleaving));
    assert(AppendPeriodicHealForecastEvents(secondBoundedTick, boundedInterleaving));
    assert(ProjectHealingTimeline(70, 100, boundedInterleaving, projection));
    assert(projection.finalHealth == 58 && projection.maximumFinalHealth == 60);
    assert(projection.effectiveHealing == 18 &&
           projection.maximumEffectiveHealing == 20);
    assert(projection.appliedDamage == 30 &&
           projection.maximumAppliedDamage == 30);

    assert(ProjectHealingTimeline(75, 100, events, projection));
    assert(projection.valid);
    assert(projection.eventCount == 4);
    assert(projection.finalHealth == 100);
    assert(projection.effectiveHealing == 25);
    assert(projection.overheal == 40);
    assert(!projection.concurrentAttributionUnknown);
    assert(projection.simultaneousArrivalGroups == 0);

    std::vector<HealingTimelineEvent> concurrent;
    assert(AppendDirectHealForecastEvent(33, 2050, 100, 30, concurrent));
    assert(AppendDirectHealForecastEvent(44, 2052, 100, 30, concurrent));
    assert(ProjectHealingTimeline(60, 100, concurrent, projection));
    assert(projection.finalHealth == 100);
    assert(projection.effectiveHealing == 40);
    assert(projection.overheal == 20);
    assert(projection.simultaneousArrivalGroups == 1);
    assert(projection.concurrentAttributionUnknown);

    // Explicit damage between heal arrivals changes the later heal's effective
    // amount. The projection must process distinct timestamps chronologically.
    std::vector<HealingTimelineEvent> interleaved;
    assert(AppendDirectHealForecastEvent(33, 2050, 100, 20, interleaved));
    assert(AppendDamageTimelineEvent(55, 0, 150, 30, interleaved));
    assert(AppendDirectHealForecastEvent(44, 2052, 200, 20, interleaved));
    assert(ProjectHealingTimeline(70, 100, interleaved, projection));
    assert(projection.finalHealth == 80);
    assert(projection.effectiveHealing == 40);
    assert(projection.overheal == 0);
    assert(projection.appliedDamage == 30);

    // Damage before the first heal creates a larger deficit, but the heal still
    // caps effective healing at the health missing when it arrives.
    std::vector<HealingTimelineEvent> damageBeforeHeal;
    assert(AppendDamageTimelineEvent(55, 133, 50, 40, damageBeforeHeal));
    assert(AppendDirectHealForecastEvent(44, 2052, 100, 50, damageBeforeHeal));
    assert(ProjectHealingTimeline(100, 100, damageBeforeHeal, projection));
    assert(projection.finalHealth == 100);
    assert(projection.appliedDamage == 40);
    assert(projection.effectiveHealing == 40);
    assert(projection.overheal == 10);

    // Heal and damage sharing a timestamp have no proven Core order; fail closed.
    std::vector<HealingTimelineEvent> ambiguousSameTime;
    assert(AppendDirectHealForecastEvent(33, 2050, 100, 15, ambiguousSameTime));
    assert(AppendDamageTimelineEvent(55, 133, 100, 15, ambiguousSameTime));
    assert(!ProjectHealingTimeline(50, 100, ambiguousSameTime, projection));
    assert(!projection.valid);

    // A normal heal must not be projected as a resurrection after lethal damage.
    std::vector<HealingTimelineEvent> lethalDamageThenHeal;
    assert(AppendDamageTimelineEvent(55, 0, 100, 20, lethalDamageThenHeal));
    assert(AppendDirectHealForecastEvent(44, 2052, 200, 20, lethalDamageThenHeal));
    assert(!ProjectHealingTimeline(20, 100, lethalDamageThenHeal, projection));
    assert(!projection.valid);

    std::size_t const eventCountBeforeInvalid = events.size();
    PeriodicHealForecastSnapshot invalid = renew;
    invalid.periodMs = 0;
    assert(!AppendPeriodicHealForecastEvents(invalid, events));
    invalid = fractionalRenew;
    invalid.healingPerTickUpperBound = 8;
    assert(!AppendPeriodicHealForecastEvents(invalid, events));
    invalid = fractionalRenew;
    invalid.healingPerTick = 0;
    invalid.healingPerTickUpperBound = 1;
    std::vector<HealingTimelineEvent> subOneEvents;
    assert(AppendPeriodicHealForecastEvents(invalid, subOneEvents));
    assert(subOneEvents.size() == 3);
    assert(subOneEvents[0].amount == 0 &&
           subOneEvents[0].maximumAmount == 1);
    assert(events.size() == eventCountBeforeInvalid);
    assert(!AppendDirectHealForecastEvent(0, 2052, 100, 20, events));
    directCaster.currentSpellEngineAction = true;
    std::size_t const beforeEngineCast = events.size();
    assert(!AppendInFlightDirectHealForecastEvent(directCaster, 99, events));
    assert(events.size() == beforeEngineCast);
    directCaster.currentSpellEngineAction = false;
    assert(!AppendInFlightDirectHealForecastEvent(directCaster, 100, events));
    directCaster.currentSpellRemainingCastMs = 0;
    assert(!AppendInFlightDirectHealForecastEvent(directCaster, 99, events));

    std::vector<HealingTimelineEvent> malformed(1);
    assert(!ProjectHealingTimeline(10, 100, malformed, projection));
    assert(!projection.valid);
    assert(!ProjectHealingTimeline(101, 100, concurrent, projection));
    assert(!projection.valid);
}

void TestHealingTriageShadowStrategy() {
    PartySnapshot snapshot; snapshot.epoch = 81;
    PartyMemberSnapshot healer, tank, damage;
    ConfigureMember(healer, 11, PartyRole::Healer, 100, 100);
    ConfigureMember(tank, 22, PartyRole::Tank, 40, 100, true);
    ConfigureMember(damage, 33, PartyRole::RangedDamage, 20, 100);
    snapshot.members = {healer, tank, damage};
    PartyCoordinator coordinator;
    DecisionContext context(snapshot, healer, coordinator, 1000);
    PartyBotEngine::PartyBotEngine engine;
    RegisterDefaultShadowStrategies(engine);

    DecisionReport report = engine.Evaluate(context);
    assert(report.candidates.size() == 2);
    assert(report.selectedIndices.empty());
    for (CandidateEvaluation const& candidate : report.candidates) {
        assert(candidate.candidate.actionName == "shadow_heal_triage");
        assert(candidate.status == CandidateStatus::RejectedThreatUnknown);
        assert(candidate.reason == "threat_impact_estimate_unknown");
    }
}
void TestTankPeelTargetShadowStrategy() {
    PartySnapshot snapshot; snapshot.epoch = 82;
    PartyMemberSnapshot tankBot, mainTank, healer;
    ConfigureMember(tankBot, 11, PartyRole::Tank, 100, 100);
    ConfigureMember(mainTank, 22, PartyRole::Unknown, 100, 100, true);
    ConfigureMember(healer, 33, PartyRole::Healer, 100, 100);
    snapshot.members = {tankBot, mainTank, healer};
    HostileSnapshot attackingHealer;
    attackingHealer.guid = 3001; attackingHealer.victimGuid = healer.guid; attackingHealer.alive = true;
    HostileSnapshot attackingMainTank;
    attackingMainTank.guid = 3002; attackingMainTank.victimGuid = mainTank.guid; attackingMainTank.alive = true;
    snapshot.hostiles = {attackingHealer, attackingMainTank};
    PartyCoordinator coordinator;
    DecisionContext context(snapshot, tankBot, coordinator, 1000);
    PartyBotEngine::PartyBotEngine engine;
    RegisterDefaultShadowStrategies(engine);

    DecisionReport report = engine.Evaluate(context);
    assert(report.candidates.size() == 1);
    assert(report.selectedIndices.empty());
    assert(report.candidates[0].candidate.strategyName == "tank_target");
    assert(report.candidates[0].candidate.targetGuid == attackingHealer.guid);
    assert(report.candidates[0].status == CandidateStatus::RejectedThreatUnknown);
}
void TestDpsAssistTargetShadowStrategy() {
    PartySnapshot snapshot; snapshot.epoch = 83;
    PartyMemberSnapshot dpsBot, mainTank, healer;
    ConfigureMember(dpsBot, 11, PartyRole::MeleeDamage, 100, 100);
    ConfigureMember(mainTank, 22, PartyRole::Tank, 100, 100, true);
    ConfigureMember(healer, 33, PartyRole::Healer, 100, 100);
    mainTank.victimGuid = 4002;
    snapshot.members = {dpsBot, mainTank, healer};
    HostileSnapshot attackingHealer;
    attackingHealer.guid = 4001; attackingHealer.victimGuid = healer.guid; attackingHealer.alive = true;
    HostileSnapshot mainTankTarget;
    mainTankTarget.guid = 4002; mainTankTarget.alive = true;
    snapshot.hostiles = {attackingHealer, mainTankTarget};
    PartyCoordinator coordinator;
    DecisionContext context(snapshot, dpsBot, coordinator, 1000);
    PartyBotEngine::PartyBotEngine engine;
    RegisterDefaultShadowStrategies(engine);

    DecisionReport report = engine.Evaluate(context);
    assert(report.candidates.size() == 2);
    assert(report.selectedIndices.empty());
    for (CandidateEvaluation const& candidate : report.candidates) {
        assert(candidate.candidate.strategyName == "dps_target");
        assert(candidate.status == CandidateStatus::RejectedThreatUnknown);
    }
}


ActionCandidate const* FindDpsAssistCandidate(
    std::vector<ActionCandidate> const& candidates, Guid targetGuid)
{
    for (ActionCandidate const& candidate : candidates)
        if (candidate.targetGuid == targetGuid)
            return &candidate;
    return nullptr;
}

void TestDpsAssistPolicyComposition()
{
    DpsAssistTrigger trigger;
    assert(trigger.GetName() == "dps_assist");

    // A DPS bot must follow the Main Tank's target even while its current
    // victim is a different hostile. Healer peel remains a lower-priority
    // alternative, and crowd-control-protected targets are never proposed.
    PartySnapshot snapshot;
    snapshot.epoch = 84;
    PartyMemberSnapshot dpsBot, mainTank, healer;
    ConfigureMember(dpsBot, 11, PartyRole::RangedDamage, 100, 100);
    ConfigureMember(mainTank, 22, PartyRole::Tank, 100, 100, true);
    ConfigureMember(healer, 33, PartyRole::Healer, 100, 100);
    mainTank.victimGuid = 4002;
    dpsBot.victimGuid = 4003;
    dpsBot.selectionGuid = 4003;
    snapshot.members = {dpsBot, mainTank, healer};

    HostileSnapshot healerPeel;
    healerPeel.guid = 4001;
    healerPeel.victimGuid = healer.guid;
    healerPeel.alive = true;
    HostileSnapshot mainTankTarget;
    mainTankTarget.guid = 4002;
    mainTankTarget.alive = true;
    HostileSnapshot currentBotVictim;
    currentBotVictim.guid = 4003;
    currentBotVictim.victimGuid = dpsBot.guid;
    currentBotVictim.alive = true;
    HostileSnapshot crowdControlTarget;
    crowdControlTarget.guid = 4004;
    crowdControlTarget.alive = true;
    crowdControlTarget.crowdControlProtected = true;
    snapshot.hostiles = {
        healerPeel, mainTankTarget, currentBotVictim, crowdControlTarget
    };

    PartyCoordinator coordinator;
    DecisionContext context(snapshot, dpsBot, coordinator, 1000);
    assert(trigger.IsActive(context));

    std::vector<ActionCandidate> candidates;
    trigger.CollectCandidates(context, candidates);
    assert(candidates.size() == 2);
    ActionCandidate const* mainTankCandidate =
        FindDpsAssistCandidate(candidates, mainTankTarget.guid);
    ActionCandidate const* healerCandidate =
        FindDpsAssistCandidate(candidates, healerPeel.guid);
    assert(mainTankCandidate && healerCandidate);
    assert(mainTankCandidate->actionName == "core_select_hostile_target");
    assert(healerCandidate->actionName == "core_select_hostile_target");
    assert(mainTankCandidate->channel == ActionChannel::Targeting);
    assert(mainTankCandidate->basePriority == 300.0f);
    assert(healerCandidate->basePriority == 260.0f);
    assert(mainTankCandidate->basePriority > healerCandidate->basePriority);
    assert(!FindDpsAssistCandidate(candidates, currentBotVictim.guid));
    assert(!FindDpsAssistCandidate(candidates, crowdControlTarget.guid));

    // Once the Main Tank target is selected, the policy advances to the
    // existing auto-attack action. It still proposes the switch while the
    // bot's old victim differs from the Main Tank target.
    dpsBot.selectionGuid = mainTankTarget.guid;
    candidates.clear();
    trigger.CollectCandidates(context, candidates);
    mainTankCandidate = FindDpsAssistCandidate(candidates, mainTankTarget.guid);
    assert(mainTankCandidate);
    assert(mainTankCandidate->actionName == "core_attack_target");

    // A non-Main-Tank tank assist still outranks an emergency healer peel.
    PartySnapshot roleSnapshot;
    roleSnapshot.epoch = 85;
    PartyMemberSnapshot roleBot, offTank, roleHealer;
    ConfigureMember(roleBot, 101, PartyRole::MeleeDamage, 100, 100);
    ConfigureMember(offTank, 102, PartyRole::Tank, 100, 100, false);
    ConfigureMember(roleHealer, 103, PartyRole::Healer, 100, 100);
    offTank.victimGuid = 5001;
    roleSnapshot.members = {roleBot, offTank, roleHealer};
    HostileSnapshot offTankTarget;
    offTankTarget.guid = 5001;
    offTankTarget.alive = true;
    HostileSnapshot roleHealerPeel;
    roleHealerPeel.guid = 5002;
    roleHealerPeel.victimGuid = roleHealer.guid;
    roleHealerPeel.alive = true;
    roleSnapshot.hostiles = {offTankTarget, roleHealerPeel};
    DecisionContext roleContext(roleSnapshot, roleBot, coordinator, 2000);
    candidates.clear();
    trigger.CollectCandidates(roleContext, candidates);
    ActionCandidate const* offTankCandidate =
        FindDpsAssistCandidate(candidates, offTankTarget.guid);
    ActionCandidate const* rolePeelCandidate =
        FindDpsAssistCandidate(candidates, roleHealerPeel.guid);
    assert(offTankCandidate && rolePeelCandidate);
    assert(offTankCandidate->basePriority == 280.0f);
    assert(rolePeelCandidate->basePriority == 260.0f);
    assert(offTankCandidate->basePriority > rolePeelCandidate->basePriority);

    // If the hostile's live victim is outside the party, use only accessible,
    // online party threat references to identify a peel target.
    PartySnapshot fallbackSnapshot;
    fallbackSnapshot.epoch = 86;
    PartyMemberSnapshot fallbackBot, fallbackHealer, fallbackTank;
    ConfigureMember(fallbackBot, 201, PartyRole::RangedDamage, 100, 100);
    ConfigureMember(fallbackHealer, 202, PartyRole::Healer, 100, 100);
    ConfigureMember(fallbackTank, 203, PartyRole::Tank, 100, 100);
    fallbackSnapshot.members = {fallbackBot, fallbackHealer, fallbackTank};
    HostileSnapshot fallbackHostile;
    fallbackHostile.guid = 6001;
    fallbackHostile.victimGuid = 9999;
    fallbackHostile.alive = true;
    ThreatEntrySnapshot inaccessibleTankThreat;
    inaccessibleTankThreat.unitGuid = fallbackTank.guid;
    inaccessibleTankThreat.threat = 900.0f;
    inaccessibleTankThreat.online = true;
    inaccessibleTankThreat.accessible = false;
    ThreatEntrySnapshot offlineTankThreat;
    offlineTankThreat.unitGuid = fallbackTank.guid;
    offlineTankThreat.threat = 800.0f;
    offlineTankThreat.online = false;
    offlineTankThreat.accessible = true;
    ThreatEntrySnapshot healerThreat;
    healerThreat.unitGuid = fallbackHealer.guid;
    healerThreat.threat = 5.0f;
    healerThreat.online = true;
    healerThreat.accessible = true;
    fallbackHostile.threat = {
        inaccessibleTankThreat, offlineTankThreat, healerThreat
    };
    fallbackSnapshot.hostiles = {fallbackHostile};
    DecisionContext fallbackContext(
        fallbackSnapshot, fallbackBot, coordinator, 3000);
    candidates.clear();
    trigger.CollectCandidates(fallbackContext, candidates);
    ActionCandidate const* fallbackCandidate =
        FindDpsAssistCandidate(candidates, fallbackHostile.guid);
    assert(fallbackCandidate);
    assert(fallbackCandidate->basePriority == 260.0f);

    // Dead/offline assist members, unassigned hostiles, non-DPS bots and
    // unavailable DPS bots must not generate actionable candidates.
    PartySnapshot inactiveSnapshot;
    inactiveSnapshot.epoch = 87;
    PartyMemberSnapshot inactiveBot, deadMainTank;
    ConfigureMember(inactiveBot, 301, PartyRole::RangedDamage, 100, 100);
    ConfigureMember(deadMainTank, 302, PartyRole::MainTank, 100, 100, true);
    deadMainTank.alive = false;
    deadMainTank.victimGuid = 7001;
    inactiveSnapshot.members = {inactiveBot, deadMainTank};
    HostileSnapshot unassignedHostile;
    unassignedHostile.guid = 7001;
    unassignedHostile.alive = true;
    HostileSnapshot crowdControlOnly;
    crowdControlOnly.guid = 7002;
    crowdControlOnly.alive = true;
    crowdControlOnly.crowdControlProtected = true;
    inactiveSnapshot.hostiles = {unassignedHostile, crowdControlOnly};
    DecisionContext inactiveContext(
        inactiveSnapshot, inactiveBot, coordinator, 4000);
    candidates.clear();
    trigger.CollectCandidates(inactiveContext, candidates);
    assert(candidates.empty());

    inactiveBot.roleId = static_cast<std::uint8_t>(PartyRole::Healer);
    DecisionContext nonDpsContext(
        inactiveSnapshot, inactiveBot, coordinator, 4000);
    assert(!trigger.IsActive(nonDpsContext));
    inactiveBot.roleId = static_cast<std::uint8_t>(PartyRole::RangedDamage);
    inactiveBot.online = false;
    assert(!trigger.IsActive(nonDpsContext));
    inactiveBot.online = true;
    inactiveBot.alive = false;
    assert(!trigger.IsActive(nonDpsContext));
}

void ConfigureThreat(HostileSnapshot& hostile, Guid victimGuid, float victimThreat,
                     Guid memberGuid, float memberThreat, bool meleeReachable) {
    hostile.guid = 9001;
    hostile.victimGuid = victimGuid;
    hostile.alive = true;
    ThreatEntrySnapshot victim;
    victim.unitGuid = victimGuid;
    victim.threat = victimThreat;
    victim.online = true;
    victim.accessible = true;
    ThreatEntrySnapshot member;
    member.unitGuid = memberGuid;
    member.threat = memberThreat;
    member.online = true;
    member.accessible = true;
    member.meleeReachable = meleeReachable;
    hostile.threat = {victim, member};
}
void TestActionThreatImpactContractDefaultsUnknownAndValidatesCompleteness() {
    Fixture f;
    TestAction action("damage");
    action.returnDefaultUnknown = true;
    ActionCandidate candidate = Candidate("damage", 10.0f, ActionChannel::GlobalCooldown);

    ThreatImpactEstimate unknown = action.EstimateThreatImpact(f.context, candidate);
    assert(unknown.completeness == ThreatImpactCompleteness::Unknown);
    assert(unknown.snapshotEpoch == f.snapshot.epoch);
    assert(unknown.reason == "action_threat_impact_not_implemented");
    assert(IsWellFormedThreatImpactEstimate(unknown));
    assert(IsThreatImpactEstimateForSnapshot(unknown, f.snapshot.epoch));
    assert(!IsThreatImpactEstimateForSnapshot(unknown, f.snapshot.epoch + 1));
    assert(!IsThreatImpactEstimateForSnapshot(unknown, 0));
    ThreatImpactEstimate noThreat;
    noThreat.completeness = ThreatImpactCompleteness::NoThreat;
    noThreat.snapshotEpoch = f.snapshot.epoch;
    assert(IsWellFormedThreatImpactEstimate(noThreat));
    assert(IsThreatImpactEstimateForSnapshot(noThreat, f.snapshot.epoch));
    noThreat.impacts.push_back(ThreatImpactEntry());
    assert(!IsWellFormedThreatImpactEstimate(noThreat));

    ThreatImpactEstimate complete;
    complete.completeness = ThreatImpactCompleteness::Complete;
    complete.snapshotEpoch = f.snapshot.epoch;
    assert(!IsWellFormedThreatImpactEstimate(complete));
    complete.reservationDurationMs = 1000;
    ThreatImpactEntry impact;
    impact.threatOwnerGuid = f.bot.guid;
    impact.hostileGuid = 9001;
    impact.threatPoints = 12.5f;
    impact.source = ThreatImpactSource::DirectHealAssist;
    impact.bound = ThreatImpactBound::UpperBound;
    complete.impacts.push_back(impact);
    assert(IsWellFormedThreatImpactEstimate(complete));
    assert(IsThreatImpactEstimateForSnapshot(complete, f.snapshot.epoch));
    complete.reservationDurationMs = 0;
    assert(!IsWellFormedThreatImpactEstimate(complete));
    complete.reservationDurationMs = 0x80000000u;
    assert(!IsWellFormedThreatImpactEstimate(complete));
    complete.reservationDurationMs = 1000;

    complete.impacts[0].source = ThreatImpactSource::PeriodicDamage;
    assert(IsWellFormedThreatImpactEstimate(complete));
    complete.impacts[0].source = ThreatImpactSource::PeriodicPowerAssist;
    assert(IsWellFormedThreatImpactEstimate(complete));
    complete.impacts[0].source = ThreatImpactSource::PeriodicManaLeech;
    assert(IsWellFormedThreatImpactEstimate(complete));
    complete.impacts[0].source = ThreatImpactSource::FlatSpellThreat;
    assert(IsWellFormedThreatImpactEstimate(complete));

    complete.impacts[0].threatPoints = std::nanf("");
    assert(!IsWellFormedThreatImpactEstimate(complete));
    complete.impacts[0].threatPoints = 12.5f;
    complete.impacts[0].source = static_cast<ThreatImpactSource>(255);
    assert(!IsWellFormedThreatImpactEstimate(complete));
    complete.impacts[0].source = ThreatImpactSource::DirectHealAssist;
    complete.impacts[0].bound = static_cast<ThreatImpactBound>(255);
    assert(!IsWellFormedThreatImpactEstimate(complete));

    ThreatImpactEstimate missingReason = unknown;
    missingReason.reason.clear();
    assert(!IsWellFormedThreatImpactEstimate(missingReason));
    ThreatImpactEstimate missingEpoch = unknown;
    missingEpoch.snapshotEpoch = 0;
    assert(!IsWellFormedThreatImpactEstimate(missingEpoch));
}

ThreatImpactEstimate MakeThreatEstimate(std::uint32_t epoch, Guid threatOwnerGuid,
                                             Guid hostileGuid, float threatPoints,
                                             std::uint32_t reservationDurationMs = 1000) {
    ThreatImpactEstimate estimate;
    estimate.completeness = ThreatImpactCompleteness::Complete;
    estimate.snapshotEpoch = epoch;
    estimate.reservationDurationMs = reservationDurationMs;
    estimate.reason = "test_complete_threat_estimate";
    ThreatImpactEntry impact;
    impact.threatOwnerGuid = threatOwnerGuid;
    impact.hostileGuid = hostileGuid;
    impact.threatPoints = threatPoints;
    impact.source = ThreatImpactSource::DirectDamage;
    impact.bound = ThreatImpactBound::UpperBound;
    estimate.impacts.push_back(impact);
    return estimate;
}

void TestThreatImpactBudgetUsesFanoutAndReservations() {
    PartySnapshot snapshot;
    snapshot.epoch = 90;
    snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank, dps;
    ConfigureMember(tank, 101, PartyRole::Tank, 100, 100, true);
    ConfigureMember(dps, 202, PartyRole::RangedDamage, 100, 100);
    snapshot.members = {tank, dps};

    HostileSnapshot first, second;
    ConfigureThreat(first, tank.guid, 100.0f, dps.guid, 90.0f, true);
    first.guid = 9001;
    ConfigureThreat(second, tank.guid, 100.0f, dps.guid, 95.0f, false);
    second.guid = 9002;
    snapshot.hostiles = {first, second};

    PartyCoordinator coordinator;
    ThreatImpactEstimate fanout;
    fanout.completeness = ThreatImpactCompleteness::Complete;
    fanout.snapshotEpoch = snapshot.epoch;
    fanout.reservationDurationMs = 1000;
    fanout.reason = "multi_hostile_fanout";
    ThreatImpactEntry firstPart;
    firstPart.threatOwnerGuid = dps.guid;
    firstPart.hostileGuid = first.guid;
    firstPart.threatPoints = 9.0f;
    ThreatImpactEntry secondPart = firstPart;
    secondPart.threatPoints = 8.0f;
    ThreatImpactEntry secondHostile = firstPart;
    secondHostile.hostileGuid = second.guid;
    secondHostile.threatPoints = 20.0f;
    fanout.impacts = {firstPart, secondPart, secondHostile};

    ThreatImpactTotals localSelected;
    ThreatImpactBudgetResult safe = AssessThreatImpactBudget(
        snapshot, fanout, coordinator, 1000, 250, &localSelected);
    assert(safe.status == ThreatImpactBudgetStatus::Safe);
    assert(std::fabs(localSelected[ThreatImpactKey(first.guid, dps.guid)] - 17.0) < 0.0001);
    assert(std::fabs(localSelected[ThreatImpactKey(second.guid, dps.guid)] - 20.0) < 0.0001);

    ThreatImpactEstimate competing = MakeThreatEstimate(snapshot.epoch, dps.guid, first.guid, 4.0f);
    ThreatImpactBudgetResult localConflict = AssessThreatImpactBudget(
        snapshot, competing, coordinator, 1000, 250, &localSelected);
    assert(localConflict.status == ThreatImpactBudgetStatus::Exceeded);
    assert(localConflict.reason == "projected_threat_exceeds_core_headroom");

    ThreatReservation inFlight;
    inFlight.ownerGuid = 303;
    inFlight.threatOwnerGuid = dps.guid;
    inFlight.hostileGuid = first.guid;
    inFlight.actionId = coordinator.NextThreatActionId();
    inFlight.threatPoints = 3.0f;
    inFlight.expiresAtMs = 1500;
    assert(coordinator.TryReserveThreatImpact(inFlight, 1000));
    ThreatImpactEstimate overReserved = MakeThreatEstimate(snapshot.epoch, dps.guid, first.guid, 18.0f);
    ThreatImpactBudgetResult reservationConflict = AssessThreatImpactBudget(
        snapshot, overReserved, coordinator, 1000, 250, nullptr);
    assert(reservationConflict.status == ThreatImpactBudgetStatus::Exceeded);
    assert(std::fabs(reservationConflict.alreadyReserved - 3.0f) < 0.0001f);
    assert(std::fabs(reservationConflict.projectedThreat - 21.0) < 0.0001);
}

void TestThreatImpactBudgetUnknownAndCurrentVictimCases() {
    PartySnapshot snapshot;
    snapshot.epoch = 91;
    snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank, dps;
    ConfigureMember(tank, 101, PartyRole::Tank, 100, 100, true);
    ConfigureMember(dps, 202, PartyRole::RangedDamage, 100, 100);
    snapshot.members = {tank, dps};
    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, dps.guid, 90.0f, true);
    snapshot.hostiles = {hostile};
    PartyCoordinator coordinator;

    ThreatImpactEstimate unknown;
    unknown.snapshotEpoch = snapshot.epoch;
    unknown.reason = "periodic_and_flat_threat_not_enumerated";
    ThreatImpactBudgetResult unknownResult = AssessThreatImpactBudget(
        snapshot, unknown, coordinator, 1000, 250, nullptr);
    assert(unknownResult.status == ThreatImpactBudgetStatus::Unknown);
    assert(unknownResult.reason == "threat_impact_estimate_unknown");

    ThreatImpactEstimate petImpact = MakeThreatEstimate(snapshot.epoch, 999, hostile.guid, 5.0f);
    ThreatImpactBudgetResult missingPetSnapshot = AssessThreatImpactBudget(
        snapshot, petImpact, coordinator, 1000, 250, nullptr);
    assert(missingPetSnapshot.status == ThreatImpactBudgetStatus::Unknown);
    assert(missingPetSnapshot.reason == "threat_budget_snapshot_unknown");

    // Pet GUID remains the actual threat owner. The Player owner is an
    // identity/cleanup relationship, not a replacement threat recipient.
    PartyCompanionSnapshot pet;
    pet.guid = 999;
    pet.ownerGuid = dps.guid;
    pet.online = true;
    pet.alive = true;
    pet.mapId = snapshot.mapId;
    pet.instanceId = snapshot.instanceId;
    snapshot.companions.push_back(pet);
    ThreatEntrySnapshot petThreat;
    petThreat.unitGuid = pet.guid;
    petThreat.threat = 30.0f;
    petThreat.online = true;
    petThreat.accessible = true;
    petThreat.meleeReachable = false;
    snapshot.hostiles[0].threat.push_back(petThreat);

    ThreatBudgetAssessment petBudget =
        AssessThreatBudget(snapshot, hostile.guid, pet.guid, 1000, 250);
    assert(petBudget.status == ThreatBudgetStatus::BudgetAvailable);
    assert(std::fabs(petBudget.thresholdMultiplier - 1.3f) < 0.0001f);
    assert(std::fabs(petBudget.memberThreat - 30.0f) < 0.0001f);
    assert(std::fabs(petBudget.rawHeadroom - 100.0f) < 0.0001f);

    ThreatImpactBudgetResult petImpactSafe = AssessThreatImpactBudget(
        snapshot, petImpact, coordinator, 1000, 250, nullptr);
    assert(petImpactSafe.status == ThreatImpactBudgetStatus::Safe);
    assert(petImpactSafe.threatOwnerGuid == pet.guid);

    PartySnapshot duplicatePet = snapshot;
    duplicatePet.companions.push_back(pet);
    assert(AssessThreatBudget(duplicatePet, hostile.guid, pet.guid, 1000, 250)
        .unknownReason == ThreatBudgetUnknownReason::MissingOrDuplicateCompanion);

    PartySnapshot missingPetOwner = snapshot;
    missingPetOwner.members = {tank};
    assert(AssessThreatBudget(missingPetOwner, hostile.guid, pet.guid, 1000, 250)
        .unknownReason == ThreatBudgetUnknownReason::MissingOrDuplicateCompanionOwner);

    PartySnapshot offlinePet = snapshot;
    offlinePet.companions[0].online = false;
    assert(AssessThreatBudget(offlinePet, hostile.guid, pet.guid, 1000, 250)
        .unknownReason == ThreatBudgetUnknownReason::CompanionNotOnlineOrAlive);

    PartySnapshot petVictimSnapshot = snapshot;
    petVictimSnapshot.hostiles[0].victimGuid = pet.guid;
    assert(AssessThreatBudget(petVictimSnapshot, hostile.guid, pet.guid, 1000, 250)
        .status == ThreatBudgetStatus::CurrentVictim);

    ThreatImpactEstimate tankImpact = MakeThreatEstimate(snapshot.epoch, tank.guid, hostile.guid, 500.0f);
    ThreatImpactBudgetResult currentVictim = AssessThreatImpactBudget(
        snapshot, tankImpact, coordinator, 1000, 250, nullptr);
    assert(currentVictim.status == ThreatImpactBudgetStatus::Safe);
    assert(currentVictim.reason == "projected_threat_within_core_headroom");

    ThreatImpactEstimate noThreat;
    noThreat.completeness = ThreatImpactCompleteness::NoThreat;
    noThreat.snapshotEpoch = snapshot.epoch;
    noThreat.reason = "movement_action_has_no_threat";
    assert(AssessThreatImpactBudget(snapshot, noThreat, coordinator, 1000, 250, nullptr).status ==
           ThreatImpactBudgetStatus::Safe);

    ThreatImpactEstimate stale = MakeThreatEstimate(snapshot.epoch + 1, dps.guid, hostile.guid, 5.0f);
    assert(AssessThreatImpactBudget(snapshot, stale, coordinator, 1000, 250, nullptr).status ==
           ThreatImpactBudgetStatus::Unknown);
}

void PrepareEngineThreatFixture(Fixture& f) {
    f.snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank;
    ConfigureMember(tank, 101, PartyRole::Tank, 100, 100, true);
    ConfigureMember(f.bot, 202, PartyRole::RangedDamage, 100, 100);
    f.snapshot.members = {tank, f.bot};
    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, f.bot.guid, 90.0f, true);
    f.snapshot.hostiles = {hostile};
}

void TestEngineThreatBudgetPetRecipientReservation() {
    Fixture f;
    f.snapshot.capturedAtMs = 1000;
    f.bot.guid = 11;
    f.bot.online = true;
    f.bot.alive = true;

    PartyMemberSnapshot tank;
    ConfigureMember(tank, 22, PartyRole::Tank, 100, 100, true);
    f.snapshot.members = {f.bot, tank};

    PartyCompanionSnapshot pet;
    pet.guid = 33;
    pet.ownerGuid = f.bot.guid;
    pet.online = true;
    pet.alive = true;
    pet.mapId = f.snapshot.mapId;
    pet.instanceId = f.snapshot.instanceId;
    f.snapshot.companions = {pet};

    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, pet.guid, 30.0f, false);
    f.snapshot.hostiles = {hostile};

    auto action = std::make_shared<TestAction>("pet_damage");
    action->useThreatImpact = true;
    action->threatImpact = MakeThreatEstimate(
        f.snapshot.epoch, pet.guid, hostile.guid, 10.0f);
    PartyBotEngine::PartyBotEngine engine;
    assert(engine.RegisterAction(action));
    auto trigger = std::make_shared<TestTrigger>("pet_damage_trigger");
    trigger->generated.push_back(Candidate("pet_damage", 10.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("pet_damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    assert(report.candidates[0].status == CandidateStatus::Selected);
    engine.ExecuteSelected(f.context, report);
    assert(report.candidates[0].status == CandidateStatus::Executed);
    assert(std::fabs(f.coordinator.GetReservedThreatImpact(
        hostile.guid, pet.guid, 0, 1000) - 10.0f) < 0.0001f);
    assert(f.coordinator.GetReservedThreatImpact(
        hostile.guid, f.bot.guid, 0, 1000) == 0.0f);

    f.coordinator.ReleaseOwner(f.bot.guid);
    assert(f.coordinator.GetReservedThreatImpact(
        hostile.guid, pet.guid, 0, 1000) == 0.0f);
}

void TestEngineThreatBudgetSelectionAndExecutionReservations() {
    Fixture f;
    PrepareEngineThreatFixture(f);
    CaptureReservationTraceSink traceSink;
    f.context.reservationTrace = &traceSink;
    PartyBotEngine::PartyBotEngine engine;
    std::shared_ptr<TestAction> action = std::make_shared<TestAction>("damage");
    action->useThreatImpact = true;
    action->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 15.0f);
    assert(engine.RegisterAction(action));
    auto trigger = std::make_shared<TestTrigger>("threat_action");
    trigger->generated.push_back(Candidate("damage", 10.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    CandidateEvaluation& selected = report.candidates[report.selectedIndices.front()];
    assert(selected.status == CandidateStatus::Selected);
    engine.ExecuteSelected(f.context, report);
    assert(action->executeCount == 1);
    assert(selected.status == CandidateStatus::Executed);
    assert(selected.threatActionId != 0);
    assert(action->lastThreatActionId == selected.threatActionId);
    assert(selected.candidate.threatActionId == selected.threatActionId);
    assert(std::fabs(f.coordinator.GetReservedThreatImpact(9001, f.bot.guid, 0, 1000) - 15.0f) < 0.0001f);
    assert(traceSink.events.size() == 2);
    assert(traceSink.events[0].stage == ReservationTraceStage::Attempt);
    assert(traceSink.events[0].actionId == selected.threatActionId);
    assert(traceSink.events[0].ownerGuid == f.bot.guid);
    assert(traceSink.events[0].threatOwnerGuid == f.bot.guid);
    assert(traceSink.events[0].hostileGuid == 9001);
    assert(std::fabs(traceSink.events[0].threatPoints - 15.0f) < 0.0001f);
    assert(traceSink.events[1].stage == ReservationTraceStage::Result);
    assert(traceSink.events[1].accepted);
    assert(traceSink.events[1].reason == std::string("coordinator_accepted"));

    f.coordinator.ReleaseThreatAction(selected.threatActionId);
    assert(f.coordinator.GetReservedThreatImpact(9001, f.bot.guid, 0, 1000) == 0.0f);
}

void TestEngineThreatBudgetRejectsUnknownAndOverBudgetCandidates() {
    Fixture f;
    PrepareEngineThreatFixture(f);
    PartyBotEngine::PartyBotEngine engine;
    std::shared_ptr<TestAction> unknown = std::make_shared<TestAction>("unknown");
    unknown->useThreatImpact = true;
    unknown->threatImpact.snapshotEpoch = f.snapshot.epoch;
    unknown->threatImpact.reason = "action_has_unmodelled_threat_sources";
    std::shared_ptr<TestAction> overBudget = std::make_shared<TestAction>("over_budget");
    overBudget->useThreatImpact = true;
    overBudget->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 15.0f);
    assert(engine.RegisterAction(unknown));
    assert(engine.RegisterAction(overBudget));

    ThreatReservation existing;
    existing.ownerGuid = 303;
    existing.threatOwnerGuid = f.bot.guid;
    existing.hostileGuid = 9001;
    existing.actionId = f.coordinator.NextThreatActionId();
    existing.threatPoints = 6.0f;
    existing.expiresAtMs = 1500;
    assert(f.coordinator.TryReserveThreatImpact(existing, 1000));

    auto trigger = std::make_shared<TestTrigger>("unsafe_threat_actions");
    trigger->generated.push_back(Candidate("unknown", 20.0f, ActionChannel::Utility));
    trigger->generated.push_back(Candidate("over_budget", 10.0f, ActionChannel::Movement));
    auto strategy = std::make_shared<Strategy>("damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.empty());
    assert(report.candidates[0].status == CandidateStatus::RejectedThreatUnknown);
    assert(report.candidates[1].status == CandidateStatus::RejectedThreatBudget);
    assert(unknown->executeCount == 0 && overBudget->executeCount == 0);
}

void TestEngineReleasesThreatReservationWhenExecutionFails() {
    Fixture f;
    PrepareEngineThreatFixture(f);
    CaptureReservationTraceSink traceSink;
    f.context.reservationTrace = &traceSink;
    PartyBotEngine::PartyBotEngine engine;
    std::shared_ptr<TestAction> action = std::make_shared<TestAction>("failing_damage");
    action->useThreatImpact = true;
    action->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 15.0f);
    action->result = ActionResult::Failed;
    assert(engine.RegisterAction(action));
    auto trigger = std::make_shared<TestTrigger>("failing_threat_action");
    trigger->generated.push_back(Candidate("failing_damage", 10.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    engine.ExecuteSelected(f.context, report);
    CandidateEvaluation const& evaluation = report.candidates[report.selectedIndices.front()];
    assert(evaluation.status == CandidateStatus::ExecutionFailed);
    assert(action->executeCount == 1);
    assert(f.coordinator.GetThreatReservationCount() == 0);
    assert(f.coordinator.GetReservedThreatImpact(9001, f.bot.guid, 0, 1000) == 0.0f);
    assert(traceSink.events.size() == 3);
    assert(traceSink.events[0].stage == ReservationTraceStage::Attempt);
    assert(traceSink.events[1].stage == ReservationTraceStage::Result);
    assert(traceSink.events[1].accepted);
    assert(traceSink.events[2].stage == ReservationTraceStage::Release);
    assert(traceSink.events[2].actionId == traceSink.events[0].actionId);
    assert(traceSink.events[2].reservationRows == 1);
    assert(traceSink.events[2].reason == std::string("action_execution_failed"));
}

void TestEngineRevalidatesReservationsImmediatelyBeforeExecution() {
    Fixture f;
    PrepareEngineThreatFixture(f);
    CaptureReservationTraceSink traceSink;
    f.context.reservationTrace = &traceSink;
    PartyBotEngine::PartyBotEngine engine;
    std::shared_ptr<TestAction> action = std::make_shared<TestAction>("revalidated_damage");
    action->useThreatImpact = true;
    action->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 15.0f);
    assert(engine.RegisterAction(action));

    auto trigger = std::make_shared<TestTrigger>("revalidate_before_execute");
    trigger->generated.push_back(Candidate("revalidated_damage", 10.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    assert(report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected);

    // Another bot reserves six points after our decision but before dispatch.
    ThreatReservation racing;
    racing.ownerGuid = 303;
    racing.threatOwnerGuid = f.bot.guid;
    racing.hostileGuid = 9001;
    racing.actionId = f.coordinator.NextThreatActionId();
    racing.threatPoints = 6.0f;
    racing.expiresAtMs = 1500;
    assert(f.coordinator.TryReserveThreatImpact(racing, 1000));

    engine.ExecuteSelected(f.context, report);
    CandidateEvaluation const& result = report.candidates[report.selectedIndices[0]];
    assert(result.status == CandidateStatus::ExecutionFailed);
    assert(result.reason == "projected_threat_exceeds_core_headroom");
    assert(result.threatActionId == 0);
    assert(action->executeCount == 0);
    assert(f.coordinator.GetThreatReservationCount() == 1);
    assert(std::fabs(f.coordinator.GetReservedThreatImpact(9001, f.bot.guid, 0, 1000) - 6.0f) <
           0.0001f);
    assert(traceSink.events.size() == 1);
    assert(traceSink.events[0].stage == ReservationTraceStage::BudgetVeto);
    assert(traceSink.events[0].hostileGuid == 9001);
    assert(traceSink.events[0].threatOwnerGuid == f.bot.guid);
    assert(std::fabs(traceSink.events[0].alreadyReserved - 6.0f) < 0.0001f);
    assert(traceSink.events[0].reason == std::string("projected_threat_exceeds_core_headroom"));
    assert(result.actionId != 0);
    assert(traceSink.actionEvents.size() == 1);
    assert(traceSink.actionEvents[0].stage == EngineActionTraceStage::Result);
    assert(traceSink.actionEvents[0].actionId == result.actionId);
    assert(!traceSink.actionEvents[0].attempted);
    assert(traceSink.actionEvents[0].result == ActionResult::Failed);
    assert(traceSink.actionEvents[0].reason ==
           std::string("projected_threat_exceeds_core_headroom"));
}

void TestEngineRollsBackPartialFanoutWhenReservationCapacityIsReached() {
    Fixture f;
    PrepareEngineThreatFixture(f);
    CaptureReservationTraceSink traceSink;
    f.context.reservationTrace = &traceSink;

    // Leave exactly one slot. The action fans out to the current Tank and
    // the DPS bot; ordered insertion accepts one row, then must fail closed
    // and roll the first row back without deleting any other bot's reservation.
    for (std::uint32_t i = 0; i < 127; ++i) {
        ThreatReservation existing;
        existing.ownerGuid = 900;
        existing.threatOwnerGuid = 5000 + i;
        existing.hostileGuid = 6000 + i;
        existing.actionId = f.coordinator.NextThreatActionId();
        existing.threatPoints = 0.25f;
        existing.expiresAtMs = 2000;
        assert(existing.actionId);
        assert(f.coordinator.TryReserveThreatImpact(existing, 1000));
    }
    assert(f.coordinator.GetThreatReservationCount() == 127);

    PartyBotEngine::PartyBotEngine engine;
    auto resultListener = std::make_shared<CaptureActionResultListener>();
    assert(engine.AddActionResultListener(resultListener));
    std::shared_ptr<TestAction> action = std::make_shared<TestAction>("fanout_damage");
    action->useThreatImpact = true;
    action->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 1.0f);
    ThreatImpactEntry tankImpact;
    tankImpact.threatOwnerGuid = 101; // The current victim is not an over-aggro challenger.
    tankImpact.hostileGuid = 9001;
    tankImpact.threatPoints = 1.0f;
    tankImpact.source = ThreatImpactSource::DirectDamage;
    tankImpact.bound = ThreatImpactBound::UpperBound;
    action->threatImpact.impacts.push_back(tankImpact);
    assert(engine.RegisterAction(action));

    auto trigger = std::make_shared<TestTrigger>("fanout_capacity");
    trigger->generated.push_back(Candidate("fanout_damage", 10.0f, ActionChannel::GlobalCooldown));
    auto strategy = std::make_shared<Strategy>("damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    assert(report.candidates[report.selectedIndices[0]].status == CandidateStatus::Selected);

    engine.ExecuteSelected(f.context, report);
    CandidateEvaluation const& result = report.candidates[report.selectedIndices[0]];    assert(result.status == CandidateStatus::ExecutionFailed);
    assert(result.reason == "threat_reservation_failed");
    assert(result.threatActionId == 0);
    assert(action->executeCount == 0);
    assert(resultListener->events.size() == 1);
    assert(!resultListener->events[0].attempted);
    assert(resultListener->events[0].result == ActionResult::Failed);
    assert(resultListener->events[0].reason == "threat_reservation_failed");
    assert(resultListener->events[0].actionId == result.actionId);
    assert(f.coordinator.GetThreatReservationCount() == 127);
    assert(f.coordinator.GetReservedThreatImpact(9001, 101, 0, 1000) == 0.0f);
    assert(f.coordinator.GetReservedThreatImpact(9001, f.bot.guid, 0, 1000) == 0.0f);
    assert(traceSink.events.size() == 5);
    assert(traceSink.events[0].stage == ReservationTraceStage::Attempt);
    assert(traceSink.events[1].stage == ReservationTraceStage::Result);
    assert(traceSink.events[1].accepted);
    assert(traceSink.events[2].stage == ReservationTraceStage::Attempt);
    assert(traceSink.events[3].stage == ReservationTraceStage::Result);
    assert(!traceSink.events[3].accepted);
    assert(traceSink.events[3].reason == std::string("coordinator_rejected"));
    assert(traceSink.events[4].stage == ReservationTraceStage::Release);
    assert(traceSink.events[4].reservationRows == 1);
    assert(traceSink.events[4].reason == std::string("partial_acquire_rollback"));
}

void TestEngineThreatBudgetAccountsForHigherPriorityCandidates() {
    Fixture f;
    PrepareEngineThreatFixture(f);
    PartyBotEngine::PartyBotEngine engine;
    std::shared_ptr<TestAction> first = std::make_shared<TestAction>("first");
    std::shared_ptr<TestAction> second = std::make_shared<TestAction>("second");
    first->useThreatImpact = second->useThreatImpact = true;
    first->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 12.0f);
    second->threatImpact = MakeThreatEstimate(f.snapshot.epoch, f.bot.guid, 9001, 12.0f);
    assert(engine.RegisterAction(first) && engine.RegisterAction(second));

    auto trigger = std::make_shared<TestTrigger>("simultaneous_channels");
    trigger->generated.push_back(Candidate("first", 20.0f, ActionChannel::Movement));
    trigger->generated.push_back(Candidate("second", 10.0f, ActionChannel::Utility));
    auto strategy = std::make_shared<Strategy>("damage_strategy");
    strategy->AddTrigger(trigger);
    engine.AddStrategy(strategy);

    DecisionReport report = engine.Evaluate(f.context);
    assert(report.selectedIndices.size() == 1);
    assert(report.candidates[report.selectedIndices.front()].candidate.actionName == "first");
    assert(report.candidates[1].status == CandidateStatus::RejectedThreatBudget);
}

void TestPendingDispatchAggregatesThreatImpactAcrossCandidates()
{
    Fixture f;
    PrepareEngineThreatFixture(f);
    PartyBotEngine::PartyBotEngine engine;
    auto higher = std::make_shared<TestAction>("pending_higher");
    auto lower = std::make_shared<TestAction>("pending_lower");
    higher->pendingLifetimeMs = 1000;
    lower->pendingLifetimeMs = 1000;
    higher->useThreatImpact = lower->useThreatImpact = true;
    higher->threatImpact = MakeThreatEstimate(
        f.snapshot.epoch, f.bot.guid, 9001, 12.0f);
    lower->threatImpact = MakeThreatEstimate(
        f.snapshot.epoch, f.bot.guid, 9001, 12.0f);
    higher->threatImpact.reservationDurationMs = 1000;
    lower->threatImpact.reservationDurationMs = 1000;
    assert(engine.RegisterAction(higher) && engine.RegisterAction(lower));

    ActionCandidate highCandidate = Candidate(
        "pending_higher", 20.0f, ActionChannel::Movement);
    ActionCandidate lowCandidate = Candidate(
        "pending_lower", 10.0f, ActionChannel::Utility);
    assert(engine.QueuePendingAction(f.context, highCandidate) ==
        PendingActionEnqueueResult::Queued);
    assert(engine.QueuePendingAction(f.context, lowCandidate) ==
        PendingActionEnqueueResult::Queued);

    PendingActionDispatchSummary const dispatch =
        engine.ExecutePendingActions(f.context);
    assert(dispatch.dequeued == 2);
    assert(dispatch.selected == 1 && dispatch.executed == 1);
    assert(dispatch.requeued == 1);
    assert(higher->executeCount == 1 && lower->executeCount == 0);
    assert(engine.GetPendingActionCount() == 1);

    DecisionContext expiredContext(
        f.snapshot, f.bot, f.coordinator, f.context.nowMs + 1000);
    PendingActionDispatchSummary const expired =
        engine.ExecutePendingActions(expiredContext);
    assert(expired.expired == 1);
    assert(engine.GetPendingActionCount() == 0);
}

void TestThreatBudgetCoreThresholdProjection() {
    PartySnapshot snapshot;
    snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank, dps;
    ConfigureMember(tank, 101, PartyRole::Tank, 100, 100, true);
    ConfigureMember(dps, 202, PartyRole::RangedDamage, 100, 100);
    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, dps.guid, 90.0f, true);
    snapshot.members = {tank, dps};
    snapshot.hostiles = {hostile};

    ThreatBudgetAssessment melee = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(melee.status == ThreatBudgetStatus::BudgetAvailable);
    assert(melee.unknownReason == ThreatBudgetUnknownReason::None);
    assert(melee.usesMeleeThreshold && melee.thresholdMultiplier == 1.1f);
    assert(melee.currentVictimThreat == 100.0f && melee.memberThreat == 90.0f);
    assert(std::fabs(melee.thresholdThreat - 110.0f) < 0.0001f);
    assert(std::fabs(melee.rawHeadroom - 20.0f) < 0.0001f);

    snapshot.hostiles[0].threat[1].meleeReachable = false;
    snapshot.hostiles[0].threat[1].accessible = false;
    snapshot.hostiles[0].threat[1].threat = 120.0f;
    ThreatBudgetAssessment ranged = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(ranged.status == ThreatBudgetStatus::BudgetAvailable);
    assert(!ranged.usesMeleeThreshold && ranged.thresholdMultiplier == 1.3f);
    assert(std::fabs(ranged.thresholdThreat - 130.0f) < 0.0001f);
    assert(std::fabs(ranged.rawHeadroom - 10.0f) < 0.0001f);
}
void TestThreatBudgetStrictCoreThresholdBoundaries() {
    PartySnapshot snapshot;
    snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank, dps;
    ConfigureMember(tank, 301, PartyRole::Tank, 100, 100, true);
    ConfigureMember(dps, 302, PartyRole::RangedDamage, 100, 100);
    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, dps.guid, 109.0f, true);
    snapshot.members = {tank, dps};
    snapshot.hostiles = {hostile};

    snapshot.hostiles[0].threat[1].threat = std::nextafter(110.0f, 0.0f);
    ThreatBudgetAssessment justBelowMelee = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(justBelowMelee.status == ThreatBudgetStatus::BudgetAvailable);
    assert(justBelowMelee.rawHeadroom > 0.0f);

    snapshot.hostiles[0].threat[1].threat = 110.0f;
    ThreatBudgetAssessment atMelee = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(atMelee.status == ThreatBudgetStatus::AtOrOverBudget);
    assert(atMelee.rawHeadroom == 0.0f);

    snapshot.hostiles[0].threat[1].threat = std::nextafter(110.0f, 200.0f);
    ThreatBudgetAssessment aboveMelee = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(aboveMelee.status == ThreatBudgetStatus::AtOrOverBudget);
    assert(aboveMelee.rawHeadroom < 0.0f);

    snapshot.hostiles[0].threat[1].meleeReachable = false;
    snapshot.hostiles[0].threat[1].threat = std::nextafter(130.0f, 0.0f);
    ThreatBudgetAssessment justBelowRanged = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(justBelowRanged.status == ThreatBudgetStatus::BudgetAvailable);
    snapshot.hostiles[0].threat[1].threat = 130.0f;
    ThreatBudgetAssessment atRanged = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(atRanged.status == ThreatBudgetStatus::AtOrOverBudget);
    assert(atRanged.rawHeadroom == 0.0f);
    snapshot.hostiles[0].threat[1].threat = std::nextafter(130.0f, 200.0f);
    assert(AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250).status ==
           ThreatBudgetStatus::AtOrOverBudget);
}

void TestThreatBudgetRejectsStaleSnapshotsAndHandlesClockWrap() {
    PartySnapshot snapshot;
    snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank, dps;
    ConfigureMember(tank, 501, PartyRole::Tank, 100, 100, true);
    ConfigureMember(dps, 502, PartyRole::RangedDamage, 100, 100);
    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, dps.guid, 50.0f, true);
    snapshot.members = {tank, dps};
    snapshot.hostiles = {hostile};

    ThreatBudgetAssessment stale = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1251, 250);
    assert(stale.status == ThreatBudgetStatus::Unknown);
    assert(stale.unknownReason == ThreatBudgetUnknownReason::StaleSnapshot);

    snapshot.capturedAtMs = 0xfffffff0u;
    ThreatBudgetAssessment wrappedFresh =
        AssessThreatBudget(snapshot, hostile.guid, dps.guid, 10u, 26u);
    assert(wrappedFresh.status == ThreatBudgetStatus::BudgetAvailable);
    ThreatBudgetAssessment wrappedStale =
        AssessThreatBudget(snapshot, hostile.guid, dps.guid, 11u, 26u);
    assert(wrappedStale.unknownReason == ThreatBudgetUnknownReason::StaleSnapshot);
}
void TestThreatBudgetUnknownAndCurrentVictimStates() {
    PartySnapshot snapshot;
    snapshot.capturedAtMs = 1000;
    PartyMemberSnapshot tank, dps;
    ConfigureMember(tank, 401, PartyRole::Tank, 100, 100, true);
    ConfigureMember(dps, 402, PartyRole::RangedDamage, 100, 100);
    HostileSnapshot hostile;
    ConfigureThreat(hostile, tank.guid, 100.0f, dps.guid, 50.0f, true);
    snapshot.members = {tank, dps};
    snapshot.hostiles = {hostile};

    ThreatBudgetAssessment current = AssessThreatBudget(snapshot, hostile.guid, tank.guid, 1000, 250);
    assert(current.status == ThreatBudgetStatus::CurrentVictim);
    assert(current.currentVictimGuid == tank.guid);

    snapshot.hostiles[0].victimGuid = 0;
    ThreatBudgetAssessment noVictim = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(noVictim.status == ThreatBudgetStatus::Unknown);
    assert(noVictim.unknownReason == ThreatBudgetUnknownReason::MissingCurrentVictim);
    snapshot.hostiles[0].victimGuid = tank.guid;

    snapshot.hostiles[0].threat.erase(snapshot.hostiles[0].threat.begin());
    ThreatBudgetAssessment missingVictim = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(missingVictim.unknownReason == ThreatBudgetUnknownReason::MissingOrDuplicateVictimThreat);
    ConfigureThreat(snapshot.hostiles[0], tank.guid, 100.0f, dps.guid, 50.0f, true);
    snapshot.hostiles[0].threat[1].online = false;
    ThreatBudgetAssessment offlineMember = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(offlineMember.unknownReason == ThreatBudgetUnknownReason::MemberThreatOffline);

    snapshot.hostiles[0].threat[1].online = true;
    snapshot.hostiles[0].threat[0].threat = std::nanf("");
    ThreatBudgetAssessment invalidVictim = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(invalidVictim.unknownReason == ThreatBudgetUnknownReason::InvalidVictimThreat);

    snapshot.hostiles[0].threat[0].threat = 100.0f;
    snapshot.members[1].alive = false;
    ThreatBudgetAssessment deadMember = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(deadMember.unknownReason == ThreatBudgetUnknownReason::MemberNotOnlineOrAlive);
    snapshot.members[1].alive = true;
    snapshot.hostiles[0].alive = false;
    ThreatBudgetAssessment deadHostile = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(deadHostile.unknownReason == ThreatBudgetUnknownReason::HostileNotAlive);

    snapshot.hostiles[0].alive = true;
    ConfigureThreat(snapshot.hostiles[0], tank.guid,
                    std::numeric_limits<float>::max(), dps.guid, 50.0f, true);
    ThreatBudgetAssessment overflow = AssessThreatBudget(snapshot, hostile.guid, dps.guid, 1000, 250);
    assert(overflow.status == ThreatBudgetStatus::Unknown);
    assert(overflow.unknownReason == ThreatBudgetUnknownReason::InvalidBudgetProjection);
}

void TestEncounterLifecycleStartSettleAndSequence()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 700;
    snapshot.mapId = 0; // World map zero is valid and must not suppress tracing.
    snapshot.instanceId = 12;
    snapshot.epoch = 1;

    PartyMemberSnapshot tank;
    tank.guid = 701;
    tank.online = true;
    tank.alive = true;
    tank.inCombat = false;
    snapshot.members.push_back(tank);

    EncounterTransition none = coordinator.ObserveEncounter(snapshot, 1000, 10000);
    assert(none.kind == EncounterTransitionKind::None);
    assert(!coordinator.HasActiveEncounter());
    assert(coordinator.NextEncounterSequence() == 0);

    snapshot.members[0].inCombat = true;
    EncounterTransition started = coordinator.ObserveEncounter(snapshot, 1000, 10000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(started.encounterId != 0 && started.sequence == 1);
    assert(started.groupId == snapshot.groupId && started.mapId == 0);
    assert(started.instanceId == snapshot.instanceId);
    assert(started.elapsedMs == 0 && std::string(started.reason) == "combat_detected");
    assert(coordinator.GetEncounterId() == started.encounterId);
    assert(coordinator.NextEncounterSequence() == 2);

    // A second bot observing the same party combat does not start a duplicate trace.
    EncounterTransition duplicate = coordinator.ObserveEncounter(snapshot, 2000, 10000);
    assert(duplicate.kind == EncounterTransitionKind::None);
    assert(duplicate.encounterId == 0);

    // Combat seen on a later snapshot refreshes the settle clock.
    snapshot.epoch = 2;
    EncounterTransition active = coordinator.ObserveEncounter(snapshot, 3000, 10000);
    assert(active.kind == EncounterTransitionKind::None);
    snapshot.members[0].inCombat = false;
    snapshot.epoch = 3;

    EncounterTransition beforeSettle = coordinator.ObserveEncounter(snapshot, 12999, 10000);
    assert(beforeSettle.kind == EncounterTransitionKind::None);
    assert(coordinator.HasActiveEncounter());

    EncounterTransition ended = coordinator.ObserveEncounter(snapshot, 13000, 10000);
    assert(ended.kind == EncounterTransitionKind::Ended);
    assert(ended.encounterId == started.encounterId);
    assert(ended.sequence == 3);
    assert(ended.groupId == snapshot.groupId && ended.mapId == 0);
    assert(ended.instanceId == snapshot.instanceId);
    assert(ended.elapsedMs == 12000);
    assert(std::string(ended.reason) == "combat_settled");
    assert(!coordinator.HasActiveEncounter());
    assert(coordinator.NextEncounterSequence() == 0);
    assert(coordinator.ObserveEncounter(snapshot, 14000, 10000).kind ==
           EncounterTransitionKind::None);
}

void TestEncounterLifecycleRejectsForeignShardSnapshots()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 710;
    snapshot.mapId = 11;
    snapshot.instanceId = 12;
    snapshot.epoch = 1;

    PartyMemberSnapshot member;
    member.guid = 711;
    member.online = true;
    member.alive = true;
    member.inCombat = true;
    snapshot.members.push_back(member);

    EncounterTransition const started =
        coordinator.ObserveEncounter(snapshot, 1000, 5000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(started.encounterId != 0);

    auto rejectForeignShard = [&coordinator, &started](PartySnapshot foreign)
    {
        // Combat on another shard must not refresh this encounter's settle clock.
        foreign.epoch += 1;
        foreign.members[0].inCombat = true;
        EncounterTransition const foreignCombat =
            coordinator.ObserveEncounter(foreign, 9000, 5000);
        assert(foreignCombat.kind == EncounterTransitionKind::None);
        assert(coordinator.HasActiveEncounter());
        assert(coordinator.GetEncounterId() == started.encounterId);

        // Nor may an idle snapshot from another shard close the old encounter.
        foreign.epoch += 1;
        foreign.members[0].inCombat = false;
        EncounterTransition const foreignIdle =
            coordinator.ObserveEncounter(foreign, 9500, 5000);
        assert(foreignIdle.kind == EncounterTransitionKind::None);
        assert(coordinator.HasActiveEncounter());
        assert(coordinator.GetEncounterId() == started.encounterId);
    };

    PartySnapshot foreignGroup = snapshot;
    ++foreignGroup.groupId;
    rejectForeignShard(foreignGroup);

    PartySnapshot foreignMap = snapshot;
    ++foreignMap.mapId;
    rejectForeignShard(foreignMap);

    PartySnapshot foreignInstance = snapshot;
    ++foreignInstance.instanceId;
    rejectForeignShard(foreignInstance);

    // Only a valid observation on the owning shard may settle/close the trace.
    snapshot.members[0].inCombat = false;
    snapshot.epoch = 2;
    EncounterTransition const beforeSettle =
        coordinator.ObserveEncounter(snapshot, 5999, 5000);
    assert(beforeSettle.kind == EncounterTransitionKind::None);
    assert(coordinator.HasActiveEncounter());

    EncounterTransition const ended =
        coordinator.ObserveEncounter(snapshot, 6000, 5000);
    assert(ended.kind == EncounterTransitionKind::Ended);
    assert(ended.encounterId == started.encounterId);
    assert(ended.elapsedMs == 5000);
    assert(std::string(ended.reason) == "combat_settled");
    assert(!coordinator.HasActiveEncounter());
}

void TestEncounterLifecycleCompanionWrapAndExplicitClose()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 800;
    snapshot.mapId = 1;
    snapshot.instanceId = 2;
    snapshot.epoch = 1;

    PartyCompanionSnapshot pet;
    pet.guid = 801;
    pet.online = true;
    pet.alive = true;
    pet.inCombat = true;
    snapshot.companions.push_back(pet);

    std::uint32_t const start = 0xfffffff0u;
    EncounterTransition started = coordinator.ObserveEncounter(snapshot, start, 5000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(started.encounterId != 0);

    snapshot.companions[0].inCombat = false;
    snapshot.epoch = 2;
    EncounterTransition beforeWrapSafeSettle =
        coordinator.ObserveEncounter(snapshot, static_cast<std::uint32_t>(start + 4999u), 5000);
    assert(beforeWrapSafeSettle.kind == EncounterTransitionKind::None);

    EncounterTransition wrappedEnd =
        coordinator.ObserveEncounter(snapshot, static_cast<std::uint32_t>(start + 5000u), 5000);
    assert(wrappedEnd.kind == EncounterTransitionKind::Ended);
    assert(wrappedEnd.encounterId == started.encounterId);
    assert(wrappedEnd.elapsedMs == 5000);

    // Explicit party teardown closes an active trace once, without mutating Core state.
    snapshot.companions[0].inCombat = true;
    snapshot.epoch = 3;
    EncounterTransition restarted = coordinator.ObserveEncounter(snapshot, 100, 5000);
    assert(restarted.kind == EncounterTransitionKind::Started);
    EncounterTransition closed = coordinator.CloseEncounter(600, "party_unavailable");
    assert(closed.kind == EncounterTransitionKind::Ended);
    assert(closed.encounterId == restarted.encounterId);
    assert(closed.elapsedMs == 500);
    assert(std::string(closed.reason) == "party_unavailable");
    assert(coordinator.CloseEncounter(700, "duplicate_close").kind ==
           EncounterTransitionKind::None);
}


void TestEncounterWipeClosesImmediatelyAndClearsReservations()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 899;
    snapshot.mapId = 33;
    snapshot.instanceId = 7;
    snapshot.epoch = 1;

    for (Guid guid = 1; guid <= 5; ++guid)
    {
        PartyMemberSnapshot member;
        member.guid = guid;
        member.online = true;
        member.alive = true;
        member.inCombat = guid == 1;
        member.isPlayerBot = guid != 1;
        snapshot.members.push_back(member);
    }

    EncounterTransition started = coordinator.ObserveEncounter(snapshot, 1000, 30000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(coordinator.HasActiveEncounter());

    Reservation reservation;
    reservation.targetGuid = 77;
    reservation.ownerGuid = 2;
    reservation.expiresAtMs = 60000;
    assert(coordinator.TryReserve(reservation, 1000));
    ThreatReservation threat;
    threat.ownerGuid = 2;
    threat.threatOwnerGuid = 2;
    threat.hostileGuid = 88;
    threat.actionId = 1;
    threat.threatPoints = 25.0f;
    threat.expiresAtMs = 60000;
    assert(coordinator.TryReserveThreatImpact(threat, 1000));
    assert(coordinator.GetReservations().size() == 1);
    assert(coordinator.GetThreatReservationCount() == 1);

    // One death is not a wipe while any online party member remains alive.
    snapshot.epoch = 2;
    snapshot.members[0].alive = false;
    snapshot.members[0].inCombat = false;
    EncounterTransition partialDeath = coordinator.ObserveEncounter(snapshot, 2000, 30000);
    assert(partialDeath.kind == EncounterTransitionKind::None);
    assert(coordinator.HasActiveEncounter());
    assert(coordinator.GetReservations().size() == 1);
    assert(coordinator.GetThreatReservationCount() == 1);

    // A complete online-roster wipe closes immediately, not after quiet-settle.
    snapshot.epoch = 3;
    for (PartyMemberSnapshot& member : snapshot.members)
        member.alive = false;
    EncounterTransition wiped = coordinator.ObserveEncounter(snapshot, 2500, 30000);
    assert(wiped.kind == EncounterTransitionKind::Ended);
    assert(wiped.reason && std::string(wiped.reason) == "party_wipe");
    assert(!coordinator.HasActiveEncounter());
    assert(coordinator.GetReservations().empty());
    assert(coordinator.GetThreatReservationCount() == 0);

    // Repeated dead snapshots cannot emit a second terminal or mutate state.
    snapshot.epoch = 4;
    EncounterTransition repeatedWipe = coordinator.ObserveEncounter(snapshot, 3000, 30000);
    assert(repeatedWipe.kind == EncounterTransitionKind::None);
    assert(coordinator.GetReservations().empty());
    assert(coordinator.GetThreatReservationCount() == 0);

    // A resurrected member alone does not reopen combat; new combat does.
    snapshot.epoch = 5;
    snapshot.members[0].alive = true;
    EncounterTransition resurrected = coordinator.ObserveEncounter(snapshot, 4000, 30000);
    assert(resurrected.kind == EncounterTransitionKind::None);
    snapshot.epoch = 6;
    snapshot.members[0].inCombat = true;
    EncounterTransition restarted = coordinator.ObserveEncounter(snapshot, 5000, 30000);
    assert(restarted.kind == EncounterTransitionKind::Started);
    assert(restarted.encounterId != started.encounterId);

    // An empty online roster is unknown, not a wipe.
    PartyCoordinator offlineCoordinator;
    snapshot.epoch = 1;
    snapshot.members[0].alive = true;
    snapshot.members[0].online = true;
    snapshot.members[0].inCombat = true;
    EncounterTransition offlineStart =
        offlineCoordinator.ObserveEncounter(snapshot, 1000, 30000);
    assert(offlineStart.kind == EncounterTransitionKind::Started);
    snapshot.epoch = 2;
    for (PartyMemberSnapshot& member : snapshot.members)
        member.online = false;
    EncounterTransition allOffline =
        offlineCoordinator.ObserveEncounter(snapshot, 2000, 30000);
    assert(allOffline.kind == EncounterTransitionKind::None);
    assert(offlineCoordinator.HasActiveEncounter());
}


void TestEncounterHeartbeatIsSharedThrottledSequencedAndWrapSafe()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 900;
    snapshot.mapId = 0;
    snapshot.instanceId = 3;
    snapshot.epoch = 1;

    PartyMemberSnapshot member;
    member.guid = 901;
    member.online = true;
    member.alive = true;
    member.inCombat = true;
    snapshot.members.push_back(member);

    std::uint32_t const start = 1000;
    EncounterTransition started = coordinator.ObserveEncounter(snapshot, start, 10000);
    assert(started.kind == EncounterTransitionKind::Started);

    EncounterHeartbeat heartbeat;
    assert(!coordinator.TryBeginEncounterHeartbeat(start, 0, heartbeat));
    assert(!coordinator.TryBeginEncounterHeartbeat(start, 0x80000000u, heartbeat));
    assert(coordinator.TryBeginEncounterHeartbeat(start, 2000, heartbeat));
    assert(heartbeat.encounterId == started.encounterId);
    assert(heartbeat.sequence == 2);
    assert(heartbeat.groupId == snapshot.groupId);
    assert(heartbeat.mapId == 0);
    assert(heartbeat.instanceId == snapshot.instanceId);
    assert(heartbeat.elapsedMs == 0);

    // All bots share the coordinator, so a second bot cannot duplicate the
    // same party heartbeat at the same map tick.
    EncounterHeartbeat duplicate;
    assert(!coordinator.TryBeginEncounterHeartbeat(start, 2000, duplicate));
    assert(!coordinator.TryBeginEncounterHeartbeat(start + 1999u, 2000, duplicate));
    assert(coordinator.TryBeginEncounterHeartbeat(start + 2000u, 2000, duplicate));
    assert(duplicate.encounterId == started.encounterId);
    assert(duplicate.sequence == 3);
    assert(duplicate.elapsedMs == 2000);

    EncounterTransition closed = coordinator.CloseEncounter(start + 2500u, "test_close");
    assert(closed.kind == EncounterTransitionKind::Ended);
    assert(!coordinator.TryBeginEncounterHeartbeat(start + 3000u, 2000, duplicate));

    // A new encounter resets the shared heartbeat gate and its sequence.
    snapshot.epoch = 2;
    EncounterTransition restarted =
        coordinator.ObserveEncounter(snapshot, start + 4000u, 10000);
    assert(restarted.kind == EncounterTransitionKind::Started);
    assert(restarted.encounterId != started.encounterId);
    assert(coordinator.TryBeginEncounterHeartbeat(start + 4000u, 2000, duplicate));
    assert(duplicate.encounterId == restarted.encounterId);
    assert(duplicate.sequence == 2);
    assert(duplicate.elapsedMs == 0);

    // Unsigned elapsed time keeps the interval gate correct across wraparound.
    PartyCoordinator wrappedCoordinator;
    snapshot.epoch = 1;
    std::uint32_t const wrapStart = 0xfffffff0u;
    EncounterTransition wrappedStart =
        wrappedCoordinator.ObserveEncounter(snapshot, wrapStart, 10000);
    assert(wrappedStart.kind == EncounterTransitionKind::Started);
    assert(wrappedCoordinator.TryBeginEncounterHeartbeat(wrapStart, 2000, heartbeat));
    assert(!wrappedCoordinator.TryBeginEncounterHeartbeat(
        static_cast<std::uint32_t>(wrapStart + 1999u), 2000, duplicate));
    assert(wrappedCoordinator.TryBeginEncounterHeartbeat(
        static_cast<std::uint32_t>(wrapStart + 2000u), 2000, duplicate));
    assert(duplicate.sequence == 3);
    assert(duplicate.elapsedMs == 2000);
}


void TestEncounterTraceBudgetIsPartyWideBoundedAndTerminalReserved()
{
    PartyCoordinator coordinator;
    PartySnapshot snapshot;
    snapshot.groupId = 990;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 1;
    PartyMemberSnapshot member;
    member.guid = 991;
    member.online = true;
    member.alive = true;
    member.inCombat = true;
    snapshot.members.push_back(member);

    EncounterTransition started = coordinator.ObserveEncounter(snapshot, 1000, 10000);
    assert(started.kind == EncounterTransitionKind::Started);
    assert(coordinator.TryAdmitEncounterTraceEvent(
        started.encounterId, EncounterTraceEventClass::Important) ==
        EncounterTraceAdmission::Accepted); // encounter_start

    for (std::uint32_t i = 0; i < PartyCoordinator::MaxDetailTraceEvents; ++i)
        assert(coordinator.TryAdmitEncounterTraceEvent(
            started.encounterId, EncounterTraceEventClass::Detail) ==
            EncounterTraceAdmission::Accepted);
    assert(coordinator.TryAdmitEncounterTraceEvent(
        started.encounterId, EncounterTraceEventClass::Detail) ==
        EncounterTraceAdmission::EmitLimitMarker);
    assert(coordinator.TryAdmitEncounterTraceEvent(
        started.encounterId, EncounterTraceEventClass::Detail) ==
        EncounterTraceAdmission::Suppressed);

    // The terminal event remains admissible after the encounter is closed,
    // even after the ordinary budget has been exhausted.
    EncounterTransition ended = coordinator.CloseEncounter(2000, "test_close");
    assert(ended.kind == EncounterTransitionKind::Ended);
    assert(coordinator.TryAdmitEncounterTraceEvent(
        ended.encounterId, EncounterTraceEventClass::Terminal) ==
        EncounterTraceAdmission::Accepted);
    assert(coordinator.TryAdmitEncounterTraceEvent(
        ended.encounterId, EncounterTraceEventClass::Terminal) ==
        EncounterTraceAdmission::Suppressed);

    // A new encounter receives a fresh budget and a distinct encounter ID.
    snapshot.epoch = 2;
    EncounterTransition restarted = coordinator.ObserveEncounter(snapshot, 3000, 10000);
    assert(restarted.kind == EncounterTransitionKind::Started);
    assert(restarted.encounterId != started.encounterId);
    assert(coordinator.TryAdmitEncounterTraceEvent(
        restarted.encounterId, EncounterTraceEventClass::Detail) ==
        EncounterTraceAdmission::Accepted);

    PartyCoordinator importantCoordinator;
    EncounterTransition importantStart =
        importantCoordinator.ObserveEncounter(snapshot, 4000, 10000);
    assert(importantStart.kind == EncounterTransitionKind::Started);
    for (std::uint32_t i = 0; i < PartyCoordinator::MaxImportantTraceEvents; ++i)
        assert(importantCoordinator.TryAdmitEncounterTraceEvent(
            importantStart.encounterId, EncounterTraceEventClass::Important) ==
            EncounterTraceAdmission::Accepted);
    assert(importantCoordinator.TryAdmitEncounterTraceEvent(
        importantStart.encounterId, EncounterTraceEventClass::Important) ==
        EncounterTraceAdmission::EmitLimitMarker);
    assert(importantCoordinator.TryAdmitEncounterTraceEvent(
        importantStart.encounterId, EncounterTraceEventClass::Important) ==
        EncounterTraceAdmission::Suppressed);
}


void TestEncounterTraceBudgetConfigurationIsClampedAndBounded()
{
    PartySnapshot snapshot;
    snapshot.groupId = 992;
    snapshot.mapId = 1;
    snapshot.instanceId = 9;
    snapshot.epoch = 1;
    PartyMemberSnapshot member;
    member.guid = 993;
    member.online = true;
    member.alive = true;
    member.inCombat = true;
    snapshot.members.push_back(member);

    PartyCoordinator small;
    small.ConfigureEncounterTraceBudget(2, 1);
    EncounterTransition smallStart = small.ObserveEncounter(snapshot, 1000, 10000);
    assert(smallStart.kind == EncounterTransitionKind::Started);
    assert(small.TryAdmitEncounterTraceEvent(
        smallStart.encounterId, EncounterTraceEventClass::Important) ==
        EncounterTraceAdmission::Accepted);
    assert(small.TryAdmitEncounterTraceEvent(
        smallStart.encounterId, EncounterTraceEventClass::Important) ==
        EncounterTraceAdmission::EmitLimitMarker);
    assert(small.TryAdmitEncounterTraceEvent(
        smallStart.encounterId, EncounterTraceEventClass::Important) ==
        EncounterTraceAdmission::Suppressed);

    // Zero is clamped to one, preserving an explicit overflow marker.
    PartyCoordinator minimum;
    minimum.ConfigureEncounterTraceBudget(0, 0);
    EncounterTransition minimumStart = minimum.ObserveEncounter(snapshot, 2000, 10000);
    assert(minimumStart.kind == EncounterTransitionKind::Started);
    assert(minimum.TryAdmitEncounterTraceEvent(
        minimumStart.encounterId, EncounterTraceEventClass::Detail) ==
        EncounterTraceAdmission::Accepted);
    assert(minimum.TryAdmitEncounterTraceEvent(
        minimumStart.encounterId, EncounterTraceEventClass::Detail) ==
        EncounterTraceAdmission::EmitLimitMarker);

    // Oversized settings are clamped to a finite hard ceiling.
    PartyCoordinator maximum;
    maximum.ConfigureEncounterTraceBudget(999999, 999999);
    EncounterTransition maximumStart = maximum.ObserveEncounter(snapshot, 3000, 10000);
    assert(maximumStart.kind == EncounterTransitionKind::Started);
    for (std::uint32_t i = 0; i < PartyCoordinator::HardMaxDetailTraceEvents; ++i)
        assert(maximum.TryAdmitEncounterTraceEvent(
            maximumStart.encounterId, EncounterTraceEventClass::Detail) ==
            EncounterTraceAdmission::Accepted);
    assert(maximum.TryAdmitEncounterTraceEvent(
        maximumStart.encounterId, EncounterTraceEventClass::Detail) ==
        EncounterTraceAdmission::EmitLimitMarker);
}

}
void TestActionCandidatePreservesStableItemIdentity() {
    ActionCandidate candidate;
    assert(candidate.itemGuid == 0);
    assert(candidate.itemEntry == 0);
    assert(candidate.itemPosition == 0xffff);
    assert(candidate.itemSpellSlot == 0xff);
    candidate.itemGuid = 0x1122334455667788ULL;
    candidate.itemEntry = 2457;
    candidate.itemPosition = 0x0203;
    candidate.itemSpellSlot = 1;
    CandidateEvaluation evaluation;
    evaluation.candidate = candidate;
    ActionCandidate copied = evaluation.candidate;
    assert(copied.itemGuid == candidate.itemGuid);
    assert(copied.itemEntry == candidate.itemEntry);
    assert(copied.itemPosition == candidate.itemPosition);
    assert(copied.itemSpellSlot == candidate.itemSpellSlot);
}



class SingleCandidateTestTrigger final : public Trigger {
public:
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const&) const override { return true; }
    void CollectCandidates(DecisionContext const&, std::vector<ActionCandidate>& candidates) const override {
        ActionCandidate candidate;
        candidate.actionName = "clear_policy_test_action";
        candidate.basePriority = 1.0f;
        candidate.channel = ActionChannel::Utility;
        candidates.push_back(candidate);
    }
private:
    std::string m_name = "single_candidate_test";
};

void TestClearPoliciesRemovesStrategiesActionsAndPendingWork() {
    PartySnapshot snapshot;
    snapshot.groupId = 1;
    snapshot.mapId = 33;
    snapshot.instanceId = 101;
    snapshot.epoch = 1;
    PartyMemberSnapshot bot;
    bot.guid = 10;
    bot.mapId = snapshot.mapId;
    bot.instanceId = snapshot.instanceId;
    bot.online = true;
    bot.alive = true;
    snapshot.members.push_back(bot);
    PartyCoordinator coordinator;
    DecisionContext context(snapshot, snapshot.members[0], coordinator, 1000);

    PartyBotEngine::PartyBotEngine engine;
    assert(engine.RegisterAction(std::make_shared<TestAction>("clear_policy_test_action")));
    std::shared_ptr<Strategy> strategy(new Strategy("clear_policy_test"));
    strategy->AddTrigger(std::make_shared<SingleCandidateTestTrigger>());
    engine.AddStrategy(strategy);
    assert(engine.Evaluate(context).candidates.size() == 1);

    engine.ClearPolicies();
    DecisionReport const cleared = engine.Evaluate(context);
    assert(cleared.candidates.empty());
    assert(engine.GetPendingActionCount() == 0);
}

void TestFormationAssignmentsRemainStableAcrossRosterAndLeaderChanges() {
    PartySnapshot snapshot;
    snapshot.groupId = 93;
    snapshot.mapId = 33;
    snapshot.instanceId = 101;
    snapshot.groupLeaderGuid = 100;

    PartyMemberSnapshot leader;
    leader.guid = 100;
    leader.mapId = snapshot.mapId;
    leader.instanceId = snapshot.instanceId;
    leader.online = true;
    leader.alive = true;
    snapshot.members.push_back(leader);

    auto addBot = [&snapshot](Guid guid) {
        PartyMemberSnapshot bot;
        bot.guid = guid;
        bot.mapId = snapshot.mapId;
        bot.instanceId = snapshot.instanceId;
        bot.online = true;
        bot.alive = true;
        bot.isPlayerBot = true;
        snapshot.members.push_back(bot);
    };
    addBot(50);
    addBot(20);
    addBot(40);
    addBot(30);

    PartyCoordinator coordinator;
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(30) == 1);
    assert(coordinator.GetFormationSlot(40) == 2);
    assert(coordinator.GetFormationSlot(50) == 3);
    assert(coordinator.GetFormationSlot(100) == InvalidFormationSlot);

    // Death does not release an in-world Bot's slot; resurrection can reuse its place.
    for (PartyMemberSnapshot& member : snapshot.members)
        if (member.guid == 40)
            member.alive = false;
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(40) == 2);

    // Snapshot/Group member iteration order must not reshuffle existing Bots.
    std::reverse(snapshot.members.begin(), snapshot.members.end());
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(30) == 1);
    assert(coordinator.GetFormationSlot(40) == 2);
    assert(coordinator.GetFormationSlot(50) == 3);

    // A Bot promoted to leader releases its own slot; other assignments remain stable.
    snapshot.groupLeaderGuid = 30;
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(30) == InvalidFormationSlot);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(40) == 2);
    assert(coordinator.GetFormationSlot(50) == 3);

    // A newly joined Bot takes the lowest vacant slot without moving incumbents.
    addBot(60);
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(60) == 1);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(40) == 2);
    assert(coordinator.GetFormationSlot(50) == 3);

    // Departed members release their slot, which is then available to a newcomer.
    snapshot.members.erase(
        std::remove_if(snapshot.members.begin(), snapshot.members.end(),
            [](PartyMemberSnapshot const& member) { return member.guid == 40; }),
        snapshot.members.end());
    addBot(70);
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(70) == 2);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(60) == 1);
    assert(coordinator.GetFormationSlot(50) == 3);

    // The bounded four-slot allocator never gives an excess Bot a shared position.
    addBot(80);
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(80) == InvalidFormationSlot);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(70) == 2);

    // Group identity changes reset the old assignment domain deterministically.
    snapshot.groupId = 94;
    snapshot.groupLeaderGuid = 20;
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(20) == InvalidFormationSlot);
    assert(coordinator.GetFormationSlot(30) == 0);
    assert(coordinator.GetFormationSlot(50) == 1);
    assert(coordinator.GetFormationSlot(60) == 2);
    assert(coordinator.GetFormationSlot(70) == 3);
    assert(coordinator.GetFormationSlot(80) == InvalidFormationSlot);

    // A map transition also resets slots; current-map membership is re-evaluated.
    snapshot.mapId = 34;
    snapshot.groupLeaderGuid = 50;
    for (PartyMemberSnapshot& member : snapshot.members)
        member.mapId = snapshot.mapId;
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(50) == InvalidFormationSlot);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(30) == 1);
    assert(coordinator.GetFormationSlot(60) == 2);
    assert(coordinator.GetFormationSlot(70) == 3);

    // Instance transitions use a distinct formation assignment domain as well.
    snapshot.instanceId = 202;
    snapshot.groupLeaderGuid = 70;
    for (PartyMemberSnapshot& member : snapshot.members)
        member.instanceId = snapshot.instanceId;
    coordinator.ReconcileFormationAssignments(snapshot);
    assert(coordinator.GetFormationSlot(70) == InvalidFormationSlot);
    assert(coordinator.GetFormationSlot(20) == 0);
    assert(coordinator.GetFormationSlot(30) == 1);
    assert(coordinator.GetFormationSlot(50) == 2);
    assert(coordinator.GetFormationSlot(60) == 3);
    assert(coordinator.GetFormationSlot(80) == InvalidFormationSlot);

    FollowFormationOffset const slot0 = GetFollowFormationOffset(0);
    FollowFormationOffset const slot1 = GetFollowFormationOffset(1);
    FollowFormationOffset const slot2 = GetFollowFormationOffset(2);
    FollowFormationOffset const slot3 = GetFollowFormationOffset(3);
    FollowFormationOffset const invalid = GetFollowFormationOffset(InvalidFormationSlot);
    assert(slot0.distance == 2.5f && std::fabs(slot0.angle - 3.14159265358979323846f) < 0.0001f);
    assert(slot1.distance == 3.25f && slot1.angle < slot0.angle);
    assert(slot2.distance == 3.25f && slot2.angle > slot0.angle);
    assert(slot3.distance == 4.75f && slot3.angle == slot0.angle);
    assert(invalid.distance == slot0.distance && invalid.angle == slot0.angle);

    // Generic slotless requests retain the old same-target no-op contract.
    assert(IsSameTargetFollowRequestNoOp(123, InvalidFormationSlot, 123, 0));
    // Production requests are no-ops only when both target and slot match.
    assert(IsSameTargetFollowRequestNoOp(123, 0, 123, 0));
    assert(!IsSameTargetFollowRequestNoOp(123, 1, 123, 0));
    assert(!IsSameTargetFollowRequestNoOp(124, 1, 123, 0));
    assert(!IsSameTargetFollowRequestNoOp(0, InvalidFormationSlot, 0, 0));
}

void TestFollowPartyLeaderTriggerRequiresIdleBotAndLiveLeader() {
    PartySnapshot snapshot;
    snapshot.groupId = 2;
    snapshot.mapId = 33;
    snapshot.instanceId = 101;
    snapshot.epoch = 7;
    snapshot.groupLeaderGuid = 22;

    PartyMemberSnapshot bot;
    bot.guid = 11;
    bot.mapId = snapshot.mapId;
    bot.instanceId = snapshot.instanceId;
    bot.online = true;
    bot.alive = true;
    bot.isPlayerBot = true;
    snapshot.members.push_back(bot);

    PartyMemberSnapshot leader;
    leader.guid = 22;
    leader.mapId = snapshot.mapId;
    leader.instanceId = snapshot.instanceId;
    leader.online = true;
    leader.alive = true;
    snapshot.members.push_back(leader);

    PartyCoordinator coordinator;
    coordinator.ReconcileFormationAssignments(snapshot);
    FollowPartyLeaderTrigger trigger;
    DecisionContext idleContext(snapshot, snapshot.members[0], coordinator, 1000);
    assert(trigger.IsActive(idleContext));
    std::vector<ActionCandidate> candidates;
    trigger.CollectCandidates(idleContext, candidates);
    assert(candidates.size() == 1);
    assert(candidates[0].actionName == "core_follow_party_member");
    assert(candidates[0].targetGuid == snapshot.groupLeaderGuid);
    assert(candidates[0].channel == ActionChannel::Movement);
    assert(candidates[0].formationSlot == 0);

    snapshot.members[0].followTargetGuid = snapshot.groupLeaderGuid;
    snapshot.members[0].followFormationSlot = 0;
    DecisionContext followingContext(snapshot, snapshot.members[0], coordinator, 2000);
    candidates.clear();
    trigger.CollectCandidates(followingContext, candidates);
    assert(candidates.empty());

    snapshot.members[0].followFormationSlot = 1;
    DecisionContext wrongFormationContext(snapshot, snapshot.members[0], coordinator, 2500);
    candidates.clear();
    trigger.CollectCandidates(wrongFormationContext, candidates);
    assert(candidates.size() == 1);
    assert(candidates[0].targetGuid == snapshot.groupLeaderGuid);
    assert(candidates[0].formationSlot == 0);

    snapshot.members[0].followTargetGuid = 0;
    snapshot.members[0].inCombat = true;
    DecisionContext combatContext(snapshot, snapshot.members[0], coordinator, 3000);
    assert(!trigger.IsActive(combatContext));
    candidates.clear();
    trigger.CollectCandidates(combatContext, candidates);
    assert(candidates.empty());

    snapshot.members[0].inCombat = false;
    snapshot.members[1].online = false;
    DecisionContext offlineLeaderContext(snapshot, snapshot.members[0], coordinator, 4000);
    assert(trigger.IsActive(offlineLeaderContext));
    candidates.clear();
    trigger.CollectCandidates(offlineLeaderContext, candidates);
    assert(candidates.empty());
}


void TestBotBehaviorStateMachineTransitionsAndTimeoutClockWrap() {
    PartySnapshot snapshot;
    snapshot.groupId = 21; snapshot.mapId = 33; snapshot.instanceId = 101;
    snapshot.groupLeaderGuid = 22;
    PartyMemberSnapshot bot;
    bot.guid = 11; bot.mapId = snapshot.mapId; bot.instanceId = snapshot.instanceId;
    bot.online = true; bot.alive = true; snapshot.members.push_back(bot);
    PartyMemberSnapshot leader;
    leader.guid = 22; leader.mapId = snapshot.mapId; leader.instanceId = snapshot.instanceId;
    leader.online = true; leader.alive = true; snapshot.members.push_back(leader);

    BotBehaviorStateMachine machine;
    BotBehaviorStateObservation state = machine.Observe(snapshot, snapshot.members[0], 0xfffffff0u);
    assert(state.changed && state.state == BotBehaviorState::Idle);
    assert(state.previousState == BotBehaviorState::Unknown);
    assert(state.reason == std::string("initial_state_observed"));

    snapshot.members[0].followTargetGuid = snapshot.groupLeaderGuid;
    state = machine.Observe(snapshot, snapshot.members[0], 0xfffffff8u);
    assert(state.changed && state.state == BotBehaviorState::FollowingLeader);
    assert(state.reason == std::string("following_group_leader"));
    state = machine.Observe(snapshot, snapshot.members[0], 0x20u);
    assert(!state.changed && state.elapsedMs == 0x28u);

    snapshot.members[0].inCombat = true;
    state = machine.Observe(snapshot, snapshot.members[0], 0x40u);
    assert(state.changed && state.state == BotBehaviorState::Combat);
    assert(state.reason == std::string("combat_entered"));
    snapshot.members[0].inCombat = false;
    snapshot.members[0].followTargetGuid = 0;
    state = machine.Observe(snapshot, snapshot.members[0], 0x50u);
    assert(state.changed && state.state == BotBehaviorState::Idle);
    assert(state.reason == std::string("combat_ended"));

    snapshot.members[0].followTargetGuid = 99;
    state = machine.Observe(snapshot, snapshot.members[0], 0x60u);
    assert(state.changed && state.state == BotBehaviorState::FollowingUnavailableTarget);
    assert(state.reason == std::string("follow_target_unavailable"));

    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs - 1);
    assert(!state.changed && !state.HasTimedOut(InvalidFollowTargetTimeoutMs));
    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs);
    assert(state.HasTimedOut(InvalidFollowTargetTimeoutMs));
    assert(state.elapsedMs == InvalidFollowTargetTimeoutMs);

    snapshot.groupId = 22;
    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs + 1);
    assert(state.changed && state.reason == std::string("party_context_changed"));
    assert(state.elapsedMs == 0);

    // Core death states must not collapse corpse and released ghost into one
    // generic dead value: NewEngine recovery policy needs the distinction.
    snapshot.members[0].followTargetGuid = 0;
    snapshot.members[0].alive = false;
    snapshot.members[0].deathState = PartyMemberDeathState::Dying;
    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs + 2);
    assert(state.changed && state.state == BotBehaviorState::Corpse);
    assert(state.reason == std::string("became_corpse"));
    snapshot.members[0].deathState = PartyMemberDeathState::ReleasedGhost;
    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs + 3);
    assert(state.changed && state.state == BotBehaviorState::ReleasedGhost);
    assert(state.reason == std::string("spirit_released"));
    snapshot.members[0].deathState = PartyMemberDeathState::Resurrecting;
    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs + 4);
    assert(state.changed && state.state == BotBehaviorState::Resurrecting);
    assert(state.reason == std::string("resurrection_started"));
    snapshot.members[0].deathState = PartyMemberDeathState::Alive;
    snapshot.members[0].alive = true;
    state = machine.Observe(snapshot, snapshot.members[0],
        0x60u + InvalidFollowTargetTimeoutMs + 5);
    assert(state.changed && state.state == BotBehaviorState::Idle);
    assert(state.reason == std::string("resurrected"));
}

void TestAutoResurrectSelfPolicyDistinguishesCorpseAndReleasedGhost() {
    PartySnapshot snapshot;
    snapshot.groupId = 41;
    snapshot.mapId = 33;
    snapshot.instanceId = 101;

    PartyMemberSnapshot bot;
    bot.guid = 100;
    bot.mapId = snapshot.mapId;
    bot.instanceId = snapshot.instanceId;
    bot.online = true;
    bot.alive = false;
    bot.deathState = PartyMemberDeathState::Corpse;
    bot.x = 0.0f; bot.y = 0.0f; bot.z = 0.0f;
    snapshot.members.push_back(bot);

    PartyMemberSnapshot nearby;
    nearby.guid = 200;
    nearby.classId = 8; // Mage: not one of the four healer-capable Vanilla classes.
    nearby.mapId = snapshot.mapId;
    nearby.instanceId = snapshot.instanceId;
    nearby.online = true;
    nearby.alive = true;
    nearby.x = 10.0f; nearby.y = 0.0f; nearby.z = 0.0f;
    snapshot.members.push_back(nearby);

    assert(ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    PartyCoordinator coordinator;
    AutoResurrectSelfTrigger trigger;
    DecisionContext corpseContext(snapshot, snapshot.members[0], coordinator, 1000);
    std::vector<ActionCandidate> candidates;
    trigger.CollectCandidates(corpseContext, candidates);
    assert(candidates.size() == 1);
    assert(candidates[0].actionName == "core_resurrect_self");
    assert(candidates[0].targetGuid == 0);
    assert(candidates[0].channel == ActionChannel::Utility);

    // A healer-capable party member suppresses corpse auto-resurrection,
    // matching CombatBotBaseAI::IsHealerClass rather than the Bot's role label.
    snapshot.members[1].classId = 5; // Priest.
    assert(IsResurrectionHealerClass(snapshot.members[1].classId));
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    candidates.clear();
    trigger.CollectCandidates(corpseContext, candidates);
    assert(candidates.empty());

    // Any online party member in combat also suppresses corpse auto-resurrection.
    snapshot.members[1].classId = 8;
    snapshot.members[1].inCombat = true;
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    snapshot.members[1].inCombat = false;

    // The corpse branch requires a nearby living player on the same map/instance.
    snapshot.members[1].x = 16.0f;
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    snapshot.members[1].x = 10.0f;
    snapshot.members[1].instanceId = 102;
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    snapshot.members[1].instanceId = snapshot.instanceId;

    // Dying/falling and unknown death states are not executable resurrection targets.
    snapshot.members[0].deathState = PartyMemberDeathState::Dying;
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    snapshot.members[0].deathState = PartyMemberDeathState::CorpseFalling;
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));

    // A released ghost may self-resurrect regardless of nearby healer/combat state,
    // preserving the existing ShouldAutoRevive() DEAD fast path.
    snapshot.members[0].deathState = PartyMemberDeathState::ReleasedGhost;
    snapshot.members[1].classId = 5;
    snapshot.members[1].inCombat = true;
    assert(ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
    candidates.clear();
    DecisionContext ghostContext(snapshot, snapshot.members[0], coordinator, 2000);
    trigger.CollectCandidates(ghostContext, candidates);
    assert(candidates.size() == 1 &&
        candidates[0].actionName == "core_resurrect_self");

    snapshot.members[0].online = false;
    assert(!ShouldAutoResurrectSelf(snapshot, snapshot.members[0]));
}

void TestResurrectGroupMemberTriggerAndActionEligibility() {
    PartySnapshot snapshot;
    snapshot.groupId = 51;
    snapshot.mapId = 33;
    snapshot.instanceId = 101;
    snapshot.groupLeaderGuid = 10;

    PartyMemberSnapshot leader;
    leader.guid = 10;
    leader.mapId = snapshot.mapId;
    leader.instanceId = snapshot.instanceId;
    leader.online = true;
    leader.alive = true;
    snapshot.members.push_back(leader);

    PartyMemberSnapshot caster;
    caster.guid = 11;
    caster.classId = 5;
    caster.mapId = snapshot.mapId;
    caster.instanceId = snapshot.instanceId;
    caster.online = true;
    caster.alive = true;
    caster.isPlayerBot = true;
    caster.resurrectionSpellId = 2006;
    snapshot.members.push_back(caster);

    PartyMemberSnapshot corpse;
    corpse.guid = 22;
    corpse.classId = 8;
    corpse.mapId = snapshot.mapId;
    corpse.instanceId = snapshot.instanceId;
    corpse.online = true;
    corpse.alive = false;
    corpse.deathState = PartyMemberDeathState::Corpse;
    snapshot.members.push_back(corpse);

    PartyCoordinator coordinator;
    ResurrectGroupMemberTrigger trigger;
    ResurrectGroupMemberAction action;
    DecisionContext context(snapshot, snapshot.members[1], coordinator, 1000);
    std::vector<ActionCandidate> candidates;
    trigger.CollectCandidates(context, candidates);
    assert(candidates.size() == 1);
    assert(candidates[0].actionName == "core_resurrect_group_member");
    assert(candidates[0].targetGuid == corpse.guid);
    assert(candidates[0].spellId == 2006);
    assert(candidates[0].channel == ActionChannel::GlobalCooldown);

    std::string reason;
    assert(action.IsPossible(context, candidates[0], reason));
    assert(action.IsUseful(context, candidates[0], reason));
    ThreatImpactEstimate const estimate = action.EstimateThreatImpact(context, candidates[0]);
    assert(estimate.completeness == ThreatImpactCompleteness::NoThreat);
    assert(estimate.snapshotEpoch == snapshot.epoch);

    // Resurrection is only offered to a living same-shard Corpse, never a
    // released ghost, corpse-falling transition or target with a pending offer.
    snapshot.members[2].deathState = PartyMemberDeathState::ReleasedGhost;
    DecisionContext ghost(snapshot, snapshot.members[1], coordinator, 1100);
    candidates.clear(); trigger.CollectCandidates(ghost, candidates);
    assert(candidates.empty());
    snapshot.members[2].deathState = PartyMemberDeathState::CorpseFalling;
    DecisionContext falling(snapshot, snapshot.members[1], coordinator, 1200);
    candidates.clear(); trigger.CollectCandidates(falling, candidates);
    assert(candidates.empty());
    snapshot.members[2].deathState = PartyMemberDeathState::Corpse;
    snapshot.members[2].resurrectionPending = true;
    DecisionContext pendingOffer(snapshot, snapshot.members[1], coordinator, 1300);
    candidates.clear(); trigger.CollectCandidates(pendingOffer, candidates);
    assert(candidates.empty());
    snapshot.members[2].resurrectionPending = false;

    // Actor must have a live, known resurrection spell and be out of combat/cast.
    snapshot.members[1].resurrectionSpellId = 0;
    DecisionContext noSpell(snapshot, snapshot.members[1], coordinator, 1400);
    candidates.clear(); trigger.CollectCandidates(noSpell, candidates);
    assert(candidates.empty());
    snapshot.members[1].resurrectionSpellId = 2006;
    snapshot.members[1].inDuel = true;
    DecisionContext duel(snapshot, snapshot.members[1], coordinator, 1500);
    candidates.clear(); trigger.CollectCandidates(duel, candidates);
    assert(candidates.empty());
    snapshot.members[1].inDuel = false;
    snapshot.members[1].currentSpellId = 2050;
    DecisionContext casting(snapshot, snapshot.members[1], coordinator, 1600);
    candidates.clear(); trigger.CollectCandidates(casting, candidates);
    assert(candidates.empty());
    snapshot.members[1].currentSpellId = 0;
    snapshot.members[1].inCombat = true;
    DecisionContext combat(snapshot, snapshot.members[1], coordinator, 1700);
    candidates.clear(); trigger.CollectCandidates(combat, candidates);
    assert(candidates.empty());
    snapshot.members[1].inCombat = false;

    // A foreign-shard target cannot be selected even when its death state is CORPSE.
    snapshot.members[2].mapId = 34;
    DecisionContext foreignMap(snapshot, snapshot.members[1], coordinator, 1800);
    candidates.clear(); trigger.CollectCandidates(foreignMap, candidates);
    assert(candidates.empty());
    snapshot.members[2].mapId = snapshot.mapId;

    // The shared coordinator reservation is the final arbitration guard
    // against two Bot casters selecting the same corpse in one update window.
    Reservation reserved;
    reserved.type = ReservationType::Resurrection;
    reserved.targetGuid = corpse.guid;
    reserved.ownerGuid = 33;
    reserved.spellId = 2008;
    reserved.expiresAtMs = 5000;
    assert(coordinator.TryReserve(reserved, 1900));
    DecisionContext alreadyReserved(snapshot, snapshot.members[1], coordinator, 2000);
    candidates.clear(); trigger.CollectCandidates(alreadyReserved, candidates);
    assert(candidates.empty());
}

void TestCoordinatorReleasesOnlyExactReservationOwnerAndTarget() {
    PartyCoordinator coordinator;
    Reservation first;
    first.type = ReservationType::Resurrection;
    first.targetGuid = 100;
    first.ownerGuid = 1;
    first.spellId = 2006;
    first.expiresAtMs = 10000;
    assert(coordinator.TryReserve(first, 1000));

    Reservation second;
    second.type = ReservationType::Resurrection;
    second.targetGuid = 200;
    second.ownerGuid = 1;
    second.spellId = 2006;
    second.expiresAtMs = 10000;
    assert(coordinator.TryReserve(second, 1000));

    Reservation third;
    third.type = ReservationType::Dispel;
    third.targetGuid = 100;
    third.ownerGuid = 1;
    third.expiresAtMs = 10000;
    assert(coordinator.TryReserve(third, 1000));

    assert(coordinator.GetReservations().size() == 3);
    assert(coordinator.ReleaseReservation(ReservationType::Resurrection, 100, 1));
    assert(coordinator.GetReservations().size() == 2);
    assert(!coordinator.ReleaseReservation(ReservationType::Resurrection, 100, 1));
    assert(!coordinator.ReleaseReservation(ReservationType::Resurrection, 200, 2));
    assert(std::any_of(coordinator.GetReservations().begin(),
        coordinator.GetReservations().end(), [](Reservation const& row)
        {
            return row.type == ReservationType::Resurrection &&
                row.targetGuid == 200 && row.ownerGuid == 1;
        }));
    assert(std::any_of(coordinator.GetReservations().begin(),
        coordinator.GetReservations().end(), [](Reservation const& row)
        {
            return row.type == ReservationType::Dispel &&
                row.targetGuid == 100 && row.ownerGuid == 1;
        }));
}

void TestProductionFollowTriggersRetargetStopAndRecover() {
    PartySnapshot snapshot;
    snapshot.groupId = 31; snapshot.mapId = 33; snapshot.instanceId = 101;
    snapshot.epoch = 9; snapshot.groupLeaderGuid = 22;
    PartyMemberSnapshot bot;
    bot.guid = 11; bot.mapId = snapshot.mapId; bot.instanceId = snapshot.instanceId;
    bot.online = true; bot.alive = true; bot.isPlayerBot = true; snapshot.members.push_back(bot);
    PartyMemberSnapshot leader;
    leader.guid = 22; leader.mapId = snapshot.mapId; leader.instanceId = snapshot.instanceId;
    leader.online = true; leader.alive = true; snapshot.members.push_back(leader);

    PartyCoordinator coordinator;
    coordinator.ReconcileFormationAssignments(snapshot);
    FollowPartyLeaderTrigger follow;
    StopFollowingUnsafeTrigger stop;
    std::vector<ActionCandidate> candidates;
    DecisionContext idle(snapshot, snapshot.members[0], coordinator, 1000);
    follow.CollectCandidates(idle, candidates);
    assert(candidates.size() == 1 && candidates[0].actionName == "core_follow_party_member");
    assert(candidates[0].formationSlot == 0);

    snapshot.members[0].followTargetGuid = snapshot.groupLeaderGuid;
    snapshot.members[0].followFormationSlot = 0;
    DecisionContext following(snapshot, snapshot.members[0], coordinator, 2000);
    candidates.clear(); follow.CollectCandidates(following, candidates);
    assert(candidates.empty() && !stop.IsActive(following));

    PartyMemberSnapshot nextLeader;
    nextLeader.guid = 33;
    nextLeader.mapId = snapshot.mapId;
    nextLeader.instanceId = snapshot.instanceId;
    nextLeader.online = true;
    nextLeader.alive = true;
    snapshot.members.push_back(nextLeader);

    // The original leader remains online/in-group, but another live member is
    // elected. Existing followers retarget immediately to the new leader.
    snapshot.groupLeaderGuid = nextLeader.guid;
    coordinator.ReconcileFormationAssignments(snapshot);
    DecisionContext leaderHandoff(snapshot, snapshot.members[0], coordinator, 2500);
    assert(leaderHandoff.botBehaviorState == BotBehaviorState::FollowingPartyMember);
    candidates.clear();
    follow.CollectCandidates(leaderHandoff, candidates);
    assert(candidates.size() == 1);
    assert(candidates[0].actionName == "core_follow_party_member");
    assert(candidates[0].targetGuid == nextLeader.guid);
    assert(candidates[0].formationSlot == 0);
    assert(!stop.IsActive(leaderHandoff));

    snapshot.members[0].followTargetGuid = nextLeader.guid;
    snapshot.members[0].followFormationSlot = 0;
    DecisionContext followingNewLeader(snapshot, snapshot.members[0], coordinator, 2750);
    candidates.clear();
    follow.CollectCandidates(followingNewLeader, candidates);
    assert(candidates.empty() && !stop.IsActive(followingNewLeader));

    snapshot.members[0].inCombat = true;
    DecisionContext combat(snapshot, snapshot.members[0], coordinator, 3000);
    assert(!follow.IsActive(combat) && stop.IsActive(combat));
    candidates.clear(); stop.CollectCandidates(combat, candidates);
    assert(candidates.size() == 1 && candidates[0].actionName == "core_stop_following");
    assert(candidates[0].channel == ActionChannel::Movement);

    snapshot.members[0].inCombat = false;
    snapshot.members[0].followTargetGuid = 0;
    DecisionContext recovered(snapshot, snapshot.members[0], coordinator, 4000);
    candidates.clear(); follow.CollectCandidates(recovered, candidates);
    assert(candidates.size() == 1 && candidates[0].targetGuid == snapshot.groupLeaderGuid);

    snapshot.members[0].followTargetGuid = 99;
    DecisionContext staleMember(snapshot, snapshot.members[0], coordinator, 5000,
        nullptr, nullptr, nullptr, nullptr, nullptr,
        BotBehaviorState::FollowingUnavailableTarget, 1000);
    candidates.clear(); follow.CollectCandidates(staleMember, candidates);
    assert(candidates.size() == 1 && candidates[0].targetGuid == snapshot.groupLeaderGuid);
    assert(!stop.IsActive(staleMember));

    snapshot.members[2].online = false;
    DecisionContext noLeaderBeforeTimeout(snapshot, snapshot.members[0], coordinator, 6000,
        nullptr, nullptr, nullptr, nullptr, nullptr,
        BotBehaviorState::FollowingUnavailableTarget, InvalidFollowTargetTimeoutMs - 1);
    assert(follow.IsActive(noLeaderBeforeTimeout) && !stop.IsActive(noLeaderBeforeTimeout));
    candidates.clear();
    follow.CollectCandidates(noLeaderBeforeTimeout, candidates);
    assert(candidates.empty());
    DecisionContext noLeaderTimedOut(snapshot, snapshot.members[0], coordinator, 7000,
        nullptr, nullptr, nullptr, nullptr, nullptr,
        BotBehaviorState::FollowingUnavailableTarget, InvalidFollowTargetTimeoutMs);
    assert(follow.IsActive(noLeaderTimedOut) && stop.IsActive(noLeaderTimedOut));
    candidates.clear();
    follow.CollectCandidates(noLeaderTimedOut, candidates);
    assert(candidates.empty());
    candidates.clear(); stop.CollectCandidates(noLeaderTimedOut, candidates);
    assert(candidates.size() == 1 && candidates[0].actionName == "core_stop_following");
}

int main() {
    TestActionCandidatePreservesStableItemIdentity();
    TestClearPoliciesRemovesStrategiesActionsAndPendingWork();
    TestFormationAssignmentsRemainStableAcrossRosterAndLeaderChanges();
    TestFollowPartyLeaderTriggerRequiresIdleBotAndLiveLeader();
    TestBotBehaviorStateMachineTransitionsAndTimeoutClockWrap();
    TestAutoResurrectSelfPolicyDistinguishesCorpseAndReleasedGhost();
    TestResurrectGroupMemberTriggerAndActionEligibility();
    TestCoordinatorReleasesOnlyExactReservationOwnerAndTarget();
    TestProductionFollowTriggersRetargetStopAndRecover();
    TestExecutionAuthorityModesAreExplicitAndFailSafe();
    TestValueCacheEpochExpiryInvalidationAndTypeSafety();
    TestValueCacheCapacityFailsOpenWithoutEvictingLiveEntries();
    TestCachedValueUsesContextEpochAndExplicitInvalidation();
    TestTriggerSchedulerCadenceInvalidationAndClockWrap();
    TestTriggerSchedulerCapacityFailsOpen();
    TestEngineTriggerCadenceAndExplicitInvalidation();
    TestTriggerTraceSelectionPrioritizesActionableRows();
    TestEngineTriggerObservationCapacityIsBounded();
    TestEngineActionAttemptAndResultTrace();
    TestActionResultListenersReceiveDispatchOutcomesWithoutTraceSinks();
    TestActionResultListenersAreBoundedAndSnapshotDuringDispatch();
    TestThreatReservationTerminalListenerReleasesOnlyMatchingOwnerAndEncounter();
    TestThreatReservationTerminalListenerReleasesNoEncounterTerminalWithoutCrossShardCollision();
    TestHealReservationsAggregateAcrossBotsAndReleaseOnlyExactCast();
    TestPriestHealReservationStartRollbackAndTerminalRelease();
    TestClosingEncounterClearsInFlightThreatReservations();
    TestCoreSpellTerminalListenersDeliverOwnedEventsAndAreBounded();
    TestCoreSpellSuccessReleasesAndExecutesBoundedContinuation();
    TestRejectedContinuationIsTombstonedUntilQueueReset();
    TestCoreTerminalCannotSatisfyActionResultPrerequisite();
    TestActionResultAndCoreSpellPrerequisitesAreDistinctAndAnded();
    TestLateActionResultDependencyRegistrationUsesRecentEventHistory();
    TestPointMovementTerminalDependencyUsesCorrelationStatusAndHistory();
    TestItemUseTerminalDependencyUsesCorrelationStatusAndHistory();
    TestFailedActionResultRejectsMixedDependency();
    TestActionDependenciesRequireAllAndPrerequisites();
    TestLateDependencyRegistrationUsesLatestTerminalAndWrapSafeTime();
    TestLateDependencyRegistrationRejectsKnownFailedParent();
    TestAlternativeDependencyKeepsRejectedBranchUntilGroupResolution();
    TestActionDependenciesRejectFailedAndInvalidAndPrerequisites();
    TestActionAlternativeGroupsWaitForAllBranchesAndChoosePriority();
    TestActionAlternativeGroupsUseFifoForPriorityTiesAndCancelOnExpiry();
    TestFailedInterruptedAndMismatchedTerminalsNeverReleaseContinuations();
    TestContinuationExpiryEncounterIsolationCapacityAndLateTerminalRace();
    TestCandidateTraceSelectionPreservesArbitrationSelectionAfterExecution();
    TestDecisionSummaryCountsAllArbitrationOutcomes();
    TestActionQueueIsBoundedPriorityOrderedAndStable();
    TestPendingActionQueueDeduplicatesAndPreservesOriginalExpiry();
    TestPendingActionQueuePriorityFifoExpiryAndClockWrap();
    TestPendingActionQueueAgePromotionPreventsPriorityStarvation();
    TestPendingActionQueueRejectsInvalidAndFullEntries();
    TestReadyContinuationRetriesAfterPendingQueueBackpressure();
    TestEnginePendingQueueRevalidatesCurrentEligibilityAndResetClearsIt();
    TestEnginePendingQueueDropsForeignGroupMapAndInstanceContext();
    TestEnginePendingQueuePriorityChannelDeferralPreservesOriginalExpiry();
    TestEnginePendingQueueFairnessSurvivesRevalidatedWorkSort();
    TestDecisionCycleQueuesOptInActionsAndDispatchesPendingWork();
    TestDecisionCycleBlocksPendingChannelsAndHonorsOriginalExpiry();
    TestPartyBotEnginePublishesItemUseRootAndIndependentMutation();
    TestItemUseAggregateWaitsForFanoutClosure();
    TestItemUseAggregateDelayedChildAndPartialOutcomes();
    TestItemUseAggregateAllFailureAndInterruptStatuses();
    TestItemUseAggregateRecordsMutationSeparately();
    TestItemUseAggregateNoChildrenDuplicateSlotsAndCapacity();
    TestItemUseAggregateExpiryAndClockWrap();
    TestItemUseAggregateLateChildCannotBeatDeadline();
    TestCoreActionRuntimeIsPreservedAcrossDecisionAndPendingContexts();
    TestCoreActionPreflightVetoPreventsSideEffectAndPreservesReason();
    TestExecuteSelectedDispatchesByPriorityWithStableTies();    TestExecuteSelectedHonorsPerDispatchLimit();
    TestPriorityAndChannelArbitration(); TestEligibilityRejections();
    TestReservationOwnershipAndExpiry();
    TestResurrectionReservationOwnershipExpiryAndCompetition();
    TestThreatReservationAggregationAndActionRelease();
    TestThreatReservationExpiryAndClockWrap();
    TestThreatReservationValidationAndBoundedCapacity();
    TestInvalidPriorityAndMultiplierVeto();
    TestSnapshotCacheFreshnessAndEpoch();
    TestCoordinatorRegistryIdentityAndBattlegroundBypass();
    TestEncounterLifecycleStartSettleAndSequence();
    TestEncounterLifecycleRejectsForeignShardSnapshots();
    TestEncounterLifecycleCompanionWrapAndExplicitClose();
    TestEncounterWipeClosesImmediatelyAndClearsReservations();
    TestEncounterHeartbeatIsSharedThrottledSequencedAndWrapSafe();
    TestEncounterTraceBudgetIsPartyWideBoundedAndTerminalReserved();
    TestEncounterTraceBudgetConfigurationIsClampedAndBounded();
    TestNonBotPartyMemberStateChangesAreSampledWithoutDuplicates();
    TestNonBotPartyMemberStateSamplingRejectsForeignShardSnapshots();
    TestSupportedSingleTargetDirectHealSpellShape();
    TestDirectHealHelpfulProcRiskClassifier();
    TestDirectHealThreatComponentsSplitAcrossAllHostileReferences();
    TestPriestDirectHealActionRemainsFailClosedUntilThreatClosure();
    TestPriestDirectHealSelectionUsesAffordableClosestRank();
    TestClassNeutralHealerPolicyAcceptsPaladinProfile();
    TestPriestHealingTargetSelectionIsDeterministicAndSnapshotBounded();
    TestPriestHealingTargetSelectionAccountsForOtherBotHealReservations();
    TestPriestHealingTargetSelectionAccountsForIncomingHealsHotsAndAbsorbs();
    TestCoreRandomDitherBoundsMatchVanillaCore();
    TestSchoolAwareAbsorbProjection();
    TestHealingTimelineProjectionOrdersHoTsAndCapsOverheal();
    TestPriestDirectHealReservationSubtractsIncomingForecasts();
    TestHealingTriageShadowStrategy();
    TestTankPeelTargetShadowStrategy();
    TestDpsAssistTargetShadowStrategy();
    TestDpsAssistPolicyComposition();
    TestActionThreatImpactContractDefaultsUnknownAndValidatesCompleteness();
    TestThreatImpactBudgetUsesFanoutAndReservations();
    TestThreatImpactBudgetUnknownAndCurrentVictimCases();
    TestEngineThreatBudgetPetRecipientReservation();
    TestEngineThreatBudgetSelectionAndExecutionReservations();
    TestEngineThreatBudgetRejectsUnknownAndOverBudgetCandidates();
    TestEngineThreatBudgetAccountsForHigherPriorityCandidates();
    TestPendingDispatchAggregatesThreatImpactAcrossCandidates();
    TestEngineRevalidatesReservationsImmediatelyBeforeExecution();
    TestEngineRollsBackPartialFanoutWhenReservationCapacityIsReached();
    TestEngineReleasesThreatReservationWhenExecutionFails();
    TestThreatBudgetCoreThresholdProjection();
    TestThreatBudgetStrictCoreThresholdBoundaries();
    TestThreatBudgetUnknownAndCurrentVictimStates();
    TestThreatBudgetRejectsStaleSnapshotsAndHandlesClockWrap();
    std::cout << "PartyBotEngine tests passed\n";
    return 0;
}
