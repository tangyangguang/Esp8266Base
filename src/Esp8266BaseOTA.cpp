#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_OTA
#include "Esp8266BaseOTA.h"
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseWatchdog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseFileLog.h"
#include "Esp8266BaseUtil.h"
#include <Updater.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool Esp8266BaseOTA::_inProgress = false;
bool Esp8266BaseOTA::_rejected = false;
bool Esp8266BaseOTA::_started = false;
bool Esp8266BaseOTA::_watchdogPaused = false;
bool Esp8266BaseOTA::_updateStarted = false;
uint16_t Esp8266BaseOTA::_status = 200;
uint32_t Esp8266BaseOTA::_startedMs = 0;
uint32_t Esp8266BaseOTA::_uploadedBytes = 0;
uint32_t Esp8266BaseOTA::_requestBytes = 0;
uint8_t  Esp8266BaseOTA::_lastProgressPct = 0;
const char* Esp8266BaseOTA::_failureMessage = "Upload failed";

static uint32_t _elapsedMs(uint32_t startedMs) {
    return startedMs ? (uint32_t)(millis() - startedMs) : 0;
}

static void _formatSeconds(uint32_t ms, char* out, size_t len) {
    if (!out || len == 0) return;
    uint32_t centis = (ms + 5UL) / 10UL;
    snprintf(out, len, "%lu.%02lus",
             (unsigned long)(centis / 100UL),
             (unsigned long)(centis % 100UL));
}

static void _formatRate(uint32_t bytes, uint32_t ms, char* out, size_t len) {
    if (!out || len == 0) return;
    if (ms == 0) {
        strncpy(out, "0 B/s", len - 1);
        out[len - 1] = '\0';
        return;
    }
    uint32_t bps = (uint32_t)(((uint64_t)bytes * 1000ULL) / (uint64_t)ms);
    if (bps < 1024UL) {
        snprintf(out, len, "%lu B/s", (unsigned long)bps);
    } else {
        uint32_t kb10 = (uint32_t)(((uint64_t)bps * 10ULL + 512ULL) / 1024ULL);
        snprintf(out, len, "%lu.%lu KB/s",
                 (unsigned long)(kb10 / 10UL),
                 (unsigned long)(kb10 % 10UL));
    }
}

static uint32_t _readLe32(const uint8_t* p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool _isLikelyEsp8266Firmware(const uint8_t* data, size_t len, const char** reason) {
    if (reason) *reason = "ok";
    if (!data || len < 16) {
        if (reason) *reason = "header_too_short";
        return false;
    }
    if (data[0] != 0xE9) {
        if (reason) *reason = "bad_magic";
        return false;
    }
    if (data[1] == 0 || data[1] > 16) {
        if (reason) *reason = "bad_segment_count";
        return false;
    }
    if (data[2] > 3) {
        if (reason) *reason = "bad_flash_mode";
        return false;
    }

    uint32_t firstAddr = _readLe32(data + 8);
    uint32_t firstSize = _readLe32(data + 12);
    bool firstAddrOk = (firstAddr >= 0x40100000UL && firstAddr < 0x40110000UL) ||
                       (firstAddr >= 0x3FFE8000UL && firstAddr < 0x40000000UL);
    if (!firstAddrOk) {
        if (reason) *reason = "not_esp8266_segment";
        return false;
    }
    if (firstSize == 0 || firstSize > 65536UL) {
        if (reason) *reason = "bad_first_segment_size";
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// begin — 注册 POST /ota
// ----------------------------------------------------------------------------
bool Esp8266BaseOTA::begin() {
    if (!Esp8266BaseWeb::isRunning()) {
        ESP8266BASE_LOG_E("OTA ", "Web not running, cannot register OTA route");
        return false;
    }

    // 注册 POST /ota，两个回调都是静态成员函数（无捕获，无 std::function 驻留堆）
    Esp8266BaseWeb::server().on("/ota", HTTP_POST,
        _handleUploadComplete,  // 上传完成后的响应
        _handleUploadChunk      // 接收每个数据块
    );

    ESP8266BASE_LOG_I("OTA ", "ota_upload_route_registered method=POST path=/ota");
    return true;
}

bool Esp8266BaseOTA::isInProgress() {
    return _inProgress;
}

void Esp8266BaseOTA::_pauseWatchdog() {
#if ESP8266BASE_USE_WATCHDOG
    if (!_watchdogPaused) {
        Esp8266BaseWatchdog::pause();
        _watchdogPaused = true;
    }
#endif
}

void Esp8266BaseOTA::_resumeWatchdog() {
#if ESP8266BASE_USE_WATCHDOG
    if (_watchdogPaused) {
        Esp8266BaseWatchdog::resume();
        _watchdogPaused = false;
    }
#endif
}

void Esp8266BaseOTA::_failUpload(uint16_t status, const char* message, bool abortUpdate) {
    _rejected = true;
    _status = status;
    _failureMessage = message ? message : "Upload failed";
    _inProgress = false;
    if (abortUpdate && _updateStarted) {
        Update.end();
        _updateStarted = false;
    }
    _resumeWatchdog();
}

// ----------------------------------------------------------------------------
// 上传完成处理（HTTP 响应）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadComplete() {
    _inProgress = false;
    _resumeWatchdog();

    if (!_started && _status != 401) {
        _status = 400;
        _rejected = true;
        _failureMessage = "Invalid upload: no firmware data";
    }

    bool ok = _started && (_status == 200) && !_rejected && !Update.hasError();
    const char* msg = ok ? "OK: Firmware updated. Rebooting..." :
        (_status == 401 ? "Unauthorized" : _failureMessage);

    Esp8266BaseWeb::server().sendHeader("Connection", "close");
    if (_status == 401) {
        Esp8266BaseWeb::server().sendHeader("WWW-Authenticate", "Basic realm=\"ESP8266Base\"");
    }
    Esp8266BaseWeb::server().send(_status, "text/plain", msg);
    Esp8266BaseWeb::server().client().stop();

    if (ok) {
        char heapBuf[16];
        char uploadedBuf[16];
        char elapsedBuf[16];
        char rateBuf[20];
        uint32_t elapsed = _elapsedMs(_startedMs);
        Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
        Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
        _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
        _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
        ESP8266BASE_LOG_I("OTA ", "upload_success uploaded=%s elapsed=%s average_speed=%s free_heap=%s action=reboot",
                          uploadedBuf, elapsedBuf, rateBuf, heapBuf);
        Esp8266BaseConfig::flush();
        Esp8266BaseFileLog::flush();
        delay(500);
        ESP.restart();
    } else {
        char uploadedBuf[16];
        char elapsedBuf[16];
        char rateBuf[20];
        uint32_t elapsed = _elapsedMs(_startedMs);
        Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
        _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
        _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
        ESP8266BASE_LOG_E("OTA ", "upload_failed status=%u uploaded=%s elapsed=%s average_speed=%s error=%s",
                          (unsigned)_status, uploadedBuf, elapsedBuf, rateBuf,
                          _failureMessage);
    }
    _started = false;
}

// ----------------------------------------------------------------------------
// 数据块处理（每块调用一次）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadChunk() {
    HTTPUpload& upload = Esp8266BaseWeb::server().upload();

    if (upload.status == UPLOAD_FILE_START) {
        _rejected = false;
        _started = false;
        _status = 200;
        _startedMs = 0;
        _uploadedBytes = 0;
        _requestBytes = 0;
        _lastProgressPct = 0;
        _watchdogPaused = false;
        _updateStarted = false;
        _failureMessage = "Upload failed";
        if (!Esp8266BaseWeb::verifyAuth()) {
            _rejected = true;
            _status = 401;
            _failureMessage = "Unauthorized";
            ESP8266BASE_LOG_W("OTA ", "upload_rejected reason=unauthorized");
            return;
        }

        _started = true;
        _inProgress = true;
        _startedMs = millis();
        _requestBytes = (uint32_t)upload.contentLength;
        _pauseWatchdog();
        char heapBuf[16];
        char spaceBuf[16];
        char requestBuf[16];
        Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
        Esp8266BaseUtil::formatBytes(ESP.getFreeSketchSpace(), spaceBuf, sizeof(spaceBuf));
        Esp8266BaseUtil::formatBytes(_requestBytes, requestBuf, sizeof(requestBuf));
        ESP8266BASE_LOG_I("OTA ", "upload_started file=%s request_total=%s started_ms=%lu free_heap=%s sketch_space=%s",
                          upload.filename.c_str(), requestBuf,
                          (unsigned long)_startedMs, heapBuf, spaceBuf);

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (_rejected) return;
        uint32_t uploadedNow = (uint32_t)(upload.totalSize + upload.currentSize);
        if (_uploadedBytes == 0) {
            const char* reason = "unknown";
            if (!_isLikelyEsp8266Firmware(upload.buf, upload.currentSize, &reason)) {
                _failUpload(400, "Invalid firmware: not an ESP8266 app image", false);
                ESP8266BASE_LOG_E("OTA ", "upload_rejected reason=%s detail=not_esp8266_firmware", reason);
                return;
            }
            if (!Update.begin(ESP.getFreeSketchSpace())) {
                _failUpload(500, "Update failed: begin failed", false);
                ESP8266BASE_LOG_E("OTA ", "update_begin_failed error=%s", Update.getErrorString().c_str());
                return;
            }
            _updateStarted = true;
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            _uploadedBytes = uploadedNow;
            _failUpload(500, "Update failed: write failed", true);
            char uploadedBuf[16];
            char elapsedBuf[16];
            char rateBuf[20];
            uint32_t elapsed = _elapsedMs(_startedMs);
            Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
            _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
            _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
            ESP8266BASE_LOG_E("OTA ", "update_write_failed uploaded=%s elapsed=%s average_speed=%s error=%s",
                              uploadedBuf, elapsedBuf, rateBuf, _failureMessage);
        } else {
            _uploadedBytes = uploadedNow;
            if (_requestBytes > 0) {
                uint8_t progress = (uint8_t)(((uint64_t)_uploadedBytes * 100ULL) / (uint64_t)_requestBytes);
                if (progress > 100) progress = 100;
                if (progress >= 10 && progress / 10 > _lastProgressPct / 10) {
                    _lastProgressPct = (progress / 10) * 10;
                    char uploadedBuf[16];
                    char requestBuf[16];
                    char elapsedBuf[16];
                    char rateBuf[20];
                    uint32_t elapsed = _elapsedMs(_startedMs);
                    Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
                    Esp8266BaseUtil::formatBytes(_requestBytes, requestBuf, sizeof(requestBuf));
                    _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
                    _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
                    ESP8266BASE_LOG_I("OTA ", "upload_progress progress=%u%% bytes=%s request_total=%s speed=%s elapsed=%s",
                                      (unsigned)_lastProgressPct, uploadedBuf, requestBuf, rateBuf, elapsedBuf);
                }
            }
        }
        yield();  // 每块写入后让出 CPU，防止 Soft WDT

    } else if (upload.status == UPLOAD_FILE_END) {
        _inProgress = false;
        if (_rejected) {
            _resumeWatchdog();
            return;
        }
        if (upload.totalSize > _uploadedBytes) {
            _uploadedBytes = (uint32_t)upload.totalSize;
        }
        if (Update.end(true)) {
            _updateStarted = false;
            char uploadedBuf[16];
            char heapBuf[16];
            char elapsedBuf[16];
            char rateBuf[20];
            uint32_t elapsed = _elapsedMs(_startedMs);
            Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
            Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
            _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
            _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
            ESP8266BASE_LOG_I("OTA ", "upload_finished uploaded=%s elapsed=%s average_speed=%s free_heap=%s",
                              uploadedBuf, elapsedBuf, rateBuf, heapBuf);
        } else {
            _updateStarted = false;
            _failUpload(500, "Update failed: end failed", false);
            char uploadedBuf[16];
            char elapsedBuf[16];
            char rateBuf[20];
            uint32_t elapsed = _elapsedMs(_startedMs);
            Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
            _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
            _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
            ESP8266BASE_LOG_E("OTA ", "update_end_failed uploaded=%s elapsed=%s average_speed=%s error=%s",
                              uploadedBuf, elapsedBuf, rateBuf, Update.getErrorString().c_str());
        }
        _resumeWatchdog();

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (upload.totalSize > _uploadedBytes) {
            _uploadedBytes = (uint32_t)upload.totalSize;
        }
        _failUpload(499, "Upload aborted", !_rejected);
        char uploadedBuf[16];
        char elapsedBuf[16];
        char rateBuf[20];
        uint32_t elapsed = _elapsedMs(_startedMs);
        Esp8266BaseUtil::formatBytes(_uploadedBytes, uploadedBuf, sizeof(uploadedBuf));
        _formatSeconds(elapsed, elapsedBuf, sizeof(elapsedBuf));
        _formatRate(_uploadedBytes, elapsed, rateBuf, sizeof(rateBuf));
        ESP8266BASE_LOG_W("OTA ", "upload_aborted uploaded=%s elapsed=%s average_speed=%s",
                          uploadedBuf, elapsedBuf, rateBuf);
    }
}
#endif
