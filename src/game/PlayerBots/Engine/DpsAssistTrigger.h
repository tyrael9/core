#ifndef MANGOS_PARTYBOT_DPS_ASSIST_TRIGGER_H
#define MANGOS_PARTYBOT_DPS_ASSIST_TRIGGER_H

#include "Trigger.h"

#include <algorithm>
#include <cstdint>

namespace PartyBotEngine
{
// DPS target policy composed from value-only party/hostile snapshots.
//
// The trigger deliberately uses the existing generic actions in two stages:
// first select the best assist target, then on the next fresh snapshot request
// Core auto-attack for that already-selected target. It does not execute Core
// operations or retain live object pointers itself.
class DpsAssistTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }

    bool IsActive(DecisionContext const& context) const override
    {
        PartyRole const role = static_cast<PartyRole>(context.bot.roleId);
        return context.bot.online && context.bot.alive &&
            (role == PartyRole::MeleeDamage || role == PartyRole::RangedDamage);
    }

    void CollectCandidates(DecisionContext const& context,
                           std::vector<ActionCandidate>& candidates) const override
    {
        for (HostileSnapshot const& hostile : context.snapshot.hostiles)
        {
            if (!hostile.guid || !hostile.alive || hostile.crowdControlProtected ||
                hostile.guid == context.bot.guid ||
                hostile.guid == context.bot.victimGuid)
                continue;

            float const priority = GetAssistPriority(context.snapshot, hostile);
            if (priority <= 0.0f)
                continue;

            ActionCandidate candidate;
            candidate.actionName = context.bot.selectionGuid == hostile.guid
                ? "core_attack_target"
                : "core_select_hostile_target";
            candidate.targetGuid = hostile.guid;
            candidate.basePriority = priority;
            candidate.channel = ActionChannel::Targeting;
            candidates.push_back(candidate);
        }
    }

private:
    static PartyRole RoleOf(PartyMemberSnapshot const& member)
    {
        return static_cast<PartyRole>(member.roleId);
    }

    static bool IsTank(PartyMemberSnapshot const& member)
    {
        PartyRole const role = RoleOf(member);
        return member.isMainTank || role == PartyRole::Tank ||
               role == PartyRole::MainTank;
    }

    static int AssistRank(PartyMemberSnapshot const& member)
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

    static PartyMemberSnapshot const* FindBestPartyMemberTargeting(
        PartySnapshot const& snapshot, Guid hostileGuid)
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

    static PartyMemberSnapshot const* ResolvePartyVictim(
        PartySnapshot const& snapshot, HostileSnapshot const& hostile)
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

    static float GetAssistPriority(PartySnapshot const& snapshot,
                                   HostileSnapshot const& hostile)
    {
        float priority = 0.0f;
        if (PartyMemberSnapshot const* assisting =
                FindBestPartyMemberTargeting(snapshot, hostile.guid))
        {
            if (assisting->isMainTank)
                priority = 300.0f;
            else if (IsTank(*assisting))
                priority = 280.0f;
            else if (RoleOf(*assisting) == PartyRole::MeleeDamage ||
                     RoleOf(*assisting) == PartyRole::RangedDamage)
                priority = 230.0f;
            else if (RoleOf(*assisting) == PartyRole::Healer)
                priority = 160.0f;
            else if (RoleOf(*assisting) == PartyRole::Support)
                priority = 230.0f;
        }

        if (PartyMemberSnapshot const* victim = ResolvePartyVictim(snapshot, hostile))
        {
            float const peelPriority = RoleOf(*victim) == PartyRole::Healer
                ? 260.0f
                : (IsTank(*victim) ? 180.0f : 210.0f);
            priority = std::max(priority, peelPriority);
        }
        return priority;
    }

    std::string m_name = "dps_assist";
};
} // namespace PartyBotEngine

#endif
