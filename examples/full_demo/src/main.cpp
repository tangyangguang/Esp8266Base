/**
 * full_demo — Esp8266Base 全模块演示
 *
 * 演示模块：
 *   Log      — 串口日志，NTP 同步后自动切换为绝对时间戳
 *   Config   — 自定义 demo_boot 计数 + demo_val 字符串 KV 存储
 *   WiFi     — STA 自动连接 + AP 配网（首次使用）
 *   Web      — 2 页面 /demo /ctrl + 2 API /api/demo /api/ctrl
 *   OTA      — 内置 /ota 页面，Web 刷机
 *   NTP      — 网络对时，状态页显示当前时间
 *   mDNS     — http://esp-demo.local/
 *   Sleep    — 唤醒原因显示 + Web 触发深度睡眠
 *   Watchdog — WDT 重启计数显示 + Web 清零
 *   Button   — GPIO0 长按 1 秒清除全部 /cfg_* 配置并重启
 *   LED      — GPIO2 板载指示灯（低电平亮）：联网常亮，AP 慢闪，连接中快闪
 *
 * 首次使用：连接 AP "ESP8266-Config-XXXX" → http://192.168.4.1/ 配置 WiFi
 * 认证：admin / esp8266
 */

#include <Arduino.h>
#include "Esp8266Base.h"
#include <LittleFS.h>

// 自定义 Config key
static const char KEY_BOOT[] = "demo_boot";
static const char KEY_VAL[]  = "demo_val";
static const char DEFAULT_DEMO_VAL[] = "demo-default";

// ESP-12F 常见板载资源：GPIO0 FLASH 键，GPIO2 蓝色 LED（active-low）
static const uint8_t  BOARD_BUTTON_PIN = 0;
static const uint8_t  BOARD_LED_PIN    = 2;
static const uint32_t CLEAR_HOLD_MS    = 1000;

// full_demo 自定义启动计数（库级 eb_boot_count 由 Esp8266Base 自动维护）
static int32_t g_bootCount = 0;
static uint32_t g_buttonDownAt = 0;
static bool     g_clearFired   = false;
static uint32_t g_ledLastMs    = 0;
static bool     g_ledBlinkOn   = false;

// ----------------------------------------------------------------
// PROGMEM HTML
// ----------------------------------------------------------------

// /demo — 状态仪表板（JS 每 2s 轮询 /api/demo）
static const char PAGE_DEMO[] PROGMEM =
    "<h2>Status Dashboard</h2>"
    "<table border='1' cellpadding='5' cellspacing='0' style='width:100%'>"
    "<tr><th>Module</th><th>Value</th></tr>"
    "<tr><td>Firmware</td><td id='fw'>-</td></tr>"
    "<tr><td>Uptime</td><td id='up'>-</td></tr>"
    "<tr><td>Free Heap</td><td id='hp'>-</td></tr>"
    "<tr><td>Max Block</td><td id='mb'>-</td></tr>"
    "<tr><td>WiFi IP</td><td id='ip'>-</td></tr>"
    "<tr><td>mDNS</td><td id='dns'>-</td></tr>"
    "<tr><td>NTP Time</td><td id='ntp'>-</td></tr>"
    "<tr><td>Wake Reason</td><td id='wake'>-</td></tr>"
    "<tr><td>Board Button</td><td>GPIO0 long press 1s clears config</td></tr>"
    "<tr><td>Board LED</td><td>GPIO2 active-low</td></tr>"
    "<tr><td>Boot Count (Config)</td><td id='bc'>-</td></tr>"
    "<tr><td>WDT Resets (Watchdog)</td><td id='wdt'>-</td></tr>"
    "<tr><td>demo_val (Config)</td><td id='cv'>-</td></tr>"
    "</table>"
    "<script>"
    "function fb(n){if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return(n/1048576).toFixed(1)+' MB';}"
    "function a(d){"
    "document.getElementById('fw').textContent=d.fw+' v'+d.ver;"
    "document.getElementById('up').textContent=d.uptime+'s';"
    "document.getElementById('hp').textContent=fb(d.heap);"
    "document.getElementById('mb').textContent=fb(d.maxblk);"
    "document.getElementById('ip').textContent=d.ip;"
    "document.getElementById('dns').textContent=d.mdns;"
    "document.getElementById('ntp').textContent=d.ntp;"
    "document.getElementById('wake').textContent=d.wake;"
    "document.getElementById('bc').textContent=d.boot;"
    "document.getElementById('wdt').textContent=d.wdt_cnt;"
    "document.getElementById('cv').textContent=d.cfg_val;"
    "}"
    "function r(){fetch('/api/demo').then(x=>x.json()).then(a)}";

static const char PAGE_DEMO_POST[] PROGMEM =
    "setInterval(r,2000);"
    "</script>";

// /ctrl — 控制面板（纯 GET 页面，动作提交到 POST /api/ctrl）
static const char PAGE_CTRL[] PROGMEM =
    "<h2>Control Panel</h2>"
    "<h3>Stored Config</h3>";

static const char PAGE_CTRL_ACTIONS[] PROGMEM =
    "<h3>Deep Sleep (Sleep module)</h3>"
    "<form method='post' action='/api/ctrl' onsubmit=\"return confirm('Enter deep sleep now? ESP8266 wakes only if GPIO16 is wired to RST.')&&once(this)\">"
    "<input type='hidden' name='action' value='sleep'>"
    "Duration (s): "
    "<input type='number' name='sec' value='10' min='1' max='3600'"
    " style='width:70px'>"
    " <input type='submit' value='Enter Deep Sleep'>"
    "</form>"
    "<h3>Watchdog</h3>"
    "<form method='post' action='/api/ctrl' onsubmit='return once(this)'>"
    "<input type='hidden' name='action' value='wdt_clear'>"
    "<input type='submit' value='Clear WDT Reset Count'>"
    "</form>"
    "<h3>Config Write Test</h3>"
    "<form method='post' action='/api/ctrl' onsubmit='return once(this)'>"
    "<input type='hidden' name='action' value='cfg_write'>"
    "Value: "
    "<input type='text' name='val' maxlength='48' style='width:160px'>"
    " <input type='submit' value='Save to Flash'>"
    "</form>";

// ----------------------------------------------------------------
// Page handlers
// ----------------------------------------------------------------
static void jsonEscape(const char* in, char* out, size_t outLen);
static void sendHtmlEscaped(const char* in);
static void sendConfigTable();
static void buildDemoJson(char* buf, size_t len);
static bool g_sleepPending = false;

void handleDemoPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PAGE_DEMO);
    char buf[360];
    buildDemoJson(buf, sizeof(buf));
    Esp8266BaseWeb::sendChunk("a(");
    Esp8266BaseWeb::sendChunk(buf);
    Esp8266BaseWeb::sendChunk(");");
    Esp8266BaseWeb::sendContent_P(PAGE_DEMO_POST);
    Esp8266BaseWeb::sendFooter();
}

void handleCtrlPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PAGE_CTRL);
    sendConfigTable();
    Esp8266BaseWeb::sendContent_P(PAGE_CTRL_ACTIONS);
    Esp8266BaseWeb::sendFooter();
}

static void jsonEscape(const char* in, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t n = 0;
    while (*in && n + 1 < outLen) {
        char c = *in++;
        if ((c == '"' || c == '\\') && n + 2 < outLen) {
            out[n++] = '\\';
            out[n++] = c;
        } else if ((uint8_t)c < 0x20) {
            out[n++] = ' ';
        } else {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

static void sendHtmlEscaped(const char* in) {
    if (!in) return;
    char out[96];
    size_t n = 0;
    while (*in) {
        const char* repl = nullptr;
        switch (*in) {
            case '&': repl = "&amp;"; break;
            case '<': repl = "&lt;"; break;
            case '>': repl = "&gt;"; break;
            case '"': repl = "&quot;"; break;
            default: break;
        }
        if (repl) {
            for (const char* p = repl; *p; p++) {
                out[n++] = *p;
                if (n == sizeof(out) - 1) {
                    out[n] = '\0';
                    Esp8266BaseWeb::sendChunk(out);
                    n = 0;
                }
            }
        } else {
            out[n++] = *in;
            if (n == sizeof(out) - 1) {
                out[n] = '\0';
                Esp8266BaseWeb::sendChunk(out);
                n = 0;
            }
        }
        in++;
    }
    if (n > 0) {
        out[n] = '\0';
        Esp8266BaseWeb::sendChunk(out);
    }
}

static void sendConfigTable() {
    Esp8266BaseConfig::flush();
    Esp8266BaseWeb::sendChunk(
        "<table border='1' cellpadding='5' cellspacing='0' style='width:100%'>"
        "<tr><th>Key</th><th>Value</th></tr>");

    bool any = false;
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) {
        String name = dir.fileName();
        const char* key = nullptr;
        if (name.startsWith("/cfg_")) {
            key = name.c_str() + 5;
        } else if (name.startsWith("cfg_")) {
            key = name.c_str() + 4;
        } else {
            continue;
        }
        any = true;

        char value[ESP8266BASE_CFG_STR_MAX + 1] = "";
        File f = LittleFS.open(name, "r");
        if (f) {
            size_t n = f.readBytes(value, sizeof(value) - 1);
            value[n] = '\0';
            f.close();
        }

        Esp8266BaseWeb::sendChunk("<tr><td>");
        sendHtmlEscaped(key);
        Esp8266BaseWeb::sendChunk("</td><td>");
        sendHtmlEscaped(value);
        Esp8266BaseWeb::sendChunk("</td></tr>");
        yield();
    }

    if (!any) {
        Esp8266BaseWeb::sendChunk("<tr><td colspan='2'>(no stored config)</td></tr>");
    }
    Esp8266BaseWeb::sendChunk("</table>");
}

static void buildDemoJson(char* buf, size_t len) {
    char timeBuf[20] = "not synced";
    Esp8266BaseNTP::formatTo(timeBuf, sizeof(timeBuf), "%H:%M:%S");

    char mdnsBuf[32];
    snprintf(mdnsBuf, sizeof(mdnsBuf), "%s.local", Esp8266Base::hostname());

    char cfgVal[48] = "";
    Esp8266BaseConfig::getStr(KEY_VAL, cfgVal, sizeof(cfgVal), "(empty)");
    char cfgJson[100];
    jsonEscape(cfgVal, cfgJson, sizeof(cfgJson));

    snprintf(buf, len,
             "{\"fw\":\"%s\",\"ver\":\"%s\","
             "\"heap\":%u,\"maxblk\":%u,"
             "\"ip\":\"%s\",\"mdns\":\"%s\","
             "\"ntp\":\"%s\",\"wake\":\"%s\","
             "\"boot\":%ld,\"wdt_cnt\":%lu,"
             "\"cfg_val\":\"%s\","
             "\"uptime\":%lu}",
             Esp8266Base::firmwareName(),
             Esp8266Base::firmwareVersion(),
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMaxFreeBlockSize(),
             Esp8266BaseWiFi::ip(),
             mdnsBuf,
             timeBuf,
             Esp8266BaseSleep::wakeReason(),
             (long)g_bootCount,
             (unsigned long)Esp8266BaseWatchdog::resetCount(),
             cfgJson,
             millis() / 1000UL);
}

// ----------------------------------------------------------------
// /api/demo — JSON 状态（GET，供 /demo 页面 JS 轮询）
// ----------------------------------------------------------------
void handleDemoApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    char buf[360];
    buildDemoJson(buf, sizeof(buf));
    Esp8266BaseWeb::server().send(200, "application/json", buf);
}

// ----------------------------------------------------------------
// /api/ctrl — 控制动作（POST）
// ----------------------------------------------------------------
void handleCtrlApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;

    char action[16] = "";
    strncpy(action,
            Esp8266BaseWeb::server().arg("action").c_str(),
            sizeof(action) - 1);

    if (strcmp(action, "sleep") == 0) {
        if (g_sleepPending) {
            Esp8266BaseWeb::server().send(409, "text/plain", "Deep sleep already requested");
            return;
        }
        g_sleepPending = true;

        int sec = Esp8266BaseWeb::server().arg("sec").toInt();
        if (sec < 1) sec = 10;

        char body[420];
        snprintf(body, sizeof(body),
                 "<!DOCTYPE html><html><head><meta charset=UTF-8></head>"
                 "<body><h2>Deep Sleep</h2>"
                 "<p>Command accepted. Sleeping for %d seconds...</p>"
                 "<p>ESP8266 deep sleep turns WiFi and Web off. The device wakes automatically only when GPIO16 is wired to RST; otherwise press RST or power-cycle it.</p>"
                 "<p>After wake, reopen <a href='/demo'>/demo</a>.</p>"
                 "</body></html>", sec);
        Esp8266BaseWeb::server().send(200, "text/html", body);
        Esp8266BaseWeb::server().client().flush();
        Esp8266BaseWeb::server().client().stop();
        delay(1200);
        ESP8266BASE_LOG_I("App ", "deep_sleep_requested source=web duration=%ds wake_requires=GPIO16_to_RST",
                          sec);
        Esp8266BaseSleep::deepSleep((uint32_t)sec);
        return;  // not reached

    } else if (strcmp(action, "wdt_clear") == 0) {
        Esp8266BaseWatchdog::clearResetCount();
        ESP8266BASE_LOG_I("App ", "WDT count cleared via web");

    } else if (strcmp(action, "cfg_write") == 0) {
        char val[48] = "";
        strncpy(val,
                Esp8266BaseWeb::server().arg("val").c_str(),
                sizeof(val) - 1);
        bool ok = Esp8266BaseConfig::setStr(KEY_VAL, val);
        ESP8266BASE_LOG_I("App ", "config_save key=%s value=%s result=%s",
                          KEY_VAL, val, ok ? "success" : "failed");
        char readBack[48] = "";
        bool got = Esp8266BaseConfig::getStr(KEY_VAL, readBack, sizeof(readBack), "");
        ESP8266BASE_LOG_I("App ", "config_read_after_save key=%s found=%s value=%s",
                          KEY_VAL, got ? "yes" : "no", readBack);
    }

    // 重定向回 /ctrl 页面
    Esp8266BaseWeb::server().sendHeader("Location", "/ctrl");
    Esp8266BaseWeb::server().send(303);
}

// ----------------------------------------------------------------
// 板载按钮 / LED
// ----------------------------------------------------------------
static void setBoardLed(bool on) {
    digitalWrite(BOARD_LED_PIN, on ? LOW : HIGH);
}

static void updateBoardLed() {
    uint32_t now = millis();
    Esp8266BaseWiFiState state = Esp8266BaseWiFi::state();

    if (state == Esp8266BaseWiFiState::CONNECTED) {
        setBoardLed(true);
        return;
    }

    uint32_t interval = (state == Esp8266BaseWiFiState::AP_CONFIG) ? 1000UL : 250UL;
    if (now - g_ledLastMs >= interval) {
        g_ledLastMs = now;
        g_ledBlinkOn = !g_ledBlinkOn;
        setBoardLed(g_ledBlinkOn);
    }
}

static void handleBoardButton() {
    bool pressed = (digitalRead(BOARD_BUTTON_PIN) == LOW);
    uint32_t now = millis();

    if (!pressed) {
        g_buttonDownAt = 0;
        g_clearFired = false;
        return;
    }

    if (g_buttonDownAt == 0) {
        g_buttonDownAt = now;
        return;
    }

    if (!g_clearFired && now - g_buttonDownAt >= CLEAR_HOLD_MS) {
        g_clearFired = true;
        setBoardLed(true);
        ESP8266BASE_LOG_W("Btn ", "button_long_press_detected pin=GPIO0 duration=1s action=clear_all_config_and_restart");
        Esp8266BaseWatchdog::pause();
        Esp8266BaseConfig::clearAll();
        Esp8266BaseLog::flushFileSink();
        delay(300);
        ESP.restart();
    }
}

// ----------------------------------------------------------------
// setup / loop
// ----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);

    Esp8266BaseLog::enableFileSink("/logs/app.log", 16384, ESP8266BASE_LOG_FILE_LEVEL, 4);
    Esp8266BaseLog::enableConfigAudit(true);
    Esp8266BaseLog::enableConfigReadAudit(false);

    pinMode(BOARD_BUTTON_PIN, INPUT_PULLUP);
    pinMode(BOARD_LED_PIN, OUTPUT);
    setBoardLed(false);

    Esp8266Base::setFirmwareInfo("full-demo", "1.0.0");
    Esp8266Base::setHostname("esp-demo");
    Esp8266BaseWeb::setDeviceName("Full Demo");
    Esp8266BaseWeb::setHomePath("/demo");
    Esp8266BaseWeb::setHomeMode(Esp8266BaseWebHomeMode::FUSED_HOME);
    Esp8266BaseWeb::setSystemNavMode(Esp8266BaseWebSystemNavMode::FOOTER_COMPACT);
    Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::HOME, "System");
    Esp8266BaseWeb::setBuiltinLabel(Esp8266BaseWebBuiltinLabel::REBOOT, "Restart");

    Esp8266Base::begin();

    // Config 演示：读取 + 递增 demo_boot（deferred 写，不阻塞启动）
    g_bootCount = Esp8266BaseConfig::getInt(KEY_BOOT, 0) + 1;
    Esp8266BaseConfig::setIntDeferred(KEY_BOOT, g_bootCount);
    ESP8266BASE_LOG_I("App ", "config_deferred_save key=%s value=%ld",
                      KEY_BOOT, (long)g_bootCount);

    char demoVal[48] = "";
    bool hasDemoVal = Esp8266BaseConfig::getStr(KEY_VAL, demoVal, sizeof(demoVal), "");
    ESP8266BASE_LOG_I("App ", "config_read key=%s found=%s value=%s",
                      KEY_VAL, hasDemoVal ? "yes" : "no", demoVal);
    if (!hasDemoVal || demoVal[0] == '\0') {
        bool saved = Esp8266BaseConfig::setStr(KEY_VAL, DEFAULT_DEMO_VAL);
        ESP8266BASE_LOG_I("App ", "config_save_default key=%s value=%s result=%s",
                          KEY_VAL, DEFAULT_DEMO_VAL, saved ? "success" : "failed");
        Esp8266BaseConfig::getStr(KEY_VAL, demoVal, sizeof(demoVal), "");
        ESP8266BASE_LOG_I("App ", "config_read_after_default_save key=%s value=%s",
                          KEY_VAL, demoVal);
    }

    // 注册路由（必须在 begin() 之后）
    // 2 页面 + 2 API，均在 4/6 上限内
    Esp8266BaseWeb::addPage("/demo",     "Demo",    handleDemoPage);
    Esp8266BaseWeb::addPage("/ctrl",     "Control", handleCtrlPage);
    Esp8266BaseWeb::addApi("/api/demo",  handleDemoApi);
    Esp8266BaseWeb::addApi("/api/ctrl",  handleCtrlApi);

    char heapBuf[16];
    Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
    ESP8266BASE_LOG_I("App ",
                      "app_started boot_count=%ld wake_reason=%s previous_watchdog_reset=%s watchdog_reset_count=%lu free_heap=%s",
                      (long)g_bootCount,
                      Esp8266BaseSleep::wakeReason(),
                      Esp8266BaseWatchdog::wasWatchdogReset() ? "yes" : "no",
                      (unsigned long)Esp8266BaseWatchdog::resetCount(),
                      heapBuf);
}

void loop() {
    Esp8266Base::handle();
    handleBoardButton();
    updateBoardLed();
}
