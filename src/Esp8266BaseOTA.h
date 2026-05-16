#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseOTA — Web OTA 固件更新
//
// 依赖 Esp8266BaseWeb 已通过 begin() 启动
// 注册 POST /ota 路由（与 GET /ota 由 Web 模块处理的页面配合）
// 上传期间在 Watchdog 启用时自动 pause/resume，每块后调用 yield()
// /ota 页面和上传 POST 都使用 Esp8266BaseWeb 的 Basic Auth
//
// RAM 预算：<= 160B
// ----------------------------------------------------------------------------

class Esp8266BaseOTA {
public:
    // 注册 POST /ota 路由到 Esp8266BaseWeb::server()
    // 必须在 Esp8266BaseWeb::begin() 之后调用
    static bool begin();

    // OTA 是否正在上传
    static bool isInProgress();

private:
    static bool _inProgress;  // 1B
    static bool _rejected;    // 1B：认证或 Update 初始化失败后拒绝后续块
    static bool _started;     // 1B：本次 POST 是否收到固件起始块
    static bool _watchdogPaused; // 1B：本次 OTA 是否已暂停 Watchdog
    static bool _updateStarted;  // 1B：Update.begin() 是否已成功
    static uint16_t _status;  // 2B：上传完成时返回的 HTTP 状态码
    static uint32_t _startedMs;       // 4B：上传开始 millis
    static uint32_t _uploadedBytes;   // 4B：已写入固件字节数
    static uint32_t _requestBytes;    // 4B：multipart request Content-Length
    static uint8_t  _lastProgressPct; // 1B：最近一次 25% 阶梯进度日志
    static const char* _failureMessage; // 失败时返回给客户端的简短原因

    // 静态回调函数（注册给 ESP8266WebServer，无捕获，无 std::function 驻留堆）
    static void _handleUploadComplete();
    static void _handleUploadChunk();
    static void _pauseWatchdog();
    static void _resumeWatchdog();
    static void _failUpload(uint16_t status, const char* message, bool abortUpdate);
};
