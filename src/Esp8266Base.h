#pragma once
#include <Arduino.h>

// Phase 1 核心模块（已实现）
#include "Esp8266BaseLog.h"
#include "Esp8266BaseUtil.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseWiFi.h"

// Phase 2+ 模块（声明已就绪，实现在各自 Phase 完成）
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseOTA.h"
#include "Esp8266BaseNTP.h"
#include "Esp8266BaseMDNS.h"
#include "Esp8266BaseSleep.h"
#include "Esp8266BaseWatchdog.h"

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

    // 设置设备 hostname（mDNS + AP SSID 后缀），最长 24 字符
    static void setHostname(const char* hostname);

    // 控制各模块是否启用（默认全部 true）
    static void enableWeb(bool enabled);
    static void enableOTA(bool enabled);
    static void enableNTP(bool enabled);
    static void enableMDNS(bool enabled);
    static void enableWatchdog(bool enabled);

    // ---- 核心 API ----

    // 按序初始化：Log → Sleep → Config → WiFi → Watchdog → Web → OTA → 诊断日志
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

private:
    static char _fwName[24];     // 24B
    static char _fwVersion[16];  // 16B
    static char _hostname[24];   // 24B

    static bool _webEnabled;       // 1B
    static bool _otaEnabled;       // 1B
    static bool _ntpEnabled;       // 1B
    static bool _mdnsEnabled;      // 1B
    static bool _watchdogEnabled;  // 1B

    static bool _ntpWasTriggered;  // 1B：WiFi 连接后 NTP 已触发
    static bool _mdnsWasStarted;   // 1B：WiFi 连接后 mDNS 已启动
};
