# PartyBotEngine

Independent party-coordinated AI for the pinned Vanilla 1.12.1 / build 5875 vMaNGOS Core. The design borrows the Strategy / Trigger / Action / Value / Multiplier model from CMaNGOS PlayerBots and encounter-strategy ideas from AzerothCore, but all runtime behavior is implemented against the pinned vMaNGOS C++14 APIs.

## Current status (2026-11-28 PT)

**Overall position: Phase 6 / Healer in progress.** Project foundations Phases 0–5 are established; Phase 7 combat coordination is partial and Phase 8 encounter strategies have not started. This repository is not yet a complete/playable autonomous combat AI.

**Current branch:** `work/partybot-engine-foundation`. Current validated code HEAD: [`2390efe`](https://github.com/tyrael9/core/commit/2390efe42e8456f2bb329459f0a05354ea0376f4). PR [#1](https://github.com/tyrael9/core/pull/1) remains Open / Draft against the pinned Vanilla 5875 validation base.

**Latest validation on code commit `2390efe`:**
- PR-safe [#46177348656](https://github.com/tyrael9/core/actions/runs/46177348656): **success**, including pure Engine tests, report collector, Configure, full Linux ARM64 Vanilla 5875 Server/Realm build and simulator-harness syntax.
- Trusted push [#46177343863](https://github.com/tyrael9/core/actions/runs/46177343863): **success**, including fixture-backed Core simulator, F55b real Group-drift rejection marker, final `Runtime PASS`, artifact [#15276392748](https://github.com/tyrael9/core/actions/runs/46177343863/artifacts/15276392748) and normal GDB exit.
- F55b's trusted negative fixture removed the still-live/self-targeted caster from its actual Group after the SpellEvent was queued; the new Core impact-context rejection fired before authorization, with no heal/mana/threat/GCD/cooldown effects and exactly-once failed terminal. Group and Player state were restored.

**Latest healer state:** H1/H2 are complete at their non-production policy/profile scope. F55a/F55b are complete at their scoped exact-target and impact-context safety gates. H3 remains in progress: dynamic hostile/proc/child-effect threat closure and trusted cast-time acceptance/rejection with terminal/reservation/resource/cooldown lifecycle proof remain open. No healer action or strategy is production-registered.

**Production behavior:** only `FollowPartyMemberAction` and `StopFollowingAction` are registered in production NewEngine. Legacy remains the default runtime mode. The remaining generic CoreAction adapters and all combat/healer/encounter policies are not production gameplay behavior.

For the authoritative phase matrix, milestone mapping, exact safety boundaries and next gates, see [ROADMAP.md](ROADMAP.md).

## Validation snapshot

- F42 PR CI [#44839755150](https://github.com/tyrael9/core/actions/runs/44839755150) passed pure Engine tests, report collector, configure, full ARM64 Core build and harness syntax. Trusted push [#44839748022](https://github.com/tyrael9/core/actions/runs/44839748022) additionally ran the full fixture-backed two-ON_USE-slot aggregate and reached final Runtime PASS, closing the initial root/child/mutation aggregation slice. The final delayed ON_USE cancellation-before/after-charge-mutation coverage and full F42 closure are validated by [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749) below.
- F43 seventh generic action / AttackTargetAction: PR [#44886273992](https://github.com/tyrael9/core/actions/runs/44886273992) and trusted push [#44886268553](https://github.com/tyrael9/core/actions/runs/44886268553) passed. The Core runtime reported `registered=1 selected=1 accepted=1 noImmediateDamageOrThreat=1 staleFriendlyRejected=1 selectionDriftRejected=1`, the AttackTargetAction PASS marker and final `Runtime PASS`. Artifact: [partybot-simulator-ca634bc](https://github.com/tyrael9/core/actions/runs/44886268553/artifacts/14699344464).
- F43 sixth generic action: PR [#44874582915](https://github.com/tyrael9/core/actions/runs/44874582915) and trusted push [#44874579164](https://github.com/tyrael9/core/actions/runs/44874579164) passed. FaceTarget verified `requestAccepted=1 facingComplete=1 positionPreserved=1 targetStatePreserved=1 alreadyFacingNoOp=1 staleFriendlyRejected=1 movingRejected=1` and final Runtime PASS. Artifact: [partybot-simulator-f6986c8](https://github.com/tyrael9/core/actions/runs/44874579164/artifacts/14695646802).
- F43 first generic action PR [#44845475962](https://github.com/tyrael9/core/actions/runs/44845475962) and trusted runtime [#44845469020](https://github.com/tyrael9/core/actions/runs/44845469020) passed.
- F43 two-action head [#44847028717](https://github.com/tyrael9/core/actions/runs/44847028717) passed PR tests/build; trusted push [#44847023073](https://github.com/tyrael9/core/actions/runs/44847023073) passed full Core simulator, including idle selection cleanup, hostile acceptance, stale-friendly rejection and cleanup.
- F43 three-action head [#44849541737](https://github.com/tyrael9/core/actions/runs/44849541737) passed PR tests/build; trusted push [#44849535626](https://github.com/tyrael9/core/actions/runs/44849535626) passed full Core simulator with `registered=1 attackableNotProposed=1 staleSelected=1 staleRejected=1 liveUnattackable=1 selected=1 stoppedOnly=1` and final Runtime PASS.
- F42 delayed-use/cancellation closure / head `dfdfdf08628d64517f0e1b25e17e6029acd31c44`: PR [#44852146750](https://github.com/tyrael9/core/actions/runs/44852146750) and trusted push [#44852141749](https://github.com/tyrael9/core/actions/runs/44852141749) passed. The trusted runtime proves pre-mutation cancel (`-5>-5`), post-mutation cancel (`-5>-4`), Interrupted child terminals, mutation-before-effects ordering, no delayed target effect, and final Runtime PASS. Artifact: [partybot-simulator-dfdfdf0](https://github.com/tyrael9/core/actions/runs/44852141749/artifacts/14685618923).
- Fourth generic action / head `f73001882d3bdde5695e481e7af06e6964faba52`: PR [#44860033826](https://github.com/tyrael9/core/actions/runs/44860033826) and trusted push [#44860028963](https://github.com/tyrael9/core/actions/runs/44860028963) passed. Follow fixture marker: `registered=1 firstStarted=1 followTargetCaptured=1 sameTargetNotRestarted=1 retargetRegistered=1 retargeted=1 firstLeaderRemoved=1 staleSelected=1 staleGroupRejected=1`; F24 trace scoping and final Runtime PASS also passed. Artifact: [partybot-simulator-f730018](https://github.com/tyrael9/core/actions/runs/44860028963/artifacts/14689004218).
- Fifth generic action / head `221e5a30f7ecdf8b29c05bc060cc63379dabe8d5`: PR build [#44864129963](https://github.com/tyrael9/core/actions/runs/44864129963) and trusted push [#44864122550](https://github.com/tyrael9/core/actions/runs/44864122550) passed. The trusted runtime emitted `registered=1 snapshot=1 staleTargetRejected=1 stopped=1 pointMovementPreserved=1`, passed F24 and reached final Runtime PASS. Artifact: [partybot-simulator-221e5a3](https://github.com/tyrael9/core/actions/runs/44864122550/artifacts/14691023636).
- Current F43 tenth adapter / MovePointAction: PR [#44943026168](https://github.com/tyrael9/core/actions/runs/44943026168) passed the ARM64 build; trusted push [#44943021972](https://github.com/tyrael9/core/actions/runs/44943021972) passed the full simulator with a matching typed Reached terminal and final Runtime PASS. Artifact [partybot-simulator-1f21304216cb6cf349bcede7c5e486def015722b](https://github.com/tyrael9/core/actions/runs/44943021972/artifacts/14722896312).
- Current F43 eleventh adapter / StopPointMovementAction: PR [#44945186718](https://github.com/tyrael9/core/actions/runs/44945186718) passed ARM64 build; trusted push [#44945182411](https://github.com/tyrael9/core/actions/runs/44945182411) passed the full Core simulator with A/B movement-stack, stale-snapshot rejection, legacy exclusion and exact C cancellation assertions. Artifact [partybot-simulator-22b6b5cc57845e54ed09ba5f921be8316aa15817](https://github.com/tyrael9/core/actions/runs/44945182411/artifacts/14723521850).
- Current F43 twelfth adapter / DismountForCombatAction: PR [#44964399919](https://github.com/tyrael9/core/actions/runs/44964399919) passed the full ARM64 build; trusted push [#44964395002](https://github.com/tyrael9/core/actions/runs/44964395002) passed the fixture-backed simulator with a DBC-backed mount, accepted dismount, already-dismounted rejection, live target-selection drift rejection, cleanup and taxi-flight exclusion, then final `Runtime PASS`. Artifact [partybot-simulator-3fb1621fccbaecd5ef0f88313bf66af3cfdaa034](https://github.com/tyrael9/core/actions/runs/44964395002/artifacts/14731595715).
- Current F43 fifteenth adapter / StopChaseAction and sixteenth / MoveDistanceAction: trusted push #44998862867 (https://github.com/tyrael9/core/actions/runs/44998862867) and PR ARM64 build #44998866883 (https://github.com/tyrael9/core/actions/runs/44998866883) passed; MoveDistance verified exact Follow and Chase preservation/restoration, Point and victim-race rejection, cleanup and final Runtime PASS. Artifact: partybot-simulator-2beecf67 (https://github.com/tyrael9/core/actions/runs/44998862867/artifacts/14744887692).
- Historical F43 fourteenth adapter / StopStaleChaseAction: trusted push [#44991193867](https://github.com/tyrael9/core/actions/runs/44991193867) passed exact-generator cleanup, underlying Follow expiry, unrelated Point rejection/preservation, replacement-Chase race rejection and cleanup, then final Runtime PASS. Matching PR ARM64 build [#44991198073](https://github.com/tyrael9/core/actions/runs/44991198073) passed. ChaseTargetAction's complete matrix is also closed by trusted push [#44990342234](https://github.com/tyrael9/core/actions/runs/44990342234).
- PR #1 remains Draft on the pinned Vanilla 5875 validation base. No image was published and no Oracle deployment or live-realm test was performed.

## Phase status

See [ROADMAP.md](ROADMAP.md) for the full Phase 0–8 status matrix and acceptance gaps.

- **Phases 0–2.5:** pinned baseline, lifecycle-host foundation, generic decision engine and Core-backed fixture infrastructure are established.
- **Phase 3:** trace/reporting is useful but intentionally bounded; richer AI/Boss state and deployment-side integration remain open.
- **Phase 4:** exclusive NewEngine execution and 17 generic CoreAction adapters are tested; production registration remains limited to Follow and StopFollowing.
- **Phase 5:** party formation and death/resurrection/map-instance lifecycle slice is closed.
- **Phase 6:** healer work is active. H1 and H2 are complete at policy/profile scope; H3 has F55a's exact recipient contract and F55b's trusted-runtime-validated Core impact-time identity gate. Party-target cast-time admission and dynamic threat closure remain open. No healer strategy is production-enabled.
- **Phase 7:** combat coordination is partial.
- **Phase 8:** encounter strategies are not started.

## Design documents

- `ARCHITECTURE.md`: runtime authority, responsibilities, strategy composition and execution boundary.
- `ROADMAP.md`: phases, milestones, open gates and acceptance criteria.
- `ACTION-QUEUE.md`: immediate/pending dispatch, continuations, prerequisites and bounded-queue semantics.
- `DECISION-TRACE.md`: encounter event schema, budgets and post-fight analysis contract.
- `VMANGOS-API-AUDIT.md`: pinned Core update/threading and action-adapter API audit.
- `CORE-COMBAT-SIMULATOR.md`: isolated fixture-backed simulator and its exact evidence boundaries.
- `ITEM-USE-AGGREGATE.md`: F40 Core API audit and F42's implemented, runtime-validated multi-spell root/child/mutation contract, including delayed cancellation before/after charge mutation.
- `TESTING.md`: CI paths, simulator execution, test-realm checks and validation vocabulary.
- `THREAT-BUDGETS.md`: Core-authoritative aggro semantics and intentionally fail-closed threat-impact estimation.
- `RESEARCH-CMANGOS.md` / `RESEARCH-AZEROTHCORE.md`: architectural research only; no foreign runtime dependency or WotLK data is imported.

## Pinned baseline

- Core: `0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`
- Deploy wrapper reference: `ebaa560951459419c35140e20d787ec5db2905b8`
- Client/data: Vanilla 1.12.1 / build 5875
- Language: C++14
- Target: Linux ARM64, Oracle A1 (1 OCPU / 6 GB)

Do not rebase onto moving upstream without auditing PartyBot, lifecycle and database changes.

## Build and test

Run the standalone C++14 decision-engine harness from the repository root:

```sh
bash src/game/PlayerBots/Engine/tests/run_tests.sh
```

The focused PartyBot Core workflow also runs report-collector tests and a full Linux ARM64 / Vanilla 5875 Core build. Trusted branch pushes additionally run the fixture-backed simulator when the extracted-data URL and checksum secrets are available. PR jobs deliberately do not receive those fixture secrets.

A CI pass is not an in-game test and does not authorize deployment.

## Operational boundaries

The simulator uses disposable `partybot_sim_*` database schemas and never connects to the Oracle realm. Repository development does not publish an image, restart a server, change deployment configuration or mutate live database volumes. Image publication and deployment remain separate, explicitly authorized operations.


### F54e–F54g triggered-heal boundary

F54e recipient-side helpful-proc discovery/classification passed trusted Core runtime in push #45581482050. F54f then passed real delayed `PROC_TRIGGER_SPELL` child healing on both caster and recipient sides in trusted push #45716916204; root + child effective healing reconciled on both paths and both hostile ThreatManagers matched the Core-derived threat deltas. F54g passed the ordinary successful-target path for `SPELL_AURA_ADD_TARGET_TRIGGER`: a real five-Player Group Prayer of Healing cast dispatched one Lesser Heal child to each of five successful recipients, with exact two-hostile threat parity in trusted push #45740565661. A separate trusted fixture then applied Core school immunity to one selected Group member; the root and child each affected exactly four successful recipients, the immune member remained unhealed, actual threat matched the four successful recipients, and the temporary immunity/Aura/SpellEntry state was cleared/restored in push #45744991152. Deleted-target and successful reflect-to-caster branches remain source-audited only; arbitrary nested-child closure is not claimed. `PriestDirectHealAction` remains unregistered and its whole-action threat estimate remains Unknown.
