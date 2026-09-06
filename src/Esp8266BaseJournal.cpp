#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_JOURNAL
#include "Esp8266BaseJournal.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseWiFi.h"
#include <LittleFS.h>

// ----------------------------------------------------------------------------
// 磁盘格式
//   槽文件: "/jr/seg<gen%N>"，文件头 12B：magic "JR01"(4) + generation u32 + reserved u32
//   其后为连续批：hdr(6B)=0x4A + type + payloadLen u16 + crc16 u16，随后 payload。
//     type=0x02 bootinfo（payload 12B: bootNo u32 + gen u32 + reset u8 + pad 3）
//     type=0x01 data（payload = 12B 记录序列）
//   记录 12B: t u32 + k u8 + f u8 + p1 i16 + p2 u16 + p3 u16
// ----------------------------------------------------------------------------

namespace {
constexpr uint8_t BATCH_MAGIC = 0x4A;
constexpr uint8_t BATCH_DATA = 0x01;
constexpr uint8_t BATCH_BOOT = 0x02;
constexpr uint32_t FILE_MAGIC = 0x31524A55UL;  // "JR01"
constexpr size_t FILE_HEAD_BYTES = 12;
constexpr size_t ENTRY_BYTES = 12;
constexpr uint16_t RING_ENTRIES = 16;
constexpr uint16_t OFFSET_RING = 16;
constexpr char DIR_PATH[] = "/jr";
constexpr size_t MAX_SESSIONS_SCAN = 16;

struct JournalEntry {
    uint32_t t;
    uint8_t  k;
    uint8_t  f;
    int16_t  p1;
    uint16_t p2;
    uint16_t p3;
};

struct FileHead {
    uint32_t magic;
    uint32_t generation;
    uint32_t reserved;
};

struct BootPayload {
    uint32_t bootNo;
    uint32_t generation;
    uint8_t  reset;
    uint8_t  pad[3];
};

JournalEntry g_ring[RING_ENTRIES];
uint16_t g_ringHead = 0;      // 下一个写入位置
uint16_t g_ringCount = 0;     // 未下刷条数
uint32_t g_lastSampleMs = 0;
uint32_t g_lastFlushMs = 0;
uint32_t g_lastEventFlushMs = 0;
uint32_t g_prevHandleMs = 0;
uint32_t g_currentGeneration = 0;
bool     g_ready = false;
Esp8266BaseJournalStats g_stats;
uint32_t g_bootNo = 0;
uint8_t  g_resetCode = 0;
uint32_t g_bootUptimeStartMs = 0;

char g_slotPath[24];
uint8_t g_payload[ESP8266BASE_JOURNAL_MAX_BATCH_BYTES];
Esp8266BaseJournalSummary g_summaryCache;   // 自本 boot 起已下刷记录的累计摘要（RAM 缓存）
uint16_t g_offsetRing[OFFSET_RING];   // 批起始偏移（文件内；类型/长度在导出时回读批头）
uint16_t g_offsetCount = 0;           // 有效批数（<= OFFSET_RING）
uint16_t g_offsetStart = 0;           // 环形起点（最旧）
}  // namespace

uint16_t Esp8266BaseJournal::_crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; ++b) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

void Esp8266BaseJournal::_slotName(uint32_t generation, char* out, size_t outLen) {
    snprintf(out, outLen, "%s/seg%u", DIR_PATH,
             (unsigned)(generation % ESP8266BASE_JOURNAL_SLOT_COUNT));
}

// ----------------------------------------------------------------------------
// RAM 环
// ----------------------------------------------------------------------------
void Esp8266BaseJournal::_pushEntry(uint8_t kind, uint8_t flags, int16_t p1,
                                    uint16_t p2, uint16_t p3) {
    if (!g_ready) return;
    JournalEntry& e = g_ring[g_ringHead];
    e.t = millis();
    e.k = kind;
    e.f = flags;
    e.p1 = p1;
    e.p2 = p2;
    e.p3 = p3;
    g_ringHead = (g_ringHead + 1) % RING_ENTRIES;
    if (g_ringCount < RING_ENTRIES) ++g_ringCount;
}

void Esp8266BaseJournal::record(uint8_t kind, uint8_t flags, int16_t p1,
                                uint16_t p2, uint16_t p3) {
    _pushEntry(kind, flags, p1, p2, p3);
}

void Esp8266BaseJournal::recordNow(uint8_t kind, uint8_t flags, int16_t p1,
                                   uint16_t p2, uint16_t p3) {
    _pushEntry(kind, flags, p1, p2, p3);
    _flushPending();
}

// ----------------------------------------------------------------------------
// 磁盘写入
// ----------------------------------------------------------------------------
void Esp8266BaseJournal::_rotate() {
    ++g_currentGeneration;
    ++g_stats.eraseEvents;
    _slotName(g_currentGeneration, g_slotPath, sizeof(g_slotPath));
    LittleFS.remove(g_slotPath);
    File f = LittleFS.open(g_slotPath, "w");
    if (f) {
        FileHead head;
        head.magic = FILE_MAGIC;
        head.generation = g_currentGeneration;
        head.reserved = 0;
        f.write(reinterpret_cast<const uint8_t*>(&head), FILE_HEAD_BYTES);
        f.close();
    } else {
        ++g_stats.ioErrors;
        ESP8266BASE_LOG_E("Jrnl", "rotate_create_failed path=%s", g_slotPath);
    }
    BootPayload boot;
    boot.bootNo = g_bootNo;
    boot.generation = g_currentGeneration;
    boot.reset = g_resetCode;
    boot.pad[0] = boot.pad[1] = boot.pad[2] = 0;
    _appendBatch(BATCH_BOOT, reinterpret_cast<const uint8_t*>(&boot), sizeof(boot));
}

bool Esp8266BaseJournal::_appendBatch(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (!g_ready || len > ESP8266BASE_JOURNAL_MAX_BATCH_BYTES) return false;
    File f = LittleFS.open(g_slotPath, "a");
    if (!f) {
        ++g_stats.ioErrors;
        ESP8266BASE_LOG_E("Jrnl", "append_open_failed path=%s", g_slotPath);
        return false;
    }
    const uint32_t size = f.size();
    if (size + FILE_HEAD_BYTES + 6 + len > ESP8266BASE_JOURNAL_SLOT_BYTES) {
        f.close();
        _rotate();
        f = LittleFS.open(g_slotPath, "a");
        if (!f) {
            ++g_stats.ioErrors;
            return false;
        }
    }
    uint8_t hdr[6];
    hdr[0] = BATCH_MAGIC;
    hdr[1] = type;
    hdr[2] = static_cast<uint8_t>(len & 0xFF);
    hdr[3] = static_cast<uint8_t>(len >> 8);
    const uint16_t crc = _crc16(payload, len);
    hdr[4] = static_cast<uint8_t>(crc & 0xFF);
    hdr[5] = static_cast<uint8_t>(crc >> 8);
    const bool ok = f.write(hdr, sizeof(hdr)) == sizeof(hdr) &&
                    (len == 0 || f.write(payload, len) == len);
    f.close();
    if (!ok) {
        ++g_stats.ioErrors;
        ESP8266BASE_LOG_E("Jrnl", "append_write_failed path=%s", g_slotPath);
        return false;
    }
    ++g_stats.writesSinceBoot;
    g_stats.bytesSinceBoot += sizeof(hdr) + len;
    return true;
}

// ----------------------------------------------------------------------------
// 启动定位
// ----------------------------------------------------------------------------
bool Esp8266BaseJournal::begin() {
    if (g_ready) return true;
    if (!LittleFS.begin()) return false;
    if (!LittleFS.exists(DIR_PATH)) {
        if (!LittleFS.mkdir(DIR_PATH)) return false;
    }
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.slotBytesKb = ESP8266BASE_JOURNAL_SLOT_BYTES / 1024;
    g_stats.slotCount = ESP8266BASE_JOURNAL_SLOT_COUNT;
    g_ringHead = 0;
    g_ringCount = 0;
    g_prevHandleMs = millis();
    g_lastSampleMs = 0;
    g_lastFlushMs = 0;
    g_lastEventFlushMs = 0;

    // 找到最大代次的槽作为当前写入目标；无效槽直接删除
    uint32_t bestGen = 0;
    bool found = false;
    for (uint8_t i = 0; i < ESP8266BASE_JOURNAL_SLOT_COUNT; ++i) {
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, (unsigned)i);
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        FileHead head;
        const int n = f.read(reinterpret_cast<uint8_t*>(&head), FILE_HEAD_BYTES);
        f.close();
        if (n != (int)FILE_HEAD_BYTES || head.magic != FILE_MAGIC) {
            LittleFS.remove(path);
            continue;
        }
        if (!found || head.generation > bestGen) {
            bestGen = head.generation;
            found = true;
        }
    }
    g_currentGeneration = found ? bestGen : 0;
    _slotName(g_currentGeneration, g_slotPath, sizeof(g_slotPath));
    if (!found) {
        File f = LittleFS.open(g_slotPath, "w");
        if (f) {
            FileHead head;
            head.magic = FILE_MAGIC;
            head.generation = 0;
            head.reserved = 0;
            f.write(reinterpret_cast<const uint8_t*>(&head), FILE_HEAD_BYTES);
            f.close();
        } else {
            ++g_stats.ioErrors;
        }
    }
    g_ready = true;
    ESP8266BASE_LOG_I("Jrnl", "journal_ready slots=%ux%uKB gen=%lu",
                      (unsigned)ESP8266BASE_JOURNAL_SLOT_COUNT,
                      (unsigned)(ESP8266BASE_JOURNAL_SLOT_BYTES / 1024),
                      (unsigned long)g_currentGeneration);
    return true;
}

void Esp8266BaseJournal::beginBoot(uint32_t bootNo, uint8_t resetCode) {
    if (!g_ready) return;
    g_bootNo = bootNo;
    g_resetCode = resetCode;
    memset(&g_summaryCache, 0, sizeof(g_summaryCache));
    g_bootUptimeStartMs = millis();
    BootPayload boot;
    boot.bootNo = bootNo;
    boot.generation = g_currentGeneration;
    boot.reset = resetCode;
    boot.pad[0] = boot.pad[1] = boot.pad[2] = 0;
    _appendBatch(BATCH_BOOT, reinterpret_cast<const uint8_t*>(&boot), sizeof(boot));
    ESP8266BASE_LOG_I("Jrnl", "journal_session_started boot=%lu reset=%u gen=%lu",
                      (unsigned long)bootNo, (unsigned)resetCode,
                      (unsigned long)g_currentGeneration);
}

void Esp8266BaseJournal::handle() {
    if (!g_ready) return;
    const uint32_t now = millis();
    const uint32_t loopDelta = now - g_prevHandleMs;
    g_prevHandleMs = now;

    if (now - g_lastSampleMs >= ESP8266BASE_JOURNAL_SAMPLE_INTERVAL_MS) {
        g_lastSampleMs = now;
        const int rssi = Esp8266BaseWiFi::isConnected() ? Esp8266BaseWiFi::rssi() : 0;
        uint16_t heap = 0;
        const uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < (1U << 20)) heap = static_cast<uint16_t>(freeHeap >> 6);
        uint16_t lag = loopDelta <= 0xFFFF ? static_cast<uint16_t>(loopDelta) : 0xFFFF;
        _pushEntry(JNL_SAMPLE, 0, static_cast<int16_t>(rssi), heap, lag);
    }

    const uint32_t oldestT = g_ring[(g_ringHead + RING_ENTRIES - g_ringCount) % RING_ENTRIES].t;
    const uint32_t trendAge = g_ringCount ? now - oldestT : 0;
    if (g_ringCount >= 16) {
        _flushPending();
    } else if (g_ringCount > 0 && now - g_lastEventFlushMs >= ESP8266BASE_JOURNAL_EVENT_FLUSH_MS) {
        _flushPending();
    } else if (trendAge >= ESP8266BASE_JOURNAL_TREND_FLUSH_AGE_MS) {
        _flushPending();
    }
}

bool Esp8266BaseJournal::_flushPending() {
    if (!g_ready || g_ringCount == 0) return true;
    const uint16_t maxEntries = ESP8266BASE_JOURNAL_MAX_BATCH_BYTES / ENTRY_BYTES;
    const uint16_t batch = g_ringCount < maxEntries ? g_ringCount : maxEntries;
    const uint16_t bytes = batch * ENTRY_BYTES;
    const uint16_t start = (g_ringHead + RING_ENTRIES - g_ringCount) % RING_ENTRIES;
    uint8_t* dst = g_payload;
    for (uint16_t i = 0; i < batch; ++i) {
        const JournalEntry& e = g_ring[(start + i) % RING_ENTRIES];
        memcpy(dst, &e.t, 4);
        dst[4] = e.k;
        dst[5] = e.f;
        memcpy(dst + 6, &e.p1, 2);
        memcpy(dst + 8, &e.p2, 2);
        memcpy(dst + 10, &e.p3, 2);
        dst += ENTRY_BYTES;
    }
    if (!_appendBatch(BATCH_DATA, g_payload, bytes)) {
        return false;  // 保留 RAM 环，下轮重试
    }
    // 累计到 RAM 缓存（与 dumpTail 汇总口径一致）
    for (uint16_t i = 0; i < batch; ++i) {
        const uint8_t* p = g_payload + i * ENTRY_BYTES;
        const uint8_t kind = p[4];
        int16_t p1;
        memcpy(&p1, p + 6, 2);
        if (kind == JNL_MQTT_ATTEMPT) ++g_summaryCache.mqttAttempts;
        else if (kind == JNL_MQTT_CLOSED) {
            ++g_summaryCache.mqttClosed;
            const uint8_t rc = static_cast<uint8_t>(p1 & 0xFF);
            if (rc < 10) ++g_summaryCache.mqttClosedReasons[rc];
            g_summaryCache.lastMqttReason = rc;
        } else if (kind == JNL_WIFI_LOST) ++g_summaryCache.wifiLost;
        else if (kind == JNL_RADIO_RESET) ++g_summaryCache.radioResets;
        else if (kind == JNL_STALL) ++g_summaryCache.stalls;
        else if (kind == JNL_SAMPLE) ++g_summaryCache.samples;
    }
    g_ringCount -= batch;  // 出队：只减计数，写游标不变（环形覆盖语义）
    if (g_ringCount == 0) g_ringHead = 0;
    g_lastFlushMs = millis();
    return true;
}

// ----------------------------------------------------------------------------
// 读取
// ----------------------------------------------------------------------------
bool Esp8266BaseJournal::getStats(Esp8266BaseJournalStats& stats) {
    stats = g_stats;
    return g_ready;
}

// 解析单个槽：把批起始偏移/类型记入环形数组（保留最近 OFFSET_RING 批），返回批数
static uint16_t scanSlotBatches(uint8_t slot, uint16_t& ringStart) {
    char path[24];
    snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, (unsigned)slot);
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    f.seek(FILE_HEAD_BYTES);
    uint16_t count = 0;
    ringStart = 0;
    uint8_t hdr[6];
    while (f.available() >= (int)sizeof(hdr)) {
        const uint32_t batchStart = f.position();
        if (f.read(hdr, sizeof(hdr)) != (int)sizeof(hdr)) break;
        if (hdr[0] != BATCH_MAGIC) break;  // 尾部损坏：停止
        const uint16_t len = hdr[2] | (static_cast<uint16_t>(hdr[3]) << 8);
        if (batchStart + sizeof(hdr) + len > ESP8266BASE_JOURNAL_SLOT_BYTES) break;
        if (!f.seek(batchStart + sizeof(hdr) + len)) break;
        if (count < OFFSET_RING) {
            g_offsetRing[count] = static_cast<uint16_t>(batchStart);
            ++count;
        } else {
            g_offsetRing[ringStart] = static_cast<uint16_t>(batchStart);
            ringStart = (ringStart + 1) % OFFSET_RING;
        }
    }
    f.close();
    g_offsetCount = count;
    return count;
}

uint16_t Esp8266BaseJournal::listSessions(Esp8266BaseJournalSession* out, uint16_t maxCount) {
    if (!g_ready || !out || maxCount == 0) return 0;
    Esp8266BaseJournalSession tmp[MAX_SESSIONS_SCAN];
    uint16_t total = 0;

    // 先收集每个槽文件头里的代次并按代次降序处理
    uint32_t gens[ESP8266BASE_JOURNAL_SLOT_COUNT];
    bool valid[ESP8266BASE_JOURNAL_SLOT_COUNT];
    for (uint8_t s = 0; s < ESP8266BASE_JOURNAL_SLOT_COUNT; ++s) {
        valid[s] = false;
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, (unsigned)s);
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        FileHead head;
        if (f.read(reinterpret_cast<uint8_t*>(&head), FILE_HEAD_BYTES) == (int)FILE_HEAD_BYTES &&
            head.magic == FILE_MAGIC) {
            gens[s] = head.generation;
            valid[s] = true;
        }
        f.close();
    }
    for (uint8_t pass = 0; pass < ESP8266BASE_JOURNAL_SLOT_COUNT && total < MAX_SESSIONS_SCAN; ++pass) {
        int best = -1;
        for (uint8_t s = 0; s < ESP8266BASE_JOURNAL_SLOT_COUNT; ++s) {
            if (valid[s] && (best < 0 || gens[s] > gens[best])) best = s;
        }
        if (best < 0) break;
        valid[best] = false;

        uint16_t ringStart = 0;
        const uint16_t count = scanSlotBatches(static_cast<uint8_t>(best), ringStart);
        // 按文件内从新到旧找 bootinfo
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, (unsigned)best);
        for (uint16_t oi = count; oi > 0; --oi) {
            const uint16_t idx = (ringStart + oi - 1) % OFFSET_RING;
            File f = LittleFS.open(path, "r");
            if (!f) continue;
            f.seek(g_offsetRing[idx]);
            uint8_t hdr[6];
            if (f.read(hdr, sizeof(hdr)) != (int)sizeof(hdr) || hdr[0] != BATCH_MAGIC ||
                hdr[1] != BATCH_BOOT) {
                f.close();
                continue;
            }
            f.seek(g_offsetRing[idx] + 6);
            uint8_t payload[12];
            const int n = f.read(payload, sizeof(payload));
            f.close();
            if (n != (int)sizeof(payload)) continue;
            BootPayload boot;
            memcpy(&boot.bootNo, payload, 4);
            memcpy(&boot.generation, payload + 4, 4);
            boot.reset = payload[8];
            if (boot.generation != gens[best]) continue;
            if (total < MAX_SESSIONS_SCAN) {
                Esp8266BaseJournalSession& s = tmp[total++];
                s.generation = boot.generation;
                s.bootNo = boot.bootNo;
                s.resetCode = boot.reset;
                s.lastRecordMs = 0;
            }
            break;  // 每个槽只取最新一个会话即可（新会话在槽尾）
        }
    }
    const uint16_t n = total < maxCount ? total : maxCount;
    memcpy(out, tmp, n * sizeof(Esp8266BaseJournalSession));
    return n;
}

bool Esp8266BaseJournal::dumpTail(uint32_t offsetRecords, uint32_t maxRecords,
                                  Esp8266BaseJournalSummary& summary, RecordVisitor visitor,
                                  void* ctx) {
    if (!g_ready || !visitor) return false;
    memset(&summary, 0, sizeof(summary));
    uint32_t skipped = 0, emitted = 0;

    // 槽代次降序
    uint32_t gens[ESP8266BASE_JOURNAL_SLOT_COUNT];
    bool valid[ESP8266BASE_JOURNAL_SLOT_COUNT];
    for (uint8_t s = 0; s < ESP8266BASE_JOURNAL_SLOT_COUNT; ++s) {
        valid[s] = false;
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, (unsigned)s);
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        FileHead head;
        if (f.read(reinterpret_cast<uint8_t*>(&head), FILE_HEAD_BYTES) == (int)FILE_HEAD_BYTES &&
            head.magic == FILE_MAGIC) {
            gens[s] = head.generation;
            valid[s] = true;
        }
        f.close();
    }
    bool done = false;
    for (uint8_t pass = 0; pass < ESP8266BASE_JOURNAL_SLOT_COUNT && !done; ++pass) {
        int best = -1;
        for (uint8_t s = 0; s < ESP8266BASE_JOURNAL_SLOT_COUNT; ++s) {
            if (valid[s] && (best < 0 || gens[s] > gens[best])) best = s;
        }
        if (best < 0) break;
        valid[best] = false;

        uint16_t ringStart = 0;
        const uint16_t count = scanSlotBatches(static_cast<uint8_t>(best), ringStart);
        if (count == 0) continue;
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, (unsigned)best);
        // 每槽只打开一次，批间 seek 定位，避免每批 open/close 的 LittleFS 开销与内存碎片
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        for (uint16_t oi = count; oi > 0 && emitted < maxRecords; --oi) {
            const uint16_t idx = (ringStart + oi - 1) % OFFSET_RING;
            f.seek(g_offsetRing[idx]);
            uint8_t hdr[6];
            const int hn = f.read(hdr, sizeof(hdr));
            const bool hdrOk = hn == (int)sizeof(hdr) && hdr[0] == BATCH_MAGIC;
            if (!hdrOk) break;
            const uint16_t len = hdr[2] | (static_cast<uint16_t>(hdr[3]) << 8);
            const uint8_t type = hdr[1];
            if (len == 0 || len > ESP8266BASE_JOURNAL_MAX_BATCH_BYTES) break;
            const int rd = f.read(g_payload, len);
            if (rd != (int)len) break;
            const uint16_t storedCrc = hdr[4] | (static_cast<uint16_t>(hdr[5]) << 8);
            if (_crc16(g_payload, len) != storedCrc) break;  // 损坏批：该文件旧数据到此为止
            if (type == BATCH_BOOT) {
                BootPayload boot;
                memcpy(&boot.bootNo, g_payload, 4);
                memcpy(&boot.generation, g_payload + 4, 4);
                boot.reset = g_payload[8];
                if (skipped < offsetRecords) {
                    ++skipped;
                    continue;
                }
                ++summary.records;
                summary.lastResetCode = boot.reset;
                if (!visitor(0, 0xFF, 0, 0, 0, 0, boot.bootNo, boot.reset, ctx)) {
                    done = true;
                    break;
                }
                ++emitted;
                continue;
            }
            // data 批：payload 为 12B 记录，从新到旧
            const uint16_t n = static_cast<uint16_t>(len / ENTRY_BYTES);
            for (uint16_t e = n; e > 0 && emitted < maxRecords; --e) {
                const uint8_t* p = g_payload + (e - 1) * ENTRY_BYTES;
                uint32_t t = 0;
                uint8_t kind = p[4], flags = p[5];
                int16_t p1;
                uint16_t p2, p3;
                memcpy(&t, p, 4);
                memcpy(&p1, p + 6, 2);
                memcpy(&p2, p + 8, 2);
                memcpy(&p3, p + 10, 2);
                if (skipped < offsetRecords) {
                    ++skipped;
                    continue;
                }
                ++summary.records;
                if (kind == JNL_MQTT_ATTEMPT) ++summary.mqttAttempts;
                else if (kind == JNL_MQTT_CLOSED) {
                    ++summary.mqttClosed;
                    const uint8_t rc = static_cast<uint8_t>(p1 & 0xFF);
                    if (rc < 10) ++summary.mqttClosedReasons[rc];
                    summary.lastMqttReason = rc;
                } else if (kind == JNL_WIFI_LOST) ++summary.wifiLost;
                else if (kind == JNL_RADIO_RESET) ++summary.radioResets;
                else if (kind == JNL_STALL) ++summary.stalls;
                else if (kind == JNL_SAMPLE) ++summary.samples;
                if (!visitor(t, kind, flags, p1, p2, p3, 0, 0, ctx)) {
                    done = true;
                    break;
                }
                ++emitted;
            }
        }
        f.close();
    }
    return true;
}

static void mergeRingInto(Esp8266BaseJournalSummary& s) {
    // 只统计尚未下刷的 g_ringCount 条（环内更早的数据已计入缓存）
    const uint16_t start = (g_ringHead + RING_ENTRIES - g_ringCount) % RING_ENTRIES;
    for (uint16_t i = 0; i < g_ringCount; ++i) {
        const JournalEntry& e = g_ring[(start + i) % RING_ENTRIES];
        if (e.k == JNL_MQTT_ATTEMPT) ++s.mqttAttempts;
        else if (e.k == JNL_MQTT_CLOSED) {
            ++s.mqttClosed;
            const uint8_t rc = static_cast<uint8_t>(e.p1 & 0xFF);
            if (rc < 10) ++s.mqttClosedReasons[rc];
            s.lastMqttReason = rc;
        } else if (e.k == JNL_WIFI_LOST) ++s.wifiLost;
        else if (e.k == JNL_RADIO_RESET) ++s.radioResets;
        else if (e.k == JNL_STALL) ++s.stalls;
        else if (e.k == JNL_SAMPLE) ++s.samples;
    }
}

bool Esp8266BaseJournal::cachedSummary(Esp8266BaseJournalSummary& out) {
    if (!g_ready) return false;
    out = g_summaryCache;
    mergeRingInto(out);
    return true;
}

uint8_t Esp8266BaseJournal::diagLevel() {
    if (!g_ready) return 0;
    Esp8266BaseJournalSummary s;
    cachedSummary(s);
    if (s.stalls > 0 || s.radioResets > 0) return 2;
    // 受控正常下线（user_ok）不算异常；只看非正常关闭与 WiFi 掉线
    const uint16_t abnormalClosed = s.mqttClosed > s.mqttClosedReasons[1]
        ? s.mqttClosed - s.mqttClosedReasons[1] : 0;
    if (abnormalClosed > 0 || s.wifiLost > 0) return 1;
    return 0;
}

// ----------------------------------------------------------------------------
// 格式化（页面/raw 共用；≤80 字符）
// ----------------------------------------------------------------------------
void Esp8266BaseJournal::formatRecord(uint32_t t, uint8_t kind, uint8_t flags, int16_t p1,
                                      uint16_t p2, uint16_t p3, uint32_t bootNo,
                                      uint8_t reset, char* out, size_t outLen) {
    char ts[16];
    snprintf(ts, sizeof(ts), "%lus", (unsigned long)(t / 1000UL));
    if (kind == 0xFF) {
        snprintf(out, outLen, "== boot #%lu reset=%u ==", (unsigned long)bootNo, (unsigned)reset);
        return;
    }
    switch (kind) {
        case JNL_SAMPLE:
            snprintf(out, outLen, "%s sample rssi=%ddBm heap=%luk lag=%ums", ts, (int)p1,
                     (unsigned long)(((uint32_t)p2 << 6) / 1024UL), (unsigned)p3);
            break;
        case JNL_WIFI_LOST:
            snprintf(out, outLen, "%s wifi_lost status=%u", ts, (unsigned)flags);
            break;
        case JNL_WIFI_UP:
            snprintf(out, outLen, "%s wifi_up rssi=%ddBm", ts, (int)p1);
            break;
        case JNL_MQTT_ATTEMPT:
            snprintf(out, outLen, "%s mqtt_attempt #%u", ts, (unsigned)(p1 & 0x7FFF));
            break;
        case JNL_MQTT_UP:
            snprintf(out, outLen, "%s mqtt_up", ts);
            break;
        case JNL_MQTT_CLOSED: {
            static const char* names[10] = {"none", "user_ok", "unacceptable_protocol",
                                            "identifier_rejected", "server_unavailable",
                                            "malformed_credentials", "not_authorized",
                                            "tls_bad_fingerprint", "tcp_disconnected", "unknown"};
            const uint8_t rc = static_cast<uint8_t>(p1 & 0xFF);
            const char* name = rc < 10 ? names[rc] : "unknown";
            if (p2 != 0xFFFF) {
                snprintf(out, outLen, "%s mqtt_closed reason=%s tls=%d", ts, name, (int)(int16_t)p2);
            } else {
                snprintf(out, outLen, "%s mqtt_closed reason=%s", ts, name);
            }
            break;
        }
        case JNL_RADIO_RESET:
            snprintf(out, outLen, "%s radio_reset total=%u", ts, (unsigned)p1);
            break;
        case JNL_STALL:
            snprintf(out, outLen, "%s stall uncovered=%lums", ts, (unsigned long)(p1 & 0x7FFF));
            break;
        case JNL_NTP_SYNC:
            snprintf(out, outLen, "%s ntp_sync epoch=%lu", ts,
                     (unsigned long)((uint32_t)p2 | ((uint32_t)p3 << 16)));
            break;
        case JNL_BUSINESS:
            snprintf(out, outLen, "%s biz subtype=%u a=%d b=%u", ts, (unsigned)flags,
                     (int)p1, (unsigned)p2);
            break;
        default:
            snprintf(out, outLen, "%s kind=%u f=%u a=%d b=%u c=%u", ts, (unsigned)kind,
                     (unsigned)flags, (int)p1, (unsigned)p2, (unsigned)p3);
            break;
    }
}
#endif
