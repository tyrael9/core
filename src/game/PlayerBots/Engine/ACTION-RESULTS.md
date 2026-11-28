# PartyBotEngine action result events

## Event boundary

F31 introduces synchronous dispatcher-result callbacks through `ActionResultListener`.

An `ActionResultEvent` owns only value data:

- copied `ActionCandidate` including its nonzero `actionId` when an ID was allocated;
- `ActionResult` returned by the engine dispatcher / action adapter;
- `attempted`, which is false when the dispatcher rejected work before calling `Action::Execute` and true when the adapter was called;
- snapshot epoch, dispatch timestamp and a stable reason string.

The event is emitted on the owning Engine/map thread for each final result recorded by `ExecuteSelected`, including pre-adapter failures (action ID unavailable, action no longer registered, threat budget veto, partial threat-reservation rollback) and synchronous adapter returns. It is emitted independently of trace and diagnostic observer configuration.

## Listener registry

Each `PartyBotEngine` owns a maximum of 32 shared listener registrations. Null pointers, duplicate registrations and registrations after capacity are rejected. At dispatch time the Engine copies the listener vector before invoking callbacks. A callback may register a listener safely, but the new listener starts receiving events on the next result, not the event already in progress.

Listeners run synchronously. They must not retain a `DecisionContext` reference, and should copy only the event's value data when storing state. The event includes no `Player*`, `Unit*`, `Map*` or other Core-owned address.

## Important distinction: adapter result versus Core terminal

`ActionResult::Success` currently means only that the synchronous `Action::Execute` adapter returned Success. For a spell action, that can mean request acceptance, not successful spell completion. F31 intentionally does not relabel it as Core success. Future asynchronous Core terminal events must use a separate explicit result stage correlated by the same `actionId`, with bounded lifetime and cleanup on cancellation, timeout, encounter close and bot teardown.

## Test matrix

- success and failure adapter returns are delivered in order with matching candidate/action ID and metadata;
- callbacks work when tracing and diagnostic observers are disabled;
- null and duplicate listeners are rejected;
- listener registration during a callback cannot invalidate the current callback iteration;
- pre-adapter dispatcher failures use `attempted=false` (covered by later Core adapter/runtime scenarios).
