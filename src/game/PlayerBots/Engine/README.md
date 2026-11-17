# PartyBotEngine

The target is a complete independent PartyBot AI for Vanilla 1.12.1 / client build 5875 on the pinned vMaNGOS Core. This is a new decision system, not a per-role rewrite that delegates unfinished behavior to the old PartyBot AI.

## Design documents

- ARCHITECTURE.md: runtime authority, strategy composition, decision loop and executor boundary.
- ROADMAP.md: development phases and acceptance criteria.
- DECISION-TRACE.md: structured encounter logs and post-fight analysis contract.
- VMANGOS-API-AUDIT.md: audited Core APIs and outstanding adapter work.
- RESEARCH-CMANGOS.md: Strategy/Trigger/Action/Value/Multiplier and ActionNode research.
- RESEARCH-AZEROTHCORE.md: class/role/encounter strategy research.
- TESTING.md: CI, in-game validation and log collection.
- CORE-COMBAT-SIMULATOR.md: headless Core-backed combat simulator design and API feasibility audit.

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
- Strategy, Trigger, Action, Value and Multiplier interfaces;
- GUID/scalar-only party snapshots and same-map snapshot collection;
- PartyCoordinator reservations and (group,map,instance) shard registry;
- deterministic candidate evaluation and basic channel arbitration;
- an opt-in read-only Shadow hook;
- coarse candidate-only healer triage, tank peel and DPS assist strategies;
- bounded Shadow diagnostics and standalone tests.

The current Shadow hook is observational only: PartyBotAI still runs Legacy combat routines afterward. The candidate actions do not cast, switch targets or mutate reservations. This is a foundation smoke test, not a usable New Engine runtime.

## Target runtime contract

- Legacy mode runs the old AI as a complete, explicit rollback/baseline mode.
- New Engine mode gives PartyBotEngine exclusive authority across its supported AI behavior.
- New Engine mode never falls back to Legacy class, role or per-domain policy when an action is missing or unusable.
- Observe-only Shadow remains a diagnostic tool, not a mixed execution architecture.
- PartyBotAI remains the Core lifecycle/map-thread host; the Engine owns bot behavior decisions.

The current implementation does not yet provide New Engine mode. See ROADMAP.md for the next implementation milestones.

## Automated combat simulation

A Core-backed headless simulator is the next test-infrastructure milestone. The pinned Core exposes test-map, creature, damage, attack and threat APIs, but global initialization requirements mean a trivial standalone unit test is insufficient. Current status and the first proof gate are documented in CORE-COMBAT-SIMULATOR.md; a first Stage A smoke-test entry point is now implemented; it creates an empty Core test map, advances one 50 ms map tick, then deletes it and has not yet been executed.

## Logging is a required feature

The current ten-second / three-candidate Shadow trace is not enough for post-fight analysis. Before meaningful New Engine combat testing, implement encounter IDs, ordered structured events, action attempt/result records, target/threat/health/death events and a bounded report collector.

A player should be able to run one read-only diagnostic command after a fight and attach the resulting report plus raw matching log lines. See DECISION-TRACE.md.

## Build and test

Run the standalone C++14 decision-engine harness:

    bash src/game/PlayerBots/Engine/tests/run_tests.sh

The focused GitHub Actions workflow performs the pure tests and full Linux ARM64 / Vanilla 5875 Core build. CI success proves build/test evidence only; it does not prove in-game behavior and does not authorize deployment.

## Operational boundaries

No Oracle deployment, server restart, configuration change or database modification is performed as part of repository development. Deployment is a separately authorized user-operated step.
