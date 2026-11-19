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

The structured records are emitted through the existing mangosd logging path, so Docker/container logs can collect them without a database migration. A local, read-only collector is available at `tools/partybot_trace_report.py`. It extracts one exact encounter ID or discovers all encounter IDs in a timestamp window, then writes a Markdown summary and a companion raw log containing only the matching trace lines. Integration into the deployment repository's existing diagnostics command remains a separate step; the core-repository collector itself never connects to a server or database.

A post-fight report should include:
1. Encounter header and party roster/roles.
2. Timeline of bot state, target and threat changes.
3. Selected actions and actual Core execution results.
4. Rejected or competing actions with reasons (at decision trace level).
5. Health/death timeline.
6. Missing-data and trace-truncation warnings.

The collector is read-only with respect to its input logs and does not restart services, modify server configuration, query or mutate the database, connect to the network, or change container images. It supports a time-window fallback when an encounter ID is not known. Usage:

```sh
python3 tools/partybot_trace_report.py /path/to/mangosd.log --encounter-id 12345 --output-dir ./partybot-reports
python3 tools/partybot_trace_report.py /path/to/log-directory --since 2026-11-19T05:00:00Z --until 2026-11-19T06:00:00Z --output-dir ./partybot-reports
```

Time-window arguments must use the same timezone style as the timestamp prefix in the source logs.

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

- The Engine threat-budget veto, reservation attempt/accepted/rejected and release/rollback outcomes now emit bounded encounter events through the optional `DecisionContext` observer. The observer is read-only during Shadow evaluation and only receives actual reservation outcomes from `ExecuteSelected`.
- `Player::SetDeathState` calls an optional `PlayerAI::OnAliveStateChanged` observer after Core commits the transition. PartyBotAI emits `death` / `resurrect` encounter records with the bot GUID and health/max-health. Only actual alive/dead transitions are recorded; JUST_DIED-to-CORPSE does not duplicate the death event. These high-value records bypass ordinary per-tick detail caps. Human party-member health/death events and damage-by-damage health deltas are not yet covered.


The first encounter-lifecycle slice is now implemented in the map/instance-scoped PartyCoordinator and the existing mangosd logging path:

- A shared, process-unique encounter ID is allocated once when a live, online party member or primary companion is observed in combat. Multiple bots observing the same party snapshot do not create duplicate starts.
- A quiet settle interval ends the encounter. The default is 30 seconds and can be overridden with PlayerBot.EngineEncounterSettleMs (positive values are capped at 300 seconds; nonpositive values use the default). This deliberately keeps short post-death/recovery gaps inside the same encounter.
- A same-map party change, unsupported/disbanded group or Shadow disable explicitly closes the active encounter. Cross-map/instance transitions do not mutate an old coordinator shard from a different map thread; explicit transition closure remains open.
- The versioned key/value boundary record uses the [PartyBotEngineTrace] prefix and includes trace_version, encounter_id, seq, event, group/map/instance, bot GUID, elapsed_ms and reason. Boundary events bypass the ordinary per-tick diagnostic detail cap.
- The initial pure suite covers start deduplication, party-member and companion combat, the settle boundary, uint32 clock wrap, monotonic boundary sequence and explicit close.

The current trace emits `encounter_start` / `encounter_end`, one versioned `party_snapshot` heartbeat per coordinator interval with bounded Core threat-reference samples, encounter-correlated `target_change` events after successful Legacy `AttackStart` victim switches, bounded Shadow `decision_summary`, `trigger_summary`, `trigger` and `candidate` records, per-hit Player `damage_taken` health deltas from Core `Unit::DealDamage`, and a three-stage Legacy/Core spell lifecycle: `cast_request_attempt`, `cast_request_result`, and `cast_terminal`. The heartbeat includes the encounter ID/sequence, snapshot epoch, counts, up to five party members, five companions and eight hostiles, with GUIDs, role/class, online/alive/combat flags, health/resources, current victims, shapeshift form, current spell ID/state/type/Unit target GUID, orientation and x/y/z position (serialized in tenths). Hostile Creature rows include template entry, rank and world-boss flag, plus up to two actual Core ThreatManager references in the order supplied by Core (unit GUID, threat in tenths, online/accessibility/melee-reachability bits) and an omitted-reference count. Omitted entity counts and a `truncated` flag make bounded output explicit. The facts payload is capped at 1600 characters. Shadow decision events share the encounter sequence and include bot, snapshot epoch, role, candidate/selected counts, strategy, originating trigger, action, spell ID, action channel, target GUID, priority validity, status and a token-safe reason. Trigger telemetry includes one per-bot sampled summary (total/evaluated/active/inactive/cadence-skipped/omitted counts) and at most four named trigger rows per ten-second Shadow detail window. The bounded sample prioritizes active triggers that produced candidates, then other active triggers, inactive triggers and cadence-skipped triggers; candidate count breaks ties among actionable triggers and registration order remains stable for equal keys. The summary distinguishes observations omitted by the Engine's 64-row capacity from observations omitted by the four-row trace-detail cap. Each row includes strategy, trigger name, check interval, active/inactive/cadence-skipped status and candidate count. The originating trigger name is also attached to each candidate. Candidate and trigger records are bounded proposals from read-only Evaluate, not execution evidence. The Engine executor also emits encounter-correlated reservation events through an optional `DecisionContext` observer: `threat_budget_veto`, `threat_reservation_attempt`, `threat_reservation_accepted` / `threat_reservation_rejected`, and `threat_reservation_release`. Records carry the logical action ID, bot owner, actual threat-owner GUID, hostile GUID, estimated threat in tenths, raw headroom, already-reserved amount, expiry and row count where applicable. They are emitted at the coordinator decision points, including partial fan-out rollback and execution-failure release; they do not infer that Core damage/healing later occurred. Reservation, cast lifecycle, alive-state and encounter-start events now draw from one shared map/instance PartyCoordinator budget: up to 1,024 detail events and 256 important events per encounter, across all bots in the party. The first overflow emits one shared `trace_limit` marker; subsequent over-budget events are suppressed. `encounter_end` has a separate one-shot terminal admission and remains emit-able after the coordinator closes the active encounter. The old per-bot cast/target/reservation event caps were removed so they cannot generate duplicate per-bot overflow markers or hide the true party-wide budget. The Shadow evaluation path leaves the observer optional and read-only; only `ExecuteSelected` reports reservation outcomes.

The Legacy `CombatBotBaseAI::DoCastSpell` path emits `cast_request_attempt` immediately before the real Core `Player::CastSpell` call, then `cast_request_result` after the synchronous return. Both share an `action_id` and receive ordered encounter sequences; the result adds numeric `SpellCastResult`, accepted flag and reason. Core `Spell::prepare` binds that action ID once to the root Spell object (matching spell ID and, when Core exposes one, Unit target GUID); destination-location spells retain the original PartyBot target GUID separately because Core deliberately omits a Unit target for those casts. `Spell::finish` emits `cast_terminal` with Core success/failure, while `Spell::cancel` marks the terminal as interrupted. The Spell object carries only numeric correlation IDs/GUIDs, not a PartyBotAI pointer, and nested triggered spells cannot inherit the parent's action ID. Cast lifecycle records share the encounter-wide important-event budget rather than independent per-bot tick counters. This keeps the cap stable as party size changes. `accepted=1` means Core returned `SPELL_CAST_OK` (request accepted), not that the spell completed; `cast_terminal` is the separate final outcome. In addition to the Legacy request/result path, F19 now covers the direct Engine adapter path: `engine_action_attempt`, `engine_action_result` and the later Core `cast_terminal` share the same 64-bit action ID and encounter ID. The actual PartyBotAI/Core Player Lesser Heal fixture passed trusted ARM64 runtime CI [#44594126823](https://github.com/tyrael9/core/actions/runs/44594126823). Terminal events after their encounter coordinator has closed are not emitted. F20's fixed-width complete-report arbitration summary (all candidate outcome buckets) is now fixture/runtime verified. Trusted push CI [#44612693003](https://github.com/tyrael9/core/actions/runs/44612693003) passed the full Linux ARM64 / Vanilla 5875 run and final simulator `Runtime PASS`; it emitted `candidates=1 selected=1 executed=1 execution_failed=0` with all rejection buckets zero. The test observer's PartyBotAI is isolated from Legacy startup so Core's `Spell::finish` Mana deduction is not overwritten by initialization; runtime observed Mana `2896>2866`, LastManaUse active, actual heal and the same action_id through the terminal event. F21 closes the basic live form/current-cast/target/position and per-hostile template-identity fields, but remaining gaps include class-specific stance and AI state-machine state, primary encounter-boss selection/identity on encounter boundaries, prioritized trigger rows when more than four are registered, per-candidate explanation detail beyond the bounded three-row sample, target changes outside Legacy `AttackStart`, damage-by-damage health deltas, cross-map/instance transition closure, and integration of the local collector into the deployment repository's diagnostics command. The local read-only report generator and its CI-tested parser are implemented; they do not make the encounter trace itself lossless. Reservation outcome tracing, PartyBotAI and ordinary group-member death/resurrection sampling, shutdown flush and same-map final-PartyBot map closure are implemented and runtime-verified. The legacy [PartyBotDecision], [PartyBotShadow] and [PartyBotShadowDecision] records remain available as compatibility diagnostics; the new encounter records are correlated. This is still an incremental diagnostic foundation, not yet a post-fight AI trace.

For the current Shadow path, both PlayerBot.EngineShadow and PlayerBot.DecisionTrace must be enabled to observe these boundary records. New Engine Mode must remain disabled for meaningful gameplay until the remaining trace contract and the audited exclusive executor are complete.

### Alive-state observer regression

The Core-backed simulator now attaches a temporary PlayerAI observer to the Main Tank in the existing real five-Player death/recovery fixture. It requires exactly one alive -> dead callback from the real Core damage/death path and explicitly advances JUST_DIED -> CORPSE to prove that the latter does not emit a duplicate. The simulator restores the previous AI pointer before cleanup. Fixture-backed ARM64 runtime run [#44448514086](https://github.com/tyrael9/core/actions/runs/44448514086) passed the death regression and the corresponding resurrection regression. This validates the Core callback boundary; it does not yet validate PartyBotAI's formatted encounter event end-to-end or human-member observation.


Core callback semantics: Player::SetDeathState invokes OnAliveStateChanged after each state assignment, including JUST_DIED followed by CORPSE. The hook is a state sample; PartyBotAI suppresses duplicate semantic events by returning when wasAlive == isAlive. The simulator regression explicitly distinguishes raw callback count from actual alive/dead transitions.


### Resurrection lifecycle coverage

The five-Player Core death/recovery fixture resurrects the Main Tank only after all death-state roster and threat snapshots have been captured. A fresh PlayerAI observer must receive exactly one dead -> alive transition from Core Player::ResurrectPlayer, and the runtime CI script requires the corresponding PASS marker. Fixture-backed ARM64 runtime run [#44448514086](https://github.com/tyrael9/core/actions/runs/44448514086) passed both the death and resurrection markers. This tests the Core callback boundary; PartyBotAI's encounter trace formatting remains a separate integration test.


## Runtime-verified PartyBotAI lifecycle trace

Fixture-backed ARM64 Runtime CI run [#44452478936](https://github.com/tyrael9/core/actions/runs/44452478936), commit `2afd7cb`, verified the actual PartyBotAI callback and formatted event path, not just the Core observer seam. The simulator enabled `PlayerBot.DecisionTrace`, attached a real PartyBotAI to the five-Player Group fixture, opened a coordinator encounter, and drove the tank through Core `DealDamage`, the repeated `JUST_DIED -> CORPSE` state, and Core `ResurrectPlayer`.

The runtime log contained two encounter-correlated records with the same encounter ID and consecutive sequence numbers:

- `event=death`, `alive_before=1`, `alive_after=0`
- `event=resurrect`, `alive_before=0`, `alive_after=1`

The simulator also asserted that the sequence advanced by exactly two events; the repeated CORPSE callback emitted no third event. The CI script requires both formatted records and the dedicated PartyBotAI lifecycle PASS marker.

This proves the PartyBotAI lifecycle trace end-to-end on the pinned Core fixture. It does not yet cover human party-member lifecycle events, damage-by-damage health deltas, or trace-budget overflow.


## Runtime verification update (2026-11-19)

Push CI [#44456096650](https://github.com/tyrael9/core/actions/runs/44456096650) on commit `5cc563a6703513c76090f6a03ed04343008494c2` passed the full Linux ARM64 Core build and the fixture-backed Stages A-F12 runtime. The configured Vanilla 5875 extracted-data fixture downloaded successfully; the simulator used disposable `partybot_sim_*` schemas and reached the final `Runtime PASS` marker.

The runtime log confirms the trace path end-to-end on the real five-Player Group fixture:
- PartyBotAI emitted one encounter-correlated `event=death` record and one `event=resurrect` record with consecutive sequence numbers.
- Core's repeated JUST_DIED -> CORPSE observer callback was filtered as an unchanged alive/dead state and did not create a duplicate semantic death event.
- The simulator requires both formatted records and the dedicated PASS marker; this is no longer only a PlayerAI observer-boundary test.

The same run also passed periodic threat-component parity fixtures: periodic power (including Energy versus Mana PERIODIC_ENERGIZE behavior and OBS_MOD_MANA), native periodic Mana Leech, and composite periodic Life Leech. These are applied-tick component proofs, not predictions of future aura ticks or proof that arbitrary whole-spell threat impact is complete.

This run validates the Core-backed CI runtime. It does not mean the whole encounter trace contract is complete: the global terminal event budget, human-party-member health/death coverage, full trigger/arbitration events and read-only report collector remain open.


## Read-only report collector (2026-11-19)

The repository-local collector `tools/partybot_trace_report.py` uses only the Python standard library. It accepts one or more files/directories and can report every discovered encounter (`--all`), filter by an exact decimal encounter ID (`--encounter-id`), or discover encounters in an inclusive ISO-8601 timestamp window (`--since` / `--until`). It produces:
- `encounter-<id>.md`: event counts, ordered timeline, sequence gaps/duplicates, boundary presence, truncation/trace-limit warnings and interpretation notes.
- `encounter-<id>.raw.log`: original structured trace lines for that encounter, ordered by the shared sequence number.

A time window discovers which encounters were active in that period, then includes all available lines for those encounter IDs so the generated timeline is not clipped to the selected window. `--all` generates a report pair for each encounter ID found in the input. Both Markdown and raw-log report files are chmod 0600 because they contain player/party identifiers and diagnostic details. The parser does not infer that a candidate executed: it distinguishes candidate/decision summary records from cast request results and Core cast terminal outcomes. Sequence gaps are reported as possible rate limiting or incomplete log collection, not automatically as server faults. Tests cover exact encounter selection, timestamp-window discovery, malformed lines, sequence gaps, truncation warnings, raw-line preservation and timezone-style mismatches.

The collector itself remains a local-file-only utility. A read-only integration into `tyrael9/vmangos-deploy`'s diagnostics wrapper is proposed in Draft PR [#1](https://github.com/tyrael9/vmangos-deploy/pull/1); its default selector reports all encounters in the captured log tail, with optional exact-ID and time-window selectors. The deployment PR currently has no Actions check runs, so integration validation remains pending. This does not close the remaining terminal/shutdown or trace-coverage gaps.


## Shared encounter trace budget (2026-11-19)

`PartyCoordinator` owns the event budget for its `(group, map, instance)` encounter shard, so all PartyBotAI instances in the same party consume the same counters. The current fixed limits are 1,024 detail records and 256 important records per encounter. Party snapshots, target changes and Shadow decision summaries/candidates use the detail budget. Encounter start, cast attempt/result/terminal, reservation outcomes and bot death/resurrection use the important budget. The first event beyond either budget emits exactly one shared `trace_limit` marker; later over-budget records are suppressed without per-bot duplicate markers. The encounter-end boundary has a dedicated one-shot terminal admission and is allowed after `CloseEncounter` clears the active encounter fields. A new encounter resets all budget state.

The pure C++ regression covers party-shared counters, both limits, one-shot overflow marker admission, terminal emission after close, terminal deduplication and fresh budget reset. Runtime settings `PlayerBot.EngineEncounterMaxDetailEvents` (default 1,024; hard cap 4,096) and `PlayerBot.EngineEncounterMaxImportantEvents` (default 256; hard cap 1,024) are applied by the owning map thread. Values below one clamp to one, preserving the first explicit overflow marker; oversized values clamp to the hard ceiling. The global budget bounds the new structured encounter stream, not legacy compatibility log categories. Shutdown flushing remains future work.


## Core-backed shared-budget overflow regression (Stage F13, pending CI)

Stage F13 extends the existing real five-Player Group fixture. It attaches two distinct PartyBotAI instances to two live Player actors and points both at one map/instance PartyCoordinator. Each actor repeatedly switches its actual Core victim between the two live hostile Creature fixtures through `PartyBotAI::AttackStart`.

Acceptance requires 1,025 successful Core victim changes across the two bots: the shared coordinator admits exactly 1,024 `target_change` detail records, the 1,025th request emits one `event=trace_limit reason=detail_budget_exhausted` record, and subsequent over-budget requests do not emit a second marker. The CI shell runner independently requires the PASS marker and exactly one formatted global detail-budget marker line. This closes the gap between pure coordinator admission/configuration tests and the actual PartyBotAI logging path if the fixture passes.

The runtime test is intentionally limited to the structured encounter log stream. It does not prove a zero-loss guarantee for legacy compatibility logs, shutdown flush, or realistic long-duration production log volume.


## Sampled non-PlayerBots party-member state trace (Stage F14, pending CI)

The map-thread PartyCoordinator now tracks value-only health/max-health/alive baselines for online party members without `CombatBotBaseAI`. The first observation is a baseline, unchanged snapshots emit nothing, health changes consume the detail budget, and alive transitions consume the important budget. PartyBots are excluded because their attached PlayerAI callbacks are authoritative. A member absent from the current map snapshot loses its baseline, so rejoining begins with a new observation.

The five-Player Core simulator drives actual `Unit::DealDamage` (nonlethal and lethal) and `Player::ResurrectPlayer`, captures fresh `PartySnapshotBuilder` values after each operation, and emits through the same DecisionTrace admission/formatting method. Runtime acceptance requires exactly one `party_member_health`, `party_member_death`, and `party_member_resurrect` record for the same member GUID. These are sampled deltas, not a per-hit damage stream.


## Runtime verification update (2026-11-19, F13/F14)

Fixture-backed Push CI run [#44556650342](https://github.com/tyrael9/core/actions/runs/44556650342), commit `7f53e5e305aa36f84b191a7af517c01ea79fe405`, passed the complete Linux ARM64 / Vanilla 5875 build and Core-backed simulator runtime.

The runtime log proves:

- **F13 party-wide budget:** two real PartyBotAI instances shared one PartyCoordinator; 1,024 real target-change detail events were admitted, one `trace_limit` marker was emitted on overflow, and subsequent detail events were suppressed.
- **F14 ordinary party member sampling:** real Core `Unit::DealDamage` and `Player::ResurrectPlayer` produced exactly one sampled `party_member_health`, `party_member_death`, and `party_member_resurrect` record for the same non-PlayerBots Group member GUID. The CI assertion matched the member GUID in the PASS record to the structured trace.
- The simulator also passed PartyBotAI death/resurrection callback records, periodic damage/healing/power/mana-leech/life-leech threat parity, real Group roster and multi-add threat scenarios, spell/movement/follow behavior, cleanup and the final `Runtime PASS` marker.

The runner's GUID assertion was hardened to tolerate ANSI color sequences. A pure C++ regression also proves that health sampling rejects snapshots from a different group, map or instance without replacing the active shard's baseline; the same-shard follow-up still emits the correct health transition.

F13-F22 are runtime-verified, not merely implemented or unit-tested: the shared budget, ordinary-member lifecycle, shutdown flush, final same-map PartyBot departure, trigger attribution, Engine action-to-Core cast correlation, full-report arbitration summary, live member form/current-cast/position, hostile Creature template identity and per-hit PartyBotAI Player health deltas have passed fixture-backed ARM64 CI. Remaining Phase 3 gaps include AI state-machine state beyond the captured Core form/current cast, primary encounter-boss selection, broader per-candidate arbitration detail, per-hit damage records for ordinary non-PlayerAI party members (currently sampled by heartbeat), cross-map/instance closure and deployment diagnostics integration. No server deployment or image publication was performed.

A lethal hit's death transition may precede its `damage_taken` record: Core `Unit::DealDamage` calls `Kill` (which notifies the alive-state observer) before the damage callback reads the final health value. Both records share the encounter ID and ordered sequence; `damage_taken` contains the actual before/after values, not the incoming damage argument.

## F23 — prioritized bounded Trigger detail (implementation awaiting runtime)

The four Trigger rows emitted per bot/ten-second sample are selected from the retained report using a stable ordering: candidate-producing active triggers first (descending candidate count), active triggers with no candidates next, then inactive and cadence-skipped triggers. Equal-priority rows preserve registration order. `trigger_summary omitted=` continues to mean Engine observations dropped beyond the 64-row report capacity; new `detail_limit=` and `detail_omitted=` fields report the separate four-row formatter cap. Pure tests cover the ordering and overflow cases. The Core fixture now registers one active-empty Trigger plus three interval-limited Triggers, evaluates twice to produce seven observations, and asserts that the emitted four rows are healing_triage, active-empty, tank_target and dps_target in that order. Fixture-backed ARM64 acceptance is pending.
