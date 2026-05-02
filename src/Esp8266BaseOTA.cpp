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

    bool ok = !Update.hasError();
    const char* msg = ok ? "OK: Firmware updated. Rebooting..." : "FAIL";

    Esp8266BaseWeb::server().sendHeader("Connection", "close");
    Esp8266BaseWeb::server().send(200, "text/plain", msg);
    Esp8266BaseWeb::server().client().stop();

    if (ok) {
        char heapBuf[16];
        Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
        ESP8266BASE_LOG_I("OTA ", "upload_success free_heap=%s action=reboot", heapBuf);
        Esp8266BaseConfig::flush();
        delay(500);
        ESP.restart();
    } else {
        ESP8266BASE_LOG_E("OTA ", "upload_failed error=%s", Update.getErrorString().c_str());
    }
}

// ----------------------------------------------------------------------------
// 数据块处理（每块调用一次）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadChunk() {
    HTTPUpload& upload = Esp8266BaseWeb::server().upload();

    if (upload.status == UPLOAD_FILE_START) {
        _inProgress = true;
        Esp8266BaseWatchdog::pause();
        char heapBuf[16];
        char spaceBuf[16];
        Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
        Esp8266BaseUtil::formatBytes(ESP.getFreeSketchSpace(), spaceBuf, sizeof(spaceBuf));
        ESP8266BASE_LOG_I("OTA ", "upload_started file=%s free_heap=%s sketch_space=%s",
                          upload.filename.c_str(), heapBuf, spaceBuf);
        if (!Update.begin(ESP.getFreeSketchSpace())) {
            ESP8266BASE_LOG_E("OTA ", "update_begin_failed error=%s", Update.getErrorString().c_str());
        }

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            char totalBuf[16];
            Esp8266BaseUtil::formatBytes(upload.totalSize, totalBuf, sizeof(totalBuf));
            ESP8266BASE_LOG_E("OTA ", "update_write_failed written=%s", totalBuf);
        }
        yield();  // 每块写入后让出 CPU，防止 Soft WDT

    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            char totalBuf[16];
            char heapBuf[16];
            Esp8266BaseUtil::formatBytes(upload.totalSize, totalBuf, sizeof(totalBuf));
            Esp8266BaseUtil::formatBytes(ESP.getFreeHeap(), heapBuf, sizeof(heapBuf));
            ESP8266BASE_LOG_I("OTA ", "upload_finished total_size=%s free_heap=%s", totalBuf, heapBuf);
        } else {
            ESP8266BASE_LOG_E("OTA ", "update_end_failed error=%s", Update.getErrorString().c_str());
        }

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        _inProgress = false;
        Esp8266BaseWatchdog::resume();
        ESP8266BASE_LOG_W("OTA ", "upload_aborted");
    }
}
