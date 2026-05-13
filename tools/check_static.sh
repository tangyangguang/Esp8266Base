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

for key in eb_wifi_ssid eb_wifi_pass eb_boot_count eb_wdt_count; do
  rg -n "$key" src docs README.md >/dev/null || fail "required reserved key/reference missing: $key"
done

if rg -n 'eb_wdt_pending|ESP8266BASE_CFG_KEY_WDT_PENDING|旧行为|旧固件|旧无前缀|兼容旧|兼容标记' \
  src README.md docs; then
  fail "historical compatibility wording or WDT pending compatibility key found"
fi

echo "[static] checking example log levels"
if rg -n 'ESP8266BASE_LOG_LEVEL=0' examples platformio.ini; then
  fail "example/root default DEBUG log level found"
fi

echo "[static] checking public docs references"
for token in Esp8266BaseFileLog ESP8266BASE_FILELOG_DEFAULT_MODE ESP8266BASE_CFG_READ_AUDIT_LEVEL /logs; do
  rg -n "$token" README.md docs >/dev/null || fail "documentation token missing: $token"
done

OLD_FILELOG_PATTERN='enableFile[S]ink|ESP8266BASE_LOG_[F]ILE_LEVEL|ESP8266BASE_LOG_[F]ILE_BUFFER_SIZE|ESP8266BASE_LOG_[F]ILE_FLUSH_INTERVAL_MS|setFile[S]inkLevel|file[S]inkLevel'
if rg -n "$OLD_FILELOG_PATTERN" \
  src examples platformio.ini README.md docs; then
  fail "old FileLog API or macro reference found"
fi

echo "[static] checking default Web Auth password"
rg -n '#define\s+ESP8266BASE_WEB_AUTH_PASS\s+"admin"' src/Esp8266BaseWeb.h >/dev/null || fail "default Web Auth password must be admin"
if rg -n 'ESP8266BASE_WEB_AUTH_PASS=\\"esp8266\\"|admin / esp8266|admin/esp8266|`"esp8266"` \| Basic Auth 编译期默认密码' \
  README.md docs examples platformio.ini; then
  fail "old default Web Auth password reference found"
fi
if rg -n '\(redacted\)|\bredacted\b|不得明文|不会明文|只记录长度、来源和结果' src README.md docs AGENTS.md CHANGELOG.md; then
  fail "password redaction wording found; plaintext password logging is intentional"
fi

echo "[static] checking optional Watchdog guards"
for file in src/Esp8266BaseOTA.cpp src/Esp8266BaseSleep.cpp; do
  if rg -n 'Esp8266BaseWatchdog::(pause|resume)\(' "$file" >/dev/null; then
    rg -n '#if ESP8266BASE_USE_WATCHDOG' "$file" >/dev/null || fail "Watchdog call without feature guard in $file"
  fi
done

echo "[static] ok"
