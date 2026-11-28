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

Branch: work/partybot-engine-foundation. Latest Core source commit validated by the trusted simulator: [6a2a1eed](https://github.com/tyrael9/core/commit/6a2a1eed3baeb2d0646df8ef199faa980aacc15b); documentation/status sync [2888a6c](https://github.com/tyrael9/core/commit/2888a6c224e7e6b6bd0f7911ad1acee83826a49b) followed that source change. PR [#1](https://github.com/tyrael9/core/pull/1) remains Open / Draft against pinned base validation/partybot-engine-base-0e3ff01 (0e3ff01e76d4758e8a7c3108b2717cc785ed56fa). Keep it Draft. No merge, image publication, Oracle deployment or live-realm change is part of this work.

### Latest validation

- Source PR-safe [#2385](https://github.com/tyrael9/core/actions/runs/46184737867) for [6a2a1eed](https://github.com/tyrael9/core/commit/6a2a1eed3baeb2d0646df8ef199faa980aacc15b): **success**. Pure Engine tests, report collector, CMake Configure, full Linux ARM64 Vanilla 5875 Server/Realm build and simulator-harness syntax passed.
- Trusted push [#2384](https://github.com/tyrael9/core/actions/runs/46184732906) for the same source commit: **success**. It passed pure tests, report collector, Configure, ARM64 Server/Realm build, fixture-secret preflight and the full Core-backed simulator. F55c emitted the exact Core rejection expectedCount=2 currentCount=3; the queued-cast fixture confirmed authorization was skipped, one Failed terminal fired, the SpellEvent was reaped, and heal/mana/threat/GCD/cooldown remained unchanged. Final Runtime PASS was reached and GDB exited normally. Artifact: [#15279712331](https://github.com/tyrael9/core/actions/runs/46184732906/artifacts/15279712331).
- Documentation-only sync [2888a6c](https://github.com/tyrael9/core/commit/2888a6c224e7e6b6bd0f7911ad1acee83826a49b): push [#2386](https://github.com/tyrael9/core/actions/runs/46185217355) and PR [#2387](https://github.com/tyrael9/core/actions/runs/46185222407) both **success**. The workflow correctly skipped the heavy ARM64/runtime job for documentation-only changes; these checks do not substitute for #2384.
- Historical trusted push [#2378](https://github.com/tyrael9/core/actions/runs/46181763333) failed before F55c cast admission because F54n intentionally left a third hostile reference in the shared fixture (gate=0, refsBefore=3). The source fix [6a2a1eed](https://github.com/tyrael9/core/commit/6a2a1eed3baeb2d0646df8ef199faa980aacc15b) snapshots/restores that incoming state and isolates a clean two-reference baseline; #2384 now proves the corrected fixture reaches the intended Core rejection and passes the aggregate runtime gate.

F55c is now **trusted-runtime validated at its hostile-reference identity scope**. This remains an identity check, not a proof of changing threat values on stable references or complete helpful-proc/Script/Aura/triggered-child fan-out.

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
| 6 | Healer vertical slice | **[~]** | Priest-first forecasts, reservations, spell/effect/threat audits and F54/F55 Core safety fixtures are substantial. H1/H2 are complete at non-production policy/profile scope. H3 has trusted-runtime-validated F55a exact-target, F55b impact-time Group/Map/Instance and F55c hostile-reference identity gates. Dynamic threat-value/proc/child-effect closure, trusted cast-time acceptance/rejection and end-to-end Core reservation/resource/cooldown cleanup remain open. No healer action/strategy is production-registered. |
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
| H3 | Core cast-time admission and dynamic threat closure | **[~]** | In progress. F55a/F55b/F55c are trusted-runtime validated at their explicitly narrow exact-target, live Group/Map/Instance and hostile-reference identity scopes. Remaining are numeric threat-value/recipient-delta closure, helpful-proc and child-effect/script closure, cast-time acceptance/rejection, and end-to-end Core terminal/HealReservation/resource/cooldown cleanup before any healer action is enabled. |
| H4 | Reassess Priest adapter against the shared contract | **[ ]** | Keep production registration off until parity and complete threat closure are demonstrated. |
| H5 | Paladin Holy Light / Flash of Light scripted-heal adapter | **[ ]** | Separate ScriptEffect/child-cast audit; do not treat these as ordinary Heal-effect profiles. |
| H6 | Druid direct, HoT and hybrid slices; later channel/AoE | **[ ]** | Separate spell-shape, timing, form and recipient acceptance gates. |

### H3 progress and remaining blockers

H2 retains Shaman Lesser Healing Wave cast-time metadata (1.5 seconds) but only proves pure profile/rank selection. The whole-action threat estimator remains intentionally instant-only because hostile references and affected recipients can change while a cast is pending. F54o proves only the narrow instant, single-effect self Mana-heal admission seam; F54u proves rejection when a queued target moves to another Map/instance.

F55a extracts the exact resolved single-Unit recipient predicate into a value-only contract and makes the F54o self-heal gate reuse it. It requires one exact GUID, one Unit target, no GameObject/Item targets, exact effect mask and zero delay, no miss/reflect, and a target row that is neither deleted nor already processed. The 64-bit Core delay is preserved without narrowing. It is a safety-contract/refactoring step, **not** a party-heal admission change.

F55b adds a Core-owned context snapshot when deferred self-heal preparation is accepted: caster and intended recipient GUIDs, the shared Group ID, and Map/Instance IDs. After Core rebuilds the actual target list, but before pre-impact authorization or any irreversible effects, it re-resolves both Players and requires exact GUID continuity, the same live Group and membership of both GUIDs, the same Map and Instance, and both Players alive/in World. The Core check first preserves the old resolved-target rejection diagnostics, then applies the new context veto. `GetMap()` is not called until World/Alive checks pass, avoiding the detached-Player assertion path.

The trusted simulator now exercises both sides of F55b's Group-context gate. After the self-targeted SpellEvent is queued, the fixture removes the caster from its real Group while leaving it alive/in World on the same Map/Instance; Core rejects the impact-context mismatch before authorization, with no healing, mana, threat, GCD or cooldown effects, exactly-once Failed terminal, event reaping and complete Group/Player restoration. F55b is complete at this scoped identity-gate level. Separately, PartyBotAI registers a per-bot HealReservationTerminalListener; OnSpellCastFinished forwards Engine spell terminals before diagnostic-trace gates, and pure Engine tests cover exact reservation matching. The F55b/F55c simulator fixtures do not yet prove an end-to-end cast-time heal reservation lifecycle; no Group-target or cast-time heal is authorized.

F55c implements a narrow hostile-reference identity sub-gate. The deferred self-heal path captures the recipient's live hostile-reference source GUID set at prepare, canonicalizes it (sorted, nonzero, unique), and requires the exact same set after Core rebuilds impact targets but before authorization or deferred effects. Invalid/stale/cross-map/dead references, duplicate or zero GUIDs and any add/remove/replace drift fail closed. An empty set means only that the captured identity set is still empty; it is not proof of NoThreat. The pure contract covers equality and malformed/drifted sets. The trusted Core fixture queues a real self-heal, adds a third live hostile reference before impact while Group/Map identity remains unchanged, and requires Core's expectedCount=2 currentCount=3 rejection plus no heal/mana/threat/GCD/cooldown side effects, one Failed terminal, event reaping and fixture-state restoration. The corrected fixture ran successfully in trusted push [#2384](https://github.com/tyrael9/core/actions/runs/46184732906): the exact mismatch branch was reached, all no-side-effect/terminal/cleanup assertions passed, and the aggregate run reached final Runtime PASS with normal GDB exit. The matching PR-safe build/test run [#2385](https://github.com/tyrael9/core/actions/runs/46184737867) also passed.

F55c is **not** full dynamic threat closure: it does not snapshot or estimate changing threat values on the same references, prove every actual hostile/recipient threat delta, enumerate the complete helpful-proc/SpellScript/AuraScript/triggered-child graph, or prove cast-time recipient stability. Those remain H3 blockers. The existing Spell::prepare veto for deferred casts with m_timer > 0 remains intentionally in force; removing it before a trusted cast-time lifecycle and threat proof would widen an unproven safety boundary.

F55d next needs to close the time-dependent threat/effect envelope, then add trusted Core-backed cast-time acceptance and rejection fixtures. The fixtures must verify impact-time target/Group/Map/Instance and hostile-reference/threat revalidation; complete proc/script/child closure; no resource/GCD/cooldown/effect side effects on denial; exactly-once terminal delivery; and exact HealReservation release on success, failure and interruption. A pure-test preparation step now extends the reservation terminal-status matrix to all three Core outcomes, but that is not the end-to-end cast-time proof. No Shaman action can be registered until those gates pass. Unknown shape, target set, hostile recipient, effect/proc graph or threat owner remains a hard veto.

## Numbering: phases, F-work items and H-healer milestones

These identifiers are different axes and must not be mixed:

- **Phase N** is the project-wide roadmap stage (0–8 above).
- **Fxx / F54a…F54u / F55a…** is the engineering work-item/test-milestone sequence. F54 remains the completed historical Priest/healing safety series, with F54u its latest item. F55a, F55b and F55c are completed scoped implementation/validation slices under the still-in-progress H3 milestone. F55d is the next H3 work item for cast-time threat/effect closure and Core-backed terminal/resource/reservation lifecycle evidence; its pure reservation-status test preparation does not complete that runtime work.
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
