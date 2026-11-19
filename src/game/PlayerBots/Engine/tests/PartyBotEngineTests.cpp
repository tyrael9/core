#include "PartyBotEngine.h"
#include "ShadowStrategies.h"
#include "ThreatBudget.h"
#include "ThreatBudgetPolicy.h"
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
        if (!possible) reason = "test_impossible";
        return possible;
    }
    bool IsUseful(DecisionContext const&, ActionCandidate const&, std::string& reason) const override {
        if (!useful) reason = "test_useless";
        return useful;
    }
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
    ActionResult result = ActionResult::Success;
    ThreatImpactEstimate threatImpact;
    ThreatActionId lastThreatActionId = 0;
    std::uint32_t executeCount = 0;
    std::vector<std::string>* executionOrder = nullptr;
private:
    std::string m_name;
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
    // ReservationTraceEvent exposes a borrowed reason pointer for synchronous
    // observers. Keep captured test events' backing strings alive with them.
    std::vector<std::shared_ptr<std::string>> reasonStorage;
    std::vector<ReservationTraceEvent> events;
};
class TestTrigger : public Trigger {
public:
    explicit TestTrigger(std::string name) : m_name(std::move(name)) {}
    std::string const& GetName() const override { return m_name; }
    bool IsActive(DecisionContext const&) const override { return active; }
    void CollectCandidates(DecisionContext const&, std::vector<ActionCandidate>& out) const override {
        out.insert(out.end(), generated.begin(), generated.end());
    }
    bool active = true;
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
        snapshot.epoch = 7; bot.guid = 11; bot.online = true; bot.alive = true;
    }
};
ActionCandidate Candidate(std::string name, float priority, ActionChannel channel) {
    ActionCandidate c; c.actionName = std::move(name); c.targetGuid = 22;
    c.basePriority = priority; c.channel = channel; return c;
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

}
int main() {
    TestActionQueueIsBoundedPriorityOrderedAndStable();
    TestPendingActionQueueDeduplicatesAndPreservesOriginalExpiry();
    TestPendingActionQueuePriorityFifoExpiryAndClockWrap();
    TestPendingActionQueueRejectsInvalidAndFullEntries();
    TestExecuteSelectedDispatchesByPriorityWithStableTies();
    TestExecuteSelectedHonorsPerDispatchLimit();
    TestPriorityAndChannelArbitration(); TestEligibilityRejections();
    TestReservationOwnershipAndExpiry();
    TestThreatReservationAggregationAndActionRelease();
    TestThreatReservationExpiryAndClockWrap();
    TestThreatReservationValidationAndBoundedCapacity();
    TestInvalidPriorityAndMultiplierVeto();
    TestSnapshotCacheFreshnessAndEpoch();
    TestCoordinatorRegistryIdentityAndBattlegroundBypass();
    TestEncounterLifecycleStartSettleAndSequence();
    TestEncounterLifecycleCompanionWrapAndExplicitClose();
    TestEncounterHeartbeatIsSharedThrottledSequencedAndWrapSafe();
    TestHealingTriageShadowStrategy();
    TestTankPeelTargetShadowStrategy();
    TestDpsAssistTargetShadowStrategy();
    TestActionThreatImpactContractDefaultsUnknownAndValidatesCompleteness();
    TestThreatImpactBudgetUsesFanoutAndReservations();
    TestThreatImpactBudgetUnknownAndCurrentVictimCases();
    TestEngineThreatBudgetPetRecipientReservation();
    TestEngineThreatBudgetSelectionAndExecutionReservations();
    TestEngineThreatBudgetRejectsUnknownAndOverBudgetCandidates();
    TestEngineThreatBudgetAccountsForHigherPriorityCandidates();
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