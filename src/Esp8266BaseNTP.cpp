#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_NTP
#include "Esp8266BaseNTP.h"
#include "Esp8266BaseLog.h"
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#endif
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <sntp.h>
#include <sys/time.h>
#include <time.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool     Esp8266BaseNTP::_synced      = false;
bool     Esp8266BaseNTP::_logSwitched = false;
uint32_t Esp8266BaseNTP::_lastCheckMs = 0;
uint32_t Esp8266BaseNTP::_startedMs = 0;
uint32_t Esp8266BaseNTP::_lastPendingLogMs = 0;
uint32_t Esp8266BaseNTP::_nextManualMs = 0;
uint32_t Esp8266BaseNTP::_manualSentMs = 0;
uint8_t  Esp8266BaseNTP::_manualServer = 0;
bool     Esp8266BaseNTP::_manualWaiting = false;

static WiFiUDP _ntpUdp;
static IPAddress _manualIp;
static const uint16_t NTP_PORT = 123;
static const uint16_t NTP_LOCAL_PORT = 2390;
static const uint32_t NTP_EPOCH_DELTA = 2208988800UL;

// NTP 服务器列表（全部在 Flash）
static const char NTP_S1[] PROGMEM = ESP8266BASE_NTP_SERVER_1;
static const char NTP_S2[] PROGMEM = ESP8266BASE_NTP_SERVER_2;
static const char NTP_S3[] PROGMEM = ESP8266BASE_NTP_SERVER_3;
static const char* const NTP_SERVERS[] PROGMEM = { NTP_S1, NTP_S2, NTP_S3 };
static const uint8_t NTP_SERVER_COUNT = sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]);
static char _ntpServer1[24];
static char _ntpServer2[24];
static char _ntpServer3[24];

static void _formatIP(const IPAddress& ip, char* out, size_t len) {
    if (!out || len == 0) return;
    snprintf(out, len, "%u.%u.%u.%u",
             (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
}

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseNTP::begin() {
    // configTime 可以在 WiFi 连接前调用；SNTP 客户端会在有网络时自动同步
    // 第二参数 daylightOffset_sec = 0（不支持夏令时）
    strncpy_P(_ntpServer1, NTP_S1, sizeof(_ntpServer1) - 1); _ntpServer1[sizeof(_ntpServer1)-1] = '\0';
    strncpy_P(_ntpServer2, NTP_S2, sizeof(_ntpServer2) - 1); _ntpServer2[sizeof(_ntpServer2)-1] = '\0';
    strncpy_P(_ntpServer3, NTP_S3, sizeof(_ntpServer3) - 1); _ntpServer3[sizeof(_ntpServer3)-1] = '\0';

    configTime(ESP8266BASE_NTP_TIMEZONE, 0, _ntpServer1, _ntpServer2, _ntpServer3);
    _synced = false;
    _logSwitched = false;
    _lastCheckMs = 0;
    _lastPendingLogMs = 0;
    _startedMs = millis();
    _nextManualMs = _startedMs + 1000UL;
    _manualSentMs = 0;
    _manualServer = 0;
    _manualWaiting = false;
    _ntpUdp.stop();
    _ntpUdp.begin(NTP_LOCAL_PORT);

    ESP8266BASE_LOG_I("NTP ", "ntp_client_started timezone=UTC+%d servers=%s,%s,%s check_interval=5s manual_udp=yes",
                      ESP8266BASE_NTP_TIMEZONE / 3600, _ntpServer1, _ntpServer2, _ntpServer3);
    return true;
}

// ----------------------------------------------------------------------------
// handle — 每轮检查同步状态（每 5 秒检查一次，同步后降频）
// ----------------------------------------------------------------------------
void Esp8266BaseNTP::handle() {
    uint32_t now = millis();
    if (!_synced && _pollManual(now)) {
        return;
    }

    // 未同步前每 5 秒检查，同步后每小时重新验证
    uint32_t interval = _synced
        ? (uint32_t)ESP8266BASE_NTP_SYNC_INTERVAL * 1000UL
        : 5000UL;

    if (now - _lastCheckMs < interval) return;
    _lastCheckMs = now;

    time_t t = time(nullptr);
    if (t < 1000000000UL) {
        _sendManual(now);
        if (!_synced && (now - _lastPendingLogMs >= 30000UL || _lastPendingLogMs == 0)) {
            _lastPendingLogMs = now;
            ESP8266BASE_LOG_W("NTP ", "ntp_sync_pending elapsed=%lus raw_time=%lu sntp_enabled=%s reach=%03o/%03o/%03o manual_waiting=%s next_check=5s",
                              (unsigned long)((now - _startedMs) / 1000UL),
                              (unsigned long)t,
                              sntp_enabled() ? "yes" : "no",
                              (unsigned)sntp_getreachability(0),
                              (unsigned)sntp_getreachability(1),
                              (unsigned)sntp_getreachability(2),
                              _manualWaiting ? "yes" : "no");
        }
        return;  // 未同步（2001 年以前的时间戳认为无效）
    }

    if (!_synced) {
        _finishSync(t);
    }
}

void Esp8266BaseNTP::reset() {
    _ntpUdp.stop();
    _synced = false;
    _logSwitched = false;
    _lastCheckMs = 0;
    _lastPendingLogMs = 0;
    _startedMs = 0;
    _nextManualMs = 0;
    _manualSentMs = 0;
    _manualServer = 0;
    _manualWaiting = false;
    ESP8266BASE_LOG_I("NTP ", "ntp_client_reset reason=wifi_disconnected");
}

bool Esp8266BaseNTP::_pollManual(uint32_t now) {
    int packetSize = _ntpUdp.parsePacket();
    if (packetSize >= 48) {
        IPAddress remoteIp = _ntpUdp.remoteIP();
        uint16_t remotePort = _ntpUdp.remotePort();
        uint8_t pkt[48];
        _ntpUdp.read(pkt, sizeof(pkt));
        uint8_t mode = pkt[0] & 0x07;
        uint8_t leap = (pkt[0] >> 6) & 0x03;
        uint8_t stratum = pkt[1];
        if (!_manualWaiting || remoteIp != _manualIp || remotePort != NTP_PORT ||
            mode != 4 || leap == 3 || stratum == 0 || stratum > 15) {
            char remote[16];
            char expected[16];
            _formatIP(remoteIp, remote, sizeof(remote));
            _formatIP(_manualIp, expected, sizeof(expected));
            ESP8266BASE_LOG_W("NTP ", "manual_ntp_packet_rejected remote=%s port=%u expected=%s mode=%u leap=%u stratum=%u waiting=%s",
                              remote, (unsigned)remotePort, expected,
                              (unsigned)mode, (unsigned)leap, (unsigned)stratum,
                              _manualWaiting ? "yes" : "no");
            return false;
        }
        uint32_t ntpSec = ((uint32_t)pkt[40] << 24)
                        | ((uint32_t)pkt[41] << 16)
                        | ((uint32_t)pkt[42] << 8)
                        |  (uint32_t)pkt[43];
        time_t epoch = (ntpSec > NTP_EPOCH_DELTA) ? (time_t)(ntpSec - NTP_EPOCH_DELTA) : 0;
        if (epoch > 1000000000UL) {
            timeval tv;
            tv.tv_sec = epoch;
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            _manualWaiting = false;
            char ip[16];
            _formatIP(_manualIp, ip, sizeof(ip));
            ESP8266BASE_LOG_I("NTP ", "manual_ntp_synchronized server_index=%u ip=%s rtt=%lums",
                              (unsigned)_manualServer,
                              ip,
                              (unsigned long)(now - _manualSentMs));
            _finishSync(time(nullptr));
            return true;
        }
    } else if (packetSize > 0) {
        while (_ntpUdp.available()) _ntpUdp.read();
    }

    if (_manualWaiting && now - _manualSentMs >= 3000UL) {
        char ip[16];
        _formatIP(_manualIp, ip, sizeof(ip));
        ESP8266BASE_LOG_W("NTP ", "manual_ntp_timeout server_index=%u ip=%s timeout=3s",
                          (unsigned)_manualServer, ip);
        _manualWaiting = false;
        _manualServer = (_manualServer + 1) % NTP_SERVER_COUNT;
        _nextManualMs = now + 2000UL;
    }
    return false;
}

void Esp8266BaseNTP::_sendManual(uint32_t now) {
    if (_manualWaiting || now < _nextManualMs) return;

    char server[24];
    strncpy_P(server, (PGM_P)pgm_read_ptr(&NTP_SERVERS[_manualServer]), sizeof(server) - 1);
    server[sizeof(server) - 1] = '\0';

#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
    int dnsOk = WiFi.hostByName(server, _manualIp);
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
    if (dnsOk != 1 || !_manualIp.isSet()) {
        ESP8266BASE_LOG_W("NTP ", "manual_ntp_dns_failed server=%s", server);
        _manualServer = (_manualServer + 1) % NTP_SERVER_COUNT;
        _nextManualMs = millis() + 30000UL;
        return;
    }

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3(client)

    _ntpUdp.beginPacket(_manualIp, NTP_PORT);
    _ntpUdp.write(pkt, sizeof(pkt));
    char ip[16];
    _formatIP(_manualIp, ip, sizeof(ip));
    if (_ntpUdp.endPacket()) {
        _manualSentMs = now;
        _manualWaiting = true;
        ESP8266BASE_LOG_I("NTP ", "manual_ntp_request server=%s ip=%s",
                          server, ip);
    } else {
        ESP8266BASE_LOG_W("NTP ", "manual_ntp_send_failed server=%s ip=%s",
                          server, ip);
        _manualServer = (_manualServer + 1) % NTP_SERVER_COUNT;
        _nextManualMs = now + 5000UL;
    }
}

void Esp8266BaseNTP::_finishSync(time_t t) {
    _synced = true;
    uint32_t uptimeMs = millis();
    time_t bootTime = t > (time_t)(uptimeMs / 1000UL)
        ? t - (time_t)(uptimeMs / 1000UL)
        : 0;

    char nowBuf[20];
    char bootBuf[20] = "unknown";
    struct tm* tm_info = localtime(&t);
    strftime(nowBuf, sizeof(nowBuf), "%Y-%m-%d %H:%M:%S", tm_info);

    if (bootTime > 0) {
        struct tm* bootTm = localtime(&bootTime);
        strftime(bootBuf, sizeof(bootBuf), "%Y-%m-%d %H:%M:%S", bootTm);
    }

    ESP8266BASE_LOG_I("NTP ", "time_synchronized actual_time=%s uptime_ms=%lu boot_time=%s",
                      nowBuf, (unsigned long)uptimeMs, bootBuf);
    ESP8266BASE_LOG_I("NTP ", "time_mapping boot_millis=0 actual_time=%s current_millis=%lu current_time=%s",
                      bootBuf, (unsigned long)uptimeMs, nowBuf);
    _ntpUdp.stop();
    _manualWaiting = false;

    if (!_logSwitched) {
        _logSwitched = true;
        Esp8266BaseLog::setTimeProvider(_timeStr);
        ESP8266BASE_LOG_I("NTP ", "log_timestamp_mode=absolute_datetime");
    }
}

// ----------------------------------------------------------------------------
// 公开查询
// ----------------------------------------------------------------------------
bool Esp8266BaseNTP::isSynced() {
    return _synced;
}

uint32_t Esp8266BaseNTP::timestamp() {
    if (!_synced) return 0;
    return (uint32_t)time(nullptr);
}

bool Esp8266BaseNTP::formatTo(char* out, size_t len, const char* fmt) {
    if (!out || !len || !fmt) return false;
    out[0] = '\0';
    if (!_synced) return false;
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    strftime(out, len, fmt, tm_info);
    return true;
}

// ----------------------------------------------------------------------------
// Log 时间回调（静态，注入 Esp8266BaseLog::setTimeProvider）
// 返回指向静态缓冲的指针（非重入，适合 ESP8266 Arduino 单线程日志路径）。
// Log hook 若需要异步保存 timestamp，必须在回调内立即复制字符串。
// ----------------------------------------------------------------------------
const char* Esp8266BaseNTP::_timeStr() {
    static char buf[20];  // "YYYY-MM-DD HH:MM:SS\0" = 20 字节
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}
#endif
