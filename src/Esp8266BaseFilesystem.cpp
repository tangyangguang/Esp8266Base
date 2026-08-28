#include "Esp8266BaseOptions.h"
#if ESP8266BASE_USE_FILESYSTEM

#include "Esp8266BaseFilesystem.h"
#include "Esp8266BaseLog.h"
#include <LittleFS.h>

bool Esp8266BaseFilesystem::_ready = false;

bool Esp8266BaseFilesystem::begin() {
    if (_ready) return true;
    if (LittleFS.begin()) {
        _ready = true;
        ESP8266BASE_LOG_I("FS  ", "littlefs_mounted");
        return true;
    }
    delay(50);
    if (LittleFS.begin()) {
        _ready = true;
        ESP8266BASE_LOG_I("FS  ", "littlefs_mounted_after_retry");
        return true;
    }
#if ESP8266BASE_CFG_FORMAT_ON_FAIL
    ESP8266BASE_LOG_W("FS  ", "littlefs_mount_failed action=format");
    if (LittleFS.format() && LittleFS.begin()) {
        _ready = true;
        ESP8266BASE_LOG_I("FS  ", "littlefs_formatted_and_mounted");
        return true;
    }
#endif
    ESP8266BASE_LOG_E("FS  ", "littlefs_mount_failed action=continue_without_storage");
    return false;
}

bool Esp8266BaseFilesystem::isReady() { return _ready; }

#endif
