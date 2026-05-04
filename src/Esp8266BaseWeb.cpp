#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_WEB
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseWiFi.h"
#include "Esp8266BaseUtil.h"

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
char                     Esp8266BaseWeb::_activeUri[32] = "";
char                     Esp8266BaseWeb::_activeMethod[5] = "";

// ----------------------------------------------------------------------------
// PROGMEM HTML 片段（全部在 Flash，不占 DRAM）
// ----------------------------------------------------------------------------
static const char WEB_HEAD[] PROGMEM =
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
    "</style>"
    "<script>"
    "function once(f){if(f.dataset.busy)return false;f.dataset.busy=1;"
    "var b=f.querySelector('[type=submit]');if(b)b.disabled=true;return true;}"
    "</script></head><body><nav>";

static const char WEB_NAV_BUILTINS[] PROGMEM =
    "<a href='/'>Home</a>"
    "<a href='/wifi'>WiFi</a>"
    "<a href='/ota'>OTA</a>"
    "<a href='/reboot'>Reboot</a>";

static const char WEB_NAV_END[]    PROGMEM = "</nav>";
static const char WEB_FOOT_PRE[]   PROGMEM = "<div class=info>Free heap: ";
static const char WEB_FOOT_POST[]  PROGMEM = "</div></body></html>";

static const char WEB_WIFI_FORM_PRE[] PROGMEM =
    "<h2>WiFi Settings</h2>"
    "<form method=post onsubmit=\""
    "var s=this.ssid.value.trim(),p=this.pass.value.trim();"
    "if(!s){alert('SSID cannot be empty');return false;}"
    "if(!p){alert('Password cannot be empty');return false;}"
    "return once(this);\">"
    "SSID<input type=text name=ssid maxlength=32 autocomplete=off required value=\"";

static const char WEB_WIFI_FORM_MID[] PROGMEM =
    "\">Password<input id=wp type=password name=pass maxlength=64 required value=\"";

static const char WEB_WIFI_FORM_POST[] PROGMEM =
    "\"><input type=button value='Show/Hide Password' onclick=\""
    "var p=document.getElementById('wp');p.type=p.type=='password'?'text':'password'\">"
    "<input type=submit value='Save &amp; Connect'>"
    "</form>";

static const char WEB_OTA_FORM[] PROGMEM =
    "<h2>OTA Update</h2>"
    "<form id=f>"
    "<input id=fw type=file name=firmware accept=.bin required>"
    "<br><br>"
    "<input type=submit value='Upload Firmware'>"
    "</form>"
    "<progress id=pg value=0 max=100 style='width:100%;display:none'></progress>"
    "<p id=st></p>"
    "<script>"
    "document.getElementById('f').onsubmit=function(e){"
    "e.preventDefault();"
    "if(this.dataset.busy)return false;this.dataset.busy=1;"
    "var file=document.getElementById('fw').files[0],st=document.getElementById('st'),pg=document.getElementById('pg');"
    "var b=this.querySelector('[type=submit]');"
    "if(!file){this.dataset.busy='';alert('Choose firmware first');return false;}"
    "var fd=new FormData();fd.append('firmware',file);"
    "function fb(n){if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return(n/1048576).toFixed(1)+' MB';}"
    "var x=new XMLHttpRequest();if(b)b.disabled=true;pg.style.display='block';pg.value=0;st.textContent='Uploading 0%';"
    "x.upload.onprogress=function(ev){if(ev.lengthComputable){var p=Math.floor(ev.loaded*100/ev.total);pg.value=p;st.textContent='Uploading '+p+'% ('+fb(ev.loaded)+'/'+fb(ev.total)+')';}};"
    "x.onload=function(){st.textContent=(x.status==200?x.responseText:('Upload failed: HTTP '+x.status+' '+x.responseText));if(x.status!=200){document.getElementById('f').dataset.busy='';if(b)b.disabled=false;}};"
    "x.onerror=function(){st.textContent='Upload failed: network error';document.getElementById('f').dataset.busy='';if(b)b.disabled=false;};"
    "x.open('POST','/ota');x.send(fd);return false;};"
    "</script>";

static const char WEB_REBOOT_CONFIRM[] PROGMEM =
    "<h2>Reboot</h2>"
    "<p>Are you sure you want to reboot the device?</p>"
    "<form method=post onsubmit=\"return confirm('Reboot device now?')&&once(this)\">"
    "<input type=submit value='Confirm Reboot' style='background:#c33'>"
    " <a href='/'>Cancel</a>"
    "</form>";

static const char WEB_REBOOTING[] PROGMEM =
    "<h2>Rebooting...</h2>"
    "<p>Device is restarting. Please wait a few seconds, then <a href='/'>reload</a>.</p>";

// 内部共享小缓冲（用于 sendContent_P 和动态内容，非重入）
static char _wb[160];

static void _trimWhitespace(char* s) {
    if (!s) return;
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[--len] = '\0';
    }
}

static void _trimLeadingWhitespace(char* s) {
    if (!s) return;
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static void _sendAttrEscaped(const char* s) {
    if (!s) return;
    size_t out = 0;
    while (*s) {
        const char* repl = nullptr;
        switch (*s) {
            case '&':  repl = "&amp;"; break;
            case '"':  repl = "&quot;"; break;
            case '<':  repl = "&lt;"; break;
            case '>':  repl = "&gt;"; break;
            default: break;
        }
        if (repl) {
            for (const char* r = repl; *r; r++) {
                _wb[out++] = *r;
                if (out == sizeof(_wb) - 1) {
                    _wb[out] = '\0';
                    Esp8266BaseWeb::sendChunk(_wb);
                    out = 0;
                }
            }
        } else {
            _wb[out++] = *s;
            if (out == sizeof(_wb) - 1) {
                _wb[out] = '\0';
                Esp8266BaseWeb::sendChunk(_wb);
                out = 0;
            }
        }
        s++;
    }
    if (out > 0) {
        _wb[out] = '\0';
        Esp8266BaseWeb::sendChunk(_wb);
    }
}

static void _redirect(const char* url) {
    Esp8266BaseWeb::server().sendHeader("Location", url);
    Esp8266BaseWeb::server().sendHeader("Cache-Control", "no-store");
    Esp8266BaseWeb::server().send(303);
}

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
    _server.on("/reboot", HTTP_GET,  _handleRebootGet);
    _server.on("/reboot", HTTP_POST, _handleRebootPost);
    _server.on("/health", HTTP_GET,  _handleHealth);
    _server.onNotFound(_handleNotFound);

    _server.begin();
    _running = true;
    ESP8266BASE_LOG_I("Web ", "web_server_started auth_required=yes builtin_routes=8 app_pages_registered=%d/%d app_apis_registered=%d/%d",
                      (int)_pageCount, ESP8266BASE_WEB_MAX_APP_PAGES,
                      (int)_apiCount,  ESP8266BASE_WEB_MAX_APP_APIS);
    return true;
}

void Esp8266BaseWeb::handle() {
    if (!_running) return;
    _activeUri[0] = '\0';
    _activeMethod[0] = '\0';
    uint32_t start = millis();
    _server.handleClient();
    uint32_t elapsed = millis() - start;
    if (elapsed > 250UL) {
        const char* method = _activeMethod[0] ? _activeMethod : "?";
        const char* uri = _activeUri[0] ? _activeUri : "(unknown)";
        ESP8266BASE_LOG_W("Web ", "slow_request method=%s uri=%s elapsed=%lums",
                          method, uri, (unsigned long)elapsed);
    }
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
    uint8_t index = _pageCount;
    _pageCount++;

    switch (index) {
        case 0: _server.on(path, HTTP_GET, _handleAppPage0); break;
        case 1: _server.on(path, HTTP_GET, _handleAppPage1); break;
        case 2: _server.on(path, HTTP_GET, _handleAppPage2); break;
        case 3: _server.on(path, HTTP_GET, _handleAppPage3); break;
        default: return false;
    }
    ESP8266BASE_LOG_I("Web ", "app_page_registered path=%s app_pages_registered=%d/%d",
                      path, (int)_pageCount, ESP8266BASE_WEB_MAX_APP_PAGES);
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
    uint8_t index = _apiCount;
    _apiCount++;

    switch (index) {
        case 0: _server.on(path, _handleAppApi0); break;
        case 1: _server.on(path, _handleAppApi1); break;
        case 2: _server.on(path, _handleAppApi2); break;
        case 3: _server.on(path, _handleAppApi3); break;
        case 4: _server.on(path, _handleAppApi4); break;
        case 5: _server.on(path, _handleAppApi5); break;
        default: return false;
    }
    ESP8266BASE_LOG_I("Web ", "app_api_registered path=%s app_apis_registered=%d/%d",
                      path, (int)_apiCount, ESP8266BASE_WEB_MAX_APP_APIS);
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

void Esp8266BaseWeb::_markRequest() {
    strncpy(_activeMethod, (_server.method() == HTTP_POST) ? "POST" : "GET",
            sizeof(_activeMethod) - 1);
    _activeMethod[sizeof(_activeMethod) - 1] = '\0';

    String uri = _server.uri();
    strncpy(_activeUri, uri.c_str(), sizeof(_activeUri) - 1);
    _activeUri[sizeof(_activeUri) - 1] = '\0';
}

void Esp8266BaseWeb::_handleAppPage(uint8_t index) {
    _markRequest();
    if (index < _pageCount && _pages[index].handler) {
        _pages[index].handler();
    } else {
        _server.send(404, "text/plain", "Page route not found");
    }
}

void Esp8266BaseWeb::_handleAppApi(uint8_t index) {
    _markRequest();
    if (index < _apiCount && _apis[index].handler) {
        _apis[index].handler();
    } else {
        _server.send(404, "text/plain", "API route not found");
    }
}

void Esp8266BaseWeb::_handleAppPage0() { _handleAppPage(0); }
void Esp8266BaseWeb::_handleAppPage1() { _handleAppPage(1); }
void Esp8266BaseWeb::_handleAppPage2() { _handleAppPage(2); }
void Esp8266BaseWeb::_handleAppPage3() { _handleAppPage(3); }
void Esp8266BaseWeb::_handleAppApi0()  { _handleAppApi(0); }
void Esp8266BaseWeb::_handleAppApi1()  { _handleAppApi(1); }
void Esp8266BaseWeb::_handleAppApi2()  { _handleAppApi(2); }
void Esp8266BaseWeb::_handleAppApi3()  { _handleAppApi(3); }
void Esp8266BaseWeb::_handleAppApi4()  { _handleAppApi(4); }
void Esp8266BaseWeb::_handleAppApi5()  { _handleAppApi(5); }

// ----------------------------------------------------------------------------
// 分段发送辅助
// ----------------------------------------------------------------------------
void Esp8266BaseWeb::sendHeader() {
    _markRequest();
    WiFiClient& client = _server.client();
    client.setNoDelay(true);
    client.setTimeout(1500);
    client.print(F("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/html; charset=utf-8\r\n"
                   "Connection: close\r\n"
                   "Cache-Control: no-store\r\n\r\n"));
    sendChunk("<!DOCTYPE html><html><head><title>");
    _sendAttrEscaped(_titleBuf);
    sendChunk("</title>");
    sendContent_P(WEB_HEAD);
    sendContent_P(WEB_NAV_BUILTINS);
    // 注册的应用页面也加入导航（显示简短路径名）
    for (uint8_t i = 0; i < _pageCount; i++) {
        snprintf(_wb, sizeof(_wb), "<a href='%s'>%s</a>",
                 _pages[i].path, _pages[i].path + 1);
        sendChunk(_wb);
    }
    sendContent_P(WEB_NAV_END);
}

void Esp8266BaseWeb::sendFooter() {
    sendContent_P(WEB_FOOT_PRE);
    Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), _wb, sizeof(_wb));
    sendChunk(_wb);
    sendContent_P(WEB_FOOT_POST);
    _server.client().flush();
    _server.client().stop();
    yield();
}

void Esp8266BaseWeb::sendContent_P(PGM_P content) {
    // 单遍从 PROGMEM 逐字节读取并分块发送，避免 strlen_P 二次遍历
    char buf[128];
    size_t chunk = 0;
    uint8_t c;
    while ((c = pgm_read_byte(content++)) != 0) {
        buf[chunk++] = (char)c;
        if (chunk == sizeof(buf) - 1) {
            _server.client().write((const uint8_t*)buf, chunk);
            yield();
            chunk = 0;
        }
    }
    if (chunk > 0) {
        _server.client().write((const uint8_t*)buf, chunk);
        yield();
    }
}

void Esp8266BaseWeb::sendChunk(const char* content) {
    if (content) {
        _server.client().write((const uint8_t*)content, strlen(content));
        yield();
    }
}

// ----------------------------------------------------------------------------
// 内置路由处理函数
// ----------------------------------------------------------------------------
void Esp8266BaseWeb::_handleRoot() {
    if (!checkAuth()) return;
    sendHeader();
        snprintf(_wb, sizeof(_wb), "<h2>%s</h2>", _titleBuf);
    sendChunk(_wb);

    if (Esp8266BaseWiFi::isConnected()) {
        snprintf(_wb, sizeof(_wb),
                 "<p>WiFi: <b>Connected</b><br>IP: %s<br>Uptime: %lus</p>",
                 Esp8266BaseWiFi::ip(), millis() / 1000UL);
        sendChunk(_wb);
    } else if (Esp8266BaseWiFi::state() == Esp8266BaseWiFiState::AP_CONFIG) {
        snprintf(_wb, sizeof(_wb),
                 "<p>WiFi: <b>AP Mode</b> (%s)<br>IP: 192.168.4.1</p>",
                 Esp8266BaseWiFi::apSSID());
        sendChunk(_wb);
    } else {
        sendChunk("<p>WiFi: Connecting...</p>");
    }
    sendFooter();
}

void Esp8266BaseWeb::_handleWiFiGet() {
    if (!checkAuth()) return;
    sendHeader();
    if (_server.hasArg("saved")) {
        sendChunk("<p class=ok>Saved. Credentials updated and connection started.</p>");
    } else if (_server.hasArg("error")) {
        char err[24] = "";
        strncpy(err, _server.arg("error").c_str(), sizeof(err) - 1);
        if (strcmp(err, "missing_ssid") == 0) {
            sendChunk("<p class=err>SSID cannot be empty.</p>");
        } else if (strcmp(err, "missing_password") == 0) {
            sendChunk("<p class=err>Password cannot be empty.</p>");
        } else if (strcmp(err, "save_failed") == 0) {
            sendChunk("<p class=err>Failed to save WiFi credentials.</p>");
        } else {
            sendChunk("<p class=err>WiFi settings were not saved.</p>");
        }
    }

    char ssid[64] = "";
    char pass[64] = "";
    Esp8266BaseConfig::getStr("wifi_ssid", ssid, sizeof(ssid), "");
    Esp8266BaseConfig::getStr("wifi_pass", pass, sizeof(pass), "");
    sendContent_P(WEB_WIFI_FORM_PRE);
    _sendAttrEscaped(ssid);
    sendContent_P(WEB_WIFI_FORM_MID);
    _sendAttrEscaped(pass);
    sendContent_P(WEB_WIFI_FORM_POST);
    sendFooter();
}

void Esp8266BaseWeb::_handleWiFiPost() {
    if (!checkAuth()) return;

    char ssid[64] = "";
    char pass[64] = "";
    strncpy(ssid, _server.arg("ssid").c_str(), sizeof(ssid) - 1);
    strncpy(pass, _server.arg("pass").c_str(), sizeof(pass) - 1);
    _trimWhitespace(ssid);
    _trimLeadingWhitespace(pass);

    if (strlen(ssid) > 0 && strlen(pass) > 0) {
        // Intentionally log the WiFi password in plaintext for field debugging.
        ESP8266BASE_LOG_I("Web ", "wifi_credentials_form_submitted ssid=%s password=%s password_length=%u",
                          ssid, pass, (unsigned)strlen(pass));
        if (Esp8266BaseWiFi::connect(ssid, pass)) {
            _redirect("/wifi?saved=1");
        } else {
            _redirect("/wifi?error=save_failed");
        }
    } else if (strlen(ssid) == 0) {
        _redirect("/wifi?error=missing_ssid");
    } else {
        _redirect("/wifi?error=missing_password");
    }
}

void Esp8266BaseWeb::_handleOtaGet() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_OTA_FORM);
    sendFooter();
}

void Esp8266BaseWeb::_handleRebootGet() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_REBOOT_CONFIRM);
    sendFooter();
}

void Esp8266BaseWeb::_handleRebootPost() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_REBOOTING);
    sendFooter();
    _server.client().stop();
    ESP8266BASE_LOG_I("Web ", "reboot_requested source=web");
    Esp8266BaseConfig::flush();
    delay(500);
    ESP.restart();
}

void Esp8266BaseWeb::_handleHealth() {
    _markRequest();
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
    _markRequest();
    _server.send(404, "text/plain", "Not found");
}
#endif
