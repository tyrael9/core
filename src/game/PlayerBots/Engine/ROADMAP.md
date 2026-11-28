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

Current branch: `work/partybot-engine-foundation`, HEAD [`09ec3dd`](https://github.com/tyrael9/core/commit/09ec3dda874aa677bf3612b35686505cb66447a7). PR [#1](https://github.com/tyrael9/core/pull/1) remains Open / Draft against pinned base `validation/partybot-engine-base-0e3ff01` (`0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`). Keep it Draft. No merge, image publication, Oracle deployment or live-realm change is part of this work.

### Latest validation on the current HEAD

- PR-safe PartyBot Core build [#46128866209](https://github.com/tyrael9/core/actions/runs/46128866209): **success**.
- Trusted push [#46128859679](https://github.com/tyrael9/core/actions/runs/46128859679): **success**. Pure Engine tests, report-collector tests, Configure, Linux ARM64 Vanilla 5875 Server/Realm build, simulator harness syntax, fixture-secret preflight and the full Core-backed simulator all completed. The log contains the F54u PASS marker, final `Runtime PASS`, and GDB `[Inferior 1] exited normally`.
- Simulator artifact: [#15256659780](https://github.com/tyrael9/core/actions/runs/46128859679/artifacts/15256659780).
- The H2 Shaman test is a pure policy/profile test. The trusted simulator run revalidates the existing Core regression suite; it does **not** claim that a Shaman Lesser Healing Wave cast has been enabled or runtime-tested as a production action.

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
| 6 | Healer vertical slice | **[~]** | Priest-first healing forecasts, reservations, spell/effect/threat audits and F54 runtime safety fixtures are substantial and trusted-runtime tested. H1 shared policy/catalog and H2 Shaman LHW profile adapter are complete as non-production policy/profile work. H3 cast-time party-heal admission and full dynamic threat closure are not implemented. No healer action/strategy is production-registered. |
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
| H3 | Core cast-time admission and dynamic threat closure | **[ ]** | Next. Must safely support a party-target cast-time heal only after impact-time recipient re-resolution, complete per-hostile/per-recipient/child-effect threat closure, side-effect-free rejection and terminal/reservation cleanup have trusted Core fixtures. |
| H4 | Reassess Priest adapter against the shared contract | **[ ]** | Keep production registration off until parity and complete threat closure are demonstrated. |
| H5 | Paladin Holy Light / Flash of Light scripted-heal adapter | **[ ]** | Separate ScriptEffect/child-cast audit; do not treat these as ordinary Heal-effect profiles. |
| H6 | Druid direct, HoT and hybrid slices; later channel/AoE | **[ ]** | Separate spell-shape, timing, form and recipient acceptance gates. |

### Why H3 is the next blocker

H2 retains Shaman Lesser Healing Wave cast-time metadata (1.5 seconds) but only proves pure profile/rank selection. The current whole-action threat estimator is intentionally instant-only: the hostile set and affected recipients can change while a cast is pending. F54o proves a narrow Core-owned deferred pre-impact admission seam for an instant, single-effect self Mana heal; F54u proves a queued target moved to another Map/instance is rejected before authorization with one Failed terminal and no side effects. Neither grants permission for a party-target cast-time heal.

H3 must add and test the missing dynamic closure before any Shaman cast action can be registered. Unknown shape, target set, hostile recipient, effect/proc graph, or threat owner remains a hard veto.

## Numbering: phases, F-work items and H-healer milestones

These identifiers are different axes and must not be mixed:

- **Phase N** is the project-wide roadmap stage (0–8 above).
- **Fxx / F54a…F54u** is the historical engineering work-item/test-milestone sequence. F54 is the long-running Priest/healing safety and runtime-validation series; its letter suffixes identify individual implementation or regression slices. F54u is the latest validated F54 item.
- **H0 / H1 / H2 / H3…** is the healer-architecture capability sequence in [HEALER-ARCHITECTURE.md](HEALER-ARCHITECTURE.md). H1 and H2 are complete; H3 is the next architecture milestone.
- F-items can implement or validate parts of a Phase or H-milestone. There is no one-to-one numeric conversion between F54 and H3. A future H3 implementation may be split into F55a, F55b, etc.; those task IDs do not rename the H3 milestone.

## F54 latest safety boundary

F54u is trusted-runtime validated on commit `ae1f3f9` (included in current HEAD). Its five-Player fixture moves a live Group recipient to a distinct Stockades Map/instance while preserving the original Group and queued target GUID. The source Map cannot resolve that GUID; Core rejects through `CheckCast(false)` before pre-impact authorization. The terminal fires exactly once as Failed (`success=0`, `interrupted=0`), the SpellEvent is reaped, health/mana/threat/cooldown/GCD remain unchanged, and Map/instance/Group/adapter state is restored. See [TESTING.md](TESTING.md) for the detailed fixture contract and artifact.

F54u is a **negative safety proof**, not successful cross-Map healing and not broader cast permission. F54o remains the narrow instant self-heal admission proof. All healer actions remain unregistered.

## Acceptance and operational constraints

A milestone is closed only when its scoped code/docs/tests agree and the evidence level is explicit. A green compile does not prove runtime; a trusted simulator does not prove in-game quality; neither authorizes deployment.

- Keep NewEngine exclusive with no Legacy policy fallback.
- Keep unsupported/unknown actions fail-closed.
- Keep party/map-thread ownership and bounded work explicit.
- Preserve the existing owner-disconnect, instance and resurrection lifecycle regressions.
- Keep PR #1 Draft and the base pinned.
- Do not merge, publish an image, deploy Oracle or change the live realm as part of roadmap work.
