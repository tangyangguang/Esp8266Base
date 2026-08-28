#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN="$(mktemp /tmp/esp8266base-mqtt-fixed.XXXXXX)"
trap 'rm -f "$TEST_BIN"' EXIT

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
  "$ROOT_DIR/tools/test_mqtt_fixed.cpp" -o "$TEST_BIN"
"$TEST_BIN"
echo "MQTT fixed-memory logic tests passed."
