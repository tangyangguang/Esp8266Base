#include "Esp8266BaseWiFi.h"
#if ESP8266BASE_USE_WIFI_CONFIG
#include "Esp8266BaseConfig.h"
#endif
#if ESP8266BASE_USE_JOURNAL
#include "Esp8266BaseJournal.h"
#endif
#include "Esp8266BaseLog.h"
#include <ESP8266WiFi.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
Esp8266BaseWiFiState Esp8266BaseWiFi::_state         = Esp8266BaseWiFiState::IDLE;
uint32_t             Esp8266BaseWiFi::_connectStart   = 0;
uint32_t             Esp8266BaseWiFi::_retryAt        = 0;
uint8_t              Esp8266BaseWiFi::_retryCount     = 0;
bool                 Esp8266BaseWiFi::_stuckRestarted = false;
Esp8266BaseLinkRecoveryCore::RadioResetGate Esp8266BaseWiFi::_radioGate;
uint32_t             Esp8266BaseWiFi::_connectedAtMs = 0;
uint16_t             Esp8266BaseWiFi::_attemptCount   = 0;
uint8_t              Esp8266BaseWiFi::_radioResetCount = 0;
bool                 Esp8266BaseWiFi::_radioResetRequest = false;
Esp8266BaseWiFi::StationAction Esp8266BaseWiFi::_stationAction =
    Esp8266BaseWiFi::StationAction::NONE;
char                 Esp8266BaseWiFi::_apSSID[28]     = "";
char                 Esp8266BaseWiFi::_ip[16]         = "";
char                 Esp8266BaseWiFi::_staSSID[64]    = "";
char                 Esp8266BaseWiFi::_staPass[64]    = "";
#if ESP8266BASE_WIFI_RECOVERY_BUTTON_ENABLED
Esp8266BaseRecoveryButtonCore::RecoveryButtonCore Esp8266BaseWiFi::_recoveryButton;
Esp8266BaseWiFi::RecoveryPortalCallback Esp8266BaseWiFi::_recoveryPortalCallback = nullptr;
#endif
bool                 Esp8266BaseWiFi::_recoveryPortalActive = false;

static void _formatIP(const IPAddress& ip, char* out, size_t len) {
    if (!out || len == 0) return;
    snprintf(out, len, "%u.%u.%u.%u",
             (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
}

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::begin() {
    // 生成 AP SSID，用 ChipID 后 4 位确保唯一性
    uint16_t suffix = (uint16_t)(ESP.getChipId() & 0xFFFF);
    snprintf(_apSSID, sizeof(_apSSID), "ESP8266-Config-%04X", suffix);

    // 关闭自动连接 / 自动重连（由状态机全权管理）
    WiFi.persistent(false);
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);
#if ESP8266BASE_WIFI_NO_SLEEP
    // 显式关闭 modem sleep（电台常开）：零省电，用于弱信号排障或低延迟场景。
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    ESP8266BASE_LOG_I_P("WiFi", "sleep_policy=none reason=explicit_no_sleep");
#else
    // 显式 modem-sleep MIN 级（setSleepMode 第二参 0 => 最小睡眠级）：
    // 每个 DTIM 醒一次收 beacon，省电与稳定性平衡，不依赖 SDK 隐性默认；
    // 不启用 MAX 级（按 listen interval 长睡会放大弱信号丢 beacon/deauth 风险）。
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);
    ESP8266BASE_LOG_I_P("WiFi", "sleep_policy=modem_sleep_min level=min_dtim_wake");
#endif
#if ESP8266BASE_WIFI_OUTPUT_POWER_X10 > 0
    // 弱信号现场可选：显式拉满发射功率（0.1dBm 量化），只影响上行，不改变缓冲/状态机。
    WiFi.setOutputPower(ESP8266BASE_WIFI_OUTPUT_POWER_X10 / 10.0f);
    ESP8266BASE_LOG_I_P("WiFi", "output_power_explicit_x10=%u",
                        (unsigned)ESP8266BASE_WIFI_OUTPUT_POWER_X10);
#endif
    ESP8266BASE_LOG_I_P("WiFi", "wifi_retry_policy connect_timeout=%lus sta_settle=%ums stuck_disconnected=%lus fast_retry=%lus fast_count=%u slow_retry=%lus radio_reset_failures=%u radio_cooldown=%lus radio_budget=%u radio_settle=%ums",
                      (unsigned long)(ESP8266BASE_WIFI_CONNECT_TIMEOUT / 1000UL),
                      (unsigned)ESP8266BASE_WIFI_STA_SETTLE_MS,
                      (unsigned long)(ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS / 1000UL),
                      (unsigned long)(ESP8266BASE_WIFI_RETRY_FAST / 1000UL),
                      (unsigned)ESP8266BASE_WIFI_RETRY_FAST_COUNT,
                      (unsigned long)(ESP8266BASE_WIFI_RETRY_SLOW / 1000UL),
                      (unsigned)Esp8266BaseLinkRecoveryCore::RADIO_RESET_FAILURE_THRESHOLD,
                      (unsigned long)(Esp8266BaseLinkRecoveryCore::RADIO_RESET_MIN_INTERVAL_MS / 1000UL),
                      (unsigned)Esp8266BaseLinkRecoveryCore::RADIO_RESET_WINDOW_BUDGET,
                      (unsigned)ESP8266BASE_WIFI_RADIO_RESET_SETTLE_MS);

    // 读取凭证并缓存，后续重连直接使用缓存，避免重复读 Flash
#if ESP8266BASE_USE_WIFI_CONFIG
    Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WIFI_SSID, _staSSID, sizeof(_staSSID), "");
#endif

    if (strlen(_staSSID) > 0) {
#if ESP8266BASE_USE_WIFI_CONFIG
        Esp8266BaseConfig::getStr(ESP8266BASE_CFG_KEY_WIFI_PASS, _staPass, sizeof(_staPass), "");
#endif
        ESP8266BASE_LOG_I_P("WiFi", "loaded_saved_wifi_credentials ssid=%s password=[redacted] password_length=%u",
                          _staSSID, (unsigned)strlen(_staPass));
        _startSTA(_staSSID, _staPass);
    } else {
        // 无凭证，直接进入 AP 配网
        ESP8266BASE_LOG_I_P("WiFi", "no_saved_wifi_credentials starting_config_ap ssid=%s", _apSSID);
        _startAP(false);
    }

#if ESP8266BASE_WIFI_RECOVERY_BUTTON_ENABLED
    // 仅在离开上电窗口后武装检测：按住上电/复位进 ROM 下载模式，不运行到此处，
    // 因而不会与运行期长按冲突。GPIO0 模块板载上拉，INPUT 即可读到高电平。
    pinMode(ESP8266BASE_WIFI_RECOVERY_BUTTON_GPIO, INPUT);
    _recoveryButton.reset(millis());
    ESP8266BASE_LOG_I_P("WiFi",
                      "recovery_button_config enabled=yes gpio=%d hold_ms=%lu debounce_ms=%lu credentials=preserved",
                      (int)ESP8266BASE_WIFI_RECOVERY_BUTTON_GPIO,
                      (unsigned long)ESP8266BASE_WIFI_RECOVERY_BUTTON_HOLD_MS,
                      (unsigned long)ESP8266BASE_WIFI_RECOVERY_BUTTON_DEBOUNCE_MS);
#endif

    return true;
}

// ----------------------------------------------------------------------------
// 恢复键轮询：只做数字读 + 消抖纯核；触发时仅登记动作，不动网
// ----------------------------------------------------------------------------
void Esp8266BaseWiFi::_pollRecoveryButton() {
#if ESP8266BASE_WIFI_RECOVERY_BUTTON_ENABLED
    // 门户中不检测：门户只能通过保存新凭证或重启退出，长按无效。
    if (_state == Esp8266BaseWiFiState::AP_CONFIG) return;
    const uint32_t now = millis();
    const bool rawPressed =
        digitalRead(ESP8266BASE_WIFI_RECOVERY_BUTTON_GPIO) == LOW;
    if (!_recoveryButton.update(rawPressed, now,
                                ESP8266BASE_WIFI_RECOVERY_BUTTON_DEBOUNCE_MS,
                                ESP8266BASE_WIFI_RECOVERY_BUTTON_HOLD_MS)) {
        return;
    }
    ESP8266BASE_LOG_W_P("WiFi",
                      "recovery_button_triggered gpio=%d hold_ms=%lu action=start_config_portal credentials=preserved",
                      (int)ESP8266BASE_WIFI_RECOVERY_BUTTON_GPIO,
                      (unsigned long)ESP8266BASE_WIFI_RECOVERY_BUTTON_HOLD_MS);
    _requestStationAction(StationAction::ENTER_RECOVERY_PORTAL);
#endif
}

// ----------------------------------------------------------------------------
// handle — 状态机推进（非阻塞）
// ----------------------------------------------------------------------------
void Esp8266BaseWiFi::handle() {
    // 恢复键优先于本轮状态机其他动作检测（人工显式操作优先级最高）。
    _pollRecoveryButton();

    switch (_state) {

        case Esp8266BaseWiFiState::CONNECTING: {
            uint32_t now = millis();

            if (_connectStart == 0) {
                if (_isDue(now, _retryAt)) {
                    // 退避后的重起：可能仍有上层 socket 残留，登记动作交主类先有序停网。
                    _requestStationAction(StationAction::RECONNECT);
                }
                break;
            }

            uint8_t status = (uint8_t)WiFi.status();
            if (status == WL_CONNECTED) {
                _handleConnected();
                break;
            }

            if (ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS > 0 &&
                status == WL_DISCONNECTED &&
                now - _connectStart >= ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS &&
                ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS < ESP8266BASE_WIFI_CONNECT_TIMEOUT) {
                if (_stuckRestarted) {
                    ESP8266BASE_LOG_W_P("WiFi",
                                      "station_connect_stuck_retrying ssid=%s status=%s status_code=%u elapsed=%lums restart_count=1 rssi=%d",
                                      _staSSID,
                                      _statusName(status),
                                      (unsigned)status,
                                      (unsigned long)(now - _connectStart),
                                      (int)WiFi.RSSI());
                    // 已重起过一次仍卡死：断开关联后走 fast/slow + radio reset 阶梯退避。
                    // 物理 disconnect 交主类在有序停网后执行，避免活 PCB UAF。
                    _requestStationAction(StationAction::DISCONNECT_BACKOFF);
                    break;
                }
                ESP8266BASE_LOG_W_P("WiFi",
                                  "station_connect_stuck_restarting ssid=%s status=%s status_code=%u elapsed=%lums restart_count=1 rssi=%d",
                                  _staSSID,
                                  _statusName(status),
                                  (unsigned)status,
                                  (unsigned long)(now - _connectStart),
                                  (int)WiFi.RSSI());
                // 首次卡死立即重起；不在这里 disconnect/_startSTA，登记动作由主类在
                // 有序停网后执行。_stuckRestarted 语义由该动作在物理执行时置位保留。
                _requestStationAction(StationAction::STUCK_RESTART);
                break;
            }

            if (now - _connectStart >= ESP8266BASE_WIFI_CONNECT_TIMEOUT) {
                ESP8266BASE_LOG_W_P("WiFi",
                                  "station_connect_timeout ssid=%s status=%s status_code=%u elapsed=%lums rssi=%d",
                                  _staSSID,
                                  _statusName(status),
                                  (unsigned)status,
                                  (unsigned long)(now - _connectStart),
                                  (int)WiFi.RSSI());
                // 物理 disconnect + 退避交主类在有序停网后执行。
                _requestStationAction(StationAction::DISCONNECT_BACKOFF);
            }
            break;
        }

        case Esp8266BaseWiFiState::CONNECTED: {
            uint8_t status = (uint8_t)WiFi.status();
            if (status != WL_CONNECTED) {
#if ESP8266BASE_USE_JOURNAL
                Esp8266BaseJournal::record(JNL_WIFI_LOST, static_cast<uint8_t>(status), 0, 0, 0);
#endif
                ESP8266BASE_LOG_W_P("WiFi",
                                  "station_connection_lost status=%s status_code=%u last_ip=%s rssi=%d reconnecting_with_saved_credentials",
                                  _statusName(status), (unsigned)status, _ip, (int)WiFi.RSSI());
                _ip[0]      = '\0';
                _retryCount   = 0;
                _stuckRestarted = false;
                _state        = Esp8266BaseWiFiState::CONNECTING;
                _connectStart = millis();
                _retryAt      = millis();   // 立即开始尝试
                // 已连接后被动掉线：先走一次“轻重连”——只清已接受的数据 socket，保留
                // STA netif 与 Web:80 监听，仅重发关联，弱信号抖动期间不主动砸局域网，
                // 也不触发 netif 拆除。若本轮 7s/20s 仍无进展，下面的 stuck/超时分支会
                // 照常升级为完整有序停网 + radio reset（SDK station 软卡死自愈不丢）。
                _connectedAtMs = 0;
                _requestStationAction(StationAction::REATTACH);
            }
            break;
        }

        case Esp8266BaseWiFiState::AP_CONFIG: {
            break;
        }

        case Esp8266BaseWiFiState::IDLE:
        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// connect — 保存新凭证并立即重连
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::connect(const char* ssid, const char* pass) {
    size_t ssidLen = ssid ? strlen(ssid) : 0;
    if (ssidLen == 0) {
        ESP8266BASE_LOG_W_P("WiFi", "connect_rejected reason=empty_ssid");
        return false;
    }
    if (ssidLen > 32) {
        ESP8266BASE_LOG_W_P("WiFi", "connect_rejected reason=ssid_too_long length=%u max=32",
                          (unsigned)ssidLen);
        return false;
    }

    const char* safePass = pass ? pass : "";
    size_t passLen = strlen(safePass);
    if (passLen > 63) {
        ESP8266BASE_LOG_W_P("WiFi", "connect_rejected reason=password_too_long length=%u max=63",
                          (unsigned)passLen);
        return false;
    }

#if ESP8266BASE_USE_WIFI_CONFIG
    bool ssidSaved = Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_SSID, ssid);
    bool passSaved = Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_PASS, safePass);
#else
    bool ssidSaved = true;
    bool passSaved = true;
#endif
    ESP8266BASE_LOG_I_P("WiFi", "saving_wifi_credentials ssid=%s password=[redacted] password_length=%u ssid_saved=%s password_saved=%s",
                      ssid, (unsigned)passLen,
                      ssidSaved ? "yes" : "no", passSaved ? "yes" : "no");
    if (!ssidSaved || !passSaved) {
        ESP8266BASE_LOG_E_P("WiFi", "connect_rejected reason=failed_to_save_credentials");
        return false;
    }

    // 更新内存缓存，后续重连无需再读 Flash
    strncpy(_staSSID, ssid, sizeof(_staSSID) - 1);
    _staSSID[sizeof(_staSSID) - 1] = '\0';
    strncpy(_staPass, safePass, sizeof(_staPass) - 1);
    _staPass[sizeof(_staPass) - 1] = '\0';

    // 若当前在 AP 模式（首启配网或恢复键门户），不能在 HTTP handler 内直接
    // softAPdisconnect/mode：此刻服务本请求的 accepted PCB 与 listen PCB 都挂在
    // AP netif 上，opmode 变更会让 SDK 同步摘除 netif（与 1.14.2 修复的 STA 侧
    // UAF 同原语族）。登记 EXIT_PORTAL，由主类下一轮先有序停网再物理切回 STA；
    // 本响应（303）在仍然存活的 AP 上先发完。
    if (_state == Esp8266BaseWiFiState::AP_CONFIG) {
        _state = Esp8266BaseWiFiState::CONNECTING;
        _connectStart = millis();
        _retryAt = millis();
        _stuckRestarted = false;
        _stationAction = StationAction::EXIT_PORTAL;
    } else {
        // STA 运行期更换凭证：可能有 MQTT/Web socket 存活，登记立即重连动作，由主类
        // 先有序停网再用新凭证重起。同步复位 attempt 计时，避免旧凭证在途 attempt 的
        // stuck/超时布臂与新 attempt 交错。
        _state = Esp8266BaseWiFiState::CONNECTING;
        _connectStart = millis();
        _retryAt = millis();
        _stuckRestarted = false;
        _requestStationAction(StationAction::RECONNECT);
    }

    _retryCount = 0;
    _radioGate.onNewCredentials();
    return true;
}

// ----------------------------------------------------------------------------
// clearCredentials
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::clearCredentials() {
#if ESP8266BASE_USE_WIFI_CONFIG
    Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_SSID, "");
    Esp8266BaseConfig::setStr(ESP8266BASE_CFG_KEY_WIFI_PASS, "");
#endif
    _staSSID[0] = '\0';
    _staPass[0] = '\0';
    ESP8266BASE_LOG_I_P("WiFi", "saved_wifi_credentials_cleared");
    return true;
}

// ----------------------------------------------------------------------------
// 查询接口
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::isConnected() {
    return (_state == Esp8266BaseWiFiState::CONNECTED)
           && (WiFi.status() == WL_CONNECTED);
}

const char* Esp8266BaseWiFi::ip() {
    return _ip;
}

const char* Esp8266BaseWiFi::ssid() {
    return _staSSID;
}

int Esp8266BaseWiFi::rssi() {
    return isConnected() ? (int)WiFi.RSSI() : 0;
}

void Esp8266BaseWiFi::macAddressTo(char* out, size_t len) {
    if (!out || len == 0) return;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void Esp8266BaseWiFi::macAddressHex(char* out, size_t len) {
    if (!out || len == 0) return;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(out, len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

Esp8266BaseWiFiState Esp8266BaseWiFi::state() {
    return _state;
}

uint32_t Esp8266BaseWiFi::connectedAtMs() {
    return _connectedAtMs;
}

uint16_t Esp8266BaseWiFi::attemptCount() {
    return _attemptCount;
}

uint8_t Esp8266BaseWiFi::radioResetCount() {
    return _radioResetCount;
}

// ----------------------------------------------------------------------------
// radio reset 分层请求（不在此处触碰电台，由 Esp8266Base 主类消费后有序停网）
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::requestRadioReset() {
    if (_staSSID[0] == '\0' || _state == Esp8266BaseWiFiState::AP_CONFIG) return false;
    // 仅保留最早的 pending 请求，避免重复；主类每轮只消费一次。
    _radioResetRequest = true;
    return true;
}

void Esp8266BaseWiFi::executeRadioReset() {
    const uint8_t status = (uint8_t)WiFi.status();
    ESP8266BASE_LOG_W_P("WiFi",
                      "station_radio_reset_begin window_resets=%u failures=%u total_attempts=%u status=%s status_code=%u",
                      (unsigned)(_radioGate.windowResets + 1U),
                      (unsigned)_radioGate.failures,
                      (unsigned)_attemptCount,
                      _statusName(status),
                      (unsigned)status);

    // 普通 disconnect + begin 仍可能保留 ESP8266 SDK 的卡滞 station 状态。
    // 只重置无线子系统，不重启 MCU、不触碰 Config，也不切换到配置 AP。
    // 调用方（Esp8266Base 主类）必须已在此前有序关闭 mDNS/Web/UDP/TCP socket，
    // 否则活跃 PCB 在 netif 摘除时会触发 use-after-free 崩溃。
    WiFi.disconnect(false, false);
    const bool offOk = WiFi.mode(WIFI_OFF);
    delay(ESP8266BASE_WIFI_RADIO_RESET_SETTLE_MS);
    const bool staOk = WiFi.mode(WIFI_STA);
    delay(ESP8266BASE_WIFI_STA_SETTLE_MS);

    if (_radioResetCount < 0xFFU) {
        _radioResetCount++;
    }
    _radioGate.onResetExecuted(millis());
#if ESP8266BASE_USE_JOURNAL
    // flags=0：来源唯一（本地状态机），该字节保留；p1 为累计 reset 次数。
    Esp8266BaseJournal::record(JNL_RADIO_RESET, 0,
                               static_cast<int16_t>(_radioResetCount), 0, 0);
#endif
    ESP8266BASE_LOG_W_P("WiFi",
                      "station_radio_reset_complete reset_count=%u off=%s sta=%s action=reconnect",
                      (unsigned)_radioResetCount,
                      offOk ? "ok" : "failed",
                      staOk ? "ok" : "failed");
    _ip[0] = '\0';
    _beginSTA(_staSSID, _staPass, false);
}

void Esp8266BaseWiFi::_requestStationAction(StationAction action) {
    // 同一主循环内理论上只会有一个来源命中（switch 互斥）；仍做幂等保护：普通动作
    // 先到不被覆盖。恢复键是人工显式运维操作，赋予最高优先级，可抢占本轮已登记的
    // 重连/退避动作（长按每 10s 才产生一次，抢占不会造成重连风暴）。
    if (_stationAction != StationAction::NONE &&
        action != StationAction::ENTER_RECOVERY_PORTAL) return;
    _stationAction = action;
}

Esp8266BaseWiFi::StationAction Esp8266BaseWiFi::consumeStationAction() {
    const StationAction action = _stationAction;
    _stationAction = StationAction::NONE;
    return action;
}

bool Esp8266BaseWiFi::consumeRadioResetRequest() {
    if (!_radioResetRequest) return false;
    // AP 配网态（首启/恢复键门户）绝不允许 radio reset：executeRadioReset()
    // 会 WIFI_OFF→STA 直接拆掉刚建的 AP。门户动作待消费的窗口同样丢弃（纵深
    // 防御：正常该窗口没有任何 requestRadioReset() 来源）。清掉陈旧请求，等
    // 退出 AP 后重新评估。
    if (_state == Esp8266BaseWiFiState::AP_CONFIG ||
        _stationAction == StationAction::ENTER_RECOVERY_PORTAL ||
        _stationAction == StationAction::EXIT_PORTAL) {
        _radioResetRequest = false;
        ESP8266BASE_LOG_W_P("WiFi", "station_radio_reset_request_discarded reason=ap_config_active");
        return false;
    }
    _radioResetRequest = false;
    return true;
}

void Esp8266BaseWiFi::executeStationAction(StationAction action) {
    // 仅由 Esp8266Base（friend）在有序停网后调用；此处上层 socket 已全部关闭。
    const uint8_t status = (uint8_t)WiFi.status();
    switch (action) {
    case StationAction::RECONNECT:
        ESP8266BASE_LOG_W_P("WiFi",
                          "station_reconnect_after_teardown total_attempts=%u state=%u status=%s status_code=%u rssi=%d",
                          (unsigned)_attemptCount,
                          (unsigned)static_cast<uint8_t>(_state),
                          _statusName(status), (unsigned)status, (int)WiFi.RSSI());
        // 保留电台与凭证，仅用缓存凭证重起 STA（内部 disconnect/mode 在停网保护下）。
        _startSTA(_staSSID, _staPass);
        break;
    case StationAction::REATTACH: {
        // 被动掉线轻量重连：调用方只清了已接受的数据 socket，netif 与监听 PCB 仍在。
        // 已在 Arduino core 3.1.2 核实：WiFi.begin()→enableSTA(true) 在已是 STA 时
        // 立即返回（不切 opmode），同配置仅 wifi_station_connect() 并重确保 DHCP 客户端
        // 在跑，不 wifi_station_disconnect，故无 netif_remove/UAF。安全性依赖该 core 行为。
        ESP8266BASE_LOG_W_P("WiFi",
                          "station_reattach_listener_preserved total_attempts=%u status=%s status_code=%u rssi=%d",
                          (unsigned)_attemptCount, _statusName(status), (unsigned)status,
                          (int)WiFi.RSSI());
        WiFi.begin(_staSSID, (_staPass[0] ? _staPass : nullptr));
        if (_attemptCount < 0xFFFFU) _attemptCount++;
        _connectStart    = millis();
        _retryAt         = millis();
        _stuckRestarted  = false;
        break;
    }
    case StationAction::STUCK_RESTART:
        ESP8266BASE_LOG_W_P("WiFi",
                          "station_stuck_restart_after_teardown total_attempts=%u status=%s status_code=%u rssi=%d",
                          (unsigned)_attemptCount, _statusName(status), (unsigned)status,
                          (int)WiFi.RSSI());
        _startSTA(_staSSID, _staPass);
        // 关键：保留“本轮 attempt 已做过一次 stuck restart”语义。物理执行下一轮才发生，
        // 若不在这里置位，下次再卡死会永远走立即重起（固定 7s 风暴），饿死 fast/slow
        // 退避与 radio reset 升级链。
        _stuckRestarted = true;
        break;
    case StationAction::DISCONNECT_BACKOFF:
        ESP8266BASE_LOG_W_P("WiFi",
                          "station_disconnect_backoff_after_teardown total_attempts=%u status=%s status_code=%u rssi=%d",
                          (unsigned)_attemptCount, _statusName(status), (unsigned)status,
                          (int)WiFi.RSSI());
        // 停网保护下断开当前关联，再按 fast/slow + radio reset 阶梯布退避臂。
        WiFi.disconnect(false, false);
        _scheduleRetry();
        break;
    case StationAction::ENTER_RECOVERY_PORTAL: {
        ESP8266BASE_LOG_W_P("WiFi",
                          "recovery_portal_enter_after_teardown total_attempts=%u status=%s status_code=%u credentials=preserved",
                          (unsigned)_attemptCount, _statusName(status), (unsigned)status);
        // 业务安全回调（如继电器强制断开）在 teardown 之后、动网之前执行。
#if ESP8266BASE_WIFI_RECOVERY_BUTTON_ENABLED
        if (_recoveryPortalCallback) _recoveryPortalCallback();
#endif
        // 清除可能与本动作同轮排队的 radio reset 请求，防止进门户后被立刻拆 AP。
        _radioResetRequest = false;
        _ip[0] = '\0';
        _startAP(true);
        break;
    }
    case StationAction::EXIT_PORTAL:
        ESP8266BASE_LOG_W_P("WiFi",
                          "recovery_portal_exit_after_teardown ssid=%s action=resume_sta credentials=preserved",
                          _staSSID);
        // 上层 PCB 已由主类关闭（含正在服务 /wifi POST 的连接），此处安全拆 AP netif。
        WiFi.softAPdisconnect(true);
        delay(100);
        _recoveryPortalActive = false;
        _startSTA(_staSSID, _staPass);
        break;
    case StationAction::NONE:
    default:
        break;
    }
}

void Esp8266BaseWiFi::setRecoveryPortalCallback(RecoveryPortalCallback callback) {
#if ESP8266BASE_WIFI_RECOVERY_BUTTON_ENABLED
    _recoveryPortalCallback = callback;
#else
    (void)callback;
#endif
}

bool Esp8266BaseWiFi::recoveryPortalActive() {
    return _recoveryPortalActive;
}

const char* Esp8266BaseWiFi::apSSID() {
    return _apSSID;
}

// ----------------------------------------------------------------------------
// 内部辅助
// ----------------------------------------------------------------------------
void Esp8266BaseWiFi::_startSTA(const char* ssid, const char* pass, bool keepAP) {
    if (keepAP) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.disconnect(false, false);
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, false);
    }
    delay(ESP8266BASE_WIFI_STA_SETTLE_MS);
    _beginSTA(ssid, pass, keepAP);
}

void Esp8266BaseWiFi::_beginSTA(const char* ssid, const char* pass, bool keepAP) {
    WiFi.begin(ssid, (pass && strlen(pass) > 0) ? pass : nullptr);
    if (_attemptCount < 0xFFFFU) {
        _attemptCount++;
    }
    if (!keepAP) {
        _state = Esp8266BaseWiFiState::CONNECTING;
    }
    _connectStart = millis();
    _retryAt      = millis();   // 立即开始计时
    _stuckRestarted = false;
    uint8_t status = (uint8_t)WiFi.status();
    ESP8266BASE_LOG_I_P("WiFi", "station_connecting ssid=%s password=[redacted] password_length=%u keep_config_ap=%s status=%s status_code=%u",
                      ssid, (unsigned)(pass ? strlen(pass) : 0),
                      keepAP ? "yes" : "no", _statusName(status), (unsigned)status);
}

void Esp8266BaseWiFi::_startAP(bool recoveryTriggered) {
    // Clean state before switching to AP. erase 默认 false 且 persistent(false)，
    // 只清 SDK 当前连接状态，不删除 LittleFS 中的 eb_wifi_* 凭证、不写 SDK 扇区。
    // 调用方必须已由主类关闭全部上层 socket（首启时本就不存在）。
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    delay(100);

    // Force channel 6 to ensure macOS/iOS can see the AP
    int channel = 6;
    bool hidden = false;

    WiFi.softAP(_apSSID, nullptr, channel, hidden);
    _state = Esp8266BaseWiFiState::AP_CONFIG;
    _connectStart = 0;
    _retryAt = 0;
    _retryCount = 0;
    _stuckRestarted = false;
    _stationAction = StationAction::NONE;
    _recoveryPortalActive = recoveryTriggered;
    char apIp[16];
    _formatIP(WiFi.softAPIP(), apIp, sizeof(apIp));
    if (recoveryTriggered) {
        ESP8266BASE_LOG_W_P("WiFi",
                          "recovery_portal_started ssid=%s ip=%s channel=%d credentials=preserved",
                          _apSSID, apIp, channel);
    } else {
        ESP8266BASE_LOG_I_P("WiFi", "config_ap_started ssid=%s ip=%s channel=%d",
                          _apSSID, apIp, channel);
    }
}

void Esp8266BaseWiFi::_handleConnected() {
    _updateIP();
    _state         = Esp8266BaseWiFiState::CONNECTED;
    if (_connectedAtMs == 0U) _connectedAtMs = millis();  // 仅稳定沿记一次，供上层建连稳定门
#if ESP8266BASE_USE_JOURNAL
    Esp8266BaseJournal::record(JNL_WIFI_UP, 0, static_cast<int16_t>(WiFi.RSSI()), 0, 0);
#endif
    _retryCount    = 0;
    _stuckRestarted = false;
    _radioGate.onLinkHealthy();
    char gateway[16];
    char dns[16];
    _formatIP(WiFi.gatewayIP(), gateway, sizeof(gateway));
    _formatIP(WiFi.dnsIP(), dns, sizeof(dns));
    ESP8266BASE_LOG_I_P("WiFi", "station_connected ip=%s gateway=%s dns=%s rssi=%d",
                      _ip, gateway, dns, (int)WiFi.RSSI());
}

void Esp8266BaseWiFi::_scheduleRetry() {
    if (_retryCount < 0xFF) {
        _retryCount++;
    }
    const uint32_t now = millis();
    // 资格判定必须在本函数内、且每次失败都重新评估。返回 true 才置 radio reset
    // 请求（主类本轮消费后 executeRadioReset 会重置计时）；返回 false（失败数不足/
    // 10min 冷却中/窗口预算用完）必须与普通慢重试走完全相同的布臂，不能提前 return，
    // 否则下一帧会落入 stuck 分支立即重关联，退化成无间隔重连。
    if (_radioGate.onFailure(now)) {
        // 此处必在 CONNECTING 且有凭证，requestRadioReset() 不会拒绝；返回值仅作防御。
        requestRadioReset();
        return;
    }
    uint32_t interval = (_retryCount <= ESP8266BASE_WIFI_RETRY_FAST_COUNT)
        ? ESP8266BASE_WIFI_RETRY_FAST
        : ESP8266BASE_WIFI_RETRY_SLOW;
    uint8_t status = (uint8_t)WiFi.status();
    ESP8266BASE_LOG_W_P("WiFi", "station_reconnect_scheduled attempt=%d retry_in=%lus mode=%s status=%s status_code=%u rssi=%d",
                      (int)_retryCount, (unsigned long)(interval / 1000),
                      (_retryCount <= ESP8266BASE_WIFI_RETRY_FAST_COUNT) ? "fast" : "slow",
                      _statusName(status), (unsigned)status, (int)WiFi.RSSI());
    _connectStart = 0;
    _retryAt      = now + interval;
    _stuckRestarted = false;
}

bool Esp8266BaseWiFi::_isDue(uint32_t now, uint32_t due) {
    return static_cast<int32_t>(now - due) >= 0;
}

void Esp8266BaseWiFi::_updateIP() {
    IPAddress addr = WiFi.localIP();
    snprintf(_ip, sizeof(_ip), "%d.%d.%d.%d",
             addr[0], addr[1], addr[2], addr[3]);
}

const char* Esp8266BaseWiFi::_statusName(uint8_t status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
        case WL_CONNECTED:       return "WL_CONNECTED";
        case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED:    return "WL_DISCONNECTED";
        default:                 return "WL_UNKNOWN";
    }
}
