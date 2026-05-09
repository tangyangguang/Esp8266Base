#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseUtil — 小型通用格式化工具
//
// 全部 inline，无全局静态 RAM。
// ----------------------------------------------------------------------------

class Esp8266BaseUtil {
public:
    static void formatBytes(uint32_t bytes, char* out, size_t len) {
        if (!out || len == 0) return;
        if (bytes < 1024UL) {
            snprintf(out, len, "%u B", (unsigned)bytes);
        } else if (bytes < 1048576UL) {
            uint32_t v10 = (bytes * 10UL + 512UL) / 1024UL;
            snprintf(out, len, "%u.%u KB", (unsigned)(v10 / 10UL), (unsigned)(v10 % 10UL));
        } else {
            uint32_t v10 = (uint32_t)(((uint64_t)bytes * 10ULL + 524288ULL) / 1048576ULL);
            snprintf(out, len, "%u.%u MB", (unsigned)(v10 / 10UL), (unsigned)(v10 % 10UL));
        }
    }
};
