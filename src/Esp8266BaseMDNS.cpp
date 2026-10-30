#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_MDNS
#include "Esp8266BaseMDNS.h"
#include "Esp8266BaseLog.h"
#include <ESP8266mDNS.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool Esp8266BaseMDNS::_running = false;
bool Esp8266BaseMDNS::_retryPending = false;
uint32_t Esp8266BaseMDNS::_lastFailureMs = 0;

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseMDNS::begin(const char* hostname) {
    if (!hostname || strlen(hostname) == 0 || strlen(hostname) > 32) {
        ESP8266BASE_LOG_E("mDNS", "Invalid hostname");
        return false;
    }

    if (_retryPending && millis() - _lastFailureMs < 5000U) return false;
    _running = false;
    if (!MDNS.begin(hostname)) {
        _retryPending = true;
        _lastFailureMs = millis();
        ESP8266BASE_LOG_E("mDNS", "MDNS.begin() failed host=%s", hostname);
        return false;
    }

    // 广播 HTTP 服务
    MDNS.addService("http", "tcp", 80);

    _retryPending = false;
    _running = true;
    ESP8266BASE_LOG_I("mDNS", "mdns_started host=%s.local service=http tcp_port=80", hostname);
    return true;
}

// ----------------------------------------------------------------------------
// handle
// ----------------------------------------------------------------------------
void Esp8266BaseMDNS::handle() {
    if (!_running) return;
    MDNS.update();
}

// ----------------------------------------------------------------------------
// stop — radio reset 前确定性释放自持的 UDP PCB
// ----------------------------------------------------------------------------
void Esp8266BaseMDNS::stop() {
    if (!_running && !_retryPending) return;
    MDNS.end();  // close()：释放内部 UdpContext、离开组播；无 IP 时不发 goodbye
    _running = false;
    _retryPending = false;
    _lastFailureMs = 0;
    ESP8266BASE_LOG_I("mDNS", "mdns_stopped_for_radio_reset");
}

bool Esp8266BaseMDNS::isRunning() {
    return _running;
}
#endif
