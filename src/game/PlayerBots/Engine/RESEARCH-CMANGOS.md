# CMaNGOS PlayerBots research

## Scope and source identity

Primary reference: [cmangos/playerbots](https://github.com/cmangos/playerbots), inspected at commit [ed2825d816dd0aca2c66abad23c5c4289b754466](https://github.com/cmangos/playerbots/commit/ed2825d816dd0aca2c66abad23c5c4289b754466).

This is the modern CMaNGOS PlayerBots AI module, not merely the older built-in PlayerBot implementation in cmangos/mangos-classic. Its README explicitly describes the bot AI as supporting CMaNGOS Classic, TBC and WotLK, with expansion-specific SQL/configuration. The main strategy implementation lives under playerbot/strategy/.

The older core-side implementation at cmangos/mangos-classic/src/game/PlayerBot is useful for understanding legacy lifecycle integration, but it is not the primary reference for the Strategy/Trigger/Action/Value/Multiplier design.

## Engine model observed in source

### Engine and decision loop

Relevant files:

- playerbot/strategy/Engine.h
- playerbot/strategy/Engine.cpp
- playerbot/strategy/Queue.h
- playerbot/strategy/Action.h
- playerbot/strategy/Trigger.h
- playerbot/strategy/Strategy.h
- playerbot/strategy/Value.h
- playerbot/strategy/Multiplier.h

Engine::Init() resets the transient action queue, trigger nodes and multipliers, then asks each active Strategy to register its multipliers, triggers and default actions for the current BotState.

Engine::DoNextAction() updates the AI value context, processes triggers, pushes default actions, and evaluates the queued action nodes. The queue is ordered by relevance. The engine evaluates action prerequisites/alternatives/continuers and checks whether an action is possible and useful before execution. The source also has action execution listeners (before/allow/after/result override), loop tracking, action logging and external-event handling.

This is a useful reference for a deterministic decision pipeline and inspectable action lifecycle. It is not a drop-in library: it is deeply coupled to CMaNGOS PlayerbotAI, its context factories, bot state, unit types, spell APIs and queue ownership.

### Strategy

Strategy is a state-specific source of behavior. It registers triggers and multipliers for combat, non-combat, dead and reaction states; supplies default actions; and provides action-node factories.

ActionNode expresses relationships around an action:

- prerequisites that must run before the action;
- alternatives to try when it cannot run;
- continuers to enqueue after it.

NextAction carries an action name and relevance. Strategy composition allows generic behavior and role/class behavior to be combined without embedding every condition into one monolithic class rotation.

### Trigger

Trigger has a named IsActive() condition, a check interval, and an optional handler list. Trigger::Check() turns an active condition into an Event. TriggerNode associates a trigger with one or more NextAction handlers.

The check interval is important: not every expensive condition needs to be recomputed every AI update. For PartyBotEngine, trigger evaluation must be backed by a shared snapshot/value cache so that the same group scan is not repeated once per trigger or once per bot.

### Action

Action separates execution from eligibility:

- Execute(Event&) performs the operation.
- isPossible() checks whether it can be performed now.
- isUseful() checks whether it is worth performing.
- getPrerequisites(), getAlternatives() and getContinuers() express fallback/dependency behavior.
- threat type and movement metadata let the engine apply additional safety/ordering rules.

The engine's action result model distinguishes OK, impossible, useless, failed and unknown. That distinction is valuable for diagnostics; a target existing is not proof that a cast succeeded.

### Value

Value provides named, typed access to AI facts. CalculatedValue caches a calculated result for a configured interval; LazyGet returns a cached result when one exists. This avoids recomputing expensive state queries for every strategy consumer.

PartyBotEngine should use the same principle, but values should read an immutable per-tick PartyBotSnapshot rather than hold CMaNGOS-specific AI objects or pointers.

### Multiplier

Multiplier returns a multiplicative adjustment for an Action's relevance. This is a compact way to apply contextual vetoes or priority changes without duplicating class rotations. A zero multiplier can suppress an otherwise valid action; a positive factor can raise or lower its priority.

For the new engine, hard safety constraints (invalid target, protected CC, wrong role assignment, impossible cast) should remain explicit eligibility checks rather than being represented only as a floating-point score. Multipliers are appropriate for contextual priority, not for bypassing correctness.

## Encounter examples found in the CMaNGOS source

The repository contains generic Molten Core strategy files:

- playerbot/strategy/generic/MoltenCoreDungeonStrategies.cpp
- playerbot/strategy/generic/MoltenCoreDungeonStrategies.h
- playerbot/strategy/actions/MoltenCoreDungeonActions.cpp
- playerbot/strategy/actions/MoltenCoreDungeonActions.h
- playerbot/strategy/triggers/MoltenCoreDungeonTriggers.cpp
- playerbot/strategy/triggers/MoltenCoreDungeonTriggers.h

The Magmadar example activates a dedicated fight strategy from a combat trigger. The fight strategy adds a ranged/healer distance response and fire-protection behavior, and removes the encounter strategy when combat ends or the bot dies. This demonstrates encounter behavior as a temporary strategy layer rather than a permanent class rotation.

The source also contains Onyxia and Blackwing Lair strategy/action/trigger files. These are useful examples of encounter-specific behavior expressed through the same engine vocabulary.

## What we should adopt

1. Named, composable strategies for class, role, party state and encounter state.
2. Trigger -> candidate action flow, with explicit possible/useful checks.
3. Action priority/relevance and explicit prerequisites/fallbacks.
4. Cached named values, but with a PartyBotSnapshot as their data source.
5. Distinct action outcomes and bounded decision tracing.
6. Encounter strategies that can be activated/deactivated as phases and fights change.
7. A clear action execution seam so candidate selection is testable independently of Core spell/movement calls.

## What we should not copy directly

- CMaNGOS PlayerbotAI, AiObjectContext, PlayerbotAIAware, Group/Unit/Spell wrappers or database integration.
- Raw-pointer ownership patterns and NextAction** arrays. PartyBotEngine should use C++14-compatible value types and explicit ownership.
- CMaNGOS-specific command, world simulation, travel, inventory and random-bot systems.
- Any expansion-specific spell identifiers or assumptions without verifying Vanilla 1.12.1 data.
- A per-bot decision model as the only coordination mechanism. PartyBot requires party-wide assignments and reservations that are not solved by merely giving each bot its own Strategy list.

## Compatibility implications for vMaNGOS

The current target is the deployed vMaNGOS revision 0e3ff01e76d4758e8a7c3108b2717cc785ed56fa, client build 5875, C++14.

The CMaNGOS module targets a different Core API and includes multiple expansion variants. The reusable material is the decision architecture and behavior design, not source-level compatibility. Every execution action must be reimplemented against the actual vMaNGOS Player, Unit, Group, Spell, MotionMaster and map APIs after an API audit.

The first implementation should keep all CMaNGOS-derived concepts behind PartyBotEngine-owned interfaces. No CMaNGOS headers, classes or runtime dependencies should enter the vMaNGOS build.

## Open audit items before implementing adapters

- Map the actual vMaNGOS PartyBot update cadence and the lifetime/thread guarantees of Player, Unit, Group and Map references.
- Identify the authoritative vMaNGOS APIs for spell readiness, cast result, GCD, cooldown, threat, target validity, line of sight and movement.
- Determine whether all bots in a party update on the same map thread and how a shared coordinator can safely be accessed.
- Confirm which encounter state can be observed from existing instance scripts without adding DB schema.
- Select a first vertical slice only after the API map is recorded in ARCHITECTURE.md.

## Source links

- [CMaNGOS PlayerBots repository](https://github.com/cmangos/playerbots)
- [Engine.h](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/Engine.h)
- [Engine.cpp](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/Engine.cpp)
- [Action.h](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/Action.h)
- [Trigger.h](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/Trigger.h)
- [Strategy.h](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/Strategy.h)
- [Value.h](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/Value.h)
- [Molten Core strategy example](https://github.com/cmangos/playerbots/blob/ed2825d816dd0aca2c66abad23c5c4289b754466/playerbot/strategy/generic/MoltenCoreDungeonStrategies.cpp)
