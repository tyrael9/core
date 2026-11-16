#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
OUT="/tmp/partybot-engine-tests-$$"
trap 'rm -f "$OUT"' EXIT
c++ -std=c++14 -Wall -Wextra -pedantic -pthread \
    -I "$ROOT/src/game/PlayerBots/Engine" \
    "$ROOT/src/game/PlayerBots/Engine/PartyCoordinator.cpp" \
    "$ROOT/src/game/PlayerBots/Engine/PartyBotEngine.cpp" \
    "$ROOT/src/game/PlayerBots/Engine/tests/PartyBotEngineTests.cpp" \
    -o "$OUT"
"$OUT"
