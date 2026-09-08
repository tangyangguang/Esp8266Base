#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN="$(mktemp /tmp/esp8266base-mqtt-fixed.XXXXXX)"
TEST_SPLIT_BIN="$(mktemp /tmp/esp8266base-mqtt-fixed-split.XXXXXX)"
NETWORK_BIN="$(mktemp /tmp/esp8266base-network-helpers.XXXXXX)"
trap 'rm -f "$TEST_BIN" "$TEST_SPLIT_BIN" "$NETWORK_BIN"' EXIT

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
  "$ROOT_DIR/tools/test_mqtt_fixed.cpp" -o "$TEST_BIN"
"$TEST_BIN"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
  -DESP8266BASE_MQTT_MAX_PAYLOAD_BYTES=614 \
  "$ROOT_DIR/tools/test_mqtt_fixed.cpp" -o "$TEST_SPLIT_BIN"
"$TEST_SPLIT_BIN"
echo "MQTT fixed-memory logic tests passed (default and split payload)."

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
  "$ROOT_DIR/tools/test_network_helpers.cpp" -o "$NETWORK_BIN"
"$NETWORK_BIN"
echo "NTP packet and bounded network writer tests passed."
