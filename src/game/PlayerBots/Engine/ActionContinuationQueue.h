#ifndef MANGOS_PARTYBOT_ACTION_CONTINUATION_QUEUE_H
#define MANGOS_PARTYBOT_ACTION_CONTINUATION_QUEUE_H

#include "ActionQueue.h"
#include "ActionContinuation.h"
#include "ActionResultListener.h"
#include "ItemUseTerminalListener.h"
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
        if (WasRecentlySettled(request, context))
            return ActionContinuationEnqueueResult::DuplicateParent;

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

        if (m_entries.size() + m_ready.size() >= Capacity)
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
                ActionDependencyPrerequisite const& prerequisite =
                    entry.request.prerequisites[i];
                if (prerequisite.source != ActionDependencyEventSource::CoreSpellTerminal ||
                    prerequisite.actionId != event.actionId ||
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

    void ObservePointMovementTerminal(PointMovementTerminalEvent const& event)
    {
        if (!event.actionId || !event.encounterId)
            return;
        for (Entry& entry : m_entries)
        {
            for (std::size_t i = 0; i < entry.request.prerequisites.size(); ++i)
            {
                if (entry.states[i] != PrerequisiteState::Waiting)
                    continue;
                ActionDependencyPrerequisite const& prerequisite =
                    entry.request.prerequisites[i];
                if (prerequisite.source != ActionDependencyEventSource::PointMovementTerminal ||
                    prerequisite.actionId != event.actionId ||
                    prerequisite.encounterId != event.encounterId)
                    continue;

                entry.states[i] =
                    prerequisite.targetGuid == event.moverGuid &&
                    prerequisite.pointId == event.pointId &&
                    prerequisite.requiredMovementStatus == event.status
                    ? PrerequisiteState::Satisfied : PrerequisiteState::Rejected;
            }
        }
    }

    void ObserveItemUseTerminal(ItemUseTerminalEvent const& event)
    {
        if (!event.itemUseId || !event.actionId || !event.encounterId)
            return;
        for (Entry& entry : m_entries)
        {
            for (std::size_t i = 0; i < entry.request.prerequisites.size(); ++i)
            {
                if (entry.states[i] != PrerequisiteState::Waiting)
                    continue;
                ActionDependencyPrerequisite const& prerequisite =
                    entry.request.prerequisites[i];
                if (prerequisite.source != ActionDependencyEventSource::ItemUseTerminal ||
                    prerequisite.actionId != event.actionId ||
                    prerequisite.encounterId != event.encounterId ||
                    prerequisite.itemUseId != event.itemUseId)
                    continue;
                entry.states[i] =
                    prerequisite.requiredItemUseStatus == event.status
                    ? PrerequisiteState::Satisfied : PrerequisiteState::Rejected;
            }
        }
    }

    void ObserveActionResult(ActionResultEvent const& event)
    {
        if (!event.actionId || !event.encounterId)
            return;
        for (Entry& entry : m_entries)
        {
            for (std::size_t i = 0; i < entry.request.prerequisites.size(); ++i)
            {
                if (entry.states[i] != PrerequisiteState::Waiting)
                    continue;
                ActionDependencyPrerequisite const& prerequisite =
                    entry.request.prerequisites[i];
                if (prerequisite.source != ActionDependencyEventSource::ActionResult ||
                    prerequisite.actionId != event.actionId ||
                    prerequisite.encounterId != event.encounterId)
                    continue;

                bool const targetMatches =
                    !prerequisite.targetGuid ||
                    prerequisite.targetGuid == event.candidate.targetGuid;
                bool const spellMatches =
                    !prerequisite.spellId ||
                    prerequisite.spellId == event.candidate.spellId;
                bool const matches =
                    prerequisite.expectedActionName == event.candidate.actionName &&
                    targetMatches && spellMatches &&
                    prerequisite.requiredActionResult == event.result &&
                    prerequisite.requiredAttempted == event.attempted;
                entry.states[i] = matches
                    ? PrerequisiteState::Satisfied : PrerequisiteState::Rejected;
            }
        }
    }

    // Collect ready one-parent dependencies and resolved alternative groups.
    // Alternative groups are atomic: no branch is released until all declared
    // branches are registered and every prerequisite in every branch resolved.
    std::size_t CollectReady(std::vector<Entry>& ready)
    {
        // Entries in m_ready have already satisfied their dependencies and
        // been tombstoned. They are retried without re-running group resolution.
        ready.insert(ready.end(), m_ready.begin(), m_ready.end());
        m_ready.clear();

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
                    RememberSettled(entry);
                    m_entries.erase(m_entries.begin() + i);
                    ++removed;
                    continue;
                }
                if (allSatisfied)
                {
                    RememberSettled(entry);
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
            // The group ID is single-use in this context, whether a
            // winner exists or every branch was rejected.
            RememberSettled(entry);
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

    // PendingActionQueue can be full when an external Core result makes a
    // continuation ready. Keep that already-settled entry in a bounded retry
    // backlog so the next Engine dispatch can release it after slots free.
    bool RestoreReady(Entry const& entry, std::uint32_t nowMs)
    {
        if (IsExpired(nowMs, entry.expiresAtMs) ||
            m_entries.size() + m_ready.size() >= Capacity)
            return false;
        m_ready.push_back(entry);
        return true;
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

        // Remember the entire alternative group before erasing it. One
        // branch expiring cancels every sibling and permanently settles the
        // group ID for this context.
        for (std::uint64_t groupId : expiredGroups)
            for (Entry const& entry : m_entries)
                if (entry.request.alternativeGroupId == groupId) {
                    RememberSettled(entry);
                    break;
                }

        // Non-alternative entries settle independently on expiry.
        for (Entry const& entry : m_entries)
            if (!entry.request.alternativeGroupId && IsExpired(nowMs, entry.expiresAtMs))
                RememberSettled(entry);

        std::size_t const previousSize = m_entries.size();
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
            [nowMs, &expiredGroups](Entry const& entry) {
                if (IsExpired(nowMs, entry.expiresAtMs))
                    return true;
                return entry.request.alternativeGroupId &&
                    std::find(expiredGroups.begin(), expiredGroups.end(),
                              entry.request.alternativeGroupId) != expiredGroups.end();
            }), m_entries.end());
        std::size_t const removedEntries = previousSize - m_entries.size();
        std::size_t const previousReadySize = m_ready.size();
        m_ready.erase(std::remove_if(m_ready.begin(), m_ready.end(),
            [nowMs](Entry const& entry) { return IsExpired(nowMs, entry.expiresAtMs); }),
            m_ready.end());
        return removedEntries + previousReadySize - m_ready.size();
    }

    std::size_t Size() const { return m_entries.size() + m_ready.size(); }
    bool Empty() const { return m_entries.empty() && m_ready.empty(); }
    void Clear()
    {
        m_entries.clear();
        m_ready.clear();
        m_recentlySettled.clear();
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
    struct SettledDependency
    {
        ActionDependencyRequest request;
        PendingActionQueue::Context context;
    };

    static std::size_t const RecentSettledCapacity = 128;

    bool WasRecentlySettled(
        ActionDependencyRequest const& request,
        PendingActionQueue::Context const& context) const
    {
        for (SettledDependency const& settled : m_recentlySettled) {
            if (!SameContext(settled.context, context))
                continue;
            if (request.alternativeGroupId) {
                if (settled.request.alternativeGroupId == request.alternativeGroupId)
                    return true;
            } else if (!settled.request.alternativeGroupId &&
                       SamePrerequisites(settled.request.prerequisites,
                                         request.prerequisites))
                return true;
        }
        return false;
    }

    void RememberSettled(Entry const& entry)
    {
        if (WasRecentlySettled(entry.request, entry.context))
            return;
        if (m_recentlySettled.size() >= RecentSettledCapacity)
            m_recentlySettled.erase(m_recentlySettled.begin());
        SettledDependency settled;
        settled.request = entry.request;
        settled.context = entry.context;
        m_recentlySettled.push_back(std::move(settled));
    }

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
        // Prerequisites are ANDed, so registration identity is a set rather
        // than a caller-selected vector order. A reordered retry is still the
        // same dependency and must not dispatch the child a second time.
        auto samePrerequisite = [](ActionTerminalPrerequisite const& a,
                                   ActionTerminalPrerequisite const& b) {
            return a.source == b.source &&
                a.actionId == b.actionId && a.encounterId == b.encounterId &&
                a.targetGuid == b.targetGuid && a.spellId == b.spellId &&
                a.requiredStatus == b.requiredStatus &&
                a.expectedActionName == b.expectedActionName &&
                a.requiredActionResult == b.requiredActionResult &&
                a.requiredAttempted == b.requiredAttempted &&
                a.pointId == b.pointId &&
                a.requiredMovementStatus == b.requiredMovementStatus &&
                a.itemUseId == b.itemUseId &&
                a.requiredItemUseStatus == b.requiredItemUseStatus;
        };
        for (ActionTerminalPrerequisite const& prerequisite : left) {
            bool found = false;
            for (ActionTerminalPrerequisite const& candidate : right)
                if (samePrerequisite(prerequisite, candidate)) {
                    found = true;
                    break;
                }
            if (!found)
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
            ActionDependencyPrerequisite const& prerequisite = request.prerequisites[i];
            if (!prerequisite.actionId || prerequisite.encounterId != context.encounterId)
                return false;

            if (prerequisite.source == ActionDependencyEventSource::CoreSpellTerminal)
            {
                if (!prerequisite.targetGuid || !prerequisite.spellId ||
                    static_cast<std::uint8_t>(prerequisite.requiredStatus) >
                        static_cast<std::uint8_t>(CoreSpellTerminalStatus::Interrupted) ||
                    !prerequisite.expectedActionName.empty())
                    return false;
            }
            else if (prerequisite.source == ActionDependencyEventSource::ActionResult)
            {
                if (prerequisite.expectedActionName.empty() ||
                    static_cast<std::uint8_t>(prerequisite.requiredActionResult) >
                        static_cast<std::uint8_t>(ActionResult::Skipped))
                    return false;
            }
            else if (prerequisite.source == ActionDependencyEventSource::PointMovementTerminal)
            {
                if (!prerequisite.targetGuid || prerequisite.spellId ||
                    !prerequisite.expectedActionName.empty() ||
                    static_cast<std::uint8_t>(prerequisite.requiredMovementStatus) >
                        static_cast<std::uint8_t>(PointMovementTerminalStatus::Failed))
                    return false;
            }
            else if (prerequisite.source == ActionDependencyEventSource::ItemUseTerminal)
            {
                if (!prerequisite.itemUseId || prerequisite.targetGuid ||
                    prerequisite.spellId || !prerequisite.expectedActionName.empty() ||
                    prerequisite.pointId ||
                    static_cast<std::uint8_t>(prerequisite.requiredItemUseStatus) >
                        static_cast<std::uint8_t>(ItemUseAggregateStatus::Expired))
                    return false;
            }
            else
                return false;

            for (std::size_t j = 0; j < i; ++j)
                if (request.prerequisites[j].source == prerequisite.source &&
                    request.prerequisites[j].actionId == prerequisite.actionId &&
                    request.prerequisites[j].itemUseId == prerequisite.itemUseId)
                    return false;
        }
        return true;
    }

    std::vector<Entry> m_entries;
    std::vector<Entry> m_ready;
    std::vector<SettledDependency> m_recentlySettled;
    std::uint64_t m_nextOrdinal = 0;
};
} // namespace PartyBotEngine

#endif
