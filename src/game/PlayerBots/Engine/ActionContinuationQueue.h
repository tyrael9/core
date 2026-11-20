#ifndef MANGOS_PARTYBOT_ACTION_CONTINUATION_QUEUE_H
#define MANGOS_PARTYBOT_ACTION_CONTINUATION_QUEUE_H

#include "ActionQueue.h"
#include "ActionContinuation.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

namespace PartyBotEngine
{
class ActionContinuationQueue
{
public:
    enum class PrerequisiteState : std::uint8_t { Waiting, Satisfied, Rejected };

    struct Entry
    {
        ActionDependencyRequest request;
        std::vector<PrerequisiteState> states;
        ActionCandidate candidate;
        PendingActionQueue::Context context;
        std::uint32_t enqueuedAtMs = 0;
        std::uint32_t expiresAtMs = 0;
        std::uint64_t ordinal = 0;
    };

    static std::size_t const Capacity = 32;
    static std::size_t const MaxPrerequisites = 4;
    static std::uint8_t const MaxAlternatives = 8;
    static std::uint32_t const MaxLifetimeMs = PendingActionQueue::MaxLifetimeMs;

    ActionContinuationEnqueueResult Enqueue(
        ActionDependencyRequest const& request,
        ActionCandidate const& candidate,
        std::uint32_t nowMs,
        PendingActionQueue::Context const& context,
        std::uint32_t ttlStartMs)
    {
        if (!IsWellFormedRequest(request, candidate, context))
            return ActionContinuationEnqueueResult::Invalid;

        PruneExpired(nowMs);
        if (request.alternativeGroupId)
        {
            std::size_t branchCount = 0;
            for (Entry const& existing : m_entries)
            {
                if (existing.request.alternativeGroupId != request.alternativeGroupId)
                    continue;
                if (!SameContext(existing.context, context) ||
                    existing.request.alternativeCount != request.alternativeCount)
                    return ActionContinuationEnqueueResult::Invalid;
                if (existing.request.alternativeIndex == request.alternativeIndex)
                    return ActionContinuationEnqueueResult::DuplicateParent;
                ++branchCount;
            }
            if (branchCount >= request.alternativeCount)
                return ActionContinuationEnqueueResult::DuplicateParent;
        }
        else
        {
            for (Entry const& existing : m_entries)
                if (!existing.request.alternativeGroupId &&
                    SameContext(existing.context, context) &&
                    SamePrerequisites(existing.request.prerequisites, request.prerequisites))
                    return ActionContinuationEnqueueResult::DuplicateParent;
        }

        if (m_entries.size() >= Capacity)
            return ActionContinuationEnqueueResult::Full;

        Entry entry;
        entry.request = request;
        entry.candidate = candidate;
        entry.candidate.actionId = 0;
        entry.candidate.threatActionId = 0;
        entry.context = context;
        entry.enqueuedAtMs = ttlStartMs;
        entry.expiresAtMs = ttlStartMs + request.lifetimeMs;
        entry.ordinal = m_nextOrdinal++;
        entry.states.assign(request.prerequisites.size(), PrerequisiteState::Waiting);
        m_entries.push_back(std::move(entry));
        return ActionContinuationEnqueueResult::Queued;
    }

    void ObserveTerminal(CoreSpellTerminalEvent const& event)
    {
        if (!event.actionId || !event.encounterId)
            return;
        for (Entry& entry : m_entries)
        {
            for (std::size_t i = 0; i < entry.request.prerequisites.size(); ++i)
            {
                if (entry.states[i] != PrerequisiteState::Waiting)
                    continue;
                ActionTerminalPrerequisite const& prerequisite =
                    entry.request.prerequisites[i];
                if (prerequisite.actionId != event.actionId ||
                    prerequisite.encounterId != event.encounterId)
                    continue;

                entry.states[i] =
                    prerequisite.targetGuid == event.targetGuid &&
                    prerequisite.spellId == event.spellId &&
                    prerequisite.requiredStatus == event.status
                    ? PrerequisiteState::Satisfied
                    : PrerequisiteState::Rejected;
            }
        }
    }

    // Collect ready one-parent dependencies and resolved alternative groups.
    // Alternative groups are atomic: no branch is released until all declared
    // branches are registered and every prerequisite in every branch resolved.
    std::size_t CollectReady(std::vector<Entry>& ready)
    {
        std::vector<std::uint64_t> visitedGroups;
        std::size_t removed = 0;
        for (std::size_t i = 0; i < m_entries.size();)
        {
            Entry const& entry = m_entries[i];
            std::uint64_t const groupId = entry.request.alternativeGroupId;
            if (!groupId)
            {
                bool rejected = false;
                bool allSatisfied = true;
                for (PrerequisiteState state : entry.states)
                {
                    rejected = rejected || state == PrerequisiteState::Rejected;
                    allSatisfied = allSatisfied && state == PrerequisiteState::Satisfied;
                }
                if (rejected)
                {
                    m_entries.erase(m_entries.begin() + i);
                    ++removed;
                    continue;
                }
                if (allSatisfied)
                {
                    ready.push_back(entry);
                    m_entries.erase(m_entries.begin() + i);
                    ++removed;
                    continue;
                }
                ++i;
                continue;
            }

            if (std::find(visitedGroups.begin(), visitedGroups.end(), groupId) !=
                visitedGroups.end())
            {
                ++i;
                continue;
            }
            visitedGroups.push_back(groupId);

            std::vector<std::size_t> groupIndices;
            bool allResolved = true;
            for (std::size_t j = 0; j < m_entries.size(); ++j)
            {
                Entry const& branch = m_entries[j];
                if (branch.request.alternativeGroupId != groupId)
                    continue;
                groupIndices.push_back(j);
                for (PrerequisiteState state : branch.states)
                    if (state == PrerequisiteState::Waiting)
                        allResolved = false;
            }
            std::uint8_t const requiredBranches = entry.request.alternativeCount;
            if (groupIndices.size() != requiredBranches || !allResolved)
            {
                ++i;
                continue;
            }

            std::size_t winner = m_entries.size();
            for (std::size_t index : groupIndices)
            {
                Entry const& branch = m_entries[index];
                bool const eligible = std::all_of(
                    branch.states.begin(), branch.states.end(),
                    [](PrerequisiteState state) { return state == PrerequisiteState::Satisfied; });
                if (!eligible)
                    continue;
                if (winner == m_entries.size() ||
                    branch.candidate.finalPriority > m_entries[winner].candidate.finalPriority ||
                    (branch.candidate.finalPriority == m_entries[winner].candidate.finalPriority &&
                     branch.ordinal < m_entries[winner].ordinal))
                    winner = index;
            }
            if (winner != m_entries.size())
                ready.push_back(m_entries[winner]);

            // Once every prerequisite is terminal, the group is settled even
            // when no branch is eligible. Remove every alternative atomically.
            std::sort(groupIndices.begin(), groupIndices.end(),
                [](std::size_t left, std::size_t right) { return left > right; });
            for (std::size_t index : groupIndices)
            {
                m_entries.erase(m_entries.begin() + index);
                ++removed;
            }
            // Erasure may have removed the current index; restart from the head.
            i = 0;
        }
        return removed;
    }

    std::size_t PruneExpired(std::uint32_t nowMs)
    {
        std::vector<std::uint64_t> expiredGroups;
        for (Entry const& entry : m_entries)
            if (entry.request.alternativeGroupId &&
                IsExpired(nowMs, entry.expiresAtMs) &&
                std::find(expiredGroups.begin(), expiredGroups.end(),
                          entry.request.alternativeGroupId) == expiredGroups.end())
                expiredGroups.push_back(entry.request.alternativeGroupId);

        std::size_t const previousSize = m_entries.size();
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [nowMs, &expiredGroups](Entry const& entry) {
                if (IsExpired(nowMs, entry.expiresAtMs))
                    return true;
                return entry.request.alternativeGroupId &&
                    std::find(expiredGroups.begin(), expiredGroups.end(),
                              entry.request.alternativeGroupId) != expiredGroups.end();
            }), m_entries.end());
        return previousSize - m_entries.size();
    }

    std::size_t Size() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }
    void Clear()
    {
        m_entries.clear();
        m_nextOrdinal = 0;
    }

    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs)
    {
        return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
    }

    static bool IsValidRequest(
        ActionDependencyRequest const& request,
        ActionCandidate const& candidate,
        PendingActionQueue::Context const& context)
    {
        return IsWellFormedRequest(request, candidate, context);
    }

private:
    static bool SameContext(PendingActionQueue::Context const& left,
                            PendingActionQueue::Context const& right)
    {
        return left.scoped == right.scoped &&
            (!left.scoped ||
                (left.groupId == right.groupId &&
                 left.mapId == right.mapId &&
                 left.instanceId == right.instanceId &&
                 left.botGuid == right.botGuid &&
                 left.encounterId == right.encounterId &&
                 left.encounterActive == right.encounterActive));
    }

    static bool SamePrerequisites(
        std::vector<ActionTerminalPrerequisite> const& left,
        std::vector<ActionTerminalPrerequisite> const& right)
    {
        if (left.size() != right.size())
            return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            ActionTerminalPrerequisite const& a = left[i];
            ActionTerminalPrerequisite const& b = right[i];
            if (a.actionId != b.actionId || a.encounterId != b.encounterId ||
                a.targetGuid != b.targetGuid || a.spellId != b.spellId ||
                a.requiredStatus != b.requiredStatus)
                return false;
        }
        return true;
    }

    static bool IsWellFormedRequest(
        ActionDependencyRequest const& request,
        ActionCandidate const& candidate,
        PendingActionQueue::Context const& context)
    {
        if (request.prerequisites.empty() ||
            request.prerequisites.size() > MaxPrerequisites ||
            candidate.actionName.empty() ||
            !std::isfinite(candidate.basePriority) ||
            !std::isfinite(candidate.finalPriority) ||
            !request.lifetimeMs || request.lifetimeMs > MaxLifetimeMs ||
            !context.scoped || !context.encounterActive || !context.encounterId ||
            (candidate.hasDestination &&
                (!std::isfinite(candidate.destinationX) ||
                 !std::isfinite(candidate.destinationY) ||
                 !std::isfinite(candidate.destinationZ))))
            return false;

        if (!request.alternativeGroupId)
        {
            if (request.alternativeIndex || request.alternativeCount)
                return false;
        }
        else if (request.alternativeCount < 2 ||
                 request.alternativeCount > MaxAlternatives ||
                 request.alternativeIndex >= request.alternativeCount)
            return false;

        for (std::size_t i = 0; i < request.prerequisites.size(); ++i)
        {
            ActionTerminalPrerequisite const& prerequisite = request.prerequisites[i];
            if (!prerequisite.actionId || prerequisite.encounterId != context.encounterId ||
                !prerequisite.targetGuid || !prerequisite.spellId ||
                static_cast<std::uint8_t>(prerequisite.requiredStatus) >
                    static_cast<std::uint8_t>(CoreSpellTerminalStatus::Interrupted))
                return false;
            for (std::size_t j = 0; j < i; ++j)
                if (request.prerequisites[j].actionId == prerequisite.actionId)
                    return false;
        }
        return true;
    }

    std::vector<Entry> m_entries;
    std::uint64_t m_nextOrdinal = 0;
};
} // namespace PartyBotEngine

#endif
