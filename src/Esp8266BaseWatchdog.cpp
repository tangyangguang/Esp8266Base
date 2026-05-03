#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"

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

    // 读取累计重启记录
    _resetCount  = (uint32_t)Esp8266BaseConfig::getInt("wdt_count");
    int pending  = Esp8266BaseConfig::getInt("wdt_pending");

    if (pending) {
        _wasWdtReset = true;
        // 清除 pending 标志，避免下次误判
        Esp8266BaseConfig::setInt("wdt_pending", 0);
        ESP8266BASE_LOG_W("WDT ", "boot_after_watchdog_reset reset_count=%u", (unsigned)_resetCount);
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
        // 超时：保存状态后重启
        _resetCount++;
        Esp8266BaseConfig::setInt("wdt_count",   (int)_resetCount);
        Esp8266BaseConfig::setInt("wdt_pending",  1);
        Esp8266BaseConfig::flush();

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
    Esp8266BaseConfig::setInt("wdt_count",   0);
    Esp8266BaseConfig::setInt("wdt_pending", 0);
    ESP8266BASE_LOG_I("WDT ", "watchdog_reset_count_cleared");
}
#endif
