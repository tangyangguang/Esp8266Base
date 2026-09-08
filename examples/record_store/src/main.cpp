#include <Arduino.h>
#include "Esp8266Base.h"

// 一个 8B 固定编码事实；真实应用自行定义版本/类型/平台序号，不持久化 C++ 对象。
static const Esp8266BaseRecordStoreConfig storeConfig = {8, 32, 4, 16384};
static uint64_t lastReadId = 0;

void setup() {
    Serial.begin(115200);
    Esp8266Base::setFirmwareInfo("record-store-example", "1.0.0");
    Esp8266Base::begin();
    if (!Esp8266BaseRecordStore::begin(storeConfig)) {
        Serial.printf("Store not ready, result=%u; no automatic rebuild.\n",
                      unsigned(Esp8266BaseRecordStore::lastResult()));
    }
    Serial.println("I: explicitly ERASE/rebuild Store; A: append; N: read next; R: release through last read; C: checkpoint");
}
void loop() {
    Esp8266Base::handle();
    if (!Serial.available()) return;
    const char command = Serial.read();
    uint8_t bytes[16] = {};
    uint64_t id = 0;
    bool ok = false;
    switch (command) {
        case 'I':
            // 仅操作者明确输入 I 时重建；只清理 Store 专属文件，不能用于静默修复。
            ESP.random(bytes, sizeof(bytes));
            ok = Esp8266BaseRecordStore::rebuild(storeConfig, bytes);
            if (ok) lastReadId = 0;
            break;
        case 'A': {
            const uint32_t now = millis();
            bytes[0] = 1; // 本示例的编码版本
            for (uint8_t i = 0; i < 4; ++i) bytes[4+i] = now >> (8*i);
            ok = Esp8266BaseRecordStore::append(bytes, 8, id);
            break;
        }
        case 'N':
            ok = Esp8266BaseRecordStore::readNext(lastReadId, id, bytes, sizeof(bytes));
            if (ok) lastReadId = id;
            break;
        case 'R':
            // 只演示调用；真实 SDK 必须等待平台业务确认，MQTT PUBACK 不足以释放。
            ok = Esp8266BaseRecordStore::releaseThrough(lastReadId);
            break;
        case 'C': ok = Esp8266BaseRecordStore::checkpoint(); break;
        default: return;
    }
    Serial.printf("command=%c ok=%u result=%u id=%llu\n", command, unsigned(ok),
                  unsigned(Esp8266BaseRecordStore::lastResult()), id);
}
