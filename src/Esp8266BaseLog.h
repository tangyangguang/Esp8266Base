#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseLog — 轻量串口日志
//
// 特性：
//   - 编译期等级过滤（ESP8266BASE_LOG_LEVEL）
//   - 格式化缓冲 128B，在栈上分配，不占全局 RAM
//   - NTP 对时成功后可切换为绝对时间戳
//   - 不支持文件日志、不支持 output hook
//
// RAM 预算：<= 160B（全局静态）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_LOG_LEVEL
#define ESP8266BASE_LOG_LEVEL 1   // 0=D 1=I 2=W 3=E 4=Off
#endif

// 时间字符串回调函数类型
typedef const char* (*Esp8266BaseTimeProviderFn)();

class Esp8266BaseLog {
public:
    // 初始化，设置初始等级（默认使用编译宏）
    static void begin(uint8_t level = ESP8266BASE_LOG_LEVEL);

    // 运行时修改等级：0=D 1=I 2=W 3=E 4=Off
    static void setLevel(uint8_t level);

    // 注入时间字符串回调（NTP 同步后由 Esp8266BaseNTP 调用）
    static void setTimeProvider(Esp8266BaseTimeProviderFn fn);

    // 内部：带等级、tag、printf 格式的输出（宏最终调用此函数）
    static void log(uint8_t level, const char* tag, const char* fmt, ...);

private:
    static uint8_t                  _level;   // 1B
    static Esp8266BaseTimeProviderFn _timeFn;  // 4B
    // 格式缓冲 128B 在 log() 栈上分配，不存此处
};

// ----------------------------------------------------------------------------
// 日志宏 — 编译期过滤，不满足等级的宏展开为空
// tag: 最多 12 字符，输出时固定 4 字符宽度
// ----------------------------------------------------------------------------

#if ESP8266BASE_LOG_LEVEL <= 0
  #define ESP8266BASE_LOG_D(tag, fmt, ...) \
      Esp8266BaseLog::log(0, tag, fmt, ##__VA_ARGS__)
#else
  #define ESP8266BASE_LOG_D(tag, fmt, ...) do {} while(0)
#endif

#if ESP8266BASE_LOG_LEVEL <= 1
  #define ESP8266BASE_LOG_I(tag, fmt, ...) \
      Esp8266BaseLog::log(1, tag, fmt, ##__VA_ARGS__)
#else
  #define ESP8266BASE_LOG_I(tag, fmt, ...) do {} while(0)
#endif

#if ESP8266BASE_LOG_LEVEL <= 2
  #define ESP8266BASE_LOG_W(tag, fmt, ...) \
      Esp8266BaseLog::log(2, tag, fmt, ##__VA_ARGS__)
#else
  #define ESP8266BASE_LOG_W(tag, fmt, ...) do {} while(0)
#endif

#if ESP8266BASE_LOG_LEVEL <= 3
  #define ESP8266BASE_LOG_E(tag, fmt, ...) \
      Esp8266BaseLog::log(3, tag, fmt, ##__VA_ARGS__)
#else
  #define ESP8266BASE_LOG_E(tag, fmt, ...) do {} while(0)
#endif
