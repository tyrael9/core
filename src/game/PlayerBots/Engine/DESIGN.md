# PartyBotEngine — current design and implementation plan

## 1. Product goal

Build a dependable five-player PartyBot group for the deployed Vanilla 1.12.1 / client build 5875 server. One human player and a small number of PartyBots should coordinate healing, tanking, damage, crowd control, recovery and instance mechanics. Correctness, predictable resource use and diagnosability take priority over maximizing bot count.

This is an independent decision engine implemented against the pinned vMaNGOS C++14 Core. It is not a rewrite of the Player/session/group runtime, and it is not a per-role migration that lets old and new policies compete.

## 2. Compatibility and operational baseline

- Core baseline: `0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`
- Client/data: Vanilla 1.12.1 / build 5875
- Language: C++14
- Target: Linux ARM64, Oracle A1 (1 OCPU / 6 GB)
- No external inference service or new database schema
- No production image publication, deployment or live-realm modification without explicit approval

The detailed Core API constraints are in `VMANGOS-API-AUDIT.md`; architecture and authority rules are in `ARCHITECTURE.md`; milestone status and evidence are in `ROADMAP.md`.

## 3. Runtime authority

The runtime has three explicit modes:

- **Legacy (default):** the existing PartyBot policy remains active.
- **Shadow:** PartyBotEngine evaluates and records decisions; Legacy remains the executor. Shadow is diagnostic only.
- **NewEngine:** PartyBotEngine is the sole decision authority. It must never fall through to Legacy healing, combat, class rotation, tank/assist, CC, dispel or target-selection policy.

An unimplemented behavior is a deliberate no-op. The current production NewEngine registration is intentionally narrow: out-of-combat Group-leader Follow and StopFollowing recovery. Priest healing and all other class combat policies remain unregistered. NewEngine is not yet a playable combat AI.

## 4. Decision and execution pipeline

1. Build or reuse a value-only snapshot for the current Group/map/instance.
2. Determine bot state, role context and active encounter.
3. Evaluate due triggers using cached facts and explicit invalidation.
4. Generate named action candidates with target identity and intent.
5. Apply eligibility/usefulness checks, multipliers, vetoes and channel arbitration.
6. Reserve shared party resources/targets where the action contract requires it.
7. Resolve actor and target GUIDs on the owning map thread and repeat action-specific live Core preflight immediately before side effects.
8. Execute through a CoreAction adapter and record the authoritative Core result.
9. Invalidate affected values, release/retain reservations by exact action/encounter correlation and emit bounded trace events.

Evaluation is read-only; execution is separate. Snapshot, queue, cache, reservation, event and trace storage must remain bounded.

## 5. Ownership and threading

`PartyBotAI` remains the Core lifecycle/map-thread host. `PartyCoordinatorRegistry` shares one coordinator among same-Group/map/instance bots; each bot owns its engine, strategy composition and pending decision work. Snapshots retain only GUIDs and scalar values. No Core `Player`, `Unit`, `Spell` or `Aura` pointer may outlive the map-thread build/execute call.

Same-map coordinator mutation stays on its owning Map update context. Cross-map transitions close/release the old shard on the source Map thread and reacquire a destination shard after insertion. Resolve GUIDs again immediately before execution. No database, network or inference work is allowed on map threads.

Owner reconnect and bot lifetime persistence remain host responsibilities. Their lifecycle tests must stay intact as engine behavior expands.

## 6. Strategy composition

One decision queue composes state, generic safety, class, role, party and encounter strategies. Roles are policy context, not separate executors. A Priest Healer should eventually arbitrate generic, Priest, healer, party and encounter actions in the same queue.

## 7. Validation and rollout

Pure C++14 tests cover deterministic policy and state-machine contracts. The dedicated PartyBot workflow validates pure tests, the read-only report collector, Configure, full Linux ARM64 / Vanilla 5875 Server+Realm compilation and simulator-harness syntax. Trusted push runs additionally execute the fixture-backed Core simulator when its data secrets are available; PR runs intentionally skip the secret-backed runtime.

A CI pass is not an in-game test. A simulator pass proves only its asserted Core fixtures, not overall gameplay quality. Legacy remains the default while NewEngine policies are incomplete. Every production policy requires Core accepted/rejected/terminal, stale-snapshot, resource, threat, cleanup and no-Legacy-fallback coverage.

No merge, image publication, deployment or realm-side change is authorised by CI alone.

## 8. Current implementation position (2026-11-26)

Phase 5 death/recovery and generic movement foundations are closed with trusted Core runtime evidence. The first production NewEngine behavior remains the narrow Follow policy; all other combat policy coverage is still development work.

Phase 6 is in the Priest Healer vertical slice. Current test-only evidence covers target/rank selection, Heal reservations, direct heals/HoTs/absorbs, bounded timeline, ordered damage/heal cases, helpful procs, target-trigger fan-out, a representative nested child chain and direct-heal pushback/interruption. F54k's Core random-dither floor/ceil interval, F54l's bounded school-specific absorb matching and F54m's four-cast/cyclic-fifth child-chain fixture all passed trusted runtime. F54m also proves five-recipient healing, per-hostile incremental threat parity, temporary-state restoration and clean simulator shutdown. F54n's trusted deferred instant-heal race proves that a new HostileReference may appear after pre-cast estimation and receive real Core healing-assist threat; the whole action now remains `Unknown`/empty rather than promoting the earlier component rows. See `ROADMAP.md`, `THREAT-BUDGETS.md` and `HEALING-FORECAST.md` for the exact evidence and next safety boundary.

Still blocked from production:
- `PriestDirectHealAction` whole-action threat remains Unknown; no Priest Healing Strategy is registered.
- F54n has proved the deferred-recipient race and the required fail-closed behavior. The next implementation step is a Core pre-impact authorization hook that refreshes actual recipients and atomically checks/reserves threat before irreversible effects; prove safe reject/refund timing first. Until then the action estimate remains Unknown and no Priest Healing Strategy is registered.
- Druid, Shaman, Paladin, Warrior, Mage, Warlock, Hunter and Rogue production strategies remain unimplemented.
- Party-wide target/threat/CC/interrupt/dispel policy and dungeon/raid encounter strategies remain future phases.

## 9. Near-term implementation order

1. Preserve F54k's trusted HoT lower/upper interval and F54l's known-school absorb boundary.
2. F54m's bounded four-cast child-chain, cyclic fifth-cast rejection and per-hostile threat parity are trusted-runtime validated.
3. F54n: implement and validate Core map-thread pre-impact authorization, exact recipient re-resolution, proc/script/trigger vetoes and atomic threat reservations; preserve Unknown if the hook cannot reject safely before irreversible effects.
4. Only then register the Priest direct-heal action and Priest Healing Strategy.
5. Expand to remaining classes/roles, party coordination and Vanilla encounter strategies.
