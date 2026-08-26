#include <Arduino.h>
#include "Esp8266Base.h"

static bool businessCommunicationAllowed = true;
static bool actuatorBusy = false;

static const char TERMINAL_PAGE[] PROGMEM =
    "<h2>Terminal</h2><p>Minimal application page.</p>";

bool prepareOta(char* reason, size_t reasonLen) {
    if (actuatorBusy) {
        snprintf(reason, reasonLen, "Actuator is busy");
        return false;
    }

    // 业务项目应在此拒绝新命令、确认输出安全、断开 MQTT 并释放 TLS。
    businessCommunicationAllowed = false;
    return true;
}

void recoverAfterOtaFailure(Esp8266BaseOTAFailure failure) {
    (void)failure;
    // 此回调每个失败请求最多调用一次；恢复逻辑应保持幂等。
    businessCommunicationAllowed = true;
}

void finishOtaSafety() {
    // 成功后保持业务通信关闭，基础库随后 flush 并重启。
    businessCommunicationAllowed = false;
}

void handleTerminalPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(TERMINAL_PAGE);
    Esp8266BaseWeb::sendFooter();
}

void handleStateApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    char json[96];
    snprintf(json, sizeof(json),
             "{\"communicationAllowed\":%s,\"actuatorBusy\":%s}",
             businessCommunicationAllowed ? "true" : "false",
             actuatorBusy ? "true" : "false");
    Esp8266BaseWeb::server().send(200, "application/json", json);
}

void handleStartApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    actuatorBusy = true;
    Esp8266BaseWeb::server().send(200, "text/plain", "busy");
}

void handleStopApi() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    actuatorBusy = false;
    Esp8266BaseWeb::server().send(200, "text/plain", "idle");
}

void setup() {
    Serial.begin(115200);
    Esp8266Base::setFirmwareInfo("minimal-terminal", "1.0.0");
    Esp8266BaseOTA::setLifecycleCallbacks(
        prepareOta, recoverAfterOtaFailure, finishOtaSafety);

    Esp8266Base::begin();
    Esp8266BaseWeb::addPage("/terminal", "Terminal", handleTerminalPage);
    Esp8266BaseWeb::addApi("/api/state", handleStateApi);
    Esp8266BaseWeb::addApi("/api/start", handleStartApi);
    Esp8266BaseWeb::addApi("/api/stop", handleStopApi);
}

void loop() {
    Esp8266Base::handle();
}
