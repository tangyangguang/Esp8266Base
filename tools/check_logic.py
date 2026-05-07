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
        v10 = (value * 10 + 512) // 1024
        return f"{v10 // 10}.{v10 % 10} KB"
    v10 = (value * 10 + 524288) // 1048576
    return f"{v10 // 10}.{v10 % 10} MB"


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


def test_format_bytes() -> None:
    cases = {
        0: "0 B",
        1: "1 B",
        1023: "1023 B",
        1024: "1.0 KB",
        1536: "1.5 KB",
        16384: "16.0 KB",
        1048575: "1024.0 KB",
        1048576: "1.0 MB",
        1572864: "1.5 MB",
    }
    for value, expected in cases.items():
        assert_eq(format_bytes(value), expected, f"formatBytes({value})")


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


def main() -> None:
    test_format_bytes()
    test_log_file_buffer_rules()
    test_wifi_retry_rules()
    test_config_deferred_rules()
    test_log_segment_paths()
    test_boot_session_log_contract()
    test_web_auth_contract()
    print("[logic] ok")


if __name__ == "__main__":
    main()
