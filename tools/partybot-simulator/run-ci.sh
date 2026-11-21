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
if [[ "$PARTYBOT_SIMULATOR_DATA_URL" == *"drive.google.com"* || "$PARTYBOT_SIMULATOR_DATA_URL" == *"docs.google.com"* ]]; then
    echo "[PartyBotSimulator] Downloading the 5875 extracted-data fixture from Google Drive." | tee -a "$LOG_FILE"
    GDOWN_VENV="$PWD/.partybot-gdown-venv"
    python3 -m venv "$GDOWN_VENV"
    "$GDOWN_VENV/bin/python" -m pip install --disable-pip-version-check --quiet "gdown==6.5.1"
    "$GDOWN_VENV/bin/gdown" --continue --retries 3 --timeout 60 "$PARTYBOT_SIMULATOR_DATA_URL" --output "$DATA_ARCHIVE"
else
    # Supports HTTPS object URLs such as an OCI Object Storage pre-authenticated
    # request (PAR). Keep the bucket private; the URL itself grants read-only
    # access to this single immutable archive. Never print the signed URL.
    echo "[PartyBotSimulator] Downloading the 5875 extracted-data fixture from a direct HTTPS source." | tee -a "$LOG_FILE"
    command -v curl >/dev/null
    curl --fail --location --show-error \
        --retry 5 --retry-all-errors --retry-delay 5 \
        --connect-timeout 30 --max-time 1800 \
        "$PARTYBOT_SIMULATOR_DATA_URL" --output "$DATA_ARCHIVE"
fi
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
    "PlayerBot.DecisionTrace": "1",
    "PlayerBot.EngineShadow": "1",
}

for key, value in replacements.items():
    pattern = re.compile(r"^\s*" + re.escape(key) + r"\s*=.*$", re.MULTILINE)
    config, count = pattern.subn(f"{key} = {value}", config)
    if count != 1:
        raise SystemExit(f"Expected exactly one config entry for {key}, found {count}")

with open(destination, "w", encoding="utf-8") as output:
    output.write(config)
PY

echo "[PartyBotSimulator] Running one-shot Stages A-F27 Core simulator on disposable schemas." | tee -a "$LOG_FILE"
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

if ! grep -Fq "[PartyBotSimulator] PASS: a real Core periodic-healing Aura tick distributed half the effective healing across both live hostile references; the component matched each actual ThreatManager delta." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core periodic-healing assist tick threat parity marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Core PERIODIC_ENERGIZE granted assist threat for energy but not mana, while OBS_MOD_MANA did grant mana assist threat; the component matched each real two-hostile ThreatManager result." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core periodic-power assist threat parity marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a real Core periodic mana-leech tick drained target Mana, credited the caster's effective Mana gain and matched the target ThreatManager delta with the direct 50-percent assist component." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core periodic mana-leech threat parity marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a real Core periodic life-leech tick matched both the hostile damage threat and the self-heal assist fan-out across two hostile ThreatManagers." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Core periodic life-leech composite threat parity marker was not found." | tee -a "$LOG_FILE"
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


if ! grep -Fq "[PartyBotSimulator] PASS: Engine action ID flowed through PartyBotAI into Core Spell::prepare/finish; one action_id correlated the adapter attempt/result and the later successful Core cast terminal, with real healing and mana effects." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The PartyBotAI-to-Core-Spell action correlation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F20 DecisionSummary aggregated the complete executed Core Player heal report and emitted the bounded encounter summary." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F20 DecisionSummary Core fixture PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F24 candidate detail cap reported five complete candidates, retained the selected Core heal first after execution, emitted three rows and counted two omitted alternatives." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F24 bounded candidate detail Core fixture PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
candidate_detail_rows="$(grep -Ec '\[PartyBotEngineTrace\].*event=candidate .*group=3 map=33 instance=101' "$LOG_FILE" || true)"
if [[ "$candidate_detail_rows" != "3" ]]; then
    echo "[PartyBotSimulator] Expected exactly three F24 Core Player candidate detail rows, found $candidate_detail_rows." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq '\[PartyBotEngineTrace\].*event=candidate .*group=3 map=33 instance=101 .*action=core_player_heal .*status=executed' "$LOG_FILE"; then
    echo "[PartyBotSimulator] The selected Core heal was not retained in the candidate detail sample after execution." | tee -a "$LOG_FILE"
    exit 1
fi
decision_summary_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=decision_summary .*candidates=5 selected=1 executed=1 execution_failed=0 channel_losers=0 missing_action=4 impossible=0 useless=0 multiplier_veto=0 invalid_priority=0 threat_unknown=0 threat_budget_veto=0 pending_queued=0 candidate_detail_limit=3 candidate_detail_omitted=2([^[:alnum:]_]|$)' "$LOG_FILE" || true)"
if [[ "$decision_summary_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one complete F20 decision_summary for the executed Core Player heal, found $decision_summary_count." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F29 real PartyBotAI::UpdateAI ran one empty exclusive NewEngine decision cycle and returned before Legacy policy entry/dispatch, with no Shadow probe strategies registered." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F29 NewEngine authority/no-Legacy-fallback PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F21 copied real Core shapeshift form, pending spell ID/state/target and member position, plus hostile creature entry/rank/world-boss identity into the bounded party heartbeat." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F21 Core snapshot/heartbeat PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f21_heartbeat_count="$(grep -E '\[PartyBotEngineTrace\].*event=party_snapshot .*facts=.*form=1,' "$LOG_FILE" | grep -Ec 'cast=2050:[0-9]+:[0-9]+:[0-9]+,pos=-?[0-9]+/-?[0-9]+/-?[0-9]+.*entry=[0-9]+,rank=[0-9]+,boss=[01]' || true)"
if [[ "$f21_heartbeat_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one F21 heartbeat containing the real form/cast/position and hostile identity fields, found $f21_heartbeat_count." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F22 recorded the real Core Unit::DealDamage health delta through PlayerAI/PartyBotAI with attacker, spell and damage-type attribution." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F22 damage_taken PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f22_damage_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=damage_taken .*attacker=[1-9][0-9]* damage=[1-9][0-9]* health_before=[1-9][0-9]* health_after=0 spell=0 damage_type=[0-9]+' "$LOG_FILE" || true)"
if [[ "$f22_damage_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one real lethal Player damage_taken event, found $f22_damage_count." | tee -a "$LOG_FILE"
    exit 1
fi
engine_action_attempt_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=engine_action_attempt .*action=core_player_heal([[:space:]]|$)' "$LOG_FILE" || true)"
engine_action_result_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=engine_action_result .*action=core_player_heal .*attempted=1 result=success reason=adapter_returned_success([^[:alnum:]_]|$)' "$LOG_FILE" || true)"
if [[ "$engine_action_attempt_count" != "2" || "$engine_action_result_count" != "2" ]]; then
    echo "[PartyBotSimulator] Expected exactly two Core Player heal adapter attempt/result pairs (F34 + F35), found attempts=$engine_action_attempt_count results=$engine_action_result_count." | tee -a "$LOG_FILE"
    exit 1
fi
engine_action_attempt_line="$(grep -E '\[PartyBotEngineTrace\].*event=engine_action_attempt .*action=core_player_heal([[:space:]]|$)' "$LOG_FILE" | head -n 1)"
engine_action_result_line="$(grep -E '\[PartyBotEngineTrace\].*event=engine_action_result .*action=core_player_heal .*attempted=1 result=success reason=adapter_returned_success([^[:alnum:]_]|$)' "$LOG_FILE" | head -n 1)"
engine_action_attempt_id="$(printf '%s\n' "$engine_action_attempt_line" | grep -Eo 'action_id=[0-9]+' | cut -d= -f2)"
engine_action_result_id="$(printf '%s\n' "$engine_action_result_line" | grep -Eo 'action_id=[0-9]+' | cut -d= -f2)"
engine_action_attempt_encounter="$(printf '%s\n' "$engine_action_attempt_line" | grep -Eo 'encounter_id=[0-9]+' | cut -d= -f2)"
engine_action_result_encounter="$(printf '%s\n' "$engine_action_result_line" | grep -Eo 'encounter_id=[0-9]+' | cut -d= -f2)"
engine_action_attempt_seq="$(printf '%s\n' "$engine_action_attempt_line" | grep -Eo 'seq=[0-9]+' | cut -d= -f2)"
engine_action_result_seq="$(printf '%s\n' "$engine_action_result_line" | grep -Eo 'seq=[0-9]+' | cut -d= -f2)"
if [[ -z "$engine_action_attempt_id" || "$engine_action_attempt_id" != "$engine_action_result_id" ||
      -z "$engine_action_attempt_encounter" ||
      "$engine_action_attempt_encounter" != "$engine_action_result_encounter" ||
      -z "$engine_action_attempt_seq" || -z "$engine_action_result_seq" ||
      "$engine_action_result_seq" -le "$engine_action_attempt_seq" ]]; then
    echo "[PartyBotSimulator] Core Player heal action trace did not preserve one action_id/encounter_id and ordered attempt/result sequences." | tee -a "$LOG_FILE"
    exit 1
fi
engine_cast_terminal_count="$(grep -Ec "\\[PartyBotEngineTrace\\].*encounter_id=$engine_action_attempt_encounter .*event=cast_terminal .*action_id=$engine_action_attempt_id .*spell=2050 .*success=1 interrupted=0 outcome=completed([^[:alnum:]_]|$)" "$LOG_FILE" || true)"
if [[ "$engine_cast_terminal_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one successful Core cast_terminal for Engine action_id=$engine_action_attempt_id, found $engine_cast_terminal_count." | tee -a "$LOG_FILE"
    exit 1
fi
engine_cast_terminal_line="$(grep -E "\\[PartyBotEngineTrace\\].*encounter_id=$engine_action_attempt_encounter .*event=cast_terminal .*action_id=$engine_action_attempt_id .*spell=2050 .*success=1 interrupted=0 outcome=completed([^[:alnum:]_]|$)" "$LOG_FILE" | tail -n 1)"
engine_cast_terminal_seq="$(printf '%s\n' "$engine_cast_terminal_line" | grep -Eo 'seq=[0-9]+' | cut -d= -f2)"
if [[ -z "$engine_cast_terminal_seq" || "$engine_cast_terminal_seq" -le "$engine_action_result_seq" ]]; then
    echo "[PartyBotSimulator] Core cast_terminal must follow the Engine action adapter result in encounter sequence order." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F32 Core Spell::finish delivered a value-only terminal event to the registered Engine listener with the same action/encounter/spell/target correlation as the adapter attempt; Core success was distinct from SPELL_CAST_OK acceptance." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F32 Core Spell terminal-listener PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F34 released the registered continuation only after the matching successful Core terminal and executed it through the next Engine pending dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F34 Core-terminal continuation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F35 gated one follow-up on two distinct successful Core Spell::finish terminals and dispatched it through the normal Engine pending queue." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F35 multi-prerequisite Core-terminal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F35 resolved a real Core terminal alternative group, rejected the higher-priority wrong-status branch, and dispatched the valid branch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F35 Core-terminal alternative-group PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F36 required both the synchronous successful ActionResult and the later successful Core Spell::finish for the same action ID before dispatching its child." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F36 mixed ActionResult/Core terminal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F39 emitted a correlated Reached terminal only after natural PointMovementGenerator completion and destination tolerance check; the movement prerequisite released its continuation through the next pending dispatch, while ClearType produced Interrupted." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F39 typed movement-terminal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F42 real Core aggregated single-slot use/rejection and a synthetic two-ON_USE-slot prototype. Child snapshots prove first/non-triggered and second/triggered Core Spells; the first child independently consumed both finite charges." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F42 Core item-use aggregate PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: generic Core target maintenance cleared an idle stale selection, selected a live hostile and rejected a stale friendly target without disturbing Player combat state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F43 generic target-maintenance accepted/rejected/cleanup marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F43 StopUnattackableTarget used live Core preflight to reject a stale negative snapshot, then AttackStop detached only the invalid victim while preserving combat state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F43 unattackable-victim AttackStop PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F41 CoreAction required a fresh map-thread actor/target resolution and rejected missing targets plus stale map/instance snapshots before the real movement adapter ran." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F41 Core action preflight PASS marker was not found." | tee -a "$LOG_FILE"
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

if ! grep -Fq "[PartyBotSimulator] PASS: Core Player::ResurrectPlayer emitted exactly one dead-to-alive observer transition after tank death recovery snapshots were captured." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player resurrection observer lifecycle PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: Core Player::SetDeathState invoked the observer for JUST_DIED and CORPSE, while the alive-state transition filter emitted exactly one alive-to-dead event." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Player alive-state observer lifecycle PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: PartyBotAI emitted encounter-correlated death and resurrection trace records through Core PlayerAI callbacks, while the repeated CORPSE state was suppressed." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The PartyBotAI encounter lifecycle trace PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq "event=death .*alive_before=1 alive_after=0" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The formatted PartyBotAI death trace record was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq "event=resurrect .*alive_before=0 alive_after=1" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The formatted PartyBotAI resurrection trace record was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: real Core DealDamage and Player::ResurrectPlayer produced one sampled health, death and resurrection trace for a non-PlayerBots Group member;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing non-PlayerBots party-member state trace PASS marker." | tee -a "$LOG_FILE"
    exit 1
fi

human_member_guid="$(grep -F "PASS: real Core DealDamage and Player::ResurrectPlayer produced one sampled health, death and resurrection trace for a non-PlayerBots Group member;" "$LOG_FILE" | grep -Eo 'member_guid=[0-9]+' | tail -n 1 | cut -d= -f2)"
if [[ -z "$human_member_guid" ]]; then
    echo "[PartyBotSimulator] Could not extract the non-PlayerBots member GUID from the F14 PASS marker." | tee -a "$LOG_FILE"
    exit 1
fi
for event in party_member_health party_member_death party_member_resurrect; do
    event_count="$(grep -Ec "\[PartyBotEngineTrace\].*event=$event .*member_guid=$human_member_guid([[:space:]]|$)" "$LOG_FILE" || true)"
    if [[ "$event_count" != "1" ]]; then
        echo "[PartyBotSimulator] Expected exactly one $event record for member $human_member_guid, found $event_count." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: server-shutdown hook closed an active shared encounter and consumed exactly one reserved terminal admission across two PartyBotAI instances." "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing server-shutdown encounter-flush PASS marker." | tee -a "$LOG_FILE"
    exit 1
fi
shutdown_end_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=encounter_end .*reason=server_shutdown([^[:alnum:]_]|$)' "$LOG_FILE" || true)"
if [[ "$shutdown_end_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one server_shutdown encounter_end record, found $shutdown_end_count." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: source Map::Remove retained an encounter while another PartyBotAI remained, then the last bot closed the old map shard exactly once before both Players were re-added." "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing source-map last-bot departure PASS marker." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a real five-Player Group left the source Map, closed its last-PartyBot encounter exactly once, reacquired a distinct destination-map coordinator through PartyBotAI::UpdatePartyBotEngineShadow, started a new encounter from the destination roster, then closed it, removed its temporary GroupBind and restored every Player to the source instance." "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing Core-backed source-to-destination dungeon-map transfer PASS marker." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: Core-backed PartyBotEngine prioritised trigger telemetry by actionable active state, then active-empty, then inactive; cadence-skipped rows were deprioritised by the four-row cap, while healer-candidate attribution remained intact." "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing Core-backed prioritised trigger telemetry PASS marker." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq '\[PartyBotEngineTrace\].*event=trigger_summary .*total=7 evaluated=4 active=2 inactive=2 cadence_skipped=3 omitted=0 detail_limit=4 detail_omitted=3' "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing formatted trigger summary with expected priority-sample omission counts." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq '\[PartyBotEngineTrace\].*event=trigger .*strategy=healing_triage trigger_name=healing_triage interval_ms=0 status=active candidates=1' "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing formatted active healer trigger row." | tee -a "$LOG_FILE"
    exit 1
fi
trigger_summary_line="$(grep -nE '\[PartyBotEngineTrace\].*event=trigger_summary .*total=7 evaluated=4 active=2 inactive=2 cadence_skipped=3 omitted=0 detail_limit=4 detail_omitted=3' "$LOG_FILE" | tail -n1 | cut -d: -f1 || true)"
trigger_encounter_id=""
if [[ -n "$trigger_summary_line" ]]; then
    trigger_encounter_id="$(sed -n "${trigger_summary_line}p" "$LOG_FILE" | sed -E 's/.*encounter_id=([0-9]+).*/\1/')"
fi
active_empty_line="$(grep -nE "\\[PartyBotEngineTrace\\].*encounter_id=${trigger_encounter_id} .*event=trigger .*strategy=simulator_trace_active_empty trigger_name=simulator_trace_active_empty .*status=active candidates=0" "$LOG_FILE" | head -n1 | cut -d: -f1 || true)"
tank_line="$(grep -nE "\\[PartyBotEngineTrace\\].*encounter_id=${trigger_encounter_id} .*event=trigger .*strategy=tank_target trigger_name=tank_target .*status=inactive" "$LOG_FILE" | head -n1 | cut -d: -f1 || true)"
dps_line="$(grep -nE "\\[PartyBotEngineTrace\\].*encounter_id=${trigger_encounter_id} .*event=trigger .*strategy=dps_target trigger_name=dps_target .*status=inactive" "$LOG_FILE" | head -n1 | cut -d: -f1 || true)"
if [[ -z "$trigger_encounter_id" || -z "$active_empty_line" || -z "$tank_line" || -z "$dps_line" ]] || (( active_empty_line >= tank_line || tank_line >= dps_line )); then
    echo "[PartyBotSimulator] Trigger detail rows were not emitted in the expected actionable-first order." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq '\[PartyBotEngineTrace\].*event=candidate .*strategy=healing_triage trigger_name=healing_triage action=shadow_heal_triage' "$LOG_FILE"; then
    echo "[PartyBotSimulator] Missing candidate-to-trigger correlation record." | tee -a "$LOG_FILE"
    exit 1
fi
map_leave_end_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=encounter_end .*reason=last_partybot_left_map([^[:alnum:]_]|$)' "$LOG_FILE" || true)"
if [[ "$map_leave_end_count" != "3" ]]; then
    echo "[PartyBotSimulator] Expected exactly three last_partybot_left_map encounter_end records (F16 source close plus F27 source/destination closes), found $map_leave_end_count." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: two real PartyBotAI instances shared one coordinator detail budget, admitted 1,024 target-change events, emitted one trace_limit on overflow and suppressed subsequent detail events." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The PartyBotAI shared encounter trace budget PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

global_detail_limit_count=$(grep -Ec '\[PartyBotEngineTrace\].*event=trace_limit .*reason=detail_budget_exhausted' "$LOG_FILE" || true)
if [[ "$global_detail_limit_count" -ne 1 ]]; then
    echo "[PartyBotSimulator] Expected exactly one encounter-wide detail trace_limit marker; found $global_detail_limit_count." | tee -a "$LOG_FILE"
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

echo "[PartyBotSimulator] Runtime PASS: Core map, Creature updates, Core-applied direct/periodic-damage, periodic-healing-assist, periodic-power-assist, periodic-mana-leech and composite periodic-life-leech threat-impact parity, threat switching, Engine attack, triggered spell healing, Player non-triggered spell completion, single-/multi-slot Core item-use aggregation, per-child Spell correlation and independent charge mutations, CoreAction map-thread actor/target/map-instance preflight, Core point movement, native FollowMovementGenerator tracking, Player follow pause/resume around a non-triggered cast, follow retarget/recovery after leader handoff and target removal, Player heal range/resource/LOS rejection, movement refusal under CAN_NOT_MOVE/POSSESSED, combined real five-Player Core Group roster/cache snapshot, multi-add healer-threat and Core threat projection, Tank/Healer death-reference cleanup, PartyBotAI encounter-correlated death/resurrection trace records, party-wide trace-budget overflow/one-shot marker, sampled non-PlayerBots health/death/resurrection events, server-shutdown encounter flush, source-map final-bot departure closure, source-to-destination dungeon-map transfer and destination shard recreation, sampled trigger activation/cadence and originating-trigger candidate correlation, Engine action ID propagation through PartyBotAI and Core Spell prepare/finish terminal, multi-add retarget recovery, non-victim/offline HostileRefManager threat discovery, movement-driven Player cast interruption and actor cleanup." | tee -a "$LOG_FILE"
