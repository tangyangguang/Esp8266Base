#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseConfig — LittleFS KV 配置存储
//
// 特性：
//   - 每个 key 对应 /cfg_<key> 文件；库保留 key 使用 eb_ 前缀
//   - 支持 string / int32 / bool 三种类型
//   - 写前比较旧值，无变化不写 Flash
//   - 高频更新使用 setXxxDeferred，由 handle() 分批写入
//   - deep sleep / 重启前调用 flush() 强制写完所有 pending
//
// RAM 预算：<= 512B（全局静态）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_CFG_DEFERRED_SIZE
#define ESP8266BASE_CFG_DEFERRED_SIZE 4
#endif

#ifndef ESP8266BASE_CFG_KEY_MAX
#define ESP8266BASE_CFG_KEY_MAX 24
#endif

#ifndef ESP8266BASE_CFG_STR_MAX
#define ESP8266BASE_CFG_STR_MAX 96
#endif

#ifndef ESP8266BASE_CFG_FORMAT_ON_FAIL
#define ESP8266BASE_CFG_FORMAT_ON_FAIL 0
#endif

// 库保留配置 key 统一使用 eb_ 前缀，业务项目不要复用。
#define ESP8266BASE_CFG_KEY_WIFI_SSID    "eb_wifi_ssid"
#define ESP8266BASE_CFG_KEY_WIFI_PASS    "eb_wifi_pass"
#define ESP8266BASE_CFG_KEY_AP_PASS      "eb_ap_pass"
#define ESP8266BASE_CFG_KEY_HOSTNAME     "eb_hostname"
#define ESP8266BASE_CFG_KEY_WEB_USER     "eb_web_user"
#define ESP8266BASE_CFG_KEY_WEB_PASS     "eb_web_pass"
#define ESP8266BASE_CFG_KEY_WDT_COUNT    "eb_wdt_count"
#define ESP8266BASE_CFG_KEY_WDT_PENDING  "eb_wdt_pending"
#define ESP8266BASE_CFG_KEY_BOOT_COUNT   "eb_boot_count"

class Esp8266BaseConfig {
public:
    // 挂载 LittleFS，初始化 deferred 队列
    static bool begin();

    // ---- 立即写入 ----
    // string: value 最大 ESP8266BASE_CFG_STR_MAX 字节，超出拒绝
    static bool setStr(const char* key, const char* value);
    static bool getStr(const char* key, char* out, size_t len, const char* def = "");

    // int32
    static bool    setInt(const char* key, int32_t value);
    static int32_t getInt(const char* key, int32_t def = 0);

    // bool（存储为 "1"/"0"）
    static bool setBool(const char* key, bool value);
    static bool getBool(const char* key, bool def = false);

    // ---- 延迟写入（deferred） ----
    // 写入先进内存队列，handle() 每轮最多刷 1 条
    // 队列满（默认 4 条）时返回 false
    static bool setIntDeferred(const char* key, int32_t value);
    static bool setBoolDeferred(const char* key, bool value);

    // 每轮 loop 调用，最多写 1 条 pending 到 Flash
    static void handle();

    // 强制写完所有 pending（deep sleep / 重启前调用）
    static bool flush();

    // 删除所有 /cfg_* 配置文件（恢复出厂配置前调用）
    static bool clearAll();

    // 当前 pending 条数（用于诊断日志）
    static uint8_t pendingCount();

    // 是否就绪
    static bool isReady();

    // 配置审计日志。写审计默认关闭；读审计单独开关，避免刷屏。
    static void enableConfigAudit(bool enabled);
    static void enableConfigReadAudit(bool enabled);
    static bool isConfigAuditEnabled();
    static bool isConfigReadAuditEnabled();

private:
    // deferred 队列条目
    struct DeferredEntry {
        char    key[ESP8266BASE_CFG_KEY_MAX + 1];  // 25B
        int32_t intVal;                             // 4B
        bool    boolVal;                            // 1B
        uint8_t type;   // 1=int32, 2=bool          // 1B
        bool    used;                               // 1B
        // pad 1B → 32B per entry, 4 entries = 128B
    };

    static DeferredEntry _deferred[ESP8266BASE_CFG_DEFERRED_SIZE]; // 128B
    static bool _ready;                                             // 1B
    static bool _auditEnabled;                                      // 1B
    static bool _readAuditEnabled;                                  // 1B

    // 内部辅助
    static bool _buildPath(const char* key, char* path, size_t pathLen);
    static bool _writeRaw(const char* path, const char* value);
    static bool _readRaw(const char* path, char* out, size_t len);
    static bool _enqueue(const char* key, int32_t iv, bool bv, uint8_t type);
    static void _flushOne();
    static bool _setStrInternal(const char* op, const char* key, const char* value);
    static void _auditRead(const char* op, const char* key, const char* value, bool found);
};
