#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

echo "[static] checking ESP8266-only constraints"
if rg -n '#\s*(if|ifdef|ifndef|elif)\b.*ESP32|platform\s*=\s*espressif32|board\s*=.*esp32' \
  src examples platformio.ini library.json; then
  fail "ESP32 conditional/platform branch found"
fi

echo "[static] checking reserved config keys"
if rg -n '#define\s+ESP8266BASE_CFG_KEY_.*"(wifi_ssid|wifi_pass|ap_pass|hostname|web_user|web_pass|wdt_count|wdt_pending|boot_count)"' src; then
  fail "reserved config key without eb_ prefix found"
fi

for key in eb_wifi_ssid eb_wifi_pass eb_boot_count eb_wdt_count eb_wdt_pending; do
  rg -n "$key" src docs README.md >/dev/null || fail "required reserved key/reference missing: $key"
done

echo "[static] checking example log levels"
if rg -n 'ESP8266BASE_LOG_LEVEL=0' examples platformio.ini; then
  fail "example/root default DEBUG log level found"
fi

echo "[static] checking public docs references"
for token in enableFileSink ESP8266BASE_LOG_FILE_LEVEL ESP8266BASE_CFG_READ_AUDIT_LEVEL /logs; do
  rg -n "$token" README.md docs >/dev/null || fail "documentation token missing: $token"
done

echo "[static] checking Web Auth password redaction"
if rg -n 'web_(auth_loaded|password_).*password=%s|expected=%s|new=%s|confirm=%s' src/Esp8266BaseWeb.cpp; then
  fail "Web Auth password must not be logged in plaintext"
fi
rg -n '\(redacted\)' src/Esp8266BaseConfig.cpp >/dev/null || fail "web password audit redaction missing"

echo "[static] checking optional Watchdog guards"
for file in src/Esp8266BaseOTA.cpp src/Esp8266BaseSleep.cpp; do
  if rg -n 'Esp8266BaseWatchdog::(pause|resume)\(' "$file" >/dev/null; then
    rg -n '#if ESP8266BASE_USE_WATCHDOG' "$file" >/dev/null || fail "Watchdog call without feature guard in $file"
  fi
done

echo "[static] ok"
