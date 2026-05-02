#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseSleep — 睡眠管理
//
// 支持两种模式：
//   1. modemSleep()  — 关闭 RF，CPU 继续运行（省电 ~70 mA → ~15 mA）
//   2. deepSleep(s)  — 全芯片休眠，指定秒数后由 GPIO16→RST 唤醒
//                       ⚠ 需要 GPIO16 连接 RST 引脚
//
// deepSleep 前自动执行预飞检查：
//   Log 诊断 → pause Watchdog → flush Config → WiFi disconnect → ESP.deepSleep()
//
// RAM 预算：<= 48B
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_SLEEP_MAX_DEEP_SEC
#define ESP8266BASE_SLEEP_MAX_DEEP_SEC 3600   // 最大深睡时间（秒）
#endif

class Esp8266BaseSleep {
public:
    // 初始化：检测并记录唤醒原因
    static bool begin();

    // Modem sleep：关闭 RF，保持 CPU 运行（WiFi 会断开）
    static void modemSleep();

    // 唤醒 Modem（重新启用 RF，需外部重新连接 WiFi）
    static void wakeModem();

    // 深度睡眠 sleepSec 秒（0 = 永久睡眠，直到 RST）
    // 超过 ESP8266BASE_SLEEP_MAX_DEEP_SEC 的值会被 clamp
    static void deepSleep(uint32_t sleepSec);

    // 返回唤醒原因字符串（"power-on" / "deep-sleep" / "soft-restart" / "wdt-reset" / "unknown"）
    static const char* wakeReason();

    // 是否从深睡唤醒
    static bool isDeepSleepWake();

private:
    static bool        _initialized;   // 1B
    static bool        _modemSleeping; // 1B
    static const char* _wakeReason;    // 4B（指向静态字符串字面量）
};
