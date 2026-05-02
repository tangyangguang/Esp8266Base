#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_MDNS
#include "Esp8266BaseMDNS.h"
#include "Esp8266BaseLog.h"
#include <ESP8266mDNS.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool Esp8266BaseMDNS::_running = false;

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseMDNS::begin(const char* hostname) {
    if (!hostname || strlen(hostname) == 0 || strlen(hostname) > 24) {
        ESP8266BASE_LOG_E("mDNS", "Invalid hostname");
        return false;
    }

    if (!MDNS.begin(hostname)) {
        ESP8266BASE_LOG_E("mDNS", "MDNS.begin() failed host=%s", hostname);
        return false;
    }

    // 广播 HTTP 服务
    MDNS.addService("http", "tcp", 80);

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

bool Esp8266BaseMDNS::isRunning() {
    return _running;
}
#endif
