# PartyBotEngine decision and encounter trace

## Purpose

Combat logs are a required part of the new AI, not optional debug decoration. A player must be able to finish a fight, collect one bounded log/report, and provide it for offline diagnosis without having to construct special situations or watch every bot continuously.

The trace must answer:
- What encounter and party composition was active?
- What did each bot believe its role, health, mana, target and combat state were?
- Which triggers fired, which actions were considered, and why were candidates rejected or deprioritized?
- Which action won arbitration, what was attempted, and what did the Core return?
- What happened to target assignments, reservations, threat, health, casts and deaths before the fight failed or ended?

This contract describes the target trace for New Engine Mode. The current Shadow summary/decision records are an earlier, incomplete diagnostic format and do not satisfy this contract.

## Correlation model

Every encounter trace must carry stable correlation keys:
- trace_version: schema version, incremented when fields or meanings change.
- encounter_id: unique server-process-local ID for one encounter window.
- seq: monotonically increasing event sequence within that encounter.
- group, map, instance: party/coordinator identity.
- boss_guid, boss_entry: primary encounter creature when known; add GUIDs remain event targets.
- bot: bot GUID for bot-specific events; zero for party/encounter-wide events.
- tick or snapshot_epoch: decision/update correlation where applicable.

Use GUIDs and numeric IDs rather than localized player names. The normal server log timestamp remains the wall-clock timestamp. Include encounter elapsed milliseconds so events can be ordered even when clocks or log collection boundaries are inconvenient.

An encounter begins when a supported hostile encounter is detected for a party and ends after combat has remained cleared for a configured settle interval, the instance changes, or the party is destroyed. A wipe/death does not immediately end the trace; recovery and regroup decisions belong to the same encounter until the settle condition is met. Re-engaging after a completed settle interval creates a new ID.

## Event vocabulary

All new-engine records use a stable prefix and key/value fields, for example:
[PartyBotEngineTrace] event=action_result trace_version=1 encounter_id=...
Values must be escaped or encoded so one event remains one parseable log line. Missing facts are explicitly represented as unknown/zero plus a validity field; do not invent values.

Required event families:

| Event | Required diagnostic content |
|---|---|
| encounter_start / encounter_end | Encounter ID, group/map/instance, boss/add identity, reason, duration, outcome when known |
| party_snapshot | Member GUID/class/role/alive/health/mana/current target; hostile GUID/current victim/health; snapshot age/epoch |
| bot_state | Bot role, state-machine state, health/mana, form/stance, current cast, victim and position/range facts relevant to decisions |
| trigger | Strategy, trigger name, active/inactive result and reason; cadence/cache status |
| candidate | Strategy, trigger, action, target, priority/channel, Possible/Useful result and rejection reason |
| arbitration | Selected action, competing candidates, multiplier/veto result, reservation conflict and channel decision |
| action_start / action_result | Action/target/spell, preconditions, Core cast/movement result, final outcome and failure reason |
| cast_start / cast_result | Spell ID, target, cast result/interruption/failure, GCD/cooldown/resource facts where available |
| target_change | Old/new victim, target source (assist, peel, threat recovery, encounter, manual) and reason |
| threat_sample | Hostile GUID, party victim, bot threat and tank/current-victim threat values when the Core exposes valid comparable data; mark unavailable otherwise |
| reservation | Reservation type, target, owner, amount, expiry and acquire/refresh/release/conflict result |
| health_change / death | Bot/member GUID, health transition, source/spell if known, alive/dead state |
| state_change | Previous/new engine state and transition cause |
| trace_limit / trace_error | Explicit indication that records were dropped, capped or could not be emitted |

The trace should record decision evidence, not duplicate the complete world/server log. Damage/heal amounts may be included when available from an authoritative Core event; otherwise they must not be inferred from health deltas.

## Sampling and volume controls

The target design has configurable levels:
- off: no engine trace, except fatal trace subsystem errors.
- encounter: encounter boundaries, party/bot state heartbeats, selected actions and results, target changes, deaths, reservation outcomes and relevant threat samples.
- decisions: encounter level plus trigger evaluations, eligible candidates, rejected candidates and arbitration explanations.
- verbose: additional cached-value diagnostics and detailed candidate facts for short targeted investigations.

New Engine Mode on a test realm should normally use encounter or decisions. The default heartbeat should be slow enough for the 1 OCPU target; an initial design target is one compact bot-state heartbeat every 2 seconds during active combat, with event-driven records for actions and state transitions. The interval and event budget must be configurable and load-tested.

The trace sink must have:
- a per-encounter event budget and a clear trace_limit marker;
- bounded in-memory buffering, if buffering is used;
- no synchronous database or network operations on map threads;
- no unbounded per-tick candidate dumps;
- per-bot and per-encounter rate limits that do not suppress action results or death events;
- a way to flush encounter-end records and preserve recent context on server shutdown.

Never silently drop high-value events such as action results, death, encounter end or trace errors. If an event budget is exhausted, reserve capacity for one terminal truncation marker and encounter-end summary.

## Post-fight collection

The first implementation may emit structured lines through the existing mangosd logging path, so Docker/container logs can collect them without a database migration. The deployment diagnostics script should later extract all lines for one encounter_id into a compact report and retain the raw matching lines as an attachment.

A post-fight report should include:
1. Encounter header and party roster/roles.
2. Timeline of bot state, target and threat changes.
3. Selected actions and actual Core execution results.
4. Rejected or competing actions with reasons (at decision trace level).
5. Health/death timeline.
6. Missing-data and trace-truncation warnings.

The collection command must be read-only and must not restart services, modify server configuration, query or mutate the database, or change container images. It should support a time-window fallback when an encounter ID is not known.

## Analysis expectations

Post-fight analysis should distinguish:
- Perception/data issue: snapshot or adapter lacked/wrongly resolved a fact.
- Policy issue: triggers, strategy composition or priority chose the wrong behavior.
- Coordination issue: reservations, role assignment or group arbitration failed.
- Eligibility issue: action was correctly or incorrectly rejected by Possible/Useful checks.
- Execution issue: action was selected but vMaNGOS rejected, interrupted or failed it.
- Encounter-model issue: mechanic state/phase/identity was missing or wrong.
- Trace gap: evidence needed to diagnose the issue was not captured.

A log analysis must not assume that a selected candidate was executed. Only an action_result/authoritative execution record proves that an action was attempted or accepted.

## Current implementation gap

At the current branch head, PlayerBot.DecisionTrace emits legacy decision diagnostics and the Shadow integration emits rate-limited summaries plus at most three candidate details per bot every ten seconds. It has no encounter ID, complete action lifecycle, durable per-fight timeline or collector report. This is useful smoke-test instrumentation only; it is not yet a post-fight AI trace.

The next observability milestone must implement this contract before New Engine Mode is used for meaningful combat testing.
