#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseLog — 轻量日志
//
// 特性：
//   - 编译期等级过滤（ESP8266BASE_LOG_LEVEL）
//   - 格式化缓冲 128B，在栈上分配，不占全局 RAM
//   - NTP 对时成功后可切换为绝对时间戳
//   - 默认只输出 Serial；可选 output hook / LittleFS file sink
//
// RAM 预算：<= 240B（全局静态）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_LOG_LEVEL
#define ESP8266BASE_LOG_LEVEL 1   // 0=D 1=I 2=W 3=E 4=Off
#endif

// 时间字符串回调函数类型
typedef const char* (*Esp8266BaseTimeProviderFn)();
typedef void (*Esp8266BaseLogHookFn)(uint8_t level,
                                     const char* tag,
                                     const char* message,
                                     const char* timestamp,
                                     const char* line);

class Esp8266BaseLog {
public:
    // 初始化，设置初始等级（默认使用编译宏）
    static void begin(uint8_t level = ESP8266BASE_LOG_LEVEL);

    // 运行时修改等级：0=D 1=I 2=W 3=E 4=Off
    static void setLevel(uint8_t level);

    // 注入时间字符串回调（NTP 同步后由 Esp8266BaseNTP 调用）
    static void setTimeProvider(Esp8266BaseTimeProviderFn fn);

    // 可选输出 hook：接收已格式化后的日志行，以及拆分后的字段
    static void setOutputHook(Esp8266BaseLogHookFn fn);

    // 可选 LittleFS 文件日志。默认关闭，不影响 Serial-only 轻量路径。
    static bool enableFileSink(const char* path,
                               uint32_t maxBytes,
                               uint8_t fileLevel = ESP8266BASE_LOG_LEVEL,
                               uint8_t rotateFiles = 4);
    static void disableFileSink();
    static void setFileSinkLevel(uint8_t level);
    static bool isFileSinkEnabled();
    static const char* fileSinkPath();
    static uint32_t fileSinkMaxBytes();
    static uint8_t fileSinkRotateFiles();
    static uint8_t fileSinkLevel();
    static uint32_t fileSinkSize();
    static uint32_t fileSinkSegmentSize(uint8_t index);
    static bool clearFileSink();

    // 启动会话分割线，建议 Config 挂载后调用
    static void beginBootSession(const char* firmware,
                                 const char* version,
                                 const char* resetReason,
                                 uint32_t bootCount,
                                 uint32_t freeHeap);

    // 转发到 Config 审计开关，方便应用只接触 Log 模块
    static void enableConfigAudit(bool enabled);
    static void enableConfigReadAudit(bool enabled);

    // 内部：带等级、tag、printf 格式的输出（宏最终调用此函数）
    static void log(uint8_t level, const char* tag, const char* fmt, ...);

private:
    static uint8_t                  _level;   // 1B
    static Esp8266BaseTimeProviderFn _timeFn;  // 4B
    static Esp8266BaseLogHookFn      _hook;    // 4B
    static bool                      _fileEnabled; // 1B
    static uint8_t                   _fileLevel; // 1B
    static uint8_t                   _fileRotateFiles; // 1B：1=current only, max 4
    static bool                      _fileDirReady; // 1B
    static uint32_t                  _fileMaxBytes; // 4B
    static uint32_t                  _fileCurrentBytes; // 4B
    static char                      _filePath[32]; // 32B
    // 格式缓冲 128B 在 log() 栈上分配，不存此处

    static const char* _timestamp(char* buf, size_t len);
    static bool _segmentPath(uint8_t index, char* out, size_t len);
    static bool _ensureFileReady();
    static bool _truncateCurrentFile();
    static bool _rotateFile();
    static bool _writeFileLine(const char* line);
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
