#pragma once
#include <stddef.h>
#include <stdint.h>

// Fixed RTC snapshot. Only code-like addresses are retained from the stack,
// never arbitrary stack contents (which can include credentials).
struct Esp8266BaseCrashSnapshot {
    uint32_t magic;
    uint32_t checksum;
    uint32_t buildId[2];
    uint32_t bootCount;
    uint32_t resetReason;
    uint32_t exceptionCause;
    uint32_t epc1;
    uint32_t epc2;
    uint32_t epc3;
    uint32_t excvaddr;
    uint32_t depc;
    uint32_t phase;
    uint32_t stackPointer;
    uint32_t frameCount;
    uint32_t archived;
    uint32_t frames[12];
};
static_assert(sizeof(Esp8266BaseCrashSnapshot) == 112, "bounded crash RTC layout");

inline uint32_t esp8266CrashChecksum(const Esp8266BaseCrashSnapshot& value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    uint32_t crc = 0xffffffffU;
    for (size_t n = 8; n < sizeof(value); ++n) {
        crc ^= bytes[n];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
inline bool esp8266CrashValid(const Esp8266BaseCrashSnapshot& value) {
    return value.magic == 0x43525331U && value.frameCount <= 12 && value.archived <= 1 &&
        value.checksum == esp8266CrashChecksum(value);
}
inline bool esp8266CrashCodeAddress(uint32_t value) {
    return (value >= 0x40000000U && value < 0x40010000U) ||
           (value >= 0x40100000U && value < 0x40110000U) ||
           (value >= 0x40200000U && value < 0x40400000U);
}
