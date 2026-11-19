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

New Engine Mode on a test realm should normally use encounter or decisions. The initial party-snapshot heartbeat is one compact shared record every 2 seconds during an active encounter. `PlayerBot.EngineEncounterHeartbeatMs` controls the interval (default 2000 ms, positive values are clamped to 1000–60000 ms, nonpositive values disable heartbeats). The map/instance-scoped coordinator owns the interval and sequence, so five bots do not emit five duplicate party snapshots. This is separate from the future per-bot bot-state heartbeat and event-driven action records; the complete event budget still needs load testing.

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

## Current implementation status and gap

The first encounter-lifecycle slice is now implemented in the map/instance-scoped PartyCoordinator and the existing mangosd logging path:

- A shared, process-unique encounter ID is allocated once when a live, online party member or primary companion is observed in combat. Multiple bots observing the same party snapshot do not create duplicate starts.
- A quiet settle interval ends the encounter. The default is 30 seconds and can be overridden with PlayerBot.EngineEncounterSettleMs (positive values are capped at 300 seconds; nonpositive values use the default). This deliberately keeps short post-death/recovery gaps inside the same encounter.
- A same-map party change, unsupported/disbanded group or Shadow disable explicitly closes the active encounter. Cross-map/instance transitions do not mutate an old coordinator shard from a different map thread; explicit transition closure remains open.
- The versioned key/value boundary record uses the [PartyBotEngineTrace] prefix and includes trace_version, encounter_id, seq, event, group/map/instance, bot GUID, elapsed_ms and reason. Boundary events bypass the ordinary per-tick diagnostic detail cap.
- The initial pure suite covers start deduplication, party-member and companion combat, the settle boundary, uint32 clock wrap, monotonic boundary sequence and explicit close.

The current trace emits `encounter_start` / `encounter_end`, one versioned `party_snapshot` heartbeat per coordinator interval, bounded Shadow `decision_summary` / `candidate` records, and actual Legacy `cast_request_result` records during active encounters. The heartbeat includes the encounter ID/sequence, snapshot epoch, counts, up to five party members, five companions and eight hostiles, with GUIDs, role/class, online/alive/combat flags, health/resources and current victims. Omitted entity counts and a `truncated` flag make bounded output explicit. The facts payload is capped at 1600 characters. Shadow decision events share the encounter sequence and include bot, snapshot epoch, role, candidate/selected counts, strategy/action, spell ID, action channel, target GUID, priority validity, status and a token-safe reason. The per-bot ten-second Shadow detail window limits each summary to at most three candidate records. These are proposals from read-only Evaluate, not execution evidence. The new `cast_request_result` record is emitted by the Legacy `CombatBotBaseAI::DoCastSpell` path only after the real Core `Player::CastSpell` returns; it records source/executor, target GUID, spell ID, numeric `SpellCastResult`, accepted flag and reason. Up to four cast results are recorded per bot AI tick; further attempts produce one explicit `trace_limit` marker for that tick. `accepted=1` means Core returned `SPELL_CAST_OK` (the request was accepted), not that a non-triggered cast completed. Direct `CastSpell` call sites that bypass `DoCastSpell` are not covered yet. Full per-bot state (form/stance/current cast/position), boss identity, trigger-level events, full arbitration detail, cast completion/interruption and action aftermath, target/threat/reservation/health/death events, bounded encounter buffers, terminal trace-limit guarantees, shutdown flush and the read-only post-fight collector remain open. The legacy [PartyBotDecision], [PartyBotShadow] and [PartyBotShadowDecision] records remain available as compatibility diagnostics; the new encounter records are correlated. This is still an incremental diagnostic foundation, not yet a post-fight AI trace.

For the current Shadow path, both PlayerBot.EngineShadow and PlayerBot.DecisionTrace must be enabled to observe these boundary records. New Engine Mode must remain disabled for meaningful gameplay until the remaining trace contract and the audited exclusive executor are complete.
