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
# Run under GDB so a post-scenario shutdown crash produces a useful native
# backtrace in the uploaded artifact. --return-child-result preserves the
# inferior's exit status; a signal is still a hard CI failure.
timeout --signal=TERM --kill-after=15s 10m gdb --batch --return-child-result \
    -ex "set pagination off" \
    -ex "set print thread-events off" \
    -ex "handle SIGPIPE nostop noprint pass" \
    -ex run \
    -ex "thread apply all bt full" \
    --args "$MANGOSD_BIN" -c "$CONFIG_FILE" 2>&1 | tee -a "$LOG_FILE"
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
f24_summary_line="$(grep -E '\[PartyBotEngineTrace\].*event=decision_summary .*group=3 map=33 instance=101 .*candidates=5 selected=1 executed=1 execution_failed=0 channel_losers=0 missing_action=4 impossible=0 useless=0 multiplier_veto=0 invalid_priority=0 threat_unknown=0 threat_budget_veto=0 pending_queued=0 candidate_detail_limit=3 candidate_detail_omitted=2([^[:alnum:]_]|$)' "$LOG_FILE" | head -n 1)"
f24_encounter_id="$(printf '%s\n' "$f24_summary_line" | grep -Eo 'encounter_id=[0-9]+' | cut -d= -f2)"
f24_bot_guid="$(printf '%s\n' "$f24_summary_line" | grep -Eo 'bot=[0-9]+' | cut -d= -f2)"
if [[ -z "$f24_encounter_id" || -z "$f24_bot_guid" ]]; then
    echo "[PartyBotSimulator] Could not identify the unique F24 Core Player encounter/bot from its decision summary." | tee -a "$LOG_FILE"
    exit 1
fi

# Other simulator scenarios may legitimately reuse group/map/instance IDs. Scope
# F24 candidate rows to the exact encounter and Player that emitted its summary.
candidate_detail_rows="$(grep -Ec "\\[PartyBotEngineTrace\\].*encounter_id=$f24_encounter_id .*event=candidate .*group=3 map=33 instance=101 bot=$f24_bot_guid([[:space:]]|$)" "$LOG_FILE" || true)"
if [[ "$candidate_detail_rows" != "3" ]]; then
    echo "[PartyBotSimulator] Expected exactly three F24 Core Player candidate detail rows for encounter_id=$f24_encounter_id bot=$f24_bot_guid, found $candidate_detail_rows." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Eq "\\[PartyBotEngineTrace\\].*encounter_id=$f24_encounter_id .*event=candidate .*group=3 map=33 instance=101 bot=$f24_bot_guid .*action=core_player_heal .*status=executed" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The selected Core heal was not retained in the F24 candidate detail sample after execution." | tee -a "$LOG_FILE"
    exit 1
fi
decision_summary_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=decision_summary .*candidates=5 selected=1 executed=1 execution_failed=0 channel_losers=0 missing_action=4 impossible=0 useless=0 multiplier_veto=0 invalid_priority=0 threat_unknown=0 threat_budget_veto=0 pending_queued=0 candidate_detail_limit=3 candidate_detail_omitted=2([^[:alnum:]_]|$)' "$LOG_FILE" || true)"
if [[ "$decision_summary_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one complete F20 decision_summary for the executed Core Player heal, found $decision_summary_count." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F29 real PartyBotAI::UpdateAI registered the production out-of-combat follow policy but emitted no candidate while the bot was in combat, with no Legacy policy entry or Shadow-policy registration." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F29 NewEngine combat-veto/no-Legacy-fallback PASS marker was not found." | tee -a "$LOG_FILE"
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
f22_damage_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=damage_taken .*attacker=[1-9][0-9]* damage=[1-9][0-9]* health_before=[1-9][0-9]* health_after=0 spell=0 damage_type=[0-9]+' "$LOG_FILE" || true)"
if [[ "$f22_damage_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one real lethal Player damage_taken event, found $f22_damage_count." | tee -a "$LOG_FILE"
    exit 1
fi
echo "[PartyBotSimulator] PASS: F22 recorded the real Core Unit::DealDamage health delta through PlayerAI/PartyBotAI with attacker, spell and damage-type attribution." | tee -a "$LOG_FILE"
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

if ! grep -Fq "[PartyBotSimulator] PASS: F42 real Core aggregated single-slot use/rejection, a synthetic two-ON_USE-slot prototype, and a real delayed Brimstone/Fireball ON_USE cancellation before and after its Core charge mutation." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F42 real delayed ON_USE cancellation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F46 bounded self-utility spell audit scanned the loaded Vanilla 5875 SpellEntry table; only self-aura spells with both caster/target proc suppression and no ScriptMgr, spell-threat, spell-proc, trigger-child, periodic or area path were listed as eligible candidates." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F46 bounded self-utility spell audit PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F47 audited loaded Vanilla item prototypes and every ON_USE spell slot; only items whose complete ON_USE fan-out consists of positive bounded self-aura spells outside Core's item-proc exception families were listed as candidates." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F47 item-backed self-utility audit PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F48 used the real Vanilla Elixir of Minor Agility ON_USE item through Player::CastItemUseSpell; Core applied the self-targeted Lesser Agility aura/stat change, aggregated its successful child and CountChanged stack mutation, then rejected an immediate repeat on the item's cooldown without consuming another item or applying a second effect." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F48 real item-backed self-utility Core execution PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F49 selected an item-backed self-utility candidate through PartyBotEngine, revalidated exact inventory identity before Core item use, correlated the Engine action ID with ItemUseTerminal, applied the real Core aura/stat and stack mutation, rejected a stale removed-item race without side effects, and suppressed a cooldown-blocked repeat before dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F49 Engine-selected item-backed utility PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi


if ! grep -Fq "[PartyBotSimulator] PASS: real Core stackable item use reduced a two-potion stack to one with a CountChanged before-effects record, and using the cast item as its own reagent consumed three items once with a GUID-less ReagentConsumed before-effects record." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F45 real-Core stack-count/item-as-reagent mutation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: CancelOwnCastAction cancelled exact Engine-owned PREPARING, DELAYED Fireball and CHANNELED Mind Flay Player spells through Core SpellCaster::InterruptSpell, rejected stale/replaced and unknown-threat dispatch, emitted matching Interrupted terminals, suppressed delayed impacts and removed the channel aura." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The CancelOwnCastAction identity-safe Core interruption PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: generic Core target maintenance cleared an idle stale selection, selected a live hostile and rejected a stale friendly target without disturbing Player combat state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F43 generic target-maintenance accepted/rejected/cleanup marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: MoveDistanceAction accepted Core MoveDistance over live Follow and Chase, preserved and restored the underlying targeted movement on Distancing expiry, preserved combat/threat state, and rejected unrelated Point and stale-victim requests." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The MoveDistanceAction Core Distancing lifecycle PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: StopChaseAction expired only a current-victim Core Chase, allowed Core to remove its underlying Follow, preserved victim/selection/combat/melee/spell/health/power/threat state, and rejected stale or unrelated movement before side effects." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The StopChaseAction current-victim cancellation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F43 StopUnattackableTarget used live Core preflight to reject a stale negative snapshot, then AttackStop detached only the invalid victim while preserving combat state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F43 unattackable-victim AttackStop PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: real five-Player Group PartySnapshotBuilder drove DPS Main Tank assist, healer peel priority and CC veto; Core selection and attack invalidated the shared cache, and a Main Tank target handoff refreshed the five-member snapshot and retargeted the DPS bot from its old victim to the new target with complete Group/session cleanup." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F44 five-Player Group DPS assist/target-handoff/cache-invalidation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f53a_priest_state_line="$(grep -F '[PartyBotSimulator] F53a Priest healer spell snapshot:' "$LOG_FILE" | tail -n 1)"
for expected in rankOne=1 rankTwo=1 spellbookCaptured=1 sortedUnique=1 coreManaCosts=1 selector=1; do
    if [[ "$f53a_priest_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F53a Priest spell snapshot assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f53b_priest_state_line="$(grep -F '[PartyBotSimulator] F53b Priest direct heal Core action:' "$LOG_FILE" | tail -n 1)"
for expected in snapshot=1 encounterStarted=1 possible=1 useful=1 engineThreatVeto=1 livePreflight=1 castAccepted=1 castCompleted=1 manaSpent=1 terminalMatched=1; do
    if [[ "$f53b_priest_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F53b Priest direct-heal runtime assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: real Core Priest Inspiration aura on the healer vetoes the direct-heal threat estimate with no impacts; after aura removal the component-only estimate is restored." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F53c direct-heal proc-closure PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: fixture-only PriestDirectHealAction passed same-Group live Core preflight, rejected the AoE Prayer of Healing spell, cast a single-target direct heal, spent Core mana and matched the exact Engine action ID to its successful Core Spell terminal; the threat budget remained Unknown." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F53b Priest direct-heal Core action PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54d_timeline_state_line="$(grep -F '[PartyBotSimulator] F54d Core healing timeline capture:' "$LOG_FILE" | tail -n 1)"
for expected in directTimerCaptured=1 snapshotDirectEventCaptured=1 hotTimerCaptured=1 snapshotHotEventsCaptured=1; do
    if [[ "$f54d_timeline_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54d Core timeline assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54d copied the live Core direct-heal remaining cast timer and the real Renew aura first-tick timer, period and remaining tick count into bounded value-only snapshot fields." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54d Core healing timeline PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54d_effective_state_line="$(grep -F '[PartyBotSimulator] F54d Core effective healing:' "$LOG_FILE" | tail -n 1)"
for expected in directActualCaptured=1 directOneHealthCap=1 hotTickCaptured=1 concurrentHealingCaptured=1 concurrentAttributionUnknown=1; do
    if [[ "$f54d_effective_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54d actual effective-healing assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54d measured a real Core direct-cast health gain, exact DealHeal overheal at a one-health deficit, an actual Renew tick and its conservative overheal lower bound, and aggregate same-time concurrent-heal effective gain/overheal without per-caster attribution." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54d Core effective-healing PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54d_effective_state_line="$(grep -F '[PartyBotSimulator] F54d Core effective healing:' "$LOG_FILE" | tail -n 1)"
for expected in directActualCaptured=1 directOneHealthCap=1 hotTickCaptured=1 concurrentHealingCaptured=1 concurrentAttributionUnknown=1; do
    if [[ "$f54d_effective_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54d actual effective-healing assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54d measured a real Core direct-cast health gain, exact DealHeal overheal at a one-health deficit, an actual Renew tick and its conservative overheal lower bound, and aggregate same-time concurrent-heal effective gain/overheal without per-caster attribution." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54d Core effective-healing PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54d_damage_interleaving_line="$(grep -F '[PartyBotSimulator] F54d Core damage interleaving:' "$LOG_FILE" | tail -n 1)"
for expected in captured=1 firstHeal=40 coreDamage=30 secondHeal=40 projectedEffective=80 projectedDamage=30 projectedOverheal=0; do
    if [[ "$f54d_damage_interleaving_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54d damage-interleaving assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54d matched two real Core DealHeal applications with a real Core SELF_DAMAGE DealDamage event between them; explicit distinct-time arrivals reproduced the observed health." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54d Core damage-interleaving PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54d_hostile_interleaving_line="$(grep -F '[PartyBotSimulator] F54d hostile damage interleaving:' "$LOG_FILE" | tail -n 1)"
for expected in captured=1 firstCast=1 hostileDamage=30 secondCast=1 healRefs=2 flatThreatZero=1 healthMatches=1 threatMatches=1 projectedDamage=30 projectedOverheal=0; do
    if [[ "$f54d_hostile_interleaving_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54d hostile damage interleaving assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54d matched two non-triggered Core Lesser Heal casts with a real hostile Creature DealDamage event between them; observed health and both hostile assist-threat deltas matched the projection." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54d hostile damage/Heal interleaving PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54i_line="$(grep -F '[PartyBotSimulator] F54i same-timestamp Core Heal/Damage ordering:' "$LOG_FILE" | tail -n 1)"
for expected in "captured=1" "coreOrder=1" "sameTimestamp=1" "projectionRejected=1" "firstHeal=40" "damage=30" "secondHeal=40"; do
    if [[ "$f54i_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54i same-timestamp Core Heal/Damage assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54i observed real Core DealHeal -> SELF_DAMAGE DealDamage -> DealHeal mutations within one WorldTimer millisecond and rejected the indistinguishable mixed Heal/Damage timeline instead of inventing an order." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54i same-timestamp fail-closed PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54j_line="$(grep -F '[PartyBotSimulator] F54j snapshot-after-pushback/interruption:' "$LOG_FILE" | tail -n 1)"
for expected in "captured=1" "spellEligible=1" "castAccepted=1" "preSnapshot=1" "casterDamage=1" "timerIncreased=1" "postPushbackSnapshot=1" "interrupted=1" "postInterruptSnapshot=1" "noStaleHealCredit=1" "targetUnchanged=1" "cleanup=1"; do
    if [[ "$f54j_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54j pushback/interruption assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54j observed Core DAMAGE_PUSHBACK extend an in-flight Priest Lesser Heal, refreshed the value-only snapshot to the live timer, then interrupted the spell and confirmed the fresh snapshot emitted no stale heal credit." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54j snapshot-after-pushback/interruption PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54k_line="$(grep -F '[PartyBotSimulator] F54k random HoT tick bounds:' "$LOG_FILE" | tail -n 1)"
for expected in "auraApplied=1" "fractional=1" "snapshotCaptured=1" "timelineCaptured=1" "projectedBounds=1" "tickObserved=1" "actualInBounds=1" "cleanup=1"; do
    if [[ "$f54k_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54k random HoT assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54k represented Core rand_ditheru periodic healing as per-tick lower/upper bounds and observed a real fractional Renew tick inside that interval." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54k random HoT interval PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54l_line="$(grep -F '[PartyBotSimulator] F54l school-aware absorbs:' "$LOG_FILE" | tail -n 1)"
for expected in "shieldApplied=1" "castAccepted=1" "weakenedSoulClear=1" "snapshotCaptured=1" "allSchoolExcluded=1" "frostNoMatch=1" "fireMatched=1" "depletion=1" "timelineMatches=1" "healthMatches=1" "cleanup=1"; do
    if [[ "$f54l_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54l school-absorb assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54l captured a real Fire-only School Absorb Aura, ignored it for Frost damage, consumed it for Fire damage and matched Core health/absorb results." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54l school-aware absorb PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

# Core log lines carry ANSI colors; strip them before parsing numeric fields.
f54m_line="$(grep -F '[PartyBotSimulator] F54m bounded child-chain depth/loop guard:' "$LOG_FILE" | tail -n 1 | sed -E 's/\x1B\[[0-9;]*[[:alpha:]]//g')"
for expected in "rowReady=1" "castLimit=4" "configApplied=1" "baselineAuraAttached=1" "baselineCast=1" "baselineTargets=5" "baselineAuraRemoved=1" "auraAttached=1" "rootCast=1" "fourthTargets=5" "threatMatched=1" "auraRemoved=1" "auraRowsRestored=1" "spellRowsRestored=1" "configRestored=1" "healthRestored=1" "guardExpected=5"; do
    if [[ "$f54m_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54m child-chain guard assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f54m_fourth_gains="$(sed -n 's/.* fourthGain=\([0-9][0-9/]*\)\.*/\1/p' <<< "$f54m_line")"
IFS='/' read -r -a f54m_gain_values <<< "$f54m_fourth_gains"
if [[ ${#f54m_gain_values[@]} -ne 5 ]]; then
    echo "[PartyBotSimulator] F54m expected five explicit fourth-stage healing deltas, found '$f54m_fourth_gains'." | tee -a "$LOG_FILE"
    exit 1
fi
for gain in "${f54m_gain_values[@]}"; do
    if ! [[ "$gain" =~ ^[0-9]+$ ]] || (( gain <= 10 )); then
        echo "[PartyBotSimulator] F54m expected each fourth-stage Player heal delta to exceed 10, found '$gain'." | tee -a "$LOG_FILE"
        exit 1
    fi
done

# Independently recheck the logged baseline and incremental Core threat values,
# rather than trusting only the fixture's C++ threatMatched boolean.
f54m_baseline_threats="$(grep -oE 'baselineThreat=[0-9]+([.][0-9]+)?/[0-9]+([.][0-9]+)?' <<< "$f54m_line" | cut -d= -f2)"
f54m_expected_fourth="$(grep -oE 'expectedFourthThreat=[0-9]+([.][0-9]+)?' <<< "$f54m_line" | cut -d= -f2)"
f54m_observed_fourth="$(grep -oE 'observedFourthThreat=[0-9]+([.][0-9]+)?/[0-9]+([.][0-9]+)?' <<< "$f54m_line" | cut -d= -f2)"
IFS='/' read -r f54m_baseline0 f54m_baseline1 <<< "$f54m_baseline_threats"
IFS='/' read -r f54m_observed0 f54m_observed1 <<< "$f54m_observed_fourth"
if [[ -z "$f54m_baseline0" || -z "$f54m_baseline1" ||
      -z "$f54m_expected_fourth" || -z "$f54m_observed0" || -z "$f54m_observed1" ]]; then
    echo "[PartyBotSimulator] F54m numeric threat diagnostics are missing or malformed." | tee -a "$LOG_FILE"
    exit 1
fi
if ! awk -v b0="$f54m_baseline0" -v b1="$f54m_baseline1" \
        -v expected="$f54m_expected_fourth" \
        -v o0="$f54m_observed0" -v o1="$f54m_observed1" \
        'BEGIN {
            if (b0 < 0 || b1 < 0 || expected <= 0 || o0 < 0 || o1 < 0) exit 1;
            if ((o0 - expected > 0.5) || (expected - o0 > 0.5) ||
                (o1 - expected > 0.5) || (expected - o1 > 0.5)) exit 1;
        }'; then
    echo "[PartyBotSimulator] F54m baseline/expected/observed threat values failed numeric parity." | tee -a "$LOG_FILE"
    exit 1
fi

f54n_line="$(grep -F '[PartyBotSimulator] F54n deferred instant-heal recipient race:' "$LOG_FILE" | tail -n 1 | sed -E 's/\x1B\[[0-9;]*[[:alpha:]]//g')"
for expected in "rowReady=1" "hostilesCreated=1" "hostilesCleaned=1" "aiRestored=1" "livePreflight=1" "estimateUnknown=1" "snapshotMatches=1" "impacts=0" "componentImpacts=2" "targetRefs=2" "uniqueHostiles=2" "procVeto=1" "procAuraRemoved=1" "procFlagsRestored=1" "castTimeUnknown=1" "aoeRejected=1" "castStarted=1" "eventDeferred=1" "lateRecipientAdded=1" "lateRecipientUnpredicted=1" "lateRecipientThreatApplied=1" "healApplied=1" "threatBoundsMatched=1" "stateRestored=1" "spellListRestored=1"; do
    if [[ "$f54n_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54n deferred-recipient fail-closed assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f54n_event_updates="$(grep -oE 'eventUpdates=[0-9]+' <<< "$f54n_line" | cut -d= -f2)"
if ! [[ "$f54n_event_updates" =~ ^[0-9]+$ ]] || (( f54n_event_updates < 1 || f54n_event_updates > 20 )); then
    echo "[PartyBotSimulator] F54n expected 1..20 bounded Core Unit::Update ticks to process the instant SpellEvent, found '$f54n_event_updates'." | tee -a "$LOG_FILE"
    exit 1
fi
f54n_expected="$(grep -oE 'expectedThreat=[0-9]+([.][0-9]+)?/[0-9]+([.][0-9]+)?' <<< "$f54n_line" | cut -d= -f2)"
f54n_actual="$(grep -oE 'actualThreat=[-]?[0-9]+([.][0-9]+)?/[-]?[0-9]+([.][0-9]+)?' <<< "$f54n_line" | cut -d= -f2)"
IFS='/' read -r f54n_expected0 f54n_expected1 <<< "$f54n_expected"
IFS='/' read -r f54n_actual0 f54n_actual1 <<< "$f54n_actual"
if [[ -z "$f54n_expected0" || -z "$f54n_expected1" || -z "$f54n_actual0" || -z "$f54n_actual1" ]]; then
    echo "[PartyBotSimulator] F54n numeric threat bounds are missing or malformed." | tee -a "$LOG_FILE"
    exit 1
fi
f54n_late_recipient_threat="$(grep -oE 'lateRecipientThreat=[0-9]+([.][0-9]+)?' <<< "$f54n_line" | cut -d= -f2)"
if ! [[ "$f54n_late_recipient_threat" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "[PartyBotSimulator] F54n late-recipient threat value is missing or malformed." | tee -a "$LOG_FILE"
    exit 1
fi
if ! awk -v e0="$f54n_expected0" -v e1="$f54n_expected1" \
        -v a0="$f54n_actual0" -v a1="$f54n_actual1" \
        -v late="$f54n_late_recipient_threat" \
        'BEGIN {
            if (e0 <= 0 || e1 <= 0 || a0 < -0.5 || a1 < -0.5 || late <= 0.5) exit 1;
            if (a0 > e0 + 0.5 || a1 > e1 + 0.5) exit 1;
        }'; then
    echo "[PartyBotSimulator] F54n observed Core threat/late-recipient values failed numeric closure checks." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54n kept the deferred instant self-heal Unknown, added a third hostile reference after cast start but before its queued SpellEvent, observed Core apply healing threat to that previously unpredicted recipient, and restored Player/AI/actor state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54n deferred-recipient fail-closed PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

# F54p extends the F54o Core fixture with explicit cooldown/GCD boundary checks.
f54p_line="$(grep -F '[PartyBotSimulator] F54o Core admission accept/reject:' "$LOG_FILE" | tail -n 1 | sed -E 's/\\x1B\\[[0-9;]*[[:alpha:]]//g')"
for expected in "rejectIntent=1" "rejectAuthorize=1" "rejectTerminal=1" "rejectNoEffects=1" "rejectThreatDelta=0.000" "rejectSpellReadyBefore=1" "rejectNoGcdBefore=1" "rejectSpellReadyAfter=1" "rejectNoGcdAfter=1" "acceptIntent=1" "acceptAuthorize=1" "acceptTerminal=1" "acceptHeal=1" "acceptSpellReadyBefore=1" "acceptNoGcdBefore=1" "acceptCooldownApplicable=1" "acceptCooldownCommitted=1" "acceptGcdApplicable=1" "acceptGcdCommitted=1" "adapterRestored=1"; do
    if [[ "$f54p_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54p deferred-admission assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54o exercised real Core deferred spell admission;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54o Core admission PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
echo "[PartyBotSimulator] PASS: F54p verified rejected admission preserves cooldown/GCD before reset and authorized admission commits applicable SpellEntry cooldown/GCD." | tee -a "$LOG_FILE"

f54q_line="$(grep -F '[PartyBotSimulator] F54q queued deferred-cast interruption:' "$LOG_FILE" | tail -n 1 | sed -E 's/\\x1B\\[[0-9;]*[[:alpha:]]//g')"
for expected in "castAccepted=1" "queuedBeforeCancel=1" "interruptApi=1" "currentSpellCleared=1" "spellFinished=1" "eventRetained=1" "terminalInterrupted=1" "authorizeSkipped=1" "terminalOnce=1" "eventReaped=1" "noEffects=1" "threatDelta=0.000" "spellReadyBefore=1" "noGcdBefore=1" "spellReadyAfter=1" "noGcdAfter=1" "adapterRestored=1"; do
    if [[ "$f54q_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54q queued-cancellation assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54q canceled a queued deferred Core Spell before pre-impact authorization;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54q queued-cancellation PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54r_line="$(grep -F '[PartyBotSimulator] F54r deferred-recipient drift:' "$LOG_FILE" | tail -n 1 | sed -E 's/\\x1B\\[[0-9;]*[[:alpha:]]//g')"
for expected in "castAccepted=1" "queuedBeforeMutation=1" "selectorChanged=1" "inputTargetChanged=1" "terminalRejected=1" "authorizeSkipped=1" "terminalOnce=1" "currentSpellCleared=1" "eventReaped=1" "noEffects=1" "threatDelta=0.000" "spellReadyBefore=1" "noGcdBefore=1" "spellReadyAfter=1" "noGcdAfter=1" "selectorRestored=1" "adapterRestored=1"; do
    if [[ "$f54r_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54r deferred-recipient assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f54r_expected_guid="$(printf '%s\n' "$f54r_line" | sed -nE 's/.*expectedTargetGuid=([0-9]+).*/\1/p')"
if [[ -z "$f54r_expected_guid" ]]; then
    echo "[PartyBotSimulator] F54r expected recipient GUID was not reported." | tee -a "$LOG_FILE"
    exit 1
fi
f54r_resolved_reject="$(grep -F '[PartyBotSpellAdmission] resolved self-heal shape rejected: spell=19236' "$LOG_FILE" | grep -F "targetUnit=$f54r_expected_guid" | grep -F "targetGuid=$f54r_expected_guid" | grep -F 'targetCount=1' | tail -n 1 || true)"
if [[ -z "$f54r_resolved_reject" ]]; then
    echo "[PartyBotSimulator] F54r Core did not log the expected single resolved Group recipient before rejecting admission." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54r rejected a queued self-heal after Core re-resolved its recipient to a different live Group member;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54r recipient-drift PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54s_line="$(grep -F '[PartyBotSimulator] F54s removed Group recipient:' "$LOG_FILE" | tail -n 1 | sed -E 's/\\x1B\\[[0-9;]*[[:alpha:]]//g')"
for expected in "castAccepted=1" "queuedBeforeRemoval=1" "selectorChanged=1" "inputTargetChanged=1" "memberRemoved=1" "recipientStillLive=1" "coreRejected=1" "authorizeSkipped=1" "terminalOnce=1" "currentSpellCleared=1" "eventReaped=1" "noEffects=1" "threatDelta=0.000" "spellReadyBefore=1" "noGcdBefore=1" "spellReadyAfter=1" "noGcdAfter=1" "selectorRestored=1" "memberRestored=1" "subgroupRestored=1" "instanceValidRestored=1" "locationRestored=1" "leaderRestored=1" "assistantRestored=1" "adapterRestored=1"; do
    if [[ "$f54s_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54s removed-Group-recipient assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f54s_expected_guid="$(printf '%s\n' "$f54s_line" | sed -nE 's/.*expectedTargetGuid=([0-9]+).*/\1/p')"
if [[ -z "$f54s_expected_guid" ]]; then
    echo "[PartyBotSimulator] F54s expected departed recipient GUID was not reported." | tee -a "$LOG_FILE"
    exit 1
fi
f54s_resolved_reject="$(grep -F '[PartyBotSpellAdmission] resolved self-heal shape rejected: spell=19236' "$LOG_FILE" | grep -F "targetUnit=$f54s_expected_guid" | grep -F 'targetCount=0' | grep -F 'targetGuid=0' | tail -n 1 || true)"
if [[ -z "$f54s_resolved_reject" ]]; then
    echo "[PartyBotSimulator] F54s Core log did not prove the departed Group recipient resolved to targetCount=0 / targetGuid=0." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54s removed the queued non-leader recipient through Group::RemoveMember(GROUP_LEAVE);" "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54s removed-Group-recipient PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi


f54t_line="$(grep -F '[PartyBotSimulator] F54t detached Group recipient:' "$LOG_FILE" | tail -n 1 | sed -E 's/\\x1B\\[[0-9;]*[[:alpha:]]//g')"
for expected in "castAccepted=1" "queuedBeforeRemoval=1" "selectorChanged=1" "inputTargetChanged=1" "mapRemoved=1" "groupPreserved=1" "queuedGuidRetained=1" "coreCanceled=1" "authorizeSkipped=1" "terminalOnce=1" "terminalInterrupted=1" "currentSpellCleared=1" "eventReaped=1" "noEffects=1" "threatDelta=0.000" "spellReadyBefore=1" "noGcdBefore=1" "spellReadyAfter=1" "noGcdAfter=1" "selectorRestored=1" "mapRestored=1" "worldRestored=1" "locationRestored=1" "groupRestored=1" "subgroupRestored=1" "instanceValidRestored=1" "leaderRestored=1" "assistantRestored=1" "adapterRestored=1"; do
    if [[ "$f54t_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54t detached-recipient assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f54t_expected_guid="$(printf '%s\n' "$f54t_line" | sed -nE 's/.*expectedTargetGuid=([0-9]+).*/\1/p')"
if [[ -z "$f54t_expected_guid" ]]; then
    echo "[PartyBotSimulator] F54t expected detached recipient GUID was not reported." | tee -a "$LOG_FILE"
    exit 1
fi
f54t_core_cancel="$(grep -F '[PartyBotSpellAdmission] update canceled deferred cast because explicit target is unavailable from caster map: spell=19236' "$LOG_FILE" | grep -F "targetGuid=$f54t_expected_guid" | grep -F 'targetUnit=0' | tail -n 1 || true)"
if [[ -z "$f54t_core_cancel" ]]; then
    echo "[PartyBotSimulator] F54t Core did not prove the retained explicit GUID resolved to no live world target." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54t detached a queued recipient from the live Map while retaining Group membership;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54t detached-recipient PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54u_line="$(grep -F '[PartyBotSimulator] F54u cross-Map Group recipient:' "$LOG_FILE" | tail -n 1 | sed -E 's/\\x1B\\[[0-9;]*[[:alpha:]]//g')"
for expected in "castAccepted=1" "queuedBeforeTransfer=1" "selectorChanged=1" "inputTargetChanged=1" "sourceRemoved=1" "groupPreserved=1" "destinationAdded=1" "liveOnDestination=1" "sourceCannotResolve=1" "queuedGuidRetained=1" "coreCanceled=1" "authorizeSkipped=1" "terminalOnce=1" "terminalInterrupted=1" "currentSpellCleared=1" "eventReaped=1" "noEffects=1" "threatDelta=0.000" "spellReadyBefore=1" "noGcdBefore=1" "spellReadyAfter=1" "noGcdAfter=1" "selectorRestored=1" "destinationRemoved=1" "mapRestored=1" "worldRestored=1" "locationRestored=1" "groupRestored=1" "subgroupRestored=1" "instanceValidRestored=1" "leaderRestored=1" "assistantRestored=1" "destinationBindReleased=1" "destinationMapDeleted=1" "adapterRestored=1"; do
    if [[ "$f54u_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54u cross-Map recipient assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
f54u_expected_guid="$(printf '%s\\n' "$f54u_line" | sed -nE 's/.*expectedTargetGuid=([0-9]+).*/\\1/p')"
if [[ -z "$f54u_expected_guid" ]]; then
    echo "[PartyBotSimulator] F54u expected cross-Map recipient GUID was not reported." | tee -a "$LOG_FILE"
    exit 1
fi
f54u_core_cancel="$(grep -F '[PartyBotSpellAdmission] update canceled deferred cast because explicit target is unavailable from caster map: spell=19236' "$LOG_FILE" | grep -F "targetGuid=$f54u_expected_guid" | grep -F 'targetUnit=0' | tail -n 1 || true)"
if [[ -z "$f54u_core_cancel" ]]; then
    echo "[PartyBotSimulator] F54u Core did not prove the exact live cross-Map GUID was unavailable from the caster's source Map." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54u kept the queued recipient alive and in the Group on a distinct destination Map/instance;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] F54u cross-Map recipient PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

f54m_guard_count="$(grep -Fc 'Spell 2061 triggered by aura spell 14892 too deep in cast chain for cast.' "$LOG_FILE" || true)"
if [[ "$f54m_guard_count" != "5" ]]; then
    echo "[PartyBotSimulator] Expected exactly five Core cast-chain guard rejections for the cyclic Flash Heal child, found $f54m_guard_count." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54m executed a four-cast nested target-trigger chain for all five Players, matched each hostile ThreatManager, and restored Core's cast-chain limit after rejecting the fifth cyclic dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54m bounded child-chain/loop-protection PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54e a real Core helpful-heal recipient aura with TAKE_HELPFUL_SPELL proc flags makes the direct-heal threat estimate fail closed; aura removal restores the clean recipient state and the temporary proc flags are restored." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54e recipient-side helpful-proc PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: F54f executed real Core PROC_TRIGGER_SPELL child heals from both caster-side and recipient-side helpful proc flags; measured effective child healing and exact per-hostile healer/tank threat deltas, and restored the temporary aura spell row." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54f real triggered-child proc PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54f_line="$(grep -F '[PartyBotSimulator] F54f triggered child heal:' "$LOG_FILE" | tail -n 1)"
for expected in "rowReady=1" "procEventPresent=1" "procEventRestored=1" "casterAura=1" "casterVeto=1" "casterProcFlush=1" "casterThreat=1" "recipientAura=1" "recipientVeto=1" "recipientProcFlush=1" "recipientThreat=1" "rowRestored=1"; do
    if ! printf '%s\n' "$f54f_line" | grep -F "$expected" >/dev/null; then
        echo "[PartyBotSimulator] F54f triggered-child assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

# These fields are measured effective-health deltas, not boolean flags. Match
# the fixture's own childObserved contract: a positive root heal, more than ten
# points of actual child healing, and an exact root + child = total accounting.
read_f54f_metric() {
    printf '%s\n' "$f54f_line" | sed -n "s/.*$1=\\([0-9][0-9]*\\).*/\\1/p"
}
for side in caster recipient; do
    control="$(read_f54f_metric "${side}Control")"
    root="$(read_f54f_metric "${side}Root")"
    total="$(read_f54f_metric "${side}Total")"
    child="$(read_f54f_metric "${side}Child")"
    if ! [[ "$control" =~ ^[0-9]+$ && "$root" =~ ^[0-9]+$ && "$total" =~ ^[0-9]+$ && "$child" =~ ^[0-9]+$ ]] ||
       (( control <= 0 || root <= 0 || child <= 10 || total != root + child )); then
        echo "[PartyBotSimulator] F54f ${side}-side effective-heal accounting invalid: control=${control:-missing} root=${root:-missing} child=${child:-missing} total=${total:-missing}." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54g real Core SPELL_AURA_ADD_TARGET_TRIGGER dispatched one child heal for each successful target of a five-Player Prayer of Healing; every recipient's live HostileRefManager fan-out matched both hostile ThreatManager deltas, and temporary spell fields plus Group health were restored." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54g real target-trigger fan-out PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54g_line="$(grep -F '[PartyBotSimulator] F54g target-trigger fan-out:' "$LOG_FILE" | tail -n 1)"
for expected in "rowReady=1" "refsValid=1" "baselineCast=1" "rootTargets=5" "auraAttached=1" "triggeredCast=1" "childTargets=5" "threatMatched=1" "auraRemoved=1" "auraRowRestored=1" "rootRowRestored=1" "childRowRestored=1"; do
    if [[ "$f54g_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54g target-trigger assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
if ! grep -Fq "[PartyBotSimulator] PASS: F54g skipped a Core-immune selected Group recipient in target-trigger fan-out; only the four successful Prayer of Healing targets received child heals, and both hostile ThreatManagers matched the successful recipients' effective-healing projections." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54g unsuccessful-target skip PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54g_immune_line="$(grep -F '[PartyBotSimulator] F54g unsuccessful-target guard:' "$LOG_FILE" | tail -n 1)"
for expected in "rowReady=1" "castAccepted=1" "rootTargets=4" "childTargets=4" "immuneTargetUnhealed=1" "threatMatched=1" "auraRemoved=1" "immunityCleared=1" "spellRowsRestored=1"; do
    if [[ "$f54g_immune_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54g unsuccessful-target assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F54g redirected SPELL_AURA_ADD_TARGET_TRIGGER child healing to the caster after Core successfully reflected the hostile root spell; the reflecting Creature remained untouched and both hostile ThreatManagers matched the caster's effective child heal." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54g reflect-to-caster fan-out PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54g_reflect_line="$(grep -F '[PartyBotSimulator] F54g reflect-to-caster fan-out:' "$LOG_FILE" | tail -n 1)"
for expected in "rowReady=1" "reflectAuraAttached=1" "reflectAuraActive=1" "controlHitCaster=1" "casterAuraAttached=1" "triggeredCast=1" "targetUntouched=1" "threatMatched=1" "reflectAuraRemoved=1" "casterAuraRemoved=1" "spellRowsRestored=1"; do
    if [[ "$f54g_reflect_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54g reflect-to-caster assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F54g skipped a target removed after the delayed projectile captured its GUID; the live-target control triggered a caster self-heal, while the removed-target projectile produced no child self-heal or healing-assist threat." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54g deleted-target skip PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54g_deleted_line="$(grep -F '[PartyBotSimulator] F54g deleted-target guard:' "$LOG_FILE" | tail -n 1)"
for expected in "rowReady=1" "auraAttached=1" "controlCast=1" "controlPending=1" "controlRootHit=1" "deletedCast=1" "projectilePending=1" "targetRemoved=1" "targetGuidUnresolved=1" "casterUnhealed=1" "remainingThreatUnchanged=1" "auraRemoved=1" "spellRowsRestored=1"; do
    if [[ "$f54g_deleted_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54g deleted-target assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F54h real Core SPELL_AURA_ADD_TARGET_TRIGGER recursively dispatched child and grandchild heals for all five targets; measured root/child/grandchild effective healing and matched both hostile ThreatManagers." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F54h nested target-trigger PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f54h_line="$(grep -F '[PartyBotSimulator] F54h nested target-trigger chain:' "$LOG_FILE" | tail -n 1)"
for expected in "rowReady=1" "rootCast=1" "rootTargets=5" "stage2AuraAttached=1" "stage2Cast=1" "childTargets=5" "stage2AuraRemoved=1" "stage3AuraAttached=1" "stage3Cast=1" "grandchildTargets=5" "threatMatched=1" "stage3AuraRemoved=1" "auraRowsRestored=1" "rootRowRestored=1" "childRowRestored=1" "grandchildRowRestored=1"; do
    if [[ "$f54h_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54h nested target-trigger assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

f54e_recipient_line="$(grep -F '[PartyBotSimulator] F54e recipient-side helpful proc:' "$LOG_FILE" | tail -n 1)"
for expected in "auraAttached=1" "procVeto=1" "impactsEmpty=1" "auraRemoved=1" "procFlagsRestored=1"; do
    if ! printf '%s\n' "$f54e_recipient_line" | grep -F "$expected" >/dev/null; then
        echo "[PartyBotSimulator] F54e recipient-side proc assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F53f a real successful Core Priest Spell terminal released every matching threat fan-out row and preserved another action by the same Bot, another Bot's row, and a stale cross-encounter event." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F53f successful Core threat-reservation terminal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f53f_success_state_line="$(grep -F '[PartyBotSimulator] F53f real Core successful terminal reservation:' "$LOG_FILE" | tail -n 1)"
for expected in "listener=1" "rowsSeeded=1" "matchingActionRowsReleased=1" "sameBotOtherActionPreserved=1" "otherBotActionPreserved=1" "staleEncounterIgnored=1"; do
    if [[ "$f53f_success_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F53f successful-terminal assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F53f real Core Interrupted and Failed Spell terminals released every in-flight threat row owned by the matching Bot/action while preserving another action by the same Bot and unrelated rows owned by another Bot." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F53f interrupted/failed Core threat-reservation terminal PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f53f_failure_state_line="$(grep -F '[PartyBotSimulator] F53f real Core terminal reservation matrix:' "$LOG_FILE" | tail -n 1)"
for expected in "listener=1" "interruptedSeeded=1" "interruptedRowsReleased=1" "interruptedUnrelatedPreserved=1" "interruptedNoEncounterTerminal=1" "failedSeeded=1" "failedRowReleased=1" "failedUnrelatedPreserved=1" "failedNoEncounterTerminal=1"; do
    if [[ "$f53f_failure_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F53f interrupted/failed-terminal assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: the real five-Player Group PartySnapshotBuilder copied a ROLE_HEALER Priest's learned Lesser Heal ranks 2050/2052 with Core-calculated mana costs, sorted unique spell ranks and deterministic affordable rank selection." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F53a real Priest spell snapshot PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: real five-Player Group PartySnapshotBuilder copied Engine point action/encounter/point correlation from the current Core generator, retained legacy point IDs without Engine ownership, and left idle member movement IDs empty." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The Group PartySnapshotBuilder point-movement integration PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: StopPointMovementAction rejected a stale A snapshot while stacked Engine move B remained current, observed B then A Interrupted terminals in Core expiry order, ignored a legacy point generator, and cancelled exact Engine move C with its matching Interrupted terminal." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The StopPointMovementAction Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: MovePointAction used an actor-only Core resolver, rejected out-of-bounds destinations, preserved the exact unrelated movement generator, and treated its matching typed Reached terminal—not request acceptance—as proof of arrival." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The MovePointAction Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
if ! grep -Fq "[PartyBotSimulator] PASS: StopAutoAttackAction used the snapshot victim GUID and live Core AttackStop, preserved selection/combat and unchanged health/threat, and rejected a different live victim without calling CombatStop." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The StopAutoAttackAction Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: FaceTargetAction used snapshot geometry to face a live selected hostile, skipped an already-facing target, and rejected stale-friendly and moving actors without disturbing position, movement or combat state." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The FaceTargetAction Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: ClearStaleTargetAction cleared a missing selection during a live encounter while preserving a different victim, melee state, attacker reference, combat flag and ThreatManager values." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The ClearStaleTargetAction Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: FollowPartyMemberAction used a GUID-only current-follow snapshot to avoid restarting the same target, retargeted the native Player FollowMovementGenerator to a new group member, and rejected stale live-Group membership without disturbing movement." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The FollowPartyMemberAction Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F52a reapplied the native FollowMovementGenerator when the same live Group leader required a different stable formation slot and verified the new Core offset/angle." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52a same-target formation-update Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F52b retargeted an existing Bot follower after Group::ChangeLeader handed leadership to another live member, then restored the original owner as Group Leader when the Bot itself was promoted and followed the restored leader through NewEngine with zero Legacy policy entry/dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52b Group-leader handoff/owner restoration Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F52d removed and re-added a real PartyBot Player through Group::RemoveMember/AddMember; the shared production snapshot released only its slot, preserved the other three assignments, and the returning Bot reclaimed the vacant slot and resumed native Follow through NewEngine with zero Legacy policy entry/dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52d production Group departure/rejoin Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F52f closed a real five-Player Group encounter immediately on a full wipe, preserved it through a single Bot death, cleared encounter reservations once, verified Core death cleanup and Dead-state observation, then resurrected all members and restored four unique native NewEngine Follow offsets without restarting combat or entering Legacy policy." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52f five-Player wipe/resurrection recovery Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52f_state_line="$(grep -F '[PartyBotSimulator] F52f five-Player wipe recovery:' "$LOG_FILE" | tail -n 1)"
for expected in encounterStarted=1 reservationsSeeded=1 partialDeathSafe=1 wipeClosed=1 reservationsCleared=1 repeatedWipeIdempotent=1 followStoppedOnDeath=1 deadStateObserved=1 resurrectionRecovered=1 noFalseEncounterRestart=1; do
    if [[ "$f52f_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52f wipe/recovery state assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
party_wipe_end_count="$(grep -Ec '\[PartyBotEngineTrace\].*event=encounter_end .*reason=party_wipe([^[:alnum:]_]|$)' "$LOG_FILE" || true)"
if [[ "$party_wipe_end_count" != "1" ]]; then
    echo "[PartyBotSimulator] Expected exactly one Core encounter_end terminal with reason=party_wipe, found $party_wipe_end_count." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F52g Core created a real resurrectable corpse and released ghost; NewEngine selected its self-resurrection action, Core Player::ResurrectPlayer restored the Bot and converted the corpse to bones, then NewEngine restored the original formation Follow without Legacy policy entry/dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g NewEngine ghost self-resurrection Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_state_line="$(grep -F '[PartyBotSimulator] F52g NewEngine ghost self-resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in corpseCreated=1 ghostReleased=1 actionCandidate=1 selfResurrected=1 corpseConvertedToBones=1 followRestored=1 newEngineOnly=1; do
    if [[ "$f52g_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g ghost self-resurrection assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-B NewEngine selected a same-Group corpse, Core Spell::finish created one resurrection offer and emitted the matching Engine action terminal, and the exact reservation was released; live released-ghost, departed-member and out-of-range snapshots were rejected before casting." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-B group-resurrection Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_b_state_line="$(grep -F '[PartyBotSimulator] F52g-B group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in spellAvailable=1 ghostRejected=1 foreignGroupRejected=1 outOfRangeRejected=1 selected=1 castStarted=1 requestPending=1 terminalMatched=1 reservationReleased=1 targetAccepted=1 newEngineOnly=1; do
    if [[ "$f52g_b_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-B group-resurrection assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C1 interrupted a real Core resurrection cast;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C1 interrupted group-resurrection Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c1_state_line="$(grep -F '[PartyBotSimulator] F52g-C1 interrupted group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 interruptedTerminal=1 reservationReleased=1 noOffer=1 unrelatedReservationPreserved=1; do
    if [[ "$f52g_c1_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C1 interruption assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C2 depleted mana after a real Core resurrection cast began;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C2 failed group-resurrection Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c2_state_line="$(grep -F '[PartyBotSimulator] F52g-C2 failed group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 failedTerminal=1 reservationReleased=1 noOffer=1; do
    if [[ "$f52g_c2_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C2 failure assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C3 expired a pending real Core resurrection reservation before its late Interrupted terminal;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C3 reservation-expiry/late-terminal Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c3_state_line="$(grep -F '[PartyBotSimulator] F52g-C3 expired pending group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 reservationExpired=1 reservationReclaimed=1 lateInterruptedTerminal=1 successorPreserved=1 noOffer=1; do
    if [[ "$f52g_c3_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C3 expiry-race assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C4 target Player::BuildPlayerRepop interrupted a real incoming Core resurrection cast," "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C4 target spirit-release Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c4_state_line="$(grep -F '[PartyBotSimulator] F52g-C4 target released spirit during group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 reservationCreated=1 targetReleased=1 interruptedTerminal=1 reservationReleased=1 noOffer=1; do
    if [[ "$f52g_c4_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C4 target-release assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C5 Core interrupted the dead resurrection caster's pending spell and PartyBotAI::OnAliveStateChanged released all of that Bot's reservations" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C5 owner-death cleanup Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c5_state_line="$(grep -F '[PartyBotSimulator] F52g-C5 resurrection caster died during group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 auxiliaryReservation=1 unrelatedReservation=1 ownerDied=1 interruptedTerminal=1 ownerRowsReleased=1 unrelatedPreserved=1 noOffer=1; do
    if [[ "$f52g_c5_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C5 owner-death assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C6 Core interrupted the departing Bot's pending resurrection cast before source Map::Remove;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C6 owner map-leave Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c6_state_line="$(grep -F '[PartyBotSimulator] F52g-C6 resurrection caster left source Map during group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 auxiliaryReservation=1 unrelatedReservation=1 interruptedTerminal=1 auxiliaryPreservedUntilLeave=1 mapLeaveProcessed=1 ownerRowsReleased=1 unrelatedPreserved=1 playerRestored=1 coordinatorReacquired=1 noOffer=1; do
    if [[ "$f52g_c6_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C6 owner-map-leave assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C8 transferred the resurrection caster through real Core Map::Remove/Map::Add into a distinct dungeon instance;" "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C8 owner map-transfer Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c8_state_line="$(grep -F '[PartyBotSimulator] F52g-C8 resurrection caster transferred to another Map during group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 destinationMapCreated=1 auxiliaryReservation=1 unrelatedReservation=1 interruptedTerminal=1 auxiliaryPreservedUntilMapLeave=1 mapLeaveProcessed=1 ownerRowsReleased=1 unrelatedPreserved=1 oldShardPreserved=1 destinationEntered=1 destinationCoordinator=1 destinationSnapshot=1 returnedToSource=1 sourceCoordinatorReacquired=1 destinationMapCleaned=1 noOffer=1 newEngineOnly=1; do
    if [[ "$f52g_c8_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C8 owner map-transfer assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C7 Core Spell::update re-resolved the departed corpse GUID," "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C7 target map-departure Core PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c7_state_line="$(grep -F '[PartyBotSimulator] F52g-C7 resurrection target left source Map during group resurrection:' "$LOG_FILE" | tail -n 1)"
for expected in listener=1 selected=1 castStarted=1 auxiliaryReservation=1 unrelatedReservation=1 targetRemoved=1 interruptedTerminal=1 resurrectionReservationReleased=1 auxiliaryPreserved=1 unrelatedPreserved=1 targetRestored=1 targetCoordinatorReacquired=1 noOffer=1; do
    if [[ "$f52g_c7_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F52g-C7 target-map-departure assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done

if ! grep -Fq "[PartyBotSimulator] PASS: F52g-C lifecycle checks retained exclusive NewEngine authority without Legacy policy entry or dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F52g-C exclusive NewEngine authority PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi
f52g_c_authority_state_line="$(grep -F '[PartyBotSimulator] F52g-C NewEngine authority:' "$LOG_FILE" | tail -n 1)"
if [[ "$f52g_c_authority_state_line" != *"valid=1"* ]]; then
    echo "[PartyBotSimulator] F52g-C NewEngine authority assertion missing valid=1." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: StopFollowingAction rejected a stale follow-target snapshot, expired only the matching native FollowMovementGenerator through MovementExpired(false), and preserved an unrelated PointMovementGenerator." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The StopFollowingAction Core PASS marker was not found." | tee -a "$LOG_FILE"
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

if ! grep -Fq "[PartyBotSimulator] PASS: Lesser Heal with a positive flat spell_threat row healed a same-Group Player with no hostile references and produced no ThreatManager delta." "$LOG_FILE"; then
    echo "[PartyBotSimulator] F53d positive-flat/no-hostile-reference Core heal marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a zero spell_threat Lesser Heal produced only the predicted Core healing-assist threat on both hostile references, with no flat-threat impact." "$LOG_FILE"; then
    echo "[PartyBotSimulator] F53d zero-flat Core threat parity marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: zero flat-threat direct heal matched Core assist-only threat on both hostile references, and the original spell_threat rows were restored." "$LOG_FILE"; then
    echo "[PartyBotSimulator] F53d zero-flat spell_threat restoration marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: a real nonzero positive spell_threat row on Lesser Heal produced the exact per-hostile flat-threat component for both Core ThreatManagers, while total direct-heal threat remained within the assist-plus-flat upper bound and the disposable DB row was restored." "$LOG_FILE"; then
    echo "[PartyBotSimulator] F53c positive flat spell-threat Core parity marker was not found." | tee -a "$LOG_FILE"
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

if ! grep -Fq "[PartyBotSimulator] PASS: F50 real PartyBotAI::UpdateAI registered the production FollowPartyLeader strategy, selected the value-only Group leader GUID, and executed the Core FollowMovementGenerator without entering Legacy policy or registering Shadow strategies." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F50 production NewEngine follow PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

if ! grep -Fq "[PartyBotSimulator] PASS: F51 production state transitions stopped the native Follow when combat began and resumed Group-leader Follow after combat ended, using only NewEngine actions with zero Legacy policy entry/dispatch." "$LOG_FILE"; then
    echo "[PartyBotSimulator] The F51 combat interruption/recovery state-machine PASS marker was not found." | tee -a "$LOG_FILE"
    exit 1
fi

for expected in "listener=1" "otherHealerSeeded=1" "ownReservationCreated=1" "ownReservationReleased=1" "otherHealerPreserved=1"; do
    if ! grep -F "[PartyBotSimulator] F54b real Core heal reservation lifecycle:" "$LOG_FILE" | grep -F "$expected" >/dev/null; then
        echo "[PartyBotSimulator] ERROR: F54b real Core heal reservation lifecycle is missing required state: $expected" >&2
        exit 1
    fi
done
grep -F "[PartyBotSimulator] PASS: F54b a real Core direct heal created an action-scoped Heal reservation" "$LOG_FILE" >/dev/null

f54c_state_line="$(grep -F '[PartyBotSimulator] F54c real Core healing forecast:' "$LOG_FILE" | tail -n 1)"
for expected in "engineCastSkipped=1" "externalDirectCaptured=1" "hotCaptured=1" "allSchoolAbsorbCaptured=1"; do
    if [[ "$f54c_state_line" != *"$expected"* ]]; then
        echo "[PartyBotSimulator] F54c Core healing-forecast assertion missing $expected." | tee -a "$LOG_FILE"
        exit 1
    fi
done
grep -F "[PartyBotSimulator] PASS: F54c live Core snapshot skipped an Engine-owned heal already covered by its Heal reservation" "$LOG_FILE" >/dev/null

echo "[PartyBotSimulator] Runtime PASS: Core map, Creature updates, Core-applied direct/periodic-damage, explicit damage-interleaved healing projection, triggered caster/recipient child-heal and per-hostile threat parity, periodic-healing-assist, periodic-power-assist, periodic-mana-leech and composite periodic-life-leech threat-impact parity, threat switching, Engine attack, triggered spell healing, Player non-triggered spell completion, Engine-selected item-backed self-utility action and stale-inventory race rejection, single-/multi-slot Core item-use aggregation, stack-count consumable and item-as-reagent mutations, per-child Spell correlation and independent charge mutations, CoreAction map-thread actor/target/map-instance preflight, Core point movement, native FollowMovementGenerator tracking, Player follow pause/resume around a non-triggered cast, follow retarget/recovery after leader handoff and target removal, Player heal range/resource/LOS rejection, movement refusal under CAN_NOT_MOVE/POSSESSED, combined real five-Player Core Group roster/cache snapshot, multi-add healer-threat and Core threat projection, Tank/Healer death-reference cleanup, PartyBotAI encounter-correlated death/resurrection trace records, party-wide trace-budget overflow/one-shot marker, sampled non-PlayerBots health/death/resurrection events, server-shutdown encounter flush, source-map final-bot departure closure, source-to-destination dungeon-map transfer and destination shard recreation, five-Player party-wipe/resurrection recovery, NewEngine ghost self-resurrection and formation recovery, sampled trigger activation/cadence and originating-trigger candidate correlation, Engine action ID propagation through PartyBotAI and Core Spell prepare/finish terminal, multi-add retarget recovery, non-victim/offline HostileRefManager threat discovery, movement-driven Player cast interruption and actor cleanup, F54i same-millisecond Core Heal/Damage fail-closed, F54j caster damage pushback plus refreshed snapshot/interruption invalidation, F54k random HoT tick bounds, F54l school-aware absorb matching, F54m bounded four-cast child-chain depth/loop protection and four-spell per-hostile threat parity, F54n instant self-target direct-heal whole-action threat bounds plus active-proc/cast-time/AoE fail-closed gates, F54o deferred Core spell admission, F54p cooldown/GCD boundary, F54q pre-impact queued-cast cancellation, F54r late-recipient re-resolution rejection and F54s removed-Group-recipient rejection, F54t out-of-World target cancellation, F54u live cross-Map target cancellation, F54g target-trigger fan-out plus F54h nested child/grandchild chain." | tee -a "$LOG_FILE"
