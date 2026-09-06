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
// 重启计数持久化到 Config：eb_wdt_count。
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

    // 每轮主循环入口调用：开始测量本轮。调用点必须在任何子系统之前。
    static void cycleStart();

    // 在某段"合法但可能较久"的阻塞子系统结束后调用：把该段实际耗时计入本轮已覆盖时间
    // （超过 maxBlockMs 的部分按未覆盖处理）。maxBlockMs 例如 MQTT 同步 TLS 建连 ~10s、
    // Web 慢客户端 5s、NTP DNS 8s；段内真实卡死不会被掩盖。
    static void account(uint32_t maxBlockMs, uint32_t startedAtMs);

    // 每轮主循环末尾调用：本轮"未覆盖"时间（整轮耗时 - 已覆盖段耗时）超过基础超时
    // 即视为停滞，写 RTC 标记并重启；正常轮内不应触发。
    static void handle();

    // 喂狗（兼容保留；本轮测量以 cycleStart 为基准，feed 不再掩盖停滞）
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
    static uint32_t _timeoutMs;     // 4B：基础超时时间（已 clamp）
    static uint32_t _lastFeedMs;    // 4B：兼容保留
    static uint32_t _cycleStartMs;  // 4B：本轮测量起点
    static uint32_t _coveredMs;     // 4B：本轮已声明宽限段覆盖的耗时（封顶防溢出）
    static uint32_t _resetCount;    // 4B：累计 WDT 重启次数
};
