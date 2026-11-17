#!/usr/bin/env bash
set -euo pipefail

# Optional GitHub Actions runtime harness for the Core-backed PartyBot simulator.
# The only private fixture required by this first smoke test is the full Vanilla
# 5875 DBC directory. World/character/login/log schemas are seeded from the
# public vmangos-deploy database image and cloned into disposable prefixed DBs.

LOG_FILE="${GITHUB_WORKSPACE:-$PWD}/simulator.log"
DB_CONTAINER="partybot-simulator-db"
DATA_DIR="${GITHUB_WORKSPACE:-$PWD}/simulator-data"
CONFIG_FILE="${GITHUB_WORKSPACE:-$PWD}/simulator-mangosd.conf"

: >"$LOG_FILE"

if [[ -z "${PARTYBOT_SIMULATOR_DBC_URL:-}" || -z "${PARTYBOT_SIMULATOR_DBC_SHA256:-}" ]]; then
    echo "[PartyBotSimulator] Runtime test skipped: configure repository secrets PARTYBOT_SIMULATOR_DBC_URL and PARTYBOT_SIMULATOR_DBC_SHA256." | tee -a "$LOG_FILE"
    exit 0
fi

cleanup() {
    if docker container inspect "$DB_CONTAINER" >/dev/null 2>&1; then
        docker logs "$DB_CONTAINER" >>"$LOG_FILE" 2>&1 || true
        docker rm -f "$DB_CONTAINER" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

command -v docker >/dev/null
command -v curl >/dev/null
command -v python3 >/dev/null

DBC_ARCHIVE="$PWD/partybot-simulator-dbc.tar.gz"
echo "[PartyBotSimulator] Downloading the private 5875 DBC fixture." | tee -a "$LOG_FILE"
curl --fail --silent --show-error --location "$PARTYBOT_SIMULATOR_DBC_URL" --output "$DBC_ARCHIVE"
echo "$PARTYBOT_SIMULATOR_DBC_SHA256  $DBC_ARCHIVE" | sha256sum --check

rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR" "$PWD/simulator-logs"
tar -xzf "$DBC_ARCHIVE" -C "$DATA_DIR"

DBC_DIR="$DATA_DIR/dbc/5875"
for required in Map.dbc FactionTemplate.dbc CreatureDisplayInfo.dbc; do
    if [[ ! -s "$DBC_DIR/$required" ]]; then
        echo "[PartyBotSimulator] Missing required DBC file: dbc/5875/$required" | tee -a "$LOG_FILE"
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
    if docker exec "$DB_CONTAINER" mariadb-admin --user=root --password=password ping --silent >/dev/null 2>&1; then
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
        "CREATE DATABASE `$target_schema` DEFAULT CHARSET utf8 COLLATE utf8_general_ci; GRANT ALL ON `$target_schema`.* TO 'mangos'@'%'; FLUSH PRIVILEGES;"
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
    pattern = re.compile(r"^\\s*" + re.escape(key) + r"\\s*=.*$", re.MULTILINE)
    config, count = pattern.subn(f"{key} = {value}", config)
    if count != 1:
        raise SystemExit(f"Expected exactly one config entry for {key}, found {count}")

with open(destination, "w", encoding="utf-8") as output:
    output.write(config)
PY

echo "[PartyBotSimulator] Running one-shot Stage A/B Core simulator on disposable schemas." | tee -a "$LOG_FILE"
set +e
timeout --signal=TERM --kill-after=15s 10m "$MANGOSD_BIN" -c "$CONFIG_FILE" 2>&1 | tee -a "$LOG_FILE"
simulator_status=${PIPESTATUS[0]}
set -e

if [[ "$simulator_status" != "0" ]]; then
    echo "[PartyBotSimulator] mangosd exited with status $simulator_status." | tee -a "$LOG_FILE"
    exit "$simulator_status"
fi

if ! grep -Fq "[PartyBotSimulator] PASS: created, ticked, ran a bounded Core Creature combat/threat scenario" "$LOG_FILE"; then
    echo "[PartyBotSimulator] mangosd exited successfully but the Stage A/B PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] Actor cleanup: boss=1 tank=1 damageDealer=1." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The actor cleanup PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

echo "[PartyBotSimulator] Runtime PASS: Core map, Creature updates, damage, threat switching and actor cleanup." | tee -a "$LOG_FILE"
