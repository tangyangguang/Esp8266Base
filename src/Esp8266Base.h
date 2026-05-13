#pragma once
#include <Arduino.h>
#include "Esp8266BaseOptions.h"

// Phase 1 核心模块（已实现）
#include "Esp8266BaseLog.h"
#include "Esp8266BaseFileLog.h"
#include "Esp8266BaseUtil.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseWiFi.h"

#if ESP8266BASE_USE_WEB
#include "Esp8266BaseWeb.h"
#endif
#if ESP8266BASE_USE_OTA
#include "Esp8266BaseOTA.h"
#endif
#if ESP8266BASE_USE_NTP
#include "Esp8266BaseNTP.h"
#endif
#if ESP8266BASE_USE_MDNS
#include "Esp8266BaseMDNS.h"
#endif
#if ESP8266BASE_USE_SLEEP
#include "Esp8266BaseSleep.h"
#endif
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#endif

// ----------------------------------------------------------------------------
// Esp8266Base — 主入口
//
// 负责：
//   - 按顺序初始化各模块
//   - 在 handle() 中统一推进所有模块的非阻塞状态机
//   - 输出标准启动诊断日志
//   - 持有固件信息和 hostname
// ----------------------------------------------------------------------------

class Esp8266Base {
public:
    // ---- 启动前配置（必须在 begin() 前调用） ----

    // 设置固件名称和版本，用于日志和 /health 响应
    static void setFirmwareInfo(const char* name, const char* version);

    // ---- 核心 API ----

    // 按序初始化：Log → Sleep → Config → FileLog → WiFi → Watchdog → Web → OTA → 诊断日志
    // 返回 false 表示 Config 或 WiFi 初始化失败（仍继续运行）
    static bool begin();

    // 在 loop() 中每轮调用，推进所有模块状态机
    static void handle();

    // 手动输出诊断日志（begin() 内部自动调用，也可在运行中随时调用）
    static void logDiagnostics();

    // ---- 信息查询 ----
    static const char* firmwareName();
    static const char* firmwareVersion();
    static const char* hostname();
    static bool isValidHostname(const char* hostname);

private:
    static char _fwName[24];     // 24B
    static char _fwVersion[16];  // 16B
    static char _hostname[33];   // 33B

    static void _resolveHostname();

#if ESP8266BASE_USE_NTP
    static bool _ntpWasTriggered;  // 1B：WiFi 连接后 NTP 已触发
#endif
#if ESP8266BASE_USE_MDNS
    static bool _mdnsWasStarted;   // 1B：WiFi 连接后 mDNS 已启动
#endif
};
