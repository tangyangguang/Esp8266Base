#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_WEB
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseFileLog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseWiFi.h"
#include "Esp8266BaseUtil.h"
#include "Esp8266Base.h"
#if ESP8266BASE_USE_NTP
#include "Esp8266BaseNTP.h"
#endif
#if ESP8266BASE_USE_SLEEP
#include "Esp8266BaseSleep.h"
#endif
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#endif
#include <LittleFS.h>
#include <time.h>

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
char                     Esp8266BaseWeb::_deviceName[24] = "";
char                     Esp8266BaseWeb::_homePath[24] = "";
char                     Esp8266BaseWeb::_hostname[33] = "esp8266base";
char                     Esp8266BaseWeb::_fwName[24] = "esp8266base";
char                     Esp8266BaseWeb::_fwVersion[16] = "1.0.0";
uint32_t                 Esp8266BaseWeb::_bootCount = 0;
char                     Esp8266BaseWeb::_titleBuf[80] = "ESP8266";
char                     Esp8266BaseWeb::_activeUri[32] = "";
char                     Esp8266BaseWeb::_activeMethod[5] = "";
char                     Esp8266BaseWeb::_builtinLabels[3][16] = {
    "Status", "Logs", "System"
};
Esp8266BaseWebHomeMode Esp8266BaseWeb::_homeMode = Esp8266BaseWebHomeMode::DEFAULT_SYSTEM_HOME;
Esp8266BaseWebSystemNavMode Esp8266BaseWeb::_systemNavMode = Esp8266BaseWebSystemNavMode::TOP_NAV;

// ----------------------------------------------------------------------------
// PROGMEM HTML 片段（全部在 Flash，不占 DRAM）
// ----------------------------------------------------------------------------
static const char WEB_HEAD[] PROGMEM =
    "<meta charset=UTF-8><meta name=viewport content=width=device-width>"
    "<style>"
    "body{font-family:Arial,Helvetica,sans-serif;padding:12px;max-width:920px;margin:0 auto;"
    "font-size:15px;font-weight:normal;color:#222;line-height:1.45}"
    "h2{margin:0 0 14px;font-size:22px;font-weight:600;line-height:1.25}"
    "p{margin:0 0 12px}"
    "a,input[type=submit],input[type=button]{background:#2f6fb3;color:#fff;padding:6px 10px;"
    "text-decoration:none;border:none;border-radius:3px;cursor:pointer;margin:2px 2px 2px 0;"
    "font-size:14px;font-weight:normal}"
    "input:not([type=submit]):not([type=button]):not([type=radio]):not([type=checkbox]){width:100%;padding:7px;margin:4px 0 12px;"
    "font-size:15px;"
    "border:1px solid #ccc;border-radius:3px;box-sizing:border-box}"
    "label{display:inline-flex;align-items:center;gap:4px;margin:4px 10px 8px 0}"
    "input[type=radio],input[type=checkbox]{width:auto;margin:0}"
    "input.danger{background:#c23b35}"
    "nav{margin-bottom:16px;border-bottom:1px solid #e5e5e5;padding-bottom:10px;display:flex;flex-wrap:wrap;gap:4px;align-items:center}"
    "nav a{font-size:14px}.brand{background:transparent;color:#222;font-weight:600;padding-left:0}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px;margin:14px 0}"
    "section{border:1px solid #e5e5e5;border-radius:4px;padding:12px;background:#fafafa;min-width:0}"
    "h3{margin:0 0 10px;font-size:16px;font-weight:600;line-height:1.25}"
    "dl{margin:0;display:grid;grid-template-columns:104px minmax(0,1fr);gap:5px 10px;font-size:14px}"
    "dt{color:#666;min-width:0;white-space:nowrap}dd{margin:0;min-width:0;overflow-wrap:break-word}"
    "pre{font-size:13px;font-weight:normal;line-height:1.35;overflow-x:auto}"
    ".sysnav{margin-top:14px;padding-top:8px;border-top:1px solid #e5e5e5;display:flex;flex-wrap:wrap;gap:4px}"
    ".tabs{display:flex;flex-wrap:wrap;gap:4px;align-items:center}"
    ".tabs span{white-space:nowrap}"
    ".tabs a,.tabs b{background:#f4f4f4;color:#333;padding:4px 7px;border-radius:3px;"
    "font-size:13px;text-decoration:none;margin:0;font-weight:normal;white-space:nowrap}"
    ".tabs b{background:#333;color:#fff;font-weight:normal}"
    ".ok{color:#188038}.err{color:#c5221f}"
    "footer{color:#777;font-size:12px;margin-top:16px;display:flex;flex-wrap:wrap;gap:6px;align-items:center}"
    "footer .tools{flex:1 1 auto;display:flex;flex-wrap:wrap;gap:4px}"
    "footer a{font-size:12px;background:#f4f4f4;color:#666;padding:3px 6px;border-radius:3px}"
    "footer .status{white-space:nowrap;margin-left:auto}"
    "@media(max-width:560px){footer{border:1px solid #d9e1e8;border-radius:6px;padding:10px;"
    "align-items:flex-start;background:#fff}footer .tools{flex:1 0 100%;gap:5px}"
    "footer a{font-size:14px;padding:5px 8px}footer .status{margin-left:0;white-space:normal;font-size:14px}}"
    "</style>"
    "<script>"
    "function once(f){if(f.dataset.busy)return false;f.dataset.busy=1;"
    "var b=f.querySelector('[type=submit]');if(b)b.disabled=true;return true;}"
    "</script></head><body><nav>";

static const char WEB_NAV_END[]    PROGMEM = "</nav>";
static const char WEB_FOOT_PRE[]   PROGMEM = "<footer>";
static const char WEB_FOOT_HEAP_PRE[] PROGMEM = "<span class=status>Free heap: ";
static const char WEB_FOOT_UP_PRE[] PROGMEM = " &middot; Up: ";
static const char WEB_FOOT_RSSI_PRE[] PROGMEM = " &middot; RSSI: ";
static const char WEB_FOOT_POST[]  PROGMEM = "</span></footer></body></html>";

static const char WEB_WIFI_FORM_PRE[] PROGMEM =
    "<h2>WiFi Settings</h2>"
    "<form method=post onsubmit=\""
    "var s=this.ssid.value.trim();"
    "if(!s){alert('SSID cannot be empty');return false;}"
    "return once(this);\">"
    "SSID<input type=text name=ssid maxlength=32 autocomplete=off required value=\"";

static const char WEB_WIFI_FORM_MID[] PROGMEM =
    "\">Password<input id=wp type=password name=pass maxlength=63 value=\"";

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
    "function fb(n){if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(2)+' KB';return(n/1048576).toFixed(2)+' MB';}"
    "function le(a,o){return(a[o]|(a[o+1]<<8)|(a[o+2]<<16)|(a[o+3]<<24))>>>0;}"
    "function ok(a){if(a.length<16||a[0]!=233||a[1]<1||a[1]>16||a[2]>3)return false;"
    "var ad=le(a,8),sz=le(a,12);return((ad>=0x40100000&&ad<0x40110000)||(ad>=0x3FFE8000&&ad<0x40000000))&&sz>0&&sz<=65536;}"
    "function fail(m){st.textContent=m;pg.style.display='none';document.getElementById('f').dataset.busy='';if(b)b.disabled=false;}"
    "function send(){var fd=new FormData();fd.append('firmware',file);var x=new XMLHttpRequest();"
    "pg.style.display='block';pg.value=0;st.textContent='Uploading 0%';"
    "x.upload.onprogress=function(ev){if(ev.lengthComputable){var p=Math.floor(ev.loaded*100/ev.total);pg.value=p;st.textContent='Uploading '+p+'% ('+fb(ev.loaded)+'/'+fb(ev.total)+')';}};"
    "x.onload=function(){st.textContent=(x.status==200?x.responseText:('Upload failed: HTTP '+x.status+' '+x.responseText));if(x.status!=200)fail(st.textContent);};"
    "x.onerror=function(){fail('Upload failed: network error');};"
    "x.open('POST','/ota');x.send(fd);}"
    "if(b)b.disabled=true;st.textContent='Checking firmware...';"
    "var r=new FileReader();r.onload=function(){var a=new Uint8Array(r.result);if(!ok(a)){fail('Invalid firmware: not an ESP8266 app image');return;}send();};"
    "r.onerror=function(){fail('Invalid firmware: cannot read file header');};r.readAsArrayBuffer(file.slice(0,16));return false;};"
    "</script>";

static const char WEB_AUTH_FORM[] PROGMEM =
    "<h2>Auth Password</h2>"
    "<form method=post onsubmit=\""
    "var c=this.current.value,n=this.newpass.value,r=this.confirm.value;"
    "if(!c){alert('Current password cannot be empty');return false;}"
    "if(!n){alert('New password cannot be empty');return false;}"
    "if(n.length>23){alert('New password is too long');return false;}"
    "if(n!=r){alert('Passwords do not match');return false;}"
    "return once(this);\">"
    "Current password<input type=password name=current maxlength=23 autocomplete=current-password required>"
    "New password<input type=password name=newpass maxlength=23 autocomplete=new-password required>"
    "Confirm new password<input type=password name=confirm maxlength=23 autocomplete=new-password required>"
    "<input type=submit value='Update Password'>"
    "</form>";

static const char WEB_HOSTNAME_FORM_PRE[] PROGMEM =
    "<section><h3>Hostname</h3>"
    "<p>Current hostname: ";

static const char WEB_HOSTNAME_FORM_MID[] PROGMEM =
    "</p><form method=post action='/system/hostname' onsubmit=\""
    "var h=this.hostname.value.trim();"
    "if(!/^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$/.test(h)){"
    "alert('Hostname must be 1-32 lowercase letters, numbers, or hyphens, and cannot start or end with hyphen.');return false;}"
    "return once(this);\">"
    "Hostname<input type=text name=hostname maxlength=32 autocomplete=off required value=\"";

static const char WEB_HOSTNAME_FORM_POST[] PROGMEM =
    "\"><p>Reboot to apply mDNS, OTA discovery, and Web title changes.</p>"
    "<input type=submit value='Save Hostname'>"
    "</form></section>";

static const char WEB_SYSTEM_PAGE[] PROGMEM =
    "<section><h3>Network</h3>"
    "<p><a href='/wifi'>WiFi Settings</a></p>"
    "</section>"
    "<section><h3>Security</h3>"
    "<p><a href='/auth'>Auth Password</a></p>"
    "</section>"
#if ESP8266BASE_USE_OTA
    "<section><h3>Firmware</h3>"
    "<p><a href='/ota'>OTA Update</a></p>"
    "</section>"
#endif
    "<section><h3>File Logs</h3>"
    "<p>Clear all rotated file log segments.</p>"
    "<form method=post action='/logs/clear' onsubmit=\"return confirm('Clear file logs?')&&once(this)\">"
    "<input class=danger type=submit value='Clear File Logs'>"
    "</form></section>"
    "<section><h3>Reboot</h3>"
    "<p>Flush pending config and file logs, then restart the device.</p>"
    "<form method=post action='/reboot' onsubmit=\"return confirm('Reboot device now?')&&once(this)\">"
    "<input class=danger type=submit value='Reboot Device'>"
    "</form></section>"
    "</div>";

static const char WEB_REBOOTING[] PROGMEM =
    "<h2>Rebooting...</h2>"
    "<p>Device is restarting. Please wait a few seconds, then <a href='/'>reload</a>.</p>";

static const char WEB_LOGS_PRE[] PROGMEM =
    "<h2>Logs</h2>";

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

static void _sendAttrEscaped(const char* s) {
    if (!s) return;
    size_t out = 0;
    while (*s) {
        const char* repl = nullptr;
        switch (*s) {
            case '&':  repl = "&amp;"; break;
            case '\'': repl = "&#39;"; break;
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

static void _sendLogFileEscaped(const char* path) {
    if (!path || !LittleFS.exists(path)) return;
    File f = LittleFS.open(path, "r");
    if (!f) return;
    char buf[96];
    while (f.available()) {
        size_t n = f.readBytes(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        _sendAttrEscaped(buf);
        yield();
    }
    f.close();
}

static void _sendLogSection(const char* label, const char* path, uint32_t size) {
    char sizeBuf[16];
    Esp8266BaseUtil::formatBytes(size, sizeBuf, sizeof(sizeBuf));
    Esp8266BaseWeb::sendChunk("\n\n----- ");
    Esp8266BaseWeb::sendChunk(label);
    Esp8266BaseWeb::sendChunk(" file=");
    _sendAttrEscaped(path);
    Esp8266BaseWeb::sendChunk(" size=");
    Esp8266BaseWeb::sendChunk(sizeBuf);
    Esp8266BaseWeb::sendChunk(" -----\n");
    _sendLogFileEscaped(path);
}

static void _redirect(const char* url) {
    Esp8266BaseWeb::server().sendHeader("Location", url);
    Esp8266BaseWeb::server().sendHeader("Cache-Control", "no-store");
    Esp8266BaseWeb::server().send(303);
}

static void _sendFileLogModeOption(const char* value,
                                   const char* label,
                                   Esp8266BaseFileLog::Mode mode) {
    Esp8266BaseWeb::sendChunk("<label><input type=radio name=mode value='");
    Esp8266BaseWeb::sendChunk(value);
    Esp8266BaseWeb::sendChunk("'");
    if (Esp8266BaseFileLog::mode() == mode) {
        Esp8266BaseWeb::sendChunk(" checked");
    }
    Esp8266BaseWeb::sendChunk("> ");
    Esp8266BaseWeb::sendChunk(label);
    Esp8266BaseWeb::sendChunk("</label> ");
}

static bool _fileLogModeFromArg(const String& raw, Esp8266BaseFileLog::Mode& mode) {
    if (raw == "off") {
        mode = Esp8266BaseFileLog::OFF;
        return true;
    }
#if ESP8266BASE_LOG_LEVEL <= ESP8266BASE_FILELOG_MODE_WARN
    if (raw == "warn") {
        mode = Esp8266BaseFileLog::WARN;
        return true;
    }
#endif
#if ESP8266BASE_LOG_LEVEL <= ESP8266BASE_FILELOG_MODE_INFO
    if (raw == "info") {
        mode = Esp8266BaseFileLog::INFO;
        return true;
    }
#endif
    return false;
}

static const char* _validatedDefaultHostname() {
    return Esp8266Base::isValidHostname(ESP8266BASE_DEFAULT_HOSTNAME)
        ? ESP8266BASE_DEFAULT_HOSTNAME
        : "esp8266base";
}

#if ESP8266BASE_USE_SLEEP
static const char* _wakeReasonText(const char* reason) {
    if (strcmp(reason, "power-on") == 0) {
        return "power-on (上电启动)";
    }
    if (strcmp(reason, "deep-sleep") == 0) {
        return "deep-sleep (深睡唤醒)";
    }
    if (strcmp(reason, "soft-restart") == 0) {
        return "soft-restart (软件重启)";
    }
    if (strcmp(reason, "wdt-reset") == 0) {
        return "wdt-reset (看门狗重启)";
    }
    return "unknown (未知)";
}
#endif

static void _sendHostnameSystemSection() {
    char persisted[33] = "";
    bool found = Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_HOSTNAME, persisted, sizeof(persisted), "");
    bool persistedValid = found && persisted[0] && Esp8266Base::isValidHostname(persisted);
    const char* formValue = persistedValid ? persisted : Esp8266BaseWeb::server().hasArg("hostname") ? "" : Esp8266Base::hostname();

    Esp8266BaseWeb::sendContent_P(WEB_HOSTNAME_FORM_PRE);
    _sendAttrEscaped(Esp8266Base::hostname());
    Esp8266BaseWeb::sendChunk("<br>mDNS: ");
    _sendAttrEscaped(Esp8266Base::hostname());
    Esp8266BaseWeb::sendChunk(".local");
    if (persistedValid && strcmp(persisted, Esp8266Base::hostname()) != 0) {
        Esp8266BaseWeb::sendChunk("<br>Saved hostname: ");
        _sendAttrEscaped(persisted);
        Esp8266BaseWeb::sendChunk(" (reboot required)");
    }
    Esp8266BaseWeb::sendContent_P(WEB_HOSTNAME_FORM_MID);
    _sendAttrEscaped(formValue);
    Esp8266BaseWeb::sendContent_P(WEB_HOSTNAME_FORM_POST);
}

static void _sendFileLogSystemSection() {
    Esp8266BaseWeb::sendChunk("<section><h3>File Log Mode</h3><p>Current mode: ");
    Esp8266BaseWeb::sendChunk(Esp8266BaseFileLog::modeName());
    Esp8266BaseWeb::sendChunk("</p><form method=post action='/system/filelog' onsubmit=\"return once(this)\">");
    _sendFileLogModeOption("off", "Off", Esp8266BaseFileLog::OFF);
#if ESP8266BASE_LOG_LEVEL <= ESP8266BASE_FILELOG_MODE_WARN
    _sendFileLogModeOption("warn", "WARN", Esp8266BaseFileLog::WARN);
#endif
#if ESP8266BASE_LOG_LEVEL <= ESP8266BASE_FILELOG_MODE_INFO
    _sendFileLogModeOption("info", "INFO", Esp8266BaseFileLog::INFO);
#endif
    Esp8266BaseWeb::sendChunk("<p>Mode is capped by the build log level.</p><input type=submit value='Save File Log'></form></section>");
}

static bool _isValidPath(const char* path) {
    if (!path || path[0] != '/') return false;
    size_t len = strlen(path);
    if (len <= 1 || len >= 24) return false;
    for (size_t i = 1; i < len; i++) {
        char c = path[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '/' || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

const char* Esp8266BaseWeb::_builtinLabel(Esp8266BaseWebBuiltinLabel label) {
    return _builtinLabels[(uint8_t)label];
}

void Esp8266BaseWeb::_sendLink(const char* path, const char* title, const char* cls) {
    Esp8266BaseWeb::sendChunk("<a href='");
    _sendAttrEscaped(path);
    Esp8266BaseWeb::sendChunk("'");
    if (cls && cls[0]) {
        Esp8266BaseWeb::sendChunk(" class='");
        _sendAttrEscaped(cls);
        Esp8266BaseWeb::sendChunk("'");
    }
    Esp8266BaseWeb::sendChunk(">");
    _sendAttrEscaped(title);
    Esp8266BaseWeb::sendChunk("</a>");
}

const char* Esp8266BaseWeb::_brandTitle() {
    return _deviceName[0] ? _deviceName : _titleBuf;
}

const char* Esp8266BaseWeb::_brandHref() {
    return _homePath[0] ? _homePath : "/";
}

void Esp8266BaseWeb::_sendSystemLinks() {
    const char* systemHome = "/";
    if (_homeMode == Esp8266BaseWebHomeMode::APP_HOME_FIRST && _homePath[0]) {
        systemHome = _homePath;
    } else if (_homeMode == Esp8266BaseWebHomeMode::FUSED_HOME && _homePath[0]) {
        systemHome = "/esp8266base";
    }
    _sendLink(systemHome, _builtinLabel(Esp8266BaseWebBuiltinLabel::HOME), nullptr);
    _sendLink("/logs", _builtinLabel(Esp8266BaseWebBuiltinLabel::LOGS), nullptr);
    _sendLink("/system", _builtinLabel(Esp8266BaseWebBuiltinLabel::SYSTEM), nullptr);
}

void Esp8266BaseWeb::_sendAppLinks() {
    for (uint8_t i = 0; i < _pageCount; i++) {
        if (!_pages[i].showInNav) continue;
        _sendLink(_pages[i].path, _pages[i].title, nullptr);
    }
}

// ----------------------------------------------------------------------------
// 公开 API
// ----------------------------------------------------------------------------
bool Esp8266BaseWeb::begin() {
    if (_running) return true;

    _loadPersistedAuth();

    // 注册内置路由（静态成员函数指针，无捕获，无 std::function 对象驻留堆）
    _server.on("/",       HTTP_GET,  _handleRoot);
    _server.on("/esp8266base", HTTP_GET, _handleSystemHome);
    _server.on("/wifi",   HTTP_GET,  _handleWiFiGet);
    _server.on("/wifi",   HTTP_POST, _handleWiFiPost);
    _server.on("/auth",   HTTP_GET,  _handleAuthGet);
    _server.on("/auth",   HTTP_POST, _handleAuthPost);
#if ESP8266BASE_USE_OTA
    _server.on("/ota",    HTTP_GET,  _handleOtaGet);
    // POST /ota 由 Esp8266BaseOTA::begin() 注册（需要 upload handler）
#endif
    _server.on("/logs",   HTTP_GET,  _handleLogsGet);
    _server.on("/logs/clear", HTTP_POST, _handleLogsClearPost);
    _server.on("/system", HTTP_GET,  _handleSystemGet);
    _server.on("/system/filelog", HTTP_POST, _handleFileLogPost);
    _server.on("/system/hostname", HTTP_POST, _handleHostnamePost);
    _server.on("/api/system/hostname", HTTP_GET, _handleHostnameApiGet);
    _server.on("/api/system/hostname", HTTP_POST, _handleHostnameApiPost);
    _server.on("/reboot", HTTP_POST, _handleRebootPost);
    _server.on("/health", HTTP_GET,  _handleHealth);
    _server.onNotFound(_handleNotFound);

    _server.begin();
    _running = true;
    ESP8266BASE_LOG_I("Web ", "web_server_started auth_required=yes builtin_routes=17 app_pages_registered=%d/%d app_apis_registered=%d/%d",
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
#if ESP8266BASE_LOG_LEVEL <= 0
        const char* method = _activeMethod[0] ? _activeMethod : "?";
        const char* uri = _activeUri[0] ? _activeUri : "(unknown)";
        ESP8266BASE_LOG_D("Web ", "slow_request method=%s uri=%s elapsed=%lums",
                          method, uri, (unsigned long)elapsed);
#endif
    }
}

bool Esp8266BaseWeb::isRunning() {
    return _running;
}

bool Esp8266BaseWeb::addPage(const char* path, Esp8266BaseWebHandler handler) {
    return addPage(path, nullptr, handler);
}

bool Esp8266BaseWeb::addPage(const char* path, const char* title, Esp8266BaseWebHandler handler) {
    if (!path || !handler) return false;
    if (!_isValidPath(path)) {
        ESP8266BASE_LOG_W("Web ", "addPage_rejected reason=invalid_path path=%s count=%u max=%u",
                          path, (unsigned)_pageCount, (unsigned)ESP8266BASE_WEB_MAX_APP_PAGES);
        return false;
    }
    if (_pageCount >= ESP8266BASE_WEB_MAX_APP_PAGES) {
        ESP8266BASE_LOG_W("Web ", "addPage_rejected reason=table_full path=%s count=%u max=%u",
                          path, (unsigned)_pageCount, (unsigned)ESP8266BASE_WEB_MAX_APP_PAGES);
        return false;
    }

    strncpy(_pages[_pageCount].path, path, 23);
    _pages[_pageCount].path[23]    = '\0';
    if (title && title[0]) {
        strncpy(_pages[_pageCount].title, title, sizeof(_pages[_pageCount].title) - 1);
    } else {
        strncpy(_pages[_pageCount].title, path + 1, sizeof(_pages[_pageCount].title) - 1);
    }
    _pages[_pageCount].title[sizeof(_pages[_pageCount].title) - 1] = '\0';
    _pages[_pageCount].handler     = handler;
    _pages[_pageCount].isApi       = false;
    _pages[_pageCount].showInNav   = true;
    uint8_t index = _pageCount;
    _pageCount++;

    switch (index) {
        case 0: _server.on(path, HTTP_GET, _handleAppPage0); break;
        case 1: _server.on(path, HTTP_GET, _handleAppPage1); break;
        case 2: _server.on(path, HTTP_GET, _handleAppPage2); break;
        case 3: _server.on(path, HTTP_GET, _handleAppPage3); break;
        default: return false;
    }
    ESP8266BASE_LOG_I("Web ", "app_page_registered path=%s title=%s app_pages_registered=%d/%d",
                      path, _pages[index].title, (int)_pageCount, ESP8266BASE_WEB_MAX_APP_PAGES);
    return true;
}

bool Esp8266BaseWeb::addApi(const char* path, Esp8266BaseWebHandler handler) {
    if (!path || !handler) return false;
    if (!_isValidPath(path)) {
        ESP8266BASE_LOG_W("Web ", "addApi_rejected reason=invalid_path path=%s count=%u max=%u",
                          path, (unsigned)_apiCount, (unsigned)ESP8266BASE_WEB_MAX_APP_APIS);
        return false;
    }
    if (_apiCount >= ESP8266BASE_WEB_MAX_APP_APIS) {
        ESP8266BASE_LOG_W("Web ", "addApi_rejected reason=table_full path=%s count=%u max=%u",
                          path, (unsigned)_apiCount, (unsigned)ESP8266BASE_WEB_MAX_APP_APIS);
        return false;
    }

    strncpy(_apis[_apiCount].path, path, 23);
    _apis[_apiCount].path[23]    = '\0';
    _apis[_apiCount].title[0]    = '\0';
    _apis[_apiCount].handler     = handler;
    _apis[_apiCount].isApi       = true;
    _apis[_apiCount].showInNav   = false;
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

bool Esp8266BaseWeb::addNavItem(const char* path, const char* title) {
    if (!_isValidPath(path) || !title || !title[0]) return false;
    for (uint8_t i = 0; i < _pageCount; i++) {
        if (strcmp(_pages[i].path, path) == 0) {
            strncpy(_pages[i].title, title, sizeof(_pages[i].title) - 1);
            _pages[i].title[sizeof(_pages[i].title) - 1] = '\0';
            _pages[i].showInNav = true;
            ESP8266BASE_LOG_I("Web ", "app_nav_registered path=%s title=%s", path, _pages[i].title);
            return true;
        }
    }
    ESP8266BASE_LOG_W("Web ", "addNavItem path not registered");
    return false;
}

void Esp8266BaseWeb::setDeviceName(const char* name) {
    if (!name) return;
    strncpy(_deviceName, name, sizeof(_deviceName) - 1);
    _deviceName[sizeof(_deviceName) - 1] = '\0';
}

void Esp8266BaseWeb::setHomePath(const char* path) {
    if (!_isValidPath(path)) return;
    strncpy(_homePath, path, sizeof(_homePath) - 1);
    _homePath[sizeof(_homePath) - 1] = '\0';
}

void Esp8266BaseWeb::setHomeMode(Esp8266BaseWebHomeMode mode) {
    _homeMode = mode;
}

void Esp8266BaseWeb::setSystemNavMode(Esp8266BaseWebSystemNavMode mode) {
    _systemNavMode = mode;
}

void Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel label, const char* title) {
    if (!title) return;
    uint8_t index = (uint8_t)label;
    if (index >= 3) return;
    strncpy(_builtinLabels[index], title, sizeof(_builtinLabels[index]) - 1);
    _builtinLabels[index][sizeof(_builtinLabels[index]) - 1] = '\0';
}

void Esp8266BaseWeb::setDefaultAuth(const char* user, const char* pass) {
    if (_running) {
        ESP8266BASE_LOG_W("Web ", "set_default_auth_ignored reason=web_already_running");
        return;
    }
    if (user) { strncpy(_authUser, user, 23); _authUser[23] = '\0'; }
    if (pass) { strncpy(_authPass, pass, 23); _authPass[23] = '\0'; }
}

void Esp8266BaseWeb::setSystemInfo(const char* hostname, const char* fw, const char* ver, uint32_t bootCount) {
    if (hostname) {
        strncpy(_hostname, hostname, sizeof(_hostname) - 1);
        _hostname[sizeof(_hostname) - 1] = '\0';
    }
    if (fw) {
        strncpy(_fwName, fw, sizeof(_fwName) - 1);
        _fwName[sizeof(_fwName) - 1] = '\0';
    }
    if (ver) {
        strncpy(_fwVersion, ver, sizeof(_fwVersion) - 1);
        _fwVersion[sizeof(_fwVersion) - 1] = '\0';
    }
    _bootCount = bootCount;
    strncpy(_titleBuf, _hostname, sizeof(_titleBuf) - 1);
    _titleBuf[sizeof(_titleBuf) - 1] = '\0';
    strncat(_titleBuf, " (", sizeof(_titleBuf) - strlen(_titleBuf) - 1);
    strncat(_titleBuf, _fwName, sizeof(_titleBuf) - strlen(_titleBuf) - 1);
    strncat(_titleBuf, " ", sizeof(_titleBuf) - strlen(_titleBuf) - 1);
    strncat(_titleBuf, _fwVersion, sizeof(_titleBuf) - strlen(_titleBuf) - 1);
    strncat(_titleBuf, ")", sizeof(_titleBuf) - strlen(_titleBuf) - 1);
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

void Esp8266BaseWeb::_loadPersistedAuth() {
    char user[24] = "";
    char pass[24] = "";
    bool userFound = false;
    bool passFound = false;
    if (Esp8266BaseConfig::isReady()) {
        userFound = Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WEB_USER, user, sizeof(user), "");
        passFound = Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WEB_PASS, pass, sizeof(pass), "");
    }
    if (userFound && user[0]) {
        strncpy(_authUser, user, sizeof(_authUser) - 1);
        _authUser[sizeof(_authUser) - 1] = '\0';
    }
    if (passFound && pass[0]) {
        strncpy(_authPass, pass, sizeof(_authPass) - 1);
        _authPass[sizeof(_authPass) - 1] = '\0';
    }
    ESP8266BASE_LOG_I("Web ", "web_auth_loaded user=%s password=%s user_source=%s pass_source=%s password_length=%u",
                      _authUser, _authPass,
                      (userFound && user[0]) ? "persisted" : "default",
                      (passFound && pass[0]) ? "persisted" : "default",
                      (unsigned)strlen(_authPass));
}

void Esp8266BaseWeb::_formatDuration(uint32_t seconds, char* out, size_t len) {
    if (!out || len == 0) return;
    uint32_t days = seconds / 86400UL;
    seconds %= 86400UL;
    uint32_t hours = seconds / 3600UL;
    seconds %= 3600UL;
    uint32_t minutes = seconds / 60UL;
    seconds %= 60UL;
    if (days > 0) {
        snprintf(out, len, "%lud %luh %lum %lus",
                 (unsigned long)days, (unsigned long)hours,
                 (unsigned long)minutes, (unsigned long)seconds);
    } else if (hours > 0) {
        snprintf(out, len, "%luh %lum %lus",
                 (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
    } else if (minutes > 0) {
        snprintf(out, len, "%lum %lus", (unsigned long)minutes, (unsigned long)seconds);
    } else {
        snprintf(out, len, "%lus", (unsigned long)seconds);
    }
}

void Esp8266BaseWeb::_formatFooterUptime(uint32_t seconds, char* out, size_t len) {
    if (!out || len == 0) return;
    uint32_t days = seconds / 86400UL;
    seconds %= 86400UL;
    uint32_t hours = seconds / 3600UL;
    seconds %= 3600UL;
    uint32_t minutes = seconds / 60UL;
    if (days > 0) {
        snprintf(out, len, "%lud %luh", (unsigned long)days, (unsigned long)hours);
    } else if (hours > 0) {
        snprintf(out, len, "%luh %lum", (unsigned long)hours, (unsigned long)minutes);
    } else {
        snprintf(out, len, "%lum", (unsigned long)minutes);
    }
}

void Esp8266BaseWeb::_sendKv(const char* key, const char* value) {
    sendChunk("<dt>");
    _sendAttrEscaped(key);
    sendChunk("</dt><dd>");
    _sendAttrEscaped(value && value[0] ? value : "-");
    sendChunk("</dd>");
}

void Esp8266BaseWeb::_markRequest() {
    strncpy(_activeMethod, (_server.method() == HTTP_POST) ? "POST" : "GET",
            sizeof(_activeMethod) - 1);
    _activeMethod[sizeof(_activeMethod) - 1] = '\0';

    strncpy(_activeUri, _server.uri().c_str(), sizeof(_activeUri) - 1);
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
    _sendLink(_brandHref(), _brandTitle(), "brand");
    _sendAppLinks();
    if (_systemNavMode == Esp8266BaseWebSystemNavMode::TOP_NAV) {
        _sendSystemLinks();
    }
    sendContent_P(WEB_NAV_END);
}

void Esp8266BaseWeb::sendFooter() {
    if (_systemNavMode == Esp8266BaseWebSystemNavMode::BOTTOM_NAV) {
        sendChunk("<div class=sysnav>");
        _sendSystemLinks();
        sendChunk("</div>");
    }
    sendContent_P(WEB_FOOT_PRE);
    if (_systemNavMode == Esp8266BaseWebSystemNavMode::FOOTER_COMPACT) {
        sendChunk("<span class=tools>");
        _sendSystemLinks();
        sendChunk("</span>");
    }
    char uptime[16];
    _formatFooterUptime(millis() / 1000UL, uptime, sizeof(uptime));
    char rssi[12] = "-";
    if (Esp8266BaseWiFi::isConnected()) {
        snprintf(rssi, sizeof(rssi), "%d dBm", Esp8266BaseWiFi::rssi());
    }
    sendContent_P(WEB_FOOT_HEAP_PRE);
    Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), _wb, sizeof(_wb));
    sendChunk(_wb);
    sendContent_P(WEB_FOOT_UP_PRE);
    sendChunk(uptime);
    sendContent_P(WEB_FOOT_RSSI_PRE);
    sendChunk(rssi);
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
    _markRequest();
    if (_homePath[0] && _homeMode != Esp8266BaseWebHomeMode::DEFAULT_SYSTEM_HOME) {
        _redirect(_homePath);
        return;
    }
    _handleSystemHome();
}

void Esp8266BaseWeb::_handleSystemHome() {
    _markRequest();
    if (_homePath[0] && _homeMode == Esp8266BaseWebHomeMode::APP_HOME_FIRST) {
        _redirect(_homePath);
        return;
    }
    if (!checkAuth()) return;
    sendHeader();
    sendChunk("<h2>");
    _sendAttrEscaped(_brandTitle());
    sendChunk("</h2>");

    const char* wifiState = "Connecting";
    const char* ssid = Esp8266BaseWiFi::ssid();
    const char* ip = "-";
    char rssi[12] = "-";
    char mac[18] = "";
    Esp8266BaseWiFi::macAddressTo(mac, sizeof(mac));
    if (Esp8266BaseWiFi::isConnected()) {
        wifiState = "Connected";
        ip = Esp8266BaseWiFi::ip();
        snprintf(rssi, sizeof(rssi), "%d dBm", Esp8266BaseWiFi::rssi());
    } else if (Esp8266BaseWiFi::state() == Esp8266BaseWiFiState::AP_CONFIG) {
        wifiState = "AP Mode";
        ssid = Esp8266BaseWiFi::apSSID();
        ip = "192.168.4.1";
    }

    char bootCount[12];
    snprintf(bootCount, sizeof(bootCount), "%lu", (unsigned long)_bootCount);

    char chipId[18];
    snprintf(chipId, sizeof(chipId), "ESP8266-%06X", (unsigned)(ESP.getChipId() & 0xFFFFFFUL));

    char cpuFreq[12];
    snprintf(cpuFreq, sizeof(cpuFreq), "%u MHz", (unsigned)ESP.getCpuFreqMHz());

    char flashSize[16];
    Esp8266BaseUtil::formatBytes(ESP.getFlashChipRealSize(), flashSize, sizeof(flashSize));

    char sketchSize[16];
    Esp8266BaseUtil::formatBytes(ESP.getSketchSize(), sketchSize, sizeof(sketchSize));

    char otaFree[16];
    Esp8266BaseUtil::formatBytes(ESP.getFreeSketchSpace(), otaFree, sizeof(otaFree));

    char freeHeap[16];
    Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), freeHeap, sizeof(freeHeap));

    char maxBlock[16];
    Esp8266BaseUtil::formatBytes(ESP.getMaxFreeBlockSize(), maxBlock, sizeof(maxBlock));

    char uptime[32];
    _formatDuration(millis() / 1000UL, uptime, sizeof(uptime));

    char ntpState[12] = "disabled";
    char currentTime[20] = "-";
    char bootTime[20] = "-";
#if ESP8266BASE_USE_NTP
    if (Esp8266BaseNTP::isSynced()) {
        strncpy(ntpState, "synced", sizeof(ntpState) - 1);
        Esp8266BaseNTP::formatTo(currentTime, sizeof(currentTime), "%Y-%m-%d %H:%M:%S");
        time_t bt = time(nullptr) - (time_t)(millis() / 1000UL);
        struct tm* tm_info = localtime(&bt);
        if (tm_info) {
            strftime(bootTime, sizeof(bootTime), "%Y-%m-%d %H:%M:%S", tm_info);
        }
    } else {
        strncpy(ntpState, "pending", sizeof(ntpState) - 1);
    }
#endif

    sendChunk("<div class=grid><section><h3>Connection</h3><dl>");
    _sendKv("Hostname", _hostname);
    _sendKv("WiFi", wifiState);
    _sendKv("SSID", ssid);
    _sendKv("IP", ip);
    _sendKv("RSSI", rssi);
    _sendKv("STA MAC", mac);
    sendChunk("</dl></section><section><h3>Runtime</h3><dl>");
    _sendKv("Free heap", freeHeap);
    _sendKv("Max block", maxBlock);
    _sendKv("Boot count", bootCount);
#if ESP8266BASE_USE_WATCHDOG
    char wdtResets[28];
    snprintf(wdtResets, sizeof(wdtResets), "%lu since clear",
             (unsigned long)Esp8266BaseWatchdog::resetCount());
    _sendKv("WDT resets", wdtResets);
#endif
#if ESP8266BASE_USE_SLEEP
    _sendKv("Wake reason", _wakeReasonText(Esp8266BaseSleep::wakeReason()));
#endif
    sendChunk("</dl></section><section><h3>Firmware</h3><dl>");
    _sendKv("Firmware", _fwName);
    _sendKv("Version", _fwVersion);
    _sendKv("Chip ID", chipId);
    _sendKv("CPU", cpuFreq);
    _sendKv("Flash", flashSize);
    _sendKv("Sketch", sketchSize);
    _sendKv("OTA free", otaFree);
    sendChunk("</dl></section><section><h3>Time</h3><dl>");
    _sendKv("Uptime", uptime);
    _sendKv("NTP", ntpState);
    _sendKv("Now", currentTime);
    _sendKv("Boot time", bootTime);
    sendChunk("</dl></section></div>");
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
        } else if (strcmp(err, "ssid_too_long") == 0) {
            sendChunk("<p class=err>SSID is too long. Maximum is 32 bytes.</p>");
        } else if (strcmp(err, "password_too_long") == 0) {
            sendChunk("<p class=err>Password is too long. Maximum is 63 bytes.</p>");
        } else if (strcmp(err, "save_failed") == 0) {
            sendChunk("<p class=err>Failed to save WiFi credentials.</p>");
        } else {
            sendChunk("<p class=err>WiFi settings were not saved.</p>");
        }
    }

    char ssid[64] = "";
    char pass[64] = "";
    Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid), "");
    Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WIFI_PASS, pass, sizeof(pass), "");
    sendContent_P(WEB_WIFI_FORM_PRE);
    _sendAttrEscaped(ssid);
    sendContent_P(WEB_WIFI_FORM_MID);
    _sendAttrEscaped(pass);
    sendContent_P(WEB_WIFI_FORM_POST);
    sendFooter();
}

void Esp8266BaseWeb::_handleWiFiPost() {
    if (!checkAuth()) return;
    _markRequest();

    String ssidArg = _server.arg("ssid");
    String passArg = _server.arg("pass");
    ssidArg.trim();
    passArg.trim();

    if (ssidArg.length() == 0) {
        _redirect("/wifi?error=missing_ssid");
        return;
    }
    if (ssidArg.length() > 32) {
        ESP8266BASE_LOG_W("Web ", "wifi_credentials_form_rejected reason=ssid_too_long length=%u max=32",
                          (unsigned)ssidArg.length());
        _redirect("/wifi?error=ssid_too_long");
        return;
    }
    if (passArg.length() > 63) {
        ESP8266BASE_LOG_W("Web ", "wifi_credentials_form_rejected reason=password_too_long length=%u max=63",
                          (unsigned)passArg.length());
        _redirect("/wifi?error=password_too_long");
        return;
    }

    // Intentionally log the WiFi password in plaintext for field debugging.
    ESP8266BASE_LOG_I("Web ", "wifi_credentials_form_submitted ssid=%s password=%s password_length=%u",
                      ssidArg.c_str(), passArg.c_str(), (unsigned)passArg.length());
    if (Esp8266BaseWiFi::connect(ssidArg.c_str(), passArg.c_str())) {
        _redirect("/wifi?saved=1");
    } else {
        _redirect("/wifi?error=save_failed");
    }
}

void Esp8266BaseWeb::_handleAuthGet() {
    if (!checkAuth()) return;
    sendHeader();
    if (_server.hasArg("saved")) {
        sendChunk("<p class=ok>Password saved. Use the new password for the next request.</p>");
    } else if (_server.hasArg("error")) {
        char err[24] = "";
        strncpy(err, _server.arg("error").c_str(), sizeof(err) - 1);
        if (strcmp(err, "current") == 0) {
            sendChunk("<p class=err>Current password is incorrect.</p>");
        } else if (strcmp(err, "empty") == 0) {
            sendChunk("<p class=err>New password cannot be empty.</p>");
        } else if (strcmp(err, "too_long") == 0) {
            sendChunk("<p class=err>New password is too long. Maximum length is 23.</p>");
        } else if (strcmp(err, "mismatch") == 0) {
            sendChunk("<p class=err>New password and confirmation do not match.</p>");
        } else if (strcmp(err, "save_failed") == 0) {
            sendChunk("<p class=err>Failed to save password.</p>");
        } else {
            sendChunk("<p class=err>Password was not saved.</p>");
        }
    }
    sendContent_P(WEB_AUTH_FORM);
    sendFooter();
}

void Esp8266BaseWeb::_handleAuthPost() {
    if (!checkAuth()) return;
    _markRequest();

    const String currentArg = _server.arg("current");
    const String newArg = _server.arg("newpass");
    const String confirmArg = _server.arg("confirm");

    if (newArg.length() == 0) {
        ESP8266BASE_LOG_W("Web ", "web_password_change_rejected reason=empty");
        _redirect("/auth?error=empty");
        return;
    }
    if (newArg.length() > 23 || currentArg.length() > 23 || confirmArg.length() > 23) {
        ESP8266BASE_LOG_W("Web ", "web_password_change_rejected reason=too_long current_length=%u new_length=%u confirm_length=%u",
                          (unsigned)currentArg.length(), (unsigned)newArg.length(), (unsigned)confirmArg.length());
        _redirect("/auth?error=too_long");
        return;
    }

    char current[24] = "";
    char newPass[24] = "";
    char confirm[24] = "";
    strncpy(current, currentArg.c_str(), sizeof(current) - 1);
    strncpy(newPass, newArg.c_str(), sizeof(newPass) - 1);
    strncpy(confirm, confirmArg.c_str(), sizeof(confirm) - 1);

    if (strcmp(current, _authPass) != 0) {
        ESP8266BASE_LOG_W("Web ", "web_password_change_rejected reason=current_password_mismatch current=%s expected=%s",
                          current, _authPass);
        _redirect("/auth?error=current");
        return;
    }
    if (strcmp(newPass, confirm) != 0) {
        ESP8266BASE_LOG_W("Web ", "web_password_change_rejected reason=mismatch new=%s confirm=%s",
                          newPass, confirm);
        _redirect("/auth?error=mismatch");
        return;
    }

    if (!Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WEB_PASS, newPass)) {
        ESP8266BASE_LOG_E("Web ", "web_password_update_failed password=%s password_length=%u",
                          newPass, (unsigned)strlen(newPass));
        _redirect("/auth?error=save_failed");
        return;
    }

    strncpy(_authPass, newPass, sizeof(_authPass) - 1);
    _authPass[sizeof(_authPass) - 1] = '\0';
    ESP8266BASE_LOG_I("Web ", "web_password_updated password=%s password_length=%u result=success",
                      _authPass, (unsigned)strlen(_authPass));
    _redirect("/auth?saved=1");
}

void Esp8266BaseWeb::_handleOtaGet() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_OTA_FORM);
    sendFooter();
}

void Esp8266BaseWeb::_handleLogsGet() {
    if (!checkAuth()) return;
    sendHeader();
    sendContent_P(WEB_LOGS_PRE);

    Esp8266BaseFileLog::flush();

    char maxBuf[16];
    char totalBuf[16];
    Esp8266BaseUtil::formatBytes(Esp8266BaseFileLog::maxBytes(), maxBuf, sizeof(maxBuf));
    Esp8266BaseUtil::formatBytes(Esp8266BaseFileLog::maxBytes() * Esp8266BaseFileLog::rotateFiles(),
                                 totalBuf, sizeof(totalBuf));
    sendChunk("<p>File log: ");
    sendChunk(Esp8266BaseFileLog::isEnabled() ? "enabled" : "disabled");
    sendChunk("<br>Mode: ");
    sendChunk(Esp8266BaseFileLog::modeName());
    sendChunk("<br>Path: ");
    _sendAttrEscaped(Esp8266BaseFileLog::path());
    snprintf(_wb, sizeof(_wb), "<br>Rotation files: %u",
             (unsigned)Esp8266BaseFileLog::rotateFiles());
    sendChunk(_wb);

    if (Esp8266BaseFileLog::bufferEnabled()) {
        char usedBuf[16];
        char sizeBuf[16];
        Esp8266BaseUtil::formatBytes(Esp8266BaseFileLog::bufferUsed(), usedBuf, sizeof(usedBuf));
        Esp8266BaseUtil::formatBytes(Esp8266BaseFileLog::bufferSize(), sizeBuf, sizeof(sizeBuf));
        snprintf(_wb, sizeof(_wb),
                 "<br>Buffer: %s / %s<br>Flush interval: %lus",
                 usedBuf, sizeBuf,
                 (unsigned long)(Esp8266BaseFileLog::flushIntervalMs() / 1000UL));
        sendChunk(_wb);
    } else {
        sendChunk("<br>Buffer: disabled");
    }
    snprintf(_wb, sizeof(_wb), "<br>Max per file: %s<br>Max total: %s",
             maxBuf, totalBuf);
    sendChunk(_wb);
    sendChunk("<br>Segments: ");
    for (uint8_t i = 0; i < Esp8266BaseFileLog::rotateFiles(); i++) {
        char segBuf[16];
        Esp8266BaseUtil::formatBytes(Esp8266BaseFileLog::segmentSize(i), segBuf, sizeof(segBuf));
        snprintf(_wb, sizeof(_wb), "%s%u=%s", i == 0 ? "" : ", ", (unsigned)i, segBuf);
        sendChunk(_wb);
    }
    sendChunk("</p>");

    uint8_t selected = 0;
    if (_server.hasArg("seg")) {
        int v = _server.arg("seg").toInt();
        if (v >= 0 && v < (int)Esp8266BaseFileLog::rotateFiles()) {
            selected = (uint8_t)v;
        }
    }

    sendChunk("<p class=tabs><span>Files:</span> ");
    for (uint8_t i = 0; i < Esp8266BaseFileLog::rotateFiles(); i++) {
        char segBuf[16];
        Esp8266BaseUtil::formatBytes(Esp8266BaseFileLog::segmentSize(i), segBuf, sizeof(segBuf));
        if (i == selected) {
            sendChunk("<b>");
            if (i == 0) {
                snprintf(_wb, sizeof(_wb), "current-0 (%s)", segBuf);
            } else {
                snprintf(_wb, sizeof(_wb), "history-%u (%s)", (unsigned)i, segBuf);
            }
            sendChunk(_wb);
            sendChunk("</b>");
        } else {
            snprintf(_wb, sizeof(_wb), "<a href='/logs?seg=%u'>", (unsigned)i);
            sendChunk(_wb);
            if (i == 0) {
                snprintf(_wb, sizeof(_wb), "current-0 (%s)", segBuf);
            } else {
                snprintf(_wb, sizeof(_wb), "history-%u (%s)", (unsigned)i, segBuf);
            }
            sendChunk(_wb);
            sendChunk("</a>");
        }
        if (i + 1 < Esp8266BaseFileLog::rotateFiles()) sendChunk(" ");
    }
    sendChunk("</p><pre>");

    char selectedPath[36];
    const char* selectedLabel = "current-0";
    uint32_t selectedSize = Esp8266BaseFileLog::segmentSize(selected);
    if (selected == 0) {
        snprintf(selectedPath, sizeof(selectedPath), "%s", Esp8266BaseFileLog::path());
    } else {
        snprintf(selectedPath, sizeof(selectedPath), "%s.%u",
                 Esp8266BaseFileLog::path(), (unsigned)selected);
        snprintf(_wb, sizeof(_wb), "history-%u", (unsigned)selected);
        selectedLabel = _wb;
    }
    _sendLogSection(selectedLabel, selectedPath, selectedSize);
    sendChunk("</pre>");
    sendFooter();
}

void Esp8266BaseWeb::_handleLogsClearPost() {
    if (!checkAuth()) return;
    _markRequest();
    bool ok = Esp8266BaseFileLog::clear();
    ESP8266BASE_LOG_I("Web ", "log_file_clear_requested result=%s", ok ? "success" : "failed");
    _redirect(ok ? "/system?cleared=1" : "/system?error=clear_failed");
}

void Esp8266BaseWeb::_handleFileLogPost() {
    if (!checkAuth()) return;
    _markRequest();

    Esp8266BaseFileLog::Mode mode = Esp8266BaseFileLog::OFF;
    if (!_fileLogModeFromArg(_server.arg("mode"), mode)) {
        ESP8266BASE_LOG_W("Web ", "filelog_mode_rejected value=%s", _server.arg("mode").c_str());
        _redirect("/system?error=filelog_invalid_mode");
        return;
    }

    ESP8266BASE_LOG_W("Web ", "filelog_mode_requested old=%s new=%s",
                      Esp8266BaseFileLog::modeName(),
                      mode == Esp8266BaseFileLog::OFF ? "off" :
                      (mode == Esp8266BaseFileLog::WARN ? "warn" : "info"));
    bool ok = Esp8266BaseFileLog::setMode(mode);
    _redirect(ok ? "/system?filelog_saved=1" : "/system?error=filelog_save_failed");
}

void Esp8266BaseWeb::_handleHostnamePost() {
    if (!checkAuth()) return;
    _markRequest();

    char hostname[33] = "";
    strncpy(hostname, _server.arg("hostname").c_str(), sizeof(hostname) - 1);
    _trimWhitespace(hostname);

    if (!Esp8266Base::isValidHostname(hostname)) {
        ESP8266BASE_LOG_W("Web ", "hostname_update_rejected reason=invalid value=%s", hostname);
        _redirect("/system?error=hostname_invalid");
        return;
    }

    if (!Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_HOSTNAME, hostname)) {
        ESP8266BASE_LOG_E("Web ", "hostname_update_failed hostname=%s", hostname);
        _redirect("/system?error=hostname_save_failed");
        return;
    }

    ESP8266BASE_LOG_I("Web ", "hostname_saved hostname=%s reboot_required=yes", hostname);
    _redirect("/system?hostname_saved=1");
}

void Esp8266BaseWeb::_handleHostnameApiGet() {
    _markRequest();
    if (!verifyAuth()) {
        _server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    char persisted[33] = "";
    bool found = Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_HOSTNAME, persisted, sizeof(persisted), "");
    bool persistedValid = found && persisted[0] && Esp8266Base::isValidHostname(persisted);
    const char* defaultHostname = _validatedDefaultHostname();
    bool rebootRequired = persistedValid && strcmp(persisted, Esp8266Base::hostname()) != 0;

    snprintf(_wb, sizeof(_wb),
             "{\"hostname\":\"%s\",\"persisted\":\"%s\",\"default\":\"%s\",\"rebootRequired\":%s}",
             Esp8266Base::hostname(),
             persistedValid ? persisted : "",
             defaultHostname,
             rebootRequired ? "true" : "false");
    _server.send(200, "application/json", _wb);
}

void Esp8266BaseWeb::_handleHostnameApiPost() {
    _markRequest();
    if (!verifyAuth()) {
        _server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    char hostname[33] = "";
    strncpy(hostname, _server.arg("hostname").c_str(), sizeof(hostname) - 1);
    _trimWhitespace(hostname);

    if (!Esp8266Base::isValidHostname(hostname)) {
        ESP8266BASE_LOG_W("Web ", "hostname_api_update_rejected reason=invalid value=%s", hostname);
        _server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_hostname\"}");
        return;
    }

    if (!Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_HOSTNAME, hostname)) {
        ESP8266BASE_LOG_E("Web ", "hostname_api_update_failed hostname=%s", hostname);
        _server.send(500, "application/json", "{\"ok\":false,\"error\":\"save_failed\"}");
        return;
    }

    ESP8266BASE_LOG_I("Web ", "hostname_api_saved hostname=%s reboot_required=yes", hostname);
    _server.send(200, "application/json", "{\"ok\":true,\"rebootRequired\":true}");
}

void Esp8266BaseWeb::_handleSystemGet() {
    if (!checkAuth()) return;
    sendHeader();
    if (_server.hasArg("cleared")) {
        sendChunk("<p class=ok>File logs cleared.</p>");
    } else if (_server.hasArg("filelog_saved")) {
        sendChunk("<p class=ok>File log mode saved.</p>");
    } else if (_server.hasArg("hostname_saved")) {
        sendChunk("<p class=ok>Hostname saved. Reboot to apply network discovery changes.</p>");
    } else if (_server.hasArg("error")) {
        char err[24] = "";
        strncpy(err, _server.arg("error").c_str(), sizeof(err) - 1);
        if (strcmp(err, "clear_failed") == 0) {
            sendChunk("<p class=err>Failed to clear file logs.</p>");
        } else if (strcmp(err, "filelog_invalid_mode") == 0) {
            sendChunk("<p class=err>Invalid file log mode.</p>");
        } else if (strcmp(err, "filelog_save_failed") == 0) {
            sendChunk("<p class=err>Failed to save file log mode.</p>");
        } else if (strcmp(err, "hostname_invalid") == 0) {
            sendChunk("<p class=err>Invalid hostname.</p>");
        } else if (strcmp(err, "hostname_save_failed") == 0) {
            sendChunk("<p class=err>Failed to save hostname.</p>");
        }
    }
    sendChunk("<h2>System</h2><div class=grid>");
    _sendHostnameSystemSection();
    _sendFileLogSystemSection();
    sendContent_P(WEB_SYSTEM_PAGE);
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
    Esp8266BaseFileLog::flush();
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
    if (!checkAuth()) return;
    _server.send(404, "text/plain", "Not found");
}
#endif
