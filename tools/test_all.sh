#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

ALL_ENVS=0
if [[ "${1:-}" == "--all-envs" ]]; then
  ALL_ENVS=1
elif [[ $# -gt 0 ]]; then
  echo "Usage: $0 [--all-envs]" >&2
  exit 2
fi

run() {
  echo
  echo "==> $*"
  "$@"
}

run git diff --check
run bash tools/check_static.sh
run python3 tools/check_logic.py

run pio run -e esp12f -j1

examples=(
  basic_wifi
  custom_web
  full_demo
  mqtt_terminal
  sleep_watchdog
  wifi_config_ota
)

for example in "${examples[@]}"; do
  run bash -lc "cd 'examples/${example}' && pio run -e esp12f -j1"
done

if [[ "$ALL_ENVS" -eq 1 ]]; then
  run pio run -e nodemcuv2 -j1
  for example in basic_wifi custom_web sleep_watchdog wifi_config_ota; do
    run bash -lc "cd 'examples/${example}' && pio run -e nodemcuv2 -j1"
  done
fi

echo
echo "All automated tests passed."
