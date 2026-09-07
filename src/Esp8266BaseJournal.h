#pragma once
#include <Arduino.h>

// Compact, append-only diagnostic journal. The on-disk format is intentionally
// versioned without a compatibility reader: files from older experimental
// formats are deleted during begin().

#ifndef ESP8266BASE_JOURNAL_SLOT_COUNT
#define ESP8266BASE_JOURNAL_SLOT_COUNT 8
#endif

#ifndef ESP8266BASE_JOURNAL_SLOT_BYTES
#define ESP8266BASE_JOURNAL_SLOT_BYTES 8192
#endif

#ifndef ESP8266BASE_JOURNAL_SAMPLE_INTERVAL_MS
#define ESP8266BASE_JOURNAL_SAMPLE_INTERVAL_MS 60000UL
#endif

#ifndef ESP8266BASE_JOURNAL_TREND_FLUSH_AGE_MS
#define ESP8266BASE_JOURNAL_TREND_FLUSH_AGE_MS 1800000UL
#endif

#ifndef ESP8266BASE_JOURNAL_EVENT_FLUSH_MS
#define ESP8266BASE_JOURNAL_EVENT_FLUSH_MS 10000UL
#endif

static_assert(ESP8266BASE_JOURNAL_SLOT_COUNT >= 2 && ESP8266BASE_JOURNAL_SLOT_COUNT <= 32,
              "ESP8266BASE_JOURNAL_SLOT_COUNT must be 2..32");
static_assert(ESP8266BASE_JOURNAL_SLOT_BYTES >= 1024 && ESP8266BASE_JOURNAL_SLOT_BYTES <= 65520,
              "ESP8266BASE_JOURNAL_SLOT_BYTES must be 1024..65520");

enum Esp8266BaseJournalKind : uint8_t {
    JNL_SAMPLE = 1,       // 30-minute aggregate: f=sample count, p1=min RSSI, p2=min heap>>6, p3=max lag
    JNL_WIFI_LOST = 2,
    JNL_WIFI_UP = 3,
    JNL_MQTT_ATTEMPT = 4,
    JNL_MQTT_UP = 5,
    JNL_MQTT_CLOSED = 6,
    JNL_RADIO_RESET = 7,
    JNL_STALL = 8,        // f=watchdog phase, p1=timeout seconds
    JNL_NTP_SYNC = 9,
    JNL_BUSINESS = 10,
    JNL_RECOVERY = 11,    // f=action, p1=result/budget reason, p2/p3 optional
};

enum Esp8266BaseJournalMqttReason : uint8_t {
    JNL_REASON_NONE = 0, JNL_REASON_USER_OK = 1, JNL_REASON_UNACCEPTABLE_PROTOCOL = 2,
    JNL_REASON_IDENTIFIER_REJECTED = 3, JNL_REASON_SERVER_UNAVAILABLE = 4,
    JNL_REASON_MALFORMED_CREDENTIALS = 5, JNL_REASON_NOT_AUTHORIZED = 6,
    JNL_REASON_TLS_BAD_FINGERPRINT = 7, JNL_REASON_TCP_DISCONNECTED = 8,
    JNL_REASON_UNKNOWN = 9
};

struct Esp8266BaseJournalStats {
    uint32_t writesSinceBoot;
    uint32_t bytesSinceBoot;
    uint32_t eraseEvents;
    uint32_t ioErrors;
    uint32_t droppedRecords;
    uint8_t slotBytesKb;
    uint8_t slotCount;
};

struct Esp8266BaseJournalSession {
    uint32_t generation;
    uint32_t bootNo;
    uint8_t resetCode;
    uint32_t lastRecordMs;
};

struct Esp8266BaseJournalSummary {
    uint16_t mqttAttempts;
    uint16_t mqttClosed;
    uint8_t mqttClosedReasons[10];
    uint16_t wifiLost;
    uint16_t radioResets;
    uint16_t stalls;
    uint16_t samples;
    uint16_t records;
    uint8_t lastMqttReason;
    uint8_t lastResetCode;
};

class Esp8266BaseJournal {
public:
    static bool begin();
    static void beginBoot(uint32_t bootNo, uint8_t resetCode);
    static void handle();

    static void record(uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2, uint16_t p3);
    static void recordNow(uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2, uint16_t p3);

    static bool getStats(Esp8266BaseJournalStats& stats);
    static uint16_t listSessions(Esp8266BaseJournalSession* out, uint16_t maxCount);
    typedef bool (*RecordVisitor)(uint32_t t, uint8_t kind, uint8_t flags, int16_t p1,
                                  uint16_t p2, uint16_t p3, uint32_t bootNo, uint8_t reset,
                                  void* ctx);
    // offsetRecords is a bounded presentation cursor. Unlike the old format the
    // implementation scans every fixed record in every retained slot.
    static bool dumpTail(uint32_t offsetRecords, uint32_t maxRecords,
                         Esp8266BaseJournalSummary& summary, RecordVisitor visitor, void* ctx);
    static uint8_t diagLevel();
    static bool cachedSummary(Esp8266BaseJournalSummary& out);

    static void formatRecord(uint32_t t, uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2,
                             uint16_t p3, uint32_t bootNo, uint8_t reset,
                             char* out, size_t outLen);

private:
    static bool _flushPending();
    static bool _appendPending(uint16_t start, uint16_t count, uint16_t& written);
    static bool _createSlot(uint32_t generation, bool includeBoot);
    static bool _appendBoot();
    static void _rotate();
    static void _pushEntry(uint8_t kind, uint8_t flags, int16_t p1, uint16_t p2, uint16_t p3,
                           bool event);
    static uint16_t _crc16(const uint8_t* data, uint16_t len);
    static void _slotName(uint32_t generation, char* out, size_t outLen);
};
