#include "Esp8266BaseFileLog.h"
#include "Esp8266BaseConfig.h"
#include <LittleFS.h>

static bool g_enabled = false;
static bool g_dirReady = false;
static Esp8266BaseFileLog::Mode g_mode =
    static_cast<Esp8266BaseFileLog::Mode>(ESP8266BASE_FILELOG_DEFAULT_MODE);
static uint32_t g_currentBytes = 0;
static char g_path[32] = ESP8266BASE_FILELOG_PATH;

#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
static char g_buffer[ESP8266BASE_FILELOG_BUFFER_SIZE];
static uint16_t g_bufferUsed = 0;
static uint32_t g_lastFlushMs = 0;
#endif

static const char* _modeName(Esp8266BaseFileLog::Mode mode) {
    switch (mode) {
        case Esp8266BaseFileLog::WARN: return "WARN";
        case Esp8266BaseFileLog::INFO: return "INFO";
        case Esp8266BaseFileLog::OFF:
        default: return "OFF";
    }
}

static bool _validMode(Esp8266BaseFileLog::Mode mode) {
    if (mode != Esp8266BaseFileLog::OFF &&
        mode != Esp8266BaseFileLog::WARN &&
        mode != Esp8266BaseFileLog::INFO) {
        return false;
    }
    return mode == Esp8266BaseFileLog::OFF || (uint8_t)mode >= ESP8266BASE_LOG_LEVEL;
}

static Esp8266BaseFileLog::Mode _readMode() {
    int32_t raw = Esp8266BaseConfig::getInt(ESP8266BASE_CFG_KEY_FILELOG_MODE, ESP8266BASE_FILELOG_DEFAULT_MODE);
    Esp8266BaseFileLog::Mode mode = static_cast<Esp8266BaseFileLog::Mode>(raw);
    return _validMode(mode)
        ? mode
        : static_cast<Esp8266BaseFileLog::Mode>(ESP8266BASE_FILELOG_DEFAULT_MODE);
}

static bool _ensureDirReady() {
    if (g_dirReady) return true;
    if (!Esp8266BaseConfig::isReady() || !g_path[0] || g_path[0] != '/') return false;

    const char* slash = strrchr(g_path, '/');
    if (slash && slash != g_path) {
        char dir[32];
        size_t n = (size_t)(slash - g_path);
        if (n >= sizeof(dir)) return false;
        memcpy(dir, g_path, n);
        dir[n] = '\0';
        if (!LittleFS.exists(dir) && !LittleFS.mkdir(dir)) {
            ESP8266BASE_LOG_W("FLog", "dir_create_failed path=%s", dir);
            return false;
        }
    }

    File f = LittleFS.open(g_path, "a");
    if (!f) {
        ESP8266BASE_LOG_W("FLog", "segment_prepare_failed path=%s rotate=%u",
                          g_path, (unsigned)ESP8266BASE_FILELOG_ROTATE_FILES);
        return false;
    }
    f.close();
    g_currentBytes = Esp8266BaseFileLog::size();
    g_dirReady = true;
    return true;
}

static bool _truncateSegment(uint8_t index) {
    char seg[36];
    if (!Esp8266BaseFileLog::segmentPath(index, seg, sizeof(seg))) return false;
    File f = LittleFS.open(seg, "w");
    if (!f) return false;
    f.close();
    return true;
}

static bool _truncateCurrent() {
    if (!g_path[0]) return false;
    bool ok = _truncateSegment(0);
    if (ok) g_currentBytes = 0;
    return ok;
}

static bool _rotateFile() {
    if (ESP8266BASE_FILELOG_ROTATE_FILES <= 1) {
        return _truncateCurrent();
    }

    char oldest[36];
    if (Esp8266BaseFileLog::segmentPath(ESP8266BASE_FILELOG_ROTATE_FILES - 1, oldest, sizeof(oldest)) &&
        LittleFS.exists(oldest) && !LittleFS.remove(oldest)) {
        return false;
    }

    for (int8_t i = (int8_t)ESP8266BASE_FILELOG_ROTATE_FILES - 2; i >= 0; i--) {
        char from[36];
        char to[36];
        if (!Esp8266BaseFileLog::segmentPath((uint8_t)i, from, sizeof(from))) continue;
        if (!Esp8266BaseFileLog::segmentPath((uint8_t)i + 1, to, sizeof(to))) continue;
        if (LittleFS.exists(from)) {
            if (LittleFS.exists(to) && !LittleFS.remove(to)) return false;
            if (!LittleFS.rename(from, to)) return false;
        }
    }

    return _truncateCurrent();
}

#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
static bool _writeBytes(const char* data, size_t len) {
    if (!g_enabled || !g_path[0] || !data) return false;
    if (!_ensureDirReady()) return false;

    size_t offset = 0;
    while (offset < len) {
        if (g_currentBytes >= ESP8266BASE_FILELOG_MAX_BYTES) {
            if (!_rotateFile() && !_truncateCurrent()) return false;
        }

        uint32_t room = ESP8266BASE_FILELOG_MAX_BYTES - g_currentBytes;
        size_t chunk = len - offset;
        if (chunk > room) chunk = room;
        if (chunk == 0) return false;

        File f = LittleFS.open(g_path, "a");
        if (!f) {
            if (!_truncateCurrent()) return false;
            f = LittleFS.open(g_path, "a");
            if (!f) return false;
        }
        size_t written = f.write((const uint8_t*)data + offset, chunk);
        f.close();
        if (written != chunk) {
            g_currentBytes = Esp8266BaseFileLog::size();
            return false;
        }
        g_currentBytes += (uint32_t)written;
        offset += written;
        yield();
    }
    return true;
}
#endif

static bool _writeLineNow(const char* line) {
    if (!line) return false;
    if (!_ensureDirReady()) return false;

    size_t lineLen = strlen(line) + 1;
    if (g_currentBytes + lineLen > ESP8266BASE_FILELOG_MAX_BYTES) {
        if (!_rotateFile() && !_truncateCurrent()) return false;
    }

    File f = LittleFS.open(g_path, "a");
    if (!f) {
        if (!_truncateCurrent()) return false;
        f = LittleFS.open(g_path, "a");
        if (!f) return false;
    }
    size_t written = f.print(line);
    written += f.print('\n');
    f.close();
    if (written != lineLen) {
        g_currentBytes = Esp8266BaseFileLog::size();
        return false;
    }
    g_currentBytes += lineLen;
    yield();
    return true;
}

static bool _writeLineBuffered(const char* line) {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    if (!line) return false;
    if (!Esp8266BaseFileLog::bufferEnabled()) return _writeLineNow(line);

    uint32_t now = millis();
    if (g_bufferUsed > 0 &&
        (uint32_t)(now - g_lastFlushMs) >= ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS) {
        Esp8266BaseFileLog::flush();
    }

    size_t lineLen = strlen(line);
    size_t needed = lineLen + 1;
    if (needed > ESP8266BASE_FILELOG_BUFFER_SIZE) {
        Esp8266BaseFileLog::flush();
        return _writeLineNow(line);
    }
    if (g_bufferUsed + needed > ESP8266BASE_FILELOG_BUFFER_SIZE) {
        Esp8266BaseFileLog::flush();
    }

    if (g_bufferUsed == 0) g_lastFlushMs = now;
    memcpy(g_buffer + g_bufferUsed, line, lineLen);
    g_bufferUsed += (uint16_t)lineLen;
    g_buffer[g_bufferUsed++] = '\n';

    if (g_bufferUsed >= ESP8266BASE_FILELOG_BUFFER_SIZE ||
        (uint32_t)(millis() - g_lastFlushMs) >= ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS) {
        return Esp8266BaseFileLog::flush();
    }
    return true;
#else
    return _writeLineNow(line);
#endif
}

static void _lineSink(uint8_t level,
                      const char*,
                      const char*,
                      const char*,
                      const char* line) {
    if (!g_enabled || !line) return;
    if (g_mode == Esp8266BaseFileLog::WARN && level < 2) return;
    if (g_mode == Esp8266BaseFileLog::INFO && level < 1) return;
    if (g_mode != Esp8266BaseFileLog::WARN && g_mode != Esp8266BaseFileLog::INFO) return;

    if (level >= 2) {
        Esp8266BaseFileLog::flush();
        _writeLineNow(line);
    } else {
        _writeLineBuffered(line);
    }
}

bool Esp8266BaseFileLog::begin() {
    Esp8266BaseLog::_setInternalHook(_lineSink);
    strncpy(g_path, ESP8266BASE_FILELOG_PATH, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    return setMode(_readMode());
}

bool Esp8266BaseFileLog::setMode(Mode mode) {
    if (!_validMode(mode)) return false;

    Mode oldMode = g_mode;
    if (mode == OFF) {
        flush();
        bool saved = Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_FILELOG_MODE, (int32_t)mode);
        ESP8266BASE_LOG_W("FLog", "mode_change old=%s new=%s saved=%s",
                          _modeName(oldMode), _modeName(mode), saved ? "yes" : "no");
        g_enabled = false;
        g_mode = OFF;
        g_dirReady = false;
        g_currentBytes = 0;
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
        g_bufferUsed = 0;
        g_lastFlushMs = 0;
#endif
        return saved;
    }

    if (!Esp8266BaseConfig::isReady() || !g_path[0] || g_path[0] != '/') return false;
    if (g_enabled) flush();

    g_mode = mode;
    if (Esp8266BaseLog::runtimeLevel() > (uint8_t)g_mode) {
        Esp8266BaseLog::setRuntimeLevel((uint8_t)g_mode);
    }
    g_dirReady = false;
    g_currentBytes = 0;
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    g_bufferUsed = 0;
    g_lastFlushMs = 0;
#endif
    g_enabled = true;
    bool saved = Esp8266BaseConfig::setInt(ESP8266BASE_CFG_KEY_FILELOG_MODE, (int32_t)g_mode);
    ESP8266BASE_LOG_W("FLog", "mode_change old=%s new=%s saved=%s path=%s max_bytes=%lu rotate_files=%u buffer_size=%u flush_interval_ms=%lu",
                      _modeName(oldMode),
                      _modeName(g_mode),
                      saved ? "yes" : "no",
                      g_path,
                      (unsigned long)ESP8266BASE_FILELOG_MAX_BYTES,
                      (unsigned)ESP8266BASE_FILELOG_ROTATE_FILES,
                      (unsigned)bufferSize(),
                      (unsigned long)flushIntervalMs());
    return saved;
}

Esp8266BaseFileLog::Mode Esp8266BaseFileLog::mode() {
    return g_mode;
}

const char* Esp8266BaseFileLog::modeName() {
    return _modeName(g_mode);
}

bool Esp8266BaseFileLog::isEnabled() {
    return g_enabled;
}

bool Esp8266BaseFileLog::flush() {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    if (g_bufferUsed == 0) return true;
    bool ok = _writeBytes(g_buffer, g_bufferUsed);
    if (ok) {
        g_bufferUsed = 0;
        g_lastFlushMs = 0;
    }
    return ok;
#else
    return true;
#endif
}

bool Esp8266BaseFileLog::clear() {
    if (!Esp8266BaseConfig::isReady() || !g_path[0]) return false;
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    g_bufferUsed = 0;
    g_lastFlushMs = 0;
#endif
    if (!_ensureDirReady()) return false;
    for (uint8_t i = 0; i < ESP8266BASE_FILELOG_ROTATE_FILES; i++) {
        if (!_truncateSegment(i)) return false;
    }
    for (uint8_t i = ESP8266BASE_FILELOG_ROTATE_FILES; i < 4; i++) {
        char seg[36];
        if (segmentPath(i, seg, sizeof(seg)) && LittleFS.exists(seg) && !LittleFS.remove(seg)) {
            return false;
        }
    }
    g_currentBytes = 0;
    ESP8266BASE_LOG_I("FLog", "cleared path=%s", g_path);
    return true;
}

void Esp8266BaseFileLog::handle() {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    if (g_bufferUsed > 0 &&
        (uint32_t)(millis() - g_lastFlushMs) >= ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS) {
        flush();
    }
#endif
}

const char* Esp8266BaseFileLog::path() {
    return g_path;
}

uint32_t Esp8266BaseFileLog::maxBytes() {
    return ESP8266BASE_FILELOG_MAX_BYTES;
}

uint8_t Esp8266BaseFileLog::rotateFiles() {
    return ESP8266BASE_FILELOG_ROTATE_FILES;
}

uint32_t Esp8266BaseFileLog::size() {
    return segmentSize(0);
}

uint32_t Esp8266BaseFileLog::segmentSize(uint8_t index) {
    char seg[36];
    if (!segmentPath(index, seg, sizeof(seg)) || !LittleFS.exists(seg)) return 0;
    File f = LittleFS.open(seg, "r");
    if (!f) return 0;
    uint32_t sz = f.size();
    f.close();
    return sz;
}

bool Esp8266BaseFileLog::bufferEnabled() {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    return g_enabled && g_mode == INFO;
#else
    return false;
#endif
}

uint16_t Esp8266BaseFileLog::bufferSize() {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    return ESP8266BASE_FILELOG_BUFFER_SIZE;
#else
    return 0;
#endif
}

uint16_t Esp8266BaseFileLog::bufferUsed() {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    return g_bufferUsed;
#else
    return 0;
#endif
}

uint32_t Esp8266BaseFileLog::flushIntervalMs() {
#if ESP8266BASE_FILELOG_BUFFER_SIZE > 0
    return ESP8266BASE_FILELOG_FLUSH_INTERVAL_MS;
#else
    return 0;
#endif
}

bool Esp8266BaseFileLog::segmentPath(uint8_t index, char* out, size_t len) {
    if (!out || len == 0 || !g_path[0]) return false;
    int n = (index == 0)
        ? snprintf(out, len, "%s", g_path)
        : snprintf(out, len, "%s.%u", g_path, (unsigned)index);
    return n > 0 && (size_t)n < len;
}
