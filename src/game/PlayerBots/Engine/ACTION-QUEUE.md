# PartyBotEngine action queue and dependency design

## Scope and current status (2026-11-20)

The generic dispatch infrastructure is implemented through F38 and is called from the explicit NewEngine decision-cycle boundary. This does **not** mean production gameplay actions are registered: there are currently no production class/role Core adapters, and the default runtime authority remains Legacy.

The design has two different queue lifetimes:

1. A per-dispatch queue orders selected candidates from the current DecisionReport.
2. An Engine-owned pending queue stores explicitly opted-in actions across update ticks until they are executed, rejected, cleared or expired.

A third, bounded continuation/dependency queue waits for typed prerequisite/result events and releases children into the normal pending queue. All retained state is value-only; no Core object pointer is stored across ticks.

## Per-dispatch ActionQueue

Implemented in `ActionQueue.h`.

- Capacity: 32 report entries.
- Payload: report index, finite priority and insertion ordinal.
- Ordering: descending priority, FIFO for equal priorities.
- Overflow: fail closed; never evict an existing entry.
- Execution: `PartyBotEngine::ExecuteSelected` invokes at most eight actions per dispatch.
- Revalidation: registered action eligibility and threat budget are rechecked before dispatch. Each Core-facing adapter must still resolve GUIDs and perform its own exact live Core state/range/LOS/resource checks.

This is a same-dispatch batch; it does not retain work across updates.

## Engine-owned PendingActionQueue

Implemented in `ActionQueue.h`, owned by each `PartyBotEngine`, and dispatched by `ExecuteDecisionCycle`.

- Capacity: 32 entries; overflow rejects new work without evicting queued entries.
- Opt-in: actions are immediate-only by default. A positive, bounded `GetPendingLifetimeMs()` is required for cross-tick queueing.
- Payload: copied ActionCandidate strings, GUIDs, spell/channel fields, priorities and destination scalars, plus group/map/instance/bot/encounter identity, enqueue time, expiry and FIFO ordinal.
- No retained Core pointers, Player*, Unit*, Map*, or action object pointer.
- Enqueue requires a registered action, current Possible/Useful eligibility and a complete valid threat-impact estimate when threat budgeting applies.
- Duplicate identity is action/target/spell/channel/destination scoped to the owner context. Refresh may update payload/priority but cannot extend the original expiry or FIFO age.
- Every dequeue uses a fresh map-thread DecisionContext and re-runs Possible/Useful, threat impact, multipliers, current priority, channel arbitration, current threat budget and owner-context checks.
- Deferred same-channel or temporarily unknown/over-budget work retains original expiry and FIFO identity. Invalid, ineligible, expired or foreign-context entries are discarded.
- Lifecycle resets on encounter boundary, map/party/instance replacement and shutdown clear pending work. Ordinary value/trigger invalidation preserves it for fresh next-tick revalidation.
- `ExecuteDecisionCycle` dispatches immediate actions first, reserves channels they consume, and shares one eight-action ceiling across immediate and pending work. Pending threat impact is accumulated across selected actions rather than assessed independently.

## Continuations and typed dependencies (F34–F38)

The bounded continuation queue contains only value data and is keyed by parent action and encounter/owner identity. A child is never executed inside a Core Spell callback; a successful match releases it into the normal PendingActionQueue for a later dispatch and fresh revalidation.

Supported prerequisite/result sources:

- `CoreSpellTerminal`: a value-only event from the root Core Spell lifecycle, correlated by action ID, encounter, target, spell and Succeeded/Failed/Interrupted status.
- `ActionResult`: the synchronous Engine dispatcher/adapter result, independently correlated and matched by action identity, optional target/spell, result and attempted flag. A Core Spell terminal cannot satisfy this source.

Dependency semantics:

- Up to four AND prerequisites per dependency.
- Bounded alternative groups with 2–8 registered branches, resolved atomically after registration and prerequisite terminal events are available; highest priority wins, FIFO breaks ties.
- Bounded recent-event ledgers support late registration and instant-cast races.
- Absolute wrap-safe TTL; when prerequisites are already terminal, lifetime is anchored to the latest terminal time.
- Bounded tombstones make resolved/rejected/expired dependency registration idempotent; AND identity is independent of prerequisite ordering, and alternative group IDs are single-use in owner context.
- When a satisfied child cannot enter a full 32-slot PendingActionQueue, F38 preserves it in a separate bounded ready backlog. Original expiry and tombstone are retained; the child transfers when capacity is free and only executes on a subsequent dispatch.

## Fairness and capacity

F33 promotes eligible pending work that has waited 3,000 ms ahead of not-yet-promoted work. Longest wait wins among promoted entries; stable FIFO breaks ties. The comparator is applied again after fresh revalidation so priority recalculation cannot undo starvation protection. Requeues preserve original enqueue time, ordinal and expiry. All capacities are bounded and fail closed.

## Remaining gaps

- Typed movement terminal results are not implemented. The legacy Core `PointMovementGenerator<Player>::Finalize` also calls `MovementInform` during forced cleanup, so that callback alone cannot prove arrival. See the F39 audit in ROADMAP.md and VMANGOS-API-AUDIT.md.
- Item-use terminal completion/cancellation semantics are unaudited.
- Production Core action adapters and uniform final pre-execution checks for target GUID/state, map/instance, range, LOS, spell, resource and cooldown are not registered.
- Queue/dependency trace events and broader production lifecycle stress tests remain to be completed where they support actual actions.

## Verification evidence

- F28 Engine-owned pending lifecycle and dequeue revalidation: trusted Push CI [#44652673352](https://github.com/tyrael9/core/actions/runs/44652673352).
- F30 NewEngine decision-cycle dispatch and cumulative pending threat: trusted Push CI [#44664709181](https://github.com/tyrael9/core/actions/runs/44664709181).
- F31/F32 result listeners and Core Spell terminal correlation: trusted Push CI [#44673953340](https://github.com/tyrael9/core/actions/runs/44673953340), [#44677767691](https://github.com/tyrael9/core/actions/runs/44677767691).
- F33 starvation resistance and F34 Core-terminal-gated continuation: [#44680955006](https://github.com/tyrael9/core/actions/runs/44680955006), [#44689339405](https://github.com/tyrael9/core/actions/runs/44689339405).
- F35 AND/alternative prerequisites, F36 mixed event sources, F37 tombstones and F38 full-queue ready backlog: [#44702889745](https://github.com/tyrael9/core/actions/runs/44702889745), [#44704096882](https://github.com/tyrael9/core/actions/runs/44704096882), [#44713147805](https://github.com/tyrael9/core/actions/runs/44713147805), [#44715114823](https://github.com/tyrael9/core/actions/runs/44715114823), [#44716873212](https://github.com/tyrael9/core/actions/runs/44716873212).

The latest full source-code CI #44716873212 passed the pure suite, report collector, Linux ARM64 Vanilla 5875 build and fixture-backed simulator Runtime PASS. PR runtime fixtures are intentionally secret-isolated; PR validation runs the pure/build path. Current docs-only head validations are not new full simulator runs.

## Operational boundary

Legacy remains the default. Shadow never executes. NewEngine is exclusive and has no hidden fallback, but should remain a development-only no-op until audited production adapters and their Core-backed acceptance tests exist. No image publication, merge or deployment is implied by queue tests or CI.