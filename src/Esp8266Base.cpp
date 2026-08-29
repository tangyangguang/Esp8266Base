#include "Esp8266Base.h"

#if ESP8266BASE_USE_CONFIG
static uint32_t _loadBootCount(const char* invalidAction) {
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
        ESP8266BASE_LOG_W("Boot", "restart_count_invalid value=%s action=%s",
                          raw, invalidAction ? invalidAction : "use_0");
    }

    return count;
}
#endif

static uint32_t _loadAndIncrementBootCount() {
#if !ESP8266BASE_USE_CONFIG
    return 0;
#else
    if (!Esp8266BaseConfig::isReady()) return 0;

#if ESP8266BASE_USE_SLEEP
    if (Esp8266BaseSleep::isDeepSleepWake()) {
        // skip restart_count increment: periodic deep sleep wake is a normal resume path.
        uint32_t count = _loadBootCount("skip_increment");
        ESP8266BASE_LOG_I("Boot", "restart_count_skip reason=deep_sleep_wake value=%lu",
                          (unsigned long)count);
        return count;
    }
#endif

    uint32_t count = _loadBootCount("reset_to_1");

    if (count < 0xFFFFFFFFUL) {
        count++;
    } else {
        ESP8266BASE_LOG_W("Boot", "restart_count_saturated value=%lu", (unsigned long)count);
    }

    char next[11];
    snprintf(next, sizeof(next), "%lu", (unsigned long)count);
    Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_BOOT_COUNT, next);
    return count;
#endif
}

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
char Esp8266Base::_fwName[24]    = "esp8266base";
char Esp8266Base::_fwVersion[16] = "1.0.0";
char Esp8266Base::_hostname[33]  = "esp8266base";

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

bool Esp8266Base::isValidHostname(const char* hostname) {
    if (!hostname) return false;
    size_t len = strlen(hostname);
    if (len < 1 || len > 32) return false;
    if (hostname[0] == '-' || hostname[len - 1] == '-') return false;

    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '-';
        if (!ok) return false;
    }
    return true;
}

void Esp8266Base::_resolveHostname() {
    const char* selected = ESP8266BASE_DEFAULT_HOSTNAME;
    const char* source = "default";

    if (!isValidHostname(selected)) {
        ESP8266BASE_LOG_W("Base", "default_hostname_invalid value=%s action=fallback fallback=esp8266base",
                          selected ? selected : "(null)");
        selected = "esp8266base";
        source = "fallback";
    }

    // 默认 hostname 追加 MAC 后 4 位十六进制，保证同一固件烧到多台设备时默认名唯一；
    // 用户手动持久化的 hostname 仍优先（下方覆盖）。基础值限 27 字节以留出 "-xxxx"。
    char mac[13];
    Esp8266BaseWiFi::macAddressHex(mac, sizeof(mac));
    snprintf(_hostname, sizeof(_hostname), "%.27s-%s", selected, mac + 8);

#if ESP8266BASE_USE_CONFIG
    if (Esp8266BaseConfig::isReady()) {
        char persisted[33] = "";
        bool found = Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_HOSTNAME, persisted, sizeof(persisted), "");
        if (found && persisted[0]) {
            if (isValidHostname(persisted)) {
                strncpy(_hostname, persisted, sizeof(_hostname) - 1);
                _hostname[sizeof(_hostname) - 1] = '\0';
                source = "persisted";
            } else {
                ESP8266BASE_LOG_W("Base", "persisted_hostname_invalid value=%s action=ignored default=%s",
                                  persisted, _hostname);
            }
        }
    } else {
        ESP8266BASE_LOG_W("Base", "hostname_config_unavailable action=use_default host=%s", _hostname);
    }
#endif

    ESP8266BASE_LOG_I("Base", "hostname_resolved host=%s source=%s", _hostname, source);
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

    // 3. 可选持久化层：挂载与 Config/FileLog 分离。
#if ESP8266BASE_USE_FILESYSTEM
    if (!Esp8266BaseFilesystem::begin()) ok = false;
#endif
#if ESP8266BASE_USE_CONFIG
    if (!Esp8266BaseConfig::begin()) {
        ok = false;  // 继续运行，但配置读写不可用
    }
#endif

#if ESP8266BASE_USE_FILELOG
    if (!Esp8266BaseFileLog::begin()) {
        ok = false;
    }
#endif

    _resolveHostname();

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

    // 9. MQTT — 配置由业务在 begin() 前提供；实际连接由 handle() 等待 WiFi/NTP。
#if ESP8266BASE_USE_MQTT
    if (!Esp8266BaseMQTT::begin()) {
        ok = false;
    }
#endif

    // 10. NTP / mDNS — 需要 WiFi 连接后触发（在 handle() 中检测）

    // 输出启动诊断
    logDiagnostics();

    return ok;
}

// ----------------------------------------------------------------------------
// handle — 每轮 loop 调用
// ----------------------------------------------------------------------------
void Esp8266Base::handle() {
    // 1. Config deferred 刷新
#if ESP8266BASE_USE_CONFIG
    Esp8266BaseConfig::handle();
#endif
#if ESP8266BASE_USE_FILELOG
    Esp8266BaseFileLog::handle();
#endif

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
    // Web must run before MQTT: a secure DNS/TCP/TLS connect attempt can block
    // for seconds on ESP8266, while local control must remain responsive.
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

    // 7. MQTT handle：NTP 本轮推进后再检查时间门控。
#if ESP8266BASE_USE_MQTT
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
    Esp8266BaseMQTT::handle();
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::feed();
#endif
#endif

    // 8. Watchdog handle — 最后检查，再喂狗，确保本轮所有模块都已执行且未超时
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

    ESP8266BASE_LOG_I("Base", "firmware=%s version=%s hostname=%s free_heap=%s",
                      _fwName, _fwVersion, _hostname, heapBuf);

#if ESP8266BASE_USE_SLEEP
    ESP8266BASE_LOG_I("SLEP", "wake_reason=%s", Esp8266BaseSleep::wakeReason());
#else
    ESP8266BASE_LOG_I("SLEP", "sleep_module=disabled");
#endif

#if ESP8266BASE_USE_CONFIG
    ESP8266BASE_LOG_I("Cfg ", "config_ready=%s pending_writes=%d/%d",
                      Esp8266BaseConfig::isReady() ? "yes" : "no",
                      (int)Esp8266BaseConfig::pendingCount(),
                      ESP8266BASE_CFG_DEFERRED_SIZE);
#else
    ESP8266BASE_LOG_I("Cfg ", "config_module=disabled filesystem=%s",
                      ESP8266BASE_USE_FILESYSTEM ? "enabled" : "disabled");
#endif

    {
        char ssid[64] = "";
#if ESP8266BASE_USE_WIFI_CONFIG
        Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid), "(none)");
#else
        strncpy(ssid, Esp8266BaseWiFi::ssid(), sizeof(ssid) - 1);
#endif
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
    ESP8266BASE_LOG_I("Web ", "web_enabled=yes profile=%s ota_enabled=%s",
#if ESP8266BASE_PROFILE_MQTT_TERMINAL
                      "mqtt_terminal",
#else
                      "full",
#endif
#if ESP8266BASE_USE_OTA
                      "yes"
#else
                      "no"
#endif
    );
#else
    ESP8266BASE_LOG_I("Web ", "web_enabled=no ota_enabled=no");
#endif

#if ESP8266BASE_USE_MQTT
    ESP8266BASE_LOG_I("MQTT", "mqtt_enabled=yes configured=%s state=%s attempt=%lu",
                      Esp8266BaseMQTT::isConfigured() ? "yes" : "no",
                      Esp8266BaseMQTT::stateName(),
                      (unsigned long)Esp8266BaseMQTT::attemptCount());
#else
    ESP8266BASE_LOG_I("MQTT", "mqtt_enabled=no");
#endif

    ESP8266BASE_LOG_I("Heap", "free_heap=%s max_block=%s", heapBuf, maxBuf);
}

// ----------------------------------------------------------------------------
// 查询
// ----------------------------------------------------------------------------
const char* Esp8266Base::firmwareName()    { return _fwName; }
const char* Esp8266Base::firmwareVersion() { return _fwVersion; }
const char* Esp8266Base::hostname()        { return _hostname; }
