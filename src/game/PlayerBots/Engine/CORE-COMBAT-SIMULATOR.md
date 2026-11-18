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
- [x] Player movement-interrupt fixture: the Engine selects the formation destination, and the simulator models the inbound mover-relocation operation by calling `Player::SetPosition`. This enters the native `Player::SetPosition` → `Unit::HandleInterruptsOnMovement` path; the simulator does not inspect spell flags or cancel spells itself. Runtime CI run `#44145923142` proves `castCancelled=1`, patient health `50000>50000`, movement completion and cleanup.
- [x] Add continuous follow/formation tracking on native Core movement generators, Player cast pause/resume, Main Tank handoff and recovery after target removal. Runtime CI `#44159191502` and `#44163805016` pass these distinct Player/Creature slices.
- [x] Stage F6 action-time range/resource revalidation: CI `#44167535205` confirms Core rejects the stale out-of-range and late no-mana Player heals without side effects.
- [x] Stage F7: real dynamic GameObject LOS rejection and `CAN_NOT_MOVE`/`POSSESSED` movement rejection passed in CI `#44170264755`.
- [x] Stage F8: create and tear down a real five-member Core Group and verify production PartySnapshotBuilder reads its same-map Player roster. CI `#44175017402` passed with five members, accurate value copies, cache reuse, Group/ObjectMgr removal, Player session logout and Map detachment.

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

**Runtime update (2026-11-17):** Stage F4 passed on push CI `#44159191502` (`5d3c9c5`): the Core Player FollowMovementGenerator remained paused during five no-movement-cast gate ticks, the ordinary Lesser Heal completed with mana cost, and following resumed to a final separation of `2.00` with cleanup. Stage F5 then passed on CI `#44163805016` (`cfdda0c`): target handoff to a new Main Tank, tracking the new leader instead of the old one, removal of the active target and recovery to the remaining Main Tank all succeeded; final separation was `4.55`, with Follow state and cleanup intact. Stage F6 passed on push CI `#44167535205` (`e52bea6`): stale-snapshot range rejection returned `SPELL_FAILED_OUT_OF_RANGE` (`89`) with no health/mana/cast/GCD side effects, and late mana depletion returned `SPELL_FAILED_NO_POWER` (`77`) with no heal/cast/GCD. Stage F7 passed on push CI `#44170264755` (`e46a246`): Core `Spell::CheckCast` rejected the heal through dynamic GameObject Entry `13965` (`testedModels=13`, `blockedLOS=1`, result `42`), LOS restored after removal, and Core rejected MovePoint for both `CAN_NOT_MOVE` and `POSSESSED`. Stage F8 passed on push CI `#44175017402` (`434231f`): the actual Core Group was registered with five members and a Main Tank assignment; production PartySnapshotBuilder returned all five same-map Player GUIDs and correct scalar state; cache reuse, Core Disband/ObjectMgr removal, session logout and Map detachment all passed, followed by final Runtime PASS. The first Stage F8 attempt (`#44172739682`) had emitted a fixture-local PASS but failed later; the accepted run moved Group teardown after the Player interruption scenario and added explicit Map GUID cleanup assertions. Stage F9 passed on push CI `#44182086347` (`02f0d80`): the single five-Player Group passed initial same-map roster/value/cache checks; two Core hostile adds initially targeted Main Tank; a 20%-max-health nonlethal hit left Tank alive at `343/1717`; ordinary Lesser Heal completed (`1374>1423`, mana `2896>2866`); Core `threatAssist(400)` distributed assist threat and both adds selected healer (`212.2` each); production snapshot copied both threats/victims and Group/session cleanup passed, followed by final Runtime PASS. Stage F10 passed on push CI `#44185236970` (`1a463d8`): after the real five-Player Group's two adds selected the healer through Core `threatAssist`, normal Core lethal damage killed the Main Tank and removed its references from both ThreatManagers; native `SelectHostileTarget` retained the living healer. A refreshed production snapshot marked the Tank dead and copied both healer victims/threat values. Killing the healer then removed both healer references; native selection moved both adds to the surviving DPS, and a second refreshed snapshot marked Tank+Healer dead, DPS alive, copied the DPS as both victims, and contained no stale dead-player threat entries. Group removal, all session cleanup and final Runtime PASS succeeded. Stage F11 adds Core-derived melee reachability to threat snapshots so the engine can distinguish Core's 110% melee and 130% ranged aggro thresholds. Do not jump to dungeon scripts.


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


## Stage F — Engine-selected Core movement and defensive-action foundations

### Stage F1 — Core point movement

The first movement slice adds a destination payload to \`ActionCandidate\` and a formation trigger that derives a 2.5-yard trailing position from the Main Tank's snapshot coordinates. The adapter re-resolves mover and leader GUIDs on the current map, checks life/faction/map/instance/control state and finite destination coordinates, then invokes \`MotionMaster::MovePoint(..., MOVE_NONE)\`.

The fixture uses two active Core Creature actors on the isolated map. Runtime CI requires the Engine to select the leader as the target, the action report to be Executed/Success, the real movement generator to become \`POINT_MOTION_TYPE\`, the mover's actual distance to decrease and its final position to reach the derived destination before both actors are detached and destroyed.

This proves one Core point-movement primitive and the Engine-to-Core movement boundary. Continuous follow and native Player cast interruption are covered by the later Stage F2/F3 fixtures; formation slot arbitration and defensive movement remain out of scope.


### Stage F2 — movement interrupts an in-flight Player cast

A second Stage F scenario starts an ordinary non-triggered Player Lesser Heal through the Engine, confirms that the cast is pending with GCD, then runs a separate Engine movement decision for the same Player. The formation trigger derives a destination from the Main Tank snapshot. For this Player fixture, the adapter models the inbound mover-relocation entry point by calling `Player::SetPosition` with that destination; the Core owns movement-interrupt policy.

Runtime CI `#44145923142` verifies the native `Player::SetPosition` -> `Unit::HandleInterruptsOnMovement` -> `SpellCaster::InterruptSpellsWithInterruptFlags` path cancels the cast, preserves target health (`50000>50000`), reaches the destination and cleans up the Player/session and Creature. The adapter does not duplicate spell-interruption policy or call `InterruptSpell` directly.

Stage F2 timing note: the fixture performs one ordinary Player/map update before the cast begins. This primes `Map::UpdatePlayers`' wall-clock cadence so the first post-movement update cannot carry a stale multi-second diff and complete the heal before a subsequent tick observes movement. The test relies on real Core Player relocation and movement-interrupt handling; it never advances cast state directly or applies healing itself.

### Stage F2 investigation history (superseded by the passing native-path runtime test)

The earlier failed runs are retained here as debugging history only. Attempts that manually inspected spell flags or called `InterruptSpell` from the simulator adapter were removed because they bypassed Core's native relocation path. Source audit established that inbound mover relocation enters `Player::SetPosition` and `Unit::HandleInterruptsOnMovement`, whereas server-side point movement updates the Player spline later in `Unit::Update`.

The accepted fixture models the inbound relocation seam through `Player::SetPosition`; it does not construct or inject a network movement packet, and it does not implement spell-interruption policy. Runtime CI `#44145923142` passed with the generic spell cleared, target health unchanged, movement complete and actor/session cleanup successful.

### Stage F3 — continuous follow through the native Core FollowMovementGenerator

The follow fixture adds a `core_follow` action and a Main-Tank follow trigger. The action re-resolves the follower and leader from the current map, validates life, faction, map/instance and movement-control state, then calls `MotionMaster::MoveFollow(leader, 2.5f, PI)`.

The simulator does not calculate per-tick movement or write positions. Core's `FollowMovementGenerator` owns path generation, movement updates and target tracking. The runtime scenario requires the Engine to select and execute the follow action, the follower to reach formation range, and then the Main Tank to move 10 yards using Core `MovePoint`. The follower must track that relocation, retain `FOLLOW_MOTION_TYPE`/`UNIT_STATE_FOLLOW`, and finish within 5 yards of the leader before both actors are cleaned up.

Runtime CI `#44153619126` on commit `ccb4707` passed: `initialCaughtUp=1`, `leaderReached=1`, `followerTracked=1`, final separation `1.87`, `followGenerator=1`, `cleanup=1`.

The fixture is still a Core-backed simulator proof using Creature actors, not production PlayerBot follow behavior or a real Core Group. Retargeting/recovery edge cases, formation slot arbitration, LOS/range checks and defensive movement remain future work.

### Stage F4 — Player follow pauses for a non-triggered cast

This fixture uses a Core `Player` with a local `WorldSession` as the follower and a template-backed Creature as the Main Tank. PartyBotEngine selects the normal `core_follow` action, which installs Core's `FollowMovementGenerator<Player>`. After the Player reaches the formation distance and stops, PartyBotEngine selects the existing Player-backed Lesser Heal action against the Main Tank. The leader then moves through its Core `PointMovementGenerator` while the Player's cast remains pending.

The intended Core behavior is the guard in `FollowMovementGenerator<T>::Update`: while `IsNoMovementSpellCasted()` is true, the Core stops the follower and does not start another movement spline. The test observes the real pending Player spell and Core Player position/state; it does not implement a simulator-side movement veto, write the Player position, or directly complete the cast. After the ordinary Core SpellEvent completes the heal and consumes mana, the same FollowMovementGenerator must resume tracking the moving Main Tank.

Acceptance requires the Engine-selected follow and heal actions, a pending/GCD cast over multiple Player updates, the Core `IsNoMovementSpellCasted()` gate returning true on at least two updates, the Player remaining within 0.25 yards of its pre-cast position while that gate is active, the leader reaching its destination, a real completed heal/mana cost, follow resuming after cast completion, final formation distance within 5 yards, the Follow generator remaining active, and Player/session plus Creature cleanup. The fixture waits for two consecutive position-stable samples within formation range, then resets the leader to half health immediately before triage so passive Creature regeneration during the approach cannot invalidate the heal candidate. Runtime CI `#44159191502` on commit `5d3c9c5` passed: `initialCaughtUpStable=1`, `stationarySamples=2`, `castAccepted=1`, `pendingTicks=6`, `coreNoMovementTicks=5`, `maxCastMove=0.00`, `castCompleted=1`, `leaderReached=1`, `followPaused=1`, `followedAfterCast=1`, `finalDistance=2.00`, `followRetained=1`, `cleanup=1`.

The first runtime attempt (`#44157542291`) compiled and ran all previous stages successfully, and showed the Player follow generator tracking the leader after relocation. The new fixture did not reach the cast phase: the Player's arrival condition used the Core `IsStopped()` flag, which did not serve as a reliable settled-position observation for this Player movement; during the approach the injured Creature also regenerated enough health that healer triage emitted no candidate. The revised fixture uses two consecutive position-stable samples and reapplies the half-health test condition immediately before evaluating healer triage. This remains a test-fixture correction; no Core combat policy is being reimplemented.

This is a Player-backed Core simulator fixture, but the test Player still uses a local unauthenticated session and is not a production PlayerBot or real Core Group member.


### Stage F5 — Follow target handoff and recovery

The simulator's \`core_follow\` action remembers the last successfully selected target GUID. A repeated candidate for the same active FollowMovementGenerator is a no-op; a candidate for a different Main Tank remains useful and is executed through the Core \`MotionMaster::MoveFollow\` API, which replaces the current movement generator with a new native \`FollowMovementGenerator\`.

The runtime fixture creates one follower and two friendly leader Creatures. It first follows Main Tank A, then changes the snapshot assignment to Main Tank B while the original FollowMovementGenerator is still active. It requires the Engine to select B and successfully execute the retarget action. Both leaders then move through Core \`PointMovementGenerator\`s in opposite directions; the follower must arrive within 5 yards of B and remain at least 15 yards from A, proving the Core generator tracks the new leader rather than the stale target.

Next, the fixture removes B while it is the active follow target, advances normal Core map updates, promotes the still-present A back to Main Tank, and requires the same Engine action to recover by installing a new Core FollowMovementGenerator for A. A moves again through Core \`MovePoint\`; the follower must track A, retain FOLLOW state and clean up all actors.

The simulator does not move the follower, rewrite positions, or synthesize target tracking. It only updates the scalar PartySnapshot, evaluates the Engine, and invokes Core movement APIs. Runtime acceptance also requires both leader movement generators, handoff and recovery decisions, followed-target removal, final formation range, and complete actor cleanup.





### Stage F8 — Real Core five-member Group and production snapshot builder

This fixture creates five real Core Player objects through the same local-session Player construction helper used by the Player spell tests. It creates a normal Core Group with the first Player as leader, registers that Group with ObjectMgr, and adds the other four Players through Group::AddMember. The group is a real five-slot party, not a hand-authored member vector.

The fixture calls the production PartySnapshotBuilder::GetOrBuild using the first Player and a real PartyCoordinator. It requires the resulting value snapshot to use the actual Group ID, Map ID and instance ID, contain exactly five members, and resolve every actual Player GUID through the Group member slots and current Map. It verifies online/alive state, Priest class/level, health/max health, power/max power and coordinates. The test Players have no PartyBotAI attached, so the expected role is explicitly Unknown; this is not misreported as a role-classification test.

A second GetOrBuild call inside the cache window must return the same snapshot epoch/capture time. The fixture then calls Core Group::Disband, verifies every Player has been unlinked, removes the Group from ObjectMgr, and logs out all five local sessions. Group and group_member rows are created/deleted only inside the runner's disposable partybot_sim_* database clones; no production character or group data is modified.

Runtime acceptance requires five unique same-map Player GUIDs, Group registration and membership, a five-member production snapshot, accurate scalar copies, cache reuse, Group/ObjectMgr removal and complete session cleanup.






### Stage F10 — Main Tank and healer death cleanup with multi-add recovery

This scenario continues inside the same five-Player Core Group and the same two hostile Creature threat lists used for F9; no second Player/session/Group fixture is created. A third Player first establishes lower real Core damage threat on both adds while the healer remains above the 130% ranged switch threshold.

The Main Tank is then killed by normal Core Creature DealDamage using its exact current health. The acceptance check requires the Player to enter the Core death state, both mobs' ThreatManager entries for the Tank to disappear through the Player HostileRefManager death cleanup, and native Unit::SelectHostileTarget to leave both adds on the still-living healer. A fresh production PartySnapshotBuilder call from the healer must contain five members, mark the Main Tank dead, and show both hostiles targeting the healer with the surviving threat entries copied.

Next the active healer is killed through another real Core DealDamage event. Its HostileRefManager must remove the healer from both hostile ThreatManagers; after native SelectHostileTarget calls, both mobs must select the remaining DPS. A final fresh production snapshot from that surviving DPS must show the Tank and healer dead, the DPS alive, exactly two hostile snapshots, both current victims set to the DPS, only the remaining live DPS threat entries, and no stale threat entries for either dead player.

The simulator does not remove references or set victims itself. Unit::DealDamage and Unit::SetDeathState own death; HostileRefManager::deleteReferences propagates the reference removals; Unit::SelectHostileTarget and ThreatManager own victim selection; PartySnapshotBuilder reads the resulting live Core state. Acceptance includes a single Group/ObjectMgr disband, both hostile Creature cleanups, all five Player session logouts, Map GUID detachment, and the full runtime PASS marker.


### Stage F9 — five-Player multi-add healer-threat projection

This fixture creates five real Core Player objects and local WorldSessions, registers a real Group, converts it to raid mode only so the pinned Group Main Tank assignment API is available, then marks the first Player as Main Tank. Two template-backed hostile Creatures are inserted on the same Map using the ordinary Core Creature lifecycle.

Both hostiles enter combat with the Main Tank and acquire their initial threat through real Player DealDamage calls. The second Player then completes a normal non-triggered Lesser Heal on the injured Main Tank. The fixture applies one explicit scripted assist-threat event through the Core HostileRefManager::threatAssist API on the healed tank; the API itself distributes that event over the two real hostile references, and each hostile ThreatManager::getHostileTarget must select the healer after the configured event crosses the actual Core threat-switch threshold. The simulator does not write threat values into either ThreatManager, sort a fake threat list, choose a victim itself or duplicate the Core selector.

The fixture then calls the production PartySnapshotBuilder::GetOrBuild from the real Group leader. It requires a five-member roster with the actual Main Tank marker, exactly two hostile snapshots, both hostile current victims set to the healer, and threat entries for Main Tank and healer copied exactly from the corresponding Core ThreatManager values.

The assist amount is an explicit scenario event (400 total across two hostile references), not a claim that this Core/database revision automatically derives threat from Lesser Heal's amount. The first F9 attempt (`#44178467285`, `c41721f`) applied 3000 damage to an unequipped level-60 Priest, killing the Tank before the heal; the zero-health/zero-mana-cost diagnostic and empty hostile-reference list came from Core's normal death cleanup. The next attempt (`#44180425226`) also confirmed that running two independent five-Player Group fixtures sequentially on this headless Map is not a valid lifecycle test. F9 now reuses one five-Player Group: initial roster/value/cache assertions run before combat; Tank damage is 20% of live max health and must leave the Tank alive and injured; the healer faces the target before the normal cast; final threat projection and the single Group/session cleanup run at the end. The intrusive HostileRefManager list is iterated directly, and both initial tank threat values remain in the diagnostics. The real heal is still performed through the ordinary spell pipeline, while the test separately targets the shared Core assist-threat path used for positive spell/buff/heal threat when such an event is emitted. Follow-up scenarios must still cover actual tank death, healer death and re-target recovery across multiple adds.

Acceptance also includes F8's initial same-map roster, Main Tank marker, copied scalar values and cached-snapshot reuse; then all hostile Creature threat references are removed, the single Group is disbanded/removed from ObjectMgr, all five sessions log out, every Player GUID disappears from the Map, and the entire simulator run reaches its final Runtime PASS marker.


### Stage F7 — Core LOS collision and movement-control safety

The LOS fixture creates a real Player healer and injured Creature target with a clear baseline ray. It discovers loaded Vanilla GameObject templates with a collision-bearing door or solid generic model, creates each candidate through GameObject::Create, and inserts it into the map so Core registers the model in the dynamic collision tree. The fixture keeps the first model/orientation that makes Player::IsWithinLOSInMap(target) return false. No simulator LOS flag or synthetic geometry result is used.

PartyBotEngine selects its ordinary Player Lesser Heal while the PartySnapshot still contains the same living injured target. After the closed GameObject model blocks the line, the action calls ordinary Player::CastSpell; Core Spell::CheckCast must return SPELL_FAILED_LINE_OF_SIGHT. The fixture requires no health change, mana cost, pending cast or GCD, then removes the GameObject through the non-persisting map remove path, verifies LOS is restored and cleans the Player/session and Creature.

The movement-control fixture creates a normal Engine formation candidate and evaluates the real SimulatorCoreMovePointAction while the Core mover has UNIT_STATE_CAN_NOT_MOVE, then UNIT_STATE_POSSESSED. Both candidates must be rejected as impossible before execution; no PointMovementGenerator may start and the mover's position must remain unchanged. The adapter's execution-time check applies the same state mask so stale selection cannot bypass the movement safety gate.

The simulator changes no combat or LOS policy. Core dynamic collision and Core Unit state are authoritative; the fixture only creates/removes a real GameObject and injects the corresponding Core unit-state bits to test the executor boundary. A real root/stun aura-to-state transition and additional crowd-control types remain future coverage.

Runtime CI `#44170264755` on `e46a246` passed: `registered=1`, `selected=1`, baseline LOS clear, `blockerTemplates=370`, `testedModels=13`, collision model `blockerEntry=13965`, `blockedLOS=1`, `executionFailed=1`, `castResult=42` (`SPELL_FAILED_LINE_OF_SIGHT`), health `50000>50000`, mana `2896>2896`, no pending/GCD, `losRestored=1`, `cleanup=1`. The movement fixture also passed with `rootedRejected=1`, `possessedRejected=1`, `noMovementStarted=1`, `positionUnchanged=1`, `cleanup=1`.


### Stage F6 — Player heal execution-time range and resource rejection

This fixture deliberately exercises the difference between a value snapshot and authoritative Core state. It first constructs an injured target 60 yards from a real Core Player healer, while the PartySnapshot still contains a stale nearby position for that target. PartyBotEngine is expected to select the heal candidate from the value snapshot; the Engine action then calls ordinary non-triggered Player::CastSpell, and the pinned Core Spell::CheckCast must return SPELL_FAILED_OUT_OF_RANGE. The action result must be ExecutionFailed/Failed, with no health change, mana consumption, pending cast, or GCD.

The second case evaluates a heal candidate while the Player has normal mana, then sets the Player's real mana to zero immediately before ExecuteSelected. The same action must reach Core Spell::CheckPower, which returns SPELL_FAILED_NO_POWER; again, the result must not be reported as success and there must be no health change, cast, GCD or resource consumption.

This is intentionally an execution-boundary proof, not a simulator implementation of spell range or mana-cost formulae. The fixture does not calculate range, LOS, or mana cost itself; it requires the pinned Core Spell pipeline to reject the action and reports that authoritative result separately from the Engine decision.

Runtime acceptance: both candidates are selected from their scalar snapshots, both execution attempts fail with the exact expected Core SpellCastResult, health remains unchanged, no cast/GCD starts, the first case consumes no mana, the second case remains at zero mana, and Player/session plus both Creature fixtures are cleaned up. LOS remains a separate open case until a deterministic blocked VMap line can be identified in the headless test map.

Runtime CI `#44167535205` on `e52bea6` passed. The stale-nearby snapshot/60-yard Core target case returned `SPELL_FAILED_OUT_OF_RANGE` (`89`) with `rangeSelected=1`, `rangeExecutionFailed=1`, health `50000>50000`, mana `2896>2896`, `rangeNoPending=1`, `rangeNoGcd=1`. The late resource depletion case returned `SPELL_FAILED_NO_POWER` (`77`) with `noPowerSelected=1`, `noPowerExecutionFailed=1`, health `50000>50000`, mana `0`, no pending cast/GCD, and `cleanup=1`.


Runtime CI `#44163805016` on commit `cfdda0c` passed: `aiReady=1`, `registered=1`, `initialSelected=1`, `initialFollow=1`, `initialCaughtUp=1`, `handoffSelected=1`, `handoffExecuted=1`, `handoffTargetChanged=1`, both leader movement generators started and reached destination, `followerTrackedNewLeader=1`, `oldLeaderRemoved=1`, `recoverySelected=1`, `recoveryExecuted=1`, `recoveryTargetRestored=1`, `recoveredToAvailableLeader=1`, final separation `4.55`, `followGenerator=1`, `cleanup=1`.



### Stage F11 — Core melee reachability on each threat reference

The production snapshot builder copies `HostileReference::getTarget()` reachability by calling the hostile Creature's native `Unit::CanReachWithMeleeAutoAttack`; it does not derive reachability from scalar distance or party-member-to-member geometry. Each `ThreatEntrySnapshot` keeps only the target GUID, Core threat value/status flags, and the Core-computed melee-reachability bit.

The F11 runtime fixture exercises the same four Main Tank/Healer references across two hostile Creatures. The first hostile is placed close enough for both Players to be melee-reachable; the second stays well outside melee reach. The simulator re-resolves each GUID from the hostile's Map and compares the copied snapshot bit with a fresh direct Core API call. Acceptance now requires exactly four parity matches with the expected coverage split: two reachable and two unreachable. The CI harness separately greps the explicit coverage counters, so a fixture that accidentally places all samples in one state cannot pass.

Runtime CI `#44213077421` on commit `6066306` passed on the Linux ARM64 / Vanilla 5875 runner. The F9 diagnostics reported `meleeReachability=4/4 reachable=2 unreachable=2`; every copied bit matched a fresh `CanReachWithMeleeAutoAttack` call for the same resolved Core Unit. The runner required the 2/2 coverage split, completed the full Core-backed simulator scenario, observed the final Runtime PASS and shut down cleanly. F11 is complete.


### Stage F12 — discover every hostile reference held by a party member

The production builder previously discovered hostiles through member current-victim and attacker relationships. That misses a hostile which still has a threat reference to a party member but has switched its current victim to an outsider or another non-party unit. This matters for healing/assist threat: Core `HostileRefManager::threatAssist` walks every reference held by the healed unit, not only mobs currently attacking it.

Stage F12 extends the builder to enumerate each Group member's `GetHostileRefManager().getFirst()` chain. Each reference's source ThreatManager resolves to the hostile owner; the builder adds that hostile to the snapshot even when no Group member currently has it as an attacker or target. It also merges a missing per-member threat entry from the linked reference itself. This preserves offline references which have been moved out of the hostile's active online `getThreatList()`, together with Core's `isOnline` and `isAccessable` flags.

The runtime fixture uses one real five-Player Group, two hostile Creatures and one same-faction Creature outside the Group. After the second hostile switches its current victim to the outsider, no Group member is its current attacker. The fixture then marks the Tank's reference offline through the Core HostileReference API, proves `getThreat(tank, false)` no longer sees it while `getThreat(tank, true)` still does, and requires PartySnapshotBuilder to discover the hostile from the Tank's HostileRefManager and copy the offline entry. It restores the reference and hostile target before F10's Tank/Healer death-recovery assertions. This direct status transition is a deterministic fixture seam to test the Core offline-container path; the builder itself does not mutate reference state.

Runtime CI `#44215877767` on commit `1a3aa02` passed on the Linux ARM64 / Vanilla 5875 runner. The fixture reported `outsideVictim=1 noPartyAttacker=1 tankRef=1 offlineSnapshot=1 nonVictimHostile=1 restored=1`; the runner required the explicit F12 PASS marker, the prior F10 death-recovery assertions, and the full simulator final Runtime PASS. The offline reference was toggled only through the Core HostileReference API to exercise the offline-container seam; PartySnapshotBuilder itself only read and copied Core state. F12 is complete.