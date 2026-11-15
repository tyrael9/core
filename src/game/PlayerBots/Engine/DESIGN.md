# PartyBot Engine — design and implementation plan

## 1. Purpose

Build a dependable five-player PartyBot group for the Vanilla 1.12.1 (5875)
server. The target is a small group controlled by one human player and a few
PartyBots, with coordinated healing, tanking, damage, crowd control and
instance mechanics. Correctness and debuggability matter more than maximizing
the number of bots.

This is an incremental redesign of the decision-making layer. It is not a
rewrite of the VMaNGOS player/session/group runtime.

## 2. Compatibility baseline and hard constraints

The implementation branch starts from the exact Core revision used by the
reference deployment:

- Core revision: `0e3ff01e76d4758e8a7c3108b2717cc785ed56fa`
- Deploy wrapper revision: `ebaa560951459419c35140e20d787ec5db2905b8`
- Client variant: `5875` (Vanilla 1.12.1)
- Target language level: C++14
- Target deployment architecture: Linux ARM64 (Oracle A1)

Constraints:

1. Keep the 1.12.1 client protocol, data and spell behavior.
2. Keep the existing `PartyBotAI` lifecycle and VMaNGOS session/group APIs.
3. Do not require an external inference service, scripting runtime or network
   service for combat decisions.
4. Do not add database tables or change character/world schema for the engine.
5. Do not reset or recreate the existing deployment database.
6. Keep the old class-specific AI available as a fallback until a replacement
   behavior has passed in-game tests.
7. Keep changes buildable with the Core's C++14 toolchain and supported ARM64
   environment.

Long-lived engine code belongs in `vmangos/core`, not in the deploy wrapper's
small build/runtime patch directory.

## 3. Design principles

- **Observe before changing decisions.** Every important rejection should have
  an explainable reason before thresholds or priorities are changed.
- **One decision authority per action.** Avoid multiple class routines issuing
  conflicting GCD spells in the same update.
- **Coordinate at party scope.** A healer, tank and damage bot should make
  decisions from the same party situation, not isolated local guesses.
- **Separate decision from execution.** Strategies propose actions; the existing
  VMaNGOS spell, movement, target and group APIs execute them.
- **Preserve lifecycle behavior.** Login, logout, resurrection, follow,
  teleport, eating/drinking, battleground and instance handling must not be
  accidentally bypassed by a new combat engine.
- **Make behavior deterministic and inspectable.** Prefer explicit rules and
  bounded traces over opaque heuristics.

## 4. Target architecture

The engine is planned under `src/game/PlayerBots/Engine/`.

### 4.1 PartyBotContext and PartyBotSnapshot

Build one lightweight snapshot for a bot decision tick. It should contain only
state needed by the current strategies:

- bot identity, class, role, health, mana, form, auras, cooldowns and cast state;
- party members, map/instance, alive/online state, health, mana, range and
  current cast target;
- hostile units attacking party members, current victims and available threat
  information;
- target validity, line of sight, crowd-control marks and raid target icons;
- relevant encounter state and nearby interactable objects when a strategy
  needs them.

The snapshot is read-only. It must not keep raw pointers beyond the update in
which they are collected unless the lifetime is explicitly guaranteed by the
core. Avoid repeatedly scanning the same group and nearby units from every
individual strategy.

### 4.2 Decision pipeline

Each strategy contributes candidate actions. The engine evaluates them in a
consistent order:

1. `IsPossible`: spell known, cooldown/GCD, resources, form, range, line of
   sight, target validity and core cast restrictions.
2. `IsUseful`: the action improves the current situation (for example, the
   target needs healing, the tank can gain aggro, or an interrupt is timely).
3. `GetPriority`: score urgency and strategic value, including party-wide
   coordination and encounter modifiers.
4. Select the best candidate and execute through the existing Core APIs.
5. Record the selected action and rejected alternatives in a bounded trace.

A normal update should execute at most one primary GCD action. Movement,
facing, target selection, follow, interrupt coordination and other non-GCD
work are handled as separate channels with explicit ordering. A failed cast
must not silently consume the decision slot without recording the reason.

### 4.3 Strategy and action model

The intended building blocks are:

- **Action** — a concrete operation such as heal, taunt, interrupt, dispel,
  crowd control, damage spell, move, follow or interact.
- **Trigger** — a named condition that makes a behavior relevant.
- **Strategy** — a set of candidate actions for a role/class/situation.
- **Value** — a query over the snapshot, such as party health deficit, target
  threat risk, cast remaining time or distance.
- **Multiplier** — a contextual priority adjustment, such as tank assignment,
  encounter phase, healer mana or CC protection.

The first implementations should use ordinary C++ objects and explicit
registration. Do not introduce a generic scripting language before the core
model has proven useful.

### 4.4 PartyCoordinator

A per-party coordinator shares short-lived decisions:

- tank/assist target assignment;
- focus target and crowd-control reservations;
- healing reservations to prevent several healers wasting casts on one target;
- interrupt reservations;
- dispel and resurrection assignments;
- encounter phase and wipe/recovery state.

The coordinator is in-memory and scoped to the current group/map/instance. It
must tolerate members joining, leaving, disconnecting and reconnecting. A
reservation expires quickly and is invalidated when its owner cannot act.

### 4.5 Encounter strategies

Encounter logic is layered on top of class/role behavior. Each encounter
module should declare mechanics, phases, important adds, interrupts, movement
constraints and recovery conditions. Start with a small number of well-tested
five-player encounters before adding raid scripts.

Do not claim a raid is supported merely because generic combat works. Track
support per encounter and test the mechanics that can cause a wipe.

## 5. Party and owner lifecycle

The PartyBot lifecycle is part of combat reliability.

### Owner disconnect policy

A temporary network disconnect must not be treated as an intentional group
disband. When the original owner is absent but their GUID remains a member slot
in the bot's current group:

- retain the PartyBots and their current instance for a reconnect grace period;
- do not set `requestRemoval` immediately;
- if the owner reconnects to the same group, reset the timer and resume normal
  control without respawning the bots;
- if the group promoted a PartyBot to leader while the owner was away, restore
  the original owner's leadership when they return;
- if the owner explicitly leaves/disbands the group, remove the bot membership
  as normal;
- after the configured grace period expires without a reconnect, allow normal
  cleanup.

The initial default is 10 minutes, configurable through:

```ini
PlayerBot.OwnerOfflineGraceMs = 600000
```

Set it to `0` to disable the grace period. The check is based on the original
owner GUID still being a member of the existing Group; a missing group or an
owner who is still present but has moved to a different group is not treated as
a temporary disconnect.

This policy prevents the current immediate-removal path from ejecting the
bots as soon as `GetPartyLeader()` cannot resolve the disconnected owner. It
does not change normal explicit group removal behavior.

## 6. Diagnostics and operational controls

`DecisionTrace` is the first observability seam. It is opt-in through:

```ini
PlayerBot.DecisionTrace = 1
```

Trace output must be bounded per bot/update and include stable event names,
target/spell identifiers, a concise rejection reason and a numeric detail when
useful. It must not write to the database or flood logs when disabled.

Important initial events include:

- spell eligibility and cast result;
- heal target rejection and no-heal cycles;
- party combat detected while a healer is not personally in combat;
- healer class assigned a non-healer role;
- tank assist/taunt with no eligible action;
- AoE skipped because too few enemies are near the caster;
- owner disconnect grace started and expired.

The trace is diagnostic evidence, not a substitute for tests.

## 7. Incremental rollout

### Phase 0 — baseline and observability

- Pin work to the deployed Core revision.
- Add bounded, opt-in decision tracing.
- Instrument spell checks, healing selection, tank assist/taunt and Mage AoE
  decisions.
- Fix the healer lifecycle so a healer can support same-map party members
  fighting even if the healer itself is not in combat.
- Preserve PartyBots during the owner's temporary disconnect.

### Phase 1 — reliable five-player roles

- Build the party snapshot and shared coordinator.
- Migrate healing selection/reservations first.
- Implement reliable Protection Warrior target/taunt/threat decisions.
- Stabilize Mage target selection, AoE safety and interrupt behavior.
- Add role-specific tests and in-game scenarios.

### Phase 2 — remaining class behavior

- Migrate Druid, Shaman, Paladin, Hunter, Warlock and Rogue behavior in small
  slices.
- Add dispel, resurrection, CC and resource-management coordination.
- Keep legacy behavior as a per-feature fallback during validation.

### Phase 3 — five-player instance mechanics

- Add encounter registry and reusable mechanic primitives.
- Implement and validate encounters individually.
- Record supported instance/boss coverage and known limitations.

### Phase 4 — raid encounters

- Add Molten Core, Onyxia, Zul'Gurub, Blackwing Lair and AQ20 strategies one
  encounter at a time.
- Consider larger raids only after five-player and early raid behavior is
  stable.

## 8. Validation requirements

For each behavior change:

1. Build the exact 5875 Core on the target architecture or a matching
   reproducible build environment.
2. Confirm no unrelated database migrations are introduced.
3. Test both the positive action and its rejection paths.
4. Test owner logout/reconnect, explicit group disband, owner changing groups,
   bot death/resurrection and instance persistence.
5. Test with the intended small party size, not only a single bot.
6. Keep the previous working commit available for rollback.

A code review or successful CI build is not an in-game behavior test. Do not
deploy to the live server until the user explicitly approves it.

## 9. Build and regression CI

The repository workflow `.github/workflows/partybot-core-build.yml` performs a
full default-target build on a GitHub-hosted Linux ARM64 runner. This matches
the deployment CPU architecture and checks the 5875/C++14 source tree.

The workflow runs for pushes to the implementation branch, pull requests
targeting the pinned validation branch, and manual dispatch. It uses the
bundled third-party sources, disables optional cpptrace and PCH for a more
predictable and resource-efficient CI build, and caches compiler outputs with
ccache. It builds the default targets, including the game library, `mangosd`
and `realmd`.

The workflow definition being committed means CI is configured, not that a
build has passed. Record the first actual Actions result separately. The
manual owner reconnect, healer, Warrior and Mage checks are documented in
[TESTING.md](TESTING.md). CI cannot validate instance persistence or combat
behavior.

## 10. Current implementation status

This document is maintained with the code. Update this section whenever a
phase changes.

- [x] Exact deployed Core baseline recorded.
- [x] Opt-in bounded decision trace added.
- [x] Common spell, AoE safety and heal-target rejection paths instrumented.
- [x] Healer combat routine can run when same-map party members are fighting.
- [x] Warrior tank assist/taunt and Mage Arcane Explosion no-action paths
      instrumented.
- [x] Owner disconnect grace period added, with a 10-minute default.
- [x] ARM64 GitHub Actions compile workflow and regression checklist added.
- [x] ARM64 full Core build passed on the implementation head.
- [x] Existing Ubuntu GCC/Clang, macOS and Windows CI matrix passed on the
      implementation head.
- [x] Fixed the pre-heal helper's success return: it now reports success only
      when a cast is accepted, rather than treating a non-null target as a
      successful action.
- [ ] In-game verification of healer behavior and owner reconnect/instance
      persistence.
- [ ] Party snapshot and coordinator implementation.
- [ ] Strategy/action migration from the legacy class routines.
