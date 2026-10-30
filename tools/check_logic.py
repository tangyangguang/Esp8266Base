#!/usr/bin/env python3
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


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


def format_quantized_heap_kib(units: int) -> str:
    heap_bytes = units << 6
    centi_kib = (heap_bytes * 100 + 512) // 1024
    return f"{centi_kib // 100}.{centi_kib % 100:02d}KiB"


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


def parse_constexpr_int(text: str, name: str) -> int:
    m = re.search(rf"constexpr\s+\w+\s+{re.escape(name)}\s*=\s*(\d+)", text)
    if not m:
        fail(f"missing constexpr integer: {name}")
    return int(m.group(1))


def require_token(text: str, token: str, label: str) -> None:
    if token not in text:
        fail(f"missing {label}: {token}")


def forbid_token(text: str, token: str, label: str) -> None:
    if token in text:
        fail(f"forbidden {label}: {token}")


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


def test_journal_heap_precision() -> None:
    assert_eq(format_quantized_heap_kib(112), "7.00KiB", "journal heap 7 KiB")
    assert_eq(format_quantized_heap_kib(117), "7.31KiB", "journal heap 7.31 KiB")
    assert_eq(format_quantized_heap_kib(127), "7.94KiB", "journal heap 7.94 KiB")
    journal_cpp = read("src/Esp8266BaseJournal.cpp")
    require_token(journal_cpp, "heap_min~%lu.%02luKiB", "quantized journal heap precision")
    forbid_token(journal_cpp, "heap_min=%luk", "integer-truncated journal heap text")


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
    core_h = read("src/Esp8266BaseLinkRecoveryCore.h")
    networking = read("docs/08_networking.md")
    api = read("docs/03_api_reference.md")
    fast = parse_define_int(wifi_h, "ESP8266BASE_WIFI_RETRY_FAST")
    fast_count = parse_define_int(wifi_h, "ESP8266BASE_WIFI_RETRY_FAST_COUNT")
    slow = parse_define_int(wifi_h, "ESP8266BASE_WIFI_RETRY_SLOW")
    stuck = parse_define_int(wifi_h, "ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS")
    radio_reset_settle = parse_define_int(
        wifi_h, "ESP8266BASE_WIFI_RADIO_RESET_SETTLE_MS")
    radio_reset_failures = parse_constexpr_int(
        core_h, "RADIO_RESET_FAILURE_THRESHOLD")
    radio_reset_interval = parse_constexpr_int(
        core_h, "RADIO_RESET_MIN_INTERVAL_MS")
    radio_reset_budget = parse_constexpr_int(core_h, "RADIO_RESET_WINDOW_BUDGET")
    local_after = parse_constexpr_int(core_h, "LINK_RESTART_AFTER_MS")
    local_resets = parse_constexpr_int(core_h, "LINK_RESTART_MIN_RADIO_RESETS")
    assert_eq(fast, 2000, "default fast retry")
    assert_eq(fast_count, 3, "default fast retry count")
    assert_eq(slow, 10000, "default slow retry")
    assert_eq(stuck, 7000, "default stuck disconnected restart")
    assert_eq(radio_reset_failures, 6, "default WiFi radio reset failure threshold")
    assert_eq(radio_reset_interval, 600000, "default WiFi radio reset min interval")
    assert_eq(radio_reset_budget, 3, "default WiFi radio reset window budget")
    assert_eq(radio_reset_settle, 100, "default WiFi radio reset settle")
    assert_eq(local_after, 1800000, "default local-link MCU recovery after")
    assert_eq(local_resets, 2, "default local-link MCU recovery radio resets")
    assert_eq([retry_interval(i, fast_count, fast, slow) for i in range(1, 6)],
              [2000, 2000, 2000, 10000, 10000],
              "retry interval sequence")
    # 时间基：新节奏下攒满阶梯所需的 2 次 radio reset 的最坏耗时必须小于本地阶梯
    # 时长（30min），否则真失联时阶梯永不触发。首次 reset 最坏约一组失败窗，第二次
    # 必须等 10min 绝对冷却 + 最多一轮慢重试。
    connect_timeout = parse_define_int(
        wifi_h, "ESP8266BASE_WIFI_CONNECT_TIMEOUT")
    first_reset_worst = 2 * connect_timeout + 3 * fast + 2 * slow + 500
    worst_two_resets = first_reset_worst + radio_reset_interval + slow
    if worst_two_resets >= local_after:
        fail("two radio resets worst-case (%dms) must be < local MCU recovery (%dms)"
             % (worst_two_resets, local_after))
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
    # radio reset 资格必须在 WiFi 层重试调度内、每次失败重新评估；冷却/预算不合格
    # 时走普通慢重试布臂，不能提前 return（否则退化成无间隔重关联）。
    require_token(wifi_cpp, "_radioGate.onFailure(now)",
                  "radio reset eligibility evaluated inside the retry scheduler")
    require_token(core_h, "RADIO_RESET_MIN_INTERVAL_MS",
                  "radio reset cadence enforced by the pure core")
    require_token(core_h, "RADIO_RESET_WINDOW_BUDGET",
                  "radio reset per-window budget in the pure core")
    sched = wifi_cpp[wifi_cpp.index("void Esp8266BaseWiFi::_scheduleRetry()"):]
    gate_pos = sched.index("_radioGate.onFailure(now)")
    arm_pos = sched.index("_connectStart = 0;", gate_pos)
    if not (sched.index("if (_radioGate.onFailure(now))") < arm_pos):
        fail("radio reset gate must be evaluated before the normal slow-retry arm")
    # radio reset 分层：WiFi 层只请求，主类消费并在有序停网后物理执行。
    require_token(wifi_cpp, "requestRadioReset",
                  "WiFi local failures must request (not directly perform) radio reset")
    require_token(wifi_cpp, "executeRadioReset",
                  "physical radio reset must be a single owner-gated entrypoint")
    if "recoverStation" in wifi_cpp or "recoverStation" in read("src/Esp8266BaseWiFi.h"):
        fail("upper-layer recoverStation() must be removed; radio reset is owner-orchestrated")
    mqtt_cpp = read("src/Esp8266BaseMQTT.cpp")
    mqtt_code = "\n".join(
        line.split("//", 1)[0] for line in mqtt_cpp.splitlines())
    if ("recoverStation" in mqtt_code or "WiFi.mode(" in mqtt_code
            or "requestRadioReset" in mqtt_code):
        fail("MQTT transport failures must never tear down or request the WiFi radio")
    wifi_h_text = read("src/Esp8266BaseWiFi.h")
    for upper in ("Esp8266BaseMQTT.h", "Esp8266BaseNTP.h", "Esp8266BaseWeb.h", "Esp8266BaseOTA.h"):
        if ('#include "%s"' % upper) in wifi_cpp or ('#include "%s"' % upper) in wifi_h_text:
            fail("Esp8266BaseWiFi must not include upper-layer module %s" % upper)
    base_cpp = read("src/Esp8266Base.cpp")
    require_token(base_cpp, "consumeRadioResetRequest",
                  "owner must consume radio reset requests")
    require_token(base_cpp, "executeRadioReset",
                  "owner must perform the physical radio reset")
    # 有序停网顺序：mDNS 自持 UdpContext → Web listen → 其余 UDP → TCP → WIFI_OFF。
    def _pos(token, after=0):
        i = base_cpp.find(token, after)
        if i < 0:
            fail("ordered teardown missing token: %s" % token)
        return i
    p_mdns = _pos("Esp8266BaseMDNS::stop();")
    p_web = _pos("suspendForRadioReset()", p_mdns)
    p_udp = _pos("WiFiUDP::stopAll()", p_web)
    p_tcp = _pos("WiFiClient::stopAll()", p_udp)
    p_exec = _pos("executeRadioReset()", p_tcp)
    if not (p_mdns < p_web < p_udp < p_tcp < p_exec):
        fail("radio reset must stop mDNS, web, UDP, TCP in order before WIFI_OFF")
    require_token(read("src/Esp8266BaseMDNS.cpp"), "MDNS.end()",
                  "mDNS must release its self-owned UdpContext before radio reset")
    require_token(base_cpp, "_handleLocalLinkRecovery",
                  "owner must run the local-link outage MCU ladder")
    if "ESP.restart()" in wifi_cpp:
        fail("WiFi recovery must not reboot the MCU")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    require_token(web_cpp, 'json.number("wifiAttempt", Esp8266BaseWiFi::attemptCount());', "health WiFi attempt evidence")
    require_token(web_cpp, 'json.number("wifiRadioReset", Esp8266BaseWiFi::radioResetCount());', "health WiFi radio reset evidence")
    require_token(networking, "不重启 MCU、不清除 Config、不进入 AP",
                  "bounded WiFi radio recovery contract")
    require_token(wifi_cpp, "_isDue(now, _retryAt)", "wrap-safe WiFi retry deadline")
    require_token(wifi_cpp, "static_cast<int32_t>(now - due) >= 0",
                  "WiFi retry signed delta comparison")
    if millis_due(0xFFFFFFF0, 0x00000010) or not millis_due(0x00000020, 0x00000010):
        fail("WiFi retry deadline must remain correct across millis wrap")
    require_token(wifi_cpp, "reason=ssid_too_long", "WiFi SSID length validation")
    require_token(wifi_cpp, "reason=password_too_long", "WiFi password length validation")
    require_token(wifi_cpp, "password=[redacted]", "WiFi password log redaction")
    if "password=%s" in wifi_cpp:
        fail("WiFi logs must never interpolate plaintext passwords")
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

    # 纯核 host 实编译 + 断言（与固件共用同一份 LinkRecoveryCore.h），覆盖 radio reset
    # 冷却/预算与 30min 失联阶梯的全部分支，含 millis 跨 2^32 回绕。
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        fail("no C++ compiler available for link-recovery core host test")
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "link_recovery_core_test"
        compiled = subprocess.run(
            [cxx, "-std=c++17", "-Wall", "-Wextra",
             str(ROOT / "tools/link_recovery_core_host_test.cpp"), "-o", str(binary)],
            capture_output=True, text=True)
        if compiled.returncode != 0:
            fail("link-recovery core host test compile error:\n" + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        if ran.returncode != 0:
            fail("link-recovery core host test failed:\n" + ran.stdout + ran.stderr)


def test_ntp_manual_packet_validation() -> None:
    ntp_cpp = read("src/Esp8266BaseNTP.cpp")
    networking = read("docs/08_networking.md")
    api = read("docs/03_api_reference.md")
    require_token(ntp_cpp, "remoteIp != _serverIp[index]", "NTP response IP validation")
    require_token(ntp_cpp, "remotePort != NTP_PORT", "NTP response port validation")
    require_token(ntp_cpp, "mode != 4", "NTP manual server mode validation")
    require_token(ntp_cpp, "stratum == 0 || stratum > 15", "NTP manual stratum validation")
    require_token(ntp_cpp, "ntp_packet_rejected", "NTP invalid manual packet log")
    require_token(ntp_cpp, "static_cast<int32_t>(now - due) >= 0",
                  "NTP retry signed delta comparison")
    if millis_due(0xFFFFFFF0, 0x00000010) or not millis_due(0x00000020, 0x00000010):
        fail("NTP retry deadline must remain correct across millis wrap")
    require_token(ntp_cpp, "Esp8266BaseNTPInternal::matchesRequest(pkt, _nonce)",
                  "NTP response must match current request")
    require_token(ntp_cpp, "memcpy(pkt + 40, _nonce", "NTP request correlation timestamp")
    poll = ntp_cpp[ntp_cpp.index("bool Esp8266BaseNTP::_pollResponse"):
                   ntp_cpp.index("void Esp8266BaseNTP::_advanceOrFinish")]
    if not poll.index("NTP_RESPONSE_TIMEOUT_MS") < poll.index("parsePacket()"):
        fail("NTP timeout must be checked before parsing a late packet")
    send = ntp_cpp[ntp_cpp.index("bool Esp8266BaseNTP::_requestServer"):
                   ntp_cpp.index("bool Esp8266BaseNTP::_pollResponse")]
    if not send.index("hostByName") < send.index("endPacket()"):
        fail("NTP DNS must resolve before the packet is sent")
    if not send.index("endPacket()") < send.index("_sentMs = millis()"):
        fail("NTP RTT origin must be sampled right after endPacket, excluding DNS")
    require_token(networking, "只接受当前等待服务器", "NTP manual validation doc")
    require_token(api, "校验响应来源 IP、端口、mode、stratum", "API NTP validation doc")


def test_ntp_trust_core() -> None:
    """可信双源对时：结构禁令 + 纯核 host 实编译断言。"""
    core_h = read("src/Esp8266BaseNtpCore.h")
    ntp_cpp = read("src/Esp8266BaseNTP.cpp")

    # 唯一设钟权威：业务路径不得启用 lwIP 内置 SNTP/DHCP 时间源。
    forbid_token(ntp_cpp, "configTime(", "configTime/lwIP SNTP clock source")
    forbid_token(ntp_cpp, "sntp_init()", "sntp_init clock source")
    forbid_token(ntp_cpp, "sntp_setservername(", "lwIP sntp server configuration")
    require_token(ntp_cpp, "sntp_servermode_dhcp(0)", "DHCP option 42 NTP disabled")

    # 纯核必须具备：不同 IP、RTT 门、绝对年份窗、双源一致窗。
    require_token(core_h, "a.serverIp4 == b.serverIp4", "same-server detection")
    require_token(core_h, "RTT_GATE_MS", "RTT gate constant")
    require_token(core_h, "MIN_PLAUSIBLE_UNIX_SEC", "plausibility floor constant")
    require_token(core_h, "normalizeToLaterUs", "common-instant normalization")

    # settimeofday 只允许出现在启动期采集收尾一次；运行期校验严禁改钟。
    acquire = ntp_cpp[ntp_cpp.index("void Esp8266BaseNTP::_finishAcquire"):
                      ntp_cpp.index("void Esp8266BaseNTP::_finishVerify")]
    require_token(acquire, "settimeofday(&tv", "startup-only settimeofday")
    verify = ntp_cpp[ntp_cpp.index("void Esp8266BaseNTP::_finishVerify"):
                     ntp_cpp.index("void Esp8266BaseNTP::_abortCycle")]
    forbid_token(verify, "settimeofday", "runtime clock adjustment")

    # WiFi 断开 reset 不得清除已可信闘锁。
    reset_seg = ntp_cpp[ntp_cpp.index("void Esp8266BaseNTP::reset"):
                        ntp_cpp.index("void Esp8266BaseNTP::_startCycle")]
    forbid_token(reset_seg, "_clockTrusted =", "reset must not clear the trust latch")
    require_token(reset_seg, "不清除 _clockTrusted", "reset latch comment")

    # 在途响应的 poll/超时必须在 1s 空闲节流之前，否则 RTT 被量化到约 1s、撞 500ms 门。
    handle_seg = ntp_cpp[ntp_cpp.index("void Esp8266BaseNTP::handle"):
                        ntp_cpp.index("void Esp8266BaseNTP::reset")]
    if not handle_seg.index("if (_waiting)") < handle_seg.index("_lastCheckMs < 1000UL"):
        fail("in-flight NTP poll must run every loop before the 1s idle throttle")

    # 纯核 host 实编译 + 断言（与固件共用同一份 NtpCore.h）。
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        fail("no C++ compiler available for NTP core host test")
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "ntp_core_test"
        compiled = subprocess.run(
            [cxx, "-std=c++17", "-Wall", "-Wextra",
             str(ROOT / "tools/ntp_core_host_test.cpp"), "-o", str(binary)],
            capture_output=True, text=True)
        if compiled.returncode != 0:
            fail("NTP core host test compile error:\n" + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        if ran.returncode != 0:
            fail("NTP core host test failed:\n" + ran.stdout + ran.stderr)


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
    legacy_parameter = re.compile(
        r"beginBootSession\s*\([^)]*const\s+char\s*\*\s*resetReason",
        re.S,
    )
    if legacy_parameter.search(log_h) or legacy_parameter.search(api):
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
    require_token(web_cpp, "password=[redacted]", "Web credential log redaction")
    forbidden_credential_logs = ["password=%s", "current=%s expected=%s", "new=%s confirm=%s"]
    if any(token in web_cpp for token in forbidden_credential_logs):
        fail("Web logs must never interpolate plaintext passwords")
    wifi_get_start = web_cpp.index("void Esp8266BaseWeb::_handleWiFiGet()")
    wifi_get_end = web_cpp.index("\n}", wifi_get_start)
    if "ESP8266BASE_CFG_KEY_WIFI_PASS" in web_cpp[wifi_get_start:wifi_get_end]:
        fail("WiFi GET must not load or echo the persisted password")

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

    require_token(watchdog_cpp, "system_rtc_mem_write", "Watchdog RTC recovery marker")
    require_token(watchdog_cpp, "os_timer_setfn", "independent SDK timer monitor")
    require_token(watchdog_cpp, "_emergencyStop", "independent emergency-stop callback")
    require_token(watchdog_cpp, "system_restart()", "bounded recovery restart")
    require_token(watchdog_cpp, "MARKER_DENIED", "restart-budget denial evidence")
    if "ESP8266BASE_CFG_KEY_WDT_PENDING" in watchdog_cpp:
        fail("Watchdog must not keep WDT pending compatibility key")
    require_token(watchdog_doc, "异步监护回调只写 RTC user memory", "Watchdog no-Flash timeout doc")
    require_token(watchdog_doc, "64-70", "Watchdog RTC reserved words doc")
    require_token(memory_doc, "152B DRAM + 28B RTC", "Watchdog RTC memory budget")
    require_token(memory_doc, "RTC user memory word 64-70", "Watchdog RTC memory budget detail")
    require_token(watchdog_cpp, "ESP8266BASE_RECOVERY_HEALTHY_RESET_MS", "30-minute application-ready chain reset")
    require_token(watchdog_cpp, "_restartsInWindow", "trusted 24-hour restart budget")
    if "Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT,   (int)_resetCount)" in watchdog_cpp:
        fail("Watchdog timeout branch must not write WDT count to LittleFS directly")
    require_token(ota_cpp, "Update.end();", "OTA write failure cleanup")
    require_token(ota_cpp, "upload_progress progress=%u%% bytes=%s request_total=%s speed=%s elapsed=%s",
                  "OTA progress diagnostics")
    require_token(ota_cpp, "upload_finished uploaded=%s elapsed=%s average_speed=%s free_heap=%s",
                  "OTA finish diagnostics")
    require_token(ota_cpp, "upload_success uploaded=%s elapsed=%s average_speed=%s free_heap=%s response_delivered=%s action=reboot",
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


def test_fixed_mqtt_terminal_and_ota_lifecycle_contract() -> None:
    ota = read("src/Esp8266BaseOTA.cpp")
    prepare = ota[ota.index("void Esp8266BaseOTA::_handlePrepareRequest()"):ota.index("void Esp8266BaseOTA::_handleUploadChunk()")]
    upload = ota[ota.index("void Esp8266BaseOTA::_handleUploadChunk()"):]
    if not prepare.index("_prepareCallback(") < prepare.index("RecordStore::prepareMaintenance()") < prepare.index("MQTT::pauseForOTA()"):
        fail("Store checkpoint must follow accepted application preparation and precede MQTT pause")
    if not upload.index("RecordStore::prepareMaintenance()") < upload.index("MQTT::pauseForOTA()") < upload.index("Update.begin("):
        fail("Store must pause before transport shutdown and OTA writes")
    for start, end in [("_expirePrepare()", "_notifyFailure("), ("_failUpload(", "_handleUploadComplete()")]:
        first = ota.index("void Esp8266BaseOTA::" + start)
        last = ota.index("void Esp8266BaseOTA::" + end, first)
        require_token(ota[first:last], "RecordStore::resumeAfterMaintenance()", "Store failure recovery")
    require_token(ota, "if (!preservePrepared) Esp8266BaseRecordStore::resumeAfterMaintenance();", "prepared upload keeps Store paused")
    for module in ("Web", "Sleep"):
        require_token(read("src/Esp8266Base" + module + ".cpp"), "RecordStore::prepareMaintenance()", "normal lifecycle checkpoint")

    actual_mqtt = read("src/Esp8266BaseMQTT.cpp")
    begin = actual_mqtt.index("void Esp8266BaseMQTT::_startGracefulDisconnect()")
    end = actual_mqtt.index("bool Esp8266BaseMQTT::_sendDisconnectPacket()", begin)
    require_token(actual_mqtt[begin:end], "flushBeforeDeadline", "checked DISCONNECT flush")
    forbid_token(actual_mqtt[begin:end], "client.flush();", "unchecked DISCONNECT flush")
    options_h = read("src/Esp8266BaseOptions.h")
    base_cpp = read("src/Esp8266Base.cpp")
    mqtt_h = read("src/Esp8266BaseMQTT.h")
    mqtt_cpp = read("src/Esp8266BaseMQTT.cpp")
    fixed_h = read("src/Esp8266BaseMQTTFixed.h")
    journal_cpp = read("src/Esp8266BaseJournal.cpp")
    web_cpp = read("src/Esp8266BaseWeb.cpp")
    ota_cpp = read("src/Esp8266BaseOTA.cpp")
    terminal_ini = read("examples/mqtt_terminal/platformio.ini")

    for token in ["ESP8266BASE_USE_FILESYSTEM", "ESP8266BASE_USE_CONFIG",
                  "ESP8266BASE_USE_WIFI_CONFIG", "ESP8266BASE_USE_WEB_AUTH_CONFIG",
                  "ESP8266BASE_USE_FILELOG"]:
        require_token(options_h, token, f"storage feature switch {token}")
    for token in ["-DPBUF_POOL_SIZE=7", "-DMEMP_NUM_TCP_PCB=4"]:
        require_token(terminal_ini, token, "relay-derived MQTT/Web pool configuration")
    require_token(terminal_ini, "[env:esp12f-no-fs]", "no-filesystem terminal build")
    for token in ["-DESP8266BASE_USE_FILESYSTEM=0", "-DESP8266BASE_USE_CONFIG=0",
                  "-DESP8266BASE_USE_FILELOG=0"]:
        require_token(terminal_ini, token, f"no-filesystem build flag {token}")

    require_token(fixed_h, "#define ESP8266BASE_MQTT_TX_SLOTS 2", "two fixed TX slots")
    require_token(fixed_h, "#define ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES 512",
                  "bounded outbound payload default")
    require_token(fixed_h, "ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES > 768",
                  "bounded outbound payload configurable maximum")
    require_token(fixed_h, "payloadHead[PAYLOAD_HEAD_BYTES]",
                  "split outbound payload head")
    require_token(fixed_h, "payloadTail[PAYLOAD_TAIL_BYTES]",
                  "split outbound payload tail")
    require_token(journal_cpp, "Esp8266BaseWeb::lastActivityMs()",
                  "Journal/Web storage quiet-window source")
    require_token(journal_cpp, "if (!storageFlushWindowAvailable(now)) return;",
                  "routine Journal flush Web quiet-window gate")
    require_token(journal_cpp, "if (g_ringCount == RING_ENTRIES) _flushPending();",
                  "forced recovery Journal flush remains explicit")
    require_token(fixed_h, "#define ESP8266BASE_MQTT_RX_CHUNK_BYTES 64",
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
    require_token(readme, "HTTP 页面、`/health` 和 MQTT 诊断均不输出凭据", "README credential redaction boundary")
    forbid_token(readme, "/wifi` GET 表单也会回显已保存密码", "README plaintext WiFi password echo")
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
    require_token(web_h, "#define ESP8266BASE_WEB_TCP_BACKLOG 1U",
                  "bounded Web TCP backlog default")
    require_token(web_cpp,
                  "_server.close();  // resets request state and collects Authorization header\n"
                  "    _server.getServer().begin(80, ESP8266BASE_WEB_TCP_BACKLOG);",
                  "authenticated bounded Web TCP listener start")
    if "_server.begin();" in web_cpp:
        fail("Web must not use the Core default backlog before the heap gate")
    require_token(web_cpp, "client->setSync(true);",
                  "Web responses avoid temporary TCP send-copy heap")
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
    # Reboot safety: GET only renders a confirm page and must never restart;
    # POST requires the per-boot CSRF token plus explicit confirm=1; the shared
    # reboot body invokes emergency-stop before sending any response.
    reboot_get = web_cpp[web_cpp.index('void Esp8266BaseWeb::_handleRebootGet'):
                         web_cpp.index('void Esp8266BaseWeb::_performReboot')]
    for bad in ('_performReboot', 'ESP.restart'):
        if bad in reboot_get:
            fail('GET /reboot must never perform the restart: ' + bad)
    require_token(reboot_get, 'WEB_REBOOT_CONFIRM_HEAD', 'GET /reboot confirm page')
    perform_seg = web_cpp[web_cpp.index('void Esp8266BaseWeb::_performReboot'):
                          web_cpp.index('void Esp8266BaseWeb::_handleRebootPost')]
    if perform_seg.index('invokeEmergencyStop') >= perform_seg.index('sendHeader'):
        fail('reboot must invoke emergency-stop before any response')
    reboot_post = web_cpp[web_cpp.index('void Esp8266BaseWeb::_handleRebootPost'):]
    require_token(reboot_post, '_verifyFormToken()', 'reboot POST CSRF check')
    require_token(reboot_post, 'arg("confirm") != "1"', 'reboot POST explicit confirm')
    require_token(reboot_post, '_performReboot();', 'reboot POST performs reboot')
    require_token(web_cpp, 'uri == "/reboot" && method == HTTP_GET', 'terminal reboot GET route')
    require_token(web_cpp, '_server.on("/reboot", HTTP_POST, _handleRebootPost);',
                  'full profile reboot POST registration')
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


def test_network_log_literals_in_flash() -> None:
    # Guard the fixed-text call sites, not the public API for dynamic formats.
    ordinary_literal = re.compile(r'ESP8266BASE_LOG_[DIWE]\(\s*"[^"]*"\s*,\s*"')
    for name in ("Esp8266BaseWiFi.cpp", "Esp8266BaseNTP.cpp", "Esp8266BaseMQTT.cpp"):
        if ordinary_literal.search((ROOT / "src" / name).read_text()):
            fail(f"{name}: fixed network log formats must use the existing _P macro")


def test_tls_handshake_admission() -> None:
    source = read("src/Esp8266BaseMQTT.cpp")
    start = source.index("if (!_prepareWill())")
    gate = source.index("availableHeap < requiredHeap || availableBlock < requiredBlock", start)
    attempts = source.index("if (_attemptCount <", gate)
    connect = source.index("if (!_connectTransport())", attempts)
    if not start < gate < attempts < connect:
        fail("TLS memory admission must precede attempt accounting and allocation")
    for token in ("sizeof(br_ssl_client_context)", "sizeof(br_x509_minimal_context)",
                  "tlsRounded + 4096U", "largestAllocation + 64U + 1023U",
                  "_retryAt = millis() + 5000UL", "return; // Not a connection attempt"):
        require_token(source, token, "TLS working-set memory admission")


def test_small_http_replies() -> None:
    source = read("src/Esp8266BaseWeb.cpp")
    start = source.index('client->print(F("HTTP/1.1 503')
    end = source.index("client->flush()", start)
    pieces = re.findall(r'"((?:\\.|[^"\\])*)"', source[start:end])
    reply = ''.join(pieces).encode().decode('unicode_escape')
    header, body = reply.split('\r\n\r\n', 1)
    declared = int(re.search(r'Content-Length: (\d+)', header).group(1))
    if declared != len(body.encode()):
        fail("503 declared length differs from its actual body")
    if '_server.send(204' in source or source.count('_sendIconResponse();') != 2:
        fail("Both icon routes must use the bounded static response path")
    icon = source[source.index('void Esp8266BaseWeb::_sendIconResponse()'):source.index('void Esp8266BaseWeb::_markRequest()')]
    for token in ('sendContent_P(PSTR(', '_server.keepAlive(false)', '_server.client().stop()'):
        require_token(icon, token, "bounded icon response")


def main() -> None:
    test_small_http_replies()
    test_tls_handshake_admission()
    test_network_log_literals_in_flash()
    test_format_bytes()
    test_journal_heap_precision()
    test_log_file_buffer_rules()
    test_wifi_retry_rules()
    test_ntp_manual_packet_validation()
    test_ntp_trust_core()
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
