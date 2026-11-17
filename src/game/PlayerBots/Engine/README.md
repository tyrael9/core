# PartyBot Engine migration

The target architecture and API boundaries are maintained in [ARCHITECTURE.md](ARCHITECTURE.md) and [VMANGOS-API-AUDIT.md](VMANGOS-API-AUDIT.md). Source research is recorded in [RESEARCH-CMANGOS.md](RESEARCH-CMANGOS.md) and [RESEARCH-AZEROTHCORE.md](RESEARCH-AZEROTHCORE.md). The milestone plan is in [ROADMAP.md](ROADMAP.md), and build/manual regression procedures are in [TESTING.md](TESTING.md). Update the relevant documents in the same commit as every implementation change.

## Baseline

- Core: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa
- Deploy wrapper: ebaa560951459419c35140e20d787ec5db2905b8
- Client: Vanilla 1.12.1 / build 5875
- Language: C++14
- Target: Linux ARM64, Oracle A1

The fork branch is pinned to the deployed Core revision. Do not rebase it onto a moving upstream branch without reviewing PartyBot and database changes.

## Current status

Phase 0 lifecycle/diagnostic groundwork is committed; it is not the new decision engine. CMaNGOS and AzerothCore source research and the first vMaNGOS API audit are documented. The remaining API call-chain/lifetime audit precedes the independent PartyBotEngine implementation.

Legacy remains the default executor until each migrated decision domain passes in-game validation. See ROADMAP.md for the current gate.


## Independent engine foundation

The first independent C++14 decision layer is committed: value-only snapshots, reservation storage, Strategy/Trigger/Action/Value/Multiplier interfaces, deterministic candidate evaluation and standalone tests. Evaluation is read-only; execution is a separate explicit call.

The live same-map snapshot builder, weakly indexed coordinator registry and opt-in read-only Shadow hook are wired into PartyBotAI. Shadow is disabled by default; when enabled, Legacy remains the only executor. Candidate-only healing-triage, tank-peel and DPS-assist strategies run on copied role/party/hostile data. Their selected candidates are diagnostic proposals only; no Strategy action is executed. See ARCHITECTURE.md, VMANGOS-API-AUDIT.md and ROADMAP.md.


## Live snapshot/coordinator primitives

The same-map snapshot builder and weakly indexed (group,map,instance) coordinator registry are connected to PartyBotAI. Each bot retains its shard only while on that map/instance, and Shadow evaluation runs from the map update. The runtime flag defaults off; no new Strategy or Action adapters are registered yet.


## Role-aware Shadow strategies

Candidate-only strategies register on demand when Shadow is enabled:
- healer: health-based party triage;
- tank/main tank: pick-up proposals for hostiles attacking non-tank party members;
- melee/ranged damage: assist-target proposals weighted toward the Main Tank's current target.

The snapshot copies same-map bot roles, the Group Main Tank flag, party members' current victims, attackers and threat entries. Shadow logs up to three detailed candidate decisions after each summary. No action adapter performs a cast, target switch or reservation mutation.
