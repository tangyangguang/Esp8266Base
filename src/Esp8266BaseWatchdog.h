#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseWatchdog — 软件看门狗
//
// 通过 millis() 追踪上次喂狗时间。
// 若超时未喂狗，先写 RTC 标记再执行 ESP.restart()；下次启动后补写重启计数。
// OTA 写入期间可通过 pause()/resume() 暂停。
//
// 超时范围：1000 ~ 3000 ms（含边界，通过宏或 begin() 参数设置）
// 重启计数持久化到 Config：eb_wdt_count；eb_wdt_pending 仅用于兼容旧固件残留标记。
//
// RAM 预算：<= 80B
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_WDT_TIMEOUT_MS
#define ESP8266BASE_WDT_TIMEOUT_MS 2500   // 默认 2.5 秒
#endif

class Esp8266BaseWatchdog {
public:
    // 初始化：读取 Config 中累计重启计数，设置超时时间（ms，会被 clamp 到 1000~3000）
    static bool begin(uint32_t timeoutMs = ESP8266BASE_WDT_TIMEOUT_MS);

    // 每轮调用：检查是否超时，超时则保存状态并重启
    static void handle();

    // 喂狗：重置计时器
    static void feed();

    // 暂停/恢复（OTA 写入时使用）
    static void pause();
    static void resume();

    // 查询
    static bool     isRunning();
    static bool     isPaused();
    static bool     wasWatchdogReset();  // 本次启动是否由 WDT 触发
    static uint32_t resetCount();        // 累计 WDT 重启次数
    static void     clearResetCount();   // 清零计数（写入 Config）

private:
    static bool     _running;       // 1B
    static bool     _paused;        // 1B
    static bool     _wasWdtReset;   // 1B：本次启动是否 WDT 触发
    static uint32_t _timeoutMs;     // 4B：超时时间（已 clamp）
    static uint32_t _lastFeedMs;    // 4B：上次喂狗时间
    static uint32_t _resetCount;    // 4B：累计 WDT 重启次数
};
