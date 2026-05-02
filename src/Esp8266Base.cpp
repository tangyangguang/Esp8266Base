#include "Esp8266Base.h"

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
char Esp8266Base::_fwName[24]    = "esp8266base";
char Esp8266Base::_fwVersion[16] = "0.1.0";
char Esp8266Base::_hostname[24]  = "esp8266";

bool Esp8266Base::_webEnabled      = true;
bool Esp8266Base::_otaEnabled      = true;
bool Esp8266Base::_ntpEnabled      = true;
bool Esp8266Base::_mdnsEnabled     = true;
bool Esp8266Base::_watchdogEnabled = true;

bool Esp8266Base::_ntpWasTriggered = false;
bool Esp8266Base::_mdnsWasStarted  = false;

// ----------------------------------------------------------------------------
// 启动前配置
// ----------------------------------------------------------------------------
void Esp8266Base::setFirmwareInfo(const char* name, const char* version) {
    if (name)    { strncpy(_fwName,    name,    sizeof(_fwName) - 1);    _fwName[sizeof(_fwName)-1] = '\0'; }
    if (version) { strncpy(_fwVersion, version, sizeof(_fwVersion) - 1); _fwVersion[sizeof(_fwVersion)-1] = '\0'; }
}

void Esp8266Base::setHostname(const char* hostname) {
    if (hostname) {
        strncpy(_hostname, hostname, sizeof(_hostname) - 1);
        _hostname[sizeof(_hostname) - 1] = '\0';
    }
}

void Esp8266Base::enableWeb(bool enabled)      { _webEnabled      = enabled; }
void Esp8266Base::enableOTA(bool enabled)      { _otaEnabled      = enabled; }
void Esp8266Base::enableNTP(bool enabled)      { _ntpEnabled      = enabled; }
void Esp8266Base::enableMDNS(bool enabled)     { _mdnsEnabled     = enabled; }
void Esp8266Base::enableWatchdog(bool enabled) { _watchdogEnabled = enabled; }

// ----------------------------------------------------------------------------
// begin — 按序初始化各模块
// ----------------------------------------------------------------------------
bool Esp8266Base::begin() {
    bool ok = true;

    // 1. Log — 最先初始化，保证后续日志可输出
    Esp8266BaseLog::begin();

    // 2. Sleep — 检测唤醒原因（在 Config 前，尽早记录）
    Esp8266BaseSleep::begin();

    // 3. Config — 挂载 LittleFS，加载配置
    if (!Esp8266BaseConfig::begin()) {
        ok = false;  // 继续运行，但配置读写不可用
    }

    // 4. WiFi — 读取凭证，启动状态机（非阻塞）
    Esp8266BaseWiFi::begin();

    // 5. Watchdog — begin() 后启动，使循环受监控
    if (_watchdogEnabled) {
        Esp8266BaseWatchdog::begin();
    }

    // 6. Web — 注册内置路由（OTA 路由由 OTA 模块在此后注册）
    if (_webEnabled) {
        Esp8266BaseWeb::setTitle(_hostname, _fwName, _fwVersion);
        Esp8266BaseWeb::begin();
    }

    // 7. OTA — 必须在 Web 启动后注册 POST /ota
    if (_webEnabled && _otaEnabled) {
        Esp8266BaseOTA::begin();
    }

    // 8. NTP / mDNS — 需要 WiFi 连接后触发（在 handle() 中检测）

    // 输出启动诊断
    logDiagnostics();

    return ok;
}

// ----------------------------------------------------------------------------
// handle — 每轮 loop 调用
// ----------------------------------------------------------------------------
void Esp8266Base::handle() {
    // 1. Config deferred 刷新
    Esp8266BaseConfig::handle();

    // 2. WiFi 状态机
    Esp8266BaseWiFi::handle();

    // 3. WiFi 连接后触发 NTP / mDNS；WiFi 掉线后重置 mDNS 标志以便重连后重启
    bool wifiNow = Esp8266BaseWiFi::isConnected();
    if (_ntpEnabled && !_ntpWasTriggered && wifiNow) {
        Esp8266BaseNTP::begin();
        _ntpWasTriggered = true;
    }
    if (_mdnsEnabled) {
        if (!_mdnsWasStarted && wifiNow) {
            Esp8266BaseMDNS::begin(_hostname);
            _mdnsWasStarted = true;
        } else if (_mdnsWasStarted && !wifiNow) {
            _mdnsWasStarted = false;  // WiFi 掉线，下次连上时重启 mDNS
        }
    }

    // 4. NTP handle（同步状态检查，每 5s 一次）
    if (_ntpEnabled && _ntpWasTriggered) {
        Esp8266BaseNTP::handle();
    }

    // 5. mDNS handle（MDNS.update()）
    if (_mdnsEnabled && _mdnsWasStarted) {
        Esp8266BaseMDNS::handle();
    }

    // 6. Web handle（server.handleClient()）
    if (_webEnabled) {
        Esp8266BaseWeb::handle();
    }

    // 7. Watchdog handle — 最后检查，再喂狗，确保本轮所有模块都已执行且未超时
    if (_watchdogEnabled) {
        Esp8266BaseWatchdog::handle();
        Esp8266BaseWatchdog::feed();
    }
}

// ----------------------------------------------------------------------------
// logDiagnostics — 标准启动诊断日志
// ----------------------------------------------------------------------------
void Esp8266Base::logDiagnostics() {
    char heapBuf[16];
    char maxBuf[16];
    Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
    Esp8266BaseUtil::formatBytes(ESP.getMaxFreeBlockSize(), maxBuf, sizeof(maxBuf));

    ESP8266BASE_LOG_I("Base", "firmware=%s version=%s free_heap=%s",
                      _fwName, _fwVersion, heapBuf);

    ESP8266BASE_LOG_I("SLEP", "wake_reason=%s", Esp8266BaseSleep::wakeReason());

    ESP8266BASE_LOG_I("Cfg ", "config_ready=%s pending_writes=%d/%d",
                      Esp8266BaseConfig::isReady() ? "yes" : "no",
                      (int)Esp8266BaseConfig::pendingCount(),
                      ESP8266BASE_CFG_DEFERRED_SIZE);

    {
        char ssid[64] = "";
        Esp8266BaseConfig::getStr("wifi_ssid", ssid, sizeof(ssid), "(none)");
        ESP8266BASE_LOG_I("WiFi", "saved_station_ssid=%s default_config_ap_ssid=%s",
                          ssid, Esp8266BaseWiFi::apSSID());
    }

    if (_watchdogEnabled) {
        ESP8266BASE_LOG_I("WDT ", "watchdog_enabled=yes previous_watchdog_reset=%s reset_count=%u",
                          Esp8266BaseWatchdog::wasWatchdogReset() ? "yes" : "no",
                          (unsigned)Esp8266BaseWatchdog::resetCount());
    }

    if (_webEnabled) {
        ESP8266BASE_LOG_I("Web ", "web_enabled=yes ota_enabled=%s", _otaEnabled ? "yes" : "no");
    }

    ESP8266BASE_LOG_I("Heap", "free_heap=%s max_block=%s", heapBuf, maxBuf);
}

// ----------------------------------------------------------------------------
// 查询
// ----------------------------------------------------------------------------
const char* Esp8266Base::firmwareName()    { return _fwName; }
const char* Esp8266Base::firmwareVersion() { return _fwVersion; }
const char* Esp8266Base::hostname()        { return _hostname; }
