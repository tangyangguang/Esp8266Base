#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_JOURNAL
#include "Esp8266BaseJournal.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseWiFi.h"
#if ESP8266BASE_USE_WEB
#include "Esp8266BaseWeb.h"
#endif
#include <LittleFS.h>

namespace {
constexpr uint32_t FILE_MAGIC = 0x32524A55UL;  // "JR02"
constexpr uint8_t RECORD_MAGIC_0 = 0xA5;
constexpr uint8_t RECORD_MAGIC_1 = 0x5A;
constexpr size_t FILE_HEAD_BYTES = 16;
constexpr size_t RECORD_BYTES = 16;
constexpr uint16_t RING_ENTRIES = 8;
constexpr char DIR_PATH[] = "/jr";
constexpr uint8_t BOOT_KIND = 0xFF;
constexpr uint32_t WEB_STORAGE_QUIET_MS = 3000UL;

struct JournalEntry {
    uint32_t t;
    uint8_t k;
    uint8_t f;
    int16_t p1;
    uint16_t p2;
    uint16_t p3;
};

JournalEntry g_ring[RING_ENTRIES];
uint16_t g_ringHead = 0;
uint16_t g_ringCount = 0;
bool g_eventPending = false;
uint32_t g_lastSampleMs = 0;
uint32_t g_lastEventFlushMs = 0;
uint32_t g_prevHandleMs = 0;
uint32_t g_currentGeneration = 0;
bool g_ready = false;
Esp8266BaseJournalStats g_stats;
uint32_t g_bootNo = 0;
uint8_t g_resetCode = 0;
char g_slotPath[24];
uint8_t g_recordBytes[RECORD_BYTES];
Esp8266BaseJournalSummary g_summaryCache;

bool g_trendActive = false;
uint32_t g_trendStartedMs = 0;
int16_t g_trendMinRssi = 0;
uint16_t g_trendMinHeap = 0;
uint16_t g_trendMaxLag = 0;
uint8_t g_trendSamples = 0;

bool storageFlushWindowAvailable(uint32_t now) {
#if ESP8266BASE_USE_WEB
    const uint32_t webLast = Esp8266BaseWeb::lastActivityMs();
    if (webLast != 0U && now - webLast < WEB_STORAGE_QUIET_MS) return false;
#else
    (void)now;
#endif
    return true;
}

uint16_t crc16Bytes(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; ++b) crc = (crc & 1U) ? (crc >> 1U) ^ 0xA001U : crc >> 1U;
    }
    return crc;
}

void put16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
}

void put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
    out[2] = static_cast<uint8_t>(value >> 16U);
    out[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t get16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8U);
}

uint32_t get32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) |
           (static_cast<uint32_t>(in[2]) << 16U) | (static_cast<uint32_t>(in[3]) << 24U);
}

void encodeRecord(const JournalEntry& entry, uint8_t* out) {
    put32(out, entry.t);
    out[4] = entry.k;
    out[5] = entry.f;
    put16(out + 6, static_cast<uint16_t>(entry.p1));
    put16(out + 8, entry.p2);
    put16(out + 10, entry.p3);
    put16(out + 12, crc16Bytes(out, 12));
    out[14] = RECORD_MAGIC_0;
    out[15] = RECORD_MAGIC_1;
}

bool decodeRecord(const uint8_t* in, JournalEntry& entry) {
    if (in[14] != RECORD_MAGIC_0 || in[15] != RECORD_MAGIC_1 ||
        get16(in + 12) != crc16Bytes(in, 12)) return false;
    entry.t = get32(in);
    entry.k = in[4];
    entry.f = in[5];
    entry.p1 = static_cast<int16_t>(get16(in + 6));
    entry.p2 = get16(in + 8);
    entry.p3 = get16(in + 10);
    return true;
}

void encodeHeader(uint32_t generation, uint8_t* out) {
    memset(out, 0, FILE_HEAD_BYTES);
    put32(out, FILE_MAGIC);
    put32(out + 4, generation);
    put16(out + 8, RECORD_BYTES);
    put16(out + 14, crc16Bytes(out, 14));
}

bool decodeHeader(const uint8_t* in, uint32_t& generation) {
    if (get32(in) != FILE_MAGIC || get16(in + 8) != RECORD_BYTES ||
        get16(in + 14) != crc16Bytes(in, 14)) return false;
    generation = get32(in + 4);
    return true;
}

void addSummary(Esp8266BaseJournalSummary& summary, const JournalEntry& entry) {
    if (entry.k == JNL_MQTT_ATTEMPT) ++summary.mqttAttempts;
    else if (entry.k == JNL_MQTT_CLOSED) {
        ++summary.mqttClosed;
        const uint8_t reason = static_cast<uint8_t>(entry.p1 & 0xFF);
        if (reason < 10) ++summary.mqttClosedReasons[reason];
        summary.lastMqttReason = reason;
    } else if (entry.k == JNL_WIFI_LOST) ++summary.wifiLost;
    else if (entry.k == JNL_RADIO_RESET) ++summary.radioResets;
    else if (entry.k == JNL_STALL) ++summary.stalls;
    else if (entry.k == JNL_SAMPLE) ++summary.samples;
    else if (entry.k == BOOT_KIND) summary.lastResetCode = entry.f;
}

bool readHeader(File& file, uint32_t& generation) {
    if (file.size() < FILE_HEAD_BYTES || !file.seek(0) ||
        file.read(g_recordBytes, FILE_HEAD_BYTES) != static_cast<int>(FILE_HEAD_BYTES)) return false;
    return decodeHeader(g_recordBytes, generation);
}

bool readRecord(File& file, uint32_t index, JournalEntry& entry) {
    const uint32_t offset = FILE_HEAD_BYTES + index * RECORD_BYTES;
    if (!file.seek(offset) || file.read(g_recordBytes, RECORD_BYTES) != static_cast<int>(RECORD_BYTES)) return false;
    return decodeRecord(g_recordBytes, entry);
}

uint32_t recordCount(File& file) {
    const uint32_t size = file.size();
    return size < FILE_HEAD_BYTES ? 0 : (size - FILE_HEAD_BYTES) / RECORD_BYTES;
}

bool slotIsAppendable(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    uint32_t generation = 0;
    if (!readHeader(file, generation)) { file.close(); return false; }
    const uint32_t payloadBytes = file.size() - FILE_HEAD_BYTES;
    if (payloadBytes % RECORD_BYTES != 0) { file.close(); return false; }
    const uint32_t count = recordCount(file);
    JournalEntry entry;
    for (uint32_t i = 0; i < count; ++i) {
        if (!readRecord(file, i, entry)) { file.close(); return false; }
    }
    file.close();
    return true;
}
}  // namespace

uint16_t Esp8266BaseJournal::_crc16(const uint8_t* data, uint16_t len) { return crc16Bytes(data, len); }

void Esp8266BaseJournal::_slotName(uint32_t generation, char* out, size_t outLen) {
    snprintf(out, outLen, "%s/seg%u", DIR_PATH,
             static_cast<unsigned>(generation % ESP8266BASE_JOURNAL_SLOT_COUNT));
}

bool Esp8266BaseJournal::_createSlot(uint32_t generation, bool includeBoot) {
    _slotName(generation, g_slotPath, sizeof(g_slotPath));
    LittleFS.remove(g_slotPath);
    File file = LittleFS.open(g_slotPath, "w");
    if (!file) { ++g_stats.ioErrors; return false; }
    encodeHeader(generation, g_recordBytes);
    const bool ok = file.write(g_recordBytes, FILE_HEAD_BYTES) == FILE_HEAD_BYTES;
    file.close();
    if (!ok) { ++g_stats.ioErrors; return false; }
    g_currentGeneration = generation;
    return !includeBoot || _appendBoot();
}

bool Esp8266BaseJournal::_appendBoot() {
    if (!g_bootNo) return true;
    JournalEntry boot = {};
    boot.k = BOOT_KIND;
    boot.f = g_resetCode;
    boot.p2 = static_cast<uint16_t>(g_bootNo);
    boot.p3 = static_cast<uint16_t>(g_bootNo >> 16U);
    File file = LittleFS.open(g_slotPath, "a");
    if (!file) { ++g_stats.ioErrors; return false; }
    encodeRecord(boot, g_recordBytes);
    const bool ok = file.write(g_recordBytes, RECORD_BYTES) == RECORD_BYTES;
    file.close();
    if (!ok) ++g_stats.ioErrors;
    else { ++g_stats.writesSinceBoot; g_stats.bytesSinceBoot += RECORD_BYTES; }
    return ok;
}

void Esp8266BaseJournal::_rotate() {
    ++g_stats.eraseEvents;
    if (!_createSlot(g_currentGeneration + 1U, true))
        ESP8266BASE_LOG_E_P("Jrnl", "rotate_create_failed gen=%lu", static_cast<unsigned long>(g_currentGeneration + 1U));
}

bool Esp8266BaseJournal::begin() {
    if (g_ready) return true;
    if (!LittleFS.begin()) return false;
    if (!LittleFS.exists(DIR_PATH) && !LittleFS.mkdir(DIR_PATH)) return false;

    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_summaryCache, 0, sizeof(g_summaryCache));
    g_stats.slotBytesKb = ESP8266BASE_JOURNAL_SLOT_BYTES / 1024U;
    g_stats.slotCount = ESP8266BASE_JOURNAL_SLOT_COUNT;
    g_ringHead = g_ringCount = 0;
    g_eventPending = false;
    g_trendActive = false;
    g_prevHandleMs = millis();
    g_lastSampleMs = g_prevHandleMs;
    g_lastEventFlushMs = g_prevHandleMs;

    bool found = false;
    uint32_t bestGeneration = 0;
    for (uint8_t slot = 0; slot < ESP8266BASE_JOURNAL_SLOT_COUNT; ++slot) {
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, static_cast<unsigned>(slot));
        File file = LittleFS.open(path, "r");
        if (!file) continue;
        uint32_t generation = 0;
        const bool valid = readHeader(file, generation);
        file.close();
        if (!valid) {
            // JR01 and any other experimental formats are intentionally discarded.
            LittleFS.remove(path);
            continue;
        }
        if (!found || generation > bestGeneration) { found = true; bestGeneration = generation; }
    }

    g_currentGeneration = found ? bestGeneration : 0;
    _slotName(g_currentGeneration, g_slotPath, sizeof(g_slotPath));
    if (!found) {
        if (!_createSlot(0, false)) return false;
    } else if (!slotIsAppendable(g_slotPath)) {
        ESP8266BASE_LOG_W_P("Jrnl", "slot_tail_corrupt gen=%lu action=preserve_and_rotate",
                          static_cast<unsigned long>(g_currentGeneration));
        ++g_stats.eraseEvents;
        if (!_createSlot(g_currentGeneration + 1U, false)) return false;
    }

    g_ready = true;
    ESP8266BASE_LOG_I_P("Jrnl", "journal_ready format=2 slots=%ux%uKB gen=%lu",
                      static_cast<unsigned>(ESP8266BASE_JOURNAL_SLOT_COUNT),
                      static_cast<unsigned>(ESP8266BASE_JOURNAL_SLOT_BYTES / 1024U),
                      static_cast<unsigned long>(g_currentGeneration));
    return true;
}

void Esp8266BaseJournal::beginBoot(uint32_t bootNo, uint8_t resetCode) {
    if (!g_ready) return;
    g_bootNo = bootNo;
    g_resetCode = resetCode;
    memset(&g_summaryCache, 0, sizeof(g_summaryCache));
    if (!_appendBoot()) ESP8266BASE_LOG_E_P("Jrnl", "journal_session_start_failed boot=%lu", static_cast<unsigned long>(bootNo));
    else ESP8266BASE_LOG_I_P("Jrnl", "journal_session_started boot=%lu reset=%u gen=%lu",
                           static_cast<unsigned long>(bootNo), static_cast<unsigned>(resetCode),
                           static_cast<unsigned long>(g_currentGeneration));
}

void Esp8266BaseJournal::_pushEntry(uint8_t kind, uint8_t flags, int16_t p1,
                                    uint16_t p2, uint16_t p3, bool event) {
    if (!g_ready) return;
    if (g_ringCount == RING_ENTRIES &&
        (!storageFlushWindowAvailable(millis()) || !_flushPending())) {
        ++g_stats.droppedRecords;
        return;
    }
    JournalEntry& entry = g_ring[g_ringHead];
    entry.t = millis();
    entry.k = kind;
    entry.f = flags;
    entry.p1 = p1;
    entry.p2 = p2;
    entry.p3 = p3;
    g_ringHead = (g_ringHead + 1U) % RING_ENTRIES;
    ++g_ringCount;
    if (event) g_eventPending = true;
}

void Esp8266BaseJournal::record(uint8_t kind, uint8_t flags, int16_t p1,
                                uint16_t p2, uint16_t p3) {
    _pushEntry(kind, flags, p1, p2, p3, true);
}

void Esp8266BaseJournal::recordNow(uint8_t kind, uint8_t flags, int16_t p1,
                                   uint16_t p2, uint16_t p3) {
    // Recovery evidence explicitly requests durable storage before a synchronous
    // restart. Unlike routine trend/event flushing, this path is never called
    // from the asynchronous heartbeat callback.
    if (g_ringCount == RING_ENTRIES) _flushPending();
    _pushEntry(kind, flags, p1, p2, p3, true);
    _flushPending();
}

void Esp8266BaseJournal::handle() {
    if (!g_ready) return;
    const uint32_t now = millis();
    const uint32_t loopDelta = now - g_prevHandleMs;
    g_prevHandleMs = now;

    if (now - g_lastSampleMs >= ESP8266BASE_JOURNAL_SAMPLE_INTERVAL_MS) {
        g_lastSampleMs = now;
        const int16_t rssi = Esp8266BaseWiFi::isConnected()
            ? static_cast<int16_t>(Esp8266BaseWiFi::rssi()) : 0;
        const uint32_t freeHeap = ESP.getFreeHeap();
        const uint16_t heap = freeHeap < (1UL << 20U) ? static_cast<uint16_t>(freeHeap >> 6U) : 0xFFFFU;
        const uint16_t lag = loopDelta > 0xFFFFU ? 0xFFFFU : static_cast<uint16_t>(loopDelta);
        if (!g_trendActive) {
            g_trendActive = true;
            g_trendStartedMs = now;
            g_trendMinRssi = rssi;
            g_trendMinHeap = heap;
            g_trendMaxLag = lag;
            g_trendSamples = 1;
        } else {
            if (rssi && (!g_trendMinRssi || rssi < g_trendMinRssi)) g_trendMinRssi = rssi;
            if (heap < g_trendMinHeap) g_trendMinHeap = heap;
            if (lag > g_trendMaxLag) g_trendMaxLag = lag;
            if (g_trendSamples < 0xFFU) ++g_trendSamples;
        }
    }

    // LittleFS opens allocate temporary heap. Do not overlap routine journal
    // writes with a recently served Web request; retain the aggregate/event in
    // the fixed ring and flush after the same 3-second quiet window used by TLS.
    if (!storageFlushWindowAvailable(now)) return;
    if (g_trendActive && now - g_trendStartedMs >= ESP8266BASE_JOURNAL_TREND_FLUSH_AGE_MS) {
        _pushEntry(JNL_SAMPLE, g_trendSamples, g_trendMinRssi, g_trendMinHeap, g_trendMaxLag, false);
        g_trendActive = false;
        _flushPending();
    } else if (g_eventPending && now - g_lastEventFlushMs >= ESP8266BASE_JOURNAL_EVENT_FLUSH_MS) {
        _flushPending();
    }
}

bool Esp8266BaseJournal::_appendPending(uint16_t start, uint16_t count, uint16_t& written) {
    written = 0;
    File sizeFile = LittleFS.open(g_slotPath, "r");
    if (!sizeFile) { ++g_stats.ioErrors; return false; }
    const uint32_t required = count * RECORD_BYTES;
    const bool rotate = sizeFile.size() + required > ESP8266BASE_JOURNAL_SLOT_BYTES;
    sizeFile.close();
    if (rotate) _rotate();

    File file = LittleFS.open(g_slotPath, "a");
    if (!file) { ++g_stats.ioErrors; return false; }
    for (; written < count; ++written) {
        const JournalEntry& entry = g_ring[(start + written) % RING_ENTRIES];
        encodeRecord(entry, g_recordBytes);
        if (file.write(g_recordBytes, RECORD_BYTES) != RECORD_BYTES) break;
    }
    file.close();
    if (written != count) { ++g_stats.ioErrors; return false; }
    g_stats.writesSinceBoot += written;
    g_stats.bytesSinceBoot += written * RECORD_BYTES;
    return true;
}

bool Esp8266BaseJournal::_flushPending() {
    if (!g_ready || !g_ringCount) return true;
    const uint16_t start = (g_ringHead + RING_ENTRIES - g_ringCount) % RING_ENTRIES;
    uint16_t written = 0;
    const bool ok = _appendPending(start, g_ringCount, written);
    for (uint16_t i = 0; i < written; ++i) addSummary(g_summaryCache, g_ring[(start + i) % RING_ENTRIES]);
    g_ringCount -= written;
    if (!g_ringCount) { g_ringHead = 0; g_eventPending = false; }
    if (written) g_lastEventFlushMs = millis();
    return ok;
}

bool Esp8266BaseJournal::getStats(Esp8266BaseJournalStats& stats) {
    stats = g_stats;
    return g_ready;
}

uint16_t Esp8266BaseJournal::listSessions(Esp8266BaseJournalSession* out, uint16_t maxCount) {
    if (!g_ready || !out || !maxCount) return 0;
    uint32_t generations[ESP8266BASE_JOURNAL_SLOT_COUNT] = {};
    bool valid[ESP8266BASE_JOURNAL_SLOT_COUNT] = {};
    for (uint8_t slot = 0; slot < ESP8266BASE_JOURNAL_SLOT_COUNT; ++slot) {
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, static_cast<unsigned>(slot));
        File file = LittleFS.open(path, "r");
        if (!file) continue;
        valid[slot] = readHeader(file, generations[slot]);
        file.close();
    }
    uint16_t total = 0;
    uint32_t lastBoot = 0xFFFFFFFFUL;
    for (uint8_t pass = 0; pass < ESP8266BASE_JOURNAL_SLOT_COUNT && total < maxCount; ++pass) {
        int best = -1;
        for (uint8_t slot = 0; slot < ESP8266BASE_JOURNAL_SLOT_COUNT; ++slot)
            if (valid[slot] && (best < 0 || generations[slot] > generations[best])) best = slot;
        if (best < 0) break;
        valid[best] = false;
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, static_cast<unsigned>(best));
        File file = LittleFS.open(path, "r");
        if (!file) continue;
        uint32_t newestRecordMs = 0;
        const uint32_t count = recordCount(file);
        for (uint32_t index = count; index > 0 && total < maxCount; --index) {
            JournalEntry entry;
            if (!readRecord(file, index - 1U, entry)) continue;
            if (!newestRecordMs && entry.k != BOOT_KIND) newestRecordMs = entry.t;
            if (entry.k != BOOT_KIND) continue;
            const uint32_t bootNo = static_cast<uint32_t>(entry.p2) |
                                    (static_cast<uint32_t>(entry.p3) << 16U);
            if (bootNo == lastBoot) continue;
            Esp8266BaseJournalSession& session = out[total++];
            session.generation = generations[best];
            session.bootNo = bootNo;
            session.resetCode = entry.f;
            session.lastRecordMs = newestRecordMs;
            lastBoot = bootNo;
            newestRecordMs = 0;
        }
        file.close();
    }
    return total;
}

bool Esp8266BaseJournal::dumpTail(uint32_t offsetRecords, uint32_t maxRecords,
                                  Esp8266BaseJournalSummary& summary, RecordVisitor visitor,
                                  void* ctx) {
    if (!g_ready || !visitor) return false;
    memset(&summary, 0, sizeof(summary));
    uint32_t generations[ESP8266BASE_JOURNAL_SLOT_COUNT] = {};
    bool valid[ESP8266BASE_JOURNAL_SLOT_COUNT] = {};
    for (uint8_t slot = 0; slot < ESP8266BASE_JOURNAL_SLOT_COUNT; ++slot) {
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, static_cast<unsigned>(slot));
        File file = LittleFS.open(path, "r");
        if (!file) continue;
        valid[slot] = readHeader(file, generations[slot]);
        file.close();
    }

    uint32_t skipped = 0;
    uint32_t emitted = 0;
    for (uint8_t pass = 0; pass < ESP8266BASE_JOURNAL_SLOT_COUNT && emitted < maxRecords; ++pass) {
        int best = -1;
        for (uint8_t slot = 0; slot < ESP8266BASE_JOURNAL_SLOT_COUNT; ++slot)
            if (valid[slot] && (best < 0 || generations[slot] > generations[best])) best = slot;
        if (best < 0) break;
        valid[best] = false;
        char path[24];
        snprintf(path, sizeof(path), "%s/seg%u", DIR_PATH, static_cast<unsigned>(best));
        File file = LittleFS.open(path, "r");
        if (!file) continue;
        const uint32_t count = recordCount(file);
        for (uint32_t index = count; index > 0 && emitted < maxRecords; --index) {
            JournalEntry entry;
            if (!readRecord(file, index - 1U, entry)) continue;
            if (skipped++ < offsetRecords) continue;
            ++summary.records;
            addSummary(summary, entry);
            const uint32_t bootNo = entry.k == BOOT_KIND
                ? static_cast<uint32_t>(entry.p2) | (static_cast<uint32_t>(entry.p3) << 16U) : 0;
            if (!visitor(entry.t, entry.k, entry.f, entry.p1, entry.p2, entry.p3,
                         bootNo, entry.k == BOOT_KIND ? entry.f : 0, ctx)) {
                file.close();
                return true;
            }
            ++emitted;
        }
        file.close();
    }
    return true;
}

bool Esp8266BaseJournal::cachedSummary(Esp8266BaseJournalSummary& out) {
    memset(&out, 0, sizeof(out));
    if (!g_ready) return false;
    out = g_summaryCache;
    const uint16_t start = (g_ringHead + RING_ENTRIES - g_ringCount) % RING_ENTRIES;
    for (uint16_t i = 0; i < g_ringCount; ++i) addSummary(out, g_ring[(start + i) % RING_ENTRIES]);
    return true;
}

uint8_t Esp8266BaseJournal::diagLevel() {
    if (!g_ready) return 0;
    Esp8266BaseJournalSummary summary;
    cachedSummary(summary);
    if (summary.stalls) return 2;
    const uint16_t abnormal = summary.mqttClosed > summary.mqttClosedReasons[JNL_REASON_USER_OK]
        ? summary.mqttClosed - summary.mqttClosedReasons[JNL_REASON_USER_OK] : 0;
    return (abnormal || summary.wifiLost || summary.radioResets) ? 1 : 0;
}

void Esp8266BaseJournal::formatRecord(uint32_t t, uint8_t kind, uint8_t flags, int16_t p1,
                                      uint16_t p2, uint16_t p3, uint32_t bootNo,
                                      uint8_t reset, char* out, size_t outLen) {
    char timestamp[16];
    snprintf(timestamp, sizeof(timestamp), "%lus", static_cast<unsigned long>(t / 1000UL));
    if (kind == BOOT_KIND) {
        snprintf(out, outLen, "== boot #%lu reset=%u ==", static_cast<unsigned long>(bootNo), static_cast<unsigned>(reset));
        return;
    }
    switch (kind) {
        case JNL_SAMPLE: {
            // Heap samples are stored in 64-byte units. Preserve that useful
            // precision in the text view without pulling floating point into
            // the firmware; '~' makes the quantization explicit.
            const uint32_t heapBytes = static_cast<uint32_t>(p2) << 6U;
            const uint32_t heapCentiKib = (heapBytes * 100UL + 512UL) / 1024UL;
            snprintf(out, outLen,
                     "%s trend samples=%u rssi_min=%ddBm heap_min~%lu.%02luKiB lag_max=%ums",
                     timestamp, static_cast<unsigned>(flags), static_cast<int>(p1),
                     static_cast<unsigned long>(heapCentiKib / 100UL),
                     static_cast<unsigned long>(heapCentiKib % 100UL),
                     static_cast<unsigned>(p3));
            break;
        }
        case JNL_WIFI_LOST: snprintf(out, outLen, "%s wifi_lost status=%u", timestamp, static_cast<unsigned>(flags)); break;
        case JNL_WIFI_UP: snprintf(out, outLen, "%s wifi_up rssi=%ddBm", timestamp, static_cast<int>(p1)); break;
        case JNL_MQTT_ATTEMPT: snprintf(out, outLen, "%s mqtt_attempt #%u", timestamp, static_cast<unsigned>(p1 & 0x7FFF)); break;
        case JNL_MQTT_UP: snprintf(out, outLen, "%s mqtt_up", timestamp); break;
        case JNL_MQTT_CLOSED: {
            static const char* names[10] = {"none", "user_ok", "unacceptable_protocol", "identifier_rejected",
                "server_unavailable", "malformed_credentials", "not_authorized", "tls_bad_fingerprint",
                "tcp_disconnected", "unknown"};
            const uint8_t reason = static_cast<uint8_t>(p1 & 0xFF);
            snprintf(out, outLen, "%s mqtt_closed reason=%s tls=%d", timestamp,
                     reason < 10 ? names[reason] : "unknown", p2 == 0xFFFFU ? 0 : static_cast<int16_t>(p2));
            break;
        }
        case JNL_RADIO_RESET: snprintf(out, outLen, "%s radio_reset total=%u", timestamp, static_cast<unsigned>(p1)); break;
        case JNL_STALL: snprintf(out, outLen, "%s stall phase=%u timeout=%us", timestamp, static_cast<unsigned>(flags), static_cast<unsigned>(p1)); break;
        case JNL_NTP_SYNC: snprintf(out, outLen, "%s ntp_sync epoch=%lu", timestamp, static_cast<unsigned long>(static_cast<uint32_t>(p2) | (static_cast<uint32_t>(p3) << 16U))); break;
        case JNL_BUSINESS: snprintf(out, outLen, "%s biz subtype=%u a=%d b=%u", timestamp, static_cast<unsigned>(flags), static_cast<int>(p1), static_cast<unsigned>(p2)); break;
        case JNL_RECOVERY: snprintf(out, outLen, "%s recovery action=%u result=%d", timestamp, static_cast<unsigned>(flags), static_cast<int>(p1)); break;
        default: snprintf(out, outLen, "%s kind=%u f=%u a=%d b=%u c=%u", timestamp, static_cast<unsigned>(kind), static_cast<unsigned>(flags), static_cast<int>(p1), static_cast<unsigned>(p2), static_cast<unsigned>(p3)); break;
    }
}
#endif
