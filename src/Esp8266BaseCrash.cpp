#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_CRASH
#include "Esp8266BaseCrash.h"
#include <Arduino.h>
extern "C" {
#include <user_interface.h>
}

namespace {
constexpr uint32_t RTC_WORD = 72;
Esp8266BaseCrashSnapshot snapshot{};
uint32_t buildId[2]{};
uint32_t bootCount = 0;
volatile uint8_t phase = 0;
bool valid = false;
}
void Esp8266BaseCrash::setBuildId(uint32_t first, uint32_t second) {
    buildId[0] = first; buildId[1] = second;
}
void Esp8266BaseCrash::begin(uint32_t count) {
    bootCount = count;
    valid = system_rtc_mem_read(RTC_WORD, &snapshot, sizeof(snapshot)) &&
        esp8266CrashValid(snapshot);
}
void Esp8266BaseCrash::setPhase(uint8_t value) { phase = value; }
uint8_t Esp8266BaseCrash::currentPhase() { return phase; }
const Esp8266BaseCrashSnapshot* Esp8266BaseCrash::last() { return valid ? &snapshot : nullptr; }
bool Esp8266BaseCrash::pending() { return valid && !snapshot.archived; }
bool Esp8266BaseCrash::markArchived() {
    if (!valid) return false;
    snapshot.archived = 1;
    snapshot.checksum = esp8266CrashChecksum(snapshot);
    if (system_rtc_mem_write(RTC_WORD, &snapshot, sizeof(snapshot))) return true;
    snapshot.archived = 0;
    snapshot.checksum = esp8266CrashChecksum(snapshot);
    return false;
}

// Called by Arduino Core after collecting reset registers. This path must not
// allocate, format strings, write Flash, yield, or touch the network.
extern "C" void custom_crash_callback(struct rst_info* info, uint32_t stack, uint32_t stackEnd) {
    if (!info) return;
    snapshot = {};
    snapshot.magic = 0x43525331U;
    snapshot.buildId[0] = buildId[0]; snapshot.buildId[1] = buildId[1];
    snapshot.bootCount = bootCount;
    snapshot.resetReason = info->reason;
    snapshot.exceptionCause = info->exccause;
    snapshot.epc1 = info->epc1; snapshot.epc2 = info->epc2; snapshot.epc3 = info->epc3;
    snapshot.excvaddr = info->excvaddr; snapshot.depc = info->depc;
    snapshot.phase = phase;
    snapshot.stackPointer = stack;
    // Bound the scan to 64 aligned DRAM words and retain at most 12 candidate
    // return addresses. These are not asserted to be an unwound call stack.
    if ((stack & 3U) == 0 && stack >= 0x3ffe8000U && stack < 0x40000000U &&
        stackEnd >= stack && stackEnd <= 0x40000000U) {
        for (unsigned n = 0; n < 64 && stack + (n + 1U) * 4U <= stackEnd && snapshot.frameCount < 12; ++n) {
            const uint32_t word = *reinterpret_cast<const volatile uint32_t*>(stack + n * 4U);
            if (esp8266CrashCodeAddress(word)) snapshot.frames[snapshot.frameCount++] = word;
        }
    }
    snapshot.checksum = esp8266CrashChecksum(snapshot);
    system_rtc_mem_write(RTC_WORD, &snapshot, sizeof(snapshot));
}
#endif
