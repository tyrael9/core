# PartyBotEngine action queue design

## Purpose

The Engine needs two related but distinct queue lifetimes:

1. A per-dispatch batch queue orders the candidates selected for the current decision report.
2. A pending event queue retains value-only action requests across update ticks until they are consumed or expire.

Neither queue owns a Core object or authorizes an action to execute without fresh checks against the current map, snapshot and coordinator state.

## Per-dispatch ActionQueue

Implemented in `ActionQueue.h`.

- Capacity: 32 report entries.
- Payload: report index, finite priority and insertion ordinal only.
- Ordering: descending priority, FIFO for equal priorities.
- Overflow: fail closed; never evict an existing entry.
- Dispatch: `PartyBotEngine::ExecuteSelected` processes at most eight entries.
- Revalidation: the executor still resolves registered actions and checks threat budget immediately before dispatch; Core-facing action adapters remain responsible for their audited execution-time checks.

The standalone queue test covers priority, FIFO, capacity, invalid priorities and clearing. Engine-level tests prove that `ExecuteSelected` actually invokes actions in priority/FIFO order and respects the per-dispatch ceiling.

This queue is intentionally local to one dispatch and does not make deferred work persistent.

## PendingActionQueue

Also implemented in `ActionQueue.h`, but not yet wired to `PartyBotEngine`'s tick loop.

Each entry owns a copy of `ActionCandidate` (strings, GUIDs, spell/channel identifiers, priorities and destination scalars), plus enqueue time, expiry deadline and FIFO ordinal. No `Player*`, `Unit*`, `Map*`, action object pointer or other Core-owned address may be stored.

Current contract:

- Capacity is 32 entries; overflow is rejected without evicting queued work.
- Enqueue requires a non-empty action name, finite priority, finite destination coordinates when present, and a positive TTL no greater than (2^{31}-1) milliseconds.
- Identity is action name + target GUID + spell ID + channel + optional exact destination. Different strategies that request the same concrete action do not create duplicate queue entries.
- Re-enqueuing the same identity refreshes the candidate payload and latest priority, but preserves original insertion order and expiry. Repeated triggers therefore cannot keep stale work alive forever.
- Pop/prune removes entries whose deadline has elapsed. Signed uint32 subtraction provides wrap-safe comparisons because accepted TTLs are shorter than half the uint32 clock range.
- Pop order is descending priority, FIFO for ties.

The pending queue is currently a tested storage primitive, not an active gameplay scheduler. It does not yet decide whether a pending action is still Possible or Useful, reserve threat, resolve its target, or execute the action.

## Required integration before New Engine use

The Engine-owned scheduler still needs to establish these behaviors:

1. Enqueue selected and event-triggered actions with explicit, action-specific bounded TTLs.
2. On every dequeue, resolve GUIDs on the current map and re-run Possible, Useful, channel arbitration and action-specific threat-impact/budget checks using the current snapshot. Never trust the enqueue-time target, range, LOS, resources, cooldowns or priority as current facts.
3. Preserve per-tick execution budgets. Work not dispatched remains pending only while its TTL and owning party/map/encounter lifetime remain valid; otherwise emit an explicit expired/deferred/skipped result.
4. Clear pending work on bot teardown, party/group change, map or instance transition, encounter closure where appropriate, and explicit runtime-mode shutdown.
5. Add prerequisites, alternatives, continuers and action-result listeners only with concrete behavior tests. A failed prerequisite or Core execution result must not silently fall through to Legacy policy.
6. Emit bounded trace records for enqueue, duplicate refresh, expiry, dequeue, revalidation, execution result and queue overflow.

## Test matrix

Current pure tests cover:
- report-index queue priority/FIFO/capacity and Engine dispatch integration;
- pending queue copy semantics, duplicate refresh without TTL extension, latest-priority ordering, FIFO ties, expiry boundaries, uint32 clock wrap, invalid payloads and capacity rejection.

Still required:
- Engine-owned cross-tick scheduling and teardown clearing;
- stale target/map/snapshot invalidation before execution;
- mixed old/new event channel conflicts and starvation behavior;
- prerequisite/alternative/continuer/result-listener semantics;
- Core-backed runtime scenarios for every newly integrated action path.
