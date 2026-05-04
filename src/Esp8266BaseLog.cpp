#include "Esp8266BaseLog.h"
#include "Esp8266BaseConfig.h"
#include <LittleFS.h>
#include <stdarg.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
uint8_t                   Esp8266BaseLog::_level  = ESP8266BASE_LOG_LEVEL;
Esp8266BaseTimeProviderFn Esp8266BaseLog::_timeFn  = nullptr;
Esp8266BaseLogHookFn      Esp8266BaseLog::_hook    = nullptr;
bool                      Esp8266BaseLog::_fileEnabled = false;
uint8_t                   Esp8266BaseLog::_fileLevel = ESP8266BASE_LOG_LEVEL;
uint8_t                   Esp8266BaseLog::_fileRotateFiles = 4;
bool                      Esp8266BaseLog::_fileDirReady = false;
uint32_t                  Esp8266BaseLog::_fileMaxBytes = 0;
uint32_t                  Esp8266BaseLog::_fileCurrentBytes = 0;
char                      Esp8266BaseLog::_filePath[32] = "";

// ----------------------------------------------------------------------------
// 内部常量
// ----------------------------------------------------------------------------
static const char* const LOG_LEVEL_STR[] = { "D", "I", "W", "E" };

// 日志格式缓冲大小（栈分配，不占全局 RAM）
static const int LOG_BUF_SIZE = 128;
static const int LOG_LINE_SIZE = 192;

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

void Esp8266BaseLog::setOutputHook(Esp8266BaseLogHookFn fn) {
    _hook = fn;
}

bool Esp8266BaseLog::enableFileSink(const char* path,
                                    uint32_t maxBytes,
                                    uint8_t fileLevel,
                                    uint8_t rotateFiles) {
    if (!path || path[0] != '/' || strlen(path) >= sizeof(_filePath) || maxBytes < 256) {
        return false;
    }
    if (rotateFiles < 1) rotateFiles = 1;
    if (rotateFiles > 4) rotateFiles = 4;
    strncpy(_filePath, path, sizeof(_filePath) - 1);
    _filePath[sizeof(_filePath) - 1] = '\0';
    _fileMaxBytes = maxBytes;
    _fileLevel = fileLevel;
    _fileRotateFiles = rotateFiles;
    _fileDirReady = false;
    _fileCurrentBytes = 0;

    _fileEnabled = true;
    log(1, "Log ", "file_sink_enabled path=%s max_bytes=%lu rotate_files=%u file_level=%u",
        _filePath, (unsigned long)_fileMaxBytes,
        (unsigned)_fileRotateFiles, (unsigned)_fileLevel);
    return true;
}

void Esp8266BaseLog::disableFileSink() {
    _fileEnabled = false;
    _fileDirReady = false;
    _fileCurrentBytes = 0;
}

void Esp8266BaseLog::setFileSinkLevel(uint8_t level) {
    _fileLevel = level;
}

bool Esp8266BaseLog::isFileSinkEnabled() {
    return _fileEnabled;
}

const char* Esp8266BaseLog::fileSinkPath() {
    return _filePath;
}

uint32_t Esp8266BaseLog::fileSinkMaxBytes() {
    return _fileMaxBytes;
}

uint8_t Esp8266BaseLog::fileSinkRotateFiles() {
    return _fileRotateFiles;
}

uint8_t Esp8266BaseLog::fileSinkLevel() {
    return _fileLevel;
}

uint32_t Esp8266BaseLog::fileSinkSize() {
    if (!_fileEnabled || !_filePath[0] || !LittleFS.exists(_filePath)) return 0;
    File f = LittleFS.open(_filePath, "r");
    if (!f) return 0;
    uint32_t sz = f.size();
    f.close();
    return sz;
}

uint32_t Esp8266BaseLog::fileSinkSegmentSize(uint8_t index) {
    char path[36];
    if (!_fileEnabled || !_segmentPath(index, path, sizeof(path)) || !LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    uint32_t sz = f.size();
    f.close();
    return sz;
}

bool Esp8266BaseLog::clearFileSink() {
    if (!_filePath[0]) return false;
    if (!_ensureFileReady()) return false;
    if (LittleFS.exists(_filePath) && !LittleFS.remove(_filePath)) return false;
    for (uint8_t i = 1; i < _fileRotateFiles; i++) {
        char path[36];
        if (_segmentPath(i, path, sizeof(path)) && LittleFS.exists(path) && !LittleFS.remove(path)) {
            return false;
        }
    }
    // Also remove stale segments left from a previous larger rotateFiles setting.
    for (uint8_t i = _fileRotateFiles; i < 4; i++) {
        char path[36];
        if (_segmentPath(i, path, sizeof(path)) && LittleFS.exists(path) && !LittleFS.remove(path)) {
            return false;
        }
    }
    File f = LittleFS.open(_filePath, "w");
    if (!f) return false;
    f.close();
    _fileCurrentBytes = 0;
    log(1, "Log ", "file_sink_cleared path=%s", _filePath);
    return true;
}

void Esp8266BaseLog::beginBootSession(const char* firmware,
                                      const char* version,
                                      const char* resetReason,
                                      uint32_t bootCount,
                                      uint32_t freeHeap) {
    log(1, "Boot", "============================================================");
    log(1, "Boot", "BOOT SESSION START boot_count=%lu reset_reason=%s firmware=%s version=%s free_heap=%lu",
        (unsigned long)bootCount,
        resetReason ? resetReason : "unknown",
        firmware ? firmware : "",
        version ? version : "",
        (unsigned long)freeHeap);
    log(1, "Boot", "============================================================");
}

void Esp8266BaseLog::enableConfigAudit(bool enabled) {
    Esp8266BaseConfig::enableConfigAudit(enabled);
}

void Esp8266BaseLog::enableConfigReadAudit(bool enabled) {
    Esp8266BaseConfig::enableConfigReadAudit(enabled);
}

const char* Esp8266BaseLog::_timestamp(char* buf, size_t len) {
    if (_timeFn) return _timeFn();
    snprintf(buf, len, "%lu", millis());
    return buf;
}

bool Esp8266BaseLog::_segmentPath(uint8_t index, char* out, size_t len) {
    if (!out || len == 0 || !_filePath[0]) return false;
    int n = (index == 0)
        ? snprintf(out, len, "%s", _filePath)
        : snprintf(out, len, "%s.%u", _filePath, (unsigned)index);
    return n > 0 && (size_t)n < len;
}

bool Esp8266BaseLog::_ensureFileReady() {
    if (_fileDirReady) return true;
    if (!_fileEnabled || !_filePath[0]) return false;
    if (!Esp8266BaseConfig::isReady()) return false;

    const char* slash = strrchr(_filePath, '/');
    if (slash && slash != _filePath) {
        char dir[32];
        size_t n = (size_t)(slash - _filePath);
        if (n >= sizeof(dir)) n = sizeof(dir) - 1;
        memcpy(dir, _filePath, n);
        dir[n] = '\0';
        if (!LittleFS.exists(dir) && !LittleFS.mkdir(dir)) {
            return false;
        }
    }

    _fileCurrentBytes = fileSinkSize();
    _fileDirReady = true;
    return true;
}

bool Esp8266BaseLog::_truncateCurrentFile() {
    if (!_fileEnabled || !_filePath[0]) return false;
    File nf = LittleFS.open(_filePath, "w");
    if (!nf) return false;
    nf.close();
    _fileCurrentBytes = 0;
    return true;
}

bool Esp8266BaseLog::_rotateFile() {
    if (_fileRotateFiles <= 1) {
        return _truncateCurrentFile();
    } else {
        char oldest[36];
        if (_segmentPath(_fileRotateFiles - 1, oldest, sizeof(oldest)) && LittleFS.exists(oldest)) {
            if (!LittleFS.remove(oldest)) return false;
        }

        for (int8_t i = (int8_t)_fileRotateFiles - 2; i >= 0; i--) {
            char from[36];
            char to[36];
            if (!_segmentPath((uint8_t)i, from, sizeof(from))) continue;
            if (!_segmentPath((uint8_t)i + 1, to, sizeof(to))) continue;
            if (LittleFS.exists(from)) {
                if (LittleFS.exists(to) && !LittleFS.remove(to)) return false;
                if (!LittleFS.rename(from, to)) return false;
            }
        }
    }

    return _truncateCurrentFile();
}

bool Esp8266BaseLog::_writeFileLine(const char* line) {
    if (!_fileEnabled || !_filePath[0] || !_fileMaxBytes || !line) return false;
    if (!_ensureFileReady()) return false;

    size_t lineLen = strlen(line) + 1;  // newline
    if (_fileCurrentBytes + lineLen > _fileMaxBytes) {
        if (!_rotateFile() && !_truncateCurrentFile()) return false;
    }

    File f = LittleFS.open(_filePath, "a");
    if (!f) {
        if (!_truncateCurrentFile()) return false;
        f = LittleFS.open(_filePath, "a");
        if (!f) return false;
    }
    size_t written = f.print(line);
    written += f.print('\n');
    f.close();
    if (written != lineLen) {
        _fileCurrentBytes = fileSinkSize();
        return false;
    }
    _fileCurrentBytes += lineLen;
    yield();
    return true;
}

void Esp8266BaseLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    // 运行时等级检查。Serial 与 file sink 可使用不同等级。
    bool serialEnabled = (level >= _level);
    bool fileEnabled = _fileEnabled && (level >= _fileLevel || level >= 2);
    if (!serialEnabled && !fileEnabled && !_hook) return;

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
    const char* levelStr = (level < 4) ? LOG_LEVEL_STR[level] : "?";
    char tsBuf[16];
    const char* ts = _timestamp(tsBuf, sizeof(tsBuf));

    char line[LOG_LINE_SIZE];
    snprintf(line, sizeof(line), "[%s][%s][%s] %s", ts, levelStr, tagBuf, msg);

    if (serialEnabled) Serial.println(line);
    if (_hook) {
        _hook(level, tagBuf, msg, ts, line);
    }
    if (fileEnabled) _writeFileLine(line);
}
