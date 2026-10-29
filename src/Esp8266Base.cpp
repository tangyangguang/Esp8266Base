#include "Esp8266Base.h"
#if ESP8266BASE_USE_CRASH
#include "Esp8266BaseCrash.h"
#endif
#if ESP8266BASE_USE_JOURNAL
#include "Esp8266BaseJournal.h"
#endif
#include <user_interface.h>
// radio reset 有序停网需关闭全部 UDP/TCP PCB（stopAll）。
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

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
uint32_t Esp8266Base::_bootCount = 0;

#if ESP8266BASE_USE_NTP
bool Esp8266Base::_ntpWasTriggered = false;
#endif
#if ESP8266BASE_USE_MDNS
bool Esp8266Base::_mdnsWasStarted  = false;
#endif

#if ESP8266BASE_USE_WATCHDOG
bool     Esp8266Base::_linkOutageActive        = false;
uint32_t Esp8266Base::_linkOutageStartedAt     = 0;
uint8_t  Esp8266Base::_linkOutageRadioBaseline = 0;
uint32_t Esp8266Base::_linkRestartRetryAt      = 0;
#endif

// ----------------------------------------------------------------------------
// 启动前配置
// ----------------------------------------------------------------------------
void Esp8266Base::setFirmwareInfo(const char* name, const char* version) {
    if (name)    { strncpy(_fwName,    name,    sizeof(_fwName) - 1);    _fwName[sizeof(_fwName)-1] = '\0'; }
    if (version) { strncpy(_fwVersion, version, sizeof(_fwVersion) - 1); _fwVersion[sizeof(_fwVersion)-1] = '\0'; }
}

void Esp8266Base::setDeployEnvLabel(const char* label) {
#if ESP8266BASE_USE_WEB
    Esp8266BaseWeb::setDeployEnvLabel(label);
#else
    (void)label;
#endif
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

    // 4. Watchdog 先消费 RTC 故障胶囊，使随后创建的 boot 档案能关联
    // 上一次受控恢复原因。独立监护到首个 cycleStart() 才正式武装。
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::begin();
#endif

    // 4.5 Journal — 断线现场诊断档案（依赖已挂载的 LittleFS）
#if ESP8266BASE_USE_JOURNAL
    if (!Esp8266BaseJournal::begin()) {
        ok = false;  // 档案不可用不阻断业务
    }
#endif

    _resolveHostname();

    _bootCount = _loadAndIncrementBootCount();
#if ESP8266BASE_USE_CRASH
    Esp8266BaseCrash::begin(_bootCount);
#endif
#if ESP8266BASE_USE_JOURNAL
    Esp8266BaseJournal::beginBoot(_bootCount, static_cast<uint8_t>(resetReason()));
#if ESP8266BASE_USE_WATCHDOG
    if (Esp8266BaseWatchdog::lastRecoveryCause() == Esp8266BaseRecoveryCause::LOOP_STALL) {
        Esp8266BaseJournal::recordNow(
            JNL_STALL,
            static_cast<uint8_t>(Esp8266BaseWatchdog::lastStallPhase()),
            static_cast<int16_t>(ESP8266BASE_WDT_STALL_TIMEOUT_MS / 1000UL), 0, 0);
    }
#endif
#endif

    Esp8266BaseLog::beginBootSession(
        _fwName,
        _fwVersion,
#if ESP8266BASE_USE_SLEEP
        Esp8266BaseSleep::wakeReason(),
#else
        "unknown",
#endif
        _bootCount,
        ESP.getFreeHeap()
    );

    // 5. WiFi — 读取凭证，启动状态机（非阻塞）
    Esp8266BaseWiFi::begin();

    // 7. Web — 注册内置路由（OTA 路由由 OTA 模块在此后注册）
#if ESP8266BASE_USE_WEB
    Esp8266BaseWeb::setSystemInfo(_hostname, _fwName, _fwVersion, _bootCount);
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
#if ESP8266BASE_USE_WATCHDOG
    // 轮首开始测量本轮；子系统前的 feed 不再掩盖停滞（见 Esp8266BaseWatchdog）。
    Esp8266BaseWatchdog::cycleStart();
#endif
    // 1. Config deferred 刷新
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::CONFIG);
#endif
#if ESP8266BASE_USE_CONFIG
    Esp8266BaseConfig::handle();
#endif
#if ESP8266BASE_USE_FILELOG
    Esp8266BaseFileLog::handle();
#endif

    // 2. WiFi 状态机
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::WIFI);
#endif
    Esp8266BaseWiFi::handle();

    // 2.2 radio reset 分层消费：WiFi 层只置请求；主类在同一主循环上下文先有序
    // 关闭上层网络（Web/UDP/TCP socket），再物理切 WIFI_OFF，避免活跃 PCB 下
    // netif 拆除崩溃。必须在 NTP/mDNS/Web/MQTT 之前完成。
    _consumeRadioReset();

    // 2.5 Journal — 趋势采样与批下刷（每轮轻量检查）
#if ESP8266BASE_USE_JOURNAL
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::JOURNAL);
#endif
    Esp8266BaseJournal::handle();
#endif

    // 3. WiFi 连接后触发 NTP / mDNS；WiFi 掉线后重置 mDNS 标志以便重连后重启
    bool wifiNow = Esp8266BaseWiFi::isConnected();

#if ESP8266BASE_USE_WATCHDOG
    // 本地链路长期失联阶梯（只认 STA 本地状态；外网断但 WiFi 关联正常时不 arm）。
    _handleLocalLinkRecovery(wifiNow);
#else
    (void)wifiNow;
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
        _mdnsWasStarted = Esp8266BaseMDNS::begin(_hostname);
    } else if (_mdnsWasStarted && !wifiNow) {
        _mdnsWasStarted = false;  // WiFi 掉线，下次连上时重启 mDNS
    }
#endif

    // 4. NTP handle（双源采集秒级轮询；DNS 有界，给 8s 记账上限）
#if ESP8266BASE_USE_NTP
    if (_ntpWasTriggered) {
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::NTP);
        const uint32_t _wdtNtpMs = millis();
#endif
        Esp8266BaseNTP::handle();
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::account(8000, _wdtNtpMs);
#endif
    }
#endif

    // 5. mDNS handle（MDNS.update()）
#if ESP8266BASE_USE_MDNS
    if (_mdnsWasStarted) {
#if ESP8266BASE_USE_WATCHDOG
        Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::MDNS);
#endif
        Esp8266BaseMDNS::handle();
    }
#endif


    // 6. Web handle（server.handleClient()）
    // Web must run before MQTT: a secure DNS/TCP/TLS connect attempt can block
    // for seconds on ESP8266, while local control must remain responsive.
    // 慢客户端读/写有界，给 5s 记账上限；不 pause/resume（每轮调用会刷日志）。
#if ESP8266BASE_USE_WEB
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::WEB);
    const uint32_t _wdtWebMs = millis();
#endif
    Esp8266BaseWeb::handle();
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::account(5000, _wdtWebMs);
#endif
#endif

    // 7. OTA 准备租约：两阶段上传未及时开始时恢复 MQTT。
#if ESP8266BASE_USE_OTA
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::OTA);
#endif
    Esp8266BaseOTA::handle();
#endif

    // 8. MQTT handle：NTP 本轮推进后再检查时间门控。
    // 同步 DNS/TCP/TLS 建连与写超时上界 ~10s，给 15s 记账上限（见看门狗语义）。
#if ESP8266BASE_USE_MQTT
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::MQTT);
    const uint32_t _wdtMqttMs = millis();
#endif
    Esp8266BaseMQTT::handle();
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::account(15000, _wdtMqttMs);
#endif
#endif

    // 9. Watchdog handle — 轮末检查整轮耗时（基础超时 + 已声明宽限），
    // 超预算视为停滞并重启；正常轮内不触发。
#if ESP8266BASE_USE_WATCHDOG
    Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase::LOOP);
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
uint32_t Esp8266Base::bootCount()          { return _bootCount; }

Esp8266BaseResetReason Esp8266Base::resetReason() {
    const rst_info* info = ESP.getResetInfoPtr();
    if (!info) return Esp8266BaseResetReason::UNKNOWN;
    switch (info->reason) {
        case REASON_DEFAULT_RST:
        case REASON_EXT_SYS_RST:
            return Esp8266BaseResetReason::POWER_ON_OR_EXTERNAL_RESET;
        case REASON_SOFT_RESTART:
            return Esp8266BaseResetReason::SOFTWARE_RESTART;
        case REASON_WDT_RST:
        case REASON_SOFT_WDT_RST:
            return Esp8266BaseResetReason::WATCHDOG_RESET;
        case REASON_EXCEPTION_RST:
            return Esp8266BaseResetReason::EXCEPTION;
        case REASON_DEEP_SLEEP_AWAKE:
            return Esp8266BaseResetReason::DEEP_SLEEP_WAKE;
        default:
            return Esp8266BaseResetReason::UNKNOWN;
    }
}

const char* Esp8266Base::resetReasonName() {
    switch (resetReason()) {
        case Esp8266BaseResetReason::POWER_ON_OR_EXTERNAL_RESET:
            return "power-on-or-external-reset";
        case Esp8266BaseResetReason::SOFTWARE_RESTART:
            return "software-restart";
        case Esp8266BaseResetReason::WATCHDOG_RESET:
            return "watchdog-reset";
        case Esp8266BaseResetReason::EXCEPTION:
            return "exception";
        case Esp8266BaseResetReason::DEEP_SLEEP_WAKE:
            return "deep-sleep-wake";
        default:
            return "unknown";
    }
}

// ----------------------------------------------------------------------------
// radio reset 主类编排：WiFi 层只置请求，由这里在同一主循环上下文先有序关闭
// 上层网络，再执行唯一的物理 WIFI_OFF。
// ----------------------------------------------------------------------------
void Esp8266Base::_consumeRadioReset() {
    Esp8266BaseRadioResetSource source;
    if (!Esp8266BaseWiFi::consumeRadioResetRequest(source)) return;

    // radio reset 只在 CONNECTING（无 IP）时被请求：活动 MQTT TLS / OTA / 流式
    // HTTP 连接不可能存在。仍确定性关闭 Web listen PCB（stopAll 关不到它）与全部
    // UDP/TCP PCB，避免 WIFI_OFF→netif_remove 时活跃 PCB 触发 use-after-free 崩溃。
#if ESP8266BASE_USE_WEB
    Esp8266BaseWeb::suspendForRadioReset();
#endif
    WiFiUDP::stopAll();   // 关闭 NTP 等 WiFiUDP PCB（mDNS 用 Core 内部 UdpContext，
                          // 不在此链表，其 socket 由 Core 的 netif 状态回调自重启）
    WiFiClient::stopAll();// 关闭残留 accepted TCP 连接

    Esp8266BaseWiFi::executeRadioReset(source);

#if ESP8266BASE_USE_WEB
    // stop()/begin() 不删除已注册路由；恢复自定义 backlog=1 监听，不等关联完成，
    // 与开机时 Web 在联网前即监听的常态一致。
    Esp8266BaseWeb::resumeAfterRadioReset();
#endif
}

#if ESP8266BASE_USE_WATCHDOG
// ----------------------------------------------------------------------------
// 本地链路长期失联阶梯（替代旧 MQTT 传输失败阶梯）：
// 只认 STA 本地状态。路由器关机/纯关联失败→持续无 IP + 本地 radio reset 累计→
// 达 30min 且 ≥2 次 radio reset 才申请受控 MCU 重启（guard/cooldown/预算在
// Watchdog）。WiFi 关联正常但外网断→isConnected 恒真，永不 arm，不重启。
// ----------------------------------------------------------------------------
void Esp8266Base::_handleLocalLinkRecovery(bool wifiConnected) {
    if (wifiConnected) {
        _linkOutageActive = false;
        return;
    }
    // AP 配网态由 AP 提供本地服务，不属于失联。
    if (Esp8266BaseWiFi::state() == Esp8266BaseWiFiState::AP_CONFIG) return;

    const uint32_t now = millis();
    if (!_linkOutageActive) {
        _linkOutageActive = true;
        _linkOutageStartedAt = now;
        _linkOutageRadioBaseline = Esp8266BaseWiFi::radioResetCount();
        _linkRestartRetryAt = 0;
        return;
    }

    const uint32_t outageMs = now - _linkOutageStartedAt;
    const uint8_t radioResets =
        (uint8_t)(Esp8266BaseWiFi::radioResetCount() - _linkOutageRadioBaseline);
    // _linkRestartRetryAt==0 表示本窗口尚未申请过（arm 时置 0），应立即可申请；
    // 不能直接 (now-0)<0 判断，否则 millis 越过 2^31 后会长期抑制首次申请。
    if (outageMs < ESP8266BASE_LOCAL_LINK_MCU_RECOVERY_AFTER_MS ||
        radioResets < ESP8266BASE_LOCAL_LINK_MCU_RECOVERY_RADIO_RESETS ||
        (_linkRestartRetryAt != 0 && static_cast<int32_t>(now - _linkRestartRetryAt) < 0)) {
        return;
    }

    const Esp8266BaseRestartDecision decision =
        Esp8266BaseWatchdog::requestRestart(Esp8266BaseRecoveryCause::NETWORK_UNRECOVERABLE);
    // guard（如继电器运行中）/cooldown/预算拒绝时低率重试，保持可观测。
    _linkRestartRetryAt = now + 60000UL;
    if (decision != Esp8266BaseRestartDecision::RESTARTING) {
        ESP8266BASE_LOG_W_P("Net",
                          "local_link_mcu_recovery_deferred decision=%u outage_s=%lu radio_resets=%u",
                          static_cast<unsigned>(decision),
                          static_cast<unsigned long>(outageMs / 1000UL),
                          static_cast<unsigned>(radioResets));
    }
}
#endif
