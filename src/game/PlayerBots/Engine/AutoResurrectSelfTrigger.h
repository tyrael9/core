#ifndef MANGOS_PARTYBOT_AUTO_RESURRECT_SELF_TRIGGER_H
#define MANGOS_PARTYBOT_AUTO_RESURRECT_SELF_TRIGGER_H

#include "Trigger.h"
#include "ResurrectionPolicy.h"

namespace PartyBotEngine
{
class AutoResurrectSelfTrigger final : public Trigger
{
public:
    std::string const& GetName() const override { return m_name; }
    std::uint32_t GetCheckIntervalMs() const override { return 0; }

    bool IsActive(DecisionContext const& context) const override
    {
        return ShouldAutoResurrectSelf(context.snapshot, context.bot);
    }

    void CollectCandidates(
        DecisionContext const& context,
        std::vector<ActionCandidate>& candidates) const override
    {
        if (!IsActive(context))
            return;

        ActionCandidate candidate;
        candidate.actionName = "core_resurrect_self";
        candidate.basePriority = 100.0f;
        candidate.channel = ActionChannel::Utility;
        candidates.push_back(candidate);
    }

private:
    std::string m_name = "auto_resurrect_self";
};
} // namespace PartyBotEngine

#endif
