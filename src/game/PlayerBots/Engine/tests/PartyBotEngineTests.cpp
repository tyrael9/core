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
private:
    std::string m_name;
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

void TestEngineThreatBudgetSelectionAndExecutionReservations() {
    Fixture f;
    PrepareEngineThreatFixture(f);
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

}
int main() {
    TestPriorityAndChannelArbitration(); TestEligibilityRejections();
    TestReservationOwnershipAndExpiry();
    TestThreatReservationAggregationAndActionRelease();
    TestThreatReservationExpiryAndClockWrap();
    TestThreatReservationValidationAndBoundedCapacity();
    TestInvalidPriorityAndMultiplierVeto();
    TestSnapshotCacheFreshnessAndEpoch();
    TestCoordinatorRegistryIdentityAndBattlegroundBypass();
    TestHealingTriageShadowStrategy();
    TestTankPeelTargetShadowStrategy();
    TestDpsAssistTargetShadowStrategy();
    TestActionThreatImpactContractDefaultsUnknownAndValidatesCompleteness();
    TestThreatImpactBudgetUsesFanoutAndReservations();
    TestThreatImpactBudgetUnknownAndCurrentVictimCases();
    TestEngineThreatBudgetSelectionAndExecutionReservations();
    TestEngineThreatBudgetRejectsUnknownAndOverBudgetCandidates();
    TestEngineThreatBudgetAccountsForHigherPriorityCandidates();
    TestEngineReleasesThreatReservationWhenExecutionFails();
    TestThreatBudgetCoreThresholdProjection();
    TestThreatBudgetStrictCoreThresholdBoundaries();
    TestThreatBudgetUnknownAndCurrentVictimStates();
    TestThreatBudgetRejectsStaleSnapshotsAndHandlesClockWrap();
    std::cout << "PartyBotEngine tests passed\n";
    return 0;
}
