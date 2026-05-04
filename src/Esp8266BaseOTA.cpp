#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_OTA
#include "Esp8266BaseOTA.h"
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseWatchdog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include "Esp8266BaseUtil.h"
#include <Updater.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool Esp8266BaseOTA::_inProgress = false;
bool Esp8266BaseOTA::_rejected = false;
bool Esp8266BaseOTA::_started = false;
uint16_t Esp8266BaseOTA::_status = 200;

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

// ----------------------------------------------------------------------------
// 上传完成处理（HTTP 响应）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadComplete() {
    _inProgress = false;
    Esp8266BaseWatchdog::resume();

    if (!_started && _status != 401) {
        _status = 400;
        _rejected = true;
    }

    bool ok = _started && (_status == 200) && !_rejected && !Update.hasError();
    const char* msg = ok ? "OK: Firmware updated. Rebooting..." :
        (_status == 401 ? "Unauthorized" : "FAIL");

    Esp8266BaseWeb::server().sendHeader("Connection", "close");
    if (_status == 401) {
        Esp8266BaseWeb::server().sendHeader("WWW-Authenticate", "Basic realm=\"ESP8266Base\"");
    }
    Esp8266BaseWeb::server().send(_status, "text/plain", msg);
    Esp8266BaseWeb::server().client().stop();

    if (ok) {
        char heapBuf[16];
        Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
        ESP8266BASE_LOG_I("OTA ", "upload_success free_heap=%s action=reboot", heapBuf);
        Esp8266BaseConfig::flush();
        delay(500);
        ESP.restart();
    } else {
        ESP8266BASE_LOG_E("OTA ", "upload_failed status=%u error=%s",
                          (unsigned)_status, Update.getErrorString().c_str());
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
        if (!Esp8266BaseWeb::verifyAuth()) {
            _rejected = true;
            _status = 401;
            ESP8266BASE_LOG_W("OTA ", "upload_rejected reason=unauthorized");
            return;
        }

        _started = true;
        _inProgress = true;
        Esp8266BaseWatchdog::pause();
        char heapBuf[16];
        char spaceBuf[16];
        Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
        Esp8266BaseUtil::formatBytes(ESP.getFreeSketchSpace(), spaceBuf, sizeof(spaceBuf));
        ESP8266BASE_LOG_I("OTA ", "upload_started file=%s free_heap=%s sketch_space=%s",
                          upload.filename.c_str(), heapBuf, spaceBuf);
        if (!Update.begin(ESP.getFreeSketchSpace())) {
            _rejected = true;
            _status = 500;
            ESP8266BASE_LOG_E("OTA ", "update_begin_failed error=%s", Update.getErrorString().c_str());
        }

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (_rejected) return;
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            _rejected = true;
            _status = 500;
            char totalBuf[16];
            Esp8266BaseUtil::formatBytes(upload.totalSize, totalBuf, sizeof(totalBuf));
            ESP8266BASE_LOG_E("OTA ", "update_write_failed written=%s", totalBuf);
        }
        yield();  // 每块写入后让出 CPU，防止 Soft WDT

    } else if (upload.status == UPLOAD_FILE_END) {
        if (_rejected) return;
        if (Update.end(true)) {
            char totalBuf[16];
            char heapBuf[16];
            Esp8266BaseUtil::formatBytes(upload.totalSize, totalBuf, sizeof(totalBuf));
            Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
            ESP8266BASE_LOG_I("OTA ", "upload_finished total_size=%s free_heap=%s", totalBuf, heapBuf);
        } else {
            _rejected = true;
            _status = 500;
            ESP8266BASE_LOG_E("OTA ", "update_end_failed error=%s", Update.getErrorString().c_str());
        }

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (!_rejected) Update.end();
        _rejected = true;
        _status = 499;
        _inProgress = false;
        Esp8266BaseWatchdog::resume();
        ESP8266BASE_LOG_W("OTA ", "upload_aborted");
    }
}
#endif
