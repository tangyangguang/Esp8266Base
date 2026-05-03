#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_NTP
#include "Esp8266BaseNTP.h"
#include "Esp8266BaseLog.h"
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
static const char NTP_S1[] PROGMEM = "ntp.aliyun.com";
static const char NTP_S2[] PROGMEM = "ntp.tencent.com";
static const char NTP_S3[] PROGMEM = "cn.pool.ntp.org";
static const char* const NTP_SERVERS[] PROGMEM = { NTP_S1, NTP_S2, NTP_S3 };

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseNTP::begin() {
    // configTime 可以在 WiFi 连接前调用；SNTP 客户端会在有网络时自动同步
    // 第二参数 daylightOffset_sec = 0（不支持夏令时）
    char s1[20], s2[20], s3[20];
    strncpy_P(s1, NTP_S1, sizeof(s1) - 1); s1[sizeof(s1)-1] = '\0';
    strncpy_P(s2, NTP_S2, sizeof(s2) - 1); s2[sizeof(s2)-1] = '\0';
    strncpy_P(s3, NTP_S3, sizeof(s3) - 1); s3[sizeof(s3)-1] = '\0';

    configTime(ESP8266BASE_NTP_TIMEZONE, 0, s1, s2, s3);
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
                      ESP8266BASE_NTP_TIMEZONE / 3600, s1, s2, s3);
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

bool Esp8266BaseNTP::_pollManual(uint32_t now) {
    int packetSize = _ntpUdp.parsePacket();
    if (packetSize >= 48) {
        uint8_t pkt[48];
        _ntpUdp.read(pkt, sizeof(pkt));
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
            ESP8266BASE_LOG_I("NTP ", "manual_ntp_synchronized server_index=%u ip=%s rtt=%lums",
                              (unsigned)_manualServer,
                              _manualIp.toString().c_str(),
                              (unsigned long)(now - _manualSentMs));
            _finishSync(time(nullptr));
            return true;
        }
    } else if (packetSize > 0) {
        while (_ntpUdp.available()) _ntpUdp.read();
    }

    if (_manualWaiting && now - _manualSentMs >= 3000UL) {
        ESP8266BASE_LOG_W("NTP ", "manual_ntp_timeout server_index=%u ip=%s timeout=3s",
                          (unsigned)_manualServer, _manualIp.toString().c_str());
        _manualWaiting = false;
        _manualServer = (_manualServer + 1) % 3;
        _nextManualMs = now + 2000UL;
    }
    return false;
}

void Esp8266BaseNTP::_sendManual(uint32_t now) {
    if (_manualWaiting || now < _nextManualMs) return;

    char server[24];
    strncpy_P(server, (PGM_P)pgm_read_ptr(&NTP_SERVERS[_manualServer]), sizeof(server) - 1);
    server[sizeof(server) - 1] = '\0';

    if (WiFi.hostByName(server, _manualIp) != 1 || !_manualIp.isSet()) {
        ESP8266BASE_LOG_W("NTP ", "manual_ntp_dns_failed server=%s", server);
        _manualServer = (_manualServer + 1) % 3;
        _nextManualMs = now + 5000UL;
        return;
    }

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3(client)

    _ntpUdp.beginPacket(_manualIp, NTP_PORT);
    _ntpUdp.write(pkt, sizeof(pkt));
    if (_ntpUdp.endPacket()) {
        _manualSentMs = now;
        _manualWaiting = true;
        ESP8266BASE_LOG_I("NTP ", "manual_ntp_request server=%s ip=%s",
                          server, _manualIp.toString().c_str());
    } else {
        ESP8266BASE_LOG_W("NTP ", "manual_ntp_send_failed server=%s ip=%s",
                          server, _manualIp.toString().c_str());
        _manualServer = (_manualServer + 1) % 3;
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
// 返回指向静态缓冲的指针（非重入，适合串口单线程环境）
// ----------------------------------------------------------------------------
const char* Esp8266BaseNTP::_timeStr() {
    static char buf[20];  // "YYYY-MM-DD HH:MM:SS\0" = 20 字节
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}
#endif
