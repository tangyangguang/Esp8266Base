#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseJournal — 网络会话诊断档案（事件驱动 + 趋势采样 + Flash 槽位环）
//
// 目的：断线/停滞/重启现场的本地留存，供局域网取证页与重连后回传使用。
// 语义（详见 docs/03_api_reference.md 与本文件注释）：
//   - RAM 事件环：最近 16 条紧凑记录（采样与事件共用，12B/条）；
//   - Flash 环形槽：LittleFS 8 文件 × 8KB（可配），顺序轮转、满则覆盖最旧；
//   - 每个 boot 写入一条 bootinfo 批（bootNo/reset/代次），作为“会话”边界；
//   - 写入策略：事件积批（≤10s 限频）/趋势 30 分钟兜底下刷；稳定期写入极少；批带 CRC16；
//   - 读侧：会话列表 + 从新到旧导出记录（供页面按预算渲染）。
// 编译开关：ESP8266BASE_USE_JOURNAL（默认 USE_CONFIG && USE_MQTT）。
// RAM 预算：<= ~0.4KB 静态（16×12B 环 + 偏移环 16×2B + 96B 批缓冲）。
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_JOURNAL_SLOT_COUNT
#define ESP8266BASE_JOURNAL_SLOT_COUNT 8     // 槽文件数（8 × 8KB = 64KB）
#endif

#ifndef ESP8266BASE_JOURNAL_SLOT_BYTES
#define ESP8266BASE_JOURNAL_SLOT_BYTES 8192  // 每槽字节数（对齐 LittleFS 块）
#endif

#ifndef ESP8266BASE_JOURNAL_MAX_BATCH_BYTES
#define ESP8266BASE_JOURNAL_MAX_BATCH_BYTES 96   // 单批 payload 上限（≤8 条记录）
#endif

#ifndef ESP8266BASE_JOURNAL_SAMPLE_INTERVAL_MS
#define ESP8266BASE_JOURNAL_SAMPLE_INTERVAL_MS 60000UL  // 趋势采样：1 分钟
#endif

#ifndef ESP8266BASE_JOURNAL_TREND_FLUSH_AGE_MS
#define ESP8266BASE_JOURNAL_TREND_FLUSH_AGE_MS 1800000UL // 趋势兜底：30 分钟
#endif

#ifndef ESP8266BASE_JOURNAL_EVENT_FLUSH_MS
#define ESP8266BASE_JOURNAL_EVENT_FLUSH_MS 10000UL       // 事件批限频
#endif

static_assert(ESP8266BASE_JOURNAL_SLOT_COUNT >= 2 && ESP8266BASE_JOURNAL_SLOT_COUNT <= 32,
              "ESP8266BASE_JOURNAL_SLOT_COUNT must be 2..32");
static_assert(ESP8266BASE_JOURNAL_SLOT_BYTES >= 1024 && ESP8266BASE_JOURNAL_SLOT_BYTES <= 65536,
              "ESP8266BASE_JOURNAL_SLOT_BYTES must be 1024..65536");
static_assert(ESP8266BASE_JOURNAL_MAX_BATCH_BYTES >= 96 &&
                  ESP8266BASE_JOURNAL_MAX_BATCH_BYTES <= 2048,
              "ESP8266BASE_JOURNAL_MAX_BATCH_BYTES must be 96..2048");

// 记录类型（k 字段）
enum Esp8266BaseJournalKind : uint8_t {
    JNL_SAMPLE = 1,     // 趋势采样：p1=rssi p2=heap>>6 p3=lagMs(f 保留)
    JNL_WIFI_LOST = 2,  // WiFi 断开：f=SDK status
    JNL_WIFI_UP = 3,    // WiFi 恢复：p1=rssi
    JNL_MQTT_ATTEMPT = 4, // MQTT 建连尝试：p1=attempt
    JNL_MQTT_UP = 5,    // MQTT 连接成功
    JNL_MQTT_CLOSED = 6, // MQTT 连接关闭：p1=reason code p2=tls error
    JNL_RADIO_RESET = 7, // WiFi radio 完整复位：p1=累计次数
    JNL_STALL = 8,      // 主循环停滞（未覆盖段超时）p1=uncoveredMs
    JNL_NTP_SYNC = 9,   // 时间同步：p2|p3<<16 = unix epoch
    JNL_BUSINESS = 10,  // 业务自定义：f=subtype p1/p2 由业务定义
};

// MQTT 关闭原因码（与 Esp8266BaseMQTTDisconnectReason 一致）
enum Esp8266BaseJournalMqttReason : uint8_t {
    JNL_REASON_NONE = 0, JNL_REASON_USER_OK = 1, JNL_REASON_UNACCEPTABLE_PROTOCOL = 2,
    JNL_REASON_IDENTIFIER_REJECTED = 3, JNL_REASON_SERVER_UNAVAILABLE = 4,
    JNL_REASON_MALFORMED_CREDENTIALS = 5, JNL_REASON_NOT_AUTHORIZED = 6,
    JNL_REASON_TLS_BAD_FINGERPRINT = 7, JNL_REASON_TCP_DISCONNECTED = 8,
    JNL_REASON_UNKNOWN = 9
};

struct Esp8266BaseJournalStats {
    uint32_t writesSinceBoot;   // 本 boot 累计成功写批次数
    uint32_t bytesSinceBoot;    // 本 boot 累计写入字节
    uint32_t eraseEvents;       // 本 boot 累计轮转（覆盖旧槽）次数
    uint32_t ioErrors;          // 本 boot 累计 IO 错误
    uint8_t  slotBytesKb;       // 单槽 KB
    uint8_t  slotCount;         // 槽数
};

struct Esp8266BaseJournalSession {
    uint32_t generation;        // 槽代次
    uint32_t bootNo;            // 会话 boot 序号
    uint8_t  resetCode;         // 会话开机复位码
    uint32_t lastRecordMs;      // 会话最后记录时间（uptime，未知 0）
};

struct Esp8266BaseJournalSummary {
    uint16_t mqttAttempts;
    uint16_t mqttClosed;                 // 非零关闭次数
    uint8_t  mqttClosedReasons[10];      // 按 reason code 计数
    uint16_t wifiLost;
    uint16_t radioResets;
    uint16_t stalls;
    uint16_t samples;
    uint16_t records;                    // 本次导出记录总数
    uint8_t  lastMqttReason;             // 最近一次关闭原因码
    uint8_t  lastResetCode;              // 最近会话复位码
};

class Esp8266BaseJournal {
public:
    static bool begin();
    // 会话头：bootNo/reset 由 Esp8266Base 在启动取得后调用（写入 bootinfo 批）
    static void beginBoot(uint32_t bootNo, uint8_t resetCode);
    static void handle();            // 采样 + 按策略下刷（主循环每轮调用）

    // ---- 写入（入 RAM 环，由 handle 下刷）----
    static void record(uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2, uint16_t p3);
    static void recordNow(uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2, uint16_t p3);

    // ---- 读取（供页面/回传；全部有界、无大分配）----
    static bool getStats(Esp8266BaseJournalStats& stats);
    // 返回会话数（从新到旧，最多 maxCount 个）
    static uint16_t listSessions(Esp8266BaseJournalSession* out, uint16_t maxCount);
    // 从新到旧导出记录：每解析出一条即调用 onRecord(t, kind, flags, p1, p2, p3, bootNo, reset, ctx)；
    // t=该 boot 内 uptime ms（bootNo/reset 仅 0xFF 伪记录携带）；返回 false 表示调用方请求停止；
    // offsetRecords 为跳过的记录数（用于翻页）。
    typedef bool (*RecordVisitor)(uint32_t t, uint8_t kind, uint8_t flags, int16_t p1,
                                  uint16_t p2, uint16_t p3, uint32_t bootNo, uint8_t reset,
                                  void* ctx);
    static bool dumpTail(uint32_t offsetRecords, uint32_t maxRecords,
                         Esp8266BaseJournalSummary& summary, RecordVisitor visitor, void* ctx);
    // 诊断状态：0=ok 1=attention 2=error（供 /health diagLevel 等）
    static uint8_t diagLevel();
    // 近端摘要：RAM 缓存（上次下刷累计）+ 未下刷环内记录合并；不访问 Flash。
    // 供 /health、/switch 高频只读场景使用，避免每次请求扫描档案文件。
    static bool cachedSummary(Esp8266BaseJournalSummary& out);


    // 把单条记录格式化为 ≤80 字符文本行（页面/raw 共用）
    static void formatRecord(uint32_t t, uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2,
                             uint16_t p3, uint32_t bootNo, uint8_t reset,
                             char* out, size_t outLen);

private:
    static bool _flushPending();          // 把 RAM 环未下刷记录打包写入当前槽
    static bool _appendBatch(uint8_t type, const uint8_t* payload, uint16_t len);
    static bool _locateCurrent();         // 启动定位当前代次/槽并写 bootinfo
    static void _rotate();
    static void _pushEntry(uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2, uint16_t p3);
    static uint16_t _crc16(const uint8_t* data, uint16_t len);
    static void _slotName(uint32_t generation, char* out, size_t outLen);
};
