/**
 * wifi_config_ota — Web + OTA 完整示例
 *
 * 功能演示：
 *   - 通过 http://esp8266base-ota.local/ 访问 Web 控制台（Basic Auth: admin/admin）
 *   - 通过 /wifi 修改 WiFi 凭证
 *   - 通过 /ota  上传固件并自动重启
 *   - 自定义页面 /status 显示实时运行状态
 *   - JSON API /api/status 返回 heap、IP、uptime、NTP 同步状态
 *   - NTP 同步后控制台日志自动切换为绝对时间
 *
 * 烧录方法（PlatformIO）：
 *   pio run -e esp12f -t upload
 *
 * OTA 更新方法：
 *   curl -u admin:admin -F "firmware=@.pio/build/esp12f/firmware.bin" http://esp8266base-ota.local/ota
 */

#include <Arduino.h>
#include "Esp8266Base.h"

// ----------------------------------------------------------------------------
// PROGMEM 自定义页面内容
// ----------------------------------------------------------------------------
static const char STATUS_PAGE[] PROGMEM =
    "<h2>Status</h2>"
    "<table border='1' cellpadding='6' cellspacing='0'>"
    "<tr><th>Key</th><th>Value</th></tr>"
    "<tr><td>Firmware</td><td id='fw'>-</td></tr>"
    "<tr><td>Uptime</td><td id='up'>-</td></tr>"
    "<tr><td>Free Heap</td><td id='hp'>-</td></tr>"
    "<tr><td>IP Address</td><td id='ip'>-</td></tr>"
    "<tr><td>NTP Synced</td><td id='ntp'>-</td></tr>"
    "</table>"
    "<script>"
    "function fb(n){if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return(n/1048576).toFixed(1)+' MB';}"
    "function refresh(){fetch('/api/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('fw').textContent=d.firmware;"
    "document.getElementById('up').textContent=d.uptime+'s';"
    "document.getElementById('hp').textContent=fb(d.heap);"
    "document.getElementById('ip').textContent=d.ip;"
    "document.getElementById('ntp').textContent=d.ntp_synced?'Yes':'No';"
    "})}"
    "refresh();setInterval(refresh,3000);"
    "</script>";

// ----------------------------------------------------------------------------
// 自定义页面处理器
// ----------------------------------------------------------------------------
void handleStatusPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(STATUS_PAGE);
    Esp8266BaseWeb::sendFooter();
}

// ----------------------------------------------------------------------------
// JSON API
// ----------------------------------------------------------------------------
void handleStatusApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"firmware\":\"%s\",\"version\":\"%s\","
             "\"heap\":%u,\"ip\":\"%s\","
             "\"uptime\":%lu,\"ntp_synced\":%s}",
             Esp8266Base::firmwareName(),
             Esp8266Base::firmwareVersion(),
             (unsigned)ESP.getFreeHeap(),
             Esp8266BaseWiFi::isConnected() ? Esp8266BaseWiFi::ip() : "0.0.0.0",
             millis() / 1000UL,
             Esp8266BaseNTP::isSynced() ? "true" : "false");

    Esp8266BaseWeb::server().send(200, "application/json", buf);
}

// ----------------------------------------------------------------------------
// setup / loop
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);

    Esp8266Base::setFirmwareInfo("wifi-ota", "0.2.0");

    Esp8266Base::begin();

    // 注册自定义路由（必须在 begin() 之后，Web 服务已启动）
    Esp8266BaseWeb::addPage("/status",     handleStatusPage);
    Esp8266BaseWeb::addApi ("/api/status", handleStatusApi);
}

void loop() {
    Esp8266Base::handle();
}
