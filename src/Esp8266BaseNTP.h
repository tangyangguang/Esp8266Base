#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseNTP — 网络对时
//
// WiFi 连接后由 Esp8266Base::handle() 调用 begin() 触发
// 对时成功后自动回调 Esp8266BaseLog::setTimeProvider() 切换时间戳格式
// 默认：UTC+8，ntp.aliyun.com / ntp.tencent.com / cn.pool.ntp.org
//
// RAM 预算：<= 160B
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_NTP_TIMEZONE
#define ESP8266BASE_NTP_TIMEZONE 28800   // UTC+8 = 8 * 3600
#endif

#ifndef ESP8266BASE_NTP_SYNC_INTERVAL
#define ESP8266BASE_NTP_SYNC_INTERVAL 3600  // 秒
#endif

class Esp8266BaseNTP {
public:
    // 配置 NTP 并启动 SNTP 客户端（WiFi 连接后自动同步）
    static bool begin();

    // 每轮检查同步状态，成功后切换 Log 时间格式（仅执行一次切换）
    static void handle();

    static bool     isSynced();
    static uint32_t timestamp();                                   // Unix 时间戳，未同步返回 0
    static bool     formatTo(char* out, size_t len, const char* fmt); // strftime 格式化

private:
    static bool     _synced;        // 1B
    static bool     _logSwitched;   // 1B：Log 时间格式是否已切换
    static uint32_t _lastCheckMs;   // 4B

    // Log 时间回调（静态函数，注入 Esp8266BaseLog）
    static const char* _timeStr();
};
