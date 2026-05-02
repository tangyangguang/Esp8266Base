#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include <LittleFS.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
Esp8266BaseConfig::DeferredEntry
    Esp8266BaseConfig::_deferred[ESP8266BASE_CFG_DEFERRED_SIZE];
bool Esp8266BaseConfig::_ready = false;

// 用于读写的共享临时缓冲（与 Web 模块共用，不重复计入预算）
// 注意：只在 handle/begin 期间使用，非重入
static char _cfgBuf[ESP8266BASE_CFG_STR_MAX + 1];

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::begin() {
    // 初始化 deferred 队列
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        _deferred[i].used = false;
    }

    if (!LittleFS.begin()) {
        // First boot or corrupted FS: format and retry
        ESP8266BASE_LOG_W("Cfg ", "LittleFS mount failed, formatting...");
        LittleFS.format();
        if (!LittleFS.begin()) {
            ESP8266BASE_LOG_E("Cfg ", "LittleFS mount failed after format");
            _ready = false;
            return false;
        }
        ESP8266BASE_LOG_I("Cfg ", "LittleFS formatted OK");
    }

    _ready = true;
    ESP8266BASE_LOG_I("Cfg ", "ready=1 pending=0/%d", ESP8266BASE_CFG_DEFERRED_SIZE);
    return true;
}

// ----------------------------------------------------------------------------
// 内部辅助
// ----------------------------------------------------------------------------

bool Esp8266BaseConfig::_buildPath(const char* key, char* path, size_t pathLen) {
    if (!key) return false;
    size_t klen = strlen(key);
    if (klen == 0 || klen > ESP8266BASE_CFG_KEY_MAX) return false;
    snprintf(path, pathLen, "/cfg_%s", key);
    return true;
}

bool Esp8266BaseConfig::_readRaw(const char* path, char* out, size_t len) {
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t n = f.readBytes(out, len - 1);
    out[n] = '\0';
    f.close();
    return true;
}

bool Esp8266BaseConfig::_writeRaw(const char* path, const char* value) {
    File f = LittleFS.open(path, "w");
    if (!f) {
        ESP8266BASE_LOG_E("Cfg ", "open failed path=%s", path);
        return false;
    }
    f.print(value);
    f.close();
    yield();   // Flash 写入后让出 CPU，给 SDK 喂狗
    return true;
}

bool Esp8266BaseConfig::_enqueue(const char* key, int32_t iv, bool bv, uint8_t type) {
    // 若 key 已在队列中，直接覆盖值（防止重复累积）
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (_deferred[i].used && strcmp(_deferred[i].key, key) == 0) {
            _deferred[i].intVal  = iv;
            _deferred[i].boolVal = bv;
            _deferred[i].type    = type;
            return true;
        }
    }
    // 找空槽
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (!_deferred[i].used) {
            strncpy(_deferred[i].key, key, ESP8266BASE_CFG_KEY_MAX);
            _deferred[i].key[ESP8266BASE_CFG_KEY_MAX] = '\0';
            _deferred[i].intVal  = iv;
            _deferred[i].boolVal = bv;
            _deferred[i].type    = type;
            _deferred[i].used    = true;
            return true;
        }
    }
    ESP8266BASE_LOG_W("Cfg ", "deferred queue full key=%s", key);
    return false;
}

void Esp8266BaseConfig::_flushOne() {
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (_deferred[i].used) {
            if (_deferred[i].type == 1) {
                setInt(_deferred[i].key, _deferred[i].intVal);
            } else if (_deferred[i].type == 2) {
                setBool(_deferred[i].key, _deferred[i].boolVal);
            }
            _deferred[i].used = false;
            return;  // 每次最多刷 1 条
        }
    }
}

// ----------------------------------------------------------------------------
// string
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setStr(const char* key, const char* value) {
    if (!_ready) return false;

    if (!value) {
        ESP8266BASE_LOG_W("Cfg ", "setStr null value key=%s", key);
        return false;
    }
    if (strlen(value) > ESP8266BASE_CFG_STR_MAX) {
        ESP8266BASE_LOG_W("Cfg ", "setStr value too long key=%s len=%d",
                          key, strlen(value));
        return false;
    }

    char path[ESP8266BASE_CFG_KEY_MAX + 6];  // "/cfg_" + key
    if (!_buildPath(key, path, sizeof(path))) {
        ESP8266BASE_LOG_W("Cfg ", "setStr invalid key");
        return false;
    }

    // 写前比较，值未变则跳过 Flash 写入
    if (_readRaw(path, _cfgBuf, sizeof(_cfgBuf))) {
        if (strcmp(_cfgBuf, value) == 0) {
            return true;  // 无变化，不写 Flash
        }
    }

    return _writeRaw(path, value);
}

bool Esp8266BaseConfig::getStr(const char* key, char* out, size_t len, const char* def) {
    if (!out || len == 0) return false;

    // 无论成功与否都要保证 out 有合法字符串
    auto _setDefault = [&]() {
        if (def) { strncpy(out, def, len - 1); out[len - 1] = '\0'; }
        else     { out[0] = '\0'; }
    };

    if (!_ready) { _setDefault(); return false; }

    char path[ESP8266BASE_CFG_KEY_MAX + 6];
    if (!_buildPath(key, path, sizeof(path))) { _setDefault(); return false; }

    if (_readRaw(path, out, len)) return true;

    _setDefault();
    return false;
}

// ----------------------------------------------------------------------------
// int32
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setInt(const char* key, int32_t value) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    return setStr(key, buf);
}

int32_t Esp8266BaseConfig::getInt(const char* key, int32_t def) {
    char buf[12] = "";
    if (!getStr(key, buf, sizeof(buf), nullptr)) return def;
    if (buf[0] == '\0') return def;
    return (int32_t)atol(buf);
}

// ----------------------------------------------------------------------------
// bool
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setBool(const char* key, bool value) {
    return setStr(key, value ? "1" : "0");
}

bool Esp8266BaseConfig::getBool(const char* key, bool def) {
    char buf[4] = "";
    if (!getStr(key, buf, sizeof(buf), nullptr)) return def;
    if (buf[0] == '\0') return def;
    return (buf[0] == '1');
}

// ----------------------------------------------------------------------------
// deferred
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setIntDeferred(const char* key, int32_t value) {
    if (!_ready) return false;
    if (!key || strlen(key) > ESP8266BASE_CFG_KEY_MAX) return false;
    return _enqueue(key, value, false, 1);
}

bool Esp8266BaseConfig::setBoolDeferred(const char* key, bool value) {
    if (!_ready) return false;
    if (!key || strlen(key) > ESP8266BASE_CFG_KEY_MAX) return false;
    return _enqueue(key, 0, value, 2);
}

// ----------------------------------------------------------------------------
// handle / flush
// ----------------------------------------------------------------------------
void Esp8266BaseConfig::handle() {
    if (!_ready) return;
    _flushOne();
}

bool Esp8266BaseConfig::flush() {
    if (!_ready) return false;
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (_deferred[i].used) {
            if (_deferred[i].type == 1) setInt(_deferred[i].key, _deferred[i].intVal);
            else if (_deferred[i].type == 2) setBool(_deferred[i].key, _deferred[i].boolVal);
            _deferred[i].used = false;
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 辅助查询
// ----------------------------------------------------------------------------
uint8_t Esp8266BaseConfig::pendingCount() {
    uint8_t n = 0;
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (_deferred[i].used) n++;
    }
    return n;
}

bool Esp8266BaseConfig::isReady() {
    return _ready;
}
