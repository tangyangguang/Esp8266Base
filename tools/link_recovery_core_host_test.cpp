// Host-only unit tests for the local-link recovery pure core
// (radio-reset gate cadence + long-outage MCU restart ladder).
// Compiled and executed by tools/check_logic.py (clang++/g++); never linked into firmware.
#include "../src/Esp8266BaseLinkRecoveryCore.h"

#include <cstdio>

using namespace Esp8266BaseLinkRecoveryCore;

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// ---------------------------------------------------------------------------
// RadioResetGate
// ---------------------------------------------------------------------------

// 首次：前 5 次失败不触发，第 6 次快速触发（本 boot 从未 reset，豁免冷却）。
static void test_gate_first_fast() {
    RadioResetGate g;
    for (int i = 0; i < RADIO_RESET_FAILURE_THRESHOLD - 1; ++i)
        CHECK(g.onFailure(1000u + uint32_t(i) * 1000u) == false);
    CHECK(g.failures == RADIO_RESET_FAILURE_THRESHOLD - 1);
    CHECK(g.onFailure(1000u + uint32_t(RADIO_RESET_FAILURE_THRESHOLD) * 1000u) == true);
}

// reset 执行后进入冷却：再攒满失败数，10min 内只慢重试；冷却一到，当次失败即触发，
// 不必再重新攒一组 6 次。
static void test_gate_cooldown() {
    RadioResetGate g;
    uint32_t t = 110000;
    for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i) g.onFailure(t += 14000);
    CHECK(g.onFailure(t) == true);  // 已第 6 次，取 true
    g.onResetExecuted(t);
    const uint32_t resetAt = t;
    CHECK(g.windowResets == 1);
    CHECK(g.failures == 0);

    // 冷却中：再失败 6 次（阈值满足但间隔不足）全部 false。
    for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i)
        CHECK(g.onFailure(t += 14000) == false);
    CHECK(g.failures >= RADIO_RESET_FAILURE_THRESHOLD);

    // 以 reset 时刻为基准：还差 1ms 到 10min 仍 false，冷却到期当次失败即 true。
    CHECK(g.onFailure(resetAt + RADIO_RESET_MIN_INTERVAL_MS - 1) == false);
    CHECK(g.onFailure(resetAt + RADIO_RESET_MIN_INTERVAL_MS) == true);
}

// 单失联窗口预算 3 次：第 3 次执行后，即使冷却已过也不再触发。
static void test_gate_window_budget() {
    RadioResetGate g;
    uint32_t t = 0;
    for (uint8_t r = 0; r < RADIO_RESET_WINDOW_BUDGET; ++r) {
        for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i) g.onFailure(t += 14000);
        CHECK(g.onFailure(t) == true);
        g.onResetExecuted(t);
        t += RADIO_RESET_MIN_INTERVAL_MS + 1;  // 远过冷却
    }
    CHECK(g.windowResets == RADIO_RESET_WINDOW_BUDGET);
    for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i) g.onFailure(t += 14000);
    CHECK(g.onFailure(t) == false);  // 预算耗尽
}

// 成功关联清窗口预算与连续失败，但保留绝对冷却（偶发 flap 不解除 ≤1次/10min 安全带）。
static void test_gate_healthy_keeps_absolute_cooldown() {
    RadioResetGate g;
    uint32_t t = 110000;
    for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i) g.onFailure(t += 14000);
    g.onFailure(t);
    g.onResetExecuted(t);

    g.onLinkHealthy();  // 期间曾短暂关联成功
    CHECK(g.windowResets == 0);
    CHECK(g.failures == 0);
    CHECK(g.lastResetMs != 0);

    for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i) g.onFailure(t += 14000);
    CHECK(g.onFailure(t) == false);  // 窗口预算虽已刷新，但仍在 10min 绝对冷却内
}

// 失败计数饱和在 255，不回绕；冷却到期仍可触发。
static void test_gate_saturation() {
    RadioResetGate g;
    for (int i = 0; i < 300; ++i) g.onFailure(uint32_t(i) * 1000u);
    CHECK(g.failures == 0xFF);
    // 从未 reset（lastResetMs==0），阈值满足 → 可触发。
    CHECK(g.onFailure(300000) == true);
}

// millis 越过 2^32 回绕：冷却比较仍正确（间隔约 600s）。
static void test_gate_wrap() {
    RadioResetGate g;
    const uint32_t last = 0xFFFFFFFFu - 100000u;  // 回绕点前 ~100s
    g.onResetExecuted(last);
    g.windowResets = 0;  // 模拟窗口预算未满
    for (uint8_t i = 0; i < RADIO_RESET_FAILURE_THRESHOLD; ++i) g.failures++;
    const uint32_t now = 500000u;  // 回绕后，与 last 相隔约 600001ms
    CHECK(int32_t(now - last) >= int32_t(RADIO_RESET_MIN_INTERVAL_MS));
    CHECK(g.onFailure(now) == true);
}

// ---------------------------------------------------------------------------
// LinkRestartLadder
// ---------------------------------------------------------------------------

// 已关联 / AP 配网态永不 arm。
static void test_ladder_not_armed_when_linked_or_ap() {
    LinkRestartLadder l;
    CHECK(l.onTick(true, false, 0, 1000) == false);
    CHECK(l.active == false);
    CHECK(l.onTick(false, true, 0, 2000) == false);
    CHECK(l.active == false);
}

// arm 后：未满 30min 不重启；满 30min 但 radio reset 增量不足 2 次也不重启。
static void test_ladder_thresholds() {
    LinkRestartLadder l;
    CHECK(l.onTick(false, false, 0, 0) == false);  // arm
    CHECK(l.active);
    CHECK(l.onTick(false, false, 0, LINK_RESTART_AFTER_MS - 1) == false);
    CHECK(l.onTick(false, false, 1, LINK_RESTART_AFTER_MS) == false);  // 只 1 次 reset
    CHECK(l.onTick(false, false, 2, LINK_RESTART_AFTER_MS + 1) == true);
}

// 首次申请放行后，60s 重试窗内抑制；60s 后可再次申请。
static void test_ladder_retry_suppression() {
    LinkRestartLadder l;
    l.onTick(false, false, 0, 0);
    CHECK(l.onTick(false, false, 2, LINK_RESTART_AFTER_MS) == true);
    CHECK(l.onTick(false, false, 2, LINK_RESTART_AFTER_MS + 1000) == false);
    CHECK(l.onTick(false, false, 2, LINK_RESTART_AFTER_MS + LINK_RESTART_RETRY_MS) == true);
}

// 任意一轮关联成功即整体撤防；重新失联时以当时 radioResetCount 为新基线。
static void test_ladder_disarm_on_link_and_rebaseline() {
    LinkRestartLadder l;
    l.onTick(false, false, 0, 0);
    l.onTick(false, false, 2, LINK_RESTART_AFTER_MS);  // 已满足
    l.onTick(true, false, 5, LINK_RESTART_AFTER_MS + 1);
    CHECK(l.active == false);
    // 再次失联，基线从 5 起算。
    const uint32_t t2 = 5000000u;
    CHECK(l.onTick(false, false, 5, t2) == false);  // arm at baseline 5
    CHECK(l.onTick(false, false, 6, t2 + LINK_RESTART_AFTER_MS - 1) == false);
    CHECK(l.onTick(false, false, 6, t2 + LINK_RESTART_AFTER_MS) == false);  // 增量 1 <2
    CHECK(l.onTick(false, false, 7, t2 + LINK_RESTART_AFTER_MS + 1) == true);
}

// millis 跨 2^32：失联窗口约 1900s 判定正确（无符号差值）。
static void test_ladder_wrap() {
    LinkRestartLadder l;
    const uint32_t start = 0xFFFFFFFFu - 1700000u;
    l.onTick(false, false, 0, start);  // arm
    const uint32_t now = 200000u;      // 回绕后，elapsed≈1900001ms
    CHECK((uint32_t)(now - start) >= LINK_RESTART_AFTER_MS);
    CHECK(l.onTick(false, false, 2, now) == true);
}

int main() {
    test_gate_first_fast();
    test_gate_cooldown();
    test_gate_window_budget();
    test_gate_healthy_keeps_absolute_cooldown();
    test_gate_saturation();
    test_gate_wrap();
    test_ladder_not_armed_when_linked_or_ap();
    test_ladder_thresholds();
    test_ladder_retry_suppression();
    test_ladder_disarm_on_link_and_rebaseline();
    test_ladder_wrap();
    if (g_failures) {
        std::printf("LINK RECOVERY CORE: %d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    std::printf("link recovery core: all checks passed\n");
    return 0;
}
