#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>

// ----------------------------------------------------------------------------
// Esp8266BaseWeb — 极简管理 Web
//
// 内置路由：GET / GET /esp8266base GET/POST /wifi GET/POST /auth GET/POST /ota GET /logs POST /logs/clear GET/POST /reboot GET /health
// 应用扩展：最多 4 页面 + 6 API（静态数组，不动态分配）
// Basic Auth 默认开启
// HTML 全部放 PROGMEM，动态响应分段发送
//
// RAM 预算：<= 800B（路由表 + 导航配置 + 状态 + 认证字段）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_WEB_MAX_APP_PAGES
#define ESP8266BASE_WEB_MAX_APP_PAGES 4
#endif

#ifndef ESP8266BASE_WEB_MAX_APP_APIS
#define ESP8266BASE_WEB_MAX_APP_APIS 6
#endif

#if ESP8266BASE_WEB_MAX_APP_PAGES > 4
#error "ESP8266BASE_WEB_MAX_APP_PAGES cannot exceed 4"
#endif

#if ESP8266BASE_WEB_MAX_APP_APIS > 6
#error "ESP8266BASE_WEB_MAX_APP_APIS cannot exceed 6"
#endif

#ifndef ESP8266BASE_WEB_AUTH_USER
#define ESP8266BASE_WEB_AUTH_USER "admin"
#endif

#ifndef ESP8266BASE_WEB_AUTH_PASS
#define ESP8266BASE_WEB_AUTH_PASS "esp8266"
#endif

typedef void (*Esp8266BaseWebHandler)();

enum class Esp8266BaseWebHomeMode : uint8_t {
    DEFAULT_SYSTEM_HOME = 0,
    APP_HOME_FIRST     = 1,
    FUSED_HOME         = 2
};

enum class Esp8266BaseWebSystemNavMode : uint8_t {
    TOP_NAV        = 0,
    BOTTOM_NAV     = 1,
    FOOTER_COMPACT = 2
};

enum class Esp8266BaseWebBuiltinLabel : uint8_t {
    HOME = 0,
    WIFI = 1,
    OTA = 2,
    LOGS = 3,
    AUTH = 4,
    REBOOT = 5
};

class Esp8266BaseWeb {
public:
    static bool begin();
    static void handle();
    static bool isRunning();

    // 注册应用自定义路由（begin() 之后调用）
    static bool addPage(const char* path, Esp8266BaseWebHandler handler);
    static bool addPage(const char* path, const char* title, Esp8266BaseWebHandler handler);
    static bool addApi (const char* path, Esp8266BaseWebHandler handler);
    static bool addNavItem(const char* path, const char* title);

    // 应用 Web 信息架构配置（通常在 begin() 前调用）
    static void setDeviceName(const char* name);
    static void setHomePath(const char* path);
    static void setHomeMode(Esp8266BaseWebHomeMode mode);
    static void setSystemNavMode(Esp8266BaseWebSystemNavMode mode);
    static void setBuiltinLabel(Esp8266BaseWebBuiltinLabel label, const char* title);

    // 应用 handler 辅助函数（在自定义 handler 内使用）
    static void sendHeader();                     // 输出 HTTP 头 + HTML head + 导航栏
    static void sendFooter();                     // 输出 HTML 尾（含 free heap）
    static void sendContent_P(PGM_P content);     // 从 PROGMEM 流式输出
    static void sendChunk(const char* content);   // 流式输出动态内容块
    static bool checkAuth();                      // 验证 Basic Auth，失败自动返回 401
    static bool verifyAuth();                     // 仅验证，不发 401
    static void setDefaultAuth(const char* user, const char* pass);

    // 暴露底层 server，供需要直接操作的 handler 使用
    static ESP8266WebServer& server();

    // 供 Esp8266Base 在 begin() 时设置设备标题
    static void setTitle(const char* hostname, const char* fw, const char* ver);

private:
    struct AppRoute {
        char                  path[24];   // 24B
        char                  title[18];  // 18B
        Esp8266BaseWebHandler handler;    // 4B
        bool                  isApi;      // 1B
        bool                  showInNav;  // 1B
    };

    static ESP8266WebServer _server;                            // ~4B ref
    static AppRoute         _pages[ESP8266BASE_WEB_MAX_APP_PAGES]; // 4×48=192B
    static AppRoute         _apis [ESP8266BASE_WEB_MAX_APP_APIS];  // 6×48=288B
    static uint8_t          _pageCount;                         // 1B
    static uint8_t          _apiCount;                          // 1B
    static bool             _running;                           // 1B
    static char             _authUser[24];                      // 24B
    static char             _authPass[24];                      // 24B
    static char             _deviceName[24];                    // 24B
    static char             _homePath[24];                      // 24B
    static char             _titleBuf[48];                      // "hostname (fw ver)" 48B
    static char             _activeUri[32];                     // 当前请求 URI，用于慢请求日志
    static char             _activeMethod[5];                   // GET/POST
    static char             _builtinLabels[6][16];              // Home/WiFi/OTA/Logs/Auth/Reboot
    static Esp8266BaseWebHomeMode      _homeMode;
    static Esp8266BaseWebSystemNavMode _systemNavMode;

    // 内置路由处理函数（静态，无捕获）
    static void _markRequest();
    static void _handleAppPage(uint8_t index);
    static void _handleAppApi(uint8_t index);
    static void _handleAppPage0();
    static void _handleAppPage1();
    static void _handleAppPage2();
    static void _handleAppPage3();
    static void _handleAppApi0();
    static void _handleAppApi1();
    static void _handleAppApi2();
    static void _handleAppApi3();
    static void _handleAppApi4();
    static void _handleAppApi5();
    static void _handleRoot();
    static void _handleSystemHome();
    static void _handleWiFiGet();
    static void _handleWiFiPost();
    static void _handleAuthGet();
    static void _handleAuthPost();
    static void _handleOtaGet();   // OTA GET 由此处理，POST 由 Esp8266BaseOTA 注册
    static void _handleLogsGet();
    static void _handleLogsClearPost();
    static void _handleRebootGet();
    static void _handleRebootPost();
    static void _handleHealth();
    static void _handleNotFound();
    static const char* _builtinLabel(Esp8266BaseWebBuiltinLabel label);
    static const char* _brandTitle();
    static const char* _brandHref();
    static void _sendLink(const char* path, const char* title, const char* cls = nullptr);
    static void _sendAppLinks();
    static void _sendSystemLinks();
    static void _loadPersistedAuth();
};
