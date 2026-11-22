# PartyBotEngine action queue and dependency design

## Scope and current status (2026-11-21)

The bounded dispatch/dependency mechanics are implemented through F38; F39 adds movement terminals; F40 audits item-use source behavior; F41 adds the map-thread CoreAction resolver; F42 implements bounded item-use root/child/mutation aggregation; F43 adds mandatory action-specific preflight/structured results and fifteen generic adapters covering target maintenance, follow/facing, attack, movement, stop/cleanup and dismount. This does **not** mean production gameplay actions are registered: all fifteen are deliberately unregistered, no class/role adapters are enabled, and Legacy remains default.

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

## Core-facing action boundary (F41)

Production actions that mutate Core state must derive from `CoreAction`. Its final `Execute` wrapper rejects a missing action ID or resolver, asks the current map-thread `CoreActionRuntime` to resolve the bot and candidate target, and refuses to enter `ExecuteCore` unless actor/map and any required target are resolved. PartyBotAI's resolver checks current actor identity, world/map ownership, snapshot map and instance, encounter scope and target presence on that same map.

The resolved `Player*`, `Unit*` and `Map*` are synchronous borrowed pointers. They may be used only inside `ExecuteCore`; never copy them into a candidate, snapshot, pending entry, continuation, observer event or member retained after return. Pending actions carry GUID/scalar identity and resolve again at dequeue/execution time.

F41 is only the common identity/lifetime boundary. Each action still owns its last-moment checks for target alive/hostile/friendly state, spell ownership and cast result, range, LOS, power/items, cooldown/GCD and movement/control state. Those checks must be adjacent to the Core side effect. The production action registry remains empty until each adapter has an acceptance/rejection fixture.

## Continuations and typed dependencies (F34–F38)

The bounded continuation queue contains only value data and is keyed by parent action and encounter/owner identity. A child is never executed inside a Core Spell callback; a successful match releases it into the normal PendingActionQueue for a later dispatch and fresh revalidation.

Supported prerequisite/result sources:

- `CoreSpellTerminal`: a value-only event from the root Core Spell lifecycle, correlated by action ID, encounter, target, spell and Succeeded/Failed/Interrupted status.
- `ActionResult`: the synchronous Engine dispatcher/adapter result, independently correlated and matched by action identity, optional target/spell, result and attempted flag. A Core Spell terminal cannot satisfy this source.
- `PointMovementTerminal`: a value-only terminal from the opt-in Engine Player point-movement path, correlated by action, encounter, mover GUID and point ID, with Reached/Interrupted/Failed status.

F42 adds an `ItemUseTerminal` aggregate source. One root request is correlated with each valid ON_USE child Spell; dispatch must close before synchronous child terminals can settle the root. Charge/count mutation remains a separate event stream and never implies child success. The root terminal can satisfy a typed continuation prerequisite by `itemUseId` and aggregate status. See `ITEM-USE-AGGREGATE.md` for the implemented contract.

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

- Player point-movement terminal results are implemented by F39; only the explicit `MovePointForEngine` path emits them. Legacy `MovePoint`/`MovementInform` behavior remains unchanged. `Reached` requires natural spline completion and destination tolerance; forced cleanup is `Interrupted`, and natural completion outside tolerance is `Failed`.
- F42 item-use root/child/mutation aggregation, including the two-ON_USE-slot prototype and real delayed Brimstone/Fireball cancellation before/after charge mutation, is closed by trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749). The log verifies pre-mutation `-5>-5`, post-mutation `-5>-4`, Interrupted child terminals, mutation-before-effects, unchanged target health, cleanup and final Runtime PASS.
- F43's common CoreAction preflight and structured execution-result boundary are implemented. SelectHostileTargetAction uses live Core CanAttack to accept hostile and reject stale-snapshot friendly targets without selection/target side effects. ClearIdleTargetAction clears a stale selection only when the live Player is idle. StopUnattackableTargetAction uses a value-only victim GUID/attackability snapshot, live map-thread revalidation and AttackStop-only cleanup; its trusted fixture rejects stale negative snapshots and verifies attacker-reference cleanup with combat flag preserved. PR [#44849541737](https://github.com/tyrael9/core/actions/runs/44849541737) and trusted runtime [#44849535626](https://github.com/tyrael9/core/actions/runs/44849535626) passed; the runtime logs `registered=1 attackableNotProposed=1 staleSelected=1 staleRejected=1 liveUnattackable=1 selected=1 stoppedOnly=1` and final Runtime PASS.
- FollowPartyMemberAction adds a GUID-only current-follow target fact and calls native Core MoveFollow only after live same-Group/map/instance/friendly/alive/control/cast checks. Same-target requests are Useless, only active Follow or idle/default movement can be retargeted, and stale live-Group membership is rejected. PR [#44860033826](https://github.com/tyrael9/core/actions/runs/44860033826) and trusted push [#44860028963](https://github.com/tyrael9/core/actions/runs/44860028963) passed; the trusted fixture marker records first start, target snapshot, same-target no-op, retarget and stale roster rejection, followed by final Runtime PASS. Artifact: [partybot-simulator-f730018](https://github.com/tyrael9/core/actions/runs/44860028963/artifacts/14689004218).
- All fifteen generic actions are deliberately unregistered. The newest StopChaseAction has action-specific live preflight and a real-Core accepted/rejected/cleanup fixture; its trusted-runtime gate is pending. Future adapters still need bounded threat estimates/fail-closed Unknown, live preflight and exact accepted/rejected/terminal Core matrices. Class/role adapters remain future work.

## Verification evidence

- F28 Engine-owned pending lifecycle and dequeue revalidation: trusted Push CI [#44652673352](https://github.com/tyrael9/core/actions/runs/44652673352).
- F30 NewEngine decision-cycle dispatch and cumulative pending threat: trusted Push CI [#44664709181](https://github.com/tyrael9/core/actions/runs/44664709181).
- F31/F32 result listeners and Core Spell terminal correlation: trusted Push CI [#44673953340](https://github.com/tyrael9/core/actions/runs/44673953340), [#44677767691](https://github.com/tyrael9/core/actions/runs/44677767691).
- F33 starvation resistance and F34 Core-terminal-gated continuation: [#44680955006](https://github.com/tyrael9/core/actions/runs/44680955006), [#44689339405](https://github.com/tyrael9/core/actions/runs/44689339405).
- F35 AND/alternative prerequisites, F36 mixed event sources, F37 tombstones and F38 full-queue ready backlog: [#44702889745](https://github.com/tyrael9/core/actions/runs/44702889745), [#44704096882](https://github.com/tyrael9/core/actions/runs/44704096882), [#44713147805](https://github.com/tyrael9/core/actions/runs/44713147805), [#44715114823](https://github.com/tyrael9/core/actions/runs/44715114823), [#44716873212](https://github.com/tyrael9/core/actions/runs/44716873212).

Trusted Push CI [#44795113292](https://github.com/tyrael9/core/actions/runs/44795113292) passed pure tests, report collector, Linux ARM64 Vanilla 5875 build and fixture-backed simulator Runtime PASS, including F39. F42's expanded delayed-use/cancellation scope is closed by trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749). F43 first generic action PR [#44845475962](https://github.com/tyrael9/core/actions/runs/44845475962) and trusted push [#44845469020](https://github.com/tyrael9/core/actions/runs/44845469020) passed. The follow-up two-action PR [#44847028717](https://github.com/tyrael9/core/actions/runs/44847028717) and trusted push [#44847023073](https://github.com/tyrael9/core/actions/runs/44847023073) passed. The three-action PR [#44849541737](https://github.com/tyrael9/core/actions/runs/44849541737) and trusted push [#44849535626](https://github.com/tyrael9/core/actions/runs/44849535626) also passed; the latter emitted `registered=1 attackableNotProposed=1 staleSelected=1 staleRejected=1 liveUnattackable=1 selected=1 stoppedOnly=1`, uploaded [partybot-simulator-c9836fc](https://github.com/tyrael9/core/actions/runs/44849535626/artifacts/14683889194) and reached final Runtime PASS.

## Operational boundary

Legacy remains the default. Shadow never executes. NewEngine is exclusive and has no hidden fallback, but should remain a development-only no-op until audited production adapters and their Core-backed acceptance tests exist. No image publication, merge or deployment is implied by queue tests or CI.