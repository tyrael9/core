# PartyBotEngine

The target is a complete independent PartyBot AI for Vanilla 1.12.1 / client build 5875 on the pinned vMaNGOS Core. This is a new decision system, not a per-role rewrite that delegates unfinished behavior to the old PartyBot AI.

## Design documents

- ARCHITECTURE.md: runtime authority, strategy composition, decision loop and executor boundary.
- ROADMAP.md: development phases and acceptance criteria.
- ACTION-QUEUE.md: per-dispatch and pending event queue semantics, expiry, deduplication and integration gates.
- DECISION-TRACE.md: structured encounter logs and post-fight analysis contract; `tools/partybot_trace_report.py` creates local Markdown/raw-log reports by encounter ID or time window.
- VMANGOS-API-AUDIT.md: audited Core APIs and outstanding adapter work.
- RESEARCH-CMANGOS.md: Strategy/Trigger/Action/Value/Multiplier and ActionNode research.
- RESEARCH-AZEROTHCORE.md: class/role/encounter strategy research.
- TESTING.md: CI, in-game validation and log collection.
- CORE-COMBAT-SIMULATOR.md: headless Core-backed combat simulator design and API feasibility audit.
- THREAT-BUDGETS.md: Core-authoritative aggro thresholds, snapshot semantics and action-specific threat-impact requirements.
- ThreatBudget.h: pure snapshot-only current-victim threshold/headroom assessment; not an action-impact estimator or Core target selector.
- ThreatImpact.h: GUID/scalar-only action threat-impact contract; unimplemented estimates default to Unknown rather than zero.
- CoreThreatImpactEstimator.h: narrow Core-backed applied direct-damage component calculation; remains Unknown until an Action proves its complete threat-impact set.

Update relevant source and design/testing documents in the same change.

## Pinned baseline

- Core: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa
- Deploy wrapper: ebaa560951459419c35140e20d787ec5db2905b8
- Client/data: Vanilla 1.12.1 / build 5875
- Language: C++14
- Target: Linux ARM64, Oracle A1 (1 OCPU / 6 GB)

Do not rebase onto a moving upstream branch without reviewing PartyBot, lifecycle and database changes.

## Current implementation status

The repository currently contains:
- C++14 Strategy, Trigger, Action, Value and Multiplier interfaces, a bounded typed per-bot ValueCache and opt-in TriggerScheduler cadence with Core lifecycle invalidation hooks;
- GUID/scalar-only PartySnapshot data and same-map snapshot collection;
- PartyCoordinator reservations and the (group,map,instance) shard registry;
- deterministic candidate evaluation, explicit eligibility and channel arbitration;
- a bounded per-dispatch action queue (32 entries, stable FIFO priority ties, maximum eight executions) with pure and `ExecuteSelected` integration coverage, plus a separate bounded pending-event queue primitive with deduplication and wrap-safe TTL expiry (not yet connected to Engine tick scheduling);
- an opt-in read-only Shadow hook and coarse candidate-only healer triage, tank peel and DPS assist strategies;
- encounter identity, versioned sequence-bearing trace records, party heartbeats, sampled trigger/candidate telemetry with originating-trigger correlation, cast request/result/terminal correlation, target changes, reservation outcomes and PartyBotAI death/resurrection events;
- Core-backed runtime fixtures for real attack, triggered and non-triggered Player spell execution, threat, movement/follow, five-Player Group snapshots, hostile discovery, death recovery and threat-impact component parity.

The current Shadow hook remains observational only: PartyBotAI still runs Legacy combat routines afterward. Shadow candidates do not own gameplay decisions or mutate world state. The repository does not yet provide the complete New Engine runtime mode; missing behavior must eventually become a traceable no-op, never a Legacy policy fallback.

## Target runtime contract

- Legacy mode runs the old AI as a complete, explicit rollback/baseline mode.
- New Engine mode gives PartyBotEngine exclusive authority across its supported AI behavior.
- New Engine mode never falls back to Legacy class, role or per-domain policy when an action is missing or unusable.
- Observe-only Shadow remains a diagnostic tool, not a mixed execution architecture.
- PartyBotAI remains the Core lifecycle/map-thread host; the Engine owns bot behavior decisions.

The current implementation does not yet provide New Engine mode. See ROADMAP.md for the next implementation milestones.

## Automated combat simulation

The Core-backed headless simulator has fixture-backed Stages A-F19 coverage. Trusted push CI [#44594126823](https://github.com/tyrael9/core/actions/runs/44594126823) passed the complete Linux ARM64 / Vanilla 5875 runtime using the configured extracted-data fixture and disposable database schemas. F19 proves one Engine action ID across the PartyBotAI adapter attempt/result and the actual Core Spell terminal, including real Lesser Heal and Mana effects. Earlier F13-F17 scenarios verify party-wide trace-budget overflow, ordinary group-member health/death/resurrection, shutdown flush, final same-map PartyBot departure and originating-trigger candidate correlation. F20's complete-report arbitration summary is implemented with pure tests and has passed full ARM64 compilation; its Core-backed runtime assertion remains pending because the current Google Drive fixture URL returned a download-quota error in [#44604943411](https://github.com/tyrael9/core/actions/runs/44604943411). The workflow supports a stable direct HTTPS archive URL plus SHA-256.

The simulator runs only in isolated CI schemas and never connects to the Oracle realm. It is a Core integration-test layer, not an alternate combat implementation; CI success is not an in-game test and does not authorize deployment. Full dungeon instance scripts and deterministic replay remain later-tier work. See CORE-COMBAT-SIMULATOR.md and TESTING.md for exact scope and gates.


## Logging is a required feature

The trace foundation is partially implemented: encounter IDs and boundaries, ordered sequence numbers, party snapshots, bounded Shadow decision summaries, sampled trigger cadence/activation telemetry, trigger-correlated candidates, cast request/result/terminal correlation, target changes, threat reservations, and PartyBotAI death/resurrection events are present. The new trigger event formatting passed fixture-backed runtime verification in [#44581612462](https://github.com/tyrael9/core/actions/runs/44581612462).

The remaining trace work is still a gate before meaningful New Engine gameplay testing: full per-bot state and boss identity, detailed per-candidate arbitration alternatives beyond the bounded sample, damage-by-damage health deltas, cross-map/instance transition closure, and integration of the repository-local report collector into the deployment diagnostics command. The fixed party-wide 1,024-detail / 256-important event budgets, shutdown flush, same-map final-PartyBot closure and one-shot terminal/overflow paths are implemented and runtime-verified. F20 adds full-report outcome counts to the bounded summary; its fixture runtime is pending a stable HTTPS extracted-data source. The local parser already emits a Markdown report and matching raw log by encounter ID or time window. See DECISION-TRACE.md.

## Build and test

Run the standalone C++14 decision-engine harness:

    bash src/game/PlayerBots/Engine/tests/run_tests.sh

The focused GitHub Actions workflow performs the pure tests and full Linux ARM64 / Vanilla 5875 Core build. CI success proves build/test evidence only; it does not prove in-game behavior and does not authorize deployment.

## Operational boundaries

No Oracle deployment, server restart, configuration change or database modification is performed as part of repository development. Deployment is a separately authorized user-operated step.
