#include "Esp8266BaseSleep.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseWatchdog.h"
#include <ESP8266WiFi.h>
#include <user_interface.h>   // rst_info, REASON_DEEP_SLEEP_AWAKE 等

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool        Esp8266BaseSleep::_initialized   = false;
bool        Esp8266BaseSleep::_modemSleeping = false;
const char* Esp8266BaseSleep::_wakeReason    = "unknown";

// ----------------------------------------------------------------------------
// begin — 检测唤醒原因
// ----------------------------------------------------------------------------
bool Esp8266BaseSleep::begin() {
    rst_info* ri = ESP.getResetInfoPtr();
    if (ri) {
        switch (ri->reason) {
            case REASON_DEFAULT_RST:
            case REASON_EXT_SYS_RST:
                _wakeReason = "power-on";
                break;
            case REASON_DEEP_SLEEP_AWAKE:
                _wakeReason = "deep-sleep";
                break;
            case REASON_SOFT_RESTART:
                _wakeReason = "soft-restart";
                break;
            case REASON_SOFT_WDT_RST:
            case REASON_WDT_RST:
                _wakeReason = "wdt-reset";
                break;
            default:
                _wakeReason = "unknown";
                break;
        }
    }

    _initialized = true;
    ESP8266BASE_LOG_I("SLEP", "ready=1 wake=%s", _wakeReason);
    return true;
}

// ----------------------------------------------------------------------------
// modemSleep — 关闭 RF
// ----------------------------------------------------------------------------
void Esp8266BaseSleep::modemSleep() {
    if (_modemSleeping) return;
    WiFi.forceSleepBegin();
    delay(1);  // 等待 modem 进入睡眠
    _modemSleeping = true;
    ESP8266BASE_LOG_I("SLEP", "modem_sleep=1");
}

// ----------------------------------------------------------------------------
// wakeModem — 重新启用 RF
// ----------------------------------------------------------------------------
void Esp8266BaseSleep::wakeModem() {
    if (!_modemSleeping) return;
    WiFi.forceSleepWake();
    delay(1);
    _modemSleeping = false;
    ESP8266BASE_LOG_I("SLEP", "modem_sleep=0");
}

// ----------------------------------------------------------------------------
// deepSleep — 预飞检查后进入深度睡眠
// ----------------------------------------------------------------------------
void Esp8266BaseSleep::deepSleep(uint32_t sleepSec) {
    // clamp 最大睡眠时间
    if (sleepSec > ESP8266BASE_SLEEP_MAX_DEEP_SEC) {
        sleepSec = ESP8266BASE_SLEEP_MAX_DEEP_SEC;
    }

    ESP8266BASE_LOG_I("SLEP", "deep_sleep sec=%u heap=%u",
                      (unsigned)sleepSec, (unsigned)ESP.getFreeHeap());

    // 预飞：暂停看门狗
    Esp8266BaseWatchdog::pause();

    // 预飞：flush Config（确保待写数据落盘）
    Esp8266BaseConfig::flush();

    // 预飞：断开 WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    // 进入深度睡眠（sleepSec=0 表示永久，直到 RST）
    uint64_t us = (uint64_t)sleepSec * 1000000ULL;
    ESP.deepSleep(us, WAKE_RF_DEFAULT);

    // 此行不会执行（deepSleep 不返回）
}

// ----------------------------------------------------------------------------
// 查询
// ----------------------------------------------------------------------------
const char* Esp8266BaseSleep::wakeReason() {
    return _wakeReason;
}

bool Esp8266BaseSleep::isDeepSleepWake() {
    return strcmp(_wakeReason, "deep-sleep") == 0;
}
