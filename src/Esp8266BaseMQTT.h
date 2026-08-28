#pragma once
#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_MQTT

#include <Arduino.h>
#include "Esp8266BaseMQTTFixed.h"

namespace BearSSL { class X509List; }

#ifndef ESP8266BASE_MQTT_RETRY_INITIAL_MS
#define ESP8266BASE_MQTT_RETRY_INITIAL_MS 2000UL
#endif

#ifndef ESP8266BASE_MQTT_RETRY_MAX_MS
#define ESP8266BASE_MQTT_RETRY_MAX_MS 60000UL
#endif

#ifndef ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS
#define ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS 5000UL
#endif

#ifndef ESP8266BASE_MQTT_ACK_TIMEOUT_MS
#define ESP8266BASE_MQTT_ACK_TIMEOUT_MS 15000UL
#endif

#ifndef ESP8266BASE_MQTT_CONNECT_TIMEOUT_MS
#define ESP8266BASE_MQTT_CONNECT_TIMEOUT_MS 10000UL
#endif

#if ESP8266BASE_MQTT_RETRY_INITIAL_MS < 1000UL
#error "ESP8266BASE_MQTT_RETRY_INITIAL_MS must be at least 1000"
#endif
#if ESP8266BASE_MQTT_RETRY_MAX_MS < ESP8266BASE_MQTT_RETRY_INITIAL_MS
#error "ESP8266BASE_MQTT_RETRY_MAX_MS must be >= ESP8266BASE_MQTT_RETRY_INITIAL_MS"
#endif
#if ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS == 0 || ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS > 0x7FFFFFFFUL
#error "ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS must be between 1 and 2147483647"
#endif

enum class Esp8266BaseMQTTState : uint8_t {
    UNCONFIGURED = 0,
    WAITING_WIFI,
    WAITING_TIME,
    BACKOFF,
    CONNECTING,
    CONNECTED,
    SHUTDOWN_WAIT_ACK,
    SHUTDOWN_DISCONNECTING,
    PAUSED
};

enum class Esp8266BaseMQTTShutdownResult : uint8_t {
    NONE = 0,
    IN_PROGRESS,
    SUCCESS,
    NOT_CONNECTED,
    INVALID_ARGUMENT,
    PUBLISH_FAILED,
    CONNECTION_LOST,
    PUBACK_TIMEOUT,
    DISCONNECT_FAILED,
    DISCONNECT_TIMEOUT
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
    CAPACITY_EXHAUSTED = 5,
    PACKET_TOO_LARGE = 6,
    PROTOCOL_ERROR = 7,
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
                            const uint8_t* payload, size_t length,
                            Esp8266BaseMQTTPublishPriority priority);
    static uint16_t publish(const char* topic, uint8_t qos, bool retain,
                            const char* payload);
    static uint16_t publish(const char* topic, uint8_t qos, bool retain,
                            const char* payload,
                            Esp8266BaseMQTTPublishPriority priority);
    static uint16_t subscribe(const char* topic, uint8_t qos);
    // 非阻塞请求释放当前传输；下一轮 handle() 执行断开，随后沿用既有退避重连。
    // 未配置、未 begin 或受控下线暂停时返回 false；重复请求幂等返回 true。
    static bool requestReconnect();
    // 业务完成订阅/初始握手后确认本连接稳定，并把后续普通断线退避恢复为初始值。
    // 仅当前已连接、未暂停且没有待处理重连请求时成功。
    static bool markConnectionReady();
    // 只控制后续 DNS/TCP/TLS 连接尝试。false 不拆除当前连接，也不停止当前
    // MQTT loop；适合执行器运行期间避免同步建连阻塞本地截止逻辑。
    static void setConnectAttemptsEnabled(bool enabled);
    static bool connectAttemptsEnabled();

    // 发布业务提供的 retained QoS1 最终消息；匹配 PUBACK 后才发送正常 MQTT
    // DISCONNECT，并保持暂停。topic/payload 会被上游出站队列复制，本库不保存业务内容。
    // 返回 true 仅表示最终消息已成功入队，不表示 PUBACK 或关闭成功。
    static bool beginShutdown(const char* topic, const uint8_t* payload, size_t length,
                              uint32_t timeoutMs = ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS);
    static bool beginShutdown(const char* topic, const char* payload,
                              uint32_t timeoutMs = ESP8266BASE_MQTT_SHUTDOWN_TIMEOUT_MS);
    static Esp8266BaseMQTTShutdownResult shutdownResult();
    static const char* shutdownResultName();
    // true 只表示已正常断开且最终消息收到匹配 PUBACK。
    static bool shutdownSucceeded();
    static bool shutdownPaused();
    // 显式恢复连接许可；供关闭失败后的业务恢复或 OTA 失败恢复使用。
    // 最后 shutdownResult 保留到下一次 beginShutdown()。
    static void resumeAfterShutdown();

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
    static size_t queuedPackets();
    static constexpr size_t outboxCapacity() { return ESP8266BASE_MQTT_TX_SLOTS; }
    static constexpr size_t maxTopicBytes() { return ESP8266BASE_MQTT_MAX_TOPIC_BYTES; }
    static constexpr size_t maxPayloadBytes() { return ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES; }

    // 由 OTA 编排调用。有活动会话时业务 prepare 必须先 beginShutdown()；无活动
    // transport 时直接暂停并返回 true，但 shutdownResult 不会伪装成 SUCCESS。
    // CONNECTED/CONNECTING 或 transport 尚未释放且未启动受控下线时返回 false。
    static bool pauseForOTA();
    static void resumeAfterOTAFailure();
    static void keepPausedAfterOTASuccess();

private:
    static bool _configured;
    static bool _begun;
    static bool _shutdownActive;
    static bool _reconnectRequested;
    static bool _connectAttemptsEnabled;
    static Esp8266BaseMQTTState _state;
    static Esp8266BaseMQTTDisconnectReason _lastReason;
    static uint32_t _attemptCount;
    static uint32_t _retryAt;
    static uint32_t _retryDelay;
    static Esp8266BaseMQTTShutdownResult _shutdownResult;
    static uint16_t _shutdownPacketId;
    static uint32_t _shutdownDeadline;

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
    static void _handleShutdown();
    static void _startGracefulDisconnect();
    static void _finishShutdown(Esp8266BaseMQTTShutdownResult result);
    static void _disconnectForGate(Esp8266BaseMQTTState waitingState);
    static bool _connectTransport();
    static bool _pumpTransport();
    static bool _pumpIncoming();
    static bool _pumpOutbox();
    static bool _sendDisconnectPacket();
    static void _closeTransport(Esp8266BaseMQTTDisconnectReason reason,
                                bool scheduleRetry, bool notifyApplication = true);
    static void _captureTlsError();
    static void _onConnect(bool sessionPresent);
    static void _onDisconnect(Esp8266BaseMQTTDisconnectReason reason);
    static void _onMessage(uint8_t qos, bool dup, bool retain, uint16_t packetId,
                           const char* topic, const uint8_t* payload,
                           size_t len, size_t index, size_t total);
    static void _onSubscribeAck(uint16_t packetId, const uint8_t* returnCodes, size_t length);
    static void _onPublishAck(uint16_t packetId);
    static void _onClientError(uint16_t packetId, Esp8266BaseMQTTClientError error);
};

#endif
