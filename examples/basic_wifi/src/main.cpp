/**
 * basic_wifi — Phase 1 验收示例
 *
 * 验收标准：
 *   [x] 能通过串口命令保存 WiFi 凭证到 LittleFS
 *   [x] 重启后自动读取凭证连接 STA
 *   [x] STA 失败后进入 AP 配网模式
 *   [x] 无业务代码时联网后 free heap >= 28KB
 *   [x] 每 5 秒输出状态和 heap
 *
 * 串口命令（115200 baud，发送后按回车）：
 *   ssid:<your_ssid>   — 设置 SSID
 *   pass:<your_pass>   — 设置密码
 *   connect            — 保存并连接
 *   clear              — 清除凭证（重启后进入 AP 模式）
 *   diag               — 输出诊断日志
 *   restart            — 重启设备
 */

#include <Arduino.h>
#include "Esp8266Base.h"

// ---- 配置 ----
static const uint32_t STATUS_INTERVAL_MS = 5000;

// ---- 状态 ----
static uint32_t lastStatusMs = 0;

// ---- 串口命令缓冲 ----
static char    cmdBuf[128];
static uint8_t cmdLen       = 0;
static char    pendingSsid[64];
static char    pendingPass[64];

// ------------------------------------------------------------
// 串口命令处理
// ------------------------------------------------------------
static void processSerialInput() {
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());

        if (c == '\n' || c == '\r') {
            cmdBuf[cmdLen] = '\0';
            if (cmdLen == 0) { return; }

            if (strncmp(cmdBuf, "ssid:", 5) == 0) {
                strncpy(pendingSsid, cmdBuf + 5, sizeof(pendingSsid) - 1);
                pendingSsid[sizeof(pendingSsid) - 1] = '\0';
                ESP8266BASE_LOG_I("Cmd ", "SSID set: %s", pendingSsid);

            } else if (strncmp(cmdBuf, "pass:", 5) == 0) {
                strncpy(pendingPass, cmdBuf + 5, sizeof(pendingPass) - 1);
                pendingPass[sizeof(pendingPass) - 1] = '\0';
                ESP8266BASE_LOG_I("Cmd ", "password_buffer_updated password=%s password_length=%u",
                                  pendingPass, (unsigned)strlen(pendingPass));

            } else if (strcmp(cmdBuf, "connect") == 0) {
                if (strlen(pendingSsid) == 0) {
                    ESP8266BASE_LOG_W("Cmd ", "No SSID. Use: ssid:<name>");
                } else {
                    ESP8266BASE_LOG_I("Cmd ", "serial_connect_command ssid=%s password=%s password_length=%u",
                                      pendingSsid, pendingPass, (unsigned)strlen(pendingPass));
                    Esp8266BaseWiFi::connect(pendingSsid, pendingPass);
                    pendingSsid[0] = '\0';
                    pendingPass[0] = '\0';
                }

            } else if (strcmp(cmdBuf, "clear") == 0) {
                Esp8266BaseWiFi::clearCredentials();
                ESP8266BASE_LOG_I("Cmd ", "saved_wifi_credentials_cleared restart_to_enter_config_ap");

            } else if (strcmp(cmdBuf, "diag") == 0) {
                Esp8266Base::logDiagnostics();

            } else if (strcmp(cmdBuf, "restart") == 0) {
                ESP8266BASE_LOG_I("Cmd ", "restart_requested source=serial_command");
                Esp8266BaseConfig::flush();
                delay(200);
                ESP.restart();

            } else {
                ESP8266BASE_LOG_W("Cmd ", "Unknown: [%s]", cmdBuf);
                Serial.println(F("Commands: ssid:<n>  pass:<p>  connect  clear  diag  restart"));
            }

            cmdLen = 0;
        } else {
            if (cmdLen < sizeof(cmdBuf) - 1) {
                cmdBuf[cmdLen++] = c;
            }
        }
    }
}

// ------------------------------------------------------------
// 定期状态输出
// ------------------------------------------------------------
static void printStatus() {
    const char* stateStr;
    switch (Esp8266BaseWiFi::state()) {
        case Esp8266BaseWiFiState::CONNECTING: stateStr = "CONNECTING"; break;
        case Esp8266BaseWiFiState::CONNECTED:  stateStr = "CONNECTED";  break;
        case Esp8266BaseWiFiState::AP_CONFIG:  stateStr = "AP_CONFIG";  break;
        case Esp8266BaseWiFiState::FAILED:     stateStr = "FAILED";     break;
        default:                               stateStr = "IDLE";       break;
    }

    char heapBuf[16];
    char maxBuf[16];
    Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
    Esp8266BaseUtil::formatBytes(ESP.getMaxFreeBlockSize(), maxBuf, sizeof(maxBuf));

    ESP8266BASE_LOG_I("App ", "wifi_state=%s ip=%s free_heap=%s max_block=%s",
                      stateStr,
                      Esp8266BaseWiFi::isConnected() ? Esp8266BaseWiFi::ip() : "-",
                      heapBuf,
                      maxBuf);
}

// ============================================================
// setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println(F("=== Esp8266Base Phase 1: basic_wifi ==="));
    Serial.println(F("Commands: ssid:<n>  pass:<p>  connect  clear  diag  restart"));

    memset(pendingSsid, 0, sizeof(pendingSsid));
    memset(pendingPass, 0, sizeof(pendingPass));
    memset(cmdBuf,      0, sizeof(cmdBuf));

    // 配置固件信息（必须在 begin() 前）
    Esp8266Base::setFirmwareInfo("basic-wifi", "1.0.0");
    Esp8266Base::setHostname("esp-basic");

    // 初始化：Log → Config（LittleFS）→ WiFi → 诊断日志
    bool ok = Esp8266Base::begin();
    if (!ok) {
        // Config 初始化失败（LittleFS 问题），继续运行但配置不可持久化
        ESP8266BASE_LOG_E("App ", "Config init failed! Running without persistence.");
    }
}

// ============================================================
// loop
// ============================================================
void loop() {
    // 推进所有模块状态机（Config deferred + WiFi 状态机）
    Esp8266Base::handle();

    // 处理串口输入命令
    processSerialInput();

    // 每 5 秒输出状态
    uint32_t now = millis();
    if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
        lastStatusMs = now;
        printStatus();
    }
}
