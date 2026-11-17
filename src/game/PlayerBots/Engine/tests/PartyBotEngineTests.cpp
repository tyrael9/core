#include "PartyBotEngine.h"
#include "ShadowStrategies.h"
#include <cassert>
#include <cmath>
#include <iostream>
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
    ActionResult Execute(DecisionContext&, ActionCandidate const&) override { ++executeCount; return result; }
    bool possible = true, useful = true;
    ActionResult result = ActionResult::Success;
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
    assert(report.selectedIndices.size() == 1);
    CandidateEvaluation const& selected = report.candidates[report.selectedIndices.front()];
    assert(selected.candidate.actionName == "shadow_heal_triage");
    assert(selected.candidate.strategyName == "healing_triage");
    assert(selected.candidate.targetGuid == damage.guid);
    assert(selected.status == CandidateStatus::Selected);
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
    assert(report.selectedIndices.size() == 1);
    CandidateEvaluation const& selected = report.candidates[report.selectedIndices.front()];
    assert(selected.candidate.strategyName == "tank_target");
    assert(selected.candidate.targetGuid == attackingHealer.guid);
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
    assert(report.selectedIndices.size() == 1);
    CandidateEvaluation const& selected = report.candidates[report.selectedIndices.front()];
    assert(selected.candidate.strategyName == "dps_target");
    assert(selected.candidate.targetGuid == mainTankTarget.guid);
}

}
int main() {
    TestPriorityAndChannelArbitration(); TestEligibilityRejections();
    TestReservationOwnershipAndExpiry(); TestInvalidPriorityAndMultiplierVeto();
    TestSnapshotCacheFreshnessAndEpoch();
    TestCoordinatorRegistryIdentityAndBattlegroundBypass();
    TestHealingTriageShadowStrategy();
    TestTankPeelTargetShadowStrategy();
    TestDpsAssistTargetShadowStrategy();
    std::cout << "PartyBotEngine tests passed\n";
    return 0;
}
