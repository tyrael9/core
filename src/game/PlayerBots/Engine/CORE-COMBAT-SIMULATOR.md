# Core-backed PartyBot combat simulator

## Purpose

Build a repeatable headless combat test environment that runs PartyBotEngine against the **real pinned vMaNGOS Core combat implementation**. It shortens the AI development loop: define a party, enemy behavior and encounter timeline; run deterministic ticks; inspect structured events; change strategy code and rerun without asking a player to construct the same fight in-game.

This is an integration-test layer between pure decision-engine tests and live realm validation, not a replacement for live testing.

## Non-negotiable properties

1. **Real Core semantics.** Damage, threat, victim selection, spells, auras, cooldowns, movement and death must come from the pinned Core. A toy combat model is acceptable only for pure strategy unit tests, never as evidence of Core combat behavior.
2. **Headless.** No WoW client or network client session is required.
3. **Isolated.** Tests run in a disposable process and isolated test data/database configuration. They must never connect to the Oracle realm, reuse its credentials, mutate its data volumes or run as part of deployment.
4. **Deterministic where controllable.** Scenario inputs, tick progression and RNG seed are explicit. Remaining nondeterminism (wall-clock, asynchronous map work, unordered iteration, DB-backed state) must be surfaced and reduced before a scenario is labelled deterministic.
5. **Observable.** Each run emits scenario ID, seed, tick, actor GUID, decision trace, Core action result, victim/threat/health/death transitions and a final assertion report.
6. **Safe by default.** A failed assertion or crash ends only the test process. No production server or persistent player/instance state is touched.

## Source audit: existing pinned-Core seams

The initial feasibility pass was performed against this branch's source.

### Useful APIs confirmed

- `MapManager::CreateTestMap(mapid, instanced, x, y)` and `DeleteTestMap(Map*)` are public. For an instanced map, the implementation allocates a fresh instance ID, constructs a `DungeonMap` for dungeon/raid map entries (otherwise `WorldMap`), calls `CreateInstanceData(false)` and registers the map. It deliberately does not call `SpawnActiveObjects()` in this test-map path.
- `Map` exposes public `Add(T*)`, `Update(uint32)`, `GetUnit(ObjectGuid)` and `GetCreature(ObjectGuid)`, candidate seams for adding actors and advancing a map without a client.
- `CreatureCreatePos` and `Creature::Create(..., CreatureInfo const*, ...)` construct a creature from a loaded Core creature template and explicit position.
- `Creature::AddToWorld()` registers the creature with its map and initializes AI if no AI was assigned. A test actor can install a test `CreatureAI` first, avoiding accidental production creature-script behavior while retaining Core Unit/Creature state.
- `Unit::DealDamage` is the real damage/death path. `Unit::AddThreat` forwards to the owning unit's real `ThreatManager`; the manager exposes threat-list values and victim state.
- `Unit::Attack` performs Core attack-state and attacker-reference bookkeeping and rejects dead, deleted or out-of-world targets.

These are promising seams. They do **not** yet prove that a standalone binary can initialize all required global services or that the full combat update loop can be driven safely outside the normal world process.

### Initialization dependencies

The test-map constructor is not lightweight. It reads global Core services including map DBC storage, ObjectMgr temporary GUID state, terrain, persistent-state manager, world configuration, weather and creature-group services; continent maps may also create worker pools. `CreateTestMap` calls instance-data initialization.

Creature construction depends on initialized creature templates/DBC and world configuration. Adding a creature invokes map registration and AI initialization unless a test AI is installed first.

Therefore, do not instantiate `Map` directly (its constructor is protected), or write a fake Map. The harness must reuse normal Core startup initialization for the services required by the selected test map.

## Recommended execution architecture

### Stage A — isolated Core bootstrap smoke test

Implemented as a configuration-gated one-shot mode in the existing `mangosd` executable. It is intentionally smaller than the eventual simulator:

1. run the normal Core initialization path using a dedicated disposable configuration/database;
2. create a fresh dungeon/raid test map through `sMapMgr.CreateTestMap`;
3. assert that the map is registered, advance one empty `Map::Update(50)` tick, and assert it remains registered;
4. delete it through `sMapMgr.DeleteTestMap` and assert the registry entry is gone;
5. set the process exit code from the assertions and reuse the normal WorldRunnable shutdown path.

The mode skips world/RA/SOAP listeners, realm-online/offline updates, daemon/PID-file behavior, account cleanup and mass-mail processing. Before connecting to any DB it verifies that all four configured schema names begin with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`). This guard prevents accidental use of the ordinary realm schema names; the schemas must still be disposable copies.

This smoke test proves only that the real Core can create, tick and delete an empty instance map in headless mode. It does not create actors or prove combat.

**Startup hook found:** `Master::Run()` calls `sWorld.SetInitialWorldSettings()` before it launches `WorldRunnable` and before `sWorldSocketMgr.StartWorldNetworking()`. This is the first practical hook: required world data/managers have been initialized, and the world listener has not yet been opened. A test-mode branch can therefore skip the listener and normal world loop while reusing the real startup path.

**Important caveat:** `SetInitialWorldSettings()` is not read-only. It writes startup/maintenance data (including the `uptime` record), starts background workers (character DB, async packets, anticheat and possibly LFG), and the Master has already started IO-context runner threads. Simulator mode must use a dedicated disposable database/configuration and reuse the normal shutdown/cleanup path. Simply returning from `Master::Run()` after a test would leak threads and leave services uncleanly stopped.

The map-level `Map::Update(diff)` accepts an explicit diff, but internally still reads wall-clock values and global services (including the movement broadcaster); it also updates sessions, cells, weather, scripts and instance data. A fixed diff alone does not make full-map ticks deterministic. Stage B must determine whether an instance-map tick can be driven safely or whether a test-only clock seam is required. Do not alter production tick semantics to simplify tests.

### Stage B — first real combat proof

Start small and avoid player/group complexity:

- one test-controlled tank-like creature;
- one test-controlled damage dealer;
- one hostile creature with a threat list;
- fixed RNG seed and explicit tick schedule.

Assertions must prove from Core-owned state:

- actors exist on the same real test map;
- `Unit::Attack` establishes actual attacker/victim state;
- `Unit::DealDamage` changes real health and reaches the real death path;
- `Unit::AddThreat` creates/updates real `ThreatManager` references;
- threat ordering and current victim change as expected when the actual Core selection/update path runs;
- cleanup removes every actor and the temporary map.

A test is not successful merely because a helper directly writes health or a fake threat number. Inspect Core-owned objects after Core methods/ticks run.

The exact mechanism for forcing victim reselection (normal CreatureAI update, threat-event processing or another Core update path) remains an implementation audit item. Do not substitute hand-coded target selection.

### Stage C — PartyBotEngine integration

Only after Stage B passes:

- create a five-member party snapshot from real map actors;
- attach the actual Engine executor adapters, not mock actions;
- run a scripted encounter timeline against the same Core tick loop;
- capture decision trace and Core action results;
- add scenarios for aggro, healer threat, tank death, healer death, adds and resource pressure.

The Engine may initially make poor decisions. The harness must expose those failures faithfully rather than patching the simulation to make tests green.

### Stage D — dungeon encounter simulation

After party combat is stable, evaluate loading a real dungeon map with real instance scripts/data. This adds grid/terrain/vmap loading, instance state, scripted creature AI, spell data, doors and encounter lifecycle. Full dungeon loading is not a prerequisite for the first Core combat proof.

## Scenario format

Use declarative scenario files (JSON or a small C++ builder initially) with scenario name/schema version, map ID and instance flag, RNG seed, actor templates/roles/positions/test AI, initial health/power/threat, timed events, expected assertions, maximum ticks and wall-clock timeout.

Initial scenarios:

| Scenario | Core behavior under test |
|---|---|
| `basic_damage_death` | Real damage, health transition and death cleanup |
| `threat_ordering` | Real threat list values and victim selection |
| `tank_loses_aggro` | Damage dealer overtakes tank and bot response |
| `healer_pulls_aggro` | Healing threat/hostile retarget and tank peel |
| `tank_dies` | Tank death, add retarget and party recovery |
| `healer_dies` | Healing coverage loss and response |
| `multiple_adds` | Multi-target coordination and target reservations |
| `healer_mana_low` | Real power state, spell costs and recovery policy |

## Determinism and performance

- Advance the map with fixed integer millisecond diffs; do not sleep to simulate time.
- Seed every Core RNG facility used. If RNG is process-global, run scenarios in separate processes or add a scoped test seed/reset API.
- Disable unrelated background services and map worker pools only where Core safely permits it; do not change production combat semantics.
- Start with one map and one update thread. No parallel scenarios until isolation is proven.
- Bound ticks, actors, event buffers and wall-clock duration. Emit the seed and complete scenario input on failure.
- Record compiler, Core SHA, data build and scenario schema version in every report.

## Boundaries and non-goals

- This harness is not an AI and must not grow strategy logic.
- It must not use the live Oracle host or its databases, configs, logs or volumes.
- It must not auto-publish or deploy a server image.
- It does not replace live tests for networking, disconnect persistence, client packets, real player behavior or long-duration stability.
- It must not claim deterministic replay until RNG, tick progression, map threading and external data dependencies are controlled.
- It does not replace pure C++14 decision tests; both layers are required.


### Stage B source audit — actor lifecycle and combat semantics

The pinned source audit now confirms the following exact lifecycle:

- Construct a temporary actor with `Creature::Create(guidLow, CreatureCreatePos(map, x, y, z, o), creatureInfo, firstCreatureId)`. The template must come from the loaded `sObjectMgr.GetCreatureTemplate(entry)`; reject a missing template and do not fabricate a `CreatureInfo`.
- Install a simulator-owned `CreatureAI` with `Creature::SetAI` **before** calling `Map::Add(creature)`. `Creature::AddToWorld` invokes `AIM_Initialize()` when no AI is installed, which would otherwise select normal script/factory AI. The creature destructor owns and deletes the installed AI.
- `Map::Add(Creature*)` sets the map, creates/loads the grid as required, inserts the creature into the grid, calls `AddToWorld`, adds active objects, registers the viewpoint and visibility, and invokes `CastSpawnSpell()`. Test templates must therefore be checked for spawn spells and other template-driven side effects; a custom AI alone does not suppress those.
- `Creature::AddToWorld` registers a normal unit GUID with the map before `Unit::AddToWorld`, then initializes AI only if none was supplied. Avoid database-spawn GUIDs and creature-group/linking behavior for simulator actors.

**Cleanup is safety-critical.** Do not call `Map::Remove(creature, true)` for a temporary simulator actor: the normal remove path calls `CleanupsBeforeDelete`, may call `SaveRespawnTime()`, and deletes the object. The test-specific sequence should be: stop the actor's combat/AI activity, call `Map::Remove(creature, false)` while it is still on the map, then delete the detached object explicitly. The `false` path calls `RemoveFromWorld`, unlinks it from the grid and resets its map without saving respawn time or deleting it. `Creature::~Creature` calls `Unit::CleanupsBeforeDelete` before destroying its AI, so references and combat state are cleaned during explicit destruction. Only after every actor is detached and destroyed may the harness call `MapManager::DeleteTestMap`; the map destructor unloads its remaining grids.

Combat API semantics confirmed:

- `Unit::Attack(victim, meleeAttack)` requires both units alive and the victim in-world; it establishes attack state and attacker references. It is not itself a damage event and does not by itself establish a victim threat entry.
- `Unit::DealDamage(..., DIRECT_DAMAGE, ...)` is the real health/death path. The normal hostile-damage path adds threat to the victim through `victim->AddThreat(attacker, threat, ...)`.
- `Unit::AddThreat` only forwards to `ThreatManager` when the receiving unit can own a threat list and both units are on the same map.
- `ThreatManager::getHostileTarget()` updates/sorts the real threat container and calls `ThreatContainer::selectNextVictim`. The pinned selector includes the normal 110% melee / 130% ranged switching thresholds and target-validity checks. A test that calls this method directly proves Core threat selection, but does not prove that a Creature AI automatically invokes it during a map tick.
- `CreatureAI::UpdateAI(diff)` is the intended test-AI update seam. The simulator must verify from the complete Creature/Map update path that this callback is reached on the selected map tick, rather than assuming that `Map::Update(50)` alone advances every creature.

The first Stage B proof should be deliberately split into independently reported assertions: (1) template-backed actors can be created, inserted and found on the map; (2) real `DealDamage` changes health and creates the expected threat reference; (3) the real threat selector chooses and switches victims at the Core thresholds; (4) custom AI receives bounded updates; and (5) all actors are removed without respawn persistence before the test map is deleted. A passing direct-damage/threat proof must not be described as a full autonomous combat simulation.

The current implementation does not yet include this Stage B code. This section records verified source behavior and the required safety contract; it is not a runtime result.

## Current status and next gate

**Status: Stage A smoke-test code committed and compile-tested in CI; not yet runtime-tested against a disposable DB/data installation.** The entry point is opt-in through `PartyBot.Simulator.Enable`. It requires all four DB schema names to begin with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`) and runs after Core initialization, before the world loop/listeners. It creates a real dungeon/raid test map with `CreateTestMap`, verifies the instance is registered, advances one real `Map::Update(50)` tick with no actors, verifies the map remains registered, deletes it with `DeleteTestMap`, and verifies the registry entry is gone. It then exits through the normal WorldRunnable shutdown path. Default map ID is 33. No Creature actors or combat/threat simulation are implemented in Stage A; its only tick is one empty `Map::Update(50)` call.

Confirmed: the pinned Core has a purpose-built test-map creation/deletion API, public map add/update APIs, native Creature creation, real Unit damage/attack methods and a real ThreatManager API.

Not yet confirmed: runtime behavior of the Stage A path with a disposable full DB copy, clean insertion/removal of temporary Creature actors, whether repeated map ticks can be driven deterministically without world-loop side effects, complete RNG control and absence of writes outside the prefixed test schemas.

**Next gate:** finish the update-path and GUID-allocation audit, then implement the first bounded Stage B actor/damage/threat proof. The proof must explicitly report which assertions execute direct Core methods and which are driven by map ticks. Stage A still needs a one-time runtime smoke test against disposable DB copies and normal 5875 data; neither Stage A nor Stage B has been runtime-tested yet. Do not begin five-player or dungeon simulation until real damage, threat, target-reselection and cleanup assertions pass in CI.
