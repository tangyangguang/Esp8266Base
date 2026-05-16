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
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_MODE_WARN"), 2, "filelog WARN mode")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_MODE_INFO"), 1, "filelog INFO mode")
    assert_eq(parse_define_int(filelog_h, "ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS"), 2000, "file flush interval")
    if "ESP8266BASE_FILELOG_DEFAULT_MODE == ESP8266BASE_FILELOG_MODE_INFO" not in filelog_h:
        fail("file buffer default must depend on FileLog default mode INFO")
    assert_eq(file_buffer_size_for_mode(1), 512, "INFO file buffer")
    assert_eq(file_buffer_size_for_mode(2), 0, "WARN file buffer")
    assert_eq(file_buffer_size_for_mode(4), 0, "OFF file buffer")
    assert_eq(file_buffer_size_for_mode(2, explicit_size=0), 0, "explicit disabled buffer")
    if "setRuntimeLevel" not in log_h or "setSerialLevel" not in log_h:
        fail("Log must split runtime and serial levels")
    if "Esp8266BaseLog::_setInternalHook(_lineSink)" not in filelog_cpp:
        fail("FileLog must register an internal log sink")
    if "eb_log.mode" not in filelog_cpp:
        fail("FileLog mode must persist to eb_log.mode")
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
    assert_eq(fast, 2000, "default fast retry")
    assert_eq(fast_count, 3, "default fast retry count")
    assert_eq(slow, 60000, "default slow retry")
    assert_eq(stuck, 7000, "default stuck disconnected restart")
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
    require_token(wifi_cpp, "reason=ssid_too_long", "WiFi SSID length validation")
    require_token(wifi_cpp, "reason=password_too_long", "WiFi password length validation")
    require_token(wifi_cpp, "max=32", "WiFi SSID length limit log")
    require_token(wifi_cpp, "max=63", "WiFi password length limit log")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
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
        "boot_session_start boot_count=%lu",
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
        if "setAuth(" in read(path):
            fail(f"old setAuth API must not remain in {path}")

    required = [
        (web_h, "setDefaultAuth"),
        (web_h, "SYSTEM = 2"),
        (web_cpp, "_server.on(\"/auth\""),
        (web_cpp, "<p><a href='/auth'>Auth Password</a></p>"),
        (web_cpp, "_handleAuthGet"),
        (web_cpp, "_handleAuthPost"),
        (web_cpp, "ESP8266BASE_CFG_KEY_WEB_PASS"),
        (web_cpp, "web_password_updated"),
        (api, "setDefaultAuth"),
        (api, "`/auth`"),
        (web_doc, "认证配置分三层"),
        (changelog, "setDefaultAuth"),
    ]
    for text, token in required:
        if token not in text:
            fail(f"missing Web Auth contract token: {token}")


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
    require_token(ota_cpp, '_failUpload(500, "Update failed: write failed", true)',
                  "OTA write failure immediate closeout")


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
    require_token(memory, "Esp8266BaseWeb | <= 1.36KB", "memory Web budget")
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
    if re.search(r"_sendKv\([^\n]+_wb", web_cpp):
        fail("Web status fields must not pass shared _wb as a value buffer")
    require_token(web_cpp, '_sendKv("STA MAC", mac)', "Web home STA MAC field")
    if web_cpp.count('"%d dBm"') < 2:
        fail("Web status card and footer RSSI must both include dBm")
    require_token(web_cpp, '_sendKv("Boot count", bootCount)', "Web home boot count label")
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
    require_token(web_cpp, "void Esp8266BaseWeb::_handleNotFound() {\n    _markRequest();\n    if (!checkAuth()) return;",
                  "404 requires Basic Auth")
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
    require_token(web_doc, "Runtime | Free heap、Max block、Boot count、Watchdog resets、Wake reason",
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
    test_log_segment_paths()
    test_ota_header_guard()
    test_boot_session_log_contract()
    test_web_auth_contract()
    test_watchdog_and_ota_failure_contract()
    test_public_default_tables()
    test_web_home_contract()
    print("[logic] ok")


if __name__ == "__main__":
    main()
