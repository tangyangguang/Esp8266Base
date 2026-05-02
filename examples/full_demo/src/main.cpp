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
 *
 * 首次使用：连接 AP "ESP8266-Config-XXXX" → http://192.168.4.1/ 配置 WiFi
 * 认证：admin / esp8266
 */

#include <Arduino.h>
#include "Esp8266Base.h"

// 自定义 Config key
static const char KEY_BOOT[] = "demo_boot";
static const char KEY_VAL[]  = "demo_val";

// 启动计数（从 Config 读取后加一）
static int32_t g_bootCount = 0;

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
    "<tr><td>Boot Count (Config)</td><td id='bc'>-</td></tr>"
    "<tr><td>WDT Resets (Watchdog)</td><td id='wdt'>-</td></tr>"
    "<tr><td>demo_val (Config)</td><td id='cv'>-</td></tr>"
    "</table>"
    "<script>"
    "function r(){"
    "fetch('/api/demo').then(x=>x.json()).then(d=>{"
    "document.getElementById('fw').textContent=d.fw+' v'+d.ver;"
    "document.getElementById('up').textContent=d.uptime+'s';"
    "document.getElementById('hp').textContent=d.heap+'B';"
    "document.getElementById('mb').textContent=d.maxblk+'B';"
    "document.getElementById('ip').textContent=d.ip;"
    "document.getElementById('dns').textContent=d.mdns;"
    "document.getElementById('ntp').textContent=d.ntp;"
    "document.getElementById('wake').textContent=d.wake;"
    "document.getElementById('bc').textContent=d.boot;"
    "document.getElementById('wdt').textContent=d.wdt_cnt;"
    "document.getElementById('cv').textContent=d.cfg_val;"
    "})}"
    "r();setInterval(r,2000);"
    "</script>";

// /ctrl — 控制面板（纯 GET 页面，动作提交到 POST /api/ctrl）
static const char PAGE_CTRL[] PROGMEM =
    "<h2>Control Panel</h2>"
    "<h3>Deep Sleep (Sleep module)</h3>"
    "<form method='post' action='/api/ctrl'>"
    "<input type='hidden' name='action' value='sleep'>"
    "Duration (s): "
    "<input type='number' name='sec' value='10' min='1' max='3600'"
    " style='width:70px'>"
    " <input type='submit' value='Enter Deep Sleep'>"
    "</form>"
    "<h3>Watchdog</h3>"
    "<form method='post' action='/api/ctrl'>"
    "<input type='hidden' name='action' value='wdt_clear'>"
    "<input type='submit' value='Clear WDT Reset Count'>"
    "</form>"
    "<h3>Config Write Test</h3>"
    "<form method='post' action='/api/ctrl'>"
    "<input type='hidden' name='action' value='cfg_write'>"
    "Value: "
    "<input type='text' name='val' maxlength='48' style='width:160px'>"
    " <input type='submit' value='Save to Flash'>"
    "</form>";

// ----------------------------------------------------------------
// Page handlers
// ----------------------------------------------------------------
void handleDemoPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PAGE_DEMO);
    Esp8266BaseWeb::sendFooter();
}

void handleCtrlPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(PAGE_CTRL);
    Esp8266BaseWeb::sendFooter();
}

// ----------------------------------------------------------------
// /api/demo — JSON 状态（GET，供 /demo 页面 JS 轮询）
// ----------------------------------------------------------------
void handleDemoApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;

    char timeBuf[20] = "not synced";
    Esp8266BaseNTP::formatTo(timeBuf, sizeof(timeBuf), "%H:%M:%S");

    char mdnsBuf[32];
    snprintf(mdnsBuf, sizeof(mdnsBuf), "%s.local", Esp8266Base::hostname());

    char cfgVal[48] = "";
    Esp8266BaseConfig::getStr(KEY_VAL, cfgVal, sizeof(cfgVal), "(empty)");

    char buf[280];
    snprintf(buf, sizeof(buf),
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
             cfgVal,
             millis() / 1000UL);

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
        int sec = Esp8266BaseWeb::server().arg("sec").toInt();
        if (sec < 1) sec = 10;

        char body[200];
        snprintf(body, sizeof(body),
                 "<!DOCTYPE html><html><head><meta charset=UTF-8></head>"
                 "<body><h2>Deep Sleep</h2>"
                 "<p>Sleeping for %d seconds...</p>"
                 "</body></html>", sec);
        Esp8266BaseWeb::server().send(200, "text/html", body);
        Esp8266BaseWeb::server().client().stop();
        delay(300);
        ESP8266BASE_LOG_I("App ", "Deep sleep triggered sec=%d", sec);
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
        ESP8266BASE_LOG_I("App ", "Config write %s val='%s'",
                          ok ? "OK" : "FAIL", val);
    }

    // 重定向回 /ctrl 页面
    Esp8266BaseWeb::server().sendHeader("Location", "/ctrl");
    Esp8266BaseWeb::server().send(303);
}

// ----------------------------------------------------------------
// setup / loop
// ----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);

    Esp8266Base::setFirmwareInfo("full-demo", "1.0.0");
    Esp8266Base::setHostname("esp-demo");

    Esp8266Base::enableWeb(true);
    Esp8266Base::enableOTA(true);
    Esp8266Base::enableNTP(true);
    Esp8266Base::enableMDNS(true);
    Esp8266Base::enableWatchdog(true);

    Esp8266Base::begin();

    // Config 演示：读取 + 递增启动计数（deferred 写，不阻塞启动）
    g_bootCount = Esp8266BaseConfig::getInt(KEY_BOOT, 0) + 1;
    Esp8266BaseConfig::setIntDeferred(KEY_BOOT, g_bootCount);

    // 注册路由（必须在 begin() 之后）
    // 2 页面 + 2 API，均在 4/6 上限内
    Esp8266BaseWeb::addPage("/demo",     handleDemoPage);
    Esp8266BaseWeb::addPage("/ctrl",     handleCtrlPage);
    Esp8266BaseWeb::addApi("/api/demo",  handleDemoApi);
    Esp8266BaseWeb::addApi("/api/ctrl",  handleCtrlApi);

    ESP8266BASE_LOG_I("App ",
                      "boot=%ld wake=%s wdt_prev=%s resets=%lu heap=%u",
                      (long)g_bootCount,
                      Esp8266BaseSleep::wakeReason(),
                      Esp8266BaseWatchdog::wasWatchdogReset() ? "YES" : "no",
                      (unsigned long)Esp8266BaseWatchdog::resetCount(),
                      (unsigned)ESP.getFreeHeap());
}

void loop() {
    Esp8266Base::handle();
}
