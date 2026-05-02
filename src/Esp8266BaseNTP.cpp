#include "Esp8266BaseNTP.h"
#include "Esp8266BaseLog.h"
#include <time.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool     Esp8266BaseNTP::_synced      = false;
bool     Esp8266BaseNTP::_logSwitched = false;
uint32_t Esp8266BaseNTP::_lastCheckMs = 0;

// NTP 服务器列表（全部在 Flash）
static const char NTP_S1[] PROGMEM = "ntp.aliyun.com";
static const char NTP_S2[] PROGMEM = "ntp.tencent.com";
static const char NTP_S3[] PROGMEM = "cn.pool.ntp.org";

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

    ESP8266BASE_LOG_I("NTP ", "ntp_started timezone=UTC+%d servers=%s,%s",
                      ESP8266BASE_NTP_TIMEZONE / 3600, s1, s2);
    return true;
}

// ----------------------------------------------------------------------------
// handle — 每轮检查同步状态（每 5 秒检查一次，同步后降频）
// ----------------------------------------------------------------------------
void Esp8266BaseNTP::handle() {
    uint32_t now = millis();
    // 未同步前每 5 秒检查，同步后每小时重新验证
    uint32_t interval = _synced
        ? (uint32_t)ESP8266BASE_NTP_SYNC_INTERVAL * 1000UL
        : 5000UL;

    if (now - _lastCheckMs < interval) return;
    _lastCheckMs = now;

    time_t t = time(nullptr);
    if (t < 1000000000UL) return;  // 未同步（2001 年以前的时间戳认为无效）

    if (!_synced) {
        _synced = true;
        struct tm* tm_info = localtime(&t);
        char tbuf[20];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);
        ESP8266BASE_LOG_I("NTP ", "time_synchronized local_time=%s", tbuf);
    }

    // 仅切换一次 Log 时间格式
    if (!_logSwitched) {
        _logSwitched = true;
        Esp8266BaseLog::setTimeProvider(_timeStr);
        ESP8266BASE_LOG_I("NTP ", "log_timestamp_mode=absolute_time");
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
    static char buf[10];  // "HH:MM:SS\0" = 9 字节
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    return buf;
}
