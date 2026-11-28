# PartyBotEngine roadmap

The product goal is a complete, independent PartyBot AI for the pinned Vanilla 1.12.1 / build 5875 vMaNGOS Core. In NewEngine mode the Engine is the sole AI decision authority: unsupported behavior must be a deliberate no-op, never a fallback to Legacy policy.

Legacy PartyBot fixes are maintenance work and do not count as NewEngine milestones.

## Status vocabulary

- **[x] Complete (scoped):** the phase's stated foundation/slice is implemented and supported by the evidence listed here. This does not imply every future extension is complete.
- **[~] In progress:** meaningful implementation exists, but named acceptance gaps remain.
- **[ ] Not started:** no substantive implementation for that phase yet.
- Compile CI, trusted Core simulator, in-game testing and deployment are separate evidence levels.

## Current overall position — 2026-11-28 PT

**The project is in Phase 6 (Healer), with Phase 0–5 foundations established.** Phase 7 combat coordination is partial; Phase 8 encounter strategies have not started. The repository is not yet a complete/playable autonomous combat AI.

Branch: `work/partybot-engine-foundation`. Current submitted HEAD: [`3bd3af5`](https://github.com/tyrael9/core/commit/3bd3af598193f3fc5fdc3d5f64a47fe19205fcd0), adding the F55b live-context runtime assertion and detached-Player safety ordering. The immediately preceding code revision [`da38917`](https://github.com/tyrael9/core/commit/da38917408db886946a4752c7357e3cd01094a9b) passed both build workflows and the trusted Core simulator; CI for `3bd3af5` is pending. PR [#1](https://github.com/tyrael9/core/pull/1) remains Open / Draft against pinned base `validation/partybot-engine-base-0e3ff01` (`0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`). Keep it Draft. No merge, image publication, Oracle deployment or live-realm change is part of this work.

### Latest validation

- F55b implementation revision [`da38917`](https://github.com/tyrael9/core/commit/da38917408db886946a4752c7357e3cd01094a9b): PR-safe [#46134339779](https://github.com/tyrael9/core/actions/runs/46134339779) and trusted push [#46134335966](https://github.com/tyrael9/core/actions/runs/46134335966) both **success**. Pure Engine tests, report collector, Configure, full Linux ARM64 Vanilla 5875 Server/Realm build and simulator harness syntax passed. Trusted push additionally ran the fixture-backed Core simulator to final `Runtime PASS`, uploaded [artifact #15258974889](https://github.com/tyrael9/core/actions/runs/46134335966/artifacts/15258974889), and exited normally under GDB.
- That trusted run proves the pre-existing F54o/F54q/F54r/F54s/F54t/F54u regressions remain intact. Those F54 recipient-drift fixtures are rejected by the earlier resolved-target/CheckCast gates; they are **not** a dedicated negative execution of the new F55b Group-context rejection branch.
- F55b pure tests directly cover Group ID/member changes, caster/recipient GUID drift, Map/instance drift, out-of-world/dead state, plus valid Map 0 / Instance 0. The F54o accepted cast also traverses the new Core context check positively. Commit `3bd3af5` adds a dedicated Core log + simulator-runner assertion for that positive path and moves IsInWorld/Alive checks before GetMap(); its CI is still pending.
- No live Core Group-drift rejection fixture exists yet. F55b remains in progress until that negative branch is exercised in trusted runtime. H2 remains pure policy/profile work: no Shaman Lesser Healing Wave cast has been enabled or runtime-tested as a production action.

## Project-wide phase status

| Phase | Scope | Status | What is actually established / what remains |
|---|---|---|---|
| 0 | Pinned baseline and repository foundation | **[x]** | Vanilla 1.12.1 / build 5875 Core baseline is pinned; branch/PR validation foundation exists. |
| 1 | Core lifecycle host and integration boundary | **[~]** | PartyBot lifecycle hosting and operational boundaries are established, including lifecycle regression coverage. Action-specific Core API audits and some operational integration remain ongoing. |
| 2 | Generic decision-engine foundation | **[x]** | Strategy / Trigger / Action / Value / Multiplier, typed snapshots/cache, trigger scheduling, deterministic arbitration, bounded immediate/pending queues, dependencies/continuations and result handling are implemented. |
| 2.5 | Core-backed test infrastructure | **[x]** | Isolated Vanilla 5875 simulator exercises real Core Players, Groups, spells, threat, movement, items, death/recovery and map/instance transitions. It is a fixture/test harness, not a full encounter simulator. |
| 3 | Encounter trace and post-fight diagnostics | **[~]** | Bounded structured trace, lifecycle/decision events and local read-only report collector exist. Richer AI/Boss state, some ordinary-member per-hit details, wider trace coverage and deployment-side diagnostics integration remain open. |
| 4 | NewEngine authority, dispatch and generic Core actions | **[~]** | Exclusive NewEngine/no-Legacy-fallback mode, map-thread resolution, action-specific preflight and 17 generic CoreAction adapters are implemented/tested. Only `FollowPartyMemberAction` and `StopFollowingAction` are registered in production NewEngine; Legacy remains the default runtime mode. Most generic adapters and combat policies are not production behavior. |
| 5 | Party coordination lifecycle and formation | **[x]** | Stable formation slots, leader handoff, real five-Player/four-Bot same-map formation, wipe/death/resurrection recovery, reservations and source/destination map/instance lifecycle have trusted Core simulator coverage. |
| 6 | Healer vertical slice | **[~]** | Priest-first forecasts, reservations, spell/effect/threat audits and F54 safety fixtures are substantial. H1/H2 are complete at non-production policy/profile scope. H3 now includes F55a's resolved-target contract and F55b's Core impact-time Group/Map/Instance identity gate; the dedicated live negative Group-drift fixture, cast-time party admission and full dynamic threat closure remain open. No healer action/strategy is production-registered. |
| 7 | Party-wide combat coordination | **[~]** | Hostile snapshots, threat budgets/reservations and initial Tank/DPS assist policy experiments exist. Full target/add assignment, healer distribution, CC/interrupt/dispel coordination and robust encounter-wide arbitration remain open. |
| 8 | Vanilla dungeon/raid encounter strategies | **[ ]** | Not started. No dungeon/raid encounter strategy set is implemented. |

### Production behavior boundary

Production NewEngine currently registers only:
- `FollowPartyMemberAction`
- `StopFollowingAction`

The generic adapter library is broader than its production registrations. No Priest, Shaman, Paladin or Druid healing strategy/action is registered; attack, CC, interrupt, dispel and encounter behaviors must not be inferred from simulator-only policy tests. Legacy remains the default mode. NewEngine, when explicitly selected, is exclusive and never falls through to Legacy decisions.

## Phase 6 — healer architecture milestones

Phase 6 is a Priest-first safety vertical slice and a staged shared pipeline, **not** an all-class healer implementation.

| Healer milestone | Scope | Status | Evidence / boundary |
|---|---|---|---|
| H0 | Shared healer contract and rollout design | **[x]** | Recorded in [HEALER-ARCHITECTURE.md](HEALER-ARCHITECTURE.md). |
| H1 | Class-neutral policy and value-only healer spell catalog | **[x]** | `HealerPolicy.h` extracts missing-health, incoming-credit, triage, reservation and rank selection. Snapshot catalog covers healer-role Priest, Paladin, Shaman and Druid with Core mana/cast-time metadata and a strict supported-shape bit. Pure tests and trusted Core CI pass. |
| H2 | Shaman Lesser Healing Wave profile adapter | **[x]** | Pure policy adapter with explicit six-rank allow-list (8004, 8008, 8010, 10469, 10470, 10471); tests class/role, rank, affordability, fallback and cast-time propagation. No spell dispatch or production action. |
| H3 | Core cast-time admission and dynamic threat closure | **[~]** | In progress. F55a's resolved-target shape and F55b's Core impact-time Group/map/instance identity gate are implemented. Pure positive/negative contract tests pass; the trusted F54o cast traverses the live Core positive gate, with a dedicated assertion being added. A live negative Group-drift fixture, complete hostile/proc/child-effect threat closure, and trusted cast-time acceptance/rejection/cleanup fixtures remain. |
| H4 | Reassess Priest adapter against the shared contract | **[ ]** | Keep production registration off until parity and complete threat closure are demonstrated. |
| H5 | Paladin Holy Light / Flash of Light scripted-heal adapter | **[ ]** | Separate ScriptEffect/child-cast audit; do not treat these as ordinary Heal-effect profiles. |
| H6 | Druid direct, HoT and hybrid slices; later channel/AoE | **[ ]** | Separate spell-shape, timing, form and recipient acceptance gates. |

### H3 progress and remaining blockers

H2 retains Shaman Lesser Healing Wave cast-time metadata (1.5 seconds) but only proves pure profile/rank selection. The whole-action threat estimator remains intentionally instant-only because hostile references and affected recipients can change while a cast is pending. F54o proves only the narrow instant, single-effect self Mana-heal admission seam; F54u proves rejection when a queued target moves to another Map/instance.

F55a extracts the exact resolved single-Unit recipient predicate into a value-only contract and makes the F54o self-heal gate reuse it. It requires one exact GUID, one Unit target, no GameObject/Item targets, exact effect mask and zero delay, no miss/reflect, and a target row that is neither deleted nor already processed. The 64-bit Core delay is preserved without narrowing. It is a safety-contract/refactoring step, **not** a party-heal admission change.

F55b adds a Core-owned context snapshot when deferred self-heal preparation is accepted: caster and intended recipient GUIDs, the shared Group ID, and Map/Instance IDs. After Core rebuilds the actual target list, but before pre-impact authorization or any irreversible effects, it re-resolves both Players and requires exact GUID continuity, the same live Group and membership of both GUIDs, the same Map and Instance, and both Players alive/in World. The Core check first preserves the old resolved-target rejection diagnostics, then applies the new context veto. `GetMap()` is not called until World/Alive checks pass, avoiding the detached-Player assertion path.

Pure tests reject GUID, Group, membership, Map/Instance, out-of-World and death drift and preserve legitimate Map 0 / Instance 0. Trusted F54o runtime proves the live positive context path; commit `3bd3af5` adds an explicit simulator assertion for that path. The trusted simulator has not yet exercised the new negative Group-context branch; older F54s/t/u negative fixtures fail earlier at Core target resolution/CheckCast and must not be counted as F55b evidence. Therefore F55b remains **in progress** until a dedicated queued-cast fixture mutates Group membership while keeping the currently resolved self-target shape intact.

Next, F55b's remaining negative runtime fixture must complete before F55c establishes full dynamic hostile-reference, helpful-proc and child-effect threat closure. F55d should then add trusted cast-time acceptance/rejection fixtures covering side-effect-free denial plus terminal, reservation, resource and cooldown cleanup. No Shaman action can be registered until those gates pass. Unknown shape, target set, hostile recipient, effect/proc graph or threat owner remains a hard veto.

## Numbering: phases, F-work items and H-healer milestones

These identifiers are different axes and must not be mixed:

- **Phase N** is the project-wide roadmap stage (0–8 above).
- **Fxx / F54a…F54u / F55a…** is the engineering work-item/test-milestone sequence. F54 remains the completed historical Priest/healing safety series, with F54u its latest item. F55a and F55b are scoped implementation slices under the still-in-progress H3 milestone.
- **H0 / H1 / H2 / H3…** is the healer-architecture capability sequence in [HEALER-ARCHITECTURE.md](HEALER-ARCHITECTURE.md). H1 and H2 are complete at policy/profile scope; H3 is now in progress, not complete.
- F-items can implement or validate parts of a Phase or H-milestone. There is no one-to-one numeric conversion between F54 and H3; F55a/F55b are task IDs nested under H3, not renamings of the H3 capability.

## F54 latest safety boundary

F54u is trusted-runtime validated on commit `ae1f3f9` (included in current HEAD). Its five-Player fixture moves a live Group recipient to a distinct Stockades Map/instance while preserving the original Group and queued target GUID. The source Map cannot resolve that GUID; Core rejects through `CheckCast(false)` before pre-impact authorization. The terminal fires exactly once as Failed (`success=0`, `interrupted=0`), the SpellEvent is reaped, health/mana/threat/cooldown/GCD remain unchanged, and Map/instance/Group/adapter state is restored. See [TESTING.md](TESTING.md) for the detailed fixture contract and artifact.

F54u is a **negative safety proof**, not successful cross-Map healing and not broader cast permission. F54o remains the narrow instant self-heal admission proof. All healer actions remain unregistered.

## F55a — exact resolved single-recipient impact contract

Code commit [`f7d5201`](https://github.com/tyrael9/core/commit/f7d52013803ad3fa7b34a6330a200f9a54530b25) adds `DirectHealImpactContract.h`, a value-only predicate for the exact Core-resolved target row. `Spell::IsSafePartyBotDeferredSelfHealShape()` now feeds its resolved `TargetInfo` and Unit/GO/Item target counts into that shared predicate. The accepted F54o shape is unchanged: the expected recipient remains the caster, exactly one Unit target is present, no GO/Item target exists, effect mask is exactly `0x01`, delay is zero, miss/reflect are absent, and the target row is neither deleted nor processed.

Pure Engine coverage includes every positive/negative condition and a delay greater than 32 bits, proving Core's `uint64` impact delay is not narrowed or allowed to wrap into the instant path. The final F55a code revision passed PR-safe [#46132022118](https://github.com/tyrael9/core/actions/runs/46132022118) and trusted push [#46132017347](https://github.com/tyrael9/core/actions/runs/46132017347). The trusted run completed the existing fixture-backed simulator through F54u, final `Runtime PASS` and normal GDB exit; it does not contain a dedicated F55a marker or prove a successful party-target/cast-time heal.

F55a does not prove Group membership, map/instance identity, dynamic hostile-reference fan-out, proc/child-effect closure or whole-action threat completeness. It does not broaden F54o, enable Shaman LHW, or register a healer action. H3 remains in progress.

## Acceptance and operational constraints

A milestone is closed only when its scoped code/docs/tests agree and the evidence level is explicit. A green compile does not prove runtime; a trusted simulator does not prove in-game quality; neither authorizes deployment.

- Keep NewEngine exclusive with no Legacy policy fallback.
- Keep unsupported/unknown actions fail-closed.
- Keep party/map-thread ownership and bounded work explicit.
- Preserve the existing owner-disconnect, instance and resurrection lifecycle regressions.
- Keep PR #1 Draft and the base pinned.
- Do not merge, publish an image, deploy Oracle or change the live realm as part of roadmap work.
