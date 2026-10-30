// Host-only unit tests for the WiFi recovery button pure core
// (debounce + long-press edge, one trigger per press, millis wrap).
// Compiled and executed by tools/check_logic.py (clang++/g++); never linked into firmware.
#include "../src/Esp8266BaseRecoveryButtonCore.h"

#include <cstdio>

using Esp8266BaseRecoveryButtonCore::RecoveryButtonCore;

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static constexpr uint32_t DEBOUNCE = 50;
static constexpr uint32_t HOLD = 10000;

// 短按（不足 hold）松开，任何时刻都不触发。
static void test_short_press() {
    RecoveryButtonCore b;
    b.reset(0);
    bool fired = false;
    // 按下并稳定消抖，保持 3s（< 10s）后松开。
    for (uint32_t t = 0; t <= 3000; t += 20) fired |= b.update(true, t, DEBOUNCE, HOLD);
    for (uint32_t t = 3000; t <= 3200; t += 20) fired |= b.update(false, t, DEBOUNCE, HOLD);
    CHECK(!fired);
    CHECK(!b.stablePressed);
}

// 按住满 hold 的那一拍恰好触发，且同一次按住只触发一次。
static void test_hold_triggers_once() {
    RecoveryButtonCore b;
    b.reset(1000);
    int fireCount = 0;
    for (uint32_t t = 1000; t <= 13000; t += 20) {
        if (b.update(true, t, DEBOUNCE, HOLD)) ++fireCount;
    }
    CHECK(fireCount == 1);
    // 首次触发时刻：1000（按下 raw）+ 50（消抖采纳）+ 10000 = 11050。
    // 重新精确验证边界。
    RecoveryButtonCore b2;
    b2.reset(0);
    CHECK(!b2.update(true, 0, DEBOUNCE, HOLD));      // raw 跳变
    CHECK(!b2.update(true, 49, DEBOUNCE, HOLD));     // 消抖未满
    CHECK(!b2.update(true, 50, DEBOUNCE, HOLD));     // 消抖采纳，pressedMs=50
    CHECK(!b2.update(true, 10049, DEBOUNCE, HOLD));  // hold-1
    CHECK(b2.update(true, 10050, DEBOUNCE, HOLD));   // 恰好 hold
    CHECK(!b2.update(true, 20000, DEBOUNCE, HOLD));  // 继续按住不重复
    CHECK(b2.triggeredThisPress);
}

// hold-1ms 不触发；消抖期内抖动（按下/松开反复短于 debounce）不采纳。
static void test_bounce_rejected() {
    RecoveryButtonCore b;
    b.reset(0);
    // 抖动：按下 30ms → 松开 30ms → 按下 30ms，均短于 50ms 消抖。
    const uint32_t seq[][2] = {
        {10, 1}, {20, 1}, {40, 0}, {50, 0}, {70, 1}, {80, 1},
    };
    for (auto [t, level] : seq) {
        CHECK(!b.update(level == 1, t, DEBOUNCE, HOLD));
    }
    CHECK(!b.stablePressed);
    // 稳定按住直到消抖采纳：raw 在 70 跳变为按下，120 稳定采纳，pressedMs=120。
    for (uint32_t t = 90; t <= 140; t += 10) CHECK(!b.update(true, t, DEBOUNCE, HOLD));
    CHECK(b.stablePressed);
    // 上一采样停在 140，继续跳过 10119，精确验证 hold-1 与恰好 hold。
    CHECK(!b.update(true, 10118, DEBOUNCE, HOLD)); // 仍不足 hold
    CHECK(!b.update(true, 10119, DEBOUNCE, HOLD)); // hold-1 不触发
    CHECK(b.update(true, 10120, DEBOUNCE, HOLD));  // pressedMs(120)+10000
}

// 触发后松开，再按一次可再次触发。
static void test_press_again_after_release() {
    RecoveryButtonCore b;
    b.reset(0);
    int fireCount = 0;
    for (uint32_t t = 0; t <= 10100; t += 25) {
        if (b.update(true, t, DEBOUNCE, HOLD)) ++fireCount;
    }
    CHECK(fireCount == 1);
    // 松开并稳定消抖。
    for (uint32_t t = 10200; t <= 10400; t += 25) b.update(false, t, DEBOUNCE, HOLD);
    CHECK(!b.stablePressed);
    // 再次长按。
    for (uint32_t t = 10500; t <= 20700; t += 25) {
        if (b.update(true, t, DEBOUNCE, HOLD)) ++fireCount;
    }
    CHECK(fireCount == 2);
}

// millis 在按住窗口内跨 2^32 回绕，仍按有符号差值正确触发。
static void test_millis_wrap() {
    RecoveryButtonCore b;
    b.reset(0xFFFFFFF0u - 100);
    uint32_t t = 0xFFFFFFF0u - 100;
    CHECK(!b.update(true, t, DEBOUNCE, HOLD));        // raw 按下
    t += DEBOUNCE;                                     // 消抖采纳
    CHECK(!b.update(true, t, DEBOUNCE, HOLD));
    const uint32_t pressedMs = t;
    CHECK(!b.update(true, pressedMs + HOLD - 1, DEBOUNCE, HOLD));
    // 再 +2ms 跨过 0。
    CHECK(b.update(true, pressedMs + HOLD + 2, DEBOUNCE, HOLD));
    // 回绕后时刻值很小，有符号差值仍为正。
    CHECK(b.update(true, 5000, DEBOUNCE, HOLD) == false);
}

// 初始未按定时，噪声采样不触发。
static void test_idle_noise() {
    RecoveryButtonCore b;
    b.reset(0);
    for (uint32_t t = 0; t < 60000; t += 37) {
        CHECK(!b.update(false, t, DEBOUNCE, HOLD));
    }
}

int main() {
    test_short_press();
    test_hold_triggers_once();
    test_bounce_rejected();
    test_press_again_after_release();
    test_millis_wrap();
    test_idle_noise();
    if (g_failures) {
        std::printf("recovery button core: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("recovery button core: all tests passed\n");
    return 0;
}
