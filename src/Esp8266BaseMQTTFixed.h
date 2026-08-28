#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ESP8266BASE_MQTT_TX_SLOTS
#define ESP8266BASE_MQTT_TX_SLOTS 2
#endif

#ifndef ESP8266BASE_MQTT_MAX_TOPIC_BYTES
#define ESP8266BASE_MQTT_MAX_TOPIC_BYTES 128
#endif

#ifndef ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES
#define ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES 512
#endif

#ifndef ESP8266BASE_MQTT_RX_CHUNK_BYTES
#define ESP8266BASE_MQTT_RX_CHUNK_BYTES 256
#endif

#ifndef ESP8266BASE_MQTT_MAX_INBOUND_PAYLOAD_BYTES
#define ESP8266BASE_MQTT_MAX_INBOUND_PAYLOAD_BYTES 768
#endif

#if ESP8266BASE_MQTT_TX_SLOTS < 1 || ESP8266BASE_MQTT_TX_SLOTS > 4
#error "ESP8266BASE_MQTT_TX_SLOTS must be between 1 and 4"
#endif
#if ESP8266BASE_MQTT_MAX_TOPIC_BYTES < 16 || ESP8266BASE_MQTT_MAX_TOPIC_BYTES > 128
#error "ESP8266BASE_MQTT_MAX_TOPIC_BYTES must be between 16 and 128"
#endif
#if ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES < 64 || ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES > 512
#error "ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES must be between 64 and 512"
#endif
#if ESP8266BASE_MQTT_RX_CHUNK_BYTES < 32 || ESP8266BASE_MQTT_RX_CHUNK_BYTES > 256
#error "ESP8266BASE_MQTT_RX_CHUNK_BYTES must be between 32 and 256"
#endif
#if ESP8266BASE_MQTT_MAX_INBOUND_PAYLOAD_BYTES < ESP8266BASE_MQTT_RX_CHUNK_BYTES || ESP8266BASE_MQTT_MAX_INBOUND_PAYLOAD_BYTES > 2048
#error "ESP8266BASE_MQTT_MAX_INBOUND_PAYLOAD_BYTES must be between RX chunk size and 2048"
#endif

enum class Esp8266BaseMQTTPublishPriority : uint8_t {
    CRITICAL = 0,
    EVIDENCE = 1,
    STATE = 2,
};

namespace Esp8266BaseMQTTInternal {

inline bool validWill(const char* topic, const uint8_t* payload, size_t length, uint8_t qos) {
    return qos <= 1 && length <= ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES &&
           (length == 0 || payload) && (length == 0 || (topic && topic[0]));
}

inline uint8_t connectFlags(bool cleanSession, bool hasWill, uint8_t willQos,
                            bool willRetain, bool hasUsername, bool hasPassword) {
    uint8_t flags = cleanSession ? 0x02U : 0;
    if (hasWill) {
        flags |= 0x04U | static_cast<uint8_t>(willQos << 3);
        if (willRetain) flags |= 0x20U;
    }
    if (hasUsername) flags |= 0x80U;
    if (hasPassword) flags |= 0x40U;
    return flags;
}

inline bool subackAccepted(const uint8_t* codes, size_t length) {
    if (!codes || length == 0) return false;
    for (size_t i = 0; i < length; ++i) {
        if (codes[i] == 0x80U || codes[i] > 1U) return false;
    }
    return true;
}

enum class PacketKind : uint8_t { NONE = 0, SUBSCRIBE, PUBLISH };

struct FixedPacket {
    PacketKind kind;
    Esp8266BaseMQTTPublishPriority priority;
    uint16_t packetId;
    uint16_t payloadLength;
    uint32_t sequence;
    uint8_t qos;
    bool retain;
    bool dup;
    bool sent;
    char topic[ESP8266BASE_MQTT_MAX_TOPIC_BYTES + 1];
    uint8_t payload[ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES];
};

class FixedOutbox {
public:
    FixedOutbox() { clear(); }

    bool enqueuePublish(uint16_t packetId, const char* topic, uint8_t qos, bool retain,
                        const uint8_t* payload, size_t length,
                        Esp8266BaseMQTTPublishPriority priority) {
        if (!validTopic(topic) || qos > 1 || length > ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES ||
            (length > 0 && !payload)) return false;
        FixedPacket* slot = freeSlot();
        if (!slot) return false;
        init(*slot, PacketKind::PUBLISH, packetId, topic, qos, priority);
        slot->retain = retain;
        slot->payloadLength = static_cast<uint16_t>(length);
        if (length > 0) memcpy(slot->payload, payload, length);
        return true;
    }

    bool enqueueSubscribe(uint16_t packetId, const char* topic, uint8_t qos) {
        if (!validTopic(topic) || qos > 1) return false;
        FixedPacket* slot = freeSlot();
        if (!slot) return false;
        init(*slot, PacketKind::SUBSCRIBE, packetId, topic, qos,
             Esp8266BaseMQTTPublishPriority::CRITICAL);
        return true;
    }

    FixedPacket* nextToSend() {
        if (_inFlight >= 0) return nullptr;
        int selected = -1;
        for (uint8_t i = 0; i < ESP8266BASE_MQTT_TX_SLOTS; ++i) {
            if (_slots[i].kind == PacketKind::NONE || _slots[i].sent) continue;
            if (selected < 0 || before(_slots[i], _slots[selected])) selected = i;
        }
        return selected < 0 ? nullptr : &_slots[selected];
    }

    void markSent(FixedPacket* packet) {
        if (!packet) return;
        const int index = static_cast<int>(packet - _slots);
        if (index < 0 || index >= ESP8266BASE_MQTT_TX_SLOTS) return;
        packet->sent = true;
        if (packet->qos == 0) {
            release(static_cast<uint8_t>(index));
        } else {
            _inFlight = static_cast<int8_t>(index);
        }
    }

    bool acknowledge(PacketKind kind, uint16_t packetId) {
        if (_inFlight < 0) return false;
        FixedPacket& packet = _slots[_inFlight];
        if (packet.kind != kind || packet.packetId != packetId) return false;
        release(static_cast<uint8_t>(_inFlight));
        _inFlight = -1;
        return true;
    }

    const FixedPacket* inFlight() const {
        return _inFlight < 0 ? nullptr : &_slots[_inFlight];
    }

    size_t size() const {
        size_t used = 0;
        for (uint8_t i = 0; i < ESP8266BASE_MQTT_TX_SLOTS; ++i) {
            if (_slots[i].kind != PacketKind::NONE) ++used;
        }
        return used;
    }

    void clear() {
        for (uint8_t i = 0; i < ESP8266BASE_MQTT_TX_SLOTS; ++i) release(i);
        _inFlight = -1;
        _sequence = 0;
    }

    void prepareReconnect(bool cleanSession) {
        if (cleanSession) return clear();
        _inFlight = -1;
        for (uint8_t i = 0; i < ESP8266BASE_MQTT_TX_SLOTS; ++i) {
            FixedPacket& packet = _slots[i];
            if (packet.kind != PacketKind::PUBLISH || packet.qos == 0) {
                release(i);
                continue;
            }
            packet.sent = false;
            packet.dup = true;
        }
    }

private:
    FixedPacket _slots[ESP8266BASE_MQTT_TX_SLOTS];
    int8_t _inFlight;
    uint32_t _sequence;

    static bool validTopic(const char* topic) {
        if (!topic || !topic[0]) return false;
        const size_t length = strlen(topic);
        return length <= ESP8266BASE_MQTT_MAX_TOPIC_BYTES;
    }

    FixedPacket* freeSlot() {
        for (uint8_t i = 0; i < ESP8266BASE_MQTT_TX_SLOTS; ++i) {
            if (_slots[i].kind == PacketKind::NONE) return &_slots[i];
        }
        return nullptr;
    }

    void init(FixedPacket& packet, PacketKind kind, uint16_t packetId,
              const char* topic, uint8_t qos,
              Esp8266BaseMQTTPublishPriority priority) {
        release(static_cast<uint8_t>(&packet - _slots));
        packet.kind = kind;
        packet.priority = priority;
        packet.packetId = packetId;
        packet.qos = qos;
        packet.sequence = ++_sequence;
        memcpy(packet.topic, topic, strlen(topic) + 1);
    }

    static bool before(const FixedPacket& left, const FixedPacket& right) {
        if (left.priority != right.priority) {
            return static_cast<uint8_t>(left.priority) < static_cast<uint8_t>(right.priority);
        }
        return static_cast<int32_t>(left.sequence - right.sequence) < 0;
    }

    void release(uint8_t index) {
        FixedPacket& packet = _slots[index];
        packet.kind = PacketKind::NONE;
        packet.priority = Esp8266BaseMQTTPublishPriority::STATE;
        packet.packetId = 0;
        packet.payloadLength = 0;
        packet.sequence = 0;
        packet.qos = 0;
        packet.retain = false;
        packet.dup = false;
        packet.sent = false;
        packet.topic[0] = '\0';
    }
};

inline bool millisDue(uint32_t now, uint32_t due) {
    return static_cast<int32_t>(now - due) >= 0;
}

}  // namespace Esp8266BaseMQTTInternal
