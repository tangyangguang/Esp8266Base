#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_OTA
#include "Esp8266BaseOTA.h"
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseWatchdog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseFileLog.h"
#include "Esp8266BaseUtil.h"
#if ESP8266BASE_USE_MQTT
#include "Esp8266BaseMQTT.h"
#endif
#include <Updater.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool Esp8266BaseOTA::_inProgress = false;
bool Esp8266BaseOTA::_rejected = false;
bool Esp8266BaseOTA::_started = false;
bool Esp8266BaseOTA::_watchdogPaused = false;
bool Esp8266BaseOTA::_updateStarted = false;
bool Esp8266BaseOTA::_failureNotified = false;
bool Esp8266BaseOTA::_successNotified = false;
uint16_t Esp8266BaseOTA::_status = 200;
uint32_t Esp8266BaseOTA::_startedMs = 0;
uint32_t Esp8266BaseOTA::_uploadedBytes = 0;
uint32_t Esp8266BaseOTA::_requestBytes = 0;
uint8_t  Esp8266BaseOTA::_lastProgressPct = 0;
char Esp8266BaseOTA::_failureMessage[64] = "Upload failed";
Esp8266BaseOTAPrepareCallback Esp8266BaseOTA::_prepareCallback = nullptr;
Esp8266BaseOTAFailureCallback Esp8266BaseOTA::_failureCallback = nullptr;
Esp8266BaseOTASuccessCallback Esp8266BaseOTA::_successCallback = nullptr;

static const uint8_t OTA_PROGRESS_STEP = 25;

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

void Esp8266BaseOTA::setLifecycleCallbacks(Esp8266BaseOTAPrepareCallback prepare,
                                           Esp8266BaseOTAFailureCallback failure,
                                           Esp8266BaseOTASuccessCallback success) {
    _prepareCallback = prepare;
    _failureCallback = failure;
    _successCallback = success;
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

void Esp8266BaseOTA::_resetRequestState() {
    if (_updateStarted) {
        Update.end();
    }
    _resumeWatchdog();
#if ESP8266BASE_USE_MQTT
    Esp8266BaseMQTT::resumeAfterOTAFailure();
#endif
    _inProgress = false;
    _rejected = false;
    _started = false;
    _watchdogPaused = false;
    _updateStarted = false;
    _failureNotified = false;
    _successNotified = false;
    _status = 200;
    _startedMs = 0;
    _uploadedBytes = 0;
    _requestBytes = 0;
    _lastProgressPct = 0;
    strncpy(_failureMessage, "Upload failed", sizeof(_failureMessage) - 1);
    _failureMessage[sizeof(_failureMessage) - 1] = '\0';
}

void Esp8266BaseOTA::_notifyFailure(Esp8266BaseOTAFailure failure) {
    if (_failureNotified) return;
    _failureNotified = true;
    if (_failureCallback) {
        _failureCallback(failure);
    }
}

void Esp8266BaseOTA::_notifySuccess() {
    if (_successNotified) return;
    _successNotified = true;
#if ESP8266BASE_USE_MQTT
    Esp8266BaseMQTT::keepPausedAfterOTASuccess();
#endif
    if (_successCallback) {
        _successCallback();
    }
}

void Esp8266BaseOTA::_failUpload(uint16_t status, const char* message, bool abortUpdate,
                                 Esp8266BaseOTAFailure failure) {
    _rejected = true;
    _status = status;
    if (message && message != _failureMessage) {
        strncpy(_failureMessage, message, sizeof(_failureMessage) - 1);
        _failureMessage[sizeof(_failureMessage) - 1] = '\0';
    } else if (!message) {
        strncpy(_failureMessage, "Upload failed", sizeof(_failureMessage) - 1);
        _failureMessage[sizeof(_failureMessage) - 1] = '\0';
    }
    _inProgress = false;
    if (abortUpdate && _updateStarted) {
        Update.end();
        _updateStarted = false;
    }
    _resumeWatchdog();
#if ESP8266BASE_USE_MQTT
    Esp8266BaseMQTT::resumeAfterOTAFailure();
#endif
    _notifyFailure(failure);
}

// ----------------------------------------------------------------------------
// 上传完成处理（HTTP 响应）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadComplete() {
    _inProgress = false;
    _resumeWatchdog();

    if (_status == 200 && (!_started || _uploadedBytes == 0)) {
        _failUpload(400, "Invalid upload: no firmware data", false,
                    Esp8266BaseOTAFailure::NO_FIRMWARE_DATA);
    } else if (_status == 200 && Update.hasError()) {
        _failUpload(500, "Update failed: updater error", false,
                    Esp8266BaseOTAFailure::UPDATE_END_FAILED);
    }

    bool ok = _started && (_status == 200) && !_rejected && !Update.hasError();

    if (ok) {
        _notifySuccess();
    }

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
        _resetRequestState();
    }
}

// ----------------------------------------------------------------------------
// 数据块处理（每块调用一次）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadChunk() {
    HTTPUpload& upload = Esp8266BaseWeb::server().upload();

    if (upload.status == UPLOAD_FILE_START) {
        _resetRequestState();
        if (!Esp8266BaseWeb::verifyAuth()) {
            _failUpload(401, "Unauthorized", false, Esp8266BaseOTAFailure::UNAUTHORIZED);
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
                _failUpload(400, "Invalid firmware: not an ESP8266 app image", false,
                            Esp8266BaseOTAFailure::INVALID_FIRMWARE);
                ESP8266BASE_LOG_E("OTA ", "upload_rejected reason=%s detail=not_esp8266_firmware", reason);
                return;
            }
            _failureMessage[0] = '\0';
            if (_prepareCallback && !_prepareCallback(_failureMessage, sizeof(_failureMessage))) {
                _failureMessage[sizeof(_failureMessage) - 1] = '\0';
                if (_failureMessage[0] == '\0') {
                    strncpy(_failureMessage, "OTA rejected by application", sizeof(_failureMessage) - 1);
                    _failureMessage[sizeof(_failureMessage) - 1] = '\0';
                }
                _failUpload(409, _failureMessage, false, Esp8266BaseOTAFailure::PREPARE_REJECTED);
                ESP8266BASE_LOG_W("OTA ", "upload_rejected reason=application_prepare detail=%s",
                                  _failureMessage);
                return;
            }
#if ESP8266BASE_USE_MQTT
            if (!Esp8266BaseMQTT::pauseForOTA()) {
                _failUpload(503, "MQTT/TLS did not stop for OTA", false,
                            Esp8266BaseOTAFailure::MQTT_PAUSE_FAILED);
                ESP8266BASE_LOG_E("OTA ", "upload_rejected reason=mqtt_pause_failed");
                return;
            }
#endif
            if (!Update.begin(ESP.getFreeSketchSpace())) {
                _failUpload(500, "Update failed: begin failed", false,
                            Esp8266BaseOTAFailure::UPDATE_BEGIN_FAILED);
                ESP8266BASE_LOG_E("OTA ", "update_begin_failed error=%s", Update.getErrorString().c_str());
                return;
            }
            _updateStarted = true;
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            _uploadedBytes = uploadedNow;
            _failUpload(500, "Update failed: write failed", true,
                        Esp8266BaseOTAFailure::UPDATE_WRITE_FAILED);
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
                if (progress >= OTA_PROGRESS_STEP &&
                    progress / OTA_PROGRESS_STEP > _lastProgressPct / OTA_PROGRESS_STEP) {
                    _lastProgressPct = (progress / OTA_PROGRESS_STEP) * OTA_PROGRESS_STEP;
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
        if (!_updateStarted || _uploadedBytes == 0) {
            _failUpload(400, "Invalid upload: no firmware data", false,
                        Esp8266BaseOTAFailure::NO_FIRMWARE_DATA);
            ESP8266BASE_LOG_E("OTA ", "upload_rejected reason=no_firmware_data");
            _resumeWatchdog();
            return;
        }
        if (!Esp8266BaseConfig::flush()) {
            ESP8266BASE_LOG_E("OTA ", "pre_reboot_config_flush_failed pending=%u action=abort_update",
                              (unsigned)Esp8266BaseConfig::pendingCount());
            _failUpload(500, "Config flush failed; update aborted", true,
                        Esp8266BaseOTAFailure::CONFIG_FLUSH_FAILED);
            return;
        }
        if (!Esp8266BaseFileLog::flush()) {
            ESP8266BASE_LOG_E("OTA ", "pre_reboot_filelog_flush_failed action=abort_update");
            _failUpload(500, "Log flush failed; update aborted", true,
                        Esp8266BaseOTAFailure::FILELOG_FLUSH_FAILED);
            return;
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
            _failUpload(500, "Update failed: end failed", false,
                        Esp8266BaseOTAFailure::UPDATE_END_FAILED);
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
        if (_rejected) {
            _inProgress = false;
            _resumeWatchdog();
            return;  // 保留首个失败的 HTTP 状态和 failure callback 语义
        }
        _failUpload(499, "Upload aborted", !_rejected,
                    Esp8266BaseOTAFailure::UPLOAD_ABORTED);
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
