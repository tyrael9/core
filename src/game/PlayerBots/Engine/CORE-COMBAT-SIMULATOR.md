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
- `Creature::AddToWorld()` registers the creature with its map and initializes AI if no AI was assigned. `Creature::AIM_Initialize()` initializes both the `MotionMaster` and template-selected AI. A test actor must call this lifecycle method first, then explicitly delete/replace the template AI with its probe AI; installing a custom AI before `Map::Add` skips `AIM_Initialize` and leaves an empty `MotionMaster`.
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
3. assert that the map is registered and remains registered before the combat scenario;
4. delete it through `sMapMgr.DeleteTestMap` and assert the registry entry is gone;
5. set the process exit code from the assertions and reuse the normal WorldRunnable shutdown path.

The mode skips world/RA/SOAP listeners, realm-online/offline updates, daemon/PID-file behavior, account cleanup and mass-mail processing. Before connecting to any DB it verifies that all four configured schema names begin with `PartyBot.Simulator.DatabaseNamePrefix` (default `partybot_sim_`). This guard prevents accidental use of the ordinary realm schema names; the schemas must still be disposable copies.

This lifecycle check proves only that the real Core can create, register and delete an instance map in headless mode. It does not create actors or prove combat. The first map tick is deliberately deferred until Stage B has inserted actors with initialized movement state.

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

### Stage C — Engine-to-Core attack executor (runtime CI passing)

- Build a five-role value-only PartySnapshot from same-map simulator actors.
- Let PartyBotEngine select a tank-assist candidate and execute a real Core \`Unit::Attack\`.
- Verify the actual Core victim, action result and actor cleanup.

### Stage D — triggered spell-effect proof (runtime CI passing)

- Let the healer trigger select an injured party member.
- Execute Vanilla Lesser Heal (2050) through the real Core Spell/SpellEffects path as a triggered Creature cast.
- Verify the Core cast result and actual health increase.

### Stage E — normal Player spell lifecycle (runtime CI passing)

- Construct a real Core \`Player\` with a local \`WorldSession\` in the isolated simulator process.
- Require spellbook ownership and let the Engine select the heal.
- Call ordinary non-triggered \`Player::CastSpell(..., false)\`; verify acceptance, pending cast, GCD, mana cost, normal completion and actual healing.
- Logout and delete the Player through the Core session/map cleanup path.

This remains a simulator fixture, not a production-authenticated PlayerBot or five-member Core Group. Detailed assertions and limitations are recorded below.

### Stage F — movement and defensive action adapters

- [x] First Core point-movement primitive: PartyBotEngine derives a formation destination from the Main Tank snapshot, the adapter calls `MotionMaster::MovePoint`, and runtime CI verifies the real position reaches the destination.
- [x] Add a runtime regression proving that an Engine-selected Player MovePoint interrupts a pending non-triggered Lesser Heal through the real Core Spell::update movement check, without applying the heal.
- [ ] Add continuous follow/formation tracking and defensive actions against real Core movement generators and unit state.
- [ ] Verify movement completion, movement cancellation of casts, target/range/LOS revalidation and cleanup.

### Stage G — dungeon encounter simulation

After the action/executor layer and party-wide combat behavior are stable, evaluate loading a real dungeon map with instance scripts/data. This adds grid/terrain/vmap loading, instance state, scripted creature AI, doors and encounter lifecycle. Full dungeon loading is not a prerequisite for the current Core-backed action proofs.

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
- Call `Creature::AIM_Initialize()` before installing the simulator-owned AI. This Core lifecycle method initializes the `MotionMaster` and creates the template-selected AI. `Creature::SetAI` is a raw pointer setter; explicitly delete the template AI before replacing it with the probe AI. Do this before `Map::Add(creature)` so `Creature::AddToWorld` does not replace the probe with a factory AI. The Creature destructor owns and deletes the currently installed AI.
- `Map::Add(Creature*)` sets the map, creates/loads the grid as required, inserts the creature into the grid, calls `AddToWorld`, adds active objects, registers the viewpoint and visibility, and invokes `CastSpawnSpell()`. Test templates must therefore be checked for spawn spells and other template-driven side effects; a custom AI alone does not suppress those.
- `Creature::AddToWorld` registers a normal unit GUID with the map before `Unit::AddToWorld`, then initializes AI only if none was supplied. Avoid database-spawn GUIDs and creature-group/linking behavior for simulator actors.

**Cleanup is safety-critical.** Do not call `Map::Remove(creature, true)` for a temporary simulator actor: the normal remove path calls `CleanupsBeforeDelete`, may call `SaveRespawnTime()`, and deletes the object. The test-specific sequence should be: stop the actor's combat/AI activity, call `Map::Remove(creature, false)` while it is still on the map, then delete the detached object explicitly. The `false` path calls `RemoveFromWorld`, unlinks it from the grid and resets its map without saving respawn time or deleting it. `Creature::~Creature` calls `Unit::CleanupsBeforeDelete` before destroying its AI, so references and combat state are cleaned during explicit destruction. Only after every actor is detached and destroyed may the harness call `MapManager::DeleteTestMap`; the map destructor unloads its remaining grids.

Combat API semantics confirmed:

- `Unit::Attack(victim, meleeAttack)` requires both units alive and the victim in-world; it establishes attack state and attacker references. It is not itself a damage event and does not by itself establish a victim threat entry.
- `Unit::DealDamage(..., DIRECT_DAMAGE, ...)` is the real health/death path. The normal hostile-damage path adds threat to the victim through `victim->AddThreat(attacker, threat, ...)`.
- `Unit::AddThreat` only forwards to `ThreatManager` when the receiving unit can own a threat list and both units are on the same map.
- `ThreatManager::getHostileTarget()` updates/sorts the real threat container and calls `ThreatContainer::selectNextVictim`. The pinned selector includes the normal 110% melee / 130% ranged switching thresholds and target-validity checks. A test that calls this method directly proves Core threat selection, but does not prove that a Creature AI automatically invokes it during a map tick.
- `CreatureAI::UpdateAI(diff)` is the intended test-AI update seam. The simulator must verify from the complete Creature/Map update path that this callback is reached on the selected map tick, rather than assuming that `Map::Update(50)` alone advances every creature.


A further tick-path constraint was confirmed in `Map::UpdateCells`: it visits cells around map players and active non-player objects. An ordinary temporary Creature in an otherwise empty headless instance is not enough to guarantee that its `Creature::Update` (and therefore `CreatureAI::UpdateAI`) is called. The simulator must mark its test actors active with `SetActiveObjectState(true)` before adding them, then assert the AI callback counter actually advances after a map tick. This uses the Core's existing active-object update path; it is not a substitute call to the AI. The cell-update cadence is wall-clock/config driven, so a single immediate `Map::Update(50)` may still not cross the configured cell-update interval. The harness needs a bounded way to satisfy that cadence and must report whether the tick updated the actor rather than assuming it did.

The first Stage B proof should be deliberately split into independently reported assertions: (1) template-backed actors can be created, inserted and found on the map; (2) real `DealDamage` changes health and creates the expected threat reference; (3) the real threat selector chooses and switches victims at the Core thresholds; (4) custom AI receives bounded updates; and (5) all actors are removed without respawn persistence before the test map is deleted. A passing direct-damage/threat proof must not be described as a full autonomous combat simulation.

### Stage B implementation now present (Core runtime assertions and push CI passing)

The simulator now contains a bounded first Core-backed Creature/threat scenario:

- Select a normal-rank creature template deterministically from loaded templates, requiring a display and excluding NPCs, civilians, racial leaders, pets, immune templates, and templates with spawn spells, totem spells, default auras, spell lists, scripts, named AI, invisibility or the no-threat-list flag.
- Select a mutually hostile faction-template pair from the loaded Core faction data, rather than hard-coding Vanilla faction IDs.
- Create one threat-owning Creature and two hostile Creature actors from that real template. Each is inserted with a Core-generated map-local GUID, first runs `AIM_Initialize()`, then has its template AI explicitly replaced with a small probe AI before insertion, is marked active before `Map::Add`, and is verified through both `IsInWorld()` and `Map::GetCreature(guid)`.
- Drive bounded real `Map::Update(50)` calls, paced with short wall-clock sleeps, and require all three probe AIs to receive `UpdateAI`.
- Emit exact health progression, applied damage, tank/DPS threat values, selected target GUIDs at each threshold step, AI update counts and per-actor cleanup results in the simulator log.
- Deal real direct damage to the threat owner from the two hostile actors (100, 105, then +10), verify health loss and damage-generated threat references, and check that the Core threat selector retains the first target below the 110% melee threshold and switches above it.
- Clear combat/threat references, detach each actor with `Map::Remove(actor, false)`, and verify it is no longer in-world or registered in the map before explicit destruction.

This Stage B section is deliberately a **Core Creature damage/threat smoke test**, not an autonomous encounter simulation. Later Stage C–E sections document the separately implemented Engine attack and spell/heal slices; movement, boss scripts and multi-phase encounter mechanics remain future work. The probe AI only counts update callbacks. Actor stats are normalized to a large health pool to keep the test damage non-lethal. The tick loop is bounded but still depends on Core wall-clock cell-update cadence; this does not establish deterministic replay.

## Current status and next gate

**Runtime evidence (2026-11-17):** [push CI #44049443380](https://github.com/tyrael9/core/actions/runs/44049443380) passed end to end on commit \`16d73f712fa3698b8a2411bac8e087cbb8dc4d26\`. The run downloaded the complete 1.21 GiB Vanilla 5875 extracted-data fixture, verified SHA-256, initialized disposable database copies, built the ARM64 server and ran the opt-in Core simulator. Runtime assertions passed for:

- Real Creature updates, direct damage, generated threat and the Core 110% melee victim-switch threshold.
- Engine-selected tank assist calling real \`Unit::Attack\`, with actual victim verification.
- Engine-selected triggered Creature Lesser Heal, with actual health increase.
- Engine-selected normal non-triggered Player Lesser Heal: spell known, \`SPELL_CAST_OK\`, pending generic cast and GCD, mana \`2896>2866\`, target health \`50000>83386\`, cast completion and Player logout cleanup.
- Test map and all actors deleted; \`mangosd\` shut down successfully.

The Player fixture is a real Core Player object with a local no-socket WorldSession, but is not authenticated as a production PlayerBot and is not inserted into a real five-member Core Group. The map update cadence is wall-clock throttled; the simulator waits between bounded updates rather than faking cast completion. Deterministic replay and broader lifecycle/failure coverage remain open.

The entry point remains opt-in through \`PartyBot.Simulator.Enable\`, checks all four DB schema names against the \`partybot_sim_\` prefix, and runs after Core initialization before world listeners. The workflow is push-only and has no image-publish or deployment side effects.

### Optional GitHub Actions runtime runner

The repository now contains `tools/partybot-simulator/run-ci.sh`, wired into the ARM64 build workflow as a push-only optional step. It is a no-op until both repository Actions secrets below are configured:

- `PARTYBOT_SIMULATOR_DATA_URL`: HTTPS read-only download URL for a `.tar.gz` archive of the complete extracted-data directory, preserving `5875/dbc/`, `maps/`, `vmaps/` and `mmaps/` at the archive root.
- `PARTYBOT_SIMULATOR_DATA_SHA256`: SHA-256 checksum of that archive.

The database readiness check deliberately requires a TCP connection to `127.0.0.1:3306`, not the container's local socket: the deployment image starts a temporary socket-only MariaDB during seed import before bringing up the normal TCP listener. This avoids launching `mangosd` while the database is still initializing.

The runner extracts the full archive into its isolated `DataDir` and checks that the required DBC files and all three world-data directories are present and nonempty. This supports later terrain, collision, navigation and map-content scenarios without uploading another fixture. It starts the same public vMangos database image used by the pinned deployment wrapper, clones its `mangos`, `characters`, `realmd` and `logs` schemas to `partybot_sim_*`, and points all four Core connections exclusively at those disposable copies. It then runs the one-shot `mangosd` simulator and uploads both console and file logs as a 14-day Actions artifact. It never reads or connects to the Oracle realm.

From the deployment directory, create a single archive while preserving the extracted-data directory structure:

```sh
tar -czf extracted-data-5875.tar.gz -C storage/mangosd/extracted-data maps mmaps vmaps 5875
sha256sum extracted-data-5875.tar.gz
```

On Windows PowerShell (run from the `vmangos-deploy` repository root):

```powershell
$source = ".\storage\mangosd\extracted-data"
tar.exe -czf .\extracted-data-5875.tar.gz -C $source maps mmaps vmaps 5875
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
(Get-FileHash .\extracted-data-5875.tar.gz -Algorithm SHA256).Hash.ToLowerInvariant()
tar.exe -tzf .\extracted-data-5875.tar.gz | Select-Object -First 12
```

The archive listing should start with paths such as `maps/`, `mmaps/`, `vmaps/` and `5875/dbc/`. This is the extracted server data, not the original WoW client `Data` directory.

Upload the archive to Google Drive and set General access to “Anyone with the link” with Viewer permission. Put the share URL in `PARTYBOT_SIMULATOR_DATA_URL` and the checksum in `PARTYBOT_SIMULATOR_DATA_SHA256` under the repository's Actions secrets. The runner uses the pinned `gdown` downloader to handle Drive's large-file confirmation flow, then verifies SHA-256 before extraction. Each trusted push to this branch runs the simulator after the full ARM64 build. If either secret is absent, the workflow records a skip rather than attempting a partial runtime test.

**Next gate:** extend point movement into continuous follow/formation tracking, then test movement-driven cast interruption and defensive actions. Expand the Player-backed spell path with rejection/cancellation/range/LOS cases and integrate a real Core Group fixture. Do not jump directly to five-player dungeon scenarios.


## Stage C — PartyBotEngine-to-Core executor vertical slice

The next integration step keeps the simulator on the same isolated test map and reuses the real template-backed Core Creature lifecycle. It creates five party-role actors (tank, healer, melee DPS, ranged DPS and support) plus one mutually hostile target. The roster and hostile are projected into GUID/scalar-only \`PartySnapshot\` values; no live Core pointers are stored in the decision model.

A simulator Trigger emits a tank-assist candidate. \`PartyBotEngine::Evaluate\` performs the normal action lookup, eligibility checks and channel arbitration. The selected action adapter resolves the bot and target GUIDs back through the current \`Map\`, revalidates life/hostility, then calls the pinned Core \`Unit::Attack\`. The test requires the report to say \`Executed\`, the action result to be \`Success\`, and the tank's actual Core victim pointer to equal the hostile Creature. All six actors are detached and destroyed before map deletion.

This is deliberately a narrow proof:
- It validates the real Engine evaluation/execution boundary and one real Core action adapter.
- The five roster entries are simulator Creature actors, not authenticated \`Player\` objects or a Core \`Group\`.
- Stage C proves the Engine-to-Core attack boundary only; Stage D and Stage E separately prove triggered Creature healing and normal non-triggered Player healing.
- These slices still do not prove production PlayerBot authentication, real Group membership, movement generators, class rotations or a full dungeon AI. Stage F should add movement/formation and defensive actions with Core precondition revalidation.



## Stage D — PartyBotEngine healer triage and triggered Core spell effect

This completed vertical slice adds a healer-role trigger and a \`core_heal\` Action adapter. The trigger reads only the value snapshot and emits a healing candidate for an injured living party member. The action resolves caster/target GUIDs on the current test map, checks that the spell is a heal effect and that the two units are alive, friendly and the target is still injured.

The simulator uses Vanilla spell 2050 (Lesser Heal, Rank 1). It requires all of the following:
- PartyBotEngine registers the action, evaluates the healer trigger and selects the injured tank.
- The adapter calls the pinned Core \`SpellCaster::CastSpell\` API and receives \`SPELL_CAST_OK\`.
- The Core Spell / SpellEffects path increases the target's actual health; the measured delta is logged and checked.
- Both Creature actors are detached and destroyed before the test map is deleted.

**Important scope limitation:** this first fixture deliberately uses a triggered cast from a template-backed Creature. This still exercises the real Core spell implementation and healing effect, but bypasses player spellbook ownership, mana cost, GCD and cast-time progression. It is not proof that an authenticated PlayerBot can cast a known spell normally. A separate non-triggered Player-backed slice must cover those mechanics before this adapter is used by production PlayerBots.

The optional CI runner now requires both Stage D and Stage E PASS markers in addition to the Stage A/B and Stage C markers. It remains push-only, uses the complete extracted-data fixture and four disposable \`partybot_sim_*\` schemas, and has no image-publish or deployment side effects.


## Stage E — normal Player spell execution

Stage E adds an actual Core \`Player\` as the Engine healer. The simulator constructs it through the pinned Player creation path (\`Player::Create\`, real Player stats/default spell initialization, a local \`WorldSession\`, map insertion and ObjectAccessor registration), rather than disguising a Creature as a Player. The fixture is not authenticated against a production account and is never persisted; it uses the disposable simulator DB and logs out through \`WorldSession::LogoutPlayer(false)\` before map teardown.

The Engine candidate still comes from the same healer-triage trigger. The Player action adapter re-resolves the healer and patient GUIDs, requires the healer to actually know spell 2050, checks spell readiness/GCD/current-cast state and friendly living target state, then calls the ordinary non-triggered \`Player::CastSpell(..., false)\` path.

The runtime assertion distinguishes acceptance from completion:
- The Engine reports the action as executed and the Core returns \`SPELL_CAST_OK\`.
- A generic spell remains pending immediately after acceptance and the Player GCD is active.
- The Core map ticks advance the normal cast to completion.
- The patient Creature's actual health increases and the Player's mana decreases.
- The Player logs out and is deleted by the Core session/map cleanup path; the patient actor is also destroyed.

This is the first Player-backed spell slice, not yet a production PlayerBot integration. It does not yet exercise a five-member Core Group, concurrent bot casters, cast interruption/movement cancellation, target range/LOS failure matrices, or a full combat rotation. The local session is deliberately not registered as a live account session; all map, spell, and cleanup behavior is exercised on the isolated test map.


### Stage E timing detail

The first Player-backed run confirmed that \`Map::Update(t_diff)\` does not advance Player spell events merely because a synthetic diff is passed. \`Map::UpdatePlayers()\` and session movement/spell processing are throttled against the Core's monotonic \`WorldTimer\`. The fixture therefore waits between bounded map updates, allowing the normal Player update cadence and spell event processor to advance in real time. This is intentional: the simulator must not fake completion by directly advancing a cast timer or applying the heal itself.


## Stage F — Engine-selected Core point movement

The first movement slice adds a destination payload to \`ActionCandidate\` and a formation trigger that derives a 2.5-yard trailing position from the Main Tank's snapshot coordinates. The adapter re-resolves mover and leader GUIDs on the current map, checks life/faction/map/instance/control state and finite destination coordinates, then invokes \`MotionMaster::MovePoint(..., MOVE_NONE)\`.

The fixture uses two active Core Creature actors on the isolated map. Runtime CI requires the Engine to select the leader as the target, the action report to be Executed/Success, the real movement generator to become \`POINT_MOTION_TYPE\`, the mover's actual distance to decrease and its final position to reach the derived destination before both actors are detached and destroyed.

This proves one point-movement primitive and the Engine-to-Core movement boundary. It does not yet implement continuous \`MoveFollow\`, pathfinding/navmesh behavior, movement interruption of a cast, formation slot arbitration or defensive movement.


### Stage F2 — movement interrupts an in-flight Player cast

A second Stage F scenario starts an ordinary non-triggered Player Lesser Heal through the Engine, confirms that the cast is pending with GCD, then runs a separate Engine movement decision for the same Player. The formation trigger derives a point from the Main Tank snapshot; the same Core \`MovePoint\` adapter moves the Player while the spell is pending.

The runtime assertion requires the cast to be cleared by the pinned Core \`Spell::update\` movement-interrupt check, the target's health to remain unchanged, the Player to reach the movement destination, and both Player/session and Creature cleanup to succeed. The simulator does not call \`InterruptSpell\` itself and does not fake a failed cast.
