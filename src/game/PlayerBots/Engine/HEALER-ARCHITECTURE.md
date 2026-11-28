# Cross-class healer architecture

Status: architecture contract and implementation snapshot, 2026-11-28. H1 and H2 are implemented at shared-policy/profile scope; no healer cast action or strategy is production-enabled.

## Decision

Build one shared PartyBotEngine healing pipeline with explicit per-class adapters. Do not create four independent healer engines, and do not treat class-neutral infrastructure as proof that every spell is supported.

Priest remains the first safety vertical slice. Shaman Lesser Healing Wave is the next adapter trial because its six ranks use an ordinary single-target Heal effect that fits the generic profile model. Paladin follows as a separate scripted-heal capability: Holy Light and Flash of Light are not ordinary Heal-effect profiles and require explicit Core script-effect/child-cast auditing before they can enter the shared action path. Druid follows after the direct-heal contract survives another class.

## Verified current state

The branch contains the following shared infrastructure and validated limits:

- `PartySnapshot` carries class/role identity, party health/resources, map/instance identity, in-flight spell data, value-only direct-heal spell rows, bounded periodic-heal forecasts and bounded school-absorb forecasts.
- `HealingForecast` models ordered Heal/Damage events and lower/upper outcome bounds. HoT ticks preserve Core random-dither bounds rather than inventing an exact future tick.
- `PartyCoordinator` reservations prevent multiple bots from independently claiming the same uncovered heal deficit.
- `Action::EstimateThreatImpact` defaults to `Unknown`; the threat budget accepts only an epoch-matched complete estimate or explicit `NoThreat`. Unknown/incomplete fan-out remains fail-closed.
- `PlayerAI` exposes generic spell lifecycle/admission hooks, but admission is opt-in and defaults do not authorize a cast.

Completed at their stated scope:

1. **H1 — shared policy and catalog:** `HealerPolicy.h` owns class-neutral missing-health accounting, incoming credit, target triage, Heal reservations and direct-heal rank selection. `PriestHealingPolicy.h` retains Priest compatibility wrappers and class gating. `PartySnapshotBuilder` captures value-only direct-heal profiles for healer-role Paladin, Shaman, Priest and Druid bots, including Core mana cost, cast time and the strict supported-single-target-direct shape flag.
2. **H2 — Shaman profile adapter:** `ShamanHealingPolicy.h` allows only Lesser Healing Wave ranks 8004, 8008, 8010, 10469, 10470 and 10471. Pure tests cover class/role, allow-list, affordability, rank fallback and cast-time propagation. Chain Heal and unrelated rows are rejected.
3. **Priest safety evidence:** the F54 fixtures validate selected forecast, healing, aura, proc/child, threat, queued-cast, interruption and recipient-lifecycle boundaries, including cross-Map rejection at F54u.

Still Priest-only or not yet authorized:

1. `PriestDirectHealAction.h` and `PartyBotAI::CanCastDirectHeal` remain Priest-only. H2 is a Shaman policy/profile adapter, not a Shaman runtime action.
2. The proven F54o Core admission shape is a non-triggered, non-scripted, instant, single-effect self Mana heal with exactly one resolved recipient (the caster) and zero impact delay. It does not authorize party-target or cast-time heals.
3. The whole-action threat estimator remains fail-closed for nonzero cast time because the hostile and recipient sets may change during the wind-up.
4. No healer action/strategy is registered in production NewEngine. Default spell admission remains `Unmanaged`; complete child/proc/recipient threat closure and terminal/reservation reconciliation are required before enablement.

## Numbering and roadmap relationship

The numbering systems describe different scopes:

- **Phase 0–8** in `ROADMAP.md` is the project-wide development stage. Healer work belongs to Phase 6.
- **F54a–F54u** are historical engineering work items and Core safety/regression milestones within the healer implementation stream; F54u is the latest validated item.
- **H0–H6** are the healer-architecture milestones below. H1 and H2 are complete at policy/profile scope; H3 is the next open architecture gate.
- An H milestone can be split into multiple F work items (for example, H3 may use F55a/F55b). F54 does not convert numerically into H3.

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

## H2 — Shaman Lesser Healing Wave profile adapter (validated; pure policy only)

The Shaman adapter is implemented in `ShamanHealingPolicy.h`. Its explicit allow-list contains Lesser Healing Wave ranks 8004, 8008, 8010, 10469, 10470 and 10471. It requires the snapshot row to carry `supportedSingleTargetDirect=true`, then composes the shared target triage and direct-heal rank selector.

The pure test covers Shaman class/role gating, rank filtering, mana affordability, fallback selection, selected base amount and cast-time propagation (1,500 ms). Chain Heal (1064) and unrelated direct-heal rows are rejected even when their synthetic profile has a larger amount.

**Validation on exact branch HEAD `09ec3dd`:** PR-safe run [#46128866209](https://github.com/tyrael9/core/actions/runs/46128866209) passed. Trusted push [#46128859679](https://github.com/tyrael9/core/actions/runs/46128859679) passed the pure tests, full Linux ARM64 Vanilla 5875 Core build and complete existing Core-backed simulator through final `Runtime PASS`, with normal GDB exit. Artifact [#15256659780](https://github.com/tyrael9/core/actions/runs/46128859679/artifacts/15256659780). This is CI validation of the pure profile adapter plus regression suite; it is **not** a real Shaman LHW cast fixture or production action.

Lesser Healing Wave is a 1.5-second party-target cast. The current estimator/admission path remains instant/self-only. H3 must prove impact-time recipient re-resolution and complete threat closure before a Shaman cast action can be implemented or registered.

Acceptance gates:
1. Pure tests cover class/role, all six allowed ranks, mana affordability, fallback and cast-time metadata — **passed**.
2. Snapshot rows remain value-only and filter unsupported spell shapes — **passed**.
3. Core-backed accepted Shaman cast, dynamic target/threat revalidation and no-side-effect failure matrix — **not started; H3**.
4. Production registration — **prohibited until H3 and the complete healer production gate pass**.

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
| H1 | Extract class-neutral policy and generalize the value-only healer spell catalog | **Complete:** pure policy tests, catalog build and trusted Core CI pass; no action dispatch |
| H2 | Implement a non-production Shaman Lesser Healing Wave profile adapter with an explicit six-rank allow-list | **Complete:** pure tests prove class/rank filtering, mana and cast-time metadata; no Core dispatch |
| H3 | Extend and validate Core cast-time admission/threat closure for the Shaman direct-heal MVP | **Next / not started:** trusted Core simulator must prove accepted cast and every target/recipient rejection and cleanup boundary |
| H4 | Reassess Priest adapter against the common contract; enable neither class by default | Review confirms parity and complete threat closure |
| H5 | Add the separate Paladin Holy Light / Flash of Light ScriptEffect adapter | Core script/child-cast behavior and complete threat effects are audited and trusted-tested |
| H6 | Add Druid direct / HoT / hybrid slices, then separately channel/AoE | Timing, form and multi-target acceptance gates pass |

## Production rule

Until a class adapter has a complete spell-shape profile, live Core preflight, complete threat envelope, terminal reconciliation and trusted Core-backed acceptance, it stays unregistered. Missing NewEngine healer behavior remains a safe no-op; it must never fall through to Legacy healer decisions.


## H1 implementation note — shared policy and generalized value-only spell catalog

Commit `b6f3b05` extracted the class-neutral missing-health, incoming-credit, target triage, Heal-reservation and direct-heal rank-selection logic into `HealerPolicy.h`; Priest compatibility wrappers retain the existing Priest gate.

The generalized catalog records learned direct-heal profiles for healer-role Paladin, Shaman, Priest and Druid bots. Rows contain spell ID, positive base heal, Core-calculated mana cost, actor-adjusted cast time and `supportedSingleTargetDirect`. The shape bit means only that the pinned Core SpellEntry matches the strict supported single-target direct-heal profile. It is not proof of proc/child closure, complete hostile threat coverage or admission safety.

The synthetic Paladin policy fixture proves the class-neutral selector can use a second class profile while ignoring a larger unsupported-shape row. The corrected Core include order compiled on ARM64 and the trusted simulator passed on parent `c82ec9c` (PR [#46128285712](https://github.com/tyrael9/core/actions/runs/46128285712), trusted push [#46128282616](https://github.com/tyrael9/core/actions/runs/46128282616)). The H2 exact-HEAD runs [#46128866209](https://github.com/tyrael9/core/actions/runs/46128866209) and [#46128859679](https://github.com/tyrael9/core/actions/runs/46128859679) passed the new Shaman pure tests and revalidated the complete trusted Core simulator. H1 and H2 are therefore complete at their stated policy/profile scope.

Paladin Holy Light / Flash of Light remain outside the ordinary Heal-effect catalog: their ScriptEffect/child-cast behavior requires a separate H5 audit. Shaman Chain Heal and Druid HoT/hybrid/channel behavior also remain separate spell-shape capabilities.
