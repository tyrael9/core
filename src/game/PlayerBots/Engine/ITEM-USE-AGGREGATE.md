# F42 design — aggregate Core item-use lifecycle

## Status

F40 completed the source audit and single-slot potion baseline. F42 has now implemented the bounded typed aggregate source and pure tests, and added a synthetic two-ON_USE-slot fixture using real Core Spell objects. The production item-use action adapter is **not implemented**; real-Core delayed-use and cancellation-before/after-mutation fixtures remain open.

This document is the F42 implementation contract and status. It deliberately separates three lifetimes that the current Core API exposes independently:

1. The root item-use request.
2. Every Core Spell child created from the item's ON_USE spell slots.
3. Item charge/count/destruction mutations performed by Core.

A child Spell terminal is not the root request terminal, and neither is proof of item mutation or final effect application.

## Observed Vanilla 5875 call path

- `WorldSession::HandleUseItemOpcode` validates the actor, bag/slot item, prototype, requested packet spell slot and ON_USE trigger, equipment requirements and `Player::CanUseItem`; it can reject before entering `Player::CastItemUseSpell`.
- The handler then calls `Player::CastItemUseSpell(item, targets)`. The current method signature does not carry `packet.spellSlot`; it loops every prototype spell slot and casts every valid ON_USE slot. Do not silently change this fan-out while adding telemetry.
- For each valid slot with a resolvable SpellEntry, `Player::CastItemUseSpell` creates a separate `Spell` and calls `prepare`. The first valid child is non-triggered; subsequent valid children are triggered. Missing/invalid SpellEntry rows are skipped.
- `Spell::prepare` can complete a child synchronously, or leave it pending. A failed pre-cast check still reaches `Spell::finish(false)`; `Spell::cancel` marks interruption and then finishes the child. Multiple child terminals can therefore occur before the outer item-use loop has finished dispatching all children.
- `Spell::IsCastByItem()` exposes only a boolean. The `m_CastItem` pointer is private to Spell.
- `Spell::TakeReagents` runs before spell effects and may clear the cast-item pointer and destroy reagent counts when the cast item is also a reagent. `Spell::TakeCastItem` decrements charges or destroys an expendable item; for delayed spells it runs before delayed impact, while for immediate spells it runs after effects. These mutations are not equivalent to the child Spell terminal.

## Required identifiers and retained data

All retained state must be bounded and value-only. No `Player*`, `Spell*`, `Item*`, `Unit*` or `Map*` may be stored in the aggregate.

Root request record:

- Unique nonzero `itemUseId` for the logical invocation.
- Optional enclosing Engine `actionId` and `encounterId`; an item use initiated outside NewEngine has no synthetic Engine action.
- Actor GUID, item GUID, item entry, bag/slot when available, item count/charge snapshot and request source.
- The requested client spell slot, when the originating WorldSession packet is available. The current `CastItemUseSpell` signature does not receive it; plumbing that scalar must not change the Core's actual all-ON_USE fan-out behavior.
- Begin timestamp, dispatch-closed flag, total valid child count, completed child count, skipped slot count and aggregate terminal state.

Child record, created before calling each child `Spell::prepare`:

- Unique child ID, parent `itemUseId`, prototype slot index, Spell ID and whether Core marks it triggered.
- Core prepare outcome and eventual Core Spell terminal (Succeeded / Failed / Interrupted), correlated by child ID.
- Target identity copied as GUID/scalars only. A terminal child does not imply a projectile/effect was applied to every intended target.

Item mutation record, separately emitted around Core mutation sites:

- Parent `itemUseId` and source child ID when known.
- Item GUID/entry, mutation kind (charge change, stack/count change, item destruction or reagent consumption), slot index if applicable, before/after count/charges and whether the item still exists after Core completes the mutation.
- Mutation timestamp and mutation phase relative to dispatch/child terminals.
- No inference that a successful Spell terminal consumed the item, or that an item mutation proves the spell effect succeeded.

## Dispatch and aggregation rules

1. Open the root request before fan-out. Allocate/register each child before entering its `Spell::prepare`; this is required because instant triggered spells may finish synchronously.
2. Close fan-out only after `Player::CastItemUseSpell` has processed all prototype slots. Invalid or absent SpellEntry slots are counted as skipped, not as failed children.
3. Buffer child terminals received before fan-out closes. Aggregate only after dispatch is closed **and** every registered child has a terminal. A child prepare rejection is a failed child, not a missing callback.
4. A bounded root result reports child totals and a distinct aggregate outcome (all succeeded, partial success, all failed, or all interrupted). Keep the child list bounded by the fixed item prototype spell-slot maximum.
5. Emit item mutation independently. If no mutation is observed, report `mutation=none_observed`; do not manufacture a consumption result from child Spell status.
6. If a delayed child remains active, the root stays pending until that child reaches a Core terminal or the root's bounded timeout/teardown policy closes it as unknown/expired. Do not declare root completion when the dispatch loop returns.
7. Cancellation before mutation and cancellation after mutation must produce different evidence. An `Interrupted` child may have consumed a charge, and a `Failed` child may still have caused a mutation earlier in its lifecycle.
8. Any continuation that requires the whole item request must depend on the new `ItemUseTerminal` aggregate source. `CoreSpellTerminal` remains the per-child source; optional mutation-dependent continuations need a separately typed mutation prerequisite.

## Preflight failures and direct callers

The network handler can reject an item request before `CastItemUseSpell` is reached. For a NewEngine adapter, its synchronous `ActionResult` must report those preflight failures; if encounter diagnostics also need those rejected attempts, add a handler-scoped begin/reject callback that closes on every early return.

The simulator and internal callers may invoke `Player::CastItemUseSpell` directly. The root hook therefore belongs at the Core method boundary, with optional source/requested-slot metadata supplied by the handler through a value-only scoped context. Do not depend exclusively on the network opcode path.

## Bounded lifecycle and thread rules

- Root records are owned by the acting PartyBotAI/Engine on the Player's map thread.
- Capacity is fixed and overflow fails closed with an explicit drop/overflow counter; never evict an active root in favor of a new request.
- Root and child IDs are monotonic/nonzero and cannot be reused while a matching tombstone is retained.
- TTL arithmetic is wrap-safe and shorter than half the uint32 clock range.
- Map leave, actor removal, encounter closure and server shutdown settle/flush or explicitly mark outstanding roots unknown before clearing their value-only state.
- Core hooks do not dispatch continuations or run another action. They only append/settle bounded value events; normal Engine pending dispatch performs later work.

## Required pure regressions

- Single child: synchronous success and synchronous prepare failure.
- Multi-slot fan-out: first child finishes before the second is registered/finished; the root must remain open until fan-out closure.
- Invalid SpellEntry slot skipped without inflating expected child count.
- Mixed child results and deterministic aggregate status.
- Delayed child: root remains pending after dispatch closes and settles only on its eventual child terminal.
- Interrupted child before any mutation versus interruption after a recorded charge/count mutation.
- Item charge decrement, stack consumption, expendable item destruction and item-as-reagent consumption.
- Duplicate/late terminal, unknown child/root, duplicate root ID, TTL expiry, uint32 wrap, full root capacity and teardown with outstanding roots.
- Callback-time reentrancy must not execute actions or mutate the active root container unsafely.

## Required Core simulator coverage

1. Preserve current single-potion and zero-charge tests.
2. Add a real item with at least two ON_USE spell slots and assert exact child fan-out, trigger flags and root aggregation despite synchronous child completion.
3. Add a delayed ON_USE spell and verify item mutation and child terminal order independently.
4. Interrupt one scenario before item mutation and another after mutation; assert root status and mutation evidence differ.
5. Exercise at least one charge-based item and one stack/count-based expendable item, including item destruction and item-as-reagent if a native Vanilla item fixture supports it.
6. Require final Core health/effect state from Core-owned objects; never accept trace counts as proof that the spell effect applied.

## F42 implementation status

Implemented in the current branch:
- Fixed-capacity, value-only root/child/mutation tracker with bounded recent IDs/tombstones and deadline handling.
- Per-child registration before Core `Spell::prepare`, per-child `finish/cancel` correlation, and a dispatch-close barrier so synchronous child terminals cannot prematurely finalize a fan-out.
- Skipped-slot accounting, delayed-child waiting, aggregate AllSucceeded / PartialSuccess / AllFailed / AllInterrupted outcomes, and Expired settlement.
- Independent mutation events for the Core item-charge/reagent mutation hooks; no inference between mutation and Spell terminal.
- PartyBotEngine ItemUseTerminal listeners, recent-terminal history and typed continuation prerequisite support. A continuation remains subject to normal pending-queue dispatch/revalidation.
- Inventory mutation invalidation of cached decisions, and a returned root `itemUseId` from `Player::CastItemUseSpell` so a caller can register dependent work after synchronous or delayed use.

Pure tests in `tests/PartyBotEngineTests.cpp` cover fan-out closure, synchronous children, delayed children, partial/all-failure/interruption outcomes, independent mutation facts, capacity, TTL/uint32 wrap, late-child expiry, listener publication and continuation correlation/history.

The Core simulator's two-slot prototype fixture has already emitted:
`multiRoot=1 multiChildren=2 firstTriggered=0 secondTriggered=1 multiMutations=2 multiChargesConsumed=1 cleanup=1/1/1`.
It temporarily sets a non-stackable one-count fixture to exercise the real finite-charge mutation path and restores the modified prototype fields before returning. The run also retains the single-potion success and zero-charge rejection checks.

Trusted Push CI [#44833250522](https://github.com/tyrael9/core/actions/runs/44833250522) printed the exact F42 PASS marker and completed all later simulator scenarios, but the wrapper exited 1 because it still required the obsolete F40 marker. PR CI [#44833258687](https://github.com/tyrael9/core/actions/runs/44833258687) passed pure tests, report collector and ARM64 build; fixture execution is intentionally push-only. Corrected trusted Push CI [#44838633233](https://github.com/tyrael9/core/actions/runs/44838633233) is validating the gate; final Runtime PASS is required to close F42.

The delayed-child and cancellation-before/after-mutation behavior is covered at the pure tracker level. A deterministic real-Core delayed ON_USE Spell fixture and a real-Core cancellation test on each side of actual item mutation are not yet verified.



## Non-goals for F42

F42 does not register NewEngine class rotations, infer item success from a single child, alter the underlying Vanilla ON_USE fan-out, or implement generic inventory optimization. The production item-use action adapter remains future work after the common executor contract and its Core-backed tests are proven.
