#include "Esp8266BaseOTA.h"
#include "Esp8266BaseWeb.h"
#include "Esp8266BaseWatchdog.h"
#include "Esp8266BaseConfig.h"
#include "Esp8266BaseLog.h"
#include <Updater.h>

// ----------------------------------------------------------------------------
// 静态成员定义
// ----------------------------------------------------------------------------
bool Esp8266BaseOTA::_inProgress = false;
bool Esp8266BaseOTA::_authOk     = false;

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

    ESP8266BASE_LOG_I("OTA ", "ready=1 route=POST/ota");
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

    if (!_authOk) {
        Esp8266BaseWeb::server().send(401, "text/plain", "Unauthorized");
        ESP8266BASE_LOG_W("OTA ", "Upload rejected: auth failed");
        return;
    }

    bool ok = !Update.hasError();
    const char* msg = ok ? "OK: Firmware updated. Rebooting..." : "FAIL";

    Esp8266BaseWeb::server().sendHeader("Connection", "close");
    Esp8266BaseWeb::server().send(200, "text/plain", msg);
    Esp8266BaseWeb::server().client().stop();

    if (ok) {
        ESP8266BASE_LOG_I("OTA ", "Upload OK heap=%u, rebooting", (unsigned)ESP.getFreeHeap());
        Esp8266BaseConfig::flush();
        delay(500);
        ESP.restart();
    } else {
        ESP8266BASE_LOG_E("OTA ", "Upload failed: %s", Update.getErrorString().c_str());
    }
}

// ----------------------------------------------------------------------------
// 数据块处理（每块调用一次）
// ----------------------------------------------------------------------------
void Esp8266BaseOTA::_handleUploadChunk() {
    HTTPUpload& upload = Esp8266BaseWeb::server().upload();

    if (upload.status == UPLOAD_FILE_START) {
        // 在接受任何数据前验证认证（verifyAuth 不发 401，由 complete 统一响应）
        _authOk     = Esp8266BaseWeb::verifyAuth();
        _inProgress = true;
        Esp8266BaseWatchdog::pause();
        if (!_authOk) {
            ESP8266BASE_LOG_W("OTA ", "Auth failed, upload will be rejected");
            return;
        }
        ESP8266BASE_LOG_I("OTA ", "Start file=%s heap=%u",
                          upload.filename.c_str(), (unsigned)ESP.getFreeHeap());
        if (!Update.begin(ESP.getFreeSketchSpace())) {
            ESP8266BASE_LOG_E("OTA ", "begin() fail: %s", Update.getErrorString().c_str());
        }

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!_authOk) return;  // 未认证，丢弃所有数据块
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            ESP8266BASE_LOG_E("OTA ", "write() fail at %u bytes", upload.totalSize);
        }
        yield();  // 每块写入后让出 CPU，防止 Soft WDT

    } else if (upload.status == UPLOAD_FILE_END) {
        if (!_authOk) return;
        if (Update.end(true)) {
            ESP8266BASE_LOG_I("OTA ", "End total=%u bytes heap_min=%u",
                              upload.totalSize, (unsigned)ESP.getFreeHeap());
        } else {
            ESP8266BASE_LOG_E("OTA ", "end() fail: %s", Update.getErrorString().c_str());
        }

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        _inProgress = false;
        _authOk     = false;
        Esp8266BaseWatchdog::resume();
        ESP8266BASE_LOG_W("OTA ", "Upload aborted");
    }
}
