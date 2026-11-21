# AzerothCore mod-playerbots research

## Scope and correct upstream

Primary reference: [mod-playerbots/mod-playerbots](https://github.com/mod-playerbots/mod-playerbots), inspected at commit [ecc4a537a3b43056e412fae63fb4a4271849b472](https://github.com/mod-playerbots/mod-playerbots/commit/ecc4a537a3b43056e412fae63fb4a4271849b472).

Important repository distinction: a repository named azerothcore/mod-playerbots also exists, but it is an old 2021 fork marked "NOT READY TO BE USED AS MODULE YET". It is not the current reference. The active module is mod-playerbots/mod-playerbots.

The active module is designed for AzerothCore and WoW 3.3.5a (Wrath). Its source is not compatible with the deployed Vanilla 1.12.1 vMaNGOS core. We use it as a source of encounter strategy design and coordination patterns, not as code to compile into this server.

## Architecture observed

The repository's AGENTS.md describes this structure:

- src/Bot/Engine: Strategy/Trigger/Action engine, Multiplier, action queue and bot runtime.
- src/Ai/Base: shared actions, triggers and strategies.
- src/Ai/Class/<Class>: class-specific strategies and actions.
- src/Ai/Dungeon: dungeon encounter strategies.
- src/Ai/Raid: raid encounter strategies.
- src/Mgr: shared managers.
- src/Script: Core script hooks and cross-thread integration.

The current engine uses the same broad decision vocabulary as CMaNGOS: named strategies, triggers, actions, values and multipliers. It has evolved APIs and Core integration specific to AzerothCore. The class and raid strategy code is therefore a behavioral reference, not a portable dependency.

The module's own contributor guidance is unusually relevant to our design:

- Triggers run frequently and must be O(1); they should consume cached values rather than scan group members, nearby units, inventory or quest state repeatedly.
- Actions should put expensive work behind isUseful()/isPossible().
- No synchronous database queries on map threads.
- Shared actions should perform one job and must not branch on strategy names.
- Expensive or behavior-changing features should be opt-in.

These constraints reinforce the need for one shared party snapshot and explicit execution boundaries in PartyBotEngine.

## Molten Core encounter examples

The active module has a complete Molten Core strategy family under src/Ai/Raid/MC:

- MCStrategy.cpp / MCStrategy.h
- MCTriggers.cpp / MCTriggers.h
- MCActions.cpp / MCActions.h
- MCMultipliers.cpp / MCMultipliers.h
- MCHelpers.h
- MCActionContext.h and MCTriggerContext.h

The following examples were inspected in source.

### Baron Geddon

The raid strategy registers triggers for Living Bomb and Inferno. The corresponding actions move the affected bot away from the group or away from the boss.

BaronGeddonAbilityMultiplier suppresses ordinary movement and reach-target spell actions while Inferno is active or while the bot has Living Bomb, while allowing the specific escape actions. This is a strong example of a mechanic-specific safety veto layered over ordinary class behavior.

Design lesson: encounter mechanics must be able to constrain the shared action candidate set. A generic class rotation must not walk a bomb carrier back into the raid or cancel a required escape.

### Golemagg

The strategy registers role-aware triggers for marking the boss, main-tank assignment, assist-tank assignment, healer positioning and Magma Splash handling.

GolemaggMultiplier changes behavior according to party role and encounter state. It handles the single-living-tank case, distinguishes main and assist tanks, suppresses unsafe AoE actions, and changes behavior below the encounter's soft-enrage threshold.

The Magma Splash trigger checks the bot's aura stacks and distance to the boss. The back-off action moves the bot out of swing range; the multiplier prevents the bot from immediately re-engaging while the debuff is still relevant. The source also defines fixed tank positions and a healer position that covers both tank camps.

Design lesson: a raid encounter is not just a priority list. It is a set of role assignments, spatial constraints, debuff state, add responsibilities and phase-dependent exceptions.

### Shazzrah

A ranged-only trigger activates when the bot is within Arcane Explosion distance. The action moves the bot out of range. The trigger deliberately avoids firing when the bot is already safe, preventing a useless movement attempt every tick.

Design lesson: triggers should encode a real transition condition, not merely repeat an action request. This avoids movement churn and no-op spam.

### Majordomo Executus

The strategy has a damage-shield trigger that causes non-tanks to stop attacking an add while the shield is active.

Design lesson: encounter strategy may veto otherwise valid damage actions based on the target's aura and the bot's role.

## Class/role examples

The current module's TankWarriorStrategy composes generic Warrior behavior with tank defaults and triggers. It defines a default action ordering, defensive stance, taunt behavior, ranged gap-closing and role-specific threat actions.

The HolyHealPriestStrategy uses health-state triggers to propose different healing actions for critical, low and medium health, and has a separate path for party-wide healing. The action priorities distinguish ordinary healing from emergency healing and movement to reach an out-of-range target.

Design lesson: role behavior should compose from generic class behavior plus role-specific strategies, while encounter strategies can modify the final candidate set.

## What we should adopt

1. Encounter strategies as first-class modules, not hard-coded exceptions buried in class rotations.
2. Separate trigger, action, helper/value and multiplier registration for each encounter.
3. Role-aware assignments: main tank, assist tank, healer, ranged and melee responsibilities.
4. Mechanic-specific action vetoes and safe movement exceptions.
5. State-sensitive transitions: debuff stacks, boss health thresholds, aura presence, range and phase.
6. Explicit spatial positioning for mechanics that require it.
7. Avoid no-op triggers and repeated movement churn.
8. Reusable helpers for encounter-specific unit classification and spell/creature identifiers, but with Vanilla data verified independently.

## What we must not port

- WotLK 3.3.5a spell IDs, talents, classes, class abilities, aura semantics or encounter tuning.
- AzerothCore Unit/Player/Group/Spell/Movement APIs, C++20 code, module registration or script hooks.
- Assumptions that a WotLK encounter mechanic exists in Vanilla with identical timing or behavior.
- The module's raid-role model without adapting it to our actual group composition and small-group goal.
- Any encounter-specific coordinate or distance constant until it is validated against the Vanilla map and vMaNGOS movement implementation.

## Vanilla adaptation priority

The initial raid research target is Molten Core, followed by Onyxia, Zul'Gurub, Blackwing Lair and AQ20. We should translate mechanics, not code. For every encounter we need a Vanilla-specific record of:

- creature entries and spell IDs from the 1.12.1 data;
- phases and transitions;
- role assignments and target priorities;
- aura/debuff triggers;
- movement and positioning constraints;
- adds, interrupts, dispels and CC rules;
- recovery and wipe conditions;
- known vMaNGOS script deviations or bugs.

The first implementation milestone remains five-player party coordination. Raid scripts must not leapfrog reliable healing, tank threat, target assignment and CC reservations.

## Source links

- [Active mod-playerbots repository](https://github.com/mod-playerbots/mod-playerbots)
- [Contributor/architecture constraints](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/AGENTS.md)
- [Molten Core strategy](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Raid/MC/MCStrategy.cpp)
- [Molten Core triggers](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Raid/MC/MCTriggers.cpp)
- [Molten Core actions](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Raid/MC/MCActions.cpp)
- [Molten Core multipliers](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Raid/MC/MCMultipliers.cpp)
- [Molten Core helpers](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Raid/MC/MCHelpers.h)
- [Tank Warrior strategy](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Class/Warrior/Strategy/TankWarriorStrategy.cpp)
- [Holy Priest strategy](https://github.com/mod-playerbots/mod-playerbots/blob/ecc4a537a3b43056e412fae63fb4a4271849b472/src/Ai/Class/Priest/Strategy/HolyPriestStrategy.cpp)
