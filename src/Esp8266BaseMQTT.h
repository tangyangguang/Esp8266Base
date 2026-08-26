#pragma once
#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_MQTT

#include <Arduino.h>

namespace BearSSL { class X509List; }

#ifndef ESP8266BASE_MQTT_RETRY_INITIAL_MS
#define ESP8266BASE_MQTT_RETRY_INITIAL_MS 2000UL
#endif

#ifndef ESP8266BASE_MQTT_RETRY_MAX_MS
#define ESP8266BASE_MQTT_RETRY_MAX_MS 60000UL
#endif

#if ESP8266BASE_MQTT_RETRY_INITIAL_MS < 1000UL
#error "ESP8266BASE_MQTT_RETRY_INITIAL_MS must be at least 1000"
#endif
#if ESP8266BASE_MQTT_RETRY_MAX_MS < ESP8266BASE_MQTT_RETRY_INITIAL_MS
#error "ESP8266BASE_MQTT_RETRY_MAX_MS must be >= ESP8266BASE_MQTT_RETRY_INITIAL_MS"
#endif

enum class Esp8266BaseMQTTState : uint8_t {
    UNCONFIGURED = 0,
    WAITING_WIFI,
    WAITING_TIME,
    BACKOFF,
    CONNECTING,
    CONNECTED,
    PAUSED_OTA
};

enum class Esp8266BaseMQTTDisconnectReason : uint8_t {
    NONE = 0,
    USER_OK,
    UNACCEPTABLE_PROTOCOL,
    IDENTIFIER_REJECTED,
    SERVER_UNAVAILABLE,
    MALFORMED_CREDENTIALS,
    NOT_AUTHORIZED,
    TLS_BAD_FINGERPRINT,
    TCP_DISCONNECTED,
    UNKNOWN
};

struct Esp8266BaseMQTTConfig {
    const char* host;
    uint16_t port;
    const char* clientId;
    const char* username;
    const char* password;
    uint16_t keepAliveSeconds;
    bool cleanSession;
    const BearSSL::X509List* trustAnchors;
    const char* willTopic;
    const uint8_t* willPayload;
    size_t willPayloadLength;
    uint8_t willQos;
    bool willRetain;
};

typedef void (*Esp8266BaseMQTTConnectedCallback)(bool sessionPresent);
typedef void (*Esp8266BaseMQTTDisconnectedCallback)(Esp8266BaseMQTTDisconnectReason reason);
typedef void (*Esp8266BaseMQTTMessageCallback)(uint8_t qos, bool dup, bool retain,
                                               uint16_t packetId, const char* topic,
                                               const uint8_t* payload, size_t len,
                                               size_t index, size_t total);

class Esp8266BaseMQTT {
public:
    // 必须在 Esp8266Base::begin() 前调用。短字符串复制到固定缓冲；trustAnchors
    // 和 willPayload 由业务持有，并须覆盖整个 MQTT 生命周期。
    static bool configure(const Esp8266BaseMQTTConfig& config);
    static void setCallbacks(Esp8266BaseMQTTConnectedCallback connected,
                             Esp8266BaseMQTTDisconnectedCallback disconnected,
                             Esp8266BaseMQTTMessageCallback message);
    static bool begin();
    static void handle();

    static uint16_t publish(const char* topic, uint8_t qos, bool retain,
                            const uint8_t* payload, size_t length);
    static uint16_t publish(const char* topic, uint8_t qos, bool retain,
                            const char* payload);
    static uint16_t subscribe(const char* topic, uint8_t qos);

    static bool connected();
    static bool isConfigured();
    static Esp8266BaseMQTTState state();
    static const char* stateName();
    static uint32_t attemptCount();
    static uint32_t nextAttemptAt();
    static Esp8266BaseMQTTDisconnectReason lastDisconnectReason();
    static const char* lastDisconnectReasonName();

    // 由 OTA 编排调用。先停止消息分发，再尝试正常 DISCONNECT；必要时强制释放 TLS。
    static bool pauseForOTA();
    static void resumeAfterOTAFailure();
    static void keepPausedAfterOTASuccess();

private:
    static bool _configured;
    static bool _begun;
    static bool _otaPaused;
    static Esp8266BaseMQTTState _state;
    static Esp8266BaseMQTTDisconnectReason _lastReason;
    static uint32_t _attemptCount;
    static uint32_t _retryAt;
    static uint32_t _retryDelay;

    static char _host[65];
    static char _clientId[49];
    static char _username[65];
    static char _password[97];
    static char _willTopic[129];
    static const uint8_t* _willPayload;
    static uint16_t _port;
    static uint16_t _keepAlive;
    static bool _cleanSession;
    static size_t _willLength;
    static uint8_t _willQos;
    static bool _willRetain;
    static const BearSSL::X509List* _trustAnchors;

    static Esp8266BaseMQTTConnectedCallback _connectedCallback;
    static Esp8266BaseMQTTDisconnectedCallback _disconnectedCallback;
    static Esp8266BaseMQTTMessageCallback _messageCallback;

    static void _scheduleRetry();
    static void _disconnectForGate(Esp8266BaseMQTTState waitingState);
    static void _onConnect(bool sessionPresent);
    static void _onDisconnect(uint8_t reason);
    static void _onMessage(uint8_t qos, bool dup, bool retain, uint16_t packetId,
                           const char* topic, const uint8_t* payload,
                           size_t len, size_t index, size_t total);
};

#endif
