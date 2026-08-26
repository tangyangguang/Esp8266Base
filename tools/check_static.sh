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

if rg -n '#include\s*[<"].*(PubSubClient|AsyncMqttClient)|setBufferSize\s*\(' src; then
  fail "unapproved MQTT dependency or buffer resizing found in base library"
fi
if rg -n '#include\s*<espMqttClient\.h>' src | rg -v 'src/Esp8266BaseMQTT.cpp'; then
  fail "espMqttClient include must stay inside optional MQTT module"
fi
rg -n 'setBufferSizes\(4096, 1024\)' src/Esp8266BaseMQTT.cpp >/dev/null || \
  fail "MQTT TLS buffers must keep the 4096/1024 baseline"
rg -n 'namespace Esp8266BaseMQTTInternal' src/Esp8266BaseMQTT.cpp >/dev/null || \
  fail "project-private MQTT diagnostic namespace missing"
rg -n 'getLastSSLError' src/Esp8266BaseMQTT.cpp >/dev/null || \
  fail "MQTT TLS last-error capture missing"
if rg -n 'EMC_(RX|TX)_BUFFER_SIZE|setBufferSizes\((512|1024),\s*(512|1024)\)' src examples platformio.ini; then
  fail "MQTT/TLS communication buffer shrinking found"
fi

if rg -n '\b(new|malloc|calloc|realloc)\s*\(|std::function\s*[<(]|std::(vector|map|list)\s*<|\bvirtual\s+' src; then
  fail "forbidden dynamic allocation, STL container/function, or virtual API found"
fi

echo "[static] checking reserved config keys"
if rg -n '#define\s+ESP8266BASE_CFG_KEY_.*"(wifi_ssid|wifi_pass|ap_pass|hostname|web_user|web_pass|wdt_count|wdt_pending|boot_count|filelog_mode)"' src; then
  fail "reserved config key without eb_ prefix found"
fi

for key in eb_wifi_ssid eb_wifi_pass eb_hostname eb_boot_count eb_wdt_count eb_filelog_mode; do
  rg -n "$key" src docs README.md >/dev/null || fail "required reserved key/reference missing: $key"
done

if rg -n 'eb_log\.mode' src docs README.md tools; then
  fail "obsolete FileLog config key found"
fi

rg -n 'ESP8266BASE_DEFAULT_HOSTNAME' src docs README.md examples platformio.ini >/dev/null || \
  fail "default hostname macro reference missing"

if rg -n 'setHostname\s*\(' src examples README.md docs; then
  fail "setHostname API/reference must not remain"
fi

for token in '/system/hostname' '/api/system/hostname' '重启生效'; do
  rg -n "$token" src docs README.md >/dev/null || fail "hostname Web/API documentation token missing: $token"
done

if rg -n 'eb_wdt_pending|ESP8266BASE_CFG_KEY_WDT_PENDING|eb_ap_pass|ESP8266BASE_CFG_KEY_AP_PASS|eb_web_user|ESP8266BASE_CFG_KEY_WEB_USER|旧行为|旧固件|旧无前缀|兼容旧|兼容标记' \
  src README.md docs; then
  fail "historical compatibility wording, WDT pending key, or pseudo config key found"
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
for file in src/Esp8266BaseOTA.cpp src/Esp8266BaseSleep.cpp examples/*/src/main.cpp; do
  [ -f "$file" ] || continue
  if rg -n 'Esp8266BaseWatchdog::(pause|resume)\(' "$file" >/dev/null; then
    rg -n '#if ESP8266BASE_USE_WATCHDOG' "$file" >/dev/null || fail "Watchdog call without feature guard in $file"
  fi
done

echo "[static] checking MQTT_TERMINAL and command-line OTA contract"
rg -n '#define ESP8266BASE_PROFILE_MQTT_TERMINAL 0' src/Esp8266BaseOptions.h >/dev/null || \
  fail "MQTT_TERMINAL profile default must remain disabled"
rg -n 'ESP8266BASE_PROFILE_MQTT_TERMINAL=1' examples/mqtt_terminal/platformio.ini >/dev/null || \
  fail "MQTT_TERMINAL example build flag missing"
rg -n 'bertmelis/espMqttClient @ 1.7.3' examples/mqtt_terminal/platformio.ini >/dev/null || \
  fail "pinned espMqttClient dependency missing"
rg -n 'EMC_MIN_FREE_MEMORY=4096' examples/mqtt_terminal/platformio.ini >/dev/null || \
  fail "MQTT_TERMINAL ESP8266 outbox reserve missing"
rg -n -- '--fail' tools/ota_upload.sh >/dev/null || fail "OTA upload script must use curl --fail"
rg -n 'firmware=@' tools/ota_upload.sh >/dev/null || fail "OTA upload script firmware multipart field missing"
if rg -n 'admin:[^$<{]' tools/ota_upload.sh README.md docs examples/mqtt_terminal; then
  fail "hard-coded OTA password found"
fi
if rg -n '(mqtts?://[^[:space:]]+:[^[:space:]@]+@|MQTT_PASSWORD\[\].*=\s*"[^"$<{]+")' src examples README.md docs; then
  fail "possible real MQTT credential found"
fi

echo "[static] ok"
