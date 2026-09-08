#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN="$(mktemp /tmp/esp8266base-record-store.XXXXXX)"
trap 'rm -f "$TEST_BIN"' EXIT
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
 -I "$ROOT_DIR/tools/native_record_store" -DESP8266BASE_USE_RECORD_STORE=1 \
 -DESP8266BASE_LOG_LEVEL=0 "$ROOT_DIR/tools/test_record_store.cpp" -o "$TEST_BIN"
"$TEST_BIN"
