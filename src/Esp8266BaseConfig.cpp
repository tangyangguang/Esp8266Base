#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include <LittleFS.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
Esp8266BaseConfig::DeferredEntry
    Esp8266BaseConfig::_deferred[ESP8266BASE_CFG_DEFERRED_SIZE];
bool Esp8266BaseConfig::_ready = false;
bool Esp8266BaseConfig::_auditEnabled = false;
bool Esp8266BaseConfig::_readAuditEnabled = false;
uint32_t Esp8266BaseConfig::_lastDeferredFlushMs = 0;

// ----------------------------------------------------------------------------
// begin
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::begin() {
    // 初始化 deferred 队列
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        _deferred[i].used = false;
    }
    _lastDeferredFlushMs = millis();

    if (!LittleFS.begin()) {
        ESP8266BASE_LOG_W("Cfg ", "littlefs_mount_failed retrying");
        delay(50);
        if (!LittleFS.begin()) {
#if ESP8266BASE_CFG_FORMAT_ON_FAIL
        ESP8266BASE_LOG_W("Cfg ", "littlefs_mount_failed formatting_enabled=yes");
        LittleFS.format();
        if (!LittleFS.begin()) {
            ESP8266BASE_LOG_E("Cfg ", "littlefs_mount_failed_after_format");
            _ready = false;
            return false;
        }
        ESP8266BASE_LOG_I("Cfg ", "littlefs_formatted result=success");
#else
            ESP8266BASE_LOG_E("Cfg ", "littlefs_mount_failed formatting_enabled=no config_disabled=yes");
            _ready = false;
            return false;
#endif
        }
    }

    _ready = true;
    ESP8266BASE_LOG_I("Cfg ", "config_storage_ready pending_writes=0/%d deferred_flush_interval=%lums",
                      ESP8266BASE_CFG_DEFERRED_SIZE,
                      (unsigned long)ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS);
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
    char bak[ESP8266BASE_CFG_KEY_MAX + 11];
    char tmp[ESP8266BASE_CFG_KEY_MAX + 11];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (!LittleFS.exists(path)) {
        if (LittleFS.exists(bak)) {
            LittleFS.rename(bak, path);
        } else if (LittleFS.exists(tmp)) {
            LittleFS.rename(tmp, path);
        } else {
            return false;
        }
    }
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t n = f.readBytes(out, len - 1);
    out[n] = '\0';
    f.close();
    return true;
}

bool Esp8266BaseConfig::_writeRaw(const char* path, const char* value) {
    char tmp[ESP8266BASE_CFG_KEY_MAX + 11];
    char bak[ESP8266BASE_CFG_KEY_MAX + 11];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);

    LittleFS.remove(tmp);
    File f = LittleFS.open(tmp, "w");
    if (!f) {
        ESP8266BASE_LOG_E("Cfg ", "open failed path=%s", tmp);
        return false;
    }
    size_t expected = strlen(value);
    size_t written = f.print(value);
    f.flush();
    f.close();
    if (written != expected) {
        LittleFS.remove(tmp);
        ESP8266BASE_LOG_E("Cfg ", "write failed path=%s written=%u expected=%u",
                          tmp, (unsigned)written, (unsigned)expected);
        return false;
    }

    char verify[ESP8266BASE_CFG_STR_MAX + 1] = "";
    File vf = LittleFS.open(tmp, "r");
    if (!vf) {
        LittleFS.remove(tmp);
        ESP8266BASE_LOG_E("Cfg ", "verify_open_failed path=%s", tmp);
        return false;
    }
    size_t n = vf.readBytes(verify, sizeof(verify) - 1);
    verify[n] = '\0';
    vf.close();
    if (strcmp(verify, value) != 0) {
        LittleFS.remove(tmp);
        ESP8266BASE_LOG_E("Cfg ", "verify_failed path=%s", tmp);
        return false;
    }

    LittleFS.remove(bak);
    bool hadOld = LittleFS.exists(path);
    if (hadOld && !LittleFS.rename(path, bak)) {
        LittleFS.remove(tmp);
        ESP8266BASE_LOG_E("Cfg ", "backup_rename_failed path=%s backup=%s", path, bak);
        return false;
    }
    if (!LittleFS.rename(tmp, path)) {
        if (hadOld && LittleFS.exists(bak)) {
            if (LittleFS.exists(path)) {
                LittleFS.remove(path);
            }
            LittleFS.rename(bak, path);
        }
        LittleFS.remove(tmp);
        ESP8266BASE_LOG_E("Cfg ", "commit_rename_failed tmp=%s path=%s", tmp, path);
        return false;
    }
    LittleFS.remove(bak);
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
            if (_auditEnabled) {
                ESP8266BASE_LOG_I("Cfg ", "config_audit op=deferred_update key=%s type=%s new=%ld mode=deferred",
                                  key, type == 1 ? "int" : "bool",
                                  type == 1 ? (long)iv : (long)(bv ? 1 : 0));
            }
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
            if (_auditEnabled) {
                ESP8266BASE_LOG_I("Cfg ", "config_audit op=deferred_enqueue key=%s type=%s new=%ld mode=deferred",
                                  key, type == 1 ? "int" : "bool",
                                  type == 1 ? (long)iv : (long)(bv ? 1 : 0));
            }
            return true;
        }
    }
    ESP8266BASE_LOG_W("Cfg ", "deferred queue full key=%s", key);
    return false;
}

void Esp8266BaseConfig::_flushOne() {
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (_deferred[i].used) {
            char key[ESP8266BASE_CFG_KEY_MAX + 1];
            strncpy(key, _deferred[i].key, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
            uint8_t type = _deferred[i].type;
            int32_t iv = _deferred[i].intVal;
            bool bv = _deferred[i].boolVal;
            bool ok = false;
            if (_deferred[i].type == 1) {
                ok = setInt(_deferred[i].key, _deferred[i].intVal);
            } else if (_deferred[i].type == 2) {
                ok = setBool(_deferred[i].key, _deferred[i].boolVal);
            }
            if (ok) {
                _deferred[i].used = false;
            }
            _lastDeferredFlushMs = millis();
            if (_auditEnabled) {
                ESP8266BASE_LOG_I("Cfg ", "config_audit op=flush_one key=%s type=%s value=%ld result=%s",
                                  key, type == 1 ? "int" : "bool",
                                  type == 1 ? (long)iv : (long)(bv ? 1 : 0),
                                  ok ? "success" : "failed");
            }
            return;  // 每次最多刷 1 条
        }
    }
}

bool Esp8266BaseConfig::_setStrInternal(const char* op, const char* key, const char* value) {
    if (!_ready) {
        ESP8266BASE_LOG_W("Cfg ", "config_write_rejected op=%s key=%s reason=not_ready",
                          op, key ? key : "(null)");
        return false;
    }

    if (!value) {
        ESP8266BASE_LOG_W("Cfg ", "%s null value key=%s", op, key);
        return false;
    }
    if (strlen(value) > ESP8266BASE_CFG_STR_MAX) {
        ESP8266BASE_LOG_W("Cfg ", "%s value too long key=%s len=%d",
                          op, key, strlen(value));
        return false;
    }

    char path[ESP8266BASE_CFG_KEY_MAX + 6];  // "/cfg_" + key
    if (!_buildPath(key, path, sizeof(path))) {
        ESP8266BASE_LOG_W("Cfg ", "%s invalid key", op);
        return false;
    }

    char oldVal[ESP8266BASE_CFG_STR_MAX + 1] = "";
    bool hadOld = _readRaw(path, oldVal, sizeof(oldVal));
    bool changed = !hadOld || strcmp(oldVal, value) != 0;
    if (!changed) {
        if (_auditEnabled) {
            ESP8266BASE_LOG_I("Cfg ", "config_audit op=%s key=%s old=%s new=%s changed=no_change mode=immediate result=skipped",
                              op, key, oldVal, value);
        }
        return true;
    }

    bool ok = _writeRaw(path, value);
    if (_auditEnabled || !ok) {
        ESP8266BASE_LOG_I("Cfg ", "config_audit op=%s key=%s old=%s new=%s changed=changed mode=immediate result=%s",
                          op, key, hadOld ? oldVal : "(none)", value,
                          ok ? "success" : "failed");
    }
    return ok;
}

void Esp8266BaseConfig::_auditRead(const char* op, const char* key, const char* value, bool found) {
    if (!_readAuditEnabled) return;
#if ESP8266BASE_LOG_LEVEL <= ESP8266BASE_CFG_READ_AUDIT_LEVEL
    Esp8266BaseLog::log(ESP8266BASE_CFG_READ_AUDIT_LEVEL,
                        "Cfg ",
                        "config_audit op=%s key=%s found=%s value=%s",
                        op, key ? key : "(null)", found ? "yes" : "no",
                        value ? value : "");
#endif
}

// ----------------------------------------------------------------------------
// string
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setStr(const char* key, const char* value) {
    return _setStrInternal("setStr", key, value);
}

bool Esp8266BaseConfig::_getStrInternal(const char* key, char* out, size_t len, const char* def, bool audit) {
    if (!out || len == 0) return false;

    // 无论成功与否都要保证 out 有合法字符串
    auto _setDefault = [&]() {
        if (def) { strncpy(out, def, len - 1); out[len - 1] = '\0'; }
        else     { out[0] = '\0'; }
    };

    if (!_ready) { _setDefault(); if (audit) _auditRead("getStr", key, out, false); return false; }

    char path[ESP8266BASE_CFG_KEY_MAX + 6];
    if (!_buildPath(key, path, sizeof(path))) {
        _setDefault();
        if (audit) _auditRead("getStr", key, out, false);
        return false;
    }

    if (_readRaw(path, out, len)) {
        if (audit) _auditRead("getStr", key, out, true);
        return true;
    }

    _setDefault();
    if (audit) _auditRead("getStr", key, out, false);
    return false;
}

bool Esp8266BaseConfig::getStr(const char* key, char* out, size_t len, const char* def) {
    return _getStrInternal(key, out, len, def, true);
}

// ----------------------------------------------------------------------------
// int32
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setInt(const char* key, int32_t value) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    return _setStrInternal("setInt", key, buf);
}

int32_t Esp8266BaseConfig::getInt(const char* key, int32_t def) {
    char buf[12] = "";
    bool found = _getStrInternal(key, buf, sizeof(buf), nullptr, false);
    if (!found || buf[0] == '\0') {
        char val[12];
        snprintf(val, sizeof(val), "%ld", (long)def);
        _auditRead("getInt", key, val, false);
        return def;
    }
    int32_t v = (int32_t)atol(buf);
    char val[12];
    snprintf(val, sizeof(val), "%ld", (long)v);
    _auditRead("getInt", key, val, true);
    return v;
}

// ----------------------------------------------------------------------------
// bool
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setBool(const char* key, bool value) {
    return _setStrInternal("setBool", key, value ? "1" : "0");
}

bool Esp8266BaseConfig::getBool(const char* key, bool def) {
    char buf[4] = "";
    bool found = _getStrInternal(key, buf, sizeof(buf), nullptr, false);
    if (!found || buf[0] == '\0') {
        _auditRead("getBool", key, def ? "true" : "false", false);
        return def;
    }
    bool v = (buf[0] == '1');
    _auditRead("getBool", key, v ? "true" : "false", true);
    return v;
}

// ----------------------------------------------------------------------------
// deferred
// ----------------------------------------------------------------------------
bool Esp8266BaseConfig::setIntDeferred(const char* key, int32_t value) {
    if (!_ready) {
        ESP8266BASE_LOG_W("Cfg ", "deferred_write_rejected key=%s reason=not_ready", key ? key : "(null)");
        return false;
    }
    if (!key || strlen(key) == 0 || strlen(key) > ESP8266BASE_CFG_KEY_MAX) {
        ESP8266BASE_LOG_W("Cfg ", "deferred_write_rejected key=%s reason=invalid_key", key ? key : "(null)");
        return false;
    }
    return _enqueue(key, value, false, 1);
}

bool Esp8266BaseConfig::setBoolDeferred(const char* key, bool value) {
    if (!_ready) {
        ESP8266BASE_LOG_W("Cfg ", "deferred_write_rejected key=%s reason=not_ready", key ? key : "(null)");
        return false;
    }
    if (!key || strlen(key) == 0 || strlen(key) > ESP8266BASE_CFG_KEY_MAX) {
        ESP8266BASE_LOG_W("Cfg ", "deferred_write_rejected key=%s reason=invalid_key", key ? key : "(null)");
        return false;
    }
    return _enqueue(key, 0, value, 2);
}

// ----------------------------------------------------------------------------
// handle / flush
// ----------------------------------------------------------------------------
void Esp8266BaseConfig::handle() {
    if (!_ready) return;
    if (ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS > 0 &&
        (uint32_t)(millis() - _lastDeferredFlushMs) < ESP8266BASE_CFG_DEFERRED_FLUSH_INTERVAL_MS) {
        return;
    }
    _flushOne();
}

bool Esp8266BaseConfig::flush() {
    if (!_ready) return false;
    bool allOk = true;
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        if (_deferred[i].used) {
            bool ok = false;
            if (_deferred[i].type == 1) ok = setInt(_deferred[i].key, _deferred[i].intVal);
            else if (_deferred[i].type == 2) ok = setBool(_deferred[i].key, _deferred[i].boolVal);
            if (_auditEnabled || !ok) {
                ESP8266BASE_LOG_I("Cfg ", "config_audit op=flush key=%s result=%s",
                                  _deferred[i].key, ok ? "success" : "failed");
            }
            if (ok) {
                _deferred[i].used = false;
            } else {
                allOk = false;
            }
            _lastDeferredFlushMs = millis();
        }
    }
    return allOk;
}

bool Esp8266BaseConfig::clearAll() {
    if (!_ready) return false;
    for (int i = 0; i < ESP8266BASE_CFG_DEFERRED_SIZE; i++) {
        _deferred[i].used = false;
    }

    bool ok = true;
    while (true) {
        char removeName[32] = "";
        Dir dir = LittleFS.openDir("/");
        while (dir.next()) {
            char name[32];
            strncpy(name, dir.fileName().c_str(), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            if (strncmp(name, "/cfg_", 5) == 0 || strncmp(name, "cfg_", 4) == 0) {
                strncpy(removeName, name, sizeof(removeName) - 1);
                removeName[sizeof(removeName) - 1] = '\0';
                break;
            }
        }
        if (removeName[0] == '\0') break;
        if (!LittleFS.remove(removeName)) {
            ok = false;
            ESP8266BASE_LOG_W("Cfg ", "remove failed path=%s", removeName);
            break;
        }
        yield();
    }

    ESP8266BASE_LOG_I("Cfg ", "clear_all_config_files result=%s", ok ? "success" : "partial_failure");
    return ok;
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

void Esp8266BaseConfig::enableConfigAudit(bool enabled) {
    _auditEnabled = enabled;
    ESP8266BASE_LOG_I("Cfg ", "config_audit_write enabled=%s", enabled ? "yes" : "no");
}

void Esp8266BaseConfig::enableConfigReadAudit(bool enabled) {
    _readAuditEnabled = enabled;
    ESP8266BASE_LOG_I("Cfg ", "config_audit_read enabled=%s", enabled ? "yes" : "no");
}

bool Esp8266BaseConfig::isConfigAuditEnabled() {
    return _auditEnabled;
}

bool Esp8266BaseConfig::isConfigReadAuditEnabled() {
    return _readAuditEnabled;
}
