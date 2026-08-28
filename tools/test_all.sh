#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

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
run bash tools/test_mqtt_fixed.sh
run bash tools/test_ota_upload.sh

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

run bash -lc "cd 'examples/mqtt_terminal' && pio run -e esp12f-no-fs -j1"
run bash -lc "cd 'examples/full_demo' && pio run -e esp12f-no-filelog -j1"

NM_TOOL="${HOME}/.platformio/packages/toolchain-xtensa/bin/xtensa-lx106-elf-nm"
NO_FS_ELF="examples/mqtt_terminal/.pio/build/esp12f-no-fs/firmware.elf"
if "$NM_TOOL" -C "$NO_FS_ELF" | rg 'LittleFS|Esp8266BaseConfig|Esp8266BaseFileLog' >/dev/null; then
  fail "no-filesystem MQTT Terminal ELF still contains filesystem/config/filelog symbols"
fi
if strings "$NO_FS_ELF" | rg '/system/filelog|/api/system/hostname|<h2>System</h2>|<h2>Logs</h2>' >/dev/null; then
  fail "MQTT Terminal ELF still contains full-management Web routes/pages"
fi
NO_FILELOG_ELF="examples/full_demo/.pio/build/esp12f-no-filelog/firmware.elf"
if "$NM_TOOL" -C "$NO_FILELOG_ELF" | rg 'Esp8266BaseFileLog' >/dev/null || \
   strings "$NO_FILELOG_ELF" | rg '/system/filelog|<h2>Logs</h2>|Clear File Logs' >/dev/null; then
  fail "no-FileLog full build still contains FileLog objects/routes/pages"
fi

if [[ "$ALL_ENVS" -eq 1 ]]; then
  run pio run -e nodemcuv2 -j1
  for example in basic_wifi custom_web sleep_watchdog wifi_config_ota; do
    run bash -lc "cd 'examples/${example}' && pio run -e nodemcuv2 -j1"
  done
fi

echo
echo "All automated tests passed."
