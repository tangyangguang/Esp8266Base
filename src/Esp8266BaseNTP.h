#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseNTP — 网络对时
//
// WiFi 连接后由 Esp8266Base::handle() 调用 begin() 触发
// 对时成功后自动回调 Esp8266BaseLog::setTimeProvider() 切换时间戳格式
// 默认：UTC+8，ntp.aliyun.com / ntp.tencent.com / cn.pool.ntp.org
//
// RAM 预算：<= 224B
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_NTP_TIMEZONE
#define ESP8266BASE_NTP_TIMEZONE 28800   // UTC+8 = 8 * 3600
#endif

#ifndef ESP8266BASE_NTP_SYNC_INTERVAL
#define ESP8266BASE_NTP_SYNC_INTERVAL 3600  // 秒
#endif

#ifndef ESP8266BASE_NTP_SERVER_1
#define ESP8266BASE_NTP_SERVER_1 "ntp.aliyun.com"
#endif

#ifndef ESP8266BASE_NTP_SERVER_2
#define ESP8266BASE_NTP_SERVER_2 "ntp.tencent.com"
#endif

#ifndef ESP8266BASE_NTP_SERVER_3
#define ESP8266BASE_NTP_SERVER_3 "cn.pool.ntp.org"
#endif

class Esp8266BaseNTP {
public:
    // 配置 NTP 并启动 SNTP 客户端（WiFi 连接后自动同步）
    static bool begin();

    // 每轮检查同步状态，成功后切换 Log 时间格式（仅执行一次切换）
    static void handle();

    // WiFi 断开时调用，释放 UDP 并让下次连接重新 begin()
    static void reset();

    static bool     isSynced();
    static uint32_t timestamp();                                   // Unix 时间戳，未同步返回 0
    static bool     formatTo(char* out, size_t len, const char* fmt); // strftime 格式化

    // 主动 UDP NTP 路径可提供本次同步的 RTT 与保守估计误差；系统 SNTP
    // 路径没有可读取的测量证据，因此 hasMeasuredUncertainty() 返回 false。
    static bool     hasMeasuredUncertainty();
    static uint32_t lastSyncRttMs();
    static uint16_t estimatedUncertaintyMs();

private:
    static bool     _synced;        // 1B
    static bool     _logSwitched;   // 1B：Log 时间格式是否已切换
    static uint32_t _lastCheckMs;   // 4B
    static uint32_t _startedMs;     // 4B：本次 NTP 启动时间
    static uint32_t _lastPendingLogMs; // 4B：未同步状态日志节流
    static uint32_t _nextManualMs;  // 4B：库内 UDP NTP 下次请求时间
    static uint32_t _manualSentMs;  // 4B：库内 UDP NTP 请求发送时间
    static uint8_t  _manualServer;  // 1B：当前库内 UDP NTP 服务器索引
    static bool     _manualWaiting; // 1B：正在等待库内 UDP NTP 响应
    static bool     _uncertaintyMeasured; // 1B：主动 NTP 有 RTT 证据
    static uint16_t _estimatedUncertaintyMs; // 2B：保守估计，不伪装硬上界
    static uint32_t _lastSyncRttMs; // 4B

    static bool _pollManual(uint32_t now);
    static bool _isDue(uint32_t now, uint32_t due);
    static void _sendManual(uint32_t now);
    static void _finishSync(time_t t);

    // Log 时间回调（静态函数，注入 Esp8266BaseLog）
    static const char* _timeStr();
};
