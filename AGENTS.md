# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

Esp8266Base is a lightweight ESP8266-only Arduino library. **ESP32 is explicitly not supported** — no `#ifdef ESP32` branches exist or should be added. All design decisions are constrained by RAM availability.

## Build & Flash Commands

```bash
# Build an example
cd examples/full_demo && pio run -e esp12f

# Upload firmware (use 460800 baud — 921600 causes packet errors on this hardware)
pio run -e esp12f --target upload

# Monitor serial (pio device monitor has a termios bug on macOS; use python instead)
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbserial-120', 115200, timeout=1)
start = time.time()
while time.time() - start < 60:
    line = s.readline()
    if line: print(line.decode('utf-8', errors='replace'), end='', flush=True)
s.close()
"
```

Each example has its own `platformio.ini` under `examples/<name>/platformio.ini`. The root `platformio.ini` is for developing/testing the library itself.

The full-featured reference example is `examples/full_demo/` — it exercises every module.

## Architecture

All modules are **static classes** — never instantiated, no virtual functions, no `new`/`malloc`. Entry point is `Esp8266Base::begin()` in `setup()` and `Esp8266Base::handle()` in `loop()`.

**Initialization order** (must not be reordered):
```
Log → Sleep → Config (LittleFS) → WiFi → Watchdog → Web → OTA → logDiagnostics()
```
NTP and mDNS are NOT initialized in `begin()` — triggered in `handle()` when WiFi first connects.

**`handle()` dispatch order** (every `loop()` iteration):
```
Config (deferred flush) → WiFi state machine → NTP trigger → mDNS trigger/reset → NTP → mDNS → Web → Watchdog
```

**Module dependency direction** (single direction, no reverse deps):
```
App code
  └─ Esp8266Base (coordinator)
       ├─ Log        (no deps)
       ├─ Config     (LittleFS)
       ├─ WiFi       (reads Config)
       ├─ Web        (starts after WiFi)
       │    └─ OTA   (needs Web server)
       ├─ NTP        (triggers after WiFi connected)
       ├─ mDNS       (activates after WiFi connected)
       ├─ Sleep      (calls Config::flush before sleep)
       └─ Watchdog   (paused during OTA)
```

## RAM Constraints — Read Before Writing Any Code

ESP8266 has ~28–30KB free heap after WiFi + WebServer + LittleFS start. The library must keep total static RAM under 2.5KB.

**Hard rules — never violate:**
1. No `static char buf[1024]` or larger. Max single buffer: 512B (the shared global `_buf`).
2. All HTML strings in `PROGMEM`, never in DRAM.
3. Web responses use `sendContent_P()` / `sendChunk()` (chunked), never concatenate a full page into `String`.
4. No `std::function` — use raw function pointers (`typedef void (*Esp8266BaseWebHandler)()`).
5. No STL containers (`std::vector`, `std::map`, etc.) — use fixed-size static arrays.
6. No `String` in global/module state — use `char[]` with fixed sizes.
7. No recursion.
8. Every new module must declare its RAM budget in `docs/04_memory_budget.md`.

**Heap targets:**

| Scenario | Target free heap |
|---|---|
| Normal operation, web idle | >= 24KB |
| Web page open | >= 18KB |
| OTA in progress | >= 12KB |
| AP config mode | >= 18KB |

## Config Storage

`Esp8266BaseConfig` stores key-value pairs as individual LittleFS files at `/cfg_<key>`. Max key length: 24 chars. Supported types: `string` (≤96B), `int32`, `bool`.

- **Immediate writes** (`setStr`/`setInt`/`setBool`): for low-frequency config like WiFi credentials. Includes write-before-compare (skips Flash write if value unchanged).
- **Deferred writes** (`setIntDeferred`/`setBoolDeferred`): for high-frequency counters. Queued in a 4-slot static array, flushed one per `handle()` call.
- **Before deep sleep or restart**: always call `Esp8266BaseConfig::flush()` first.
- **Reserved keys** (don't reuse): `wifi_ssid`, `wifi_pass`, `ap_pass`, `hostname`, `web_user`, `web_pass`, `wdt_count`, `wdt_pending`, `boot_count`.

## Adding Custom Web Pages/APIs

Register after `Esp8266Base::begin()`:
```cpp
Esp8266BaseWeb::addPage("/mypage", handleMyPage);   // max 4 app pages
Esp8266BaseWeb::addApi("/api/myapi", handleMyApi);  // max 6 app APIs
```

Handler pattern:
```cpp
void handleMyPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PSTR("<h2>My Page</h2>"));
    char buf[64];
    snprintf(buf, sizeof(buf), "<p>Value: %d</p>", myValue);
    Esp8266BaseWeb::sendChunk(buf);
    Esp8266BaseWeb::sendFooter();
}
```

Handlers must be plain functions or captureless lambdas (lambdas with captures cannot convert to function pointers).

## Key Build Flags

| Flag | Default | Notes |
|---|---|---|
| `ESP8266BASE_LOG_LEVEL` | `1` | 0=Debug 1=Info 2=Warn 3=Error 4=Off |
| `ESP8266BASE_WEB_MAX_APP_PAGES` | `4` | App page route slots |
| `ESP8266BASE_WEB_MAX_APP_APIS` | `6` | App API route slots |
| `ESP8266BASE_CFG_DEFERRED_SIZE` | `4` | Deferred write queue depth |
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` | Watchdog timeout |
| `ESP8266BASE_NTP_TIMEZONE` | `28800` | UTC offset in seconds (UTC+8) |
| `ESP8266BASE_WIFI_CONNECT_TIMEOUT` | `20000` | ms for one STA connection attempt before scheduling retry |

Log macros (`ESP8266BASE_LOG_D/I/W/E`) are compiled out entirely when the level is below the threshold — zero runtime cost.

WiFi credential logs intentionally include plaintext passwords for field debugging. Do not classify this as a bug in this project unless the product policy changes.

## WiFi Behavior

- No saved credentials → immediately enters AP config mode (SSID: `ESP8266-<hostname>`)
- Credentials saved but connection fails after timeout → stays in STA mode and retries forever with backoff
- Reconnect intervals: 15s fast retry, then 60s slow retry
- mDNS and NTP only activate after WiFi STA connects

## Partition

The custom linker script `partitions/esp8266-4mb-2mfs.ld` configures 2MB firmware + 2MB LittleFS on 4MB Flash. Use it via `board_build.ldscript = partitions/esp8266-4mb-2mfs.ld`.

The script only defines `MEMORY` regions and filesystem `PROVIDE` symbols, then ends with `INCLUDE "local.eagle.app.v6.common.ld"`. This include is mandatory — it supplies all ROM function addresses. Without it the linker fails with hundreds of undefined reference errors.

## OTA

`Esp8266BaseOTA` uses `Update.begin(ESP.getFreeSketchSpace())`. Do **not** use `UPDATE_SIZE_UNKNOWN` — that constant exists only in the ESP32 Update library and is undefined on ESP8266.

## LittleFS First Boot

`Esp8266BaseConfig::begin()` retries `LittleFS.begin()` once and does not format by default. Automatic format is allowed only when `ESP8266BASE_CFG_FORMAT_ON_FAIL=1` is explicitly set. The framework's `LittleFS.begin()` takes no arguments (unlike ESP32).
