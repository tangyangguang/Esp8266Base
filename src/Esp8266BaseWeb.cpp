#include "Esp8266BaseWeb.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseWiFi.h"

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
ESP8266WebServer         Esp8266BaseWeb::_server(80);
Esp8266BaseWeb::AppRoute Esp8266BaseWeb::_pages[ESP8266BASE_WEB_MAX_APP_PAGES];
Esp8266BaseWeb::AppRoute Esp8266BaseWeb::_apis [ESP8266BASE_WEB_MAX_APP_APIS];
uint8_t                  Esp8266BaseWeb::_pageCount = 0;
uint8_t                  Esp8266BaseWeb::_apiCount  = 0;
bool                     Esp8266BaseWeb::_running   = false;
char                     Esp8266BaseWeb::_authUser[24] = ESP8266BASE_WEB_AUTH_USER;
char                     Esp8266BaseWeb::_authPass[24] = ESP8266BASE_WEB_AUTH_PASS;
char                     Esp8266BaseWeb::_titleBuf[48] = "ESP8266";

// ----------------------------------------------------------------------------
// PROGMEM HTML 片段（全部在 Flash，不占 DRAM）
// ----------------------------------------------------------------------------
static const char WEB_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head>"
    "<meta charset=UTF-8><meta name=viewport content=width=device-width>"
    "<style>"
    "body{font-family:sans-serif;padding:12px;max-width:480px}"
    "h2{margin:0 0 10px}"
    "a,input[type=submit]{background:#2678c8;color:#fff;padding:7px 12px;"
    "text-decoration:none;border:none;border-radius:3px;cursor:pointer;margin:2px 2px 2px 0}"
    "input:not([type=submit]){width:100%;padding:7px;margin:4px 0 12px;"
    "border:1px solid #ccc;border-radius:3px;box-sizing:border-box}"
    "nav{margin-bottom:14px;border-bottom:1px solid #eee;padding-bottom:8px}"
    "nav a{font-size:.88em}.ok{color:green}.err{color:red}"
    ".info{color:#999;font-size:.8em;margin-top:16px}"
    "</style></head><body><nav>";

static const char WEB_NAV_BUILTINS[] PROGMEM =
    "<a href='/'>Home</a>"
    "<a href='/wifi'>WiFi</a>"
    "<a href='/ota'>OTA</a>"
    "<a href='/reboot'>Reboot</a>";

static const char WEB_NAV_END[]    PROGMEM = "</nav>";
static const char WEB_FOOT_PRE[]   PROGMEM = "<div class=info>Free heap: ";
static const char WEB_FOOT_POST[]  PROGMEM = " B</div></body></html>";

static const char WEB_WIFI_FORM[] PROGMEM =
    "<h2>WiFi Settings</h2>"
    "<form method=post>"
    "SSID<input type=text name=ssid maxlength=32 autocomplete=off>"
    "Password<input type=password name=pass maxlength=64>"
    "<input type=submit value='Save &amp; Connect'>"
    "</form>";

static const char WEB_OTA_FORM[] PROGMEM =
    "<h2>OTA Update</h2>"
    "<form method=post enctype=multipart/form-data>"
    "<input type=file name=firmware accept=.bin>"
    "<br><br>"
    "<input type=submit value='Upload Firmware'>"
    "</form>";

static const char WEB_REBOOT_MSG[] PROGMEM =
    "<h2>Reboot</h2><p>Rebooting in 1 second...</p>"
    "<script>setTimeout(()=>location='/',3000)</script>";

// 内部共享小缓冲（用于 sendContent_P 和动态内容，非重入）
static char _wb[160];

// ----------------------------------------------------------------------------
// 公开 API
// ----------------------------------------------------------------------------
bool Esp8266BaseWeb::begin() {
    if (_running) return true;

    // 注册内置路由（静态成员函数指针，无捕获，无 std::function 对象驻留堆）
    _server.on("/",       HTTP_GET,  _handleRoot);
    _server.on("/wifi",   HTTP_GET,  _handleWiFiGet);
    _server.on("/wifi",   HTTP_POST, _handleWiFiPost);
    _server.on("/ota",    HTTP_GET,  _handleOtaGet);
    // POST /ota 由 Esp8266BaseOTA::begin() 注册（需要 upload handler）
    _server.on("/reboot", HTTP_GET,  _handleReboot);
    _server.on("/health", HTTP_GET,  _handleHealth);
    _server.onNotFound(_handleNotFound);

    _server.begin();
    _running = true;
    ESP8266BASE_LOG_I("Web ", "ready=1 auth=1 pages=%d/%d apis=%d/%d",
                      (int)_pageCount, ESP8266BASE_WEB_MAX_APP_PAGES,
                      (int)_apiCount,  ESP8266BASE_WEB_MAX_APP_APIS);
    return true;
}

void Esp8266BaseWeb::handle() {
    if (!_running) return;
    _server.handleClient();
}

bool Esp8266BaseWeb::isRunning() {
    return _running;
}

bool Esp8266BaseWeb::addPage(const char* path, Esp8266BaseWebHandler handler) {
    if (!path || !handler) return false;
    if (strlen(path) >= 24) { ESP8266BASE_LOG_W("Web ", "addPage path too long"); return false; }
    if (_pageCount >= ESP8266BASE_WEB_MAX_APP_PAGES) { ESP8266BASE_LOG_W("Web ", "addPage table full"); return false; }

    strncpy(_pages[_pageCount].path, path, 23);
    _pages[_pageCount].path[23]    = '\0';
    _pages[_pageCount].handler     = handler;
    _pages[_pageCount].isApi       = false;
    _pageCount++;

    _server.on(path, HTTP_GET, handler);
    ESP8266BASE_LOG_D("Web ", "addPage %s", path);
    return true;
}

bool Esp8266BaseWeb::addApi(const char* path, Esp8266BaseWebHandler handler) {
    if (!path || !handler) return false;
    if (strlen(path) >= 24) { ESP8266BASE_LOG_W("Web ", "addApi path too long"); return false; }
    if (_apiCount >= ESP8266BASE_WEB_MAX_APP_APIS) { ESP8266BASE_LOG_W("Web ", "addApi table full"); return false; }

    strncpy(_apis[_apiCount].path, path, 23);
    _apis[_apiCount].path[23]    = '\0';
    _apis[_apiCount].handler     = handler;
    _apis[_apiCount].isApi       = true;
    _apiCount++;

    _server.on(path, handler);   // GET + POST 均响应，handler 内自行区分
    ESP8266BASE_LOG_D("Web ", "addApi %s", path);
    return true;
}

void Esp8266BaseWeb::setAuth(const char* user, const char* pass) {
    if (user) { strncpy(_authUser, user, 23); _authUser[23] = '\0'; }
    if (pass) { strncpy(_authPass, pass, 23); _authPass[23] = '\0'; }
}

void Esp8266BaseWeb::setTitle(const char* hostname, const char* fw, const char* ver) {
    snprintf(_titleBuf, sizeof(_titleBuf), "%s (%s %s)", hostname, fw, ver);
}

ESP8266WebServer& Esp8266BaseWeb::server() {
    return _server;
}

// ----------------------------------------------------------------------------
// Auth
// ----------------------------------------------------------------------------
bool Esp8266BaseWeb::checkAuth() {
    if (_server.authenticate(_authUser, _authPass)) return true;
    _server.requestAuthentication();
    return false;
}

bool Esp8266BaseWeb::verifyAuth() {
    return _server.authenticate(_authUser, _authPass);
}

// ----------------------------------------------------------------------------
// 分段发送辅助
// ----------------------------------------------------------------------------
void Esp8266BaseWeb::sendHeader() {
    _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server.send(200, "text/html", "");
    sendContent_P(WEB_HEAD);
    sendContent_P(WEB_NAV_BUILTINS);
    // 注册的应用页面也加入导航（显示简短路径名）
    for (uint8_t i = 0; i < _pageCount; i++) {
        snprintf(_wb, sizeof(_wb), "<a href='%s'>%s</a>",
                 _pages[i].path, _pages[i].path + 1);
        _server.sendContent(_wb);
    }
    sendContent_P(WEB_NAV_END);
}

void Esp8266BaseWeb::sendFooter() {
    sendContent_P(WEB_FOOT_PRE);
    snprintf(_wb, sizeof(_wb), "%u", (unsigned)ESP.getFreeHeap());
    _server.sendContent(_wb);
    sendContent_P(WEB_FOOT_POST);
}

void Esp8266BaseWeb::sendContent_P(PGM_P content) {
    // 单遍从 PROGMEM 逐字节读取并分块发送，避免 strlen_P 二次遍历
    char buf[128];
    size_t chunk = 0;
    uint8_t c;
    while ((c = pgm_read_byte(content++)) != 0) {
        buf[chunk++] = (char)c;
        if (chunk == sizeof(buf) - 1) {
            buf[chunk] = '\0';
            _server.sendContent(buf);
            chunk = 0;
        }
    }
    if (chunk > 0) {
        buf[chunk] = '\0';
        _server.sendContent(buf);
    }
}

void Esp8266BaseWeb::sendChunk(const char* content) {
    if (content) _server.sendContent(content);
}

// ----------------------------------------------------------------------------
// 内置路由处理函数
// ----------------------------------------------------------------------------
void Esp8266BaseWeb::_handleRoot() {
    if (!checkAuth()) return;
    sendHeader();
    snprintf(_wb, sizeof(_wb), "<h2>%s</h2>", _titleBuf);
    _server.sendContent(_wb);

    if (Esp8266BaseWiFi::isConnected()) {
        snprintf(_wb, sizeof(_wb),
                 "<p>WiFi: <b>Connected</b><br>IP: %s<br>Uptime: %lus</p>",
                 Esp8266BaseWiFi::ip(), millis() / 1000UL);
        _server.sendContent(_wb);
    } else if (Esp8266BaseWiFi::state() == Esp8266BaseWiFiState::AP_CONFIG) {
        snprintf(_wb, sizeof(_wb),
                 "<p>WiFi: <b>AP Mode</b> (%s)<br>IP: 192.168.4.1</p>",
                 Esp8266BaseWiFi::apSSID());
        _server.sendContent(_wb);
    } else {
        _server.sendContent("<p>WiFi: Connecting...</p>");
    }
    sendFooter();
}

void Esp8266BaseWeb::_handleWiFiGet() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_WIFI_FORM);
    // 显示当前已保存的 SSID
    char ssid[64] = "";
    Esp8266BaseConfig::getStr("wifi_ssid", ssid, sizeof(ssid), "");
    if (strlen(ssid) > 0) {
        snprintf(_wb, sizeof(_wb), "<p>Saved SSID: <b>%s</b></p>", ssid);
        _server.sendContent(_wb);
    }
    sendFooter();
}

void Esp8266BaseWeb::_handleWiFiPost() {
    if (!checkAuth()) return;

    char ssid[64] = "";
    char pass[64] = "";
    strncpy(ssid, _server.arg("ssid").c_str(), sizeof(ssid) - 1);
    strncpy(pass, _server.arg("pass").c_str(), sizeof(pass) - 1);

    sendHeader();
    sendContent_P(WEB_WIFI_FORM);
    if (strlen(ssid) > 0) {
        Esp8266BaseWiFi::connect(ssid, pass);
        snprintf(_wb, sizeof(_wb),
                 "<p class=ok>Saved. Connecting to <b>%s</b>...</p>", ssid);
        _server.sendContent(_wb);
        ESP8266BASE_LOG_I("Web ", "WiFi credentials updated ssid=%s", ssid);
    } else {
        _server.sendContent("<p class=err>SSID cannot be empty.</p>");
    }
    sendFooter();
}

void Esp8266BaseWeb::_handleOtaGet() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_OTA_FORM);
    sendFooter();
}

void Esp8266BaseWeb::_handleReboot() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_REBOOT_MSG);
    sendFooter();
    _server.client().stop();
    ESP8266BASE_LOG_I("Web ", "Reboot requested");
    Esp8266BaseConfig::flush();
    delay(500);
    ESP.restart();
}

void Esp8266BaseWeb::_handleHealth() {
    // 不需要 Auth（健康检查通常开放）
    snprintf(_wb, sizeof(_wb),
             "{\"heap\":%u,\"maxBlock\":%u,\"ip\":\"%s\","
             "\"uptime\":%lu,\"wifi\":\"%s\"}",
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMaxFreeBlockSize(),
             Esp8266BaseWiFi::ip(),
             millis() / 1000UL,
             Esp8266BaseWiFi::isConnected() ? "connected" : "disconnected");
    _server.send(200, "application/json", _wb);
}

void Esp8266BaseWeb::_handleNotFound() {
    _server.send(404, "text/plain", "Not found");
}
