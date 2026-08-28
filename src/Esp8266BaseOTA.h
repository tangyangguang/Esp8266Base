#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseOTA — Web OTA 固件更新
//
// 依赖 Esp8266BaseWeb 已通过 begin() 启动
// 注册 POST /ota 路由；完整 Web 模式可配合 GET 页面，MQTT_TERMINAL 由脚本上传
// 上传期间在 Watchdog 启用时自动 pause/resume，每块后调用 yield()
// /ota 页面和上传 POST 都使用 Esp8266BaseWeb 的 Basic Auth
//
// RAM 预算：<= 160B
// ----------------------------------------------------------------------------

enum class Esp8266BaseOTAFailure : uint8_t {
    UNAUTHORIZED = 0,
    INVALID_FIRMWARE,
    PREPARE_REJECTED,
    UPDATE_BEGIN_FAILED,
    UPDATE_WRITE_FAILED,
    UPDATE_END_FAILED,
    UPLOAD_ABORTED,
    NO_FIRMWARE_DATA,
    CONFIG_FLUSH_FAILED,
    FILELOG_FLUSH_FAILED,
    MQTT_PAUSE_FAILED
};

// prepare 可把拒绝原因写入 reason（含结尾 \0 的容量为 reasonLen）。启用 MQTT 时，
// MQTT_TERMINAL 业务须在 prepare 中完成安全停机并调用 Esp8266BaseMQTT::beginShutdown()。
typedef bool (*Esp8266BaseOTAPrepareCallback)(char* reason, size_t reasonLen);
typedef void (*Esp8266BaseOTAFailureCallback)(Esp8266BaseOTAFailure failure);
typedef void (*Esp8266BaseOTASuccessCallback)();

class Esp8266BaseOTA {
public:
    // 注册 POST /ota 路由到 Esp8266BaseWeb::server()
    // 必须在 Esp8266BaseWeb::begin() 之后调用
    static bool begin();

    // OTA 是否正在上传
    static bool isInProgress();

    // 必须在上传前注册；允许传 nullptr。回调只保存固定函数指针，不分配内存。
    static void setLifecycleCallbacks(Esp8266BaseOTAPrepareCallback prepare,
                                      Esp8266BaseOTAFailureCallback failure,
                                      Esp8266BaseOTASuccessCallback success = nullptr);

private:
    static bool _inProgress;  // 1B
    static bool _rejected;    // 1B：认证或 Update 初始化失败后拒绝后续块
    static bool _started;     // 1B：本次 POST 是否收到固件起始块
    static bool _watchdogPaused; // 1B：本次 OTA 是否已暂停 Watchdog
    static bool _updateStarted;  // 1B：Update.begin() 是否已成功
    static bool _failureNotified;// 1B：failure callback 是否已调用
    static bool _successNotified;// 1B：success callback 是否已调用
    static uint16_t _status;  // 2B：上传完成时返回的 HTTP 状态码
    static uint32_t _startedMs;       // 4B：上传开始 millis
    static uint32_t _uploadedBytes;   // 4B：已写入固件字节数
    static uint32_t _requestBytes;    // 4B：multipart request Content-Length
    static uint8_t  _lastProgressPct; // 1B：最近一次 25% 阶梯进度日志
    static char _failureMessage[64]; // 失败时返回给客户端的固定长度原因
    static Esp8266BaseOTAPrepareCallback _prepareCallback;
    static Esp8266BaseOTAFailureCallback _failureCallback;
    static Esp8266BaseOTASuccessCallback _successCallback;

    // 静态回调函数（注册给 ESP8266WebServer，无捕获，无 std::function 驻留堆）
    static void _handleUploadComplete();
    static void _handleUploadChunk();
    static void _pauseWatchdog();
    static void _resumeWatchdog();
    static void _resetRequestState();
    static void _notifyFailure(Esp8266BaseOTAFailure failure);
    static void _notifySuccess();
    static void _failUpload(uint16_t status, const char* message, bool abortUpdate,
                            Esp8266BaseOTAFailure failure);
};
