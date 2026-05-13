#pragma once
#include <Arduino.h>
#include "Esp8266BaseLog.h"

// ----------------------------------------------------------------------------
// Esp8266BaseFileLog — LittleFS 文件日志运行时模式
//
// 运行模式只支持 OFF / WARN / INFO。路径、单段大小、轮转段数、buffer
// 和 flush interval 都是构建期资源策略，不作为 Web 普通运维配置。
// ----------------------------------------------------------------------------

#define ESP8266BASE_FILELOG_MODE_OFF  4
#define ESP8266BASE_FILELOG_MODE_WARN 2
#define ESP8266BASE_FILELOG_MODE_INFO 1

#ifndef ESP8266BASE_FILELOG_DEFAULT_MODE
#define ESP8266BASE_FILELOG_DEFAULT_MODE ESP8266BASE_FILELOG_MODE_WARN
#endif

#ifndef ESP8266BASE_FILELOG_PATH
#define ESP8266BASE_FILELOG_PATH "/logs/app.log"
#endif

static_assert(sizeof(ESP8266BASE_FILELOG_PATH) <= 32,
              "ESP8266BASE_FILELOG_PATH must fit in 31 chars plus NUL");

#ifndef ESP8266BASE_FILELOG_MAX_BYTES
#define ESP8266BASE_FILELOG_MAX_BYTES (16UL * 1024UL)
#endif

#ifndef ESP8266BASE_FILELOG_ROTATE_FILES
#define ESP8266BASE_FILELOG_ROTATE_FILES 4
#endif

#ifndef ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS
#define ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS 2000
#endif

#ifndef ESP8266BASE_FILELOG_BUFFER_SIZE
#if ESP8266BASE_FILELOG_DEFAULT_MODE == ESP8266BASE_FILELOG_MODE_INFO
#define ESP8266BASE_FILELOG_BUFFER_SIZE 512
#else
#define ESP8266BASE_FILELOG_BUFFER_SIZE 0
#endif
#endif

#if ESP8266BASE_FILELOG_BUFFER_SIZE > 512
#error "ESP8266BASE_FILELOG_BUFFER_SIZE must be <= 512"
#endif

#if ESP8266BASE_FILELOG_ROTATE_FILES < 1 || ESP8266BASE_FILELOG_ROTATE_FILES > 4
#error "ESP8266BASE_FILELOG_ROTATE_FILES must be 1..4"
#endif

#if ESP8266BASE_FILELOG_MAX_BYTES < 256
#error "ESP8266BASE_FILELOG_MAX_BYTES must be >= 256"
#endif

#if ESP8266BASE_FILELOG_DEFAULT_MODE != ESP8266BASE_FILELOG_MODE_OFF && ESP8266BASE_FILELOG_DEFAULT_MODE != ESP8266BASE_FILELOG_MODE_WARN && ESP8266BASE_FILELOG_DEFAULT_MODE != ESP8266BASE_FILELOG_MODE_INFO
#error "ESP8266BASE_FILELOG_DEFAULT_MODE must be ESP8266BASE_FILELOG_MODE_OFF, ESP8266BASE_FILELOG_MODE_WARN, or ESP8266BASE_FILELOG_MODE_INFO"
#endif

#if ESP8266BASE_FILELOG_DEFAULT_MODE != ESP8266BASE_FILELOG_MODE_OFF && ESP8266BASE_FILELOG_DEFAULT_MODE < ESP8266BASE_LOG_LEVEL
#error "ESP8266BASE_FILELOG_DEFAULT_MODE cannot exceed ESP8266BASE_LOG_LEVEL"
#endif

class Esp8266BaseFileLog {
public:
    enum Mode : uint8_t {
        OFF  = ESP8266BASE_FILELOG_MODE_OFF,
        WARN = ESP8266BASE_FILELOG_MODE_WARN,
        INFO = ESP8266BASE_FILELOG_MODE_INFO
    };

    static bool begin();
    static bool setMode(Mode mode);
    static Mode mode();
    static const char* modeName();
    static bool isEnabled();

    static bool flush();
    static bool clear();
    static void handle();

    static const char* path();
    static uint32_t maxBytes();
    static uint8_t rotateFiles();
    static uint32_t size();
    static uint32_t segmentSize(uint8_t index);
    static bool bufferEnabled();
    static uint16_t bufferSize();
    static uint16_t bufferUsed();
    static uint32_t flushIntervalMs();

    static bool segmentPath(uint8_t index, char* out, size_t len);
};
