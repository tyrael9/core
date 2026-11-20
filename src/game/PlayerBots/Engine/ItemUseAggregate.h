#ifndef MANGOS_PARTYBOT_ITEM_USE_AGGREGATE_H
#define MANGOS_PARTYBOT_ITEM_USE_AGGREGATE_H

#include "DecisionTypes.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PartyBotEngine
{
using ItemUseId = std::uint64_t;
using ItemUseChildId = std::uint64_t;

enum : std::size_t { ItemUseMaxSpellSlots = 5 };

enum class ItemUseChildStatus : std::uint8_t
{
    Pending,
    Succeeded,
    Failed,
    Interrupted
};

enum class ItemUseAggregateStatus : std::uint8_t
{
    AllSucceeded,
    PartialSuccess,
    AllFailed,
    AllInterrupted,
    NoChildren,
    Expired
};

enum class ItemUseMutationKind : std::uint8_t
{
    ChargeChanged,
    CountChanged,
    ItemDestroyed,
    ReagentConsumed
};

enum class ItemUseMutationPhase : std::uint8_t
{
    BeforeEffects,
    DuringCast,
    AfterEffects,
    Unknown
};

enum class ItemUseTrackerResult : std::uint8_t
{
    Accepted,
    Completed,
    Invalid,
    Full,
    NotFound,
    AlreadySettled,
    Duplicate,
    SlotAlreadyReported,
    ChildCapacityReached,
    MutationQueueFull
};

// This is a value-only description captured on the actor's map thread.
// 0xff denotes a caller that cannot supply the packet's requested spell slot.
struct ItemUseRequestDescriptor
{
    ActionId actionId = 0;
    std::uint64_t encounterId = 0;
    Guid actorGuid = 0;
    Guid itemGuid = 0;
    std::uint32_t itemEntry = 0;
    std::uint8_t bagIndex = 0xff;
    std::uint8_t itemSlot = 0xff;
    std::uint8_t requestedSpellSlot = 0xff;
    std::uint32_t itemCountBefore = 0;
    std::uint32_t startedAtMs = 0;
    std::uint32_t lifetimeMs = 0;
};

struct ItemUseChildRecord
{
    ItemUseChildId childId = 0;
    std::uint8_t slotIndex = 0xff;
    std::uint32_t spellId = 0;
    bool triggered = false;
    ItemUseChildStatus status = ItemUseChildStatus::Pending;
    std::uint32_t terminalAtMs = 0;
};

struct ItemUseTerminalEvent
{
    ItemUseId itemUseId = 0;
    ActionId actionId = 0;
    std::uint64_t encounterId = 0;
    Guid actorGuid = 0;
    Guid itemGuid = 0;
    std::uint32_t itemEntry = 0;
    std::uint32_t occurredAtMs = 0;
    ItemUseAggregateStatus status = ItemUseAggregateStatus::Expired;
    std::uint8_t childCount = 0;
    std::uint8_t completedChildCount = 0;
    std::uint8_t succeededChildCount = 0;
    std::uint8_t failedChildCount = 0;
    std::uint8_t interruptedChildCount = 0;
    std::uint8_t skippedSlotCount = 0;
    std::uint8_t observedMutationCount = 0;
    std::uint8_t droppedMutationCount = 0;
    std::array<ItemUseChildRecord, ItemUseMaxSpellSlots> children;
};

// Mutation facts are independent of Spell terminal status. They intentionally
// carry no Core object pointers and may be drained separately from root results.
struct ItemUseMutationEvent
{
    ItemUseId itemUseId = 0;
    ItemUseChildId childId = 0;
    Guid itemGuid = 0;
    std::uint32_t itemEntry = 0;
    ItemUseMutationKind kind = ItemUseMutationKind::CountChanged;
    ItemUseMutationPhase phase = ItemUseMutationPhase::Unknown;
    std::uint8_t slotIndex = 0xff;
    std::uint32_t beforeCount = 0;
    std::uint32_t afterCount = 0;
    std::int32_t beforeCharges = 0;
    std::int32_t afterCharges = 0;
    bool itemExistsAfter = true;
    std::uint32_t occurredAtMs = 0;
};

class ItemUseAggregateTracker
{
public:
    enum : std::size_t
    {
        Capacity = 32,
        MaxSpellSlots = ItemUseMaxSpellSlots,
        MutationQueueCapacity = 256,
        RecentSettledCapacity = 64
    };
    static std::uint32_t const MaxLifetimeMs = 30000;
    static std::uint8_t const UnknownSlot = 0xff;

    ItemUseAggregateTracker()
    {
        m_roots.reserve(Capacity);
        m_mutations.reserve(MutationQueueCapacity);
    }

    ItemUseTrackerResult Begin(
        ItemUseRequestDescriptor const& request, ItemUseId& itemUseId)
    {
        itemUseId = 0;
        if (!request.actorGuid || !request.itemGuid || !request.itemEntry ||
            !request.lifetimeMs || request.lifetimeMs > MaxLifetimeMs ||
            (request.requestedSpellSlot != UnknownSlot &&
             request.requestedSpellSlot >= MaxSpellSlots))
            return ItemUseTrackerResult::Invalid;
        if (m_roots.size() >= Capacity)
            return ItemUseTrackerResult::Full;

        ItemUseId const id = AllocateRootId();
        if (!id)
            return ItemUseTrackerResult::Full;

        Root root;
        root.id = id;
        root.request = request;
        root.expiresAtMs = request.startedAtMs + request.lifetimeMs;
        m_roots.push_back(root);
        itemUseId = id;
        return ItemUseTrackerResult::Accepted;
    }

    ItemUseTrackerResult RegisterChild(
        ItemUseId itemUseId, std::uint8_t slotIndex, std::uint32_t spellId,
        bool triggered, ItemUseChildId& childId)
    {
        childId = 0;
        Root* root = FindRoot(itemUseId);
        if (!root)
            return WasRecentlySettled(itemUseId)
                ? ItemUseTrackerResult::AlreadySettled : ItemUseTrackerResult::NotFound;
        if (root->dispatchClosed || slotIndex >= MaxSpellSlots || !spellId)
            return ItemUseTrackerResult::Invalid;
        if (root->slotMask & static_cast<std::uint8_t>(1u << slotIndex))
            return ItemUseTrackerResult::SlotAlreadyReported;
        if (root->childCount >= MaxSpellSlots)
            return ItemUseTrackerResult::ChildCapacityReached;

        ItemUseChildId const id = AllocateChildId();
        if (!id)
            return ItemUseTrackerResult::Full;
        ItemUseChildRecord& child = root->children[root->childCount++];
        child.childId = id;
        child.slotIndex = slotIndex;
        child.spellId = spellId;
        child.triggered = triggered;
        child.status = ItemUseChildStatus::Pending;
        child.terminalAtMs = 0;
        root->slotMask |= static_cast<std::uint8_t>(1u << slotIndex);
        childId = id;
        return ItemUseTrackerResult::Accepted;
    }

    // Empty, non-ON_USE and invalid SpellEntry slots are skipped slots, not
    // failed child Spells. Each prototype slot may be reported exactly once.
    ItemUseTrackerResult MarkSlotSkipped(ItemUseId itemUseId, std::uint8_t slotIndex)
    {
        Root* root = FindRoot(itemUseId);
        if (!root)
            return WasRecentlySettled(itemUseId)
                ? ItemUseTrackerResult::AlreadySettled : ItemUseTrackerResult::NotFound;
        if (root->dispatchClosed || slotIndex >= MaxSpellSlots)
            return ItemUseTrackerResult::Invalid;
        if (root->slotMask & static_cast<std::uint8_t>(1u << slotIndex))
            return ItemUseTrackerResult::SlotAlreadyReported;
        root->slotMask |= static_cast<std::uint8_t>(1u << slotIndex);
        Increment(root->skippedSlotCount);
        return ItemUseTrackerResult::Accepted;
    }

    // Called only after Core's ON_USE loop has processed every prototype slot.
    // A child may already have reached a terminal synchronously inside prepare().
    ItemUseTrackerResult CloseDispatch(
        ItemUseId itemUseId, std::uint32_t nowMs, ItemUseTerminalEvent& terminal)
    {
        Root* root = FindRoot(itemUseId);
        if (!root)
            return WasRecentlySettled(itemUseId)
                ? ItemUseTrackerResult::AlreadySettled : ItemUseTrackerResult::NotFound;
        if (root->dispatchClosed)
            return ItemUseTrackerResult::Duplicate;
        root->dispatchClosed = true;
        if (IsExpired(nowMs, root->expiresAtMs)) {
            ItemUseId const expiredId = root->id;
            BuildTerminal(*root, nowMs, ItemUseAggregateStatus::Expired, terminal);
            RemoveRoot(expiredId);
            return ItemUseTrackerResult::Completed;
        }
        if (TryFinalize(*root, nowMs, terminal))
            return ItemUseTrackerResult::Completed;
        return ItemUseTrackerResult::Accepted;
    }

    // Core Spell::finish/cancel is per child, never per item-use root. Results
    // arriving before CloseDispatch are buffered until fan-out has closed.
    ItemUseTrackerResult RecordChildTerminal(
        ItemUseId itemUseId, ItemUseChildId childId, ItemUseChildStatus status,
        std::uint32_t nowMs, ItemUseTerminalEvent& terminal)
    {
        Root* root = FindRoot(itemUseId);
        if (!root)
            return WasRecentlySettled(itemUseId)
                ? ItemUseTrackerResult::AlreadySettled : ItemUseTrackerResult::NotFound;
        if (!childId || status == ItemUseChildStatus::Pending ||
            static_cast<std::uint8_t>(status) >
                static_cast<std::uint8_t>(ItemUseChildStatus::Interrupted))
            return ItemUseTrackerResult::Invalid;

        ItemUseChildRecord* child = FindChild(*root, childId);
        if (!child)
            return ItemUseTrackerResult::NotFound;
        if (child->status != ItemUseChildStatus::Pending)
            return ItemUseTrackerResult::Duplicate;
        child->status = status;
        child->terminalAtMs = nowMs;
        if (IsExpired(nowMs, root->expiresAtMs)) {
            ItemUseId const expiredId = root->id;
            BuildTerminal(*root, nowMs, ItemUseAggregateStatus::Expired, terminal);
            RemoveRoot(expiredId);
            return ItemUseTrackerResult::Completed;
        }
        if (TryFinalize(*root, nowMs, terminal))
            return ItemUseTrackerResult::Completed;
        return ItemUseTrackerResult::Accepted;
    }

    // A mutation can be the cast item itself or a reagent destroyed by the
    // spell. The item GUID/entry on the event therefore need not equal the root.
    ItemUseTrackerResult RecordMutation(ItemUseMutationEvent const& event)
    {
        Root* root = FindRoot(event.itemUseId);
        if (!root)
            return WasRecentlySettled(event.itemUseId)
                ? ItemUseTrackerResult::AlreadySettled : ItemUseTrackerResult::NotFound;
        if ((!event.itemGuid && event.kind != ItemUseMutationKind::ReagentConsumed) ||
            !event.itemEntry ||
            static_cast<std::uint8_t>(event.kind) >
                static_cast<std::uint8_t>(ItemUseMutationKind::ReagentConsumed) ||
            static_cast<std::uint8_t>(event.phase) >
                static_cast<std::uint8_t>(ItemUseMutationPhase::Unknown))
            return ItemUseTrackerResult::Invalid;
        if (event.childId && !FindChild(*root, event.childId))
            return ItemUseTrackerResult::NotFound;

        Increment(root->observedMutationCount);
        if (m_mutations.size() >= MutationQueueCapacity) {
            Increment(root->droppedMutationCount);
            Increment(m_droppedMutationEvents);
            return ItemUseTrackerResult::MutationQueueFull;
        }
        m_mutations.push_back(event);
        return ItemUseTrackerResult::Accepted;
    }

    bool PopMutationEvent(ItemUseMutationEvent& event)
    {
        if (m_mutations.empty())
            return false;
        event = m_mutations.front();
        m_mutations.erase(m_mutations.begin());
        return true;
    }

    std::size_t PruneExpired(
        std::uint32_t nowMs, std::vector<ItemUseTerminalEvent>& expired)
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i < m_roots.size();) {
            Root& root = m_roots[i];
            if (!IsExpired(nowMs, root.expiresAtMs)) {
                ++i;
                continue;
            }
            ItemUseTerminalEvent event;
            BuildTerminal(root, nowMs, ItemUseAggregateStatus::Expired, event);
            expired.push_back(event);
            RememberSettled(root.id);
            m_roots.erase(m_roots.begin() + i);
            ++count;
        }
        return count;
    }

    std::size_t ExpireAll(
        std::uint32_t nowMs, std::vector<ItemUseTerminalEvent>& expired)
    {
        std::size_t count = 0;
        for (Root const& root : m_roots) {
            ItemUseTerminalEvent event;
            BuildTerminal(root, nowMs, ItemUseAggregateStatus::Expired, event);
            expired.push_back(event);
            RememberSettled(root.id);
            ++count;
        }
        m_roots.clear();
        return count;
    }

    void Clear()
    {
        m_roots.clear();
        m_mutations.clear();
        m_recentlySettled.fill(0);
        m_nextSettledSlot = 0;
        m_droppedMutationEvents = 0;
    }

    std::size_t GetPendingRootCount() const { return m_roots.size(); }
    std::size_t GetQueuedMutationCount() const { return m_mutations.size(); }
    std::uint32_t GetDroppedMutationEventCount() const { return m_droppedMutationEvents; }

    static bool IsExpired(std::uint32_t nowMs, std::uint32_t expiresAtMs)
    {
        return static_cast<std::int32_t>(nowMs - expiresAtMs) >= 0;
    }

private:
    struct Root
    {
        ItemUseId id = 0;
        ItemUseRequestDescriptor request;
        std::uint32_t expiresAtMs = 0;
        std::uint8_t childCount = 0;
        std::uint8_t skippedSlotCount = 0;
        std::uint8_t observedMutationCount = 0;
        std::uint8_t droppedMutationCount = 0;
        std::uint8_t slotMask = 0;
        bool dispatchClosed = false;
        std::array<ItemUseChildRecord, MaxSpellSlots> children;
    };

    static void Increment(std::uint8_t& value)
    {
        if (value != 0xff)
            ++value;
    }

    static void Increment(std::uint32_t& value)
    {
        if (value != 0xffffffffu)
            ++value;
    }

    Root* FindRoot(ItemUseId id)
    {
        for (Root& root : m_roots)
            if (root.id == id)
                return &root;
        return nullptr;
    }

    ItemUseChildRecord* FindChild(Root& root, ItemUseChildId id)
    {
        for (std::size_t i = 0; i < root.childCount; ++i)
            if (root.children[i].childId == id)
                return &root.children[i];
        return nullptr;
    }

    bool WasRecentlySettled(ItemUseId id) const
    {
        if (!id)
            return false;
        for (ItemUseId settled : m_recentlySettled)
            if (settled == id)
                return true;
        return false;
    }

    void RememberSettled(ItemUseId id)
    {
        m_recentlySettled[m_nextSettledSlot] = id;
        m_nextSettledSlot = (m_nextSettledSlot + 1) % RecentSettledCapacity;
    }

    ItemUseId AllocateRootId()
    {
        for (std::size_t attempt = 0;
             attempt < Capacity + RecentSettledCapacity + 2; ++attempt) {
            ItemUseId id = m_nextRootId++;
            if (!m_nextRootId)
                m_nextRootId = 1;
            if (id && !FindRoot(id) && !WasRecentlySettled(id))
                return id;
        }
        return 0;
    }

    ItemUseChildId AllocateChildId()
    {
        for (std::size_t attempt = 0;
             attempt < Capacity * MaxSpellSlots + 2; ++attempt) {
            ItemUseChildId id = m_nextChildId++;
            if (!m_nextChildId)
                m_nextChildId = 1;
            if (!id)
                continue;
            bool used = false;
            for (Root const& root : m_roots)
                for (std::size_t i = 0; i < root.childCount; ++i)
                    if (root.children[i].childId == id) {
                        used = true;
                        break;
                    }
            if (!used)
                return id;
        }
        return 0;
    }

    static void BuildTerminal(
        Root const& root, std::uint32_t nowMs,
        ItemUseAggregateStatus status, ItemUseTerminalEvent& event)
    {
        event = ItemUseTerminalEvent();
        event.itemUseId = root.id;
        event.actionId = root.request.actionId;
        event.encounterId = root.request.encounterId;
        event.actorGuid = root.request.actorGuid;
        event.itemGuid = root.request.itemGuid;
        event.itemEntry = root.request.itemEntry;
        event.occurredAtMs = nowMs;
        event.status = status;
        event.childCount = root.childCount;
        event.skippedSlotCount = root.skippedSlotCount;
        event.observedMutationCount = root.observedMutationCount;
        event.droppedMutationCount = root.droppedMutationCount;
        for (std::size_t i = 0; i < root.childCount; ++i) {
            event.children[i] = root.children[i];
            switch (root.children[i].status) {
                case ItemUseChildStatus::Succeeded:
                    Increment(event.succeededChildCount);
                    break;
                case ItemUseChildStatus::Failed:
                    Increment(event.failedChildCount);
                    break;
                case ItemUseChildStatus::Interrupted:
                    Increment(event.interruptedChildCount);
                    break;
                case ItemUseChildStatus::Pending:
                    break;
            }
        }
        event.completedChildCount = static_cast<std::uint8_t>(
            event.succeededChildCount + event.failedChildCount +
            event.interruptedChildCount);
    }

    bool TryFinalize(
        Root& root, std::uint32_t nowMs, ItemUseTerminalEvent& event)
    {
        if (!root.dispatchClosed)
            return false;
        if (!root.childCount) {
            BuildTerminal(root, nowMs, ItemUseAggregateStatus::NoChildren, event);
            RemoveRoot(root.id);
            return true;
        }
        for (std::size_t i = 0; i < root.childCount; ++i)
            if (root.children[i].status == ItemUseChildStatus::Pending)
                return false;

        std::uint8_t succeeded = 0;
        std::uint8_t interrupted = 0;
        for (std::size_t i = 0; i < root.childCount; ++i) {
            if (root.children[i].status == ItemUseChildStatus::Succeeded)
                Increment(succeeded);
            else if (root.children[i].status == ItemUseChildStatus::Interrupted)
                Increment(interrupted);
        }
        ItemUseAggregateStatus status = ItemUseAggregateStatus::AllFailed;
        if (succeeded == root.childCount)
            status = ItemUseAggregateStatus::AllSucceeded;
        else if (succeeded)
            status = ItemUseAggregateStatus::PartialSuccess;
        else if (interrupted == root.childCount)
            status = ItemUseAggregateStatus::AllInterrupted;

        BuildTerminal(root, nowMs, status, event);
        RemoveRoot(root.id);
        return true;
    }

    void RemoveRoot(ItemUseId id)
    {
        for (std::size_t i = 0; i < m_roots.size(); ++i)
            if (m_roots[i].id == id) {
                RememberSettled(id);
                m_roots.erase(m_roots.begin() + i);
                return;
            }
    }

    std::vector<Root> m_roots;
    std::vector<ItemUseMutationEvent> m_mutations;
    std::array<ItemUseId, RecentSettledCapacity> m_recentlySettled{{0}};
    std::size_t m_nextSettledSlot = 0;
    ItemUseId m_nextRootId = 1;
    ItemUseChildId m_nextChildId = 1;
    std::uint32_t m_droppedMutationEvents = 0;
};
} // namespace PartyBotEngine

#endif
