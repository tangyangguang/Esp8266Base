#pragma once
#include <Arduino.h>

#ifndef ESP8266BASE_WDT_TIMEOUT_MS
#define ESP8266BASE_WDT_TIMEOUT_MS 2500UL
#endif

#ifndef ESP8266BASE_WDT_STALL_TIMEOUT_MS
#define ESP8266BASE_WDT_STALL_TIMEOUT_MS 90000UL
#endif

#ifndef ESP8266BASE_RECOVERY_RESTART_COOLDOWN_MS
#define ESP8266BASE_RECOVERY_RESTART_COOLDOWN_MS 3600000UL
#endif

#ifndef ESP8266BASE_RECOVERY_HEALTHY_RESET_MS
#define ESP8266BASE_RECOVERY_HEALTHY_RESET_MS 1800000UL
#endif

#ifndef ESP8266BASE_RECOVERY_BUDGET_WINDOW_SECONDS
#define ESP8266BASE_RECOVERY_BUDGET_WINDOW_SECONDS 86400UL
#endif

enum class Esp8266BaseWatchdogPhase : uint8_t {
    LOOP = 0,
    CONFIG = 1,
    WIFI = 2,
    JOURNAL = 3,
    NTP = 4,
    MDNS = 5,
    WEB = 6,
    OTA = 7,
    MQTT = 8,
};

enum class Esp8266BaseRecoveryCause : uint8_t {
    NONE = 0,
    LOOP_STALL = 1,
    NETWORK_UNRECOVERABLE = 2,
};

enum class Esp8266BaseRestartDecision : uint8_t {
    RESTARTING = 0,
    BLOCKED_BY_GUARD = 1,
    BLOCKED_BY_COOLDOWN = 2,
    BLOCKED_BY_BUDGET = 3,
    NOT_RUNNING = 4,
};

typedef void (*Esp8266BaseEmergencyStopCallback)();
typedef bool (*Esp8266BaseRestartGuardCallback)();

struct Esp8266BaseRestartDecisionReport {
    uint32_t serial = 0;
    Esp8266BaseRecoveryCause cause = Esp8266BaseRecoveryCause::NONE;
    Esp8266BaseRestartDecision decision = Esp8266BaseRestartDecision::NOT_RUNNING;
    Esp8266BaseWatchdogPhase phase = Esp8266BaseWatchdogPhase::LOOP;
    bool emergencyStopApplied = false;
};

// Cooperative-loop monitor plus an independent SDK timer. The timer catches
// yielding calls which never return to loop(); normal over-budget loops are
// diagnosed at the end of the same cycle. All restarts share one RTC-backed,
// bounded policy.
class Esp8266BaseWatchdog {
public:
    static void setSafetyCallbacks(Esp8266BaseEmergencyStopCallback emergencyStop,
                                   Esp8266BaseRestartGuardCallback restartGuard);
    static bool begin(uint32_t timeoutMs = ESP8266BASE_WDT_TIMEOUT_MS);
    static void cycleStart();
    static void setPhase(Esp8266BaseWatchdogPhase phase);
    static void account(uint32_t maxBlockMs, uint32_t startedAtMs);
    static void handle();
    static void feed();
    static void pause();
    static void resume();
    // application-ready 必须同时具备传输、必需订阅与初始证据。连续稳定
    // 30 分钟只打断连续重启链；可信 UTC 下的 24 小时预算独立保留。
    static void setApplicationReady(bool ready);

    // NETWORK_UNRECOVERABLE obeys restartGuard. LOOP_STALL always invokes the
    // emergency-stop callback before evaluating the restart budget.
    static Esp8266BaseRestartDecision requestRestart(Esp8266BaseRecoveryCause cause);
    static bool restartDecisionReport(uint32_t afterSerial,
                                      Esp8266BaseRestartDecisionReport& report);

    static bool isRunning();
    static bool isPaused();
    static bool wasWatchdogReset();
    static uint32_t resetCount();
    static void clearResetCount();
    static Esp8266BaseRecoveryCause lastRecoveryCause();
    static Esp8266BaseWatchdogPhase lastStallPhase();
    static bool lastRecoveryWasDenied();
    static Esp8266BaseRestartDecision lastRecoveryDecision();
    static uint8_t consecutiveRecoveryRestarts();
    static uint8_t recoveryRestartsInWindow();
    static bool restartBudgetAvailable();

private:
    static void _timerCallback(void*);
    static Esp8266BaseRestartDecision _decision(Esp8266BaseRecoveryCause cause, uint32_t nowMs);
    static void _persistMarker(Esp8266BaseRecoveryCause cause, Esp8266BaseWatchdogPhase phase,
                               bool denied,
                               Esp8266BaseRestartDecision decision =
                                   Esp8266BaseRestartDecision::NOT_RUNNING);
    static void _restartNow(Esp8266BaseRecoveryCause cause, Esp8266BaseWatchdogPhase phase,
                            bool asynchronous);
    static bool _publishDecision(Esp8266BaseRecoveryCause cause,
                                 Esp8266BaseRestartDecision decision,
                                 Esp8266BaseWatchdogPhase phase,
                                 bool emergencyStopApplied);

    static bool _running;
    static bool _paused;
    static bool _monitorArmed;
    static bool _wasWdtReset;
    static uint32_t _timeoutMs;
    static uint32_t _lastFeedMs;
    static uint32_t _cycleStartMs;
    static uint32_t _coveredMs;
    static uint32_t _resetCount;
    static uint32_t _startedMs;
    static uint32_t _applicationReadySinceMs;
    static uint32_t _budgetWindowStartedEpoch;
    static uint32_t _trustedEpoch;
    static volatile uint32_t _heartbeat;
    static volatile uint32_t _timerHeartbeat;
    static volatile uint16_t _stalledTicks;
    static volatile uint8_t _phase;
    static uint8_t _consecutiveRestarts;
    static uint8_t _restartsInWindow;
    static bool _applicationReady;
    static bool _lastRecoveryDenied;
    static Esp8266BaseRestartDecisionReport _lastDecisionReport;
    static Esp8266BaseRestartDecision _lastRecoveryDecision;
    static Esp8266BaseRecoveryCause _lastCause;
    static Esp8266BaseWatchdogPhase _lastPhase;
    static Esp8266BaseEmergencyStopCallback _emergencyStop;
    static Esp8266BaseRestartGuardCallback _restartGuard;
};
