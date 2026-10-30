#pragma once

#include <stdint.h>

// ----------------------------------------------------------------------------
// Esp8266BaseRecoveryButtonCore — WiFi 恢复键消抖/长按纯核
//
// 零 Arduino 依赖，可在 host 上单测（见 tools/recovery_button_core_host_test.cpp，
// 由 tools/check_logic.py 编译执行）。
//
// 语义：
//   - 输入为低电平有效“rawPressed”（GPIO0 板载 BOOT/FLASH 键，按下接地）。
//   - raw 电平变化后须稳定 debounceMs 才采纳，消除抖动/尖峰。
//   - 稳定按下后保持 holdMs 触发一次；同一次按住（未松开）不重复触发。
//   - 松开（经消抖采纳）后重置单次触发，下次按住可再次触发。
//   - 全部时长为单调毫秒，差值用 int32_t 有符号比较，回绕安全。
//
// 上层只在正常启动（begin 末尾，离开上电窗口）后调用 update()，因此
// “按住上电进 ROM 下载模式”不会与本检测冲突。
// ----------------------------------------------------------------------------

namespace Esp8266BaseRecoveryButtonCore {

struct RecoveryButtonCore {
    bool     rawPressed         = false;  // 最近一次原始电平
    bool     stablePressed      = false;  // 消抖后采纳的电平
    bool     triggeredThisPress = false;  // 本次按住是否已触发
    uint32_t rawChangedMs       = 0;      // 原始电平最近一次跳变时刻
    uint32_t pressedMs          = 0;      // 稳定按下起始时刻

    void reset(uint32_t nowMs) {
        rawPressed = false;
        stablePressed = false;
        triggeredThisPress = false;
        rawChangedMs = nowMs;
        pressedMs = 0;
    }

    // 喂入一轮采样；返回 true 的这一轮表示“长按已满足，触发一次”。
    bool update(bool rawPressedNow, uint32_t nowMs,
                uint32_t debounceMs, uint32_t holdMs) {
        if (rawPressedNow != rawPressed) {
            rawPressed = rawPressedNow;
            rawChangedMs = nowMs;
        }
        if (rawPressedNow != stablePressed &&
            static_cast<int32_t>(nowMs - rawChangedMs) >=
                static_cast<int32_t>(debounceMs)) {
            stablePressed = rawPressedNow;
            // 新一轮按下/松开都清单次触发；按下时重新计时。
            triggeredThisPress = false;
            if (rawPressedNow) {
                pressedMs = nowMs;
            }
        }
        if (!stablePressed || triggeredThisPress ||
            static_cast<int32_t>(nowMs - pressedMs) <
                static_cast<int32_t>(holdMs)) {
            return false;
        }
        triggeredThisPress = true;
        return true;
    }
};

}  // namespace Esp8266BaseRecoveryButtonCore
