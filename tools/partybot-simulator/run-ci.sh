#!/usr/bin/env bash
set -euo pipefail

# Optional GitHub Actions runtime harness for the Core-backed PartyBot simulator.
# The Google Drive fixture is the complete extracted-data directory:
# 5875/dbc, maps, vmaps and mmaps. World/character/login/log schemas are seeded
# from the public vmangos-deploy database image and cloned into disposable DBs.

LOG_FILE="${GITHUB_WORKSPACE:-$PWD}/simulator.log"
DB_CONTAINER="partybot-simulator-db"
DATA_DIR="${GITHUB_WORKSPACE:-$PWD}/simulator-data"
CONFIG_FILE="${GITHUB_WORKSPACE:-$PWD}/simulator-mangosd.conf"

: >"$LOG_FILE"

if [[ -z "${PARTYBOT_SIMULATOR_DATA_URL:-}" && -z "${PARTYBOT_SIMULATOR_DATA_SHA256:-}" ]]; then
    echo "[PartyBotSimulator] Runtime test skipped: configure repository secrets PARTYBOT_SIMULATOR_DATA_URL and PARTYBOT_SIMULATOR_DATA_SHA256." | tee -a "$LOG_FILE"
    exit 0
fi
if [[ -z "${PARTYBOT_SIMULATOR_DATA_URL:-}" || -z "${PARTYBOT_SIMULATOR_DATA_SHA256:-}" ]]; then
    echo "[PartyBotSimulator] Both extracted-data fixture secrets must be configured together." | tee -a "$LOG_FILE"
    exit 1
fi
if [[ "$PARTYBOT_SIMULATOR_DATA_URL" != https://* ]]; then
    echo "[PartyBotSimulator] The extracted-data fixture URL must use HTTPS." | tee -a "$LOG_FILE"
    exit 1
fi

cleanup() {
    if docker container inspect "$DB_CONTAINER" >/dev/null 2>&1; then
        docker logs "$DB_CONTAINER" >>"$LOG_FILE" 2>&1 || true
        docker rm -f "$DB_CONTAINER" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

command -v docker >/dev/null
command -v python3 >/dev/null

DATA_ARCHIVE="$PWD/partybot-simulator-extracted-data.tar.gz"
GDOWN_VENV="$PWD/.partybot-gdown-venv"
echo "[PartyBotSimulator] Downloading the complete Google Drive 5875 extracted-data fixture." | tee -a "$LOG_FILE"
python3 -m venv "$GDOWN_VENV"
"$GDOWN_VENV/bin/python" -m pip install --disable-pip-version-check --quiet "gdown==6.5.1"
"$GDOWN_VENV/bin/gdown" --continue --retries 3 --timeout 60 "$PARTYBOT_SIMULATOR_DATA_URL" --output "$DATA_ARCHIVE"
printf '%s  %s\n' "$(printf '%s' "$PARTYBOT_SIMULATOR_DATA_SHA256" | tr '[:upper:]' '[:lower:]')" "$DATA_ARCHIVE" | sha256sum --check

rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR" "$PWD/simulator-logs"
tar -xzf "$DATA_ARCHIVE" -C "$DATA_DIR"

DBC_DIR="$DATA_DIR/5875/dbc"
for required in Map.dbc FactionTemplate.dbc CreatureDisplayInfo.dbc; do
    if [[ ! -s "$DBC_DIR/$required" ]]; then
        echo "[PartyBotSimulator] Missing required DBC file: 5875/dbc/$required" | tee -a "$LOG_FILE"
        exit 1
    fi
done

for required_dir in maps vmaps mmaps; do
    if [[ ! -d "$DATA_DIR/$required_dir" ]] || ! find "$DATA_DIR/$required_dir" -type f -print -quit | grep -q .; then
        echo "[PartyBotSimulator] Missing or empty extracted-data directory: $required_dir/" | tee -a "$LOG_FILE"
        exit 1
    fi
done

echo "[PartyBotSimulator] Starting isolated MariaDB fixture." | tee -a "$LOG_FILE"
docker rm -f "$DB_CONTAINER" >/dev/null 2>&1 || true
docker run --detach \
    --name "$DB_CONTAINER" \
    --publish 127.0.0.1:3306:3306 \
    --env MARIADB_ROOT_PASSWORD=password \
    --env MARIADB_USER=mangos \
    --env MARIADB_PASSWORD=mangos \
    --env VMANGOS_DEPLOY_VERSION=1 \
    --env VMANGOS_REALMLIST_NAME=PartyBotSimulator \
    --env VMANGOS_REALMLIST_ADDRESS=127.0.0.1 \
    --env VMANGOS_REALMLIST_PORT=8085 \
    --env VMANGOS_REALMLIST_ICON=1 \
    --env VMANGOS_REALMLIST_TIMEZONE=0 \
    --env VMANGOS_REALMLIST_ALLOWED_SECURITY_LEVEL=0 \
    --env VMANGOS_ENABLE_AUTOMATIC_WORLD_DB_CORRECTIONS=0 \
    --env VMANGOS_HALT_ON_MIGRATION_EDITS=0 \
    --env VMANGOS_PROCESS_CUSTOM_SQL=0 \
    ghcr.io/mserajnik/vmangos-database:latest >/dev/null

database_ready=0
for attempt in $(seq 1 180); do
    # The image entrypoint first starts a temporary socket-only MariaDB while
    # importing seed SQL. Require the final TCP listener used by mangosd.
    if docker exec "$DB_CONTAINER" mariadb-admin \
        --host=127.0.0.1 --protocol=tcp \
        --user=root --password=password ping --silent >/dev/null 2>&1; then
        database_ready=1
        break
    fi
    sleep 2
done

if [[ "$database_ready" != "1" ]]; then
    echo "[PartyBotSimulator] MariaDB did not become ready within six minutes." | tee -a "$LOG_FILE"
    docker logs "$DB_CONTAINER" >>"$LOG_FILE" 2>&1 || true
    exit 1
fi

# Clone every Core schema. The simulator guard will only accept these copies.
for source_schema in mangos characters realmd logs; do
    target_schema="partybot_sim_$source_schema"
    docker exec "$DB_CONTAINER" mariadb --user=root --password=password --execute \
        "CREATE DATABASE $target_schema DEFAULT CHARSET utf8 COLLATE utf8_general_ci; GRANT ALL ON $target_schema.* TO 'mangos'@'%'; FLUSH PRIVILEGES;"
    docker exec "$DB_CONTAINER" bash -o pipefail -c \
        "mariadb-dump --user=root --password=password --single-transaction --skip-lock-tables '$source_schema' | mariadb --user=root --password=password '$target_schema'"
done

CONFIG_TEMPLATE="$PWD/build/src/mangosd/mangosd.conf.dist"
MANGOSD_BIN="$PWD/build/src/mangosd/mangosd"
if [[ ! -s "$CONFIG_TEMPLATE" || ! -x "$MANGOSD_BIN" ]]; then
    echo "[PartyBotSimulator] Expected configured mangosd binary/config were not found under build/src/mangosd." | tee -a "$LOG_FILE"
    exit 1
fi

python3 - "$CONFIG_TEMPLATE" "$CONFIG_FILE" "$DATA_DIR" "$PWD/simulator-logs" <<'PY'
import re
import sys

template, destination, data_dir, logs_dir = sys.argv[1:]
with open(template, "r", encoding="utf-8") as source:
    config = source.read()

replacements = {
    "DataDir": f'"{data_dir}"',
    "LogsDir": f'"{logs_dir}"',
    "LoginDatabase.Info": '"127.0.0.1;3306;mangos;mangos;partybot_sim_realmd"',
    "WorldDatabase.Info": '"127.0.0.1;3306;mangos;mangos;partybot_sim_mangos"',
    "CharacterDatabase.Info": '"127.0.0.1;3306;mangos;mangos;partybot_sim_characters"',
    "LogsDatabase.Info": '"127.0.0.1;3306;mangos;mangos;partybot_sim_logs"',
    "PartyBot.Simulator.Enable": "1",
    "PartyBot.Simulator.DatabaseNamePrefix": "partybot_sim_",
    "PartyBot.Simulator.MapId": "33",
}

for key, value in replacements.items():
    pattern = re.compile(r"^\s*" + re.escape(key) + r"\s*=.*$", re.MULTILINE)
    config, count = pattern.subn(f"{key} = {value}", config)
    if count != 1:
        raise SystemExit(f"Expected exactly one config entry for {key}, found {count}")

with open(destination, "w", encoding="utf-8") as output:
    output.write(config)
PY

echo "[PartyBotSimulator] Running one-shot Stages A-F12 Core simulator on disposable schemas." | tee -a "$LOG_FILE"
set +e
timeout --signal=TERM --kill-after=15s 10m "$MANGOSD_BIN" -c "$CONFIG_FILE" 2>&1 | tee -a "$LOG_FILE"
simulator_status=${PIPESTATUS[0]}
set -e

if [[ "$simulator_status" != "0" ]]; then
    echo "[PartyBotSimulator] mangosd exited with status $simulator_status." | tee -a "$LOG_FILE"
    exit "$simulator_status"
fi

if ! grep -Fq "[PartyBotSimulator] PASS: real Creature updates, a Core-calculated applied-damage threat component matched the real ThreatManager increment, and Core retained its 110% melee target switching." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core applied-damage threat-impact parity assertion marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: negative flat spell-threat estimator rejected incomplete target sets, invalid masks, duplicate/unresolved targets and mixed-sign spells without emitting impacts." "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing negative flat spell-threat fail-closed PASS marker." >&2
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: a real harmful Core aura spell routed negative flat spell_threat to its affected hostile and matched the estimator against the actual ThreatManager delta." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The negative flat spell-threat Core cast parity assertion marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a real Core Aura update delivered one periodic damage tick through Aura::PeriodicTick and Unit::DealDamage(DOT); the applied-tick threat component matched the actual ThreatManager delta." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core periodic-damage tick threat parity marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a real Core periodic-healing Aura tick distributed 50% of effective healing across both live hostile references; the component matched each actual ThreatManager delta." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core periodic-healing assist tick threat parity marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "impactMatches=1" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core applied-damage threat-impact estimate did not match the actual ThreatManager increment." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] Actor cleanup: boss=1 tank=1 damageDealer=1." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The actor cleanup PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi


if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine selected a tank-assist action and executed real Unit::Attack against a hostile Core Creature." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The PartyBotEngine attack-executor PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine selected healer triage and the Core Spell pipeline applied a real Lesser Heal effect." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The PartyBotEngine Core spell/heal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi


if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine selected a known Player spell, Core accepted a non-triggered cast with GCD, and the cast completed with real healing and mana cost." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player-backed non-triggered spell/heal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi


if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine selected a formation destination and the Core PointMovementGenerator moved the actor to it." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core PointMovementGenerator PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi


if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine selected Core MoveFollow and the native FollowMovementGenerator tracked the Main Tank after it relocated." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core FollowMovementGenerator PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Core Player FollowMovementGenerator paused while its non-triggered Lesser Heal cast was pending and resumed tracking the Main Tank after the cast completed." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player follow/cast safety PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine retargeted native Core FollowMovementGenerator to the new Main Tank and recovered after that target disappeared." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core follow retarget/recovery PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Core Player Spell::CheckCast rejected a heal through a real closed GameObject collision model without applying healing, spending mana or starting a cast." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player heal LOS rejection PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Core death cleanup removed Main Tank and healer references from both adds; SelectHostileTarget retargeted first to the surviving healer and then to the surviving DPS, and PartySnapshotBuilder copied both post-death states." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core Tank/Healer death-reference recovery PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: one real five-Player Core Group passed initial roster/cache validation, two Core hostile adds switched from Main Tank to healer through assist-threat, final threat values and both reachable/unreachable per-reference melee states matched Core, and all Group/session cleanup succeeded." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The combined five-Player Group and multi-add/healer-threat PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "meleeReachability=4/4 reachable=2 unreachable=2" "$LOG_FILE"; then
    echo "[PartyBotSimulator] Expected two reachable and two unreachable per-reference melee-reachability parity checks." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: PartySnapshotBuilder discovered a non-current-victim hostile through a party member HostileRefManager and copied its offline threat reference state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The non-victim/offline HostileRefManager snapshot PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotEngine refused Core MovePoint while the mover had CAN_NOT_MOVE or POSSESSED state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The MovePoint control-safety PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Core Player CastSpell rejected a stale out-of-range heal and a late no-mana heal without applying effects, consuming mana, starting a cast or setting GCD." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player heal range/resource revalidation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Engine-selected Player movement interrupted a pending Core Lesser Heal cast without applying the heal." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player movement-cast-interruption PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: created, ran a bounded Core Creature combat/threat scenario and deleted map" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The map/combat lifecycle PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

echo "[PartyBotSimulator] Runtime PASS: Core map, Creature updates, Core-applied direct/periodic-damage and periodic-healing-assist threat-impact parity, threat switching, Engine attack, triggered spell healing, Player non-triggered spell completion, Core point movement, native FollowMovementGenerator tracking, Player follow pause/resume around a non-triggered cast, follow retarget/recovery after leader handoff and target removal, Player heal range/resource/LOS rejection, movement refusal under CAN_NOT_MOVE/POSSESSED, combined real five-Player Core Group roster/cache snapshot, multi-add healer-threat and Core threat projection, Tank/Healer death-reference cleanup, multi-add retarget recovery, non-victim/offline HostileRefManager threat discovery, movement-driven Player cast interruption and actor cleanup." | tee -a "$LOG_FILE"
