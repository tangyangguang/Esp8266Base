#pragma once
#include "Esp8266BaseCrashData.h"

// ESP8266 SDK crash hook + RTC only. No filesystem/network/actuator dependency.
// RTC words 72..99; Watchdog owns 64..70. Applications may use 104..191.
class Esp8266BaseCrash {
public:
    static void setBuildId(uint32_t first, uint32_t second);
    static void begin(uint32_t bootCount);
    static void setPhase(uint8_t phase);
    static uint8_t currentPhase();
    static const Esp8266BaseCrashSnapshot* last();
    static bool pending();
    // Call only after the consumer has durably stored the report. Retains the
    // last snapshot for local diagnostics; stops repeated uploads on reboot.
    static bool markArchived();
};
class Esp8266BaseCrashPhase {
public:
    explicit Esp8266BaseCrashPhase(uint8_t phase) : previous_(Esp8266BaseCrash::currentPhase()) {
        Esp8266BaseCrash::setPhase(phase);
    }
    ~Esp8266BaseCrashPhase() { Esp8266BaseCrash::setPhase(previous_); }
    Esp8266BaseCrashPhase(const Esp8266BaseCrashPhase&) = delete;
    Esp8266BaseCrashPhase& operator=(const Esp8266BaseCrashPhase&) = delete;
private:
    uint8_t previous_;
};
