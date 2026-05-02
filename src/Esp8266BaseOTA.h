#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseOTA — Web OTA 固件更新
//
// 依赖 Esp8266BaseWeb 已通过 begin() 启动
// 注册 POST /ota 路由（与 GET /ota 由 Web 模块处理的页面配合）
// 上传期间自动 pause/resume Watchdog，每块后调用 yield()
// /ota 页面复用 Esp8266BaseWeb 的 Basic Auth；上传 POST 不做额外认证
//
// RAM 预算：<= 128B
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

    // 静态回调函数（注册给 ESP8266WebServer，无捕获，无 std::function 驻留堆）
    static void _handleUploadComplete();
    static void _handleUploadChunk();
};
