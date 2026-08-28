#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/Esp8266BaseMQTTFixed.h"

using namespace Esp8266BaseMQTTInternal;

static void fill(char* value, size_t length, char byte) {
    memset(value, byte, length);
    value[length] = '\0';
}

int main() {
    FixedOutbox queue;
    const uint8_t one[] = {1};

    assert(queue.enqueuePublish(10, "runtime", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::STATE));
    assert(queue.enqueuePublish(11, "overview", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::STATE));
    assert(!queue.enqueuePublish(12, "overflow", 1, false, one, sizeof(one),
                                 Esp8266BaseMQTTPublishPriority::STATE));
    FixedPacket* runtime = queue.nextToSend();
    assert(runtime && runtime->packetId == 10);
    queue.markSent(runtime);
    assert(queue.nextToSend() == nullptr);
    assert(!queue.acknowledge(PacketKind::PUBLISH, 99));
    assert(queue.nextToSend() == nullptr);
    assert(queue.acknowledge(PacketKind::PUBLISH, 10));
    FixedPacket* overview = queue.nextToSend();
    assert(overview && overview->packetId == 11);

    queue.clear();
    assert(queue.enqueuePublish(20, "state", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::STATE));
    assert(queue.enqueuePublish(21, "receipt", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::EVIDENCE));
    assert(queue.nextToSend()->packetId == 21);

    queue.clear();
    assert(queue.enqueuePublish(22, "runtime", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::STATE));
    FixedPacket* active = queue.nextToSend();
    queue.markSent(active);
    assert(queue.enqueuePublish(23, "receipt", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::CRITICAL));
    assert(queue.nextToSend() == nullptr);
    assert(queue.acknowledge(PacketKind::PUBLISH, 22));
    assert(queue.nextToSend()->packetId == 23);

    queue.clear();
    assert(queue.enqueueSubscribe(30, "command", 1));
    FixedPacket* subscribe = queue.nextToSend();
    queue.markSent(subscribe);
    assert(!queue.acknowledge(PacketKind::SUBSCRIBE, 31));
    assert(queue.acknowledge(PacketKind::SUBSCRIBE, 30));

    const uint8_t subackOk[] = {1};
    const uint8_t subackDenied[] = {0x80};
    const uint8_t subackInvalid[] = {2};
    assert(subackAccepted(subackOk, sizeof(subackOk)));
    assert(!subackAccepted(subackDenied, sizeof(subackDenied)));
    assert(!subackAccepted(subackInvalid, sizeof(subackInvalid)));
    assert(!subackAccepted(nullptr, 0));

    queue.clear();
    assert(queue.enqueuePublish(40, "state", 1, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::STATE));
    queue.markSent(queue.nextToSend());
    queue.prepareReconnect(false);
    FixedPacket* retry = queue.nextToSend();
    assert(retry && retry->packetId == 40 && retry->dup);
    queue.prepareReconnect(true);
    assert(queue.size() == 0);

    queue.clear();
    assert(queue.enqueuePublish(0, "telemetry", 0, false, one, sizeof(one),
                                Esp8266BaseMQTTPublishPriority::STATE));
    queue.markSent(queue.nextToSend());
    assert(queue.size() == 0);

    char topic[ESP8266BASE_MQTT_MAX_TOPIC_BYTES + 2];
    fill(topic, ESP8266BASE_MQTT_MAX_TOPIC_BYTES, 't');
    uint8_t payload[ESP8266BASE_MQTT_MAX_PAYLOAD_BYTES];
    memset(payload, 7, sizeof(payload));
    assert(queue.enqueuePublish(50, topic, 1, false, payload, sizeof(payload),
                                Esp8266BaseMQTTPublishPriority::STATE));
    queue.clear();
    topic[ESP8266BASE_MQTT_MAX_TOPIC_BYTES] = 'x';
    topic[ESP8266BASE_MQTT_MAX_TOPIC_BYTES + 1] = '\0';
    assert(!queue.enqueuePublish(51, topic, 1, false, one, sizeof(one),
                                 Esp8266BaseMQTTPublishPriority::STATE));
    assert(!queue.enqueuePublish(52, "state", 1, false, payload,
                                 sizeof(payload) + 1U,
                                 Esp8266BaseMQTTPublishPriority::STATE));

    assert(validWill("availability", one, sizeof(one), 1));
    assert(!validWill(nullptr, one, sizeof(one), 1));
    assert(!validWill("availability", one, sizeof(one), 2));
    assert(connectFlags(true, true, 1, true, true, true) == 0xeeU);

    assert(!millisDue(0xfffffff0U, 0x00000010U));
    assert(millisDue(0x00000020U, 0x00000010U));
    return 0;
}
