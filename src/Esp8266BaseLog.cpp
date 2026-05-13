#include "Esp8266BaseLog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseUtil.h"
#include <stdarg.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
uint8_t                   Esp8266BaseLog::_runtimeLevel = ESP8266BASE_LOG_LEVEL;
uint8_t                   Esp8266BaseLog::_serialLevel  = ESP8266BASE_LOG_LEVEL;
Esp8266BaseTimeProviderFn Esp8266BaseLog::_timeFn       = nullptr;
Esp8266BaseLogHookFn      Esp8266BaseLog::_hook         = nullptr;
Esp8266BaseLogHookFn      Esp8266BaseLog::_internalHook = nullptr;

// ----------------------------------------------------------------------------
// 内部常量
// ----------------------------------------------------------------------------
static const char* const LOG_LEVEL_STR[] = { "D", "I", "W", "E" };

// 日志格式缓冲大小（栈分配，不占全局 RAM）
static const int LOG_BUF_SIZE = 128;
static const int LOG_LINE_SIZE = 192;

// ----------------------------------------------------------------------------
// 公开方法
// ----------------------------------------------------------------------------

void Esp8266BaseLog::begin(uint8_t level) {
    _runtimeLevel = level;
    _serialLevel = level;
}

void Esp8266BaseLog::setRuntimeLevel(uint8_t level) {
    _runtimeLevel = level;
}

uint8_t Esp8266BaseLog::runtimeLevel() {
    return _runtimeLevel;
}

void Esp8266BaseLog::setSerialLevel(uint8_t level) {
    _serialLevel = level;
}

uint8_t Esp8266BaseLog::serialLevel() {
    return _serialLevel;
}

void Esp8266BaseLog::setTimeProvider(Esp8266BaseTimeProviderFn fn) {
    _timeFn = fn;
}

void Esp8266BaseLog::setOutputHook(Esp8266BaseLogHookFn fn) {
    _hook = fn;
}

const char* Esp8266BaseLog::_levelName(uint8_t level) {
    switch (level) {
        case 0: return "DEBUG";
        case 1: return "INFO";
        case 2: return "WARN";
        case 3: return "ERROR";
        case 4: return "OFF";
        default: return "UNKNOWN";
    }
}

const char* Esp8266BaseLog::_bootReasonDesc(const char* bootReason) {
    if (!bootReason || strcmp(bootReason, "unknown") == 0) return "未知启动原因";
    if (strcmp(bootReason, "power-on") == 0) return "上电或外部复位";
    if (strcmp(bootReason, "deep-sleep") == 0) return "深度睡眠唤醒";
    if (strcmp(bootReason, "soft-restart") == 0) return "软件重启";
    if (strcmp(bootReason, "wdt-reset") == 0) return "看门狗重启";
    return "未知启动原因";
}

void Esp8266BaseLog::beginBootSession(const char* firmware,
                                      const char* version,
                                      const char* bootReason,
                                      uint32_t bootCount,
                                      uint32_t freeHeap) {
    const char* reason = (bootReason && bootReason[0] && strcmp(bootReason, "undefined") != 0)
        ? bootReason
        : "unknown";
    if (strcmp(reason, "power-on") != 0 &&
        strcmp(reason, "deep-sleep") != 0 &&
        strcmp(reason, "soft-restart") != 0 &&
        strcmp(reason, "wdt-reset") != 0 &&
        strcmp(reason, "unknown") != 0) {
        reason = "unknown";
    }
    char heapBuf[16];
    Esp8266BaseUtil::formatBytes(freeHeap, heapBuf, sizeof(heapBuf));
    log(1, "Boot", "============================================================");
    log(1, "Boot", "boot_session_start boot_count=%lu", (unsigned long)bootCount);
    log(1, "Boot", "boot_reason=%s boot_desc=%s", reason, _bootReasonDesc(reason));
    log(1, "Boot", "firmware=%s version=%s free_heap=%s",
        firmware ? firmware : "",
        version ? version : "",
        heapBuf);
    log(1, "Boot", "============================================================");
}

void Esp8266BaseLog::enableConfigAudit(bool enabled) {
    Esp8266BaseConfig::enableConfigAudit(enabled);
}

void Esp8266BaseLog::enableConfigReadAudit(bool enabled) {
    Esp8266BaseConfig::enableConfigReadAudit(enabled);
}

const char* Esp8266BaseLog::_timestamp(char* buf, size_t len) {
    if (_timeFn) return _timeFn();
    snprintf(buf, len, "%lu", millis());
    return buf;
}

void Esp8266BaseLog::_setInternalHook(Esp8266BaseLogHookFn fn) {
    _internalHook = fn;
}

void Esp8266BaseLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    if (level < _runtimeLevel || level >= 4) return;

    bool serialEnabled = (level >= _serialLevel);
    if (!serialEnabled && !_hook && !_internalHook) return;

    // 消息内容格式化（128B 栈缓冲）
    char msg[LOG_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // tag 固定输出 4 字符宽（不足补空格，超出截断）
    char tagBuf[5];
    snprintf(tagBuf, sizeof(tagBuf), "%-4.4s", tag ? tag : "");

    // 时间戳：NTP 成功后切换为绝对时间，否则用 millis()
    const char* levelStr = (level < 4) ? LOG_LEVEL_STR[level] : "?";
    char tsBuf[16];
    const char* ts = _timestamp(tsBuf, sizeof(tsBuf));

    char line[LOG_LINE_SIZE];
    snprintf(line, sizeof(line), "[%s][%s][%s] %s", ts, levelStr, tagBuf, msg);

    if (serialEnabled) Serial.println(line);
    if (_hook) {
        _hook(level, tagBuf, msg, ts, line);
    }
    if (_internalHook) {
        _internalHook(level, tagBuf, msg, ts, line);
    }
}
