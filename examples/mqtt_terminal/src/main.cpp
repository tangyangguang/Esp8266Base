#include <Arduino.h>
#include <BearSSLHelpers.h>
#include "Esp8266Base.h"

// 示例值不能直接用于部署。业务项目应从自己的构建期私有配置或 Config 中加载，
// 并在 Esp8266Base::begin() 前调用 configure()；不要把真实凭据提交到仓库。
static const char MQTT_HOST[] = "mqtt.example.invalid";
static const char MQTT_CLIENT_ID[] = "replace-me-terminal";
static const char MQTT_SUBSCRIBE_TOPIC[] = "esp8266base/example";
static const char MQTT_AVAILABILITY_TOPIC[] = "esp8266base/example/availability";
static const char MQTT_LWT_PAYLOAD[] = "unexpected_disconnect";
static const char MQTT_SHUTDOWN_PAYLOAD[] = "shutdown";

// DigiCert Global Root G2 是公开 trust anchor，不是设备凭据；保存在 Flash。
static const char MQTT_ROOT_CA[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----)CERT";

static BearSSL::X509List mqttTrustAnchor(MQTT_ROOT_CA);
static uint16_t pendingSubscribePacketId = 0;

static void onMqttConnected(bool sessionPresent) {
    ESP8266BASE_LOG_I("App ", "mqtt_connected session_present=%s action=subscribe",
                      sessionPresent ? "yes" : "no");
    pendingSubscribePacketId = Esp8266BaseMQTT::subscribe(MQTT_SUBSCRIBE_TOPIC, 1);
    if (pendingSubscribePacketId == 0) {
        ESP8266BASE_LOG_E("App ", "mqtt_subscribe_failed");
        Esp8266BaseMQTT::requestReconnect();
    }
}

static void onMqttDisconnected(Esp8266BaseMQTTDisconnectReason reason) {
    ESP8266BASE_LOG_W("App ", "mqtt_disconnected reason=%u", (unsigned)reason);
}

static void onMqttSubscribeAck(uint16_t packetId, const uint8_t* returnCodes, size_t length) {
    const bool accepted = packetId == pendingSubscribePacketId &&
                          returnCodes != nullptr && length == 1 && returnCodes[0] == 1U;
    ESP8266BASE_LOG_I("App ", "mqtt_suback packet_id=%u accepted=%s count=%u",
                      (unsigned)packetId, accepted ? "yes" : "no", (unsigned)length);
    if (accepted) {
        if (!Esp8266BaseMQTT::markConnectionReady()) {
            Esp8266BaseMQTT::requestReconnect();
        }
    } else {
        Esp8266BaseMQTT::requestReconnect();
    }
}

static void onMqttPublishAck(uint16_t packetId) {
    ESP8266BASE_LOG_I("App ", "mqtt_publish_ack packet_id=%u", (unsigned)packetId);
}

static void onMqttClientError(uint16_t packetId, Esp8266BaseMQTTClientError error) {
    ESP8266BASE_LOG_E("App ", "mqtt_client_error packet_id=%u error=%u",
                      (unsigned)packetId, (unsigned)error);
}

static void onMqttMessage(uint8_t qos, bool dup, bool retain, uint16_t packetId,
                          const char* topic, const uint8_t* payload,
                          size_t len, size_t index, size_t total) {
    (void)payload;
    ESP8266BASE_LOG_I("App ", "mqtt_message topic=%s qos=%u dup=%s retain=%s packet_id=%u chunk=%u+%u/%u",
                      topic ? topic : "", (unsigned)qos, dup ? "yes" : "no",
                      retain ? "yes" : "no", (unsigned)packetId,
                      (unsigned)index, (unsigned)len, (unsigned)total);
}

static bool onOtaPrepare(char* reason, size_t reasonLen) {
    // 真实业务可在这里先让执行器进入安全状态，再构造自己的 availability payload。
    // 基础库不解析 topic 或 payload，只确认这条 retained QoS1 消息的 PUBACK。
    if (Esp8266BaseMQTT::beginShutdown(MQTT_AVAILABILITY_TOPIC,
                                       MQTT_SHUTDOWN_PAYLOAD)) {
        return true;
    }
    if (reason && reasonLen > 0) {
        strncpy(reason, "MQTT controlled shutdown failed", reasonLen - 1);
        reason[reasonLen - 1] = '\0';
    }
    return false;
}

static void onOtaFailure(Esp8266BaseOTAFailure failure) {
    // 回调前基础库已经显式恢复 MQTT 连接许可。
    ESP8266BASE_LOG_E("App ", "ota_failed reason=%u mqtt_shutdown=%s",
                      (unsigned)failure, Esp8266BaseMQTT::shutdownResultName());
}

void setup() {
    Serial.begin(115200);
    Esp8266Base::setFirmwareInfo("mqtt-terminal-example", "1.0.0");

    Esp8266BaseMQTTConfig config = {};
    config.host = MQTT_HOST;
    config.port = 8883;
    config.clientId = MQTT_CLIENT_ID;
    config.keepAliveSeconds = 30;
    config.cleanSession = true;
    config.trustAnchors = &mqttTrustAnchor;
    config.willTopic = MQTT_AVAILABILITY_TOPIC;
    config.willPayload = reinterpret_cast<const uint8_t*>(MQTT_LWT_PAYLOAD);
    config.willPayloadLength = strlen(MQTT_LWT_PAYLOAD);
    config.willQos = 1;
    config.willRetain = true;
    Esp8266BaseMQTT::configure(config);
    Esp8266BaseMQTT::setCallbacks(onMqttConnected, onMqttDisconnected, onMqttMessage,
                                  onMqttSubscribeAck, onMqttPublishAck, onMqttClientError);
    Esp8266BaseOTA::setLifecycleCallbacks(onOtaPrepare, onOtaFailure);

    Esp8266Base::begin();
}

void loop() {
    Esp8266Base::handle();
}
