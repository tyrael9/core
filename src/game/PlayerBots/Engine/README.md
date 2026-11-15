# PartyBot Engine migration

The target architecture and adapter constraints are maintained in [ARCHITECTURE.md](ARCHITECTURE.md). Source research is recorded in [RESEARCH-CMANGOS.md](RESEARCH-CMANGOS.md) and [RESEARCH-AZEROTHCORE.md](RESEARCH-AZEROTHCORE.md). The milestone plan is in [ROADMAP.md](ROADMAP.md), and the build/manual regression procedure is in [TESTING.md](TESTING.md). Update the relevant documents in the same commit as every implementation change.

## Baseline

- Core: 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa
- Deploy wrapper: ebaa560951459419c35140e20d787ec5db2905b8
- Client: Vanilla 1.12.1 / build 5875
- Language: C++14
- Target: Linux ARM64, Oracle A1

The fork branch is pinned to the deployed Core revision. Do not rebase it onto a moving upstream branch without reviewing PartyBot and database changes.

## Current implementation status

Phase 0 is committed as lifecycle/diagnostic groundwork. It is not the new decision engine. The current DecisionTrace and legacy healer/owner fixes remain useful, but the old class-specific combat routines are not the architecture target.

The next work is source research (documented), then an exact vMaNGOS API/thread/lifetime audit, then an independent PartyBotEngine. Legacy remains the default executor until a migrated decision domain passes in-game validation.

See ROADMAP.md for checked milestones and current gates.
