#!/usr/bin/env python3
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def assert_eq(actual, expected, label: str) -> None:
    if actual != expected:
        fail(f"{label}: expected {expected!r}, got {actual!r}")


def format_bytes(value: int) -> str:
    if value < 1024:
        return f"{value} B"
    if value < 1048576:
        v100 = (value * 100 + 512) // 1024
        return f"{v100 // 100}.{v100 % 100:02d} KB"
    v100 = (value * 100 + 524288) // 1048576
    return f"{v100 // 100}.{v100 % 100:02d} MB"


def file_buffer_size_for_mode(mode: int, explicit_size: int | None = None) -> int:
    if explicit_size is not None:
        if explicit_size > 512:
            raise ValueError("buffer size must be <= 512")
        return explicit_size
    return 512 if mode == 1 else 0


def retry_interval(attempt: int, fast_count: int, fast_ms: int, slow_ms: int) -> int:
    return fast_ms if attempt <= fast_count else slow_ms


def millis_due(now: int, deadline: int) -> bool:
    delta = (now - deadline) & 0xFFFFFFFF
    return delta < 0x80000000


def log_segment_path(base: str, index: int) -> str:
    return base if index == 0 else f"{base}.{index}"


def ota_header_ok(data: bytes) -> bool:
    if len(data) < 16:
        return False
    if data[0] != 0xE9:
        return False
    if data[1] == 0 or data[1] > 16:
        return False
    if data[2] > 3:
        return False
    first_addr = int.from_bytes(data[8:12], "little")
    first_size = int.from_bytes(data[12:16], "little")
    first_addr_ok = (0x40100000 <= first_addr < 0x40110000) or (0x3FFE8000 <= first_addr < 0x40000000)
    return first_addr_ok and 0 < first_size <= 65536


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def parse_define_int(text: str, name: str) -> int:
    m = re.search(rf"^\s*#define\s+{re.escape(name)}\s+(\d+)", text, re.M)
    if not m:
        fail(f"missing integer define: {name}")
    return int(m.group(1))


def require_token(text: str, token: str, label: str) -> None:
    if token not in text:
        fail(f"missing {label}: {token}")


def test_format_bytes() -> None:
    cases = {
        0: "0 B",
        1: "1 B",
        1023: "1023 B",
        1024: "1.00 KB",
        1536: "1.50 KB",
        16384: "16.00 KB",
        1048575: "1024.00 KB",
        1048576: "1.00 MB",
        1562378: "1.49 MB",
        1572864: "1.50 MB",
        4294967295: "4096.00 MB",
    }
    for value, expected in cases.items():
        assert_eq(format_bytes(value), expected, f"formatBytes({value})")

    util_h = read("src/Esp8266BaseUtil.h")
    if "(uint64_t)bytes" not in util_h:
        fail("formatBytes MB path must avoid uint32_t overflow")


def test_log_file_buffer_rules() -> None:
    log_h = read("src/Esp8266BaseLog.h")
    filelog_h = read("src/Esp8266BaseFileLog.h")
    filelog_cpp = read("src/Esp8266BaseFileLog.cpp")
    log_cpp = read("src/Esp8266BaseLog.cpp")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_MODE_OFF"), 4, "filelog OFF mode")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_MODE_ERROR"), 3, "filelog ERROR mode")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_MODE_WARN"), 2, "filelog WARN mode")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_MODE_INFO"), 1, "filelog INFO mode")
    require_token(filelog_h,
                  "#define ESP8266BASE_FILELOG_DEFAULT_MODE ESP8266BASE_FILELOG_MODE_ERROR",
                  "default filelog ERROR mode")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS"), 2000, "file flush interval")
    if "ESP8266BASE_FILELOG_DEFAULT_MODE == ESP8266BASE_FILELOG_MODE_INFO" not in filelog_h:
        fail("file buffer default must depend on FileLog default mode INFO")
    assert_eq(file_buffer_size_for_mode(1), 512, "INFO file buffer")
    assert_eq(file_buffer_size_for_mode(2), 0, "WARN file buffer")
    assert_eq(file_buffer_size_for_mode(3), 0, "ERROR file buffer")
    assert_eq(file_buffer_size_for_mode(4), 0, "OFF file buffer")
    assert_eq(file_buffer_size_for_mode(2, explicit_size=0), 0, "explicit disabled buffer")
    if 'case Esp8266BaseFileLog::ERROR: return "ERROR";' not in filelog_cpp:
        fail("FileLog must expose ERROR mode name")
    require_token(filelog_cpp, "if (g_mode == Esp8266BaseFileLog::ERROR && level < 3) return;",
                  "FileLog ERROR threshold")
    for path in ["platformio.ini", "examples/full_demo/platformio.ini"]:
        text = read(path)
        if "ESP8266BASE_FILELOG_DEFAULT_MODE=ESP8266BASE_FILELOG_MODE_INFO" in text:
            fail(f"{path} must not default FileLog to INFO")
    if "setRuntimeLevel" not in log_h or "setSerialLevel" not in log_h:
        fail("Log must split runtime and serial levels")
    if "Esp8266BaseLog::_setInternalHook(_lineSink)" not in filelog_cpp:
        fail("FileLog must register an internal log sink")
    if "ESP8266BASE_CFG_KEY_FILELOG_MODE" not in filelog_cpp:
        fail("FileLog mode must use the reserved key macro")
    config_h = read("src/Esp8266BaseConfig.h")
    require_token(config_h, '#define ESP8266BASE_CFG_KEY_FILELOG_MODE "eb_filelog_mode"', "FileLog reserved config key")
    if "static_assert(sizeof(ESP8266BASE_FILELOG_PATH) <= 32" not in filelog_h:
        fail("FileLog path macro must have a compile-time length guard")
    old_enable_api = "enableFile" + "Sink"
    old_sink_word = "file" + "Sink"
    if old_enable_api in log_h or old_sink_word in log_h or "LittleFS" in log_cpp:
        fail("core Log must not expose or implement FileLog sink")


def test_wifi_retry_rules() -> None:
    wifi_h = read("src/Esp8266BaseWiFi.h")
    wifi_cpp = read("src/Esp8266BaseWiFi.cpp")
    networking = read("docs/08_networking.md")
    api = read("docs/03_api_reference.md")
    fast = parse_define_int(wifi_h, "ESP8266BASE_WIFI_RETRY_FAST")
    fast_count = parse_define_int(wifi_h, "ESP8266BASE_WIFI_RETRY_FAST_COUNT")
    slow = parse_define_int(wifi_h, "ESP8266BASE_WIFI_RETRY_SLOW")
    stuck = parse_define_int(wifi_h, "ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS")
    radio_reset_failures = parse_define_int(
        wifi_h, "ESP8266BASE_WIFI_RADIO_RESET_FAILURE_COUNT")
    radio_reset_settle = parse_define_int(
        wifi_h, "ESP8266BASE_WIFI_RADIO_RESET_SETTLE_MS")
    assert_eq(fast, 2000, "default fast retry")
    assert_eq(fast_count, 3, "default fast retry count")
    assert_eq(slow, 60000, "default slow retry")
    assert_eq(stuck, 7000, "default stuck disconnected restart")
    assert_eq(radio_reset_failures, 6, "default WiFi radio reset failure count")
    assert_eq(radio_reset_settle, 100, "default WiFi radio reset settle")
    assert_eq([retry_interval(i, fast_count, fast, slow) for i in range(1, 6)],
              [2000, 2000, 2000, 60000, 60000],
              "retry interval sequence")
    if "station_connect_stuck_restarting" not in wifi_cpp:
        fail("WiFi must log stuck disconnected restarts")
    if "station_connect_stuck_retrying" not in wifi_cpp:
        fail("WiFi must fast retry when a stuck restart also gets stuck")
    if "ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS < ESP8266BASE_WIFI_CONNECT_TIMEOUT" not in wifi_cpp:
        fail("stuck restart must not replace the full connect timeout")
    if "stuck_disconnected=%lus" not in wifi_cpp:
        fail("wifi_retry_policy must include stuck_disconnected")
    require_token(wifi_cpp, "WiFi.disconnect(false, false)",
                  "WiFi reconnect must retain SDK credentials")
    require_token(wifi_cpp, "WiFi.mode(WIFI_OFF)",
                  "WiFi repeated failures must reset the radio")
    require_token(wifi_cpp, "WiFi.mode(WIFI_STA)",
                  "WiFi radio reset must restore STA mode")
    require_token(wifi_cpp, "station_radio_reset_begin",
                  "WiFi radio reset start diagnostic")
    require_token(wifi_cpp, "station_radio_reset_complete",
                  "WiFi radio reset completion diagnostic")
    require_token(wifi_cpp, "_failuresSinceRadioReset >= ESP8266BASE_WIFI_RADIO_RESET_FAILURE_COUNT",
                  "WiFi radio reset failure threshold")
    if "ESP.restart()" in wifi_cpp:
        fail("WiFi recovery must not reboot the MCU")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    require_token(web_cpp, r'\"wifiAttempt\":%u', "health WiFi attempt evidence")
    require_token(web_cpp, r'\"wifiRadioReset\":%u', "health WiFi radio reset evidence")
    require_token(networking, "不重启 MCU、不清除 Config、不进入 AP",
                  "bounded WiFi radio recovery contract")
    require_token(wifi_cpp, "_isDue(now, _retryAt)", "wrap-safe WiFi retry deadline")
    require_token(wifi_cpp, "static_cast<int32_t>(now - due) >= 0",
                  "WiFi retry signed delta comparison")
    if millis_due(0xFFFFFFF0, 0x00000010) or not millis_due(0x00000020, 0x00000010):
        fail("WiFi retry deadline must remain correct across millis wrap")
    require_token(wifi_cpp, "reason=ssid_too_long", "WiFi SSID length validation")
    require_token(wifi_cpp, "reason=password_too_long", "WiFi password length validation")
    require_token(wifi_cpp, "max=32", "WiFi SSID length limit log")
    require_token(wifi_cpp, "max=63", "WiFi password length limit log")
    require_token(web_cpp, "ssidArg.length() > 32", "Web WiFi raw SSID length validation")
    require_token(web_cpp, "passArg.length() > 63", "Web WiFi raw password length validation")
    require_token(web_cpp, "reason=password_too_long length=%u max=63", "Web WiFi password too long log")
    require_token(web_cpp, "Maximum is 32 bytes.", "Web WiFi SSID too long message")
    require_token(web_cpp, "Maximum is 63 bytes.", "Web WiFi password too long message")
    require_token(networking, "SSID 必须为 1-32 字节，密码必须为 0-63 字节",
                  "WiFi credential length doc")
    require_token(networking, "密码可以为空，用于连接开放 WiFi", "WiFi open network doc")
    require_token(api, "避免 Config 中保存的值与实际 `WiFi.begin()` 使用的截断值不一致",
                  "WiFi credential truncation doc")


def test_ntp_manual_packet_validation() -> None:
    ntp_cpp = read("src/Esp8266BaseNTP.cpp")
    networking = read("docs/08_networking.md")
    api = read("docs/03_api_reference.md")
    require_token(ntp_cpp, "remoteIp != _manualIp", "NTP manual response IP validation")
    require_token(ntp_cpp, "remotePort != NTP_PORT", "NTP manual response port validation")
    require_token(ntp_cpp, "mode != 4", "NTP manual server mode validation")
    require_token(ntp_cpp, "stratum == 0 || stratum > 15", "NTP manual stratum validation")
    require_token(ntp_cpp, "manual_ntp_packet_rejected", "NTP invalid manual packet log")
    require_token(ntp_cpp, "!_isDue(now, _nextManualMs)", "wrap-safe NTP manual retry deadline")
    require_token(ntp_cpp, "static_cast<int32_t>(now - due) >= 0",
                  "NTP retry signed delta comparison")
    if millis_due(0xFFFFFFF0, 0x00000010) or not millis_due(0x00000020, 0x00000010):
        fail("NTP manual retry deadline must remain correct across millis wrap")
    require_token(networking, "主动 UDP NTP 只接受当前等待服务器", "NTP manual validation doc")
    require_token(api, "校验响应来源 IP、端口、mode、stratum", "API NTP validation doc")


def test_config_deferred_rules() -> None:
    config_h = read("src/Esp8266BaseConfig.h")
    config_cpp = read("src/Esp8266BaseConfig.cpp")
    assert_eq(parse_define_int(config_h, "ESP8266BASE_CFG_DEFERRED_SIZE"), 4, "default deferred queue size")
    assert_eq(parse_define_int(config_h, "ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS"), 5000,
              "default deferred flush interval")
    require_token(config_cpp, "strtol(buf, &end, 10)", "strict Config int parsing")
    require_token(config_cpp, 'strcmp(buf, "0") != 0 && strcmp(buf, "1") != 0', "strict Config bool parsing")
    require_token(config_cpp, "config_value_invalid op=getInt", "invalid Config int warning")
    require_token(config_cpp, "config_value_invalid op=getBool", "invalid Config bool warning")


def test_restart_count_skips_deep_sleep_wake() -> None:
    base_cpp = read("src/Esp8266Base.cpp")
    config_doc = read("docs/05_config_storage.md")
    api = read("docs/03_api_reference.md")
    observability = read("docs/07_observability.md")

    require_token(base_cpp, "skip restart_count increment", "deep sleep restart count skip comment")
    require_token(base_cpp, "Esp8266BaseSleep::isDeepSleepWake()", "deep sleep wake guard")
    require_token(base_cpp, "restart_count_skip reason=deep_sleep_wake", "deep sleep restart count skip log")
    require_token(config_doc, "重启计数", "Config restart count wording")
    require_token(api, "重启计数", "API restart count wording")
    require_token(observability, "重启计数", "Observability restart count wording")
    require_token(config_doc, "deep sleep 唤醒不递增", "Config deep sleep skip doc")
    require_token(api, "deep sleep 唤醒不递增", "API deep sleep skip doc")
    require_token(observability, "deep sleep 唤醒不递增", "Observability deep sleep skip doc")


def test_log_segment_paths() -> None:
    assert_eq([log_segment_path("/logs/app.log", i) for i in range(4)],
              ["/logs/app.log", "/logs/app.log.1", "/logs/app.log.2", "/logs/app.log.3"],
              "log rotation paths")


def test_ota_header_guard() -> None:
    esp8266 = bytes.fromhex("e902024080f4104000f01040600d0000")
    esp32 = bytes.fromhex("e907022040088040ee00000000000000")
    gzip = bytes.fromhex("1f8b0800000000000000000000000000")
    assert_eq(ota_header_ok(esp8266), True, "ESP8266 OTA header")
    assert_eq(ota_header_ok(esp32), False, "ESP32 OTA header")
    assert_eq(ota_header_ok(gzip), False, "gzip OTA header")

    ota_cpp = read("src/Esp8266BaseOTA.cpp")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    api = read("docs/03_api_reference.md")
    web_doc = read("docs/06_web_ota.md")
    troubleshooting = read("docs/10_troubleshooting.md")
    require_token(web_cpp, "FileReader", "OTA browser-side header reader")
    require_token(web_cpp, "readAsArrayBuffer(file.slice(0,16))", "OTA browser-side 16-byte preflight")
    require_token(web_cpp, "Invalid firmware: not an ESP8266 app image", "OTA browser-side invalid firmware message")
    require_token(ota_cpp, "_isLikelyEsp8266Firmware", "OTA ESP8266 firmware guard")
    require_token(ota_cpp, "not_esp8266_segment", "OTA ESP32 segment rejection")
    require_token(ota_cpp, "detail=not_esp8266_firmware", "OTA rejection log detail")
    require_token(ota_cpp, "Invalid firmware: not an ESP8266 app image", "OTA server invalid firmware response")
    if '"FAIL"' in ota_cpp:
        fail("OTA server response must not use generic FAIL")
    require_token(ota_cpp, "if (!Update.begin(ESP.getFreeSketchSpace()))", "OTA begin after header guard")
    require_token(api, "内置 OTA 页用 `FileReader`", "API OTA browser preflight doc")
    require_token(api, "浏览器进度条表示上传进度，不代表服务端已经接受固件", "API OTA progress meaning doc")
    require_token(web_doc, "不承诺识别所有同平台非 app 镜像", "Web OTA heuristic limit doc")
    require_token(web_doc, "进度条表示浏览器上传进度", "Web OTA progress meaning doc")
    require_token(troubleshooting, "页面立即提示 `Invalid firmware: not an ESP8266 app image`",
                  "troubleshooting OTA browser rejection doc")


def test_boot_session_log_contract() -> None:
    log_cpp = read("src/Esp8266BaseLog.cpp")
    log_h = read("src/Esp8266BaseLog.h")
    observability = read("docs/07_observability.md")
    api = read("docs/03_api_reference.md")

    if "BOOT SESSION START" in log_cpp or "BOOT SESSION START" in observability:
        fail("boot session log must use the split lowercase format")
    if "reset_reason=%s" in log_cpp:
        fail("boot session log must not emit reset_reason")
    if "const char* resetReason" in log_h or "const char* resetReason" in api:
        fail("beginBootSession parameter must be named bootReason")

    required = [
        "boot_session_start restart_count=%lu",
        "boot_reason=%s boot_desc=%s",
        "firmware=%s version=%s free_heap=%s",
        "reason = \"unknown\"",
        "上电或外部复位",
        "深度睡眠唤醒",
        "软件重启",
        "看门狗重启",
        "未知启动原因",
    ]
    for token in required:
        if token not in log_cpp and token not in observability:
            fail(f"missing boot session contract token: {token}")

    if "boot_desc=上电或外部复位" not in observability:
        fail("observability doc must show the Chinese boot reason description")


def test_web_auth_contract() -> None:
    web_h = read("src/Esp8266BaseWeb.h")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    api = read("docs/03_api_reference.md")
    web_doc = read("docs/06_web_ota.md")
    changelog = read("CHANGELOG.md")

    for path in [
        "src/Esp8266BaseWeb.h",
        "src/Esp8266BaseWeb.cpp",
        "docs/03_api_reference.md",
        "docs/06_web_ota.md",
    ]:
        text = read(path)
        if "setAuth(" in text:
            fail(f"old setAuth API must not remain in {path}")
        if "addNavItem" in text:
            fail(f"old addNavItem API must not remain in {path}")

    if "addPage(const char* path, Esp8266BaseWebHandler handler)" in web_h:
        fail("old title-less addPage overload must not remain")
    if "ESP8266BASE_CFG_KEY_WEB_USER" in web_cpp or "eb_web_user" in api or "eb_web_user" in web_doc:
        fail("persisted Web Auth username pseudo config must not remain")

    required = [
        (web_h, "setDefaultAuth"),
        (web_h, "SYSTEM = 2"),
        (web_cpp, "_server.on(\"/auth\""),
        (web_cpp, "<p><a href='/auth'>Auth Password</a></p>"),
        (web_cpp, "_handleAuthGet"),
        (web_cpp, "_handleAuthPost"),
        (web_cpp, "ESP8266BASE_CFG_KEY_WEB_PASS"),
        (web_cpp, "web_password_updated"),
        (web_cpp, "_formToken"),
        (web_cpp, "name=csrf"),
        (web_cpp, "_verifyFormToken()"),
        (api, "setDefaultAuth"),
        (api, "`/auth`"),
        (web_doc, "认证配置分三层"),
        (changelog, "setDefaultAuth"),
    ]
    for text, token in required:
        if token not in text:
            fail(f"missing Web Auth contract token: {token}")
    if "ssidArg.trim()" in web_cpp or "passArg.trim()" in web_cpp:
        fail("WiFi credentials must preserve leading and trailing spaces")
    for handler in ["_handleWiFiPost", "_handleAuthPost"]:
        start = web_cpp.index(f"void Esp8266BaseWeb::{handler}()")
        end = web_cpp.index("\n}", start)
        require_token(web_cpp[start:end], "if (!_verifyFormToken()) return;",
                      f"{handler} CSRF check")


def test_watchdog_and_ota_failure_contract() -> None:
    watchdog_cpp = read("src/Esp8266BaseWatchdog.cpp")
    watchdog_doc = read("docs/09_power_watchdog.md")
    memory_doc = read("docs/04_memory_budget.md")
    ota_cpp = read("src/Esp8266BaseOTA.cpp")

    require_token(watchdog_cpp, "system_rtc_mem_write", "Watchdog RTC timeout marker")
    require_token(watchdog_cpp, "source=rtc", "Watchdog RTC recovery log")
    require_token(watchdog_cpp, "if (countOk)", "Watchdog RTC clear after Config persistence")
    require_token(watchdog_cpp, "rtc_clear=%s", "Watchdog RTC clear diagnostic")
    if "ESP8266BASE_CFG_KEY_WDT_PENDING" in watchdog_cpp:
        fail("Watchdog must not keep WDT pending compatibility key")
    require_token(watchdog_doc, "超时时只写 RTC user memory 标记，不写 LittleFS", "Watchdog no-Flash timeout doc")
    require_token(watchdog_doc, "64-66", "Watchdog RTC reserved words doc")
    require_token(memory_doc, "96B DRAM + 12B RTC", "Watchdog RTC memory budget")
    require_token(memory_doc, "RTC user memory word 64-66", "Watchdog RTC memory budget detail")
    if "Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT,   (int)_resetCount)" in watchdog_cpp:
        fail("Watchdog timeout branch must not write WDT count to LittleFS directly")
    require_token(ota_cpp, "Update.end();", "OTA write failure cleanup")
    require_token(ota_cpp, "upload_progress progress=%u%% bytes=%s request_total=%s speed=%s elapsed=%s",
                  "OTA progress diagnostics")
    require_token(ota_cpp, "upload_finished uploaded=%s elapsed=%s average_speed=%s free_heap=%s",
                  "OTA finish diagnostics")
    require_token(ota_cpp, "upload_success uploaded=%s elapsed=%s average_speed=%s free_heap=%s action=reboot",
                  "OTA success diagnostics")
    require_token(ota_cpp, "_startedMs", "OTA elapsed state")
    require_token(ota_cpp, "_uploadedBytes", "OTA uploaded byte state")
    require_token(ota_cpp, "_requestBytes", "OTA request byte state")
    require_token(ota_cpp, "_lastProgressPct", "OTA progress step state")
    require_token(ota_cpp, "_resumeWatchdog();", "OTA watchdog resume helper")
    require_token(ota_cpp, "_watchdogPaused", "OTA watchdog resume state")
    require_token(ota_cpp, "_failUpload(", "OTA single failure closeout helper")
    require_token(ota_cpp, "_updateStarted", "OTA Update.begin state")
    require_token(ota_cpp, "Invalid upload: no firmware data", "OTA empty upload rejection")
    require_token(ota_cpp, "OTA_PROGRESS_STEP = 25", "OTA progress log step")
    require_token(ota_cpp, '_failUpload(500, "Update failed: write failed", true,',
                  "OTA write failure immediate closeout")


def legacy_mqtt_terminal_and_ota_lifecycle_contract() -> None:
    options_h = read("src/Esp8266BaseOptions.h")
    base_cpp = read("src/Esp8266Base.cpp")
    mqtt_h = read("src/Esp8266BaseMQTT.h")
    mqtt_cpp = read("src/Esp8266BaseMQTT.cpp")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    web_h = read("src/Esp8266BaseWeb.h")
    ota_cpp = read("src/Esp8266BaseOTA.cpp")
    ota_h = read("src/Esp8266BaseOTA.h")
    terminal_ini = read("examples/mqtt_terminal/platformio.ini")
    terminal_example = read("examples/mqtt_terminal/src/main.cpp")
    upload_script = read("tools/ota_upload.sh")
    config_cpp = read("src/Esp8266BaseConfig.cpp")

    require_token(options_h, "#define ESP8266BASE_PROFILE_MQTT_TERMINAL 0", "MQTT_TERMINAL default")
    require_token(options_h, '#define ESP8266BASE_TERMINAL_HOME_PATH "/health"',
                  "MQTT_TERMINAL default home")
    require_token(options_h, 'ESP8266BASE_PROFILE_MQTT_TERMINAL must be 0 or 1', "profile value guard")
    for dependency in ["ESP8266BASE_USE_WEB", "ESP8266BASE_USE_OTA", "ESP8266BASE_USE_NTP",
                       "ESP8266BASE_USE_WATCHDOG", "ESP8266BASE_USE_MQTT"]:
        require_token(options_h, f"ESP8266BASE_PROFILE_MQTT_TERMINAL && !{dependency}",
                      f"MQTT_TERMINAL requires {dependency}")
    require_token(terminal_ini, "-DESP8266BASE_PROFILE_MQTT_TERMINAL=1", "MQTT_TERMINAL build")
    require_token(terminal_ini, "bertmelis/espMqttClient @ 1.7.3", "pinned MQTT client")
    require_token(terminal_ini, "-DEMC_MIN_FREE_MEMORY=4096", "ESP8266 MQTT outbox reserve")
    require_token(terminal_ini, "-DESP8266BASE_WEB_MAX_APP_PAGES=0", "zero page capacity")
    require_token(terminal_ini, "-DESP8266BASE_WEB_MAX_APP_APIS=0", "zero API capacity")
    require_token(web_h, "#if ESP8266BASE_WEB_MAX_APP_PAGES > 0", "page array compile exclusion")
    require_token(web_h, "#if ESP8266BASE_WEB_MAX_APP_APIS > 0", "API array compile exclusion")

    begin_start = web_cpp.index("bool Esp8266BaseWeb::begin()")
    begin_end = web_cpp.index("void Esp8266BaseWeb::handle()", begin_start)
    begin_body = web_cpp[begin_start:begin_end]
    for route in ["/wifi", "/auth", "/health"]:
        require_token(begin_body, f'_server.on("{route}"', f"terminal retained route {route}")
    require_token(begin_body, '_server.on("/",       HTTP_GET,  _handleTerminalRoot);',
                  "terminal root route")
    root_start = web_cpp.index("void Esp8266BaseWeb::_handleTerminalRoot()")
    root_end = web_cpp.index("#endif", root_start)
    root_body = web_cpp[root_start:root_end]
    require_token(root_body, 'Esp8266BaseWiFiState::AP_CONFIG', "AP root state check")
    require_token(root_body, '_redirect("/wifi")', "AP root WiFi redirect")
    require_token(root_body, '_redirect(ESP8266BASE_TERMINAL_HOME_PATH)',
                  "STA configurable terminal home redirect")
    full_guard_start = begin_body.index("#if !ESP8266BASE_PROFILE_MQTT_TERMINAL")
    full_guard_end = begin_body.index("#endif", full_guard_start)
    full_routes = begin_body[full_guard_start:full_guard_end]
    for route in ["/esp8266base", "/logs", "/system", "/reboot"]:
        require_token(full_routes, route, f"full-only route {route}")
    require_token(ota_cpp, 'server().on("/ota", HTTP_POST', "OTA POST route independent of GET page")
    if '_server.on("/ota",    HTTP_GET' not in web_cpp:
        fail("full Web mode OTA GET route missing")
    require_token(web_h, "ESP8266BASE_USE_OTA && !ESP8266BASE_PROFILE_MQTT_TERMINAL",
                  "MQTT_TERMINAL OTA GET exclusion")

    # WiFi and NTP must gate every MQTT/TLS attempt; loop/connect happen only afterward.
    handle_start = mqtt_cpp.index("void Esp8266BaseMQTT::handle()")
    handle_end = mqtt_cpp.index("uint16_t Esp8266BaseMQTT::publish", handle_start)
    mqtt_handle = mqtt_cpp[handle_start:handle_end]
    wifi_gate = mqtt_handle.index("!Esp8266BaseWiFi::isConnected()")
    ntp_gate = mqtt_handle.index("!Esp8266BaseNTP::isSynced()")
    shutdown_gate = mqtt_handle.index("if (_shutdownActive)")
    client_loop = mqtt_handle.index("\n    mqttClient.loop();", ntp_gate)
    connect_gate = mqtt_handle.index("if (!_connectAttemptsEnabled)")
    connect_call = mqtt_handle.index("mqttClient.connect()")
    if not (shutdown_gate < wifi_gate < ntp_gate < client_loop < connect_gate < connect_call):
        fail("MQTT handle must gate client loop/new connects on WiFi, NTP and business permission")

    base_handle_start = base_cpp.index("void Esp8266Base::handle()")
    base_handle_end = base_cpp.index("void Esp8266Base::logDiagnostics()", base_handle_start)
    base_handle = base_cpp[base_handle_start:base_handle_end]
    if base_handle.index("Esp8266BaseWeb::handle()") > base_handle.index("Esp8266BaseMQTT::handle()"):
        fail("Web handle must run before potentially blocking MQTT connect work")

    for state in ["UNCONFIGURED", "WAITING_WIFI", "WAITING_TIME", "BACKOFF",
                  "CONNECTING", "CONNECTED", "SHUTDOWN_WAIT_ACK",
                  "SHUTDOWN_DISCONNECTING", "PAUSED"]:
        require_token(mqtt_h, state, f"MQTT state {state}")
    require_token(mqtt_cpp, "if (_retryDelay < ESP8266BASE_MQTT_RETRY_MAX_MS)", "bounded backoff")
    require_token(mqtt_cpp, "_retryDelay * 2UL", "exponential backoff")
    require_token(mqtt_cpp, "return (int32_t)(now - due) >= 0;", "wrap-safe retry due check")
    delays = []
    delay = 2000
    for _ in range(7):
        delays.append(delay)
        delay = min(delay * 2, 60000)
    if delays != [2000, 4000, 8000, 16000, 32000, 60000, 60000]:
        fail("MQTT backoff model is not bounded exponential")
    def due(now: int, deadline: int) -> bool:
        delta = (now - deadline) & 0xFFFFFFFF
        signed = delta if delta < 0x80000000 else delta - 0x100000000
        return signed >= 0
    if due(0xFFFFFFF0, 0x00000010) or not due(0x00000010, 0xFFFFFFF0):
        fail("MQTT retry due policy is not uint32 wrap-safe")
    require_token(mqtt_cpp, "if (_connectedCallback) _connectedCallback(sessionPresent);",
                  "business resubscribe callback on every connect")
    require_token(mqtt_cpp, "ota_pause ready=%s result=%u heap=%u max=%u",
                  "OTA post-TLS-release heap diagnostic")
    require_token(mqtt_h, "static bool requestReconnect();", "deferred reconnect public API")
    require_token(mqtt_h, "static bool markConnectionReady();", "application ready public API")
    require_token(mqtt_h, "static void setConnectAttemptsEnabled(bool enabled);",
                  "new connection gate public API")
    require_token(mqtt_h, "static bool connectAttemptsEnabled();",
                  "new connection gate query API")
    setter_start = mqtt_cpp.index("void Esp8266BaseMQTT::setConnectAttemptsEnabled")
    setter_end = mqtt_cpp.index("bool Esp8266BaseMQTT::connectAttemptsEnabled", setter_start)
    setter_body = mqtt_cpp[setter_start:setter_end]
    require_token(setter_body, "_connectAttemptsEnabled = enabled;", "new connection gate state")
    if "mqttClient.disconnect(" in setter_body or "mqttClient.connect()" in setter_body:
        fail("setConnectAttemptsEnabled must not synchronously change transport")
    request_start = mqtt_cpp.index("bool Esp8266BaseMQTT::requestReconnect()")
    request_end = mqtt_cpp.index("bool Esp8266BaseMQTT::markConnectionReady()", request_start)
    request_body = mqtt_cpp[request_start:request_end]
    require_token(request_body, "_reconnectRequested = true;", "deferred reconnect request flag")
    if "mqttClient.disconnect" in request_body or "mqttClient.connect" in request_body:
        fail("requestReconnect must not change transport synchronously")
    reconnect_branch = mqtt_handle.index("if (_reconnectRequested)")
    ordinary_loop = mqtt_handle.index("\n    mqttClient.loop();", reconnect_branch)
    if not ntp_gate < reconnect_branch < ordinary_loop:
        fail("deferred reconnect must run after network gates and before ordinary MQTT work")
    reconnect_body = mqtt_handle[reconnect_branch:ordinary_loop]
    for token in ["_reconnectRequested = false;", "mqttClient.disconnect(true);",
                  "_scheduleRetry();", "return;"]:
        require_token(reconnect_body, token, f"deferred reconnect handling {token}")
    require_token(reconnect_body,
                  "mqttClient.disconnected() && _state != Esp8266BaseMQTTState::BACKOFF",
                  "async disconnect callback double-schedule guard")
    ready_start = mqtt_cpp.index("bool Esp8266BaseMQTT::markConnectionReady()")
    ready_end = mqtt_cpp.index("bool Esp8266BaseMQTT::connected()", ready_start)
    ready_body = mqtt_cpp[ready_start:ready_end]
    for token in ["_configured", "_begun", "_shutdownActive", "_reconnectRequested",
                  "mqttClient.connected()", "_retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;"]:
        require_token(ready_body, token, f"application ready boundary {token}")
    on_connect_start = mqtt_cpp.index("void Esp8266BaseMQTT::_onConnect")
    on_connect_end = mqtt_cpp.index("void Esp8266BaseMQTT::_onDisconnect", on_connect_start)
    on_connect_body = mqtt_cpp[on_connect_start:on_connect_end]
    if "_retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS" in on_connect_body:
        fail("CONNACK must not reset application handshake backoff")
    handshake_delay = 2000
    handshake_schedule = []
    for _ in range(7):
        handshake_schedule.append(handshake_delay)
        # CONNACK intentionally preserves the current value.
        handshake_delay = min(handshake_delay * 2, 60000)
    if handshake_schedule != [2000, 4000, 8000, 16000, 32000, 60000, 60000]:
        fail("application handshake failures must retain bounded exponential backoff")
    handshake_delay = 2000  # markConnectionReady contract
    if handshake_delay != 2000:
        fail("application readiness must restore initial ordinary-disconnect backoff")
    disconnect_start = mqtt_cpp.index("void Esp8266BaseMQTT::_onDisconnect")
    disconnect_end = mqtt_cpp.index("void Esp8266BaseMQTT::_onMessage", disconnect_start)
    disconnect_body = mqtt_cpp[disconnect_start:disconnect_end]
    require_token(disconnect_body, "_reconnectRequested = false;",
                  "completed transport release clears deferred request")
    clean_session_guard = disconnect_body.index("if (_cleanSession)")
    queue_size = disconnect_body.index("mqttClient.queueSize()", clean_session_guard)
    clear_session_queue = disconnect_body.index("mqttClient.clearQueue(true)", queue_size)
    business_disconnect = disconnect_body.index("if (_disconnectedCallback)", clear_session_queue)
    if not clean_session_guard < queue_size < clear_session_queue < business_disconnect:
        fail("clean-session disconnect must discard queued session data before business reconnect")
    require_token(disconnect_body, "session_queue_discarded clean_session=yes packets=%u",
                  "clean-session queue discard diagnostic")

    # Controlled shutdown is one fixed contract: retained QoS1 enqueue, exact PUBACK,
    # graceful DISCONNECT, then a paused terminal state until explicit resume.
    begin_shutdown = mqtt_cpp[mqtt_cpp.index("bool Esp8266BaseMQTT::beginShutdown("):
                              mqtt_cpp.index("Esp8266BaseMQTTShutdownResult Esp8266BaseMQTT::shutdownResult")]
    for token in ["mqttClient.publish(topic, 1, true, payload, length)",
                  "Esp8266BaseMQTTShutdownResult::PUBLISH_FAILED",
                  "_finishShutdown(Esp8266BaseMQTTShutdownResult::PUBLISH_FAILED)",
                  "_shutdownActive = true;"]:
        require_token(begin_shutdown, token, f"controlled shutdown start {token}")
    graceful_start = mqtt_cpp.index("void Esp8266BaseMQTT::_startGracefulDisconnect()")
    graceful_end = mqtt_cpp.index("void Esp8266BaseMQTT::_finishShutdown", graceful_start)
    graceful_body = mqtt_cpp[graceful_start:graceful_end]
    require_token(graceful_body, "mqttClient.disconnect(false)", "normal MQTT DISCONNECT")
    if "disconnect(true)" in graceful_body:
        fail("controlled shutdown must never force disconnect and risk LWT overwrite")
    publish_ack_start = mqtt_cpp.index("void Esp8266BaseMQTT::_onPublishAck")
    publish_ack_end = mqtt_cpp.index("void Esp8266BaseMQTT::_onClientError", publish_ack_start)
    publish_ack_body = mqtt_cpp[publish_ack_start:publish_ack_end]
    exact_ack = publish_ack_body.index("packetId == _shutdownPacketId")
    graceful_after_ack = publish_ack_body.index("_startGracefulDisconnect();", exact_ack)
    ignored_ack = publish_ack_body.index("shutdown_ack_ignored", graceful_after_ack)
    if not exact_ack < graceful_after_ack < ignored_ack:
        fail("only the exact shutdown PUBACK may advance graceful disconnect")
    shutdown_handle_start = mqtt_cpp.index("void Esp8266BaseMQTT::_handleShutdown()")
    shutdown_handle_end = mqtt_cpp.index("void Esp8266BaseMQTT::resumeAfterShutdown", shutdown_handle_start)
    shutdown_handle = mqtt_cpp[shutdown_handle_start:shutdown_handle_end]
    for token in ["PUBACK_TIMEOUT", "DISCONNECT_TIMEOUT"]:
        require_token(shutdown_handle, token, f"bounded shutdown failure {token}")
    timeout_branch = shutdown_handle[shutdown_handle.index("SHUTDOWN_WAIT_ACK"):]
    if timeout_branch.index("mqttClient.clearQueue(true)") > timeout_branch.index("PUBACK_TIMEOUT"):
        fail("PUBACK timeout must discard the stale final packet before recovery")
    resume_start = mqtt_cpp.index("void Esp8266BaseMQTT::resumeAfterShutdown()")
    resume_end = mqtt_cpp.index("bool Esp8266BaseMQTT::pauseForOTA()", resume_start)
    resume_body = mqtt_cpp[resume_start:resume_end]
    require_token(resume_body, "_shutdownActive = false;", "explicit shutdown recovery")
    require_token(resume_body, "reconnect=yes", "explicit reconnect recovery diagnostic")
    require_token(resume_body, "_state = Esp8266BaseMQTTState::UNCONFIGURED;",
                  "unconfigured OTA failure recovery state")
    ota_pause_start = mqtt_cpp.index("bool Esp8266BaseMQTT::pauseForOTA()")
    ota_pause_end = mqtt_cpp.index("void Esp8266BaseMQTT::resumeAfterOTAFailure", ota_pause_start)
    ota_pause_body = mqtt_cpp[ota_pause_start:ota_pause_end]
    for token in ["!_configured || !_begun", "mqttClient.disconnected()",
                  "Esp8266BaseMQTTShutdownResult::NOT_CONNECTED",
                  "_shutdownActive = true;", "_state = Esp8266BaseMQTTState::PAUSED;"]:
        require_token(ota_pause_body, token, f"OTA inactive-transport pause {token}")
    if "shutdownSucceeded()" in ota_pause_body:
        fail("OTA readiness must not require or forge shutdown SUCCESS when no transport is active")
    inactive_gate = ota_pause_body.index("if (!mqttClient.disconnected())")
    missing_shutdown_failure = ota_pause_body.index("return false;", inactive_gate)
    enter_inactive_pause = ota_pause_body.index("_shutdownActive = true;", missing_shutdown_failure)
    transport_ready = ota_pause_body.index("bool ready = mqttClient.disconnected();",
                                           enter_inactive_pause)
    if not inactive_gate < missing_shutdown_failure < enter_inactive_pause < transport_ready:
        fail("OTA pause must reject active/busy transport and accept only fully disconnected transport")
    for blocked_api in ["uint16_t Esp8266BaseMQTT::publish", "uint16_t Esp8266BaseMQTT::subscribe",
                        "bool Esp8266BaseMQTT::requestReconnect", "bool Esp8266BaseMQTT::markConnectionReady"]:
        start = mqtt_cpp.index(blocked_api)
        end = mqtt_cpp.index("\n}", start)
        require_token(mqtt_cpp[start:end], "_shutdownActive", f"paused API gate {blocked_api}")

    # Executable transition vectors cover success, mismatched ACK, enqueue failure,
    # ACK timeout, normal disconnect completion, and explicit recovery.
    def shutdown_vector(enqueued: bool, acks: list[int], expected: int,
                        ack_timeout: bool, disconnect_reason: str) -> tuple[str, bool, bool]:
        paused = True
        if not enqueued:
            return ("publish_failed", paused, False)
        matched = any(packet_id == expected for packet_id in acks)
        if not matched:
            return ("puback_timeout" if ack_timeout else "in_progress", paused, False)
        if disconnect_reason == "user_ok":
            return ("success", paused, True)
        return ("connection_lost", paused, False)

    vectors = [
        (shutdown_vector(True, [41], 41, False, "user_ok"), ("success", True, True)),
        (shutdown_vector(True, [40], 41, False, "none"), ("in_progress", True, False)),
        (shutdown_vector(False, [], 0, False, "none"), ("publish_failed", True, False)),
        (shutdown_vector(True, [], 41, True, "none"), ("puback_timeout", True, False)),
        (shutdown_vector(True, [41], 41, False, "tcp_lost"), ("connection_lost", True, False)),
    ]
    for actual, expected in vectors:
        if actual != expected:
            fail(f"controlled shutdown vector mismatch: {actual} != {expected}")

    # OTA readiness is separate from final-message success. An absent/released transport
    # may proceed with an honest non-success result; active or connecting transports may not.
    def ota_pause_vector(configured: bool, begun: bool, transport: str,
                         shutdown_started: bool, shutdown_terminal: bool,
                         shutdown_result: str) -> tuple[bool, str]:
        if not configured or not begun:
            return (True, "not_connected")
        if not shutdown_started:
            if transport != "disconnected":
                return (False, shutdown_result)
            return (True, "not_connected")
        if not shutdown_terminal:
            return (False, shutdown_result)
        return (transport == "disconnected", shutdown_result)

    ota_vectors = [
        (ota_pause_vector(False, False, "disconnected", False, False, "none"),
         (True, "not_connected")),
        (ota_pause_vector(True, False, "disconnected", False, False, "none"),
         (True, "not_connected")),
        (ota_pause_vector(True, True, "disconnected", False, False, "none"),
         (True, "not_connected")),
        (ota_pause_vector(True, True, "connected", False, False, "none"),
         (False, "none")),
        (ota_pause_vector(True, True, "connecting", False, False, "none"),
         (False, "none")),
        (ota_pause_vector(True, True, "connected", True, True, "publish_failed"),
         (False, "publish_failed")),
        (ota_pause_vector(True, True, "disconnected", True, True, "success"),
         (True, "success")),
        (ota_pause_vector(True, True, "disconnected", True, True, "connection_lost"),
         (True, "connection_lost")),
    ]
    for actual, expected in ota_vectors:
        if actual != expected:
            fail(f"OTA pause vector mismatch: {actual} != {expected}")
    require_token(mqtt_cpp, "missing_required_config", "missing MQTT config failure")
    require_token(mqtt_cpp, "config.password == nullptr || config.password[0] == '\\0' ||",
                  "password requires username")
    require_token(mqtt_cpp, "config.willTopic != nullptr && config.willTopic[0] != '\\0'",
                  "LWT payload requires topic")
    require_token(mqtt_cpp, ".setBufferSizes(4096, 1024)", "reliable TLS buffer baseline")
    if "EMC_RX_BUFFER_SIZE" in mqtt_cpp or "EMC_TX_BUFFER_SIZE" in terminal_ini:
        fail("MQTT_TERMINAL must not shrink espMqttClient communication buffers")
    for api in ["publish", "subscribe", "setWill", "setKeepAlive", "onMessage", "onDisconnect",
                "onSubscribe", "onPublish", "setClientErrorCallback"]:
        require_token(mqtt_cpp, api, f"MQTT transport API {api}")
    require_token(terminal_example, "Esp8266BaseMQTT::setCallbacks", "generic MQTT callback example")
    require_token(terminal_example, "Esp8266BaseMQTT::requestReconnect()",
                  "application readiness reconnect example")
    require_token(terminal_example, "Esp8266BaseMQTT::markConnectionReady()",
                  "application readiness success example")
    for callback in ["onMqttSubscribeAck", "onMqttPublishAck", "onMqttClientError"]:
        require_token(terminal_example, callback, f"MQTT example callback {callback}")
    require_token(mqtt_cpp, "class DiagnosticSecureClient", "private diagnostic secure client")
    require_token(mqtt_cpp, "State::disconnectingTcp1", "TLS error capture before transport release")
    require_token(mqtt_cpp, "getLastSSLError", "BearSSL last TLS error capture")
    require_token(mqtt_cpp, "clearTlsError();", "clear stale TLS error before connect")
    require_token(mqtt_cpp, "suback_rejected", "SUBACK rejection diagnostic")
    require_token(mqtt_cpp, "client_error", "MQTT client error diagnostic")
    require_token(mqtt_h, "lastTlsErrorCode", "TLS error code diagnostic API")
    require_token(mqtt_h, "lastTlsErrorText", "TLS error text diagnostic API")
    require_token(mqtt_h, "Esp8266BaseMQTTSubscribeAckCallback", "stable SUBACK callback API")
    require_token(mqtt_h, "Esp8266BaseMQTTPublishAckCallback", "stable publish ack callback API")
    require_token(mqtt_h, "Esp8266BaseMQTTClientError", "stable client error mapping")
    if "espMqttClientTypes" in mqtt_h or "espMqttClient.h" in mqtt_h:
        fail("third-party MQTT types must not leak into the public header")

    diagnostic_start = mqtt_cpp.index("class DiagnosticSecureClient")
    diagnostic_end = mqtt_cpp.index("}  // namespace Esp8266BaseMQTTInternal", diagnostic_start)
    diagnostic = mqtt_cpp[diagnostic_start:diagnostic_end]
    base_loop = diagnostic.index("MqttClient::loop();")
    first_capture = diagnostic.index("captureTlsError();")
    second_capture = diagnostic.index("captureTlsError();", first_capture + 1)
    if not first_capture < base_loop < second_capture:
        fail("TLS error capture must bracket the third-party loop before transport release")

    prepare_call = ota_cpp.index("_prepareCallback(_failureMessage")
    update_begin = ota_cpp.index("Update.begin(ESP.getFreeSketchSpace())")
    if prepare_call >= update_begin:
        fail("OTA prepare callback must run before Update.begin")
    rejection_return = ota_cpp.index("return;", prepare_call)
    mqtt_pause = ota_cpp.index("Esp8266BaseMQTT::pauseForOTA()", prepare_call)
    if not (rejection_return < mqtt_pause < update_begin):
        fail("OTA prepare rejection must leave MQTT untouched; accepted prepare must pause MQTT before Update.begin")
    require_token(ota_cpp, "if (_prepareCallback &&", "optional prepare callback")
    require_token(ota_cpp, "if (_failureCallback)", "optional failure callback")
    require_token(ota_cpp, "if (_successCallback)", "optional success callback")
    require_token(ota_cpp, "if (_failureNotified) return;", "failure callback once guard")
    require_token(ota_cpp, "_failureNotified = false;", "failure callback per-request reset")
    fail_start = ota_cpp.index("void Esp8266BaseOTA::_failUpload")
    fail_end = ota_cpp.index("// ----------------------------------------------------------------------------", fail_start)
    fail_body = ota_cpp[fail_start:fail_end]
    if not (fail_body.index("_resumeWatchdog();") <
            fail_body.index("Esp8266BaseMQTT::resumeAfterOTAFailure();") <
            fail_body.index("_notifyFailure(failure);")):
        fail("OTA failure must restore Watchdog and MQTT before business callback")
    success_start = ota_cpp.index("void Esp8266BaseOTA::_notifySuccess()")
    success_end = ota_cpp.index("void Esp8266BaseOTA::_failUpload", success_start)
    success_body = ota_cpp[success_start:success_end]
    require_token(success_body, "Esp8266BaseMQTT::keepPausedAfterOTASuccess();",
                  "OTA success keeps MQTT paused")
    for failure in ["UNAUTHORIZED", "INVALID_FIRMWARE", "PREPARE_REJECTED",
                    "UPDATE_BEGIN_FAILED", "UPDATE_WRITE_FAILED", "UPDATE_END_FAILED",
                    "UPLOAD_ABORTED", "NO_FIRMWARE_DATA", "CONFIG_FLUSH_FAILED",
                    "FILELOG_FLUSH_FAILED", "MQTT_PAUSE_FAILED"]:
        require_token(ota_h, failure, f"OTA failure reason {failure}")
    require_token(ota_cpp, "pre_reboot_config_flush_failed", "OTA config flush failure diagnostic")
    require_token(ota_cpp, "action=abort_update", "OTA flush failure aborts update")
    config_flush = ota_cpp.index("if (!Esp8266BaseConfig::flush())")
    update_end = ota_cpp.index("if (Update.end(true))")
    if config_flush >= update_end:
        fail("OTA Config flush must succeed before Update.end(true)")
    require_token(ota_cpp, '"Config flush failed; update aborted", true,',
                  "OTA Config flush failure aborts Update")
    require_token(ota_cpp, "保留首个失败的 HTTP 状态", "OTA first failure status preservation")
    require_token(ota_cpp, "if (_status == 200 && (!_started || _uploadedBytes == 0))",
                  "OTA completion preserves prior rejection status")
    require_token(ota_cpp, "upload_failed status=%u", "OTA failure diagnostic before state reset")
    require_token(ota_cpp, "_resetRequestState();\n    }", "OTA failure internal state reset")

    for token in ["firmware", "version", "uptime", "heap", "maxBlock", "wifi", "ip",
                  "ntp", "mqtt", "mqttConnected", "mqttAttempt", "mqttLastReason",
                  "mqttTlsError", "lastWdtReset", "otaInProgress"]:
        require_token(web_cpp, token, f"health field {token}")
    health_start = web_cpp.index("void Esp8266BaseWeb::_handleHealth()")
    health_end = web_cpp.index("void Esp8266BaseWeb::_handleNotFound()", health_start)
    if "lastTlsErrorText" in web_cpp[health_start:health_end]:
        fail("health endpoint must not expose long TLS error text")

    require_token(upload_script, "--fail-with-body", "curl body-preserving fail support")
    require_token(upload_script, "exit 22", "legacy curl explicit HTTP failure")
    if "--fail --fail-with-body" in upload_script or "--fail-with-body --fail" in upload_script:
        fail("OTA upload script must not combine mutually exclusive curl fail options")
    require_token(upload_script, 'firmware=@${firmware}', "curl firmware field")
    require_token(upload_script, "Firmware size:", "OTA script firmware size output")
    require_token(upload_script, "HTTP result:", "OTA script HTTP result output")
    require_token(upload_script, "Device response:", "OTA script response output")

    for token in [".tmp", ".bak", "verify_failed", "backup_rename_failed",
                  "commit_rename_failed", "LittleFS.rename(bak, path)"]:
        require_token(config_cpp, token, f"Config recovery invariant {token}")


def test_fixed_mqtt_terminal_and_ota_lifecycle_contract() -> None:
    options_h = read("src/Esp8266BaseOptions.h")
    base_cpp = read("src/Esp8266Base.cpp")
    mqtt_h = read("src/Esp8266BaseMQTT.h")
    mqtt_cpp = read("src/Esp8266BaseMQTT.cpp")
    fixed_h = read("src/Esp8266BaseMQTTFixed.h")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    ota_cpp = read("src/Esp8266BaseOTA.cpp")
    terminal_ini = read("examples/mqtt_terminal/platformio.ini")

    for token in ["ESP8266BASE_USE_FILESYSTEM", "ESP8266BASE_USE_CONFIG",
                  "ESP8266BASE_USE_WIFI_CONFIG", "ESP8266BASE_USE_WEB_AUTH_CONFIG",
                  "ESP8266BASE_USE_FILELOG"]:
        require_token(options_h, token, f"storage feature switch {token}")
    require_token(terminal_ini, "[env:esp12f-no-fs]", "no-filesystem terminal build")
    for token in ["-DESP8266BASE_USE_FILESYSTEM=0", "-DESP8266BASE_USE_CONFIG=0",
                  "-DESP8266BASE_USE_FILELOG=0"]:
        require_token(terminal_ini, token, f"no-filesystem build flag {token}")

    require_token(fixed_h, "#define ESP8266BASE_MQTT_TX_SLOTS 2", "two fixed TX slots")
    require_token(fixed_h, "#define ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES 512",
                  "bounded outbound payload")
    require_token(fixed_h, "#define ESP8266BASE_MQTT_RX_CHUNK_BYTES 256",
                  "streaming receive chunk")
    for token in ["CAPACITY_EXHAUSTED", "PACKET_TOO_LARGE", "PROTOCOL_ERROR"]:
        require_token(mqtt_h, token, f"explicit MQTT error {token}")
    require_token(mqtt_cpp, "implementation=fixed_sync_tls", "fixed MQTT implementation")
    require_token(mqtt_cpp, "heap_outbox=no heap_packet_buffer=no",
                  "steady-state allocation diagnostic")
    require_token(mqtt_cpp, "client.setBufferSizes(4096, 1024)", "TLS buffer baseline")
    if "espMqttClient" in mqtt_cpp or "EMC_MIN_FREE_MEMORY" in terminal_ini:
        fail("dynamic espMqttClient path must not remain")

    handle = mqtt_cpp[mqtt_cpp.index("void Esp8266BaseMQTT::handle()"):
                      mqtt_cpp.index("bool Esp8266BaseMQTT::_connectTransport()")]
    gates = [handle.index("!Esp8266BaseWiFi::isConnected()"),
             handle.index("!Esp8266BaseNTP::isSynced()"),
             handle.index("if (_reconnectRequested)"),
             handle.index("!_connectAttemptsEnabled"),
             handle.index("_connectTransport()")]
    if gates != sorted(gates):
        fail("MQTT connect must remain behind WiFi, NTP, reconnect, and actuator gates")

    base_handle = base_cpp[base_cpp.index("void Esp8266Base::handle()"):
                           base_cpp.index("void Esp8266Base::logDiagnostics()")]
    if base_handle.index("Esp8266BaseWeb::handle()") > base_handle.index("Esp8266BaseMQTT::handle()"):
        fail("Web must run before synchronous TLS connection work")

    for token in ["outbox.nextToSend()", "outbox.markSent(packet)",
                  "outbox.acknowledge(PacketKind::PUBLISH, packetId)",
                  "outbox.acknowledge(PacketKind::SUBSCRIBE, id)"]:
        require_token(mqtt_cpp, token, f"fixed ACK lifecycle {token}")
    require_token(fixed_h, "if (_inFlight >= 0) return nullptr;",
                  "only one QoS packet in flight")
    require_token(fixed_h, "packet.packetId != packetId", "matching ACK requirement")
    require_token(fixed_h, "packet.dup = true;", "persistent-session resend DUP")
    require_token(mqtt_cpp, "outbox.prepareReconnect(_cleanSession)",
                  "clean-session queue release")

    begin_shutdown = mqtt_cpp[mqtt_cpp.index("bool Esp8266BaseMQTT::beginShutdown("):
                              mqtt_cpp.index("Esp8266BaseMQTTShutdownResult Esp8266BaseMQTT::shutdownResult")]
    for token in ["enqueuePublish(id, topic, 1, true", "_shutdownPacketId = id",
                  "_shutdownDeadline = millis() + timeoutMs"]:
        require_token(begin_shutdown, token, f"controlled shutdown {token}")
    ack_body = mqtt_cpp[mqtt_cpp.index("void Esp8266BaseMQTT::_onPublishAck"):
                        mqtt_cpp.index("void Esp8266BaseMQTT::_onClientError")]
    if ack_body.index("outbox.acknowledge") > ack_body.index("packetId == _shutdownPacketId"):
        fail("shutdown may advance only after matching fixed-outbox PUBACK")
    require_token(mqtt_cpp, "const uint8_t packet[] = {0xe0, 0};", "normal MQTT DISCONNECT")
    require_token(mqtt_cpp, "Esp8266BaseMQTTShutdownResult::PUBACK_TIMEOUT",
                  "bounded shutdown PUBACK timeout")

    terminal_begin = web_cpp[web_cpp.index("bool Esp8266BaseWeb::begin()"):
                             web_cpp.index("void Esp8266BaseWeb::handle()")]
    require_token(terminal_begin, "_server.onNotFound(_handleTerminalDispatch)",
                  "single terminal base dispatcher")
    dispatch_start = web_cpp.index("void Esp8266BaseWeb::_handleTerminalDispatch")
    dispatch = web_cpp[dispatch_start:web_cpp.index("#endif", dispatch_start)]
    for route in ['uri == "/"', 'uri == "/wifi"', 'uri == "/auth"', 'uri == "/health"']:
        require_token(dispatch, route, f"terminal fixed route {route}")
    require_token(ota_cpp, 'server().on("/ota", HTTP_POST', "multipart OTA route")
    require_token(ota_cpp, "Esp8266BaseMQTT::pauseForOTA()", "OTA transport release gate")
    require_token(ota_cpp, "Esp8266BaseMQTT::resumeAfterOTAFailure()", "OTA failure recovery")
    require_token(ota_cpp, "Esp8266BaseMQTT::keepPausedAfterOTASuccess()", "OTA success pause")

    # 对受控下线/OTA 的外部可观察转换做纯逻辑向量测试。传输细节由上面的
    # 源码契约和 C++ 固定队列测试覆盖；这里验证错误不能伪装成成功。
    def shutdown(enqueued: bool, expected_id: int, acks: list[int],
                 timed_out: bool, transport_lost: bool, disconnect_written: bool) -> str:
        if not enqueued:
            return "publish_failed"
        if transport_lost:
            return "connection_lost"
        if expected_id not in acks:
            return "puback_timeout" if timed_out else "in_progress"
        return "success" if disconnect_written else "disconnect_failed"

    vectors = [
        ((True, 41, [41], False, False, True), "success"),
        ((True, 41, [40], False, False, True), "in_progress"),
        ((True, 41, [], True, False, True), "puback_timeout"),
        ((True, 41, [], False, True, True), "connection_lost"),
        ((True, 41, [41], False, False, False), "disconnect_failed"),
        ((False, 0, [], False, False, False), "publish_failed"),
    ]
    for args, expected in vectors:
        if shutdown(*args) != expected:
            fail(f"fixed shutdown vector mismatch: {args}")

    def ota_pause(configured: bool, begun: bool, transport_open: bool,
                  shutdown_active: bool, shutdown_result: str) -> tuple[bool, str]:
        if not configured or not begun or not transport_open:
            return (True, "not_connected")
        if not shutdown_active:
            return (False, shutdown_result)
        return (shutdown_result == "success", shutdown_result)

    ota_vectors = [
        ((False, False, False, False, "none"), (True, "not_connected")),
        ((True, True, False, False, "none"), (True, "not_connected")),
        ((True, True, True, False, "none"), (False, "none")),
        ((True, True, True, True, "success"), (True, "success")),
        ((True, True, True, True, "puback_timeout"), (False, "puback_timeout")),
    ]
    for args, expected in ota_vectors:
        if ota_pause(*args) != expected:
            fail(f"fixed OTA vector mismatch: {args}")


def test_public_default_tables() -> None:
    readme = read("README.md")
    api = read("docs/03_api_reference.md")
    overview = read("docs/01_overview.md")
    web_doc = read("docs/06_web_ota.md")
    architecture = read("docs/02_architecture.md")
    memory = read("docs/04_memory_budget.md")
    options_h = read("src/Esp8266BaseOptions.h")
    base_h = read("src/Esp8266Base.h")
    base_cpp = read("src/Esp8266Base.cpp")

    for text, label in [(readme, "README"), (api, "API reference"), (overview, "overview")]:
        require_token(text, '| `ESP8266BASE_WEB_AUTH_PASS` | `"admin"` |', f"{label} Web Auth default")
        require_token(text, '| `ESP8266BASE_WIFI_RETRY_FAST` | `2000` |', f"{label} WiFi fast retry default")
        require_token(text, '| `ESP8266BASE_USE_OTA` | `0` | 编译 OTA；要求 `ESP8266BASE_USE_WEB=1` |',
                      f"{label} OTA/Web dependency")
        require_token(text, '| `ESP8266BASE_DEFAULT_HOSTNAME` | `"esp8266base"` |',
                      f"{label} default hostname")

    require_token(options_h, '#define ESP8266BASE_DEFAULT_HOSTNAME "esp8266base"', "default hostname macro")
    require_token(base_h, "static bool isValidHostname(const char* hostname);", "hostname validation API")
    require_token(base_cpp, "ESP8266BASE_CFG_KEY_HOSTNAME", "hostname persisted key usage")
    require_token(base_cpp, "default_hostname_invalid", "invalid default hostname diagnostic")
    require_token(base_cpp, "persisted_hostname_invalid", "invalid persisted hostname diagnostic")
    require_token(readme, 'ESP8266BASE_WEB_AUTH_PASS=\\"admin\\"', "README build flag default")
    require_token(readme, 'ESP8266BASE_DEFAULT_HOSTNAME=\\"esp8266base-full\\"', "README hostname build flag")
    require_token(overview, 'ESP8266BASE_WEB_AUTH_PASS=\\"admin\\"', "overview build flag default")
    require_token(overview, 'ESP8266BASE_DEFAULT_HOSTNAME=\\"esp8266base-full\\"', "overview hostname build flag")
    require_token(readme, "/wifi` GET 表单也会回显已保存密码", "README plaintext WiFi password echo")
    require_token(readme, "硬件运行时目标", "README free heap target scope")
    require_token(web_doc, "路径字符集", "Web route path charset table")
    require_token(web_doc, "函数返回 `false` 并输出 WARN 日志", "Web invalid route path behavior")
    require_token(web_doc, "Web Auth 改密成功和失败路径都会", "Web Auth plaintext change logs")
    require_token(web_doc, "upload_progress", "Web OTA progress log doc")
    require_token(web_doc, "average_speed", "Web OTA speed log doc")
    require_token(api, "upload_finished", "API OTA finish log doc")
    require_token(api, "average_speed", "API OTA speed log doc")
    require_token(memory, "它不是运行时 free heap 实测", "memory build RAM scope")
    require_token(memory, "Esp8266BaseOTA | <= 160B", "OTA memory budget with diagnostics")
    require_token(memory, "Esp8266BaseWeb | <= 1.20KB", "memory Web budget")
    require_token(memory, "核心裁剪目标（自有）", "memory core profile budget")
    require_token(memory, "全模块默认目标（自有）", "memory full default profile budget")
    require_token(memory, "全模块 INFO FileLog 目标（自有）", "memory full INFO FileLog profile budget")
    if "**< 2.5KB**" in memory or "控制在 2.5KB" in read("AGENTS.md"):
        fail("memory budget must not keep the obsolete single <2.5KB full-library target")

    if "| LittleFS | ~2-3KB |" in architecture or "| Arduino Core | ~3-4KB |" in architecture:
        fail("architecture doc must not keep orphan RAM table rows")
    if "事件总线 | 动态订阅" in architecture:
        fail("architecture doc must not duplicate event bus non-goal rows")


def test_web_home_contract() -> None:
    web_h = read("src/Esp8266BaseWeb.h")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    base_h = read("src/Esp8266Base.h")
    base_cpp = read("src/Esp8266Base.cpp")
    wifi_h = read("src/Esp8266BaseWiFi.h")
    api = read("docs/03_api_reference.md")
    web_doc = read("docs/06_web_ota.md")
    user_guide = read("docs/00_user_guide.md")
    architecture = read("docs/02_architecture.md")
    observability = read("docs/07_observability.md")
    maintainer = read("docs/11_maintainer_guide.md")
    memory = read("docs/04_memory_budget.md")
    custom_web = read("examples/custom_web/src/main.cpp")
    full_demo = read("examples/full_demo/src/main.cpp")

    for token in [
        "setSystemInfo",
        "_formatDuration",
        "Boot time",
        "Connection",
        "Runtime",
        "Firmware",
        "Time",
        "margin:0 auto",
        "%Y-%m-%d %H:%M:%S",
    ]:
        if token not in web_cpp and token not in web_h:
            fail(f"missing Web home contract token: {token}")

    for token in ["ssid()", "rssi()", "macAddressTo"]:
        if token not in wifi_h or token not in api:
            fail(f"missing WiFi home query API token: {token}")

    if "setTitle" in web_h or "setTitle" in api:
        fail("old Web title-only API must not remain")
    for text, label in [(base_h, "base_h"), (base_cpp, "base_cpp"), (web_h, "web_h"),
                        (web_cpp, "web_cpp"), (api, "api"), (web_doc, "web_doc"),
                        (custom_web, "custom_web"), (full_demo, "full_demo")]:
        if "setHostname(" in text:
            fail(f"setHostname must not remain in {label}")
    if "系统首页轻量分组展示当前设备状态" not in web_doc:
        fail("Web doc must describe system home information groups")

    require_token(web_cpp, '"Status", "Logs", "System"', "default Web nav labels")
    require_token(web_cpp, "Password<input id=wp type=password name=pass maxlength=63 value=", "WiFi password optional form")
    if "Password cannot be empty" in web_cpp or "missing_password" in web_cpp:
        fail("WiFi Web form must allow empty password for open networks")
    require_token(web_doc, "密码可为空以连接开放网络", "Web WiFi open network doc")
    require_token(web_cpp, 'max-width:920px', "Web home wider card layout")
    require_token(web_cpp, 'grid-template-columns:repeat(auto-fit,minmax(240px,1fr))', "Web home card min width")
    require_token(web_cpp, 'grid-template-columns:104px minmax(0,1fr)', "Web status label column width")
    require_token(web_cpp, 'white-space:nowrap', "Web status label no-wrap")
    require_token(web_cpp, '_sendKv("Hostname", _hostname)', "Web home hostname field")
    if "_wb" in web_cpp or "_wb" in web_h or "_wb" in memory:
        fail("Web must not keep the old shared _wb buffer")
    require_token(web_cpp, '_sendKv("STA MAC", mac)', "Web home STA MAC field")
    if web_cpp.count('"%d dBm"') < 2:
        fail("Web status card and footer RSSI must both include dBm")
    require_token(web_cpp, '_sendKv("Restart count", bootCount)', "Web home restart count label")
    require_token(web_cpp, '_sendKv("Free heap", freeHeap)', "Web home free heap field")
    require_token(web_cpp, '_sendKv("Max block", maxBlock)', "Web home max block field")
    require_token(web_cpp, '"%lu since clear"', "Web home watchdog reset clear scope")
    require_token(web_cpp, '_sendKv("WDT resets", wdtResets)', "Web home watchdog reset field")
    require_token(web_cpp, '_sendKv("Wake reason", _wakeReasonText(Esp8266BaseSleep::wakeReason()))', "Web home wake reason field")
    require_token(web_cpp, 'ESP.getChipId()', "Web home chip id source")
    require_token(web_cpp, '"ESP8266-%06X"', "Web home chip id format")
    require_token(web_cpp, 'ESP.getCpuFreqMHz()', "Web home CPU frequency")
    require_token(web_cpp, 'ESP.getFlashChipRealSize()', "Web home flash size")
    require_token(web_cpp, 'ESP.getSketchSize()', "Web home sketch size")
    require_token(web_cpp, 'ESP.getFreeSketchSpace()', "Web home OTA free space")
    require_token(web_cpp, '_sendKv("Chip ID", chipId)', "Web home chip id field")
    require_token(web_cpp, '_sendKv("CPU", cpuFreq)', "Web home CPU field")
    require_token(web_cpp, '_sendKv("Flash", flashSize)', "Web home flash field")
    require_token(web_cpp, '_sendKv("Sketch", sketchSize)', "Web home sketch field")
    require_token(web_cpp, '_sendKv("OTA free", otaFree)', "Web home OTA free field")
    require_token(web_cpp, "_formatFooterUptime", "Web footer compact uptime formatter")
    require_token(web_cpp, "Free heap: ", "Web footer keeps Free heap label")
    require_token(web_cpp, "&middot; Up: ", "Web footer compact Up label")
    require_token(web_cpp, "&middot; RSSI: ", "Web footer compact RSSI label")
    require_token(web_cpp, "<h2>System</h2>", "System page heading")
    require_token(web_cpp, "_sendLink(\"/logs\", _builtinLabel(Esp8266BaseWebBuiltinLabel::LOGS)", "Logs outer system nav")
    require_token(web_cpp, "_sendLink(\"/system\", _builtinLabel(Esp8266BaseWebBuiltinLabel::SYSTEM)", "System outer nav")
    require_token(web_cpp, "<p><a href='/wifi'>WiFi Settings</a></p>", "System WiFi entry")
    require_token(web_cpp, "<p><a href='/auth'>Auth Password</a></p>", "System Auth entry")
    require_token(web_cpp, "<p><a href='/ota'>OTA Update</a></p>", "System OTA entry")
    require_token(web_cpp, '_server.on("/system/hostname", HTTP_POST, _handleHostnamePost);',
                  "hostname System POST route")
    require_token(web_cpp, '_server.on("/api/system/hostname", HTTP_GET, _handleHostnameApiGet);',
                  "hostname API GET route")
    require_token(web_cpp, '_server.on("/api/system/hostname", HTTP_POST, _handleHostnameApiPost);',
                  "hostname API POST route")
    require_token(web_cpp, "Hostname saved. Reboot to apply network discovery changes.",
                  "hostname reboot notice")
    require_token(web_cpp, "invalid_hostname", "hostname API invalid error")
    require_token(web_doc, "/api/system/hostname", "hostname API doc")
    require_token(api, "/system/hostname", "hostname System POST doc")
    require_token(web_cpp, "Clear File Logs", "System page log clear action")
    require_token(web_cpp, "_redirect(ok ? \"/system?cleared=1\" : \"/system?error=clear_failed\")",
                  "log clear returns to System page")
    not_found_start = web_cpp.index("void Esp8266BaseWeb::_handleNotFound()")
    not_found_end = web_cpp.index("\n}\n#endif", not_found_start)
    not_found = web_cpp[not_found_start:not_found_end]
    favicon_check = not_found.index('uri == "/favicon.ico"')
    auth_check = not_found.index("if (!checkAuth()) return;")
    not_found_response = not_found.index('_server.send(404, "text/plain", "Not found")')
    if not favicon_check < auth_check < not_found_response:
        fail("only favicon probes may bypass 404 Basic Auth")
    require_token(web_cpp, '_server.send(401, "application/json"', "hostname API JSON 401")
    require_token(web_cpp, '\\"error\\":\\"unauthorized\\"', "hostname API unauthorized JSON body")
    require_token(web_doc, "未认证时返回 JSON 401", "Web JSON API auth policy")
    require_token(api, "未知路径认证通过后返回 404", "API doc 404 auth policy")
    require_token(web_cpp, "addPage_rejected reason=invalid_path path=%s count=%u max=%u",
                  "Web addPage diagnostic rejection")
    require_token(web_cpp, "addPage_rejected reason=web_not_running", "Web addPage before begin rejection")
    require_token(web_cpp, "addApi_rejected reason=web_not_running", "Web addApi before begin rejection")
    require_token(web_cpp, "addApi_rejected reason=table_full path=%s count=%u max=%u",
                  "Web addApi table full diagnostic")
    require_token(web_cpp, "#if ESP8266BASE_USE_OTA", "OTA page/System entry compile guard")
    require_token(web_cpp, '_server.on("/ota",    HTTP_GET,  _handleOtaGet);', "OTA GET route")
    require_token(api, "`Status/Logs/System`", "API built-in nav label list")
    require_token(web_doc, "Connection | Hostname、WiFi 状态、SSID、IP、RSSI(dBm)、STA MAC",
                  "Web doc Connection fields")
    require_token(web_doc, "Runtime | Free heap、Max block、Restart count、Watchdog resets、Wake reason",
                  "Web doc Runtime fields")
    require_token(web_doc, "`N since clear`", "Web doc watchdog reset clear scope")
    require_token(web_doc, "Firmware | Firmware、Version、Chip ID、CPU、Flash、Sketch、OTA free",
                  "Web doc Firmware fields")
    require_token(api, "ESP8266-XXXXXX", "API chip id display format")
    require_token(api, "仅 `ESP8266BASE_USE_OTA=1` 时注册", "API OTA route guard doc")
    require_token(web_doc, "不会注册 `/ota` 页面、System 页面 OTA 入口或上传 POST 路由", "Web OTA disabled route doc")
    if '_sendKv("Chip", "ESP8266")' in web_cpp:
        fail("Web home must not show a fixed Chip value")
    require_token(api, "入口在 System 页面", "API log clear location")
    require_token(web_doc, "入口在 System 页面", "Web doc log clear location")
    require_token(web_cpp, "content='width=device-width,initial-scale=1'", "Web mobile viewport meta")
    require_token(web_cpp, "@media(max-width:700px)", "Web footer compact mobile media query")
    require_token(web_cpp, "footer .tools{flex:1 0 100%", "Web footer compact mobile tools row")
    require_token(web_cpp, "footer .status{margin-left:0;white-space:normal", "Web footer compact mobile status row")
    require_token(api, "窄屏下切换为两行左对齐面板", "API footer compact mobile doc")
    require_token(web_doc, "状态信息独占第二行并左对齐", "Web footer compact mobile doc")
    require_token(user_guide, "入口在 System 页面", "user guide log clear location")
    require_token(architecture, "入口在 System 页面", "architecture log clear location")
    require_token(observability, "System 页面中的清除文件日志按钮", "observability log clear location")
    require_token(maintainer, "System 页面可通过 `/logs/clear` 清空日志", "maintainer log clear location")
    if "Clear Log" in web_cpp:
        fail("Logs page must not keep the old Clear Log action")
    if "GET  /reboot" in architecture or "| `/reboot` | GET" in api or "| `/reboot` | GET" in web_doc:
        fail("GET /reboot must not remain; System page is GET /system")
    for text, label in [(web_cpp, "web_cpp"), (web_h, "web_h"), (api, "api"), (web_doc, "web_doc")]:
        for old in ["Esp8266BaseWebBuiltinLabel::WIFI", "Esp8266BaseWebBuiltinLabel::OTA",
                    "Esp8266BaseWebBuiltinLabel::AUTH", "Esp8266BaseWebBuiltinLabel::REBOOT",
                    "/reboot/filelog", "<h2>Tools</h2>"]:
            if old in text:
                fail(f"{old} must not remain in {label}")
    wifi_cpp = read("src/Esp8266BaseWiFi.cpp")
    config_h = read("src/Esp8266BaseConfig.h")
    config_doc = read("docs/05_config_storage.md")
    networking = read("docs/08_networking.md")
    for text, label in [(wifi_h, "wifi_h"), (wifi_cpp, "wifi_cpp"), (config_h, "config_h"),
                        (config_doc, "config_doc"), (networking, "networking"), (api, "api")]:
        for old in ["Esp8266BaseWiFiState::FAILED", "ESP8266BASE_CFG_KEY_AP_PASS", "eb_ap_pass"]:
            if old in text:
                fail(f"{old} must not remain in {label}")
    if "FAILED" in wifi_h:
        fail("unreachable WiFi FAILED state must not remain")
    require_token(web_doc, 'Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::HOME, "Status");',
                  "Web doc Status nav label")
    require_token(web_doc, 'Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::SYSTEM, "System");',
                  "Web doc System nav label")
    for text, label in [(custom_web, "custom_web"), (full_demo, "full_demo")]:
        require_token(text, 'Esp8266BaseWebBuiltinLabel::SYSTEM, "System"', f"{label} System nav label")
    if "String name = dir.fileName()" in full_demo:
        fail("full_demo config table must not keep a local String filename")
    if "char body[420]" in full_demo:
        fail("full_demo deep sleep response must not build a full HTML page in a stack buffer")


def main() -> None:
    test_format_bytes()
    test_log_file_buffer_rules()
    test_wifi_retry_rules()
    test_ntp_manual_packet_validation()
    test_config_deferred_rules()
    test_restart_count_skips_deep_sleep_wake()
    test_log_segment_paths()
    test_ota_header_guard()
    test_boot_session_log_contract()
    test_web_auth_contract()
    test_watchdog_and_ota_failure_contract()
    test_fixed_mqtt_terminal_and_ota_lifecycle_contract()
    test_public_default_tables()
    test_web_home_contract()
    print("[logic] ok")


if __name__ == "__main__":
    main()
