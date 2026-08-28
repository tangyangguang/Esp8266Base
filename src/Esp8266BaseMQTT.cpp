#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_MQTT

#include "Esp8266BaseMQTT.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseNTP.h"
#include "Esp8266BaseWiFi.h"
#include <espMqttClient.h>

namespace Esp8266BaseMQTTInternal {

// 仅在基础库实现内暴露 espMqttClient 的 protected transport/state。TLS 错误
// 必须在 disconnectingTcp1 调用 transport.stop() 释放 BearSSL 前读取。
class DiagnosticSecureClient : public espMqttClientSecure {
public:
    DiagnosticSecureClient() : _lastTlsCode(0) { _lastTlsText[0] = '\0'; }

    bool connect() {
        clearTlsError();
        return MqttClient::connect();
    }

    void loop() {
        if (_state == State::disconnectingTcp1) captureTlsError();
        MqttClient::loop();
        if (_state == State::disconnectingTcp1) captureTlsError();
    }

    void setClientErrorCallback(espMqttClientTypes::OnErrorCallback callback) {
        _onErrorCallback = callback;
    }

    int lastTlsErrorCode() const { return _lastTlsCode; }
    const char* lastTlsErrorText() const { return _lastTlsText; }

private:
    int _lastTlsCode;
    char _lastTlsText[96];

    void clearTlsError() {
        _lastTlsCode = 0;
        _lastTlsText[0] = '\0';
    }

    void captureTlsError() {
        char detail[sizeof(_lastTlsText)] = "";
        int code = _client.client.getLastSSLError(detail, sizeof(detail));
        if (code == 0) return;
        _lastTlsCode = code;
        strncpy(_lastTlsText, detail, sizeof(_lastTlsText) - 1);
        _lastTlsText[sizeof(_lastTlsText) - 1] = '\0';
    }
};

}  // namespace Esp8266BaseMQTTInternal

static Esp8266BaseMQTTInternal::DiagnosticSecureClient mqttClient;

bool Esp8266BaseMQTT::_configured = false;
bool Esp8266BaseMQTT::_begun = false;
bool Esp8266BaseMQTT::_shutdownActive = false;
bool Esp8266BaseMQTT::_reconnectRequested = false;
bool Esp8266BaseMQTT::_connectAttemptsEnabled = true;
Esp8266BaseMQTTState Esp8266BaseMQTT::_state = Esp8266BaseMQTTState::UNCONFIGURED;
Esp8266BaseMQTTDisconnectReason Esp8266BaseMQTT::_lastReason = Esp8266BaseMQTTDisconnectReason::NONE;
uint32_t Esp8266BaseMQTT::_attemptCount = 0;
uint32_t Esp8266BaseMQTT::_retryAt = 0;
uint32_t Esp8266BaseMQTT::_retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;
Esp8266BaseMQTTShutdownResult Esp8266BaseMQTT::_shutdownResult = Esp8266BaseMQTTShutdownResult::NONE;
uint16_t Esp8266BaseMQTT::_shutdownPacketId = 0;
uint32_t Esp8266BaseMQTT::_shutdownDeadline = 0;
uint32_t Esp8266BaseMQTT::_shutdownTimeout = ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS;
char Esp8266BaseMQTT::_host[65] = "";
char Esp8266BaseMQTT::_clientId[49] = "";
char Esp8266BaseMQTT::_username[65] = "";
char Esp8266BaseMQTT::_password[97] = "";
char Esp8266BaseMQTT::_willTopic[129] = "";
const uint8_t* Esp8266BaseMQTT::_willPayload = nullptr;
uint16_t Esp8266BaseMQTT::_port = 0;
uint16_t Esp8266BaseMQTT::_keepAlive = 30;
bool Esp8266BaseMQTT::_cleanSession = true;
size_t Esp8266BaseMQTT::_willLength = 0;
uint8_t Esp8266BaseMQTT::_willQos = 0;
bool Esp8266BaseMQTT::_willRetain = false;
const BearSSL::X509List* Esp8266BaseMQTT::_trustAnchors = nullptr;
Esp8266BaseMQTTConnectedCallback Esp8266BaseMQTT::_connectedCallback = nullptr;
Esp8266BaseMQTTDisconnectedCallback Esp8266BaseMQTT::_disconnectedCallback = nullptr;
Esp8266BaseMQTTMessageCallback Esp8266BaseMQTT::_messageCallback = nullptr;
Esp8266BaseMQTTSubscribeAckCallback Esp8266BaseMQTT::_subscribeAckCallback = nullptr;
Esp8266BaseMQTTPublishAckCallback Esp8266BaseMQTT::_publishAckCallback = nullptr;
Esp8266BaseMQTTClientErrorCallback Esp8266BaseMQTT::_clientErrorCallback = nullptr;

static bool copyText(const char* source, char* target, size_t capacity, bool required) {
    if (!target || capacity == 0) return false;
    target[0] = '\0';
    if (!source || source[0] == '\0') return !required;
    size_t length = strlen(source);
    if (length >= capacity) return false;
    memcpy(target, source, length + 1);
    return true;
}

static Esp8266BaseMQTTDisconnectReason mapReason(espMqttClientTypes::DisconnectReason reason) {
    switch (reason) {
        case espMqttClientTypes::DisconnectReason::USER_OK: return Esp8266BaseMQTTDisconnectReason::USER_OK;
        case espMqttClientTypes::DisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION: return Esp8266BaseMQTTDisconnectReason::UNACCEPTABLE_PROTOCOL;
        case espMqttClientTypes::DisconnectReason::MQTT_IDENTIFIER_REJECTED: return Esp8266BaseMQTTDisconnectReason::IDENTIFIER_REJECTED;
        case espMqttClientTypes::DisconnectReason::MQTT_SERVER_UNAVAILABLE: return Esp8266BaseMQTTDisconnectReason::SERVER_UNAVAILABLE;
        case espMqttClientTypes::DisconnectReason::MQTT_MALFORMED_CREDENTIALS: return Esp8266BaseMQTTDisconnectReason::MALFORMED_CREDENTIALS;
        case espMqttClientTypes::DisconnectReason::MQTT_NOT_AUTHORIZED: return Esp8266BaseMQTTDisconnectReason::NOT_AUTHORIZED;
        case espMqttClientTypes::DisconnectReason::TLS_BAD_FINGERPRINT: return Esp8266BaseMQTTDisconnectReason::TLS_BAD_FINGERPRINT;
        case espMqttClientTypes::DisconnectReason::TCP_DISCONNECTED: return Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED;
        default: return Esp8266BaseMQTTDisconnectReason::UNKNOWN;
    }
}

bool Esp8266BaseMQTT::configure(const Esp8266BaseMQTTConfig& config) {
    if (_begun) {
        ESP8266BASE_LOG_E("MQTT", "configure_rejected reason=already_begun");
        return false;
    }
    bool stringsOk = copyText(config.host, _host, sizeof(_host), true) &&
                     copyText(config.clientId, _clientId, sizeof(_clientId), true) &&
                     copyText(config.username, _username, sizeof(_username), false) &&
                     copyText(config.password, _password, sizeof(_password), false) &&
                     copyText(config.willTopic, _willTopic, sizeof(_willTopic), false);
    bool credentialsOk = config.password == nullptr || config.password[0] == '\0' ||
                         (config.username != nullptr && config.username[0] != '\0');
    bool willOk = config.willPayloadLength <= 65535U &&
                  (config.willPayloadLength == 0 || config.willPayload != nullptr) &&
                  (config.willPayloadLength == 0 ||
                   (config.willTopic != nullptr && config.willTopic[0] != '\0')) &&
                  config.willQos <= 2;
    bool requiredOk = config.port != 0 && config.keepAliveSeconds != 0 &&
                      config.trustAnchors != nullptr;
    if (!stringsOk || !credentialsOk || !willOk || !requiredOk) {
        _configured = false;
        _state = Esp8266BaseMQTTState::UNCONFIGURED;
        ESP8266BASE_LOG_E("MQTT", "configure_rejected reason=invalid_or_missing_config host=%s port=%u client_id_length=%u trust_anchor=%s credentials=%s lwt=%s",
                          _host[0] ? "set" : "missing", (unsigned)config.port,
                          (unsigned)strlen(_clientId), config.trustAnchors ? "set" : "missing",
                          credentialsOk ? "valid" : "password_without_username",
                          willOk ? "valid" : "invalid");
        return false;
    }
    _willPayload = config.willPayload;
    _willLength = config.willPayloadLength;
    _willQos = config.willQos;
    _willRetain = config.willRetain;
    _port = config.port;
    _keepAlive = config.keepAliveSeconds;
    _cleanSession = config.cleanSession;
    _trustAnchors = config.trustAnchors;
    _configured = true;
    _state = Esp8266BaseMQTTState::WAITING_WIFI;
    ESP8266BASE_LOG_I("MQTT", "configured host=%s port=%u client_id=%s keepalive=%us clean_session=%s lwt=%s tls=trust_anchor",
                      _host, (unsigned)_port, _clientId, (unsigned)_keepAlive,
                      _cleanSession ? "yes" : "no", _willTopic[0] ? "yes" : "no");
    return true;
}

void Esp8266BaseMQTT::setCallbacks(Esp8266BaseMQTTConnectedCallback connected,
                                    Esp8266BaseMQTTDisconnectedCallback disconnected,
                                    Esp8266BaseMQTTMessageCallback message,
                                    Esp8266BaseMQTTSubscribeAckCallback subscribeAck,
                                    Esp8266BaseMQTTPublishAckCallback publishAck,
                                    Esp8266BaseMQTTClientErrorCallback clientError) {
    _connectedCallback = connected;
    _disconnectedCallback = disconnected;
    _messageCallback = message;
    _subscribeAckCallback = subscribeAck;
    _publishAckCallback = publishAck;
    _clientErrorCallback = clientError;
}

bool Esp8266BaseMQTT::begin() {
    _begun = true;
    _retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;
    if (!_configured) {
        _state = Esp8266BaseMQTTState::UNCONFIGURED;
        ESP8266BASE_LOG_E("MQTT", "mqtt_start_failed reason=missing_required_config action=configure_before_base_begin");
        return false;
    }
    mqttClient.setTrustAnchors(_trustAnchors)
              .setBufferSizes(4096, 1024)
              .setServer(_host, _port)
              .setClientId(_clientId)
              .setCleanSession(_cleanSession)
              .setKeepAlive(_keepAlive)
              .setTimeout(10)
              .onConnect(_onConnect)
              .onDisconnect([](espMqttClientTypes::DisconnectReason reason) {
                  Esp8266BaseMQTT::_onDisconnect((uint8_t)reason);
              })
              .onSubscribe([](uint16_t packetId,
                              const espMqttClientTypes::SubscribeReturncode* returnCodes,
                              size_t length) {
                  Esp8266BaseMQTT::_onSubscribeAck(
                      packetId, reinterpret_cast<const uint8_t*>(returnCodes), length);
              })
              .onPublish([](uint16_t packetId) {
                  Esp8266BaseMQTT::_onPublishAck(packetId);
              })
              .onMessage([](const espMqttClientTypes::MessageProperties& properties,
                            const char* topic, const uint8_t* payload,
                            size_t len, size_t index, size_t total) {
                  Esp8266BaseMQTT::_onMessage(properties.qos, properties.dup, properties.retain,
                                              properties.packetId, topic, payload, len, index, total);
              });
    mqttClient.setClientErrorCallback([](uint16_t packetId, espMqttClientTypes::Error error) {
        Esp8266BaseMQTT::_onClientError(packetId, (uint8_t)error);
    });
    if (_username[0]) mqttClient.setCredentials(_username, _password);
    if (_willTopic[0]) mqttClient.setWill(_willTopic, _willQos, _willRetain, _willPayload, _willLength);
    _retryAt = millis();
    ESP8266BASE_LOG_I("MQTT", "mqtt_transport_ready library=espMqttClient version=1.7.3 tls_buffers=4096/1024 mqtt_rx_buffer=library_default mqtt_tx_buffer=library_default");
    return true;
}

void Esp8266BaseMQTT::handle() {
    if (!_begun || !_configured) return;
    if (_shutdownActive) {
        _handleShutdown();
        return;
    }
    if (!Esp8266BaseWiFi::isConnected()) {
        _disconnectForGate(Esp8266BaseMQTTState::WAITING_WIFI);
        return;
    }
    if (!Esp8266BaseNTP::isSynced()) {
        _disconnectForGate(Esp8266BaseMQTTState::WAITING_TIME);
        return;
    }

    if (_reconnectRequested) {
        _reconnectRequested = false;
        ESP8266BASE_LOG_W("MQTT", "application_reconnect_begin action=disconnect_then_backoff");
        if (!mqttClient.disconnected()) {
            mqttClient.disconnect(true);
            mqttClient.loop();
            if (mqttClient.disconnected() && _state != Esp8266BaseMQTTState::BACKOFF) {
                _scheduleRetry();
            }
        } else {
            _scheduleRetry();
        }
        return;
    }

    mqttClient.loop();
    if (mqttClient.connected()) {
        _state = Esp8266BaseMQTTState::CONNECTED;
        return;
    }
    if (!mqttClient.disconnected()) {
        _state = Esp8266BaseMQTTState::CONNECTING;
        return;
    }
    if (!_connectAttemptsEnabled) {
        _state = Esp8266BaseMQTTState::BACKOFF;
        return;
    }

    uint32_t now = millis();
    if (!_isDue(now, _retryAt)) {
        _state = Esp8266BaseMQTTState::BACKOFF;
        return;
    }
    if (_attemptCount < 0xFFFFFFFFUL) _attemptCount++;
    ESP8266BASE_LOG_I("MQTT", "connect_attempt attempt=%lu host=%s port=%u free_heap=%u max_block=%u",
                      (unsigned long)_attemptCount, _host, (unsigned)_port,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    if (mqttClient.connect()) {
        _state = Esp8266BaseMQTTState::CONNECTING;
    } else {
        ESP8266BASE_LOG_E("MQTT", "connect_queue_failed attempt=%lu", (unsigned long)_attemptCount);
        _scheduleRetry();
    }
}

bool Esp8266BaseMQTT::_isDue(uint32_t now, uint32_t due) {
    return (int32_t)(now - due) >= 0;
}

uint16_t Esp8266BaseMQTT::publish(const char* topic, uint8_t qos, bool retain,
                                  const uint8_t* payload, size_t length) {
    if (_shutdownActive || !mqttClient.connected() || !topic ||
        (length > 0 && !payload) || qos > 2) return 0;
    return mqttClient.publish(topic, qos, retain, payload, length);
}

uint16_t Esp8266BaseMQTT::publish(const char* topic, uint8_t qos, bool retain,
                                  const char* payload) {
    if (!payload) return 0;
    return publish(topic, qos, retain, reinterpret_cast<const uint8_t*>(payload), strlen(payload));
}

uint16_t Esp8266BaseMQTT::subscribe(const char* topic, uint8_t qos) {
    if (_shutdownActive || !mqttClient.connected() || !topic || qos > 2) return 0;
    return mqttClient.subscribe(topic, qos);
}

bool Esp8266BaseMQTT::requestReconnect() {
    if (!_configured || !_begun || _shutdownActive) return false;
    if (!_reconnectRequested) {
        _reconnectRequested = true;
        ESP8266BASE_LOG_W("MQTT", "application_reconnect_requested deferred=yes");
    }
    return true;
}

bool Esp8266BaseMQTT::markConnectionReady() {
    if (!_configured || !_begun || _shutdownActive || _reconnectRequested ||
        !mqttClient.connected()) {
        return false;
    }
    _retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;
    ESP8266BASE_LOG_I("MQTT", "application_connection_ready retry_backoff=initial");
    return true;
}

void Esp8266BaseMQTT::setConnectAttemptsEnabled(bool enabled) {
    if (_connectAttemptsEnabled == enabled) return;
    _connectAttemptsEnabled = enabled;
    ESP8266BASE_LOG_I("MQTT", "connect_attempts_enabled=%s existing_connection=%s",
                      enabled ? "yes" : "no", mqttClient.connected() ? "kept" : "none");
}

bool Esp8266BaseMQTT::connectAttemptsEnabled() {
    return _connectAttemptsEnabled;
}

bool Esp8266BaseMQTT::connected() { return !_shutdownActive && mqttClient.connected(); }
bool Esp8266BaseMQTT::isConfigured() { return _configured; }
Esp8266BaseMQTTState Esp8266BaseMQTT::state() { return _state; }
uint32_t Esp8266BaseMQTT::attemptCount() { return _attemptCount; }
uint32_t Esp8266BaseMQTT::nextAttemptAt() { return _retryAt; }
Esp8266BaseMQTTDisconnectReason Esp8266BaseMQTT::lastDisconnectReason() { return _lastReason; }
int Esp8266BaseMQTT::lastTlsErrorCode() { return mqttClient.lastTlsErrorCode(); }
const char* Esp8266BaseMQTT::lastTlsErrorText() { return mqttClient.lastTlsErrorText(); }

const char* Esp8266BaseMQTT::stateName() {
    switch (_state) {
        case Esp8266BaseMQTTState::UNCONFIGURED: return "unconfigured";
        case Esp8266BaseMQTTState::WAITING_WIFI: return "waiting_wifi";
        case Esp8266BaseMQTTState::WAITING_TIME: return "waiting_time";
        case Esp8266BaseMQTTState::BACKOFF: return "backoff";
        case Esp8266BaseMQTTState::CONNECTING: return "connecting";
        case Esp8266BaseMQTTState::CONNECTED: return "connected";
        case Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK: return "shutdown_wait_ack";
        case Esp8266BaseMQTTState::SHUTDOWN_DISCONNECTING: return "shutdown_disconnecting";
        case Esp8266BaseMQTTState::PAUSED: return "paused";
        default: return "unknown";
    }
}

const char* Esp8266BaseMQTT::lastDisconnectReasonName() {
    switch (_lastReason) {
        case Esp8266BaseMQTTDisconnectReason::NONE: return "none";
        case Esp8266BaseMQTTDisconnectReason::USER_OK: return "user_ok";
        case Esp8266BaseMQTTDisconnectReason::UNACCEPTABLE_PROTOCOL: return "unacceptable_protocol";
        case Esp8266BaseMQTTDisconnectReason::IDENTIFIER_REJECTED: return "identifier_rejected";
        case Esp8266BaseMQTTDisconnectReason::SERVER_UNAVAILABLE: return "server_unavailable";
        case Esp8266BaseMQTTDisconnectReason::MALFORMED_CREDENTIALS: return "malformed_credentials";
        case Esp8266BaseMQTTDisconnectReason::NOT_AUTHORIZED: return "not_authorized";
        case Esp8266BaseMQTTDisconnectReason::TLS_BAD_FINGERPRINT: return "tls_bad_fingerprint";
        case Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED: return "tcp_disconnected";
        default: return "unknown";
    }
}

bool Esp8266BaseMQTT::beginShutdown(const char* topic, const uint8_t* payload,
                                    size_t length, uint32_t timeoutMs) {
    if (_shutdownActive) return false;
    if (!_configured || !_begun) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::NOT_CONNECTED;
        ESP8266BASE_LOG_E("MQTT", "shutdown_rejected result=%u", (unsigned)_shutdownResult);
        return false;
    }
    if (!topic || topic[0] == '\0' || (length > 0 && !payload) ||
        timeoutMs == 0 || timeoutMs > 0x7FFFFFFFUL) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::INVALID_ARGUMENT;
        ESP8266BASE_LOG_E("MQTT", "shutdown_rejected result=%u", (unsigned)_shutdownResult);
        return false;
    }

    _reconnectRequested = false;
    _shutdownActive = true;
    _shutdownTimeout = timeoutMs;
    if (!mqttClient.connected()) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::NOT_CONNECTED;
        _state = Esp8266BaseMQTTState::PAUSED;
        ESP8266BASE_LOG_E("MQTT", "shutdown_finished result=%u transport=unavailable reconnect=no",
                          (unsigned)_shutdownResult);
        return false;
    }
    _shutdownResult = Esp8266BaseMQTTShutdownResult::IN_PROGRESS;
    _shutdownDeadline = millis() + timeoutMs;
    _shutdownPacketId = mqttClient.publish(topic, 1, true, payload, length);
    if (_shutdownPacketId == 0) {
        _finishShutdown(Esp8266BaseMQTTShutdownResult::PUBLISH_FAILED);
        return false;
    }
    _state = Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK;
    ESP8266BASE_LOG_I("MQTT", "shutdown_started packet=%u timeout_ms=%lu",
                      (unsigned)_shutdownPacketId, (unsigned long)timeoutMs);
    return true;
}

bool Esp8266BaseMQTT::beginShutdown(const char* topic, const char* payload,
                                    uint32_t timeoutMs) {
    if (!payload) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::INVALID_ARGUMENT;
        return false;
    }
    return beginShutdown(topic, reinterpret_cast<const uint8_t*>(payload), strlen(payload), timeoutMs);
}

Esp8266BaseMQTTShutdownResult Esp8266BaseMQTT::shutdownResult() { return _shutdownResult; }

const char* Esp8266BaseMQTT::shutdownResultName() {
    switch (_shutdownResult) {
        case Esp8266BaseMQTTShutdownResult::NONE: return "none";
        case Esp8266BaseMQTTShutdownResult::IN_PROGRESS: return "in_progress";
        case Esp8266BaseMQTTShutdownResult::SUCCESS: return "success";
        case Esp8266BaseMQTTShutdownResult::NOT_CONNECTED: return "not_connected";
        case Esp8266BaseMQTTShutdownResult::INVALID_ARGUMENT: return "invalid_argument";
        case Esp8266BaseMQTTShutdownResult::PUBLISH_FAILED: return "publish_failed";
        case Esp8266BaseMQTTShutdownResult::CONNECTION_LOST: return "connection_lost";
        case Esp8266BaseMQTTShutdownResult::PUBACK_TIMEOUT: return "puback_timeout";
        case Esp8266BaseMQTTShutdownResult::DISCONNECT_FAILED: return "disconnect_failed";
        case Esp8266BaseMQTTShutdownResult::DISCONNECT_TIMEOUT: return "disconnect_timeout";
        default: return "unknown";
    }
}

bool Esp8266BaseMQTT::shutdownSucceeded() {
    return _shutdownActive && _state == Esp8266BaseMQTTState::PAUSED &&
           _shutdownResult == Esp8266BaseMQTTShutdownResult::SUCCESS;
}

bool Esp8266BaseMQTT::shutdownPaused() {
    return _shutdownActive;
}

void Esp8266BaseMQTT::_startGracefulDisconnect() {
    _state = Esp8266BaseMQTTState::SHUTDOWN_DISCONNECTING;
    _shutdownDeadline = millis() + _shutdownTimeout;
    if (mqttClient.disconnected()) {
        _finishShutdown(_shutdownResult == Esp8266BaseMQTTShutdownResult::IN_PROGRESS
                            ? Esp8266BaseMQTTShutdownResult::CONNECTION_LOST
                            : _shutdownResult);
        return;
    }
    if (!mqttClient.disconnect(false)) {
        _finishShutdown(Esp8266BaseMQTTShutdownResult::DISCONNECT_FAILED);
    }
}

void Esp8266BaseMQTT::_finishShutdown(Esp8266BaseMQTTShutdownResult result) {
    _shutdownResult = result;
    _shutdownPacketId = 0;
    _state = Esp8266BaseMQTTState::PAUSED;
    ESP8266BASE_LOG_I("MQTT", "shutdown_finished result=%u transport=%s reconnect=no",
                      (unsigned)_shutdownResult,
                      mqttClient.disconnected() ? "released" : "kept");
}

void Esp8266BaseMQTT::_handleShutdown() {
    if (_state == Esp8266BaseMQTTState::PAUSED) {
        // 超时结果已经对调用方终结，但若底层仍在释放 TLS，继续低扰动推进。
        if (!mqttClient.disconnected()) mqttClient.loop();
        return;
    }
    mqttClient.loop();
    if (_state == Esp8266BaseMQTTState::PAUSED) return;

    if (_state == Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK &&
        _isDue(millis(), _shutdownDeadline)) {
        // 上游没有按 packetId 删除 API；失败恢复前清空 outbox，避免未确认的
        // 最终消息在 resume 后迟到并改写 retained availability。
        mqttClient.clearQueue(true);
        _finishShutdown(Esp8266BaseMQTTShutdownResult::PUBACK_TIMEOUT);
        return;
    }
    if (_state == Esp8266BaseMQTTState::SHUTDOWN_DISCONNECTING) {
        if (mqttClient.disconnected()) {
            _finishShutdown(_shutdownResult == Esp8266BaseMQTTShutdownResult::IN_PROGRESS
                                ? Esp8266BaseMQTTShutdownResult::CONNECTION_LOST
                                : _shutdownResult);
        } else if (_isDue(millis(), _shutdownDeadline)) {
            _finishShutdown(Esp8266BaseMQTTShutdownResult::DISCONNECT_TIMEOUT);
        }
    }
}

void Esp8266BaseMQTT::resumeAfterShutdown() {
    if (!_shutdownActive) return;
    _shutdownActive = false;
    _shutdownPacketId = 0;
    _retryAt = millis();
    _retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;
    _state = Esp8266BaseWiFi::isConnected() ? Esp8266BaseMQTTState::WAITING_TIME
                                             : Esp8266BaseMQTTState::WAITING_WIFI;
    ESP8266BASE_LOG_I("MQTT", "shutdown_resume result=%u reconnect=yes",
                      (unsigned)_shutdownResult);
}

bool Esp8266BaseMQTT::pauseForOTA() {
    if (!_configured || !_begun) return true;
    if (!_shutdownActive) {
        ESP8266BASE_LOG_E("MQTT", "ota_pause result=not_started");
        return false;
    }
    while (_state != Esp8266BaseMQTTState::PAUSED) {
        _handleShutdown();
        yield();
    }
    bool ready = shutdownSucceeded() && mqttClient.disconnected();
    ESP8266BASE_LOG_I("MQTT", "ota_pause ready=%s result=%u heap=%u max=%u",
                      ready ? "yes" : "no", (unsigned)_shutdownResult,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    return ready;
}

void Esp8266BaseMQTT::resumeAfterOTAFailure() {
    resumeAfterShutdown();
}

void Esp8266BaseMQTT::keepPausedAfterOTASuccess() {
    if (!_configured || !_begun) return;
    _shutdownActive = true;
    _state = Esp8266BaseMQTTState::PAUSED;
    ESP8266BASE_LOG_I("MQTT", "ota_success reconnect_allowed=no action=reboot");
}

void Esp8266BaseMQTT::_scheduleRetry() {
    _retryAt = millis() + _retryDelay;
    uint32_t nextAttempt = _attemptCount < 0xFFFFFFFFUL ? _attemptCount + 1UL : _attemptCount;
    ESP8266BASE_LOG_W("MQTT", "reconnect_scheduled retry_in=%lus next_attempt=%lu reason=%s",
                      (unsigned long)(_retryDelay / 1000UL),
                      (unsigned long)nextAttempt, lastDisconnectReasonName());
    if (_retryDelay < ESP8266BASE_MQTT_RETRY_MAX_MS) {
        uint32_t next = _retryDelay * 2UL;
        _retryDelay = next > ESP8266BASE_MQTT_RETRY_MAX_MS ? ESP8266BASE_MQTT_RETRY_MAX_MS : next;
    }
    _state = Esp8266BaseMQTTState::BACKOFF;
}

void Esp8266BaseMQTT::_disconnectForGate(Esp8266BaseMQTTState waitingState) {
    if (!mqttClient.disconnected()) {
        mqttClient.disconnect(true);
        mqttClient.loop();
    }
    _state = waitingState;
}

void Esp8266BaseMQTT::_onConnect(bool sessionPresent) {
    if (_shutdownActive) {
        // beginShutdown() 在未连接/建连中会形成 NOT_CONNECTED 暂停；若迟到的
        // CONNACK 随后到达，保持传输暂停，不能在没有最终 PUBACK 时发送 DISCONNECT。
        _state = Esp8266BaseMQTTState::PAUSED;
        return;
    }
    _state = Esp8266BaseMQTTState::CONNECTED;
    _lastReason = Esp8266BaseMQTTDisconnectReason::NONE;
    ESP8266BASE_LOG_I("MQTT", "connected session_present=%s attempt=%lu free_heap=%u max_block=%u action=application_resubscribe",
                      sessionPresent ? "yes" : "no", (unsigned long)_attemptCount,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    if (_connectedCallback) _connectedCallback(sessionPresent);
}

void Esp8266BaseMQTT::_onDisconnect(uint8_t reason) {
    _reconnectRequested = false;
    _lastReason = mapReason((espMqttClientTypes::DisconnectReason)reason);
    if (_lastReason == Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED &&
        mqttClient.lastTlsErrorCode() != 0) {
        ESP8266BASE_LOG_W("MQTT", "disconnected reason=%s reason_code=%u tls_code=%d tls_detail=%s free_heap=%u max_block=%u",
                          lastDisconnectReasonName(), (unsigned)reason,
                          mqttClient.lastTlsErrorCode(), mqttClient.lastTlsErrorText(),
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    } else {
        ESP8266BASE_LOG_W("MQTT", "disconnected reason=%s reason_code=%u free_heap=%u max_block=%u",
                          lastDisconnectReasonName(), (unsigned)reason,
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    }
    if (_cleanSession) {
        const size_t discardedPackets = mqttClient.queueSize();
        mqttClient.clearQueue(true);
        if (discardedPackets > 0) {
            ESP8266BASE_LOG_I("MQTT", "session_queue_discarded clean_session=yes packets=%u",
                              (unsigned)discardedPackets);
        }
    }
    const bool controlledShutdown = _shutdownActive;
    if (controlledShutdown) {
        Esp8266BaseMQTTShutdownResult result = _shutdownResult;
        if (_state == Esp8266BaseMQTTState::SHUTDOWN_DISCONNECTING &&
            result == Esp8266BaseMQTTShutdownResult::IN_PROGRESS &&
            _lastReason == Esp8266BaseMQTTDisconnectReason::USER_OK) {
            result = Esp8266BaseMQTTShutdownResult::SUCCESS;
        } else if (result == Esp8266BaseMQTTShutdownResult::IN_PROGRESS) {
            result = Esp8266BaseMQTTShutdownResult::CONNECTION_LOST;
        }
        _finishShutdown(result);
    }
    if (_disconnectedCallback) _disconnectedCallback(_lastReason);
    if (!controlledShutdown) _scheduleRetry();
}

void Esp8266BaseMQTT::_onMessage(uint8_t qos, bool dup, bool retain, uint16_t packetId,
                                 const char* topic, const uint8_t* payload,
                                 size_t len, size_t index, size_t total) {
    if (_shutdownActive || !_messageCallback) return;
    _messageCallback(qos, dup, retain, packetId, topic, payload, len, index, total);
}

void Esp8266BaseMQTT::_onSubscribeAck(uint16_t packetId, const uint8_t* returnCodes,
                                      size_t length) {
    bool rejected = false;
    for (size_t i = 0; returnCodes && i < length; i++) {
        if (returnCodes[i] == 0x80U) {
            rejected = true;
            ESP8266BASE_LOG_E("MQTT", "suback_rejected packet_id=%u index=%u code=0x80",
                              (unsigned)packetId, (unsigned)i);
        }
    }
    if (!rejected) {
        ESP8266BASE_LOG_I("MQTT", "suback_accepted packet_id=%u count=%u",
                          (unsigned)packetId, (unsigned)length);
    }
    if (_subscribeAckCallback) _subscribeAckCallback(packetId, returnCodes, length);
}

void Esp8266BaseMQTT::_onPublishAck(uint16_t packetId) {
    ESP8266BASE_LOG_I("MQTT", "publish_ack packet_id=%u", (unsigned)packetId);
    if (_shutdownActive && _state == Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK) {
        if (packetId == _shutdownPacketId) {
            _shutdownPacketId = 0;
            _startGracefulDisconnect();
        } else {
            ESP8266BASE_LOG_W("MQTT", "shutdown_ack_ignored got=%u expected=%u",
                              (unsigned)packetId, (unsigned)_shutdownPacketId);
        }
    }
    if (_publishAckCallback) _publishAckCallback(packetId);
}

void Esp8266BaseMQTT::_onClientError(uint16_t packetId, uint8_t error) {
    Esp8266BaseMQTTClientError mapped = Esp8266BaseMQTTClientError::UNKNOWN;
    const char* name = "unknown";
    switch (error) {
        case 0: mapped = Esp8266BaseMQTTClientError::SUCCESS; name = "success"; break;
        case 1: mapped = Esp8266BaseMQTTClientError::OUT_OF_MEMORY; name = "out_of_memory"; break;
        case 2: mapped = Esp8266BaseMQTTClientError::MAX_RETRIES; name = "max_retries"; break;
        case 3: mapped = Esp8266BaseMQTTClientError::MALFORMED_PARAMETER; name = "malformed_parameter"; break;
        case 4: mapped = Esp8266BaseMQTTClientError::MISC_ERROR; name = "misc_error"; break;
        default: break;
    }
    ESP8266BASE_LOG_E("MQTT", "client_error packet_id=%u error=%u name=%s",
                      (unsigned)packetId, (unsigned)mapped, name);
    if (_shutdownActive && _state == Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK &&
        packetId == _shutdownPacketId && mapped != Esp8266BaseMQTTClientError::SUCCESS) {
        mqttClient.clearQueue(true);
        _finishShutdown(Esp8266BaseMQTTShutdownResult::PUBLISH_FAILED);
    }
    if (_clientErrorCallback) _clientErrorCallback(packetId, mapped);
}

#endif
