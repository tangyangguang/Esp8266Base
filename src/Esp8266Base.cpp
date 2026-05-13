#include "Esp8266Base.h"

static uint32_t _loadAndIncrementBootCount() {
    if (!Esp8266BaseConfig::isReady()) return 0;

    char raw[16] = "";
    uint32_t count = 0;
    bool found = Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_BOOT_COUNT, raw, sizeof(raw), "");
    bool valid = found && raw[0] != '\0';
    for (size_t i = 0; valid && raw[i]; i++) {
        if (raw[i] < '0' || raw[i] > '9') valid = false;
    }

    if (valid) {
        count = (uint32_t)strtoul(raw, nullptr, 10);
    } else if (found) {
        ESP8266BASE_LOG_W("Boot", "boot_count_invalid value=%s action=reset_to_1", raw);
    }

    if (count < 0xFFFFFFFFUL) {
        count++;
    } else {
        ESP8266BASE_LOG_W("Boot", "boot_count_saturated value=%lu", (unsigned long)count);
    }

    char next[11];
    snprintf(next, sizeof(next), "%lu", (unsigned long)count);
    Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_BOOT_COUNT, next);
    return count;
}

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
char Esp8266Base::_fwName[24]    = "esp8266base";
char Esp8266Base::_fwVersion[16] = "1.0.0";
char Esp8266Base::_hostname[24]  = "esp8266";

#if ESP8266BASE_USE_NTP
bool Esp8266Base::_ntpWasTriggered = false;
#endif
#if ESP8266BASE_USE_MDNS
bool Esp8266Base::_mdnsWasStarted  = false;
#endif

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

// ----------------------------------------------------------------------------
// begin — 按序初始化各模块
// ----------------------------------------------------------------------------
bool Esp8266Base::begin() {
    bool ok = true;

    // 1. Log — 最先初始化，保证后续日志可输出
    Esp8266BaseLog::begin();

    // 2. Sleep — 检测唤醒原因（在 Config 前，尽早记录）
#if ESP8266BASE_USE_SLEEP
    Esp8266BaseSleep::begin();
#endif

    // 3. Config — 挂载 LittleFS，加载配置
    if (!Esp8266BaseConfig::begin()) {
        ok = false;  // 继续运行，但配置读写不可用
    }

    // 4. FileLog — Config ready 后加载 eb_log.mode，确保 boot session 可写文件
    if (!Esp8266BaseFileLog::begin()) {
        ok = false;
    }

    uint32_t bootCount = 0;
    bootCount = _loadAndIncrementBootCount();

    Esp8266BaseLog::beginBootSession(
        _fwName,
        _fwVersion,
#if ESP8266BASE_USE_SLEEP
        Esp8266BaseSleep::wakeReason(),
#else
        "unknown",
#endif
        bootCount,
        ESP.getFreeHeap()
    );

    // 5. WiFi — 读取凭证，启动状态机（非阻塞）
    Esp8266BaseWiFi::begin();

    // 6. Watchdog — begin() 后启动，使循环受监控
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::begin();
#endif

    // 7. Web — 注册内置路由（OTA 路由由 OTA 模块在此后注册）
#if ESP8266BASE_USE_WEB
    Esp8266BaseWeb::setSystemInfo(_hostname, _fwName, _fwVersion, bootCount);
    Esp8266BaseWeb::begin();
#endif

    // 8. OTA — 必须在 Web 启动后注册 POST /ota
#if ESP8266BASE_USE_OTA
    Esp8266BaseOTA::begin();
#endif

    // 9. NTP / mDNS — 需要 WiFi 连接后触发（在 handle() 中检测）

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
    Esp8266BaseFileLog::handle();

    // 2. WiFi 状态机
    Esp8266BaseWiFi::handle();

    // 3. WiFi 连接后触发 NTP / mDNS；WiFi 掉线后重置 mDNS 标志以便重连后重启
#if ESP8266BASE_USE_NTP || ESP8266BASE_USE_MDNS
    bool wifiNow = Esp8266BaseWiFi::isConnected();
#endif
#if ESP8266BASE_USE_NTP
    if (!_ntpWasTriggered && wifiNow) {
        Esp8266BaseNTP::begin();
        _ntpWasTriggered = true;
    } else if (_ntpWasTriggered && !wifiNow) {
        Esp8266BaseNTP::reset();
        _ntpWasTriggered = false;
    }
#endif
#if ESP8266BASE_USE_MDNS
    if (!_mdnsWasStarted && wifiNow) {
        Esp8266BaseMDNS::begin(_hostname);
        _mdnsWasStarted = true;
    } else if (_mdnsWasStarted && !wifiNow) {
        _mdnsWasStarted = false;  // WiFi 掉线，下次连上时重启 mDNS
    }
#endif

    // 4. NTP handle（同步状态检查，每 5s 一次）
#if ESP8266BASE_USE_NTP
    if (_ntpWasTriggered) {
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::feed();
#endif
        Esp8266BaseNTP::handle();
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::feed();
#endif
    }
#endif

    // 5. mDNS handle（MDNS.update()）
#if ESP8266BASE_USE_MDNS
    if (_mdnsWasStarted) {
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::feed();
#endif
        Esp8266BaseMDNS::handle();
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::feed();
#endif
    }
#endif

    // 6. Web handle（server.handleClient()）
    // Feed around Web I/O so slow clients do not trip the library watchdog.
    // Do not pause/resume here: handle() runs every loop and Debug logs would flood serial.
#if ESP8266BASE_USE_WEB
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
    Esp8266BaseWeb::handle();
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
#endif

    // 7. Watchdog handle — 最后检查，再喂狗，确保本轮所有模块都已执行且未超时
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::handle();
    Esp8266BaseWatchdog::feed();
#endif
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

#if ESP8266BASE_USE_SLEEP
    ESP8266BASE_LOG_I("SLEP", "wake_reason=%s", Esp8266BaseSleep::wakeReason());
#else
    ESP8266BASE_LOG_I("SLEP", "sleep_module=disabled");
#endif

    ESP8266BASE_LOG_I("Cfg ", "config_ready=%s pending_writes=%d/%d",
                      Esp8266BaseConfig::isReady() ? "yes" : "no",
                      (int)Esp8266BaseConfig::pendingCount(),
                      ESP8266BASE_CFG_DEFERRED_SIZE);

    {
        char ssid[64] = "";
        Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid), "(none)");
        ESP8266BASE_LOG_I("WiFi", "saved_station_ssid=%s default_config_ap_ssid=%s",
                          ssid, Esp8266BaseWiFi::apSSID());
    }

#if ESP8266BASE_USE_WATCHDOG
    ESP8266BASE_LOG_I("WDT ", "watchdog_enabled=yes previous_watchdog_reset=%s reset_count=%u",
                      Esp8266BaseWatchdog::wasWatchdogReset() ? "yes" : "no",
                      (unsigned)Esp8266BaseWatchdog::resetCount());
#else
    ESP8266BASE_LOG_I("WDT ", "watchdog_enabled=no");
#endif

#if ESP8266BASE_USE_WEB
    ESP8266BASE_LOG_I("Web ", "web_enabled=yes ota_enabled=%s",
#if ESP8266BASE_USE_OTA
                      "yes"
#else
                      "no"
#endif
    );
#else
    ESP8266BASE_LOG_I("Web ", "web_enabled=no ota_enabled=no");
#endif

    ESP8266BASE_LOG_I("Heap", "free_heap=%s max_block=%s", heapBuf, maxBuf);
}

// ----------------------------------------------------------------------------
// 查询
// ----------------------------------------------------------------------------
const char* Esp8266Base::firmwareName()    { return _fwName; }
const char* Esp8266Base::firmwareVersion() { return _fwVersion; }
const char* Esp8266Base::hostname()        { return _hostname; }
