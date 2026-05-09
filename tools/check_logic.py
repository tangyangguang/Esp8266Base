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


def file_buffer_size_for_level(file_level: int, explicit_size: int | None = None) -> int:
    if explicit_size is not None:
        if explicit_size > 512:
            raise ValueError("buffer size must be <= 512")
        return explicit_size
    return 512 if file_level < 2 else 0


def retry_interval(attempt: int, fast_count: int, fast_ms: int, slow_ms: int) -> int:
    return fast_ms if attempt <= fast_count else slow_ms


def log_segment_path(base: str, index: int) -> str:
    return base if index == 0 else f"{base}.{index}"


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
    log_cpp = read("src/Esp8266BaseLog.cpp")
    assert_eq(parse_define_int(log_h, "ESP8266BASE_LOG_FILE_LEVEL"), 2, "default file log level")
    assert_eq(parse_define_int(log_h, "ESP8266BASE_LOG_FILE_FLUSH_INTERVAL_MS"), 2000, "file flush interval")
    if "#if ESP8266BASE_LOG_FILE_LEVEL < 2" not in log_h:
        fail("file buffer default must depend on file level < WARN")
    assert_eq(file_buffer_size_for_level(0), 512, "DEBUG file buffer")
    assert_eq(file_buffer_size_for_level(1), 512, "INFO file buffer")
    assert_eq(file_buffer_size_for_level(2), 0, "WARN file buffer")
    assert_eq(file_buffer_size_for_level(3), 0, "ERROR file buffer")
    assert_eq(file_buffer_size_for_level(2, explicit_size=0), 0, "explicit disabled buffer")
    if "file_sink_buffer low_priority=%s" not in log_cpp:
        fail("file sink buffer details must be logged on a separate line")


def test_wifi_retry_rules() -> None:
    wifi_h = read("src/Esp8266BaseWiFi.h")
    wifi_cpp = read("src/Esp8266BaseWiFi.cpp")
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


def test_config_deferred_rules() -> None:
    config_h = read("src/Esp8266BaseConfig.h")
    assert_eq(parse_define_int(config_h, "ESP8266BASE_CFG_DEFERRED_SIZE"), 4, "default deferred queue size")
    assert_eq(parse_define_int(config_h, "ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS"), 5000,
              "default deferred flush interval")


def test_log_segment_paths() -> None:
    assert_eq([log_segment_path("/logs/app.log", i) for i in range(4)],
              ["/logs/app.log", "/logs/app.log.1", "/logs/app.log.2", "/logs/app.log.3"],
              "log rotation paths")


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
        (web_h, "AUTH = 4"),
        (web_cpp, "_server.on(\"/auth\""),
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


def test_public_default_tables() -> None:
    readme = read("README.md")
    api = read("docs/03_api_reference.md")
    overview = read("docs/01_overview.md")
    web_doc = read("docs/06_web_ota.md")
    architecture = read("docs/02_architecture.md")
    memory = read("docs/04_memory_budget.md")

    for text, label in [(readme, "README"), (api, "API reference"), (overview, "overview")]:
        require_token(text, '| `ESP8266BASE_WEB_AUTH_PASS` | `"admin"` |', f"{label} Web Auth default")
        require_token(text, '| `ESP8266BASE_WIFI_RETRY_FAST` | `2000` |', f"{label} WiFi fast retry default")
        require_token(text, '| `ESP8266BASE_USE_OTA` | `0` | 编译 OTA；要求 `ESP8266BASE_USE_WEB=1` |',
                      f"{label} OTA/Web dependency")

    require_token(readme, 'ESP8266BASE_WEB_AUTH_PASS=\\"admin\\"', "README build flag default")
    require_token(overview, 'ESP8266BASE_WEB_AUTH_PASS=\\"admin\\"', "overview build flag default")
    require_token(readme, "/wifi` GET 表单也会回显已保存密码", "README plaintext WiFi password echo")
    require_token(readme, "硬件运行时目标", "README free heap target scope")
    require_token(web_doc, "路径字符集", "Web route path charset table")
    require_token(web_doc, "addPage()` / `addApi()` 返回 `false`", "Web invalid route path behavior")
    require_token(web_doc, "Web Auth 改密成功和失败路径都会", "Web Auth plaintext change logs")
    require_token(web_doc, "upload_progress", "Web OTA progress log doc")
    require_token(web_doc, "average_speed", "Web OTA speed log doc")
    require_token(api, "upload_finished", "API OTA finish log doc")
    require_token(api, "average_speed", "API OTA speed log doc")
    require_token(memory, "它不是运行时 free heap 实测", "memory build RAM scope")
    require_token(memory, "Esp8266BaseOTA | <= 160B", "OTA memory budget with diagnostics")

    if "| LittleFS | ~2-3KB |" in architecture or "| Arduino Core | ~3-4KB |" in architecture:
        fail("architecture doc must not keep orphan RAM table rows")
    if "事件总线 | 动态订阅" in architecture:
        fail("architecture doc must not duplicate event bus non-goal rows")


def test_web_home_contract() -> None:
    web_h = read("src/Esp8266BaseWeb.h")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
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
        "Network",
        "Device",
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
    if "系统首页以轻量分组展示" not in web_doc:
        fail("Web doc must describe system home information groups")

    require_token(web_cpp, '"Status", "WiFi", "OTA", "Logs", "Auth", "Tools"', "default Web nav labels")
    require_token(web_cpp, 'max-width:920px', "Web home wider card layout")
    require_token(web_cpp, 'grid-template-columns:repeat(auto-fit,minmax(240px,1fr))', "Web home card min width")
    require_token(web_cpp, '_sendKv("Hostname", _hostname)', "Web home hostname field")
    require_token(web_cpp, '_sendKv("Boot count", bootCount)', "Web home boot count label")
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
    require_token(web_cpp, "<h2>Tools</h2>", "Tools page heading")
    require_token(web_cpp, "Clear File Logs", "Tools page log clear action")
    require_token(web_cpp, "_redirect(ok ? \"/reboot?cleared=1\" : \"/reboot?error=clear_failed\")",
                  "log clear returns to Tools page")
    require_token(web_cpp, "#if ESP8266BASE_USE_OTA", "OTA page/nav compile guard")
    require_token(web_cpp, '_sendLink("/ota"', "OTA nav link")
    require_token(web_cpp, '_server.on("/ota",    HTTP_GET,  _handleOtaGet);', "OTA GET route")
    require_token(api, "`Status/WiFi/OTA/Logs/Auth/Tools`", "API built-in nav label list")
    require_token(web_doc, "Hostname、WiFi 状态、SSID、IP、RSSI、MAC", "Web doc Network hostname fields")
    require_token(web_doc, "Boot count、Chip ID、CPU、Flash、Sketch、OTA free",
                  "Web doc Device card hardware fields")
    require_token(api, "ESP8266-XXXXXX", "API chip id display format")
    require_token(api, "仅 `ESP8266BASE_USE_OTA=1` 时注册", "API OTA route guard doc")
    require_token(web_doc, "不会注册 `/ota` 页面和导航入口", "Web OTA disabled route doc")
    if '_sendKv("Chip", "ESP8266")' in web_cpp:
        fail("Web home must not show a fixed Chip value")
    require_token(api, "入口在 Tools 页面", "API log clear location")
    require_token(web_doc, "入口在 Tools 页面", "Web doc log clear location")
    require_token(user_guide, "入口在 Tools 页面", "user guide log clear location")
    require_token(architecture, "入口在 Tools 页面", "architecture log clear location")
    require_token(observability, "Tools 页面中的清除文件日志按钮", "observability log clear location")
    require_token(maintainer, "Tools 页面可通过 `/logs/clear` 清空日志", "maintainer log clear location")
    if "Clear Log" in web_cpp:
        fail("Logs page must not keep the old Clear Log action")
    if "重启确认页" in api or "重启确认页" in web_doc or "重启确认" in user_guide:
        fail("docs must describe /reboot as Tools, not the old reboot-only page")
    require_token(web_doc, 'Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::HOME, "Status");',
                  "Web doc Status nav label")
    require_token(web_doc, 'Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::AUTH, "Auth");',
                  "Web doc Auth nav label")
    require_token(web_doc, 'Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::REBOOT, "Tools");',
                  "Web doc Tools nav label")
    for text, label in [(custom_web, "custom_web"), (full_demo, "full_demo")]:
        if 'Esp8266BaseWebBuiltinLabel::HOME, "System"' in text:
            fail(f"{label} must not use old System nav label")
        if 'Esp8266BaseWebBuiltinLabel::REBOOT, "Restart"' in text:
            fail(f"{label} must not use old Restart nav label")


def main() -> None:
    test_format_bytes()
    test_log_file_buffer_rules()
    test_wifi_retry_rules()
    test_config_deferred_rules()
    test_log_segment_paths()
    test_boot_session_log_contract()
    test_web_auth_contract()
    test_watchdog_and_ota_failure_contract()
    test_public_default_tables()
    test_web_home_contract()
    print("[logic] ok")


if __name__ == "__main__":
    main()
