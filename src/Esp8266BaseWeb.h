#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>

// ----------------------------------------------------------------------------
// Esp8266BaseWeb — 极简管理 Web
//
// 内置路由：GET/ GET/wifi POST/wifi GET/ota POST/ota GET/reboot GET/health
// 应用扩展：最多 4 页面 + 6 API（静态数组，不动态分配）
// Basic Auth 默认开启
// HTML 全部放 PROGMEM，动态响应分段发送
//
// RAM 预算：<= 512B（路由表 + 状态 + 认证字段）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_WEB_MAX_APP_PAGES
#define ESP8266BASE_WEB_MAX_APP_PAGES 4
#endif

#ifndef ESP8266BASE_WEB_MAX_APP_APIS
#define ESP8266BASE_WEB_MAX_APP_APIS 6
#endif

#ifndef ESP8266BASE_WEB_AUTH_USER
#define ESP8266BASE_WEB_AUTH_USER "admin"
#endif

#ifndef ESP8266BASE_WEB_AUTH_PASS
#define ESP8266BASE_WEB_AUTH_PASS "esp8266"
#endif

typedef void (*Esp8266BaseWebHandler)();

class Esp8266BaseWeb {
public:
    static bool begin();
    static void handle();
    static bool isRunning();

    // 注册应用自定义路由（begin() 之后调用）
    static bool addPage(const char* path, Esp8266BaseWebHandler handler);
    static bool addApi (const char* path, Esp8266BaseWebHandler handler);

    // 应用 handler 辅助函数（在自定义 handler 内使用）
    static void sendHeader();                     // 输出 HTML 头 + 导航栏（开始 chunked 传输）
    static void sendFooter();                     // 输出 HTML 尾（含 free heap）
    static void sendContent_P(PGM_P content);     // 从 PROGMEM 分段发送
    static void sendChunk(const char* content);   // 发送动态内容块
    static bool checkAuth();                      // 验证 Basic Auth，失败自动返回 401
    static bool verifyAuth();                     // 仅验证，不发 401
    static void setAuth(const char* user, const char* pass);

    // 暴露底层 server，供需要直接操作的 handler 使用
    static ESP8266WebServer& server();

    // 供 Esp8266Base 在 begin() 时设置设备标题
    static void setTitle(const char* hostname, const char* fw, const char* ver);

private:
    struct AppRoute {
        char                  path[24];   // 24B
        Esp8266BaseWebHandler handler;    // 4B
        bool                  isApi;      // 1B
        // padding 3B → 32B per entry
    };

    static ESP8266WebServer _server;                            // ~4B ref
    static AppRoute         _pages[ESP8266BASE_WEB_MAX_APP_PAGES]; // 4×32=128B
    static AppRoute         _apis [ESP8266BASE_WEB_MAX_APP_APIS];  // 6×32=192B
    static uint8_t          _pageCount;                         // 1B
    static uint8_t          _apiCount;                          // 1B
    static bool             _running;                           // 1B
    static char             _authUser[24];                      // 24B
    static char             _authPass[24];                      // 24B
    static char             _titleBuf[48];                      // "hostname (fw ver)" 48B

    // 内置路由处理函数（静态，无捕获）
    static void _handleRoot();
    static void _handleWiFiGet();
    static void _handleWiFiPost();
    static void _handleOtaGet();   // OTA GET 由此处理，POST 由 Esp8266BaseOTA 注册
    static void _handleRebootGet();
    static void _handleRebootPost();
    static void _handleHealth();
    static void _handleNotFound();
};
