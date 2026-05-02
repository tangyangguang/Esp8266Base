#include "Esp8266BaseWiFi.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include <ESP8266WiFi.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
Esp8266BaseWiFiState Esp8266BaseWiFi::_state         = Esp8266BaseWiFiState::IDLE;
uint32_t             Esp8266BaseWiFi::_connectStart   = 0;
uint32_t             Esp8266BaseWiFi::_retryAt        = 0;
uint8_t              Esp8266BaseWiFi::_retryCount     = 0;
bool                 Esp8266BaseWiFi::_everConnected  = false;
char                 Esp8266BaseWiFi::_apSSID[28]     = "";
char                 Esp8266BaseWiFi::_ip[16]         = "";
char                 Esp8266BaseWiFi::_staSSID[64]    = "";
char                 Esp8266BaseWiFi::_staPass[64]    = "";

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

    // 读取凭证并缓存，后续重连直接使用缓存，避免重复读 Flash
    Esp8266BaseConfig::getStr("wifi_ssid", _staSSID, sizeof(_staSSID), "");

    if (strlen(_staSSID) > 0) {
        Esp8266BaseConfig::getStr("wifi_pass", _staPass, sizeof(_staPass), "");
        _startSTA(_staSSID, _staPass);
    } else {
        // 无凭证，直接进入 AP 配网
        ESP8266BASE_LOG_I("WiFi", "No credentials, starting AP ssid=%s", _apSSID);
        _startAP();
    }

    return true;
}

// ----------------------------------------------------------------------------
// handle — 状态机推进（非阻塞）
// ----------------------------------------------------------------------------
void Esp8266BaseWiFi::handle() {
    switch (_state) {

        case Esp8266BaseWiFiState::CONNECTING: {
            if (WiFi.status() == WL_CONNECTED) {
                _updateIP();
                _state        = Esp8266BaseWiFiState::CONNECTED;
                _retryCount   = 0;
                _everConnected = true;
                ESP8266BASE_LOG_I("WiFi", "Connected ip=%s rssi=%d",
                                  _ip, (int)WiFi.RSSI());
                break;
            }

            uint32_t now = millis();

            // 等待 _retryAt（首次连接 / 掉线重试都走此路径）
            if (now < _retryAt) break;

            // 超时判断
            if (now - _connectStart >= ESP8266BASE_WIFI_CONNECT_TIMEOUT) {
                WiFi.disconnect(true);

                if (!_everConnected) {
                    // 首次启动连接失败 → 进入 AP 配网
                    ESP8266BASE_LOG_W("WiFi", "Connect timeout, starting AP ssid=%s", _apSSID);
                    _startAP();
                } else {
                    // 断线重连失败 → 继续慢速重试
                    _retryCount++;
                    uint32_t interval = (_retryCount == 1)
                        ? ESP8266BASE_WIFI_RETRY_FAST
                        : ESP8266BASE_WIFI_RETRY_SLOW;
                    ESP8266BASE_LOG_W("WiFi", "Reconnect fail #%d, retry in %lus",
                                      (int)_retryCount, (unsigned long)(interval / 1000));

                    // 重新尝试连接（使用缓存的凭证，不再读 Flash）
                    uint32_t t = millis();
                    _connectStart = t;
                    _retryAt      = t + interval;
                    _startSTA(_staSSID, _staPass);
                }
            }
            break;
        }

        case Esp8266BaseWiFiState::CONNECTED: {
            if (WiFi.status() != WL_CONNECTED) {
                ESP8266BASE_LOG_W("WiFi", "Connection lost, reconnecting...");
                _ip[0]      = '\0';
                _retryCount   = 0;
                _state        = Esp8266BaseWiFiState::CONNECTING;
                _connectStart = millis();
                _retryAt      = millis();   // 立即开始尝试
                _startSTA(_staSSID, _staPass);
            }
            break;
        }

        case Esp8266BaseWiFiState::AP_CONFIG:
        case Esp8266BaseWiFiState::IDLE:
        case Esp8266BaseWiFiState::FAILED:
        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// connect — 保存新凭证并立即重连
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::connect(const char* ssid, const char* pass) {
    if (!ssid || strlen(ssid) == 0) {
        ESP8266BASE_LOG_W("WiFi", "connect: empty ssid");
        return false;
    }

    Esp8266BaseConfig::setStr("wifi_ssid", ssid);
    Esp8266BaseConfig::setStr("wifi_pass", pass ? pass : "");

    // 更新内存缓存，后续重连无需再读 Flash
    strncpy(_staSSID, ssid, sizeof(_staSSID) - 1);
    _staSSID[sizeof(_staSSID) - 1] = '\0';
    strncpy(_staPass, pass ? pass : "", sizeof(_staPass) - 1);
    _staPass[sizeof(_staPass) - 1] = '\0';

    // 若当前在 AP 模式，先关闭 AP
    if (_state == Esp8266BaseWiFiState::AP_CONFIG) {
        WiFi.softAPdisconnect(true);
        delay(100);
    }

    _startSTA(_staSSID, _staPass);
    return true;
}

// ----------------------------------------------------------------------------
// clearCredentials
// ----------------------------------------------------------------------------
bool Esp8266BaseWiFi::clearCredentials() {
    Esp8266BaseConfig::setStr("wifi_ssid", "");
    Esp8266BaseConfig::setStr("wifi_pass", "");
    ESP8266BASE_LOG_I("WiFi", "Credentials cleared");
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

Esp8266BaseWiFiState Esp8266BaseWiFi::state() {
    return _state;
}

const char* Esp8266BaseWiFi::apSSID() {
    return _apSSID;
}

// ----------------------------------------------------------------------------
// 内部辅助
// ----------------------------------------------------------------------------
void Esp8266BaseWiFi::_startSTA(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, (pass && strlen(pass) > 0) ? pass : nullptr);
    _state        = Esp8266BaseWiFiState::CONNECTING;
    _connectStart = millis();
    _retryAt      = millis();   // 立即开始计时
    ESP8266BASE_LOG_I("WiFi", "STA connecting ssid=%s", ssid);
}

void Esp8266BaseWiFi::_startAP() {
    char apPass[32] = "";
    Esp8266BaseConfig::getStr("ap_pass", apPass, sizeof(apPass), "");

    WiFi.mode(WIFI_AP);
    if (strlen(apPass) > 0) {
        WiFi.softAP(_apSSID, apPass);
    } else {
        WiFi.softAP(_apSSID);
    }
    _state = Esp8266BaseWiFiState::AP_CONFIG;
    ESP8266BASE_LOG_I("WiFi", "AP started ssid=%s ip=192.168.4.1", _apSSID);
}

void Esp8266BaseWiFi::_updateIP() {
    IPAddress addr = WiFi.localIP();
    snprintf(_ip, sizeof(_ip), "%d.%d.%d.%d",
             addr[0], addr[1], addr[2], addr[3]);
}
