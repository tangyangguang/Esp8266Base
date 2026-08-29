#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_MQTT

#include "Esp8266BaseMQTT.h"
#if ESP8266BASE_USE_WEB
#include "Esp8266BaseWeb.h"
#endif
#include "Esp8266BaseLog.h"
#include "Esp8266BaseNTP.h"
#include "Esp8266BaseWiFi.h"
#include <WiFiClientSecureBearSSL.h>

namespace Esp8266BaseMQTTInternal {
static BearSSL::WiFiClientSecure client;
static FixedOutbox outbox;
static bool mqttConnected = false;
static bool transportOpen = false;
static bool pingOutstanding = false;
static uint32_t connectDeadline = 0;
static uint32_t lastIoAt = 0;
static uint32_t pingDeadline = 0;
static uint32_t inFlightSentAt = 0;
static uint16_t nextPacketId = 0;
static int lastTlsCode = 0;
static char lastTlsText[96] = "";

struct RxState {
    uint8_t header;
    uint8_t remainingBytes;
    uint8_t control[8];
    bool remainingDone;
    uint32_t remainingLength;
    uint32_t multiplier;
    uint32_t bodyRead;
    uint16_t topicLength;
    uint16_t packetId;
    uint32_t payloadLength;
    uint32_t payloadRead;
    char topic[ESP8266BASE_MQTT_MAX_TOPIC_BYTES + 1];
    uint8_t payload[ESP8266BASE_MQTT_RX_CHUNK_BYTES];

    void reset() {
        header = 0;
        remainingBytes = 0;
        remainingDone = false;
        remainingLength = 0;
        multiplier = 1;
        bodyRead = 0;
        topicLength = 0;
        packetId = 0;
        payloadLength = 0;
        payloadRead = 0;
        topic[0] = '\0';
    }
};
static RxState rx;

static bool copyText(const char* source, char* target, size_t capacity, bool required) {
    if (!target || capacity == 0) return false;
    target[0] = '\0';
    if (!source || !source[0]) return !required;
    const size_t length = strlen(source);
    if (length >= capacity) return false;
    memcpy(target, source, length + 1);
    return true;
}

static uint16_t allocatePacketId() {
    ++nextPacketId;
    if (nextPacketId == 0) ++nextPacketId;
    return nextPacketId;
}

static uint8_t encodeRemainingLength(uint32_t value, uint8_t out[4]) {
    uint8_t used = 0;
    do {
        uint8_t byte = static_cast<uint8_t>(value % 128U);
        value /= 128U;
        if (value) byte |= 0x80U;
        out[used++] = byte;
    } while (value && used < 4);
    return used;
}

static bool writeExact(const uint8_t* data, size_t length) {
    if (length == 0) return true;
    if (!data || !transportOpen) return false;
    size_t written = 0;
    const uint32_t deadline = millis() + ESP8266BASE_MQTT_CONNECT_TIMEOUT_MS;
    while (written < length) {
        const size_t count = client.write(data + written, length - written);
        if (count > 0) {
            written += count;
            continue;
        }
        if (!client.connected() || static_cast<int32_t>(millis() - deadline) >= 0) return false;
        yield();
    }
    lastIoAt = millis();
    return true;
}

static bool writeFixedHeader(uint8_t header, uint32_t remainingLength) {
    uint8_t encoded[5];
    encoded[0] = header;
    const uint8_t used = encodeRemainingLength(remainingLength, encoded + 1);
    return writeExact(encoded, static_cast<size_t>(used) + 1U);
}

static bool writeUint16(uint16_t value) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value >> 8),
                              static_cast<uint8_t>(value & 0xffU)};
    return writeExact(bytes, sizeof(bytes));
}

static bool writeString(const char* text) {
    const size_t length = text ? strlen(text) : 0;
    return length <= 65535U && writeUint16(static_cast<uint16_t>(length)) &&
           writeExact(reinterpret_cast<const uint8_t*>(text), length);
}

static bool writeBinaryString(const uint8_t* data, size_t length) {
    return length <= 65535U && writeUint16(static_cast<uint16_t>(length)) &&
           writeExact(data, length);
}

static bool sendAck(uint8_t type, uint16_t packetId) {
    return writeFixedHeader(type, 2) && writeUint16(packetId);
}
}  // namespace Esp8266BaseMQTTInternal

using namespace Esp8266BaseMQTTInternal;

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

bool Esp8266BaseMQTT::configure(const Esp8266BaseMQTTConfig& config) {
    if (_begun) return false;
    const bool stringsOk = copyText(config.host, _host, sizeof(_host), true) &&
        copyText(config.clientId, _clientId, sizeof(_clientId), true) &&
        copyText(config.username, _username, sizeof(_username), false) &&
        copyText(config.password, _password, sizeof(_password), false) &&
        copyText(config.willTopic, _willTopic, sizeof(_willTopic), false);
    const bool credentialsOk = !config.password || !config.password[0] ||
        (config.username && config.username[0]);
    const bool willOk = validWill(config.willTopic, config.willPayload,
                                  config.willPayloadLength, config.willQos);
    const bool requiredOk = config.port && config.keepAliveSeconds && config.trustAnchors;
    if (!stringsOk || !credentialsOk || !willOk || !requiredOk) {
        _configured = false;
        _state = Esp8266BaseMQTTState::UNCONFIGURED;
        ESP8266BASE_LOG_E("MQTT", "configure_rejected reason=invalid_or_oversize_config");
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
    ESP8266BASE_LOG_I("MQTT", "configured host=%s port=%u client_id=%s keepalive=%us clean_session=%s tx_slots=%u max_payload=%u rx_chunk=%u",
        _host, static_cast<unsigned>(_port), _clientId, static_cast<unsigned>(_keepAlive),
        _cleanSession ? "yes" : "no", static_cast<unsigned>(ESP8266BASE_MQTT_TX_SLOTS),
        static_cast<unsigned>(ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES),
        static_cast<unsigned>(ESP8266BASE_MQTT_RX_CHUNK_BYTES));
    return true;
}

void Esp8266BaseMQTT::setCallbacks(Esp8266BaseMQTTConnectedCallback connected,
    Esp8266BaseMQTTDisconnectedCallback disconnected, Esp8266BaseMQTTMessageCallback message,
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
    outbox.clear();
    rx.reset();
    if (!_configured) {
        _state = Esp8266BaseMQTTState::UNCONFIGURED;
        return false;
    }
    client.setTrustAnchors(_trustAnchors);
    client.setBufferSizes(4096, 1024);
    client.setTimeout(ESP8266BASE_MQTT_CONNECT_TIMEOUT_MS);
    client.setNoDelay(true);
    _retryAt = millis();
    ESP8266BASE_LOG_I("MQTT", "mqtt_transport_ready implementation=fixed_sync_tls tls_buffers=4096/1024 heap_outbox=no heap_packet_buffer=no");
    return true;
}

void Esp8266BaseMQTT::handle() {
    if (!_begun || !_configured) return;
    if (_shutdownActive) return _handleShutdown();
    if (!Esp8266BaseWiFi::isConnected()) return _disconnectForGate(Esp8266BaseMQTTState::WAITING_WIFI);
    if (!Esp8266BaseNTP::isSynced()) return _disconnectForGate(Esp8266BaseMQTTState::WAITING_TIME);
    if (_reconnectRequested) {
        _reconnectRequested = false;
        _closeTransport(Esp8266BaseMQTTDisconnectReason::USER_OK, true);
        return;
    }
    if (transportOpen) {
        if (!_pumpTransport()) {
            const Esp8266BaseMQTTDisconnectReason reason =
                _lastReason == Esp8266BaseMQTTDisconnectReason::NONE
                    ? Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED : _lastReason;
            _closeTransport(reason, true);
        }
        return;
    }
    if (!_connectAttemptsEnabled || !_isDue(millis(), _retryAt)) {
        _state = Esp8266BaseMQTTState::BACKOFF;
        return;
    }
#if ESP8266BASE_USE_WEB
    // Defer new DNS/TCP/TLS handshakes while a local Web request was served
    // recently: handshake allocations and Web response peaks must not overlap.
    // Established connections are unaffected; the attempt resumes next loop.
    {
        const uint32_t webLast = Esp8266BaseWeb::lastActivityMs();
        if (webLast != 0U && (millis() - webLast) < 3000UL) {
            _state = Esp8266BaseMQTTState::BACKOFF;
            return;
        }
    }
#endif
    if (_attemptCount < 0xffffffffUL) ++_attemptCount;
    ESP8266BASE_LOG_I("MQTT", "connect_attempt attempt=%lu host=%s port=%u free_heap=%u max_block=%u",
        static_cast<unsigned long>(_attemptCount), _host, static_cast<unsigned>(_port),
        static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
    if (!_connectTransport()) {
        _lastReason = Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED;
        _scheduleRetry();
    }
}

bool Esp8266BaseMQTT::_connectTransport() {
    lastTlsCode = 0;
    lastTlsText[0] = '\0';
    rx.reset();
    if (!client.connect(_host, _port)) {
        _captureTlsError();
        ESP8266BASE_LOG_W("MQTT", "transport_connect_failed tls_code=%d tls_error=%s",
                          lastTlsCode, lastTlsText[0] ? lastTlsText : "none");
        client.stop();
        return false;
    }
    transportOpen = true;
    mqttConnected = false;
    pingOutstanding = false;
    lastIoAt = millis();
    uint32_t remaining = 10U + 2U + strlen(_clientId);
    const uint8_t flags = connectFlags(_cleanSession, _willTopic[0], _willQos,
                                       _willRetain, _username[0], _password[0]);
    if (_willTopic[0]) {
        remaining += 2U + strlen(_willTopic) + 2U + _willLength;
    }
    if (_username[0]) remaining += 2U + strlen(_username);
    if (_password[0]) remaining += 2U + strlen(_password);
    const uint8_t protocol[] = {0, 4, 'M', 'Q', 'T', 'T', 4};
    const uint8_t tail[] = {flags, static_cast<uint8_t>(_keepAlive >> 8),
                            static_cast<uint8_t>(_keepAlive & 0xffU)};
    bool ok = writeFixedHeader(0x10, remaining) && writeExact(protocol, sizeof(protocol)) &&
        writeExact(tail, sizeof(tail)) && writeString(_clientId);
    if (ok && _willTopic[0]) ok = writeString(_willTopic) && writeBinaryString(_willPayload, _willLength);
    if (ok && _username[0]) ok = writeString(_username);
    if (ok && _password[0]) ok = writeString(_password);
    if (!ok) {
        _captureTlsError();
        client.stop();
        transportOpen = false;
        return false;
    }
    connectDeadline = millis() + ESP8266BASE_MQTT_CONNECT_TIMEOUT_MS;
    _state = Esp8266BaseMQTTState::CONNECTING;
    return true;
}

bool Esp8266BaseMQTT::_pumpTransport() {
    if (!client.connected() || !_pumpIncoming() || !_pumpOutbox()) return false;
    const uint32_t now = millis();
    if (!mqttConnected) {
        if (_isDue(now, connectDeadline)) {
            ESP8266BASE_LOG_W("MQTT", "connack_timeout timeout_ms=%lu",
                              static_cast<unsigned long>(ESP8266BASE_MQTT_CONNECT_TIMEOUT_MS));
            return false;
        }
        return true;
    }
    const FixedPacket* pending = outbox.inFlight();
    if (pending && _isDue(now, inFlightSentAt + ESP8266BASE_MQTT_ACK_TIMEOUT_MS)) {
        _onClientError(pending->packetId, Esp8266BaseMQTTClientError::MAX_RETRIES);
        return false;
    }
    const uint32_t keepAliveMs = static_cast<uint32_t>(_keepAlive) * 1000UL;
    if (pingOutstanding) {
        if (_isDue(now, pingDeadline)) return false;
    } else if (_isDue(now, lastIoAt + keepAliveMs)) {
        const uint8_t ping[] = {0xc0, 0};
        if (!writeExact(ping, sizeof(ping))) return false;
        pingOutstanding = true;
        pingDeadline = now + keepAliveMs;
    }
    return true;
}

bool Esp8266BaseMQTT::_pumpOutbox() {
    if (!mqttConnected) return true;
    FixedPacket* packet = outbox.nextToSend();
    if (!packet) return true;
    bool ok = false;
    if (packet->kind == PacketKind::SUBSCRIBE) {
        const uint32_t remaining = 5U + strlen(packet->topic);
        ok = writeFixedHeader(0x82, remaining) && writeUint16(packet->packetId) &&
             writeString(packet->topic) && writeExact(&packet->qos, 1);
    } else if (packet->kind == PacketKind::PUBLISH) {
        const uint32_t remaining = 2U + strlen(packet->topic) + (packet->qos ? 2U : 0U) + packet->payloadLength;
        const uint8_t header = 0x30U | (packet->retain ? 1U : 0U) |
            (packet->qos ? 2U : 0U) | (packet->dup ? 8U : 0U);
        ok = writeFixedHeader(header, remaining) && writeString(packet->topic) &&
            (!packet->qos || writeUint16(packet->packetId)) &&
            writeExact(packet->payload, packet->payloadLength);
    }
    if (!ok) return false;
    const bool waits = packet->qos != 0;
    outbox.markSent(packet);
    if (waits) inFlightSentAt = millis();
    return true;
}

bool Esp8266BaseMQTT::_pumpIncoming() {
    uint16_t budget = 512;
    while (budget && client.available() > 0) {
        if (!rx.header) {
            const int value = client.read();
            if (value < 0) break;
            rx.header = static_cast<uint8_t>(value);
            --budget;
            continue;
        }
        if (!rx.remainingDone) {
            const int value = client.read();
            if (value < 0) break;
            const uint8_t byte = static_cast<uint8_t>(value);
            rx.remainingLength += static_cast<uint32_t>(byte & 0x7fU) * rx.multiplier;
            rx.multiplier *= 128U;
            ++rx.remainingBytes;
            --budget;
            if (byte & 0x80U) {
                if (rx.remainingBytes >= 4) return false;
                continue;
            }
            rx.remainingDone = true;
            if ((rx.header >> 4) != 3 && rx.remainingLength > sizeof(rx.control)) return false;
            continue;
        }
        const uint8_t type = rx.header >> 4;
        if (type != 3) {
            while (budget && client.available() > 0 && rx.bodyRead < rx.remainingLength) {
                rx.control[rx.bodyRead++] = static_cast<uint8_t>(client.read());
                --budget;
            }
            if (rx.bodyRead < rx.remainingLength) break;
            const uint8_t header = rx.header;
            const uint32_t length = rx.remainingLength;
            uint8_t control[8];
            memcpy(control, rx.control, length);
            rx.reset();
            lastIoAt = millis();
            if (type == 2 && header == 0x20 && length == 2) {
                if (control[1]) {
                    static const Esp8266BaseMQTTDisconnectReason reasons[] = {
                        Esp8266BaseMQTTDisconnectReason::NONE,
                        Esp8266BaseMQTTDisconnectReason::UNACCEPTABLE_PROTOCOL,
                        Esp8266BaseMQTTDisconnectReason::IDENTIFIER_REJECTED,
                        Esp8266BaseMQTTDisconnectReason::SERVER_UNAVAILABLE,
                        Esp8266BaseMQTTDisconnectReason::MALFORMED_CREDENTIALS,
                        Esp8266BaseMQTTDisconnectReason::NOT_AUTHORIZED};
                    _lastReason = control[1] < sizeof(reasons) / sizeof(reasons[0])
                        ? reasons[control[1]] : Esp8266BaseMQTTDisconnectReason::UNKNOWN;
                    ESP8266BASE_LOG_W("MQTT", "connack_rejected return_code=%u reason=%s",
                                      static_cast<unsigned>(control[1]), lastDisconnectReasonName());
                    return false;
                }
                _onConnect((control[0] & 1U) != 0);
            } else if (type == 4 && header == 0x40 && length == 2) {
                _onPublishAck(static_cast<uint16_t>((control[0] << 8) | control[1]));
            } else if (type == 9 && header == 0x90 && length >= 3) {
                const uint16_t id = static_cast<uint16_t>((control[0] << 8) | control[1]);
                if (outbox.acknowledge(PacketKind::SUBSCRIBE, id)) {
                    _onSubscribeAck(id, control + 2, length - 2U);
                }
                else ESP8266BASE_LOG_W("MQTT", "suback_ignored packet_id=%u", static_cast<unsigned>(id));
            } else if (type == 13 && header == 0xd0 && length == 0) {
                pingOutstanding = false;
            } else {
                _onClientError(0, Esp8266BaseMQTTClientError::PROTOCOL_ERROR);
                return false;
            }
            continue;
        }
        const uint8_t qos = static_cast<uint8_t>((rx.header >> 1) & 3U);
        if (qos > 1) return false;
        while (budget && client.available() > 0 && rx.bodyRead < 2) {
            rx.control[rx.bodyRead++] = static_cast<uint8_t>(client.read());
            --budget;
            if (rx.bodyRead == 2) {
                rx.topicLength = static_cast<uint16_t>((rx.control[0] << 8) | rx.control[1]);
                if (!rx.topicLength || rx.topicLength > ESP8266BASE_MQTT_MAX_TOPIC_BYTES ||
                    rx.remainingLength < 2U + rx.topicLength + (qos ? 2U : 0U)) return false;
                rx.payloadLength = rx.remainingLength - 2U - rx.topicLength - (qos ? 2U : 0U);
                if (rx.payloadLength > ESP8266BASE_MQTT_MAX_INBOUND_PAYLOAD_BYTES) return false;
            }
        }
        while (budget && client.available() > 0 && rx.bodyRead < 2U + rx.topicLength) {
            rx.topic[rx.bodyRead - 2U] = static_cast<char>(client.read());
            ++rx.bodyRead;
            --budget;
            if (rx.bodyRead == 2U + rx.topicLength) rx.topic[rx.topicLength] = '\0';
        }
        while (qos && budget && client.available() > 0 && rx.bodyRead < 4U + rx.topicLength) {
            rx.packetId = static_cast<uint16_t>((rx.packetId << 8) | static_cast<uint8_t>(client.read()));
            ++rx.bodyRead;
            --budget;
        }
        const uint32_t headerBytes = 2U + rx.topicLength + (qos ? 2U : 0U);
        if (rx.bodyRead < headerBytes) break;
        if (qos && rx.packetId == 0) return false;
        if (rx.payloadRead < rx.payloadLength && budget && client.available() > 0) {
            size_t chunk = rx.payloadLength - rx.payloadRead;
            if (chunk > sizeof(rx.payload)) chunk = sizeof(rx.payload);
            if (chunk > budget) chunk = budget;
            if (chunk > static_cast<size_t>(client.available())) chunk = client.available();
            const int count = client.read(rx.payload, chunk);
            if (count <= 0) break;
            _onMessage(qos, (rx.header & 8U) != 0, (rx.header & 1U) != 0,
                rx.packetId, rx.topic, rx.payload, count, rx.payloadRead, rx.payloadLength);
            rx.payloadRead += count;
            rx.bodyRead += count;
            budget -= count;
        }
        if (rx.payloadRead < rx.payloadLength) break;
        const uint16_t id = rx.packetId;
        if (rx.payloadLength == 0) _onMessage(qos, (rx.header & 8U) != 0,
            (rx.header & 1U) != 0, id, rx.topic, rx.payload, 0, 0, 0);
        rx.reset();
        lastIoAt = millis();
        if (qos == 1 && !sendAck(0x40, id)) return false;
    }
    return true;
}

uint16_t Esp8266BaseMQTT::publish(const char* topic, uint8_t qos, bool retain,
    const uint8_t* payload, size_t length) {
    return publish(topic, qos, retain, payload, length, Esp8266BaseMQTTPublishPriority::STATE);
}

uint16_t Esp8266BaseMQTT::publish(const char* topic, uint8_t qos, bool retain,
    const uint8_t* payload, size_t length, Esp8266BaseMQTTPublishPriority priority) {
    if (_shutdownActive || !mqttConnected || !topic || qos > 1 || (length && !payload)) return 0;
    const uint16_t id = qos ? allocatePacketId() : 0;
    if (!outbox.enqueuePublish(id, topic, qos, retain, payload, length, priority)) {
        _onClientError(id, (strlen(topic) > ESP8266BASE_MQTT_MAX_TOPIC_BYTES ||
            length > ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES)
            ? Esp8266BaseMQTTClientError::PACKET_TOO_LARGE
            : Esp8266BaseMQTTClientError::CAPACITY_EXHAUSTED);
        return 0;
    }
    return id;
}

uint16_t Esp8266BaseMQTT::publish(const char* topic, uint8_t qos, bool retain, const char* payload) {
    return payload ? publish(topic, qos, retain, reinterpret_cast<const uint8_t*>(payload), strlen(payload)) : 0;
}

uint16_t Esp8266BaseMQTT::publish(const char* topic, uint8_t qos, bool retain,
    const char* payload, Esp8266BaseMQTTPublishPriority priority) {
    return payload ? publish(topic, qos, retain, reinterpret_cast<const uint8_t*>(payload), strlen(payload), priority) : 0;
}

uint16_t Esp8266BaseMQTT::subscribe(const char* topic, uint8_t qos) {
    if (_shutdownActive || !mqttConnected || !topic || qos > 1) return 0;
    const uint16_t id = allocatePacketId();
    if (!outbox.enqueueSubscribe(id, topic, qos)) {
        _onClientError(id, strlen(topic) > ESP8266BASE_MQTT_MAX_TOPIC_BYTES
            ? Esp8266BaseMQTTClientError::PACKET_TOO_LARGE
            : Esp8266BaseMQTTClientError::CAPACITY_EXHAUSTED);
        return 0;
    }
    return id;
}

bool Esp8266BaseMQTT::requestReconnect() {
    if (!_configured || !_begun || _shutdownActive) return false;
    _reconnectRequested = true;
    return true;
}

bool Esp8266BaseMQTT::markConnectionReady() {
    if (!_configured || !_begun || _shutdownActive || _reconnectRequested || !mqttConnected) return false;
    _retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;
    return true;
}

void Esp8266BaseMQTT::setConnectAttemptsEnabled(bool enabled) { _connectAttemptsEnabled = enabled; }
bool Esp8266BaseMQTT::connectAttemptsEnabled() { return _connectAttemptsEnabled; }
bool Esp8266BaseMQTT::connected() { return !_shutdownActive && mqttConnected; }
bool Esp8266BaseMQTT::isConfigured() { return _configured; }
Esp8266BaseMQTTState Esp8266BaseMQTT::state() { return _state; }
uint32_t Esp8266BaseMQTT::attemptCount() { return _attemptCount; }
uint32_t Esp8266BaseMQTT::nextAttemptAt() { return _retryAt; }
Esp8266BaseMQTTDisconnectReason Esp8266BaseMQTT::lastDisconnectReason() { return _lastReason; }
int Esp8266BaseMQTT::lastTlsErrorCode() { return lastTlsCode; }
const char* Esp8266BaseMQTT::lastTlsErrorText() { return lastTlsText; }
size_t Esp8266BaseMQTT::queuedPackets() { return outbox.size(); }

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
    if (!_configured || !_begun || !mqttConnected) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::NOT_CONNECTED;
        _shutdownActive = true;
        _state = Esp8266BaseMQTTState::PAUSED;
        return false;
    }
    if (!topic || !topic[0] || (length && !payload) || !timeoutMs || timeoutMs > 0x7fffffffUL) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::INVALID_ARGUMENT;
        return false;
    }
    const uint16_t id = allocatePacketId();
    if (!outbox.enqueuePublish(id, topic, 1, true, payload, length,
                               Esp8266BaseMQTTPublishPriority::CRITICAL)) {
        _shutdownResult = Esp8266BaseMQTTShutdownResult::PUBLISH_FAILED;
        return false;
    }
    _reconnectRequested = false;
    _shutdownActive = true;
    _shutdownResult = Esp8266BaseMQTTShutdownResult::IN_PROGRESS;
    _shutdownPacketId = id;
    _shutdownDeadline = millis() + timeoutMs;
    _state = Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK;
    ESP8266BASE_LOG_I("MQTT", "shutdown_started packet_id=%u timeout_ms=%lu",
                      static_cast<unsigned>(id), static_cast<unsigned long>(timeoutMs));
    return true;
}

bool Esp8266BaseMQTT::beginShutdown(const char* topic, const char* payload, uint32_t timeoutMs) {
    return payload && beginShutdown(topic, reinterpret_cast<const uint8_t*>(payload), strlen(payload), timeoutMs);
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
bool Esp8266BaseMQTT::shutdownPaused() { return _shutdownActive; }

void Esp8266BaseMQTT::_handleShutdown() {
    if (_state == Esp8266BaseMQTTState::PAUSED) return;
    if (!transportOpen || !client.connected()) {
        _closeTransport(Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED, false);
        _finishShutdown(Esp8266BaseMQTTShutdownResult::CONNECTION_LOST);
        return;
    }
    const bool pumped = _pumpTransport();
    if (_state == Esp8266BaseMQTTState::PAUSED) return;
    if (!pumped) {
        _closeTransport(Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED, false);
        _finishShutdown(Esp8266BaseMQTTShutdownResult::CONNECTION_LOST);
        return;
    }
    if (_state == Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK && _isDue(millis(), _shutdownDeadline)) {
        outbox.clear();
        _closeTransport(Esp8266BaseMQTTDisconnectReason::USER_OK, false);
        _finishShutdown(Esp8266BaseMQTTShutdownResult::PUBACK_TIMEOUT);
    }
}

void Esp8266BaseMQTT::_startGracefulDisconnect() {
    _state = Esp8266BaseMQTTState::SHUTDOWN_DISCONNECTING;
    const bool sent = _sendDisconnectPacket();
    if (sent) client.flush();
    _closeTransport(Esp8266BaseMQTTDisconnectReason::USER_OK, false);
    _finishShutdown(sent ? Esp8266BaseMQTTShutdownResult::SUCCESS
                         : Esp8266BaseMQTTShutdownResult::DISCONNECT_FAILED);
}

bool Esp8266BaseMQTT::_sendDisconnectPacket() {
    const uint8_t packet[] = {0xe0, 0};
    return writeExact(packet, sizeof(packet));
}

void Esp8266BaseMQTT::_finishShutdown(Esp8266BaseMQTTShutdownResult result) {
    _shutdownResult = result;
    _shutdownPacketId = 0;
    _state = Esp8266BaseMQTTState::PAUSED;
    ESP8266BASE_LOG_I("MQTT", "shutdown_finished result=%s transport_open=%s",
                      shutdownResultName(), transportOpen ? "yes" : "no");
}

void Esp8266BaseMQTT::resumeAfterShutdown() {
    if (!_shutdownActive) return;
    _shutdownActive = false;
    _shutdownPacketId = 0;
    _retryAt = millis();
    _retryDelay = ESP8266BASE_MQTT_RETRY_INITIAL_MS;
    _state = Esp8266BaseWiFi::isConnected() ? Esp8266BaseMQTTState::WAITING_TIME
                                             : Esp8266BaseMQTTState::WAITING_WIFI;
}

bool Esp8266BaseMQTT::pauseForOTA() {
    if (!_configured || !_begun || !transportOpen) {
        _shutdownActive = true;
        _shutdownResult = Esp8266BaseMQTTShutdownResult::NOT_CONNECTED;
        _state = Esp8266BaseMQTTState::PAUSED;
        return true;
    }
    if (!_shutdownActive) return false;
    while (_state != Esp8266BaseMQTTState::PAUSED) {
        _handleShutdown();
        yield();
    }
    return !transportOpen && _shutdownResult == Esp8266BaseMQTTShutdownResult::SUCCESS;
}

void Esp8266BaseMQTT::resumeAfterOTAFailure() { resumeAfterShutdown(); }
void Esp8266BaseMQTT::keepPausedAfterOTASuccess() {
    _shutdownActive = true;
    _state = Esp8266BaseMQTTState::PAUSED;
}

void Esp8266BaseMQTT::_scheduleRetry() {
    _retryAt = millis() + _retryDelay;
    if (_retryDelay < ESP8266BASE_MQTT_RETRY_MAX_MS) {
        const uint32_t next = _retryDelay * 2UL;
        _retryDelay = next > ESP8266BASE_MQTT_RETRY_MAX_MS ? ESP8266BASE_MQTT_RETRY_MAX_MS : next;
    }
    _state = Esp8266BaseMQTTState::BACKOFF;
}

bool Esp8266BaseMQTT::_isDue(uint32_t now, uint32_t due) { return millisDue(now, due); }

void Esp8266BaseMQTT::_disconnectForGate(Esp8266BaseMQTTState waitingState) {
    if (transportOpen) _closeTransport(Esp8266BaseMQTTDisconnectReason::TCP_DISCONNECTED, false);
    _state = waitingState;
}

void Esp8266BaseMQTT::_captureTlsError() {
    char detail[sizeof(lastTlsText)] = "";
    const int code = client.getLastSSLError(detail, sizeof(detail));
    if (!code) return;
    lastTlsCode = code;
    strncpy(lastTlsText, detail, sizeof(lastTlsText) - 1);
    lastTlsText[sizeof(lastTlsText) - 1] = '\0';
}

void Esp8266BaseMQTT::_closeTransport(Esp8266BaseMQTTDisconnectReason reason,
    bool scheduleRetry, bool notifyApplication) {
    const bool wasConnected = mqttConnected;
    const size_t discarded = _cleanSession ? outbox.size() : 0;
    _captureTlsError();
    client.stop();
    transportOpen = false;
    mqttConnected = false;
    pingOutstanding = false;
    rx.reset();
    outbox.prepareReconnect(_cleanSession);
    _lastReason = reason;
    if (_cleanSession && discarded) ESP8266BASE_LOG_I("MQTT", "session_queue_discarded clean_session=yes packets=%u", static_cast<unsigned>(discarded));
    ESP8266BASE_LOG_W("MQTT", "transport_closed reason=%s retry=%s tls_code=%d queued=%u",
                      lastDisconnectReasonName(), scheduleRetry ? "yes" : "no", lastTlsCode,
                      static_cast<unsigned>(outbox.size()));
    if (notifyApplication && wasConnected && _disconnectedCallback) _disconnectedCallback(reason);
    if (scheduleRetry && !_shutdownActive) _scheduleRetry();
}

void Esp8266BaseMQTT::_onConnect(bool sessionPresent) {
    if (_shutdownActive) return;
    mqttConnected = true;
    _state = Esp8266BaseMQTTState::CONNECTED;
    _lastReason = Esp8266BaseMQTTDisconnectReason::NONE;
    ESP8266BASE_LOG_I("MQTT", "connected session_present=%s free_heap=%u max_block=%u",
                      sessionPresent ? "yes" : "no", static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
    if (_connectedCallback) _connectedCallback(sessionPresent);
}

void Esp8266BaseMQTT::_onDisconnect(Esp8266BaseMQTTDisconnectReason reason) { _closeTransport(reason, !_shutdownActive); }

void Esp8266BaseMQTT::_onMessage(uint8_t qos, bool dup, bool retain, uint16_t packetId,
    const char* topic, const uint8_t* payload, size_t len, size_t index, size_t total) {
    if (!_shutdownActive && _messageCallback) _messageCallback(qos, dup, retain, packetId, topic, payload, len, index, total);
}

void Esp8266BaseMQTT::_onSubscribeAck(uint16_t packetId, const uint8_t* codes, size_t length) {
    const bool accepted = subackAccepted(codes, length);
    ESP8266BASE_LOG_I("MQTT", "%s packet_id=%u codes=%u",
                      accepted ? "suback_accepted" : "suback_rejected",
                      static_cast<unsigned>(packetId), static_cast<unsigned>(length));
    if (_subscribeAckCallback) _subscribeAckCallback(packetId, codes, length);
}

void Esp8266BaseMQTT::_onPublishAck(uint16_t packetId) {
    if (!outbox.acknowledge(PacketKind::PUBLISH, packetId)) {
        ESP8266BASE_LOG_W("MQTT", "puback_ignored packet_id=%u",
                          static_cast<unsigned>(packetId));
        return;
    }
    if (_shutdownActive && _state == Esp8266BaseMQTTState::SHUTDOWN_WAIT_ACK && packetId == _shutdownPacketId) {
        _shutdownPacketId = 0;
        _startGracefulDisconnect();
    }
    if (_publishAckCallback) _publishAckCallback(packetId);
}

void Esp8266BaseMQTT::_onClientError(uint16_t packetId, Esp8266BaseMQTTClientError error) {
    ESP8266BASE_LOG_E("MQTT", "client_error packet_id=%u error=%u", static_cast<unsigned>(packetId), static_cast<unsigned>(error));
    if (_clientErrorCallback) _clientErrorCallback(packetId, error);
}

#endif
