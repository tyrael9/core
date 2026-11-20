# PartyBotEngine action queue design

## Purpose

The Engine has two related but distinct queue lifetimes:

1. A per-dispatch batch queue orders the candidates selected for the current decision report.
2. An Engine-owned pending event queue retains explicitly opted-in action requests across update ticks until they are consumed or expire.

Neither queue owns a Core object or authorizes an action to execute without fresh checks against the current map, snapshot and coordinator state.

## Per-dispatch ActionQueue

Implemented in `ActionQueue.h`.

- Capacity: 32 report entries.
- Payload: report index, finite priority and insertion ordinal only.
- Ordering: descending priority, FIFO for equal priorities.
- Overflow: fail closed; never evict an existing entry.
- Dispatch: `PartyBotEngine::ExecuteSelected` processes at most eight entries.
- Revalidation: the executor resolves registered actions and checks threat budget immediately before dispatch; Core-facing action adapters remain responsible for GUID resolution and their audited execution-time checks.

The standalone queue test covers priority, FIFO, capacity, invalid priorities and clearing. Engine-level tests prove that `ExecuteSelected` actually invokes actions in priority/FIFO order and respects the per-dispatch ceiling.

This queue is intentionally local to one dispatch and does not make deferred work persistent.

## PendingActionQueue

Implemented in `ActionQueue.h` and owned by each `PartyBotEngine`.

Each entry owns a copy of `ActionCandidate` (strings, GUIDs, spell/channel identifiers, priorities and destination scalars), plus group/map/instance/bot/encounter identity, enqueue time, expiry deadline and FIFO ordinal. No `Player*`, `Unit*`, `Map*`, action object pointer or other Core-owned address may be stored.

Current contract:

- Capacity is 32 entries; overflow is rejected without evicting queued work.
- An action is immediate-only by default. It must explicitly override `Action::GetPendingLifetimeMs()` with a positive TTL no greater than (2^31)-1 milliseconds to opt into cross-tick queueing.
- Enqueue requires a registered action, current Possible/Useful eligibility, a valid current threat-impact estimate and finite candidate data.
- Identity is action name + target GUID + spell ID + channel + optional exact destination, scoped to the same group/map/instance/bot/encounter. Different strategies that request the same concrete action do not create duplicate queue entries within that context.
- Re-enqueuing the same identity refreshes the latest payload/priority, but preserves original insertion order and expiry. It cannot keep stale work alive forever.
- Every dequeue uses the new DecisionContext. It re-runs Possible/Useful, threat-impact estimation, multipliers, priority, channel arbitration and current threat-budget assessment. Enqueue-time range, LOS, resource, cooldown or target facts are never trusted as current facts.
- A selected entry runs through the existing `ExecuteSelected` path, including the immediate reservation recheck and the registered action adapter. That adapter remains responsible for resolving the GUID against Core and checking exact live target/range/LOS/state.
- Same-channel losers and temporarily unknown/exceeded threat-budget entries may be deferred, but requeue preserves their original deadline and FIFO ordinal. Invalid, ineligible and foreign-context entries are discarded.
- Expiry uses signed uint32 subtraction; lifetimes shorter than half the clock range make comparison wrap-safe.
- `ResetDecisionState` clears the queue. Encounter boundaries, map/party/instance teardown and server shutdown reach a reset/clear path. Ordinary value/trigger invalidation intentionally preserves pending events so the next tick can revalidate them.

The queue APIs are tested but are not currently invoked by PartyBotAI's live tick loop. The existing Legacy AI remains authoritative, and Shadow remains observation-only. This integration does not enable New Engine gameplay or any Legacy fallback.

## Required integration before New Engine use

The New Engine authority loop still needs to establish these behaviors:

1. Call `QueueSelectedActions` or `QueuePendingAction` from the owning Engine tick/event loop, then call `ExecutePendingActions` with a fresh map-thread DecisionContext.
2. Add prerequisites, alternatives, continuers and action-result listeners only with concrete behavior tests. A failed prerequisite or Core execution result must not silently fall through to Legacy policy.
3. Add bounded trace records for enqueue, duplicate refresh, expiry, dequeue, revalidation, execution result and queue overflow.
4. Define starvation/fairness policy when higher-priority channel work repeatedly defers lower-priority entries.
5. Keep Core runtime adapters responsible for resolving GUIDs and verifying exact live Core range/LOS/resources/cooldowns just before the side effect.

## Test matrix

Pure tests cover:
- report-index queue priority/FIFO/capacity and Engine dispatch integration;
- pending queue copy semantics, duplicate refresh without TTL extension, latest-priority ordering, FIFO ties, expiry boundaries, uint32 clock wrap, invalid payloads and capacity rejection;
- Engine-owned enqueue with action-specific TTL opt-in;
- Possible/Useful revalidation between enqueue and dequeue;
- group/map/instance/bot/encounter context rejection;
- same-channel deferral and immutable expiry;
- reset/lifecycle clearing.

Still required:
- direct invocation from an exclusive New Engine tick loop;
- stale Core GUID/range/LOS/resource/cooldown checks in each concrete adapter;
- prerequisite/alternative/continuer/result-listener semantics;
- fairness/starvation policy and queue-specific trace records;
- Core-backed runtime scenarios for every newly integrated action path.
