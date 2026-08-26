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

enum class Esp8266BaseMQTTClientError : uint8_t {
    SUCCESS = 0,
    OUT_OF_MEMORY = 1,
    MAX_RETRIES = 2,
    MALFORMED_PARAMETER = 3,
    MISC_ERROR = 4,
    UNKNOWN = 255
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
// returnCodes 是只读 uint8_t 数组，仅在回调期间有效；0x80 表示订阅被拒绝。
typedef void (*Esp8266BaseMQTTSubscribeAckCallback)(uint16_t packetId,
                                                    const uint8_t* returnCodes,
                                                    size_t length);
typedef void (*Esp8266BaseMQTTPublishAckCallback)(uint16_t packetId);
typedef void (*Esp8266BaseMQTTClientErrorCallback)(uint16_t packetId,
                                                   Esp8266BaseMQTTClientError error);

class Esp8266BaseMQTT {
public:
    // 必须在 Esp8266Base::begin() 前调用。短字符串复制到固定缓冲；trustAnchors
    // 和 willPayload 由业务持有，并须覆盖整个 MQTT 生命周期。
    static bool configure(const Esp8266BaseMQTTConfig& config);
    static void setCallbacks(Esp8266BaseMQTTConnectedCallback connected,
                             Esp8266BaseMQTTDisconnectedCallback disconnected,
                             Esp8266BaseMQTTMessageCallback message,
                             Esp8266BaseMQTTSubscribeAckCallback subscribeAck = nullptr,
                             Esp8266BaseMQTTPublishAckCallback publishAck = nullptr,
                             Esp8266BaseMQTTClientErrorCallback clientError = nullptr);
    static bool begin();
    static void handle();

    static uint16_t publish(const char* topic, uint8_t qos, bool retain,
                            const uint8_t* payload, size_t length);
    static uint16_t publish(const char* topic, uint8_t qos, bool retain,
                            const char* payload);
    static uint16_t subscribe(const char* topic, uint8_t qos);
    // 非阻塞请求释放当前传输；下一轮 handle() 执行断开，随后沿用既有退避重连。
    // 未配置、未 begin 或 OTA 暂停时返回 false；重复请求幂等返回 true。
    static bool requestReconnect();
    // 业务完成订阅/初始握手后确认本连接稳定，并把后续普通断线退避恢复为初始值。
    // 仅当前已连接、未暂停且没有待处理重连请求时成功。
    static bool markConnectionReady();

    static bool connected();
    static bool isConfigured();
    static Esp8266BaseMQTTState state();
    static const char* stateName();
    static uint32_t attemptCount();
    static uint32_t nextAttemptAt();
    static Esp8266BaseMQTTDisconnectReason lastDisconnectReason();
    static const char* lastDisconnectReasonName();
    static int lastTlsErrorCode();
    // 指向内部固定缓冲；下一次连接尝试会清空，调用方不得保存或修改。
    static const char* lastTlsErrorText();

    // 由 OTA 编排调用。先停止消息分发，再尝试正常 DISCONNECT；必要时强制释放 TLS。
    static bool pauseForOTA();
    static void resumeAfterOTAFailure();
    static void keepPausedAfterOTASuccess();

private:
    static bool _configured;
    static bool _begun;
    static bool _otaPaused;
    static bool _reconnectRequested;
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
    static Esp8266BaseMQTTSubscribeAckCallback _subscribeAckCallback;
    static Esp8266BaseMQTTPublishAckCallback _publishAckCallback;
    static Esp8266BaseMQTTClientErrorCallback _clientErrorCallback;

    static void _scheduleRetry();
    static bool _isDue(uint32_t now, uint32_t due);
    static void _disconnectForGate(Esp8266BaseMQTTState waitingState);
    static void _onConnect(bool sessionPresent);
    static void _onDisconnect(uint8_t reason);
    static void _onMessage(uint8_t qos, bool dup, bool retain, uint16_t packetId,
                           const char* topic, const uint8_t* payload,
                           size_t len, size_t index, size_t total);
    static void _onSubscribeAck(uint16_t packetId, const uint8_t* returnCodes, size_t length);
    static void _onPublishAck(uint16_t packetId);
    static void _onClientError(uint16_t packetId, uint8_t error);
};

#endif
