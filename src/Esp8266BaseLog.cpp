#include "Esp8266BaseLog.h"
#include <stdarg.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
uint8_t                   Esp8266BaseLog::_level  = ESP8266BASE_LOG_LEVEL;
Esp8266BaseTimeProviderFn Esp8266BaseLog::_timeFn  = nullptr;

// ----------------------------------------------------------------------------
// 内部常量
// ----------------------------------------------------------------------------
static const char* const LOG_LEVEL_STR[] = { "D", "I", "W", "E" };

// 日志格式缓冲大小（栈分配，不占全局 RAM）
static const int LOG_BUF_SIZE = 128;

// ----------------------------------------------------------------------------
// 公开方法
// ----------------------------------------------------------------------------

void Esp8266BaseLog::begin(uint8_t level) {
    _level = level;
}

void Esp8266BaseLog::setLevel(uint8_t level) {
    _level = level;
}

void Esp8266BaseLog::setTimeProvider(Esp8266BaseTimeProviderFn fn) {
    _timeFn = fn;
}

void Esp8266BaseLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    // 运行时等级检查（编译期宏已过滤大部分，此处作为运行时 setLevel 的保障）
    if (level < _level) return;

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
    const char* levelStr = LOG_LEVEL_STR[level & 3u];
    if (_timeFn) {
        Serial.printf("[%s][%s][%s] %s\n", _timeFn(), levelStr, tagBuf, msg);
    } else {
        Serial.printf("[%lu][%s][%s] %s\n", millis(), levelStr, tagBuf, msg);
    }
}
