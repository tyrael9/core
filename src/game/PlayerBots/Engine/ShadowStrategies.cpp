#include "ShadowStrategies.h"
#include "PartyBotEngine.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace PartyBotEngine {
namespace {

PartyRole RoleOf(PartyMemberSnapshot const& member)
{
    return static_cast<PartyRole>(member.roleId);
}

bool IsTank(PartyMemberSnapshot const& member)
{
    PartyRole const role = RoleOf(member);
    return member.isMainTank || role == PartyRole::Tank || role == PartyRole::MainTank;
}

float HealthPercent(PartyMemberSnapshot const& member)
{
    if (member.maxHealth == 0)
        return 100.0f;
    return 100.0f * static_cast<float>(member.health) /
           static_cast<float>(member.maxHealth);
}

// Prefer the live victim GUID; if it is absent/stale, use the highest accessible
// online threat reference that resolves to a same-map party member.
PartyMemberSnapshot const* ResolvePartyVictim(PartySnapshot const& snapshot,
                                               HostileSnapshot const& hostile)
{
    if (PartyMemberSnapshot const* victim = snapshot.FindMember(hostile.victimGuid))
        if (victim->online && victim->alive)
            return victim;

    PartyMemberSnapshot const* bestMember = nullptr;
    float bestThreat = -1.0f;
    for (ThreatEntrySnapshot const& entry : hostile.threat)
    {
        if (!entry.online || !entry.accessible || entry.threat < 0.0f)
            continue;
        PartyMemberSnapshot const* member = snapshot.FindMember(entry.unitGuid);
        if (!member || !member->online || !member->alive)
            continue;
        if (!bestMember || entry.threat > bestThreat)
        {
            bestMember = member;
            bestThreat = entry.threat;
        }
    }
    return bestMember;
}

int AssistRank(PartyMemberSnapshot const& member)
{
    if (member.isMainTank)
        return 5;
    PartyRole const role = RoleOf(member);
    if (role == PartyRole::Tank || role == PartyRole::MainTank)
        return 4;
    if (role == PartyRole::MeleeDamage || role == PartyRole::RangedDamage)
        return 3;
    if (role == PartyRole::Support)
        return 2;
    if (role == PartyRole::Healer)
        return 1;
    return 0;
}

PartyMemberSnapshot const* FindBestPartyMemberTargeting(PartySnapshot const& snapshot,
                                                         Guid hostileGuid)
{
    PartyMemberSnapshot const* best = nullptr;
    int bestRank = 0;
    for (PartyMemberSnapshot const& member : snapshot.members)
    {
        if (!member.online || !member.alive || member.victimGuid != hostileGuid)
            continue;
        int const rank = AssistRank(member);
        if (!best || rank > bestRank)
        {
            best = &member;
            bestRank = rank;
        }
    }
    return best;
}

ActionCandidate MakeCandidate(char const* actionName, ActionChannel channel,
                              Guid targetGuid, float priority)
{
    ActionCandidate candidate;
    candidate.actionName = actionName;
    candidate.targetGuid = targetGuid;
    candidate.basePriority = priority;
    candidate.channel = channel;
    return candidate;
}

class ShadowTargetAction : public Action {
public:
    std::string const& GetName() const override { return m_name; }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        HostileSnapshot const* hostile = context.snapshot.FindHostile(candidate.targetGuid);
        if (!hostile || !hostile->alive)
        {
            reason = "hostile_missing_or_dead";
            return false;
        }
        if (hostile->crowdControlProtected)
        {
            reason = "hostile_cc_protected";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        if (context.bot.victimGuid == candidate.targetGuid)
        {
            reason = "already_targeting_hostile";
            return false;
        }
        return true;
    }

    // Candidate-only action. This must never be used by Strategy-mode execution.
    ActionResult Execute(DecisionContext&, ActionCandidate const&) override
    {
        return ActionResult::Skipped;
    }

private:
    std::string m_name = "shadow_select_target";
};

class ShadowHealTriageAction : public Action {
public:
    std::string const& GetName() const override { return m_name; }

    bool IsPossible(DecisionContext const& context, ActionCandidate const& candidate,
                    std::string& reason) const override
    {
        PartyMemberSnapshot const* target = context.snapshot.FindMember(candidate.targetGuid);
        if (!target || !target->online || !target->alive || target->maxHealth == 0)
        {
            reason = "heal_target_missing_or_unavailable";
            return false;
        }
        return true;
    }

    bool IsUseful(DecisionContext const& context, ActionCandidate const& candidate,
                  std::string& reason) const override
    {
        PartyMemberSnapshot const* target = context.snapshot.FindMember(candidate.targetGuid);
        if (!target || HealthPercent(*target) >= 90.0f)
        {
            reason = "health_above_triage_threshold";
            return false;
        }
        return true;
    }

    // Candidate-only action. It does not cast or reserve healing in Shadow mode.
    ActionResult Execute(DecisionContext&, ActionCandidate const&) override
    {
        return ActionResult::Skipped;
    }

private:
    std::string m_name = "shadow_heal_triage";
};

class HealingTriageTrigger : public Trigger {
public:
    std::string const& GetName() const override { return m_name; }

    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && RoleOf(context.bot) == PartyRole::Healer;
    }

    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (PartyMemberSnapshot const& member : context.snapshot.members)
        {
            if (!member.online || !member.alive || member.maxHealth == 0)
                continue;

            float const healthPercent = HealthPercent(member);
            if (healthPercent >= 90.0f)
                continue;

            float priority = 50.0f + (100.0f - healthPercent) * 2.0f;
            if (healthPercent <= 30.0f)
                priority += 100.0f;
            else if (healthPercent <= 50.0f)
                priority += 40.0f;
            if (IsTank(member))
                priority += 20.0f;
            if (member.guid == context.bot.guid)
                priority += 5.0f;

            candidates.push_back(MakeCandidate("shadow_heal_triage",
                                               ActionChannel::Utility,
                                               member.guid, priority));
        }
    }

private:
    std::string m_name = "healing_triage";
};

class TankTargetTrigger : public Trigger {
public:
    std::string const& GetName() const override { return m_name; }

    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.alive && IsTank(context.bot);
    }

    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (HostileSnapshot const& hostile : context.snapshot.hostiles)
        {
            if (!hostile.alive || hostile.crowdControlProtected)
                continue;

            PartyMemberSnapshot const* victim = ResolvePartyVictim(context.snapshot, hostile);
            if (!victim || victim->guid == context.bot.guid || IsTank(*victim))
                continue;

            float priority = 220.0f;
            if (RoleOf(*victim) == PartyRole::Healer)
                priority = 320.0f;
            else if (RoleOf(*victim) == PartyRole::MeleeDamage ||
                     RoleOf(*victim) == PartyRole::RangedDamage)
                priority = 260.0f;

            candidates.push_back(MakeCandidate("shadow_select_target",
                                               ActionChannel::Targeting,
                                               hostile.guid, priority));
        }
    }

private:
    std::string m_name = "tank_target";
};

class DpsTargetTrigger : public Trigger {
public:
    std::string const& GetName() const override { return m_name; }

    bool IsActive(DecisionContext const& context) const override
    {
        PartyRole const role = RoleOf(context.bot);
        return context.bot.alive &&
               (role == PartyRole::MeleeDamage || role == PartyRole::RangedDamage);
    }

    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (HostileSnapshot const& hostile : context.snapshot.hostiles)
        {
            if (!hostile.alive || hostile.crowdControlProtected)
                continue;

            float priority = 0.0f;
            if (PartyMemberSnapshot const* assisting =
                    FindBestPartyMemberTargeting(context.snapshot, hostile.guid))
            {
                if (assisting->isMainTank)
                    priority = 300.0f;
                else if (IsTank(*assisting))
                    priority = 280.0f;
                else if (RoleOf(*assisting) == PartyRole::Healer)
                    priority = 160.0f;
                else
                    priority = 230.0f;
            }

            if (PartyMemberSnapshot const* victim = ResolvePartyVictim(context.snapshot, hostile))
            {
                float const peelPriority = RoleOf(*victim) == PartyRole::Healer
                    ? 260.0f
                    : (IsTank(*victim) ? 180.0f : 210.0f);
                priority = std::max(priority, peelPriority);
            }

            if (priority <= 0.0f)
                continue;

            candidates.push_back(MakeCandidate("shadow_select_target",
                                               ActionChannel::Targeting,
                                               hostile.guid, priority));
        }
    }

private:
    std::string m_name = "dps_target";
};

} // namespace

void RegisterDefaultShadowStrategies(PartyBotEngine& engine)
{
    engine.RegisterAction(std::make_shared<ShadowTargetAction>());
    engine.RegisterAction(std::make_shared<ShadowHealTriageAction>());

    std::shared_ptr<Strategy> healing(new Strategy("healing_triage"));
    healing->AddTrigger(std::make_shared<HealingTriageTrigger>());
    engine.AddStrategy(healing);

    std::shared_ptr<Strategy> tank(new Strategy("tank_target"));
    tank->AddTrigger(std::make_shared<TankTargetTrigger>());
    engine.AddStrategy(tank);

    std::shared_ptr<Strategy> dps(new Strategy("dps_target"));
    dps->AddTrigger(std::make_shared<DpsTargetTrigger>());
    engine.AddStrategy(dps);
}

} // namespace PartyBotEngine
