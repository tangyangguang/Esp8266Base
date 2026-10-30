#pragma once

#include <stdint.h>

// ----------------------------------------------------------------------------
// Esp8266BaseLinkRecoveryCore — 本地链路自愈节奏纯核（零 Arduino 依赖，可 host 单测）
//
// 两个决策，都只认“设备↔路由器”本地 STA 状态，与云端 MQTT/DNS/TLS 无关：
//   1) RadioResetGate：CONNECTING 连续失败时，是否有资格冷拆 WiFi 电台。
//   2) LinkRestartLadder：本地长期失联时，是否申请受控 MCU 重启。
//
// 设计取舍（见 docs/08_networking.md）：
//   - 云端故障绝不拆电台；电台冷拆是针对“SDK station 软卡死”这一本地故障的最后手段。
//   - 首次仍快速（连续失败 6 次，约 2 分钟），保住历史确证的软卡死自愈；
//     之后受 10 分钟绝对冷却 + 每失联窗口 3 次预算约束，避免错密码/弱信号下
//     每 ~2 分钟反复 WIFI_OFF（冷关联可能拖死自己），也把崩溃 PCB 操作压到低频。
//   - 关联成功会清连续失败与窗口预算，但不清绝对冷却：偶发 1s flap 不会解除
//     “≤1 次/10min”这条真正的安全带。
//
// 所有时间均为单调毫秒；跨 2^32 回绕一律用有符号差值比较。
// ----------------------------------------------------------------------------

namespace Esp8266BaseLinkRecoveryCore {

// radio reset 节奏
constexpr uint8_t  RADIO_RESET_FAILURE_THRESHOLD = 6;        // 连续失败达到该次数才“有资格”
constexpr uint32_t RADIO_RESET_MIN_INTERVAL_MS   = 600000U;  // 本 boot 首次后，两次 reset 最小间隔 10min
constexpr uint8_t  RADIO_RESET_WINDOW_BUDGET     = 3;        // 每个失联窗口最多 reset 次数

// 长期失联受控 MCU 重启阶梯
constexpr uint32_t LINK_RESTART_AFTER_MS          = 1800000U; // 连续本地不可达 30min
constexpr uint8_t  LINK_RESTART_MIN_RADIO_RESETS  = 2;        // 且窗口内 radio reset 增量至少 2 次
constexpr uint32_t LINK_RESTART_RETRY_MS          = 60000U;   // guard/cooldown/预算拒绝后的重试间隔

// 单调毫秒有符号差值：now 不早于 past 时 >=0，回绕安全。
inline int32_t elapsedSigned(uint32_t now, uint32_t past) {
    return static_cast<int32_t>(now - past);
}

//  radio reset 资格闸门。WiFi 层每次失败进入重试调度时调用一次 onFailure()：
//  返回 true 的这一轮才允许置 radio reset 请求；返回 false 必须走与普通慢重试
//  完全相同的布臂路径（不能提前 return，否则会退化成无间隔疯狂重关联）。
struct RadioResetGate {
    uint32_t lastResetMs  = 0;  // 本 boot 上次实际 reset 时刻；0=从未 reset（首次豁免冷却）
    uint8_t  windowResets = 0;  // 当前失联窗口已用 reset 次数（关联成功/新凭据清零，饱和）
    uint8_t  failures     = 0;  // 距上次成功/reset 的连续失败次数（饱和）

    bool onFailure(uint32_t now) {
        if (failures < 0xFFU) ++failures;
        if (failures < RADIO_RESET_FAILURE_THRESHOLD) return false;
        if (windowResets >= RADIO_RESET_WINDOW_BUDGET) return false;
        if (lastResetMs != 0 &&
            elapsedSigned(now, lastResetMs) <
                static_cast<int32_t>(RADIO_RESET_MIN_INTERVAL_MS)) {
            return false;  // 冷却中：阈值已满足也只做普通慢重试；下一次失败会重新判定
        }
        return true;
    }

    // radio reset 真正物理执行后调用。
    void onResetExecuted(uint32_t now) {
        lastResetMs = now;
        if (windowResets < 0xFFU) ++windowResets;
        failures = 0;
    }

    // 成功关联：清连续失败与窗口预算；绝对冷却 lastResetMs 刻意保留。
    void onLinkHealthy() {
        failures = 0;
        windowResets = 0;
    }

    // 新凭据：与成功关联同样重置窗口（lastResetMs 仍保留，作为跨配置的安全带）。
    void onNewCredentials() { onLinkHealthy(); }
};

// 长期失联受控 MCU 重启阶梯。每个主循环调用一次 onTick()，返回 true 的这一轮
// 申请 Watchdog 受控重启（guard/cooldown/预算由 Watchdog 决定，被拒后按
// LINK_RESTART_RETRY_MS 低率重试）。
struct LinkRestartLadder {
    bool     active        = false;
    uint32_t startedMs     = 0;  // 本失联窗口起点
    uint8_t  radioBaseline = 0;  // 窗口起点的累计 radio reset 次数
    uint32_t nextRetryMs   = 0;  // 0=本窗口尚未申请过（首次立即可申请）

    bool onTick(bool linked, bool apMode, uint8_t radioResetCount, uint32_t now) {
        if (linked) { active = false; return false; }  // 任意一轮关联成功即整体撤防
        if (apMode) return false;                      // AP 配网态由 AP 提供本地服务
        if (!active) {
            active = true;
            startedMs = now;
            radioBaseline = radioResetCount;
            nextRetryMs = 0;
            return false;
        }
        const uint32_t outage = now - startedMs;  // 无符号回绕安全（窗口 <49.7 天）
        const uint8_t resets = static_cast<uint8_t>(radioResetCount - radioBaseline);
        if (outage < LINK_RESTART_AFTER_MS) return false;
        if (resets < LINK_RESTART_MIN_RADIO_RESETS) return false;
        // nextRetryMs==0 表示本窗口首次申请，必须放行，不能用 (now-0) 判负。
        if (nextRetryMs != 0 && elapsedSigned(now, nextRetryMs) < 0) return false;
        nextRetryMs = now + LINK_RESTART_RETRY_MS;
        return true;
    }
};

}  // namespace Esp8266BaseLinkRecoveryCore
