#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include <user_interface.h>

static const uint32_t WDT_RTC_ADDR = 64;
static const uint32_t WDT_RTC_MAGIC = 0xEB0BDA6DUL;
static const uint32_t WDT_RTC_SALT = 0xA5C35A3CUL;

struct WatchdogRtcState {
    uint32_t magic;
    uint32_t count;
    uint32_t checksum;
};

static uint32_t _wdtRtcChecksum(uint32_t count) {
    return WDT_RTC_MAGIC ^ count ^ WDT_RTC_SALT;
}

static bool _readWdtRtcState(WatchdogRtcState& state) {
    if (!system_rtc_mem_read(WDT_RTC_ADDR, &state, sizeof(state))) return false;
    return state.magic == WDT_RTC_MAGIC && state.checksum == _wdtRtcChecksum(state.count);
}

static bool _writeWdtRtcState(uint32_t count) {
    WatchdogRtcState state;
    state.magic = WDT_RTC_MAGIC;
    state.count = count;
    state.checksum = _wdtRtcChecksum(count);
    return system_rtc_mem_write(WDT_RTC_ADDR, &state, sizeof(state));
}

static bool _clearWdtRtcState() {
    WatchdogRtcState state;
    state.magic = 0;
    state.count = 0;
    state.checksum = 0;
    return system_rtc_mem_write(WDT_RTC_ADDR, &state, sizeof(state));
}

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool     Esp8266BaseWatchdog::_running     = false;
bool     Esp8266BaseWatchdog::_paused      = false;
bool     Esp8266BaseWatchdog::_wasWdtReset = false;
uint32_t Esp8266BaseWatchdog::_timeoutMs   = ESP8266BASE_WDT_TIMEOUT_MS;
uint32_t Esp8266BaseWatchdog::_lastFeedMs  = 0;
uint32_t Esp8266BaseWatchdog::_resetCount  = 0;

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseWatchdog::begin(uint32_t timeoutMs) {
    // clamp timeout 到 1000~3000ms
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 3000) timeoutMs = 3000;
    _timeoutMs = timeoutMs;

    // 读取累计重启记录；WDT 超时路径只写 RTC 标记，Flash 在本次正常启动后补写。
    _resetCount  = (uint32_t)Esp8266BaseConfig::getInt(ESP8266BASE_CFG_KEY_WDT_COUNT);
    WatchdogRtcState rtcState;
    bool rtcPending = _readWdtRtcState(rtcState);

    if (rtcPending) {
        _wasWdtReset = true;
        if (rtcState.count > _resetCount) {
            _resetCount = rtcState.count;
        } else if (_resetCount < 0xFFFFFFFFUL) {
            _resetCount++;
        }
        bool countOk = Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT, (int)_resetCount);
        bool clearOk = false;
        if (countOk) {
            clearOk = _clearWdtRtcState();
        }
        ESP8266BASE_LOG_W("WDT ", "boot_after_watchdog_reset reset_count=%u source=rtc persist=%s rtc_clear=%s",
                          (unsigned)_resetCount,
                          countOk ? "success" : "failed",
                          clearOk ? "success" : "failed");
    } else {
        _wasWdtReset = false;
    }

    _lastFeedMs = millis();
    _running    = true;

    ESP8266BASE_LOG_I("WDT ", "watchdog_ready timeout=%ums reset_count=%u",
                      (unsigned)_timeoutMs, (unsigned)_resetCount);
    return true;
}

// ----------------------------------------------------------------------------
// handle — 每轮检查
// ----------------------------------------------------------------------------
void Esp8266BaseWatchdog::handle() {
    if (!_running || _paused) return;

    uint32_t elapsed = millis() - _lastFeedMs;
    if (elapsed >= _timeoutMs) {
        // 超时路径避免写 LittleFS，防止在系统已卡住时二次阻塞。
        if (_resetCount < 0xFFFFFFFFUL) {
            _resetCount++;
        }
        _writeWdtRtcState(_resetCount);

        ESP8266BASE_LOG_E("WDT ", "watchdog_timeout elapsed=%ums reset_count=%u action=restart",
                          (unsigned)elapsed, (unsigned)_resetCount);

        // 给串口缓冲区时间输出
        delay(50);
        ESP.restart();
    }
}

// ----------------------------------------------------------------------------
// feed
// ----------------------------------------------------------------------------
void Esp8266BaseWatchdog::feed() {
    _lastFeedMs = millis();
}

// ----------------------------------------------------------------------------
// pause / resume
// ----------------------------------------------------------------------------
void Esp8266BaseWatchdog::pause() {
    _paused = true;
    ESP8266BASE_LOG_D("WDT ", "watchdog_paused");
}

void Esp8266BaseWatchdog::resume() {
    _lastFeedMs = millis();  // resume 时重置计时，避免因暂停期间"超时"
    _paused     = false;
    ESP8266BASE_LOG_D("WDT ", "watchdog_resumed");
}

// ----------------------------------------------------------------------------
// 查询
// ----------------------------------------------------------------------------
bool Esp8266BaseWatchdog::isRunning() {
    return _running;
}

bool Esp8266BaseWatchdog::isPaused() {
    return _paused;
}

bool Esp8266BaseWatchdog::wasWatchdogReset() {
    return _wasWdtReset;
}

uint32_t Esp8266BaseWatchdog::resetCount() {
    return _resetCount;
}

void Esp8266BaseWatchdog::clearResetCount() {
    _resetCount = 0;
    _clearWdtRtcState();
    Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT, 0);
    ESP8266BASE_LOG_I("WDT ", "watchdog_reset_count_cleared");
}
#endif
