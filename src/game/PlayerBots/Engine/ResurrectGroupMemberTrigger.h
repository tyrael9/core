#ifndef MANGOS_PARTYBOT_RESURRECT_GROUP_MEMBER_TRIGGER_H
#define MANGOS_PARTYBOT_RESURRECT_GROUP_MEMBER_TRIGGER_H

#include "Trigger.h"

namespace PartyBotEngine
{
// Select only same-shard Group corpses. Live castability (LOS/range/spellbook,
// pending offers, reservations and Core state) is revalidated by the action.
class ResurrectGroupMemberTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetCheckIntervalMs() const override { return 0; }

    bool IsActive(DecisionContext const& context) const override
    {
        return context.bot.online && context.bot.alive &&
            context.bot.resurrectionSpellId && !context.bot.inDuel &&
            !context.bot.inCombat && !context.bot.victimGuid &&
            !context.bot.currentSpellId;
    }

    void CollectCandidates(
        DecisionContext const& context,
        std::vector<ActionCandidate>& candidates) const override
    {
        if (!IsActive(context))
            return;

        for (PartyMemberSnapshot const& member : context.snapshot.members)
        {
            if (!member.guid || member.guid == context.bot.guid ||
                !member.online || member.alive ||
                member.deathState != PartyMemberDeathState::Corpse ||
                member.resurrectionPending ||
                member.mapId != context.snapshot.mapId ||
                member.instanceId != context.snapshot.instanceId)
                continue;

            if (context.coordinator.HasReservation(
                    ReservationType::Resurrection, member.guid, 0, context.nowMs))
                continue;

            ActionCandidate candidate;
            candidate.actionName = "core_resurrect_group_member";
            candidate.targetGuid = member.guid;
            candidate.spellId = context.bot.resurrectionSpellId;
            candidate.basePriority = 110.0f;
            candidate.channel = ActionChannel::GlobalCooldown;
            candidates.push_back(candidate);
            return; // One corpse/spell attempt per decision cycle.
        }
    }

private:
    std::string m_name = "resurrect_group_member";
};
} // namespace PartyBotEngine

#endif
