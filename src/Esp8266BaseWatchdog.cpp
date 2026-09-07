#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_WATCHDOG
#include "Esp8266BaseWatchdog.h"
#if ESP8266BASE_USE_CONFIG
#include "Esp8266BaseConfig.h"
#endif
#if ESP8266BASE_USE_JOURNAL
#include "Esp8266BaseJournal.h"
#endif
#if ESP8266BASE_USE_NTP
#include "Esp8266BaseNTP.h"
#endif
#include "Esp8266BaseLog.h"
#include <osapi.h>
#include <user_interface.h>

namespace {
constexpr uint32_t RTC_ADDR = 64;
constexpr uint32_t RTC_MAGIC = 0xEB0BDA7EUL;
constexpr uint32_t RTC_SALT = 0xA5C35A3CUL;
constexpr uint32_t MARKER_PENDING = 0x80000000UL;
constexpr uint32_t MARKER_DENIED = 0x40000000UL;
constexpr uint32_t TIMER_PERIOD_MS = 1000UL;
constexpr uint16_t STALL_TICKS = static_cast<uint16_t>(
    (ESP8266BASE_WDT_STALL_TIMEOUT_MS + TIMER_PERIOD_MS - 1U) / TIMER_PERIOD_MS);

struct WatchdogRtcState {
    uint32_t magic;
    uint32_t watchdogCount;
    uint32_t consecutiveRestarts;
    uint32_t marker;
    uint32_t budgetWindowStartedEpoch;
    uint32_t restartsInWindow;
    uint32_t checksum;
};

os_timer_t g_monitorTimer;

uint32_t checksum(const WatchdogRtcState& state) {
    return RTC_MAGIC ^ state.watchdogCount ^ state.consecutiveRestarts ^
           state.marker ^ state.budgetWindowStartedEpoch ^
           state.restartsInWindow ^ RTC_SALT;
}

bool readRtc(WatchdogRtcState& state) {
    if (!system_rtc_mem_read(RTC_ADDR, &state, sizeof(state))) return false;
    return state.magic == RTC_MAGIC && state.checksum == checksum(state);
}

bool writeRtc(uint32_t watchdogCount, uint32_t consecutiveRestarts,
              uint32_t marker, uint32_t budgetWindowStartedEpoch,
              uint32_t restartsInWindow) {
    WatchdogRtcState state = {RTC_MAGIC, watchdogCount, consecutiveRestarts,
                              marker, budgetWindowStartedEpoch,
                              restartsInWindow, 0};
    state.checksum = checksum(state);
    return system_rtc_mem_write(RTC_ADDR, &state, sizeof(state));
}

uint32_t markerFor(Esp8266BaseRecoveryCause cause,
                   Esp8266BaseWatchdogPhase phase, bool denied,
                   Esp8266BaseRestartDecision decision) {
    return (denied ? MARKER_DENIED : MARKER_PENDING) |
           static_cast<uint32_t>(static_cast<uint8_t>(cause)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(phase)) << 8U) |
           (static_cast<uint32_t>(static_cast<uint8_t>(decision)) << 16U);
}
}  // namespace

bool Esp8266BaseWatchdog::_running = false;
bool Esp8266BaseWatchdog::_paused = false;
bool Esp8266BaseWatchdog::_monitorArmed = false;
bool Esp8266BaseWatchdog::_wasWdtReset = false;
uint32_t Esp8266BaseWatchdog::_timeoutMs = ESP8266BASE_WDT_TIMEOUT_MS;
uint32_t Esp8266BaseWatchdog::_lastFeedMs = 0;
uint32_t Esp8266BaseWatchdog::_cycleStartMs = 0;
uint32_t Esp8266BaseWatchdog::_coveredMs = 0;
uint32_t Esp8266BaseWatchdog::_resetCount = 0;
uint32_t Esp8266BaseWatchdog::_startedMs = 0;
uint32_t Esp8266BaseWatchdog::_applicationReadySinceMs = 0;
uint32_t Esp8266BaseWatchdog::_budgetWindowStartedEpoch = 0;
uint32_t Esp8266BaseWatchdog::_trustedEpoch = 0;
volatile uint32_t Esp8266BaseWatchdog::_heartbeat = 0;
volatile uint32_t Esp8266BaseWatchdog::_timerHeartbeat = 0;
volatile uint16_t Esp8266BaseWatchdog::_stalledTicks = 0;
volatile uint8_t Esp8266BaseWatchdog::_phase = static_cast<uint8_t>(Esp8266BaseWatchdogPhase::LOOP);
uint8_t Esp8266BaseWatchdog::_consecutiveRestarts = 0;
uint8_t Esp8266BaseWatchdog::_restartsInWindow = 0;
bool Esp8266BaseWatchdog::_applicationReady = false;
bool Esp8266BaseWatchdog::_lastRecoveryDenied = false;
Esp8266BaseRestartDecisionReport Esp8266BaseWatchdog::_lastDecisionReport = {};
Esp8266BaseRestartDecision Esp8266BaseWatchdog::_lastRecoveryDecision =
    Esp8266BaseRestartDecision::NOT_RUNNING;
Esp8266BaseRecoveryCause Esp8266BaseWatchdog::_lastCause = Esp8266BaseRecoveryCause::NONE;
Esp8266BaseWatchdogPhase Esp8266BaseWatchdog::_lastPhase = Esp8266BaseWatchdogPhase::LOOP;
Esp8266BaseEmergencyStopCallback Esp8266BaseWatchdog::_emergencyStop = nullptr;
Esp8266BaseRestartGuardCallback Esp8266BaseWatchdog::_restartGuard = nullptr;

void Esp8266BaseWatchdog::setSafetyCallbacks(Esp8266BaseEmergencyStopCallback emergencyStop,
                                              Esp8266BaseRestartGuardCallback restartGuard) {
    _emergencyStop = emergencyStop;
    _restartGuard = restartGuard;
}

bool Esp8266BaseWatchdog::begin(uint32_t timeoutMs) {
    if (timeoutMs < 1000U) timeoutMs = 1000U;
    if (timeoutMs > 3000U) timeoutMs = 3000U;
    _timeoutMs = timeoutMs;
#if ESP8266BASE_USE_CONFIG
    _resetCount = static_cast<uint32_t>(Esp8266BaseConfig::getInt(ESP8266BASE_CFG_KEY_WDT_COUNT));
#else
    _resetCount = 0;
#endif

    WatchdogRtcState rtc = {};
    const bool rtcValid = readRtc(rtc);
    const bool pending = rtcValid && (rtc.marker & MARKER_PENDING) != 0;
    const bool denied = rtcValid && (rtc.marker & MARKER_DENIED) != 0;
    if (rtcValid && rtc.watchdogCount > _resetCount) _resetCount = rtc.watchdogCount;

    _lastCause = (pending || denied)
        ? static_cast<Esp8266BaseRecoveryCause>(rtc.marker & 0xFFU)
        : Esp8266BaseRecoveryCause::NONE;
    _lastPhase = (pending || denied)
        ? static_cast<Esp8266BaseWatchdogPhase>((rtc.marker >> 8U) & 0xFFU)
        : Esp8266BaseWatchdogPhase::LOOP;
    _wasWdtReset = pending && _lastCause == Esp8266BaseRecoveryCause::LOOP_STALL;
    _lastRecoveryDenied = denied;
    _lastRecoveryDecision = denied
        ? static_cast<Esp8266BaseRestartDecision>((rtc.marker >> 16U) & 0xFFU)
        : Esp8266BaseRestartDecision::RESTARTING;
    // External/manual reset breaks only the consecutive chain. The trusted
    // 24-hour count remains when RTC memory survived that reset; power loss may
    // clear RTC and is intentionally not treated as an automatic restart.
    _consecutiveRestarts = pending && rtc.consecutiveRestarts <= 0xFFU
        ? static_cast<uint8_t>(rtc.consecutiveRestarts) : 0;
    _budgetWindowStartedEpoch = rtcValid ? rtc.budgetWindowStartedEpoch : 0;
    _restartsInWindow = rtcValid && rtc.restartsInWindow <= 0xFFU
        ? static_cast<uint8_t>(rtc.restartsInWindow) : 0;

#if ESP8266BASE_USE_CONFIG
    if (rtcValid && rtc.watchdogCount > static_cast<uint32_t>(
            Esp8266BaseConfig::getInt(ESP8266BASE_CFG_KEY_WDT_COUNT)))
        Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT, static_cast<int>(_resetCount));
#endif
    writeRtc(_resetCount, _consecutiveRestarts, 0,
             _budgetWindowStartedEpoch, _restartsInWindow);

    _startedMs = millis();
    _lastFeedMs = _startedMs;
    _cycleStartMs = _startedMs;
    _coveredMs = 0;
    _heartbeat = 0;
    _timerHeartbeat = 0;
    _stalledTicks = 0;
    _monitorArmed = false;
    _paused = false;
    _applicationReady = false;
    _applicationReadySinceMs = 0;
    _trustedEpoch = 0;
    _lastDecisionReport = Esp8266BaseRestartDecisionReport{};
    _running = true;
    _phase = static_cast<uint8_t>(Esp8266BaseWatchdogPhase::LOOP);

    os_timer_disarm(&g_monitorTimer);
    os_timer_setfn(&g_monitorTimer, reinterpret_cast<os_timer_func_t*>(&_timerCallback), nullptr);
    os_timer_arm(&g_monitorTimer, TIMER_PERIOD_MS, true);

    ESP8266BASE_LOG_I_P("WDT ", "watchdog_ready loop_timeout=%ums stall_timeout=%lus resets=%lu consecutive=%u/2 window=%u/2 previous=%u phase=%u denied=%u",
                      static_cast<unsigned>(_timeoutMs),
                      static_cast<unsigned long>(ESP8266BASE_WDT_STALL_TIMEOUT_MS / 1000UL),
                      static_cast<unsigned long>(_resetCount),
                      static_cast<unsigned>(_consecutiveRestarts),
                      static_cast<unsigned>(_restartsInWindow),
                      static_cast<unsigned>(_lastCause), static_cast<unsigned>(_lastPhase),
                      _lastRecoveryDenied ? 1U : 0U);
    return true;
}

void Esp8266BaseWatchdog::cycleStart() {
    _cycleStartMs = millis();
    _coveredMs = 0;
    _phase = static_cast<uint8_t>(Esp8266BaseWatchdogPhase::LOOP);
    ++_heartbeat;
    _monitorArmed = true;
}

void Esp8266BaseWatchdog::setPhase(Esp8266BaseWatchdogPhase phase) {
    _phase = static_cast<uint8_t>(phase);
}

void Esp8266BaseWatchdog::account(uint32_t maxBlockMs, uint32_t startedAtMs) {
    uint32_t elapsed = millis() - startedAtMs;
    if (elapsed > maxBlockMs) elapsed = maxBlockMs;
    _coveredMs += elapsed;
    if (_coveredMs > 0xFFFF0000UL) _coveredMs = 0xFFFF0000UL;
}

void Esp8266BaseWatchdog::handle() {
    if (!_running || _paused) return;
    const uint32_t now = millis();
#if ESP8266BASE_USE_NTP
    if (Esp8266BaseNTP::isSynced()) {
        _trustedEpoch = Esp8266BaseNTP::timestamp();
        bool persistBudget = false;
        if (_budgetWindowStartedEpoch == 0 && _restartsInWindow != 0) {
            // Conservatively anchor automatic restarts that occurred before UTC
            // became trustworthy to the first trusted sample.
            _budgetWindowStartedEpoch = _trustedEpoch;
            persistBudget = true;
        } else if (_budgetWindowStartedEpoch != 0 &&
                   static_cast<uint32_t>(_trustedEpoch - _budgetWindowStartedEpoch) >=
                       ESP8266BASE_RECOVERY_BUDGET_WINDOW_SECONDS) {
            _budgetWindowStartedEpoch = _trustedEpoch;
            _restartsInWindow = 0;
            persistBudget = true;
        }
        if (persistBudget) {
            writeRtc(_resetCount, _consecutiveRestarts, 0,
                     _budgetWindowStartedEpoch, _restartsInWindow);
        }
    }
#endif
    if (_applicationReady && _consecutiveRestarts != 0 &&
        static_cast<uint32_t>(now - _applicationReadySinceMs) >=
            ESP8266BASE_RECOVERY_HEALTHY_RESET_MS) {
        _consecutiveRestarts = 0;
        writeRtc(_resetCount, 0, 0, _budgetWindowStartedEpoch,
                 _restartsInWindow);
        ESP8266BASE_LOG_I_P("WDT ", "recovery_consecutive_chain_reset application_ready_s=%lu window=%u/2",
                          static_cast<unsigned long>((now - _applicationReadySinceMs) / 1000UL),
                          static_cast<unsigned>(_restartsInWindow));
    }
    const uint32_t elapsed = now - _cycleStartMs;
    const uint32_t uncovered = elapsed > _coveredMs ? elapsed - _coveredMs : 0;
    if (uncovered < _timeoutMs) return;
#if ESP8266BASE_USE_JOURNAL
    Esp8266BaseJournal::recordNow(JNL_STALL, _phase,
        static_cast<int16_t>(uncovered / 1000UL > 0x7FFFU ? 0x7FFFU : uncovered / 1000UL), 0, 0);
#endif
    ESP8266BASE_LOG_E_P("WDT ", "loop_overrun uncovered=%lums elapsed=%lums covered=%lums phase=%u action=recovery_restart",
                      static_cast<unsigned long>(uncovered), static_cast<unsigned long>(elapsed),
                      static_cast<unsigned long>(_coveredMs), static_cast<unsigned>(_phase));
    requestRestart(Esp8266BaseRecoveryCause::LOOP_STALL);
}

void Esp8266BaseWatchdog::feed() { _lastFeedMs = millis(); }

void Esp8266BaseWatchdog::pause() {
    _paused = true;
    _stalledTicks = 0;
    ESP8266BASE_LOG_D_P("WDT ", "watchdog_paused");
}

void Esp8266BaseWatchdog::resume() {
    _lastFeedMs = millis();
    ++_heartbeat;
    _timerHeartbeat = _heartbeat;
    _stalledTicks = 0;
    _paused = false;
    ESP8266BASE_LOG_D_P("WDT ", "watchdog_resumed");
}

void Esp8266BaseWatchdog::setApplicationReady(bool ready) {
    if (_applicationReady == ready) return;
    _applicationReady = ready;
    _applicationReadySinceMs = ready ? millis() : 0;
}

Esp8266BaseRestartDecision Esp8266BaseWatchdog::_decision(Esp8266BaseRecoveryCause cause,
                                                           uint32_t nowMs) {
    if (!_running) return Esp8266BaseRestartDecision::NOT_RUNNING;
    if (cause == Esp8266BaseRecoveryCause::NETWORK_UNRECOVERABLE &&
        _restartGuard && !_restartGuard()) return Esp8266BaseRestartDecision::BLOCKED_BY_GUARD;
    if (_lastCause != Esp8266BaseRecoveryCause::NONE &&
        nowMs - _startedMs < ESP8266BASE_RECOVERY_RESTART_COOLDOWN_MS)
        return Esp8266BaseRestartDecision::BLOCKED_BY_COOLDOWN;
    if (_consecutiveRestarts >= 2U) return Esp8266BaseRestartDecision::BLOCKED_BY_BUDGET;
    if (_trustedEpoch != 0 && _budgetWindowStartedEpoch != 0 &&
        static_cast<uint32_t>(_trustedEpoch - _budgetWindowStartedEpoch) <
            ESP8266BASE_RECOVERY_BUDGET_WINDOW_SECONDS &&
        _restartsInWindow >= 2U)
        return Esp8266BaseRestartDecision::BLOCKED_BY_BUDGET;
    return Esp8266BaseRestartDecision::RESTARTING;
}

Esp8266BaseRestartDecision Esp8266BaseWatchdog::requestRestart(Esp8266BaseRecoveryCause cause) {
    const Esp8266BaseRestartDecision decision = _decision(cause, millis());
    if (cause == Esp8266BaseRecoveryCause::LOOP_STALL && _emergencyStop) _emergencyStop();
    if (decision != Esp8266BaseRestartDecision::RESTARTING) {
        const bool newDecision = _publishDecision(
            cause, decision, static_cast<Esp8266BaseWatchdogPhase>(_phase),
            cause == Esp8266BaseRecoveryCause::LOOP_STALL);
        (void)newDecision;
#if ESP8266BASE_USE_JOURNAL
        if (newDecision) {
            Esp8266BaseJournal::recordNow(JNL_RECOVERY, static_cast<uint8_t>(cause),
                                          static_cast<int16_t>(decision), 0, 0);
        }
#endif
        ESP8266BASE_LOG_E_P("WDT ", "recovery_restart_blocked cause=%u decision=%u budget=%u/2",
                          static_cast<unsigned>(cause), static_cast<unsigned>(decision),
                          static_cast<unsigned>(_consecutiveRestarts));
        return decision;
    }
    _restartNow(cause, static_cast<Esp8266BaseWatchdogPhase>(_phase), false);
    return Esp8266BaseRestartDecision::RESTARTING;
}

bool Esp8266BaseWatchdog::restartDecisionReport(
    uint32_t afterSerial, Esp8266BaseRestartDecisionReport& report) {
    const uint32_t serial = _lastDecisionReport.serial;
    if (serial == 0 || serial == afterSerial) return false;
    report = _lastDecisionReport;
    return report.serial == serial;
}

bool Esp8266BaseWatchdog::_publishDecision(
    Esp8266BaseRecoveryCause cause, Esp8266BaseRestartDecision decision,
    Esp8266BaseWatchdogPhase phase, bool emergencyStopApplied) {
    if (_lastDecisionReport.serial != 0 &&
        _lastDecisionReport.cause == cause &&
        _lastDecisionReport.decision == decision &&
        _lastDecisionReport.phase == phase &&
        _lastDecisionReport.emergencyStopApplied == emergencyStopApplied) return false;
    _lastDecisionReport.cause = cause;
    _lastDecisionReport.decision = decision;
    _lastDecisionReport.phase = phase;
    _lastDecisionReport.emergencyStopApplied = emergencyStopApplied;
    ++_lastDecisionReport.serial;
    if (_lastDecisionReport.serial == 0) ++_lastDecisionReport.serial;
    return true;
}

void Esp8266BaseWatchdog::_persistMarker(
    Esp8266BaseRecoveryCause cause, Esp8266BaseWatchdogPhase phase,
    bool denied, Esp8266BaseRestartDecision decision) {
    writeRtc(_resetCount, _consecutiveRestarts,
             markerFor(cause, phase, denied, decision),
             _budgetWindowStartedEpoch, _restartsInWindow);
}

void Esp8266BaseWatchdog::_restartNow(Esp8266BaseRecoveryCause cause,
                                      Esp8266BaseWatchdogPhase phase, bool asynchronous) {
    os_timer_disarm(&g_monitorTimer);
    _monitorArmed = false;
    if (_emergencyStop) _emergencyStop();
    if (_consecutiveRestarts < 0xFFU) ++_consecutiveRestarts;
    if (_restartsInWindow < 0xFFU) ++_restartsInWindow;
    if (_budgetWindowStartedEpoch == 0 && _trustedEpoch != 0)
        _budgetWindowStartedEpoch = _trustedEpoch;
    if (cause == Esp8266BaseRecoveryCause::LOOP_STALL && _resetCount < 0xFFFFFFFFUL) ++_resetCount;
    _persistMarker(cause, phase, false);
#if ESP8266BASE_USE_JOURNAL
    if (!asynchronous) Esp8266BaseJournal::recordNow(JNL_RECOVERY, static_cast<uint8_t>(cause),
                                                      static_cast<int16_t>(Esp8266BaseRestartDecision::RESTARTING), 0, 0);
#endif
    if (!asynchronous) {
        ESP8266BASE_LOG_E_P("WDT ", "recovery_restart cause=%u phase=%u consecutive=%u/2 window=%u/2",
                          static_cast<unsigned>(cause), static_cast<unsigned>(phase),
                          static_cast<unsigned>(_consecutiveRestarts),
                          static_cast<unsigned>(_restartsInWindow));
        delay(20);
    }
    system_restart();
}

void Esp8266BaseWatchdog::_timerCallback(void*) {
    if (!_running || _paused || !_monitorArmed) { _stalledTicks = 0; return; }
    const uint32_t heartbeat = _heartbeat;
    if (heartbeat != _timerHeartbeat) {
        _timerHeartbeat = heartbeat;
        _stalledTicks = 0;
        return;
    }
    if (_stalledTicks < 0xFFFFU) ++_stalledTicks;
    if (_stalledTicks < STALL_TICKS) return;

    os_timer_disarm(&g_monitorTimer);
    _monitorArmed = false;
    const Esp8266BaseWatchdogPhase phase = static_cast<Esp8266BaseWatchdogPhase>(_phase);
    if (_emergencyStop) _emergencyStop();
    const Esp8266BaseRestartDecision decision = _decision(Esp8266BaseRecoveryCause::LOOP_STALL,
                                                           millis());
    if (decision != Esp8266BaseRestartDecision::RESTARTING) {
        _publishDecision(Esp8266BaseRecoveryCause::LOOP_STALL, decision,
                         phase, true);
        _persistMarker(Esp8266BaseRecoveryCause::LOOP_STALL, phase, true,
                       decision);
        _timerHeartbeat = _heartbeat;
        _stalledTicks = 0;
        // A truly frozen loop remains disarmed and safely off. If it later
        // returns, cycleStart() re-arms monitoring without causing a restart storm.
        os_timer_arm(&g_monitorTimer, TIMER_PERIOD_MS, true);
        return;
    }
    _restartNow(Esp8266BaseRecoveryCause::LOOP_STALL, phase, true);
}

bool Esp8266BaseWatchdog::isRunning() { return _running; }
bool Esp8266BaseWatchdog::isPaused() { return _paused; }
bool Esp8266BaseWatchdog::wasWatchdogReset() { return _wasWdtReset; }
uint32_t Esp8266BaseWatchdog::resetCount() { return _resetCount; }
Esp8266BaseRecoveryCause Esp8266BaseWatchdog::lastRecoveryCause() { return _lastCause; }
Esp8266BaseWatchdogPhase Esp8266BaseWatchdog::lastStallPhase() { return _lastPhase; }
bool Esp8266BaseWatchdog::lastRecoveryWasDenied() { return _lastRecoveryDenied; }
Esp8266BaseRestartDecision Esp8266BaseWatchdog::lastRecoveryDecision() {
    return _lastRecoveryDecision;
}
uint8_t Esp8266BaseWatchdog::consecutiveRecoveryRestarts() { return _consecutiveRestarts; }
uint8_t Esp8266BaseWatchdog::recoveryRestartsInWindow() { return _restartsInWindow; }

bool Esp8266BaseWatchdog::restartBudgetAvailable() {
    return _decision(Esp8266BaseRecoveryCause::LOOP_STALL, millis()) ==
           Esp8266BaseRestartDecision::RESTARTING;
}

void Esp8266BaseWatchdog::clearResetCount() {
    _resetCount = 0;
    _consecutiveRestarts = 0;
    _restartsInWindow = 0;
    _budgetWindowStartedEpoch = 0;
    _lastCause = Esp8266BaseRecoveryCause::NONE;
    _lastRecoveryDenied = false;
    _lastRecoveryDecision = Esp8266BaseRestartDecision::NOT_RUNNING;
    _wasWdtReset = false;
    writeRtc(0, 0, 0, 0, 0);
#if ESP8266BASE_USE_CONFIG
    Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_WDT_COUNT, 0);
#endif
    ESP8266BASE_LOG_I_P("WDT ", "watchdog_reset_count_cleared");
}
#endif
