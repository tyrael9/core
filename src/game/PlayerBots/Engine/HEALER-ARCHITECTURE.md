# Cross-class healer architecture

Status: design contract and rollout plan, 2026-11-28. This document does not claim that Priest healing is production-enabled or that Paladin/Shaman/Druid adapters already exist.

## Decision

Build one shared PartyBotEngine healing pipeline with explicit per-class adapters. Do not create four independent healer engines, and do not treat class-neutral infrastructure as proof that every spell is supported.

Priest remains the first safety vertical slice. Shaman Lesser Healing Wave is the next adapter trial because its six ranks use an ordinary single-target Heal effect that fits the generic profile model. Paladin follows as a separate scripted-heal capability: Holy Light and Flash of Light are not ordinary Heal-effect profiles and require explicit Core script-effect/child-cast auditing before they can enter the shared action path. Druid follows after the direct-heal contract survives another class.

## Verified current state

The branch already contains useful shared infrastructure:

- `PartySnapshot` carries class/role identity, party health and resources, map/instance identity, in-flight spell data, value-only direct-heal spell rows, bounded periodic-heal forecasts and bounded school-absorb forecasts.
- `HealingForecast` models ordered Heal/Damage events and lower/upper outcome bounds. HoT tick values preserve Core random-dither bounds rather than inventing an exact future tick.
- `PartyCoordinator` reservations prevent multiple bots from independently claiming the same uncovered heal deficit.
- `Action::EstimateThreatImpact` defaults to `Unknown`; the threat budget accepts only an epoch-matched complete estimate or explicit `NoThreat`. Unknown/incomplete fan-out remains fail-closed.
- `PlayerAI` exposes generic spell lifecycle/admission hooks, but admission is opt-in and the defaults do not authorize a cast.

These are shared mechanisms, not yet a complete shared healer implementation. Important current Priest-only gates remain:

1. `PartySnapshotBuilder.cpp` now collects value-only direct-heal profiles for healer-role Paladin, Shaman, Priest and Druid bots. Rows contain spell ID, positive direct-heal base amount, Core-calculated mana cost, actor-adjusted Core cast time and a strict `supportedSingleTargetDirect` effect-shape flag. This catalog change is in CI validation; its shape flag is not whole-action safety.
2. `HealerPolicy.h` now owns class-neutral uncovered-health accounting, incoming-credit, target triage, Heal-reservation and deterministic direct-heal rank selection. `PriestHealingPolicy.h` preserves Priest-named compatibility wrappers and the explicit Priest class gate.
3. `PriestDirectHealAction.h` and `PartyBotAI::CanCastDirectHeal` remain Priest-only. No Paladin/Shaman/Druid action adapter exists yet; a later adapter must apply its own spell allow-list and class-specific policy.
4. No healing action/strategy is registered in production NewEngine. Default spell-cast admission remains `Unmanaged`; whole-action healing threat remains fail-closed until complete proc/child-effect fan-out and the relevant Core admission shape are proven.

## Non-negotiable safety boundaries

### Spell-shape support is explicit

A class adapter may select only spell profiles whose behavior is explicitly modeled and tested. At minimum the profile must distinguish:

- direct single-target friendly healing;
- self-only healing;
- periodic healing (HoT);
- direct multi-target/group healing;
- jump/chain healing;
- hybrid direct-plus-periodic effects;
- channelled healing;
- triggered/child casts and scripted effects.

Unknown target selectors, scripts, AuraScripts, triggered child graphs, projectiles, effect fan-out or unresolved spell data do not silently degrade to a single-target heal. They are unsupported and must be rejected or remain diagnostic-only.

### F54o is not a universal healing-cast permission

The currently validated deferred-admission path is intentionally narrow: a non-triggered, non-scripted, zero-projectile-speed, instant single-effect Mana heal whose sole resolved recipient is the caster, with no delayed effect. The generic hook names do not make party-target casts, cast-time heals, chained heals, AoE heals or scripted effects safe. Expanding that Core admission shape requires its own fixtures and threat/effect proof.

### F54u is a negative safety proof

F54u validates that a queued spell targeting a Group member who moved to another Map/instance is rejected before authorization, with exactly one Failed terminal and no side effects. It does not validate successful cross-Map healing and does not broaden the admission contract.

### Whole-action threat must be complete

The estimate must enumerate every hostile/actual-recipient pair affected by the root cast and every supported triggered/child/proc effect. It is not enough to estimate only the nominal heal target or only the root spell. If any effect, recipient or threat owner is unknown, the entire action estimate is `Unknown` and the action remains blocked. A zero estimate is allowed only when the adapter can prove `NoThreat` for the complete action.

## Shared pipeline contract

The shared engine owns the lifecycle and party-level math:

1. Build an immutable, value-only party snapshot. Collect supported healer-class direct-heal spell profiles and preserve Core-calculated cost/cast time plus a conservative shape classification. No Core object pointers escape snapshot construction.
2. Compute each member's raw missing health and supported incoming healing/absorb credit. Use only conservative lower bounds for uncertain future healing.
3. Subtract current in-flight heals and active Heal reservations without double-counting the acting bot's own reservation.
4. Produce a triage candidate using generic policy inputs: effective health fraction, uncovered deficit, role priority, and deterministic tie-breaks.
5. Ask the class adapter to choose a spell from the actor's supported spell catalog and current resource budget.
6. Re-resolve live Core actor/target/spell state immediately before cast. Re-check Group membership, Map/instance, alive/online state, range, LOS, cast state, GCD/cooldown and mana cost.
7. Obtain a complete action-level threat envelope and assess all hostile/recipient pairs against current Core threat, reservations and other candidates in the report.
8. Reserve the target deficit with action/encounter correlation, start the Core cast, then reconcile/release reservations on every terminal outcome (success, failure, interruption, actor/target removal or timeout).

Shared code must not contain per-class spell IDs or assume that every healer uses mana, direct heals, one target, or the same ranking policy.

## Class adapter contract

Each adapter supplies explicit, independently testable policy for:

- **Eligibility:** supported actor class, healer role, resource model and any class state that gates healing (for example, Druid form).
- **Spell catalog:** ranks actually known by the bot, Core-calculated cost, cast time, range, effect/target shape and a support classification. Store scalar/value data only.
- **Spell selection:** class-specific rank/cost trade-offs and healing style; never assume the highest base amount is always best.
- **Forecast contribution:** the supported healing interval and timing shape of the spell; do not credit unknown scripts or triggers.
- **Threat closure:** every hostile target and threat owner affected by root and child effects, or an explicit Unknown result.
- **Live cast validation:** re-resolve Core spell and target, then verify every supported precondition. Unsupported shapes fail closed.
- **Terminal handling:** consume the shared action/encounter lifecycle and reservation contract without reinterpreting Failed as Interrupted or vice versa.

The adapter returns a value-only candidate/description to the common pipeline; it does not own a separate scheduler, reservation store, snapshot clock or threat-budget implementation.

## Second adapter: Shaman Lesser Healing Wave MVP

Use Shaman Lesser Healing Wave (six known ranks) as the next pure class-adapter trial. The Vanilla 1.12 spell profiles identify these ranks as one friendly-target Heal effect; the rank allow-list is explicit:

- 8004, 8008, 8010, 10469, 10470, 10471;
- use only rows whose snapshot also has `supportedSingleTargetDirect=true`;
- exclude Chain Heal, Healing Wave ranks with unsupported target metadata, totems, proc-triggered spells and all other Shaman healing abilities from this first adapter.

The pure adapter owns only the class/rank allow-list and shared triage/rank-selection composition. It preserves Core-calculated mana cost and cast time. It is not a Core action and cannot dispatch spells. These ranks have nonzero cast time; the current whole-action threat estimator remains instant-only, and F54o's proven admission shape remains self-only. A future action therefore stays blocked until Core cast-time recipient/threat closure and pre-impact admission are separately implemented and trusted-runtime tested.

Acceptance gates:

1. Pure tests cover the Shaman class/role guard, all six known LHW rank IDs, mana affordability, selected rank/base amount/cast time, and rejection of Chain Heal or unrelated direct-heal IDs.
2. Snapshot collection includes the learned ranks only as value data; no Core pointers escape the snapshot.
3. Core-backed tests prove a real accepted cast only after cast-time target/recipient re-resolution, terminal outcomes, mana/GCD/cooldown, complete effect/proc closure and full hostile threat fan-out are modeled.
4. Production registration remains off until all gates pass and a separate review explicitly enables it.

## Paladin scripted-heal follow-on

Holy Light (for example spell 635) and Flash of Light (for example spell 19750) are `SPELL_EFFECT_SCRIPT_EFFECT` carriers in the Vanilla 1.12 spell data, not ordinary `SPELL_EFFECT_HEAL` rows. The pinned Core's `Spell::EffectScriptEffect` contains a Paladin Flash of Light path (spell icon 242) that calculates a dithered amount, applies the Libram of Divinity/Light modifiers and triggers spell 19993 on the recipient. The generic one-target Heal profile must keep rejecting this shape. Holy Light's script-effect/DB script path also requires its own trace from the source spell through the actual child heal before an adapter may claim support.

Paladin remains in scope, but its first implementation must be a separately audited ScriptEffect adapter. Do not whitelist all Paladin ScriptEffect spells, and do not infer action-level threat closure from the nominal heal amount alone.

## Follow-on class slices

- **Shaman:** first implement the bounded Lesser Healing Wave rank family through the ordinary Heal-effect profile. Chain Heal is a separate multi-target/fan-out capability and must model jump order, range, recipient selection and per-hostile threat. Healing Wave ranks need target-mode audit where the profile is not single-target; totems are a separate subsystem.
- **Druid:** Healing Touch is a direct-heal slice; Rejuvenation is periodic; Regrowth is hybrid. Form exit/entry and mana policy are class adapter responsibilities. Tranquility is a later channelled/AoE capability.
- **Shared expansion:** add additional spell shapes only when at least one real class adapter supplies a complete profile and both pure and Core-backed fixtures validate it. Never broaden support by class label alone.

## Rollout sequence

| Milestone | Scope | Exit condition |
|---|---|---|
| H0 (this document) | Record the shared-engine / class-adapter contract and current support limits | Docs agree with current code and trusted evidence |
| H1 | Extract class-neutral missing-health, incoming-credit and reservation helpers without changing Priest decisions | Existing Priest unit/fixture behavior stays identical; generic helper tests pass |
| H2 | Implement a non-production Shaman Lesser Healing Wave profile adapter with an explicit six-rank allow-list | Pure tests prove class/rank filtering, mana and cast-time metadata; no Core dispatch |
| H3 | Extend and validate Core cast-time admission/threat closure for the Shaman direct-heal MVP | **In progress (F55a/F55b validated; F55c trusted runtime pending):** PR-safe CI passes the full ARM64 build and harness syntax for the hostile-reference GUID-set identity fixture. A first trusted run exposed fixture coupling to F54n's deliberately retained third reference; `6a2a1eed` isolates a two-reference baseline and restores the incoming set. Complete threat-value/proc/child closure and cast-time acceptance/rejection plus reservation/resource cleanup remain mandatory before any healer action is enabled |
| H4 | Reassess Priest adapter against the common contract; enable neither class by default | Review confirms parity and complete threat closure |
| H5 | Add the separate Paladin Holy Light / Flash of Light ScriptEffect adapter | Core script/child-cast behavior and complete threat effects are audited and trusted-tested |
| H6 | Add Druid direct / HoT / hybrid slices, then separately channel/AoE | Timing, form and multi-target acceptance gates pass |

## H3 progress — F55a/F55b/F55c Core impact safety

F55a extracts Core's exact resolved single Unit-target shape into `DirectHealImpactContract.h` and reuses it from the narrow F54o deferred self-heal gate. Pure tests cover exact GUID, single Unit count, zero GO/Item targets, effect mask, 64-bit zero delay, miss/reflect, and deleted/processed state. F55a does not enable party-target or cast-time healing.

F55b adds a captured impact context to the existing deferred self-heal path: caster/recipient GUID, Group ID, Map ID and Instance ID. At Core impact, after actual target-list resolution but before authorization and effects, Core re-resolves the Players and verifies both remain alive/in World, in the same exact Group and Group membership, and on the same captured Map/Instance. Pure contract tests cover Group/member/GUID/Map/Instance/World/Alive drift and valid zero-valued world Map/Instance IDs.

Trusted PR-safe [#46177348656](https://github.com/tyrael9/core/actions/runs/46177348656) and push [#46177343863](https://github.com/tyrael9/core/actions/runs/46177343863) for commit [`2390efe`](https://github.com/tyrael9/core/commit/2390efe42e8456f2bb329459f0a05354ea0376f4) both passed. The trusted Core simulator exercised both the F54o accepted positive context path and the new F55b live negative path: after a self-targeted SpellEvent was queued, the caster was removed from its real Group while remaining alive/in World on the same Map/Instance. Core emitted the new impact-context rejection for matching caster/recipient GUID and captured Group ID; authorization was skipped, failed terminal fired exactly once, there was no heal/mana/threat/cooldown/GCD effect, the event was reaped and Group/player state restored. Artifact: [#15276392748](https://github.com/tyrael9/core/actions/runs/46177343863/artifacts/15276392748). GDB exited normally and the run reached final `Runtime PASS`.

F55c adds a narrow identity-stability check to that deferred self-heal path. Core captures the exact sorted set of live hostile-reference source GUIDs attached to the recipient when prepare accepts the deferred shape. After rebuilding the impact target list but before pre-impact authorization or deferred effects, Core recollects and validates the same canonical set. Zero/duplicate GUIDs, stale or invalid sources, and any reference add/remove/replace drift veto the cast. The trusted fixture is queued before mutation, adds a third live hostile actor's threat reference, preserves Group/Map identity, and requires the Core mismatch branch (`expectedCount=2 currentCount=3`) plus no heal/mana/threat/GCD/cooldown effects, one Failed terminal, event reaping and reference/resource/adapter cleanup. Pure identity-contract tests passed. PR-safe [#2385](https://github.com/tyrael9/core/actions/runs/46184737867) passed the full ARM64 build and simulator harness syntax. Trusted push [#2384](https://github.com/tyrael9/core/actions/runs/46184732906) passed preflight tests and is still compiling. The previous trusted run [#2378](https://github.com/tyrael9/core/actions/runs/46181763333) exposed fixture coupling (`gate=0`, `refsBefore=3`): F54n intentionally leaves a third hostile reference. Source commit [`6a2a1eed`](https://github.com/tyrael9/core/commit/6a2a1eed3baeb2d0646df8ef199faa980aacc15b) now saves that original set/threat state, constructs an isolated two-reference baseline and restores the exact incoming state afterward. The new trusted runtime result is pending.

F55c's identity sub-gate must not be confused with complete dynamic threat closure. It does not account for changes in threat amounts while the hostile GUID set stays the same, nor does it prove all threat-owner/recipient deltas from helpful procs, Script/Aura triggers or child casts. It also does not enable Group-target/cast-time healing. F55b remains complete at its scoped identity-gate and fixture level; its simulator adapter does not prove production HealReservation listener cleanup. H3 remains in progress until complete threat/proc/child closure and later trusted cast-time lifecycle fixtures prove acceptance/rejection and reservation/resource/cooldown cleanup. Shaman LHW remains non-dispatchable; no healer strategy/action is registered.

## Production rule

Until a class adapter has a complete spell-shape profile, live Core preflight, complete threat envelope, terminal reconciliation and trusted Core-backed acceptance, it stays unregistered. Missing NewEngine healer behavior remains a safe no-op; it must never fall through to Legacy healer decisions.


## H1 implementation note — generalized value-only spell catalog

The follow-up to the common-policy extraction records direct-heal profiles for all four supported Vanilla healer classes (Paladin, Shaman, Priest and Druid) when their CombatBot role is Healer. Each profile now carries the learned spell ID, positive base direct-heal amount, Core-calculated mana cost, actor-adjusted Core cast time, and a `supportedSingleTargetDirect` shape bit computed from the pinned Core spell definition.

The shape bit is deliberately narrow: one positive single-target direct-heal effect with a supported explicit friendly/self target mode, excluding AoE, chain/fan-out selectors, extra non-heal effects, scripts, weapon procs, reflects and channels. It is a candidate/profile filter only. It does not prove the active aura/proc graph, child-cast closure, full hostile threat set, or whether F54o's deferred pre-impact admission hook accepts the spell. In particular, nonzero cast-time party heals are still blocked by the current threat/admission path.

The shared pure rank selector ignores profile rows without this shape bit. Priest compatibility entry points remain class-gated; a later class adapter must apply its own explicit spell allow-list and class-specific rank/cast-time policy.
