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

This is not yet wired into PartyBotAI. The live snapshot builder, map/instance coordinator registry, Shadow runtime hook and vMaNGOS action adapters are next. See ARCHITECTURE.md, VMANGOS-API-AUDIT.md and ROADMAP.md.
