/**
 * custom_web — 自定义 Web 页面完整示例
 *
 * 功能演示：
 *   - 注册应用自定义页面 /sensor（最多 4 个）
 *   - 注册应用自定义 API /api/sensor、/api/control（最多 6 个）
 *   - PROGMEM HTML + sendContent_P 分段输出（不占 DRAM）
 *   - checkAuth() Basic Auth 保护
 *   - 简短 JSON API 响应（snprintf，不用 ArduinoJson）
 *   - POST 请求参数解析示例
 *
 * 访问方式（设备连上 WiFi 后）：
 *   http://esp-sensor.local/sensor       传感器页面
 *   http://esp-sensor.local/api/sensor   JSON 数据接口
 *   http://esp-sensor.local/api/control  控制接口（POST ?value=1）
 *   http://esp-sensor.local/             内置控制台（WiFi / OTA / 重启）
 *
 * 烧录方法（PlatformIO）：
 *   pio run -e esp12f -t upload
 */

#include <Arduino.h>
#include "Esp8266Base.h"

// ============================================================
// 传感器模拟（替换为实际驱动，如 DHT22 / BMP280）
// ============================================================
static float readTemp() { return 25.3f + (float)(millis() % 100) / 100.0f; }
static float readHumi() { return 60.1f + (float)(millis() % 50)  / 100.0f; }
static int   outputPin  = 0;   // 模拟输出状态（0/1）

// ============================================================
// 传感器页面 HTML（全部放 PROGMEM）
// ============================================================
static const char SENSOR_PAGE[] PROGMEM =
    "<h2>Sensor Dashboard</h2>"
    "<table border='1' cellpadding='8' cellspacing='0'>"
    "<tr><th>Parameter</th><th>Value</th></tr>"
    "<tr><td>Temperature</td><td id='t'>-</td></tr>"
    "<tr><td>Humidity</td><td id='h'>-</td></tr>"
    "<tr><td>Uptime</td><td id='u'>-</td></tr>"
    "<tr><td>Free Heap</td><td id='hp'>-</td></tr>"
    "</table>"
    "<br>"
    "<form id='ctrl'>"
    "<label>Output (0/1): <input type='number' id='v' min='0' max='1' value='0'></label>"
    " <button type='button' onclick='sendCtrl()'>Set</button>"
    " <span id='cr'></span>"
    "</form>"
    "<script>"
    "function refresh(){"
    "  fetch('/api/sensor').then(r=>r.json()).then(d=>{"
    "    document.getElementById('t').textContent=d.temp.toFixed(1)+' C';"
    "    document.getElementById('h').textContent=d.humi.toFixed(1)+'%';"
    "    document.getElementById('u').textContent=d.uptime+'s';"
    "    document.getElementById('hp').textContent=d.heap+' B';"
    "  });"
    "}"
    "function sendCtrl(){"
    "  var v=document.getElementById('v').value;"
    "  fetch('/api/control?value='+v,{method:'POST'})"
    "    .then(r=>r.json())"
    "    .then(d=>document.getElementById('cr').textContent=d.ok?'OK':'FAIL');"
    "}"
    "refresh(); setInterval(refresh,3000);"
    "</script>";

// ============================================================
// 页面 / API 处理器
// ============================================================

void handleSensorPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(SENSOR_PAGE);
    Esp8266BaseWeb::sendFooter();
}

void handleSensorApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"temp\":%.1f,\"humi\":%.1f,\"uptime\":%lu,\"heap\":%u}",
             readTemp(), readHumi(),
             millis() / 1000UL,
             (unsigned)ESP.getFreeHeap());
    Esp8266BaseWeb::server().send(200, "application/json", buf);
}

void handleControlApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    // 从 URL 参数读取 value（不使用 String 持久化，仅局部处理）
    String valStr = Esp8266BaseWeb::server().arg("value");
    int val = valStr.toInt();
    if (val < 0) val = 0;
    if (val > 1) val = 1;
    outputPin = val;
    // 实际项目：digitalWrite(OUTPUT_PIN, outputPin);
    ESP8266BASE_LOG_I("App ", "control output=%d", outputPin);
    Esp8266BaseWeb::server().send(200, "application/json", "{\"ok\":true}");
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(100);

    Esp8266Base::setFirmwareInfo("custom-web", "0.2.0");
    Esp8266Base::setHostname("esp-sensor");

    Esp8266Base::enableWeb(true);
    Esp8266Base::enableOTA(true);
    Esp8266Base::enableNTP(true);
    Esp8266Base::enableMDNS(true);
    Esp8266Base::enableWatchdog(true);

    Esp8266Base::begin();

    // 注册自定义路由（必须在 begin() 之后）
    Esp8266BaseWeb::addPage("/sensor",      handleSensorPage);
    Esp8266BaseWeb::addApi ("/api/sensor",  handleSensorApi);
    Esp8266BaseWeb::addApi ("/api/control", handleControlApi);
}

void loop() {
    Esp8266Base::handle();
}
