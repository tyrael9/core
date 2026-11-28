# Cross-class healer architecture

Status: design contract and rollout plan, 2026-11-28. This document does not claim that Priest healing is production-enabled or that Paladin/Shaman/Druid adapters already exist.

## Decision

Build one shared PartyBotEngine healing pipeline with explicit per-class adapters. Do not create four independent healer engines, and do not treat class-neutral infrastructure as proof that every spell is supported.

Priest remains the first safety vertical slice. Paladin is the second adapter and the first test that the shared contract is genuinely reusable. Shaman and Druid follow after the contract survives that second class.

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

## Second adapter: Paladin MVP

Use Paladin as the second-class abstraction test because its first slice can stay within ordinary friendly single-target direct healing while still exercising a different class spell catalog and class-specific tuning.

Initial scope:

- Holy Light and Flash of Light ranks known by the bot from the generalized value-only catalog; the adapter must explicitly allow-list those profiles and not select Lay on Hands, Holy Shock, or unknown healing spells by base amount alone;
- explicit single friendly Player target in the same Group, Map and instance;
- direct heal only, with Core-calculated mana cost and cast time;
- rank selection balancing uncovered deficit, cast time and mana reserve;
- no HoT, multi-target, trigger, proc, scripted or item-backed healing in this first slice.

Do not assume these casts are covered by F54o's instant self-heal admission gate. Cast-time party-target admission and whole-action threat closure must be explicitly proven before the action can be enabled. If Core does not yet provide a safe admission shape, the Paladin adapter may be developed and unit-tested but must remain non-production and unable to dispatch the unsupported cast.

Acceptance gates:

1. Pure tests prove the shared triage/deficit/reservation behavior is unchanged from the Priest baseline.
2. Paladin tests cover known-rank discovery, unaffordable ranks, mana reserve, cast-time trade-offs, zero/full-health targets, tie-breaking and duplicate/stale spell rows.
3. Core-backed tests cover real accepted cast, terminal success/failure/interruption, movement/Map/Group changes during cast, mana/GCD/cooldown boundaries, all spell effects and every hostile threat recipient.
4. Unknown spell shape or incomplete threat fan-out rejects the whole action before irreversible cast effects.
5. Production registration remains off until all gates pass and a separate review explicitly enables it.

## Follow-on class slices

- **Shaman:** first implement bounded single-target direct heals (Healing Wave / Lesser Healing Wave). Chain Heal is a separate multi-target/fan-out capability and must model jump order, range, recipient selection and per-hostile threat. Totems are a separate subsystem.
- **Druid:** Healing Touch is a direct-heal slice; Rejuvenation is periodic; Regrowth is hybrid. Form exit/entry and mana policy are class adapter responsibilities. Tranquility is a later channelled/AoE capability.
- **Shared expansion:** add additional spell shapes only when at least one real class adapter supplies a complete profile and both pure and Core-backed fixtures validate it. Never broaden support by class label alone.

## Rollout sequence

| Milestone | Scope | Exit condition |
|---|---|---|
| H0 (this document) | Record the shared-engine / class-adapter contract and current support limits | Docs agree with current code and trusted evidence |
| H1 | Extract class-neutral missing-health, incoming-credit and reservation helpers without changing Priest decisions | Existing Priest unit/fixture behavior stays identical; generic helper tests pass |
| H2 | Implement a non-production Paladin direct-heal adapter for Holy Light / Flash of Light | Pure profile tests pass; unsupported Core cast shapes remain blocked |
| H3 | Extend and validate only the Core spell-admission/threat shapes required by the Paladin MVP | Trusted Core simulator proves successful cast and every rejection/cleanup boundary |
| H4 | Reassess Priest adapter against the common contract; enable neither class by default | Review confirms parity and complete threat closure |
| H5 | Add Shaman direct heals, then separately Chain Heal | Direct and fan-out acceptance gates pass |
| H6 | Add Druid direct / HoT / hybrid slices, then separately channel/AoE | Timing, form and multi-target acceptance gates pass |

## Production rule

Until a class adapter has a complete spell-shape profile, live Core preflight, complete threat envelope, terminal reconciliation and trusted Core-backed acceptance, it stays unregistered. Missing NewEngine healer behavior remains a safe no-op; it must never fall through to Legacy healer decisions.


## H1 implementation note — generalized value-only spell catalog

The follow-up to the common-policy extraction records direct-heal profiles for all four supported Vanilla healer classes (Paladin, Shaman, Priest and Druid) when their CombatBot role is Healer. Each profile now carries the learned spell ID, positive base direct-heal amount, Core-calculated mana cost, actor-adjusted Core cast time, and a `supportedSingleTargetDirect` shape bit computed from the pinned Core spell definition.

The shape bit is deliberately narrow: one positive single-target direct-heal effect with a supported explicit friendly/self target mode, excluding AoE, chain/fan-out selectors, extra non-heal effects, scripts, weapon procs, reflects and channels. It is a candidate/profile filter only. It does not prove the active aura/proc graph, child-cast closure, full hostile threat set, or whether F54o's deferred pre-impact admission hook accepts the spell. In particular, nonzero cast-time party heals are still blocked by the current threat/admission path.

The shared pure rank selector ignores profile rows without this shape bit. Priest compatibility entry points remain class-gated; a later class adapter must apply its own explicit spell allow-list and class-specific rank/cast-time policy.
