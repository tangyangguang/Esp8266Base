#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseWiFi — STA 自动连接 + AP 配网
//
// 状态机：
//   IDLE → CONNECTING → CONNECTED
//               ↓ 连接超时
//           CONNECTING（退避后一直重连）
//
// 只有无保存凭证，或用户明确清除凭证后重启，才进入 AP_CONFIG。
//
// RAM 预算：<= 256B（全局静态，不含 WiFi SDK 内部）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_WIFI_CONNECT_TIMEOUT
#define ESP8266BASE_WIFI_CONNECT_TIMEOUT 20000   // ms：单次 STA 连接观察窗口
#endif

#ifndef ESP8266BASE_WIFI_STA_SETTLE_MS
#define ESP8266BASE_WIFI_STA_SETTLE_MS 150       // ms：切换 STA/断开旧状态后的稳定等待
#endif

#ifndef ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS
#define ESP8266BASE_WIFI_STUCK_DISCONNECTED_MS 7000 // ms：WL_DISCONNECTED 无进展时提前重启本轮连接
#endif

#ifndef ESP8266BASE_WIFI_RETRY_FAST
#define ESP8266BASE_WIFI_RETRY_FAST 2000         // ms：快速重试间隔
#endif

#ifndef ESP8266BASE_WIFI_RETRY_FAST_COUNT
#define ESP8266BASE_WIFI_RETRY_FAST_COUNT 3      // 前几次使用快速重试
#endif

#ifndef ESP8266BASE_WIFI_RETRY_SLOW
// 弱信号下旧值 60s 的慢重试空窗会让设备在掉关联后长时间无法恢复（局域网也不可达）。
// 10s 仍保留退避、避免重连风暴，但把最坏恢复时间从分钟级压到十秒级。
#define ESP8266BASE_WIFI_RETRY_SLOW 10000        // ms：后续慢速重试间隔
#endif

#ifndef ESP8266BASE_WIFI_RADIO_RESET_FAILURE_COUNT
#define ESP8266BASE_WIFI_RADIO_RESET_FAILURE_COUNT 6 // 连续失败达到该次数后完整重置 WiFi radio
#endif

// WiFi 睡眠策略：默认 modem-sleep MIN 级（显式设置，见 begin()）。
// 需要完全关闭省电（弱信号排障/低延迟场景）时定义 ESP8266BASE_WIFI_NO_SLEEP=1。
#ifndef ESP8266BASE_WIFI_NO_SLEEP
#define ESP8266BASE_WIFI_NO_SLEEP 0
#endif

static_assert(ESP8266BASE_WIFI_NO_SLEEP == 0 || ESP8266BASE_WIFI_NO_SLEEP == 1,
              "ESP8266BASE_WIFI_NO_SLEEP must be 0 or 1");

#ifndef ESP8266BASE_WIFI_RADIO_RESET_SETTLE_MS
#define ESP8266BASE_WIFI_RADIO_RESET_SETTLE_MS 100 // ms：WIFI_OFF 后的稳定等待
#endif

// 本地链路长期失联受控 MCU 重启阶梯（只认 STA 本地状态，与云端无关）：
// 连续不可用达到该时长、且窗口内本地 radio reset 增量达标，才申请重启。
#ifndef ESP8266BASE_LOCAL_LINK_MCU_RECOVERY_AFTER_MS
#define ESP8266BASE_LOCAL_LINK_MCU_RECOVERY_AFTER_MS 1800000UL // 30min
#endif
#ifndef ESP8266BASE_LOCAL_LINK_MCU_RECOVERY_RADIO_RESETS
#define ESP8266BASE_LOCAL_LINK_MCU_RECOVERY_RADIO_RESETS 2
#endif

static_assert(ESP8266BASE_WIFI_RADIO_RESET_FAILURE_COUNT <= 255,
              "ESP8266BASE_WIFI_RADIO_RESET_FAILURE_COUNT must fit uint8_t");

// radio reset 请求来源（同时用于 JNL_RADIO_RESET 的 flags）
enum class Esp8266BaseRadioResetSource : uint8_t {
    STATE_MACHINE = 1,  // WiFi CONNECTING 连续失败 N 次，本地链路状态机自愈
};

// WiFi 状态枚举
enum class Esp8266BaseWiFiState : uint8_t {
    IDLE       = 0,
    CONNECTING = 1,
    CONNECTED  = 2,
    AP_CONFIG  = 3
};

class Esp8266BaseWiFi {
public:
    // 从 Config 读取凭证，启动状态机（非阻塞）
    static bool begin();

    // 状态机推进，必须在 loop() 中通过 Esp8266Base::handle() 调用
    static void handle();

    // 保存新凭证并立即重新连接 STA（Web /wifi POST 时调用）
    static bool connect(const char* ssid, const char* pass);

    // 清除保存的 WiFi 凭证，下次重启进入 AP 配网
    static bool clearCredentials();

    // 当前是否 STA 已连接
    static bool isConnected();

    // 当前 IP 地址字符串（未连接时返回 ""）
    static const char* ip();

    // 当前 STA SSID（来自内存缓存，未配置时返回 ""）
    static const char* ssid();

    // 当前 RSSI；仅 STA 已连接时有效
    static int rssi();

    // 当前 STA MAC 地址，格式化到调用方缓冲（大写、带冒号，用于展示）
    static void macAddressTo(char* out, size_t len);
    // 当前 STA MAC，12 位小写十六进制（无分隔符），用于拼设备唯一 ID
    static void macAddressHex(char* out, size_t len);

    // 当前状态机状态
    static Esp8266BaseWiFiState state();

    // 本次启动累计发起 STA 连接次数 / 完整 WiFi radio 重置次数
    static uint16_t attemptCount();
    static uint8_t radioResetCount();
    static Esp8266BaseRadioResetSource lastRadioResetSource();

    // radio reset 分层：任何模块都不得自行 WIFI_OFF。CONNECTING 连续失败时只在
    // WiFi 层置“请求”，由 Esp8266Base 主类在同一主循环上下文先有序关闭上层网络
    // （Web/UDP/TCP），再调用 executeRadioReset() 做唯一物理执行，避免活跃 PCB 下
    // 切 WIFI_OFF 触发 netif_remove 崩溃，也避免云端故障误砸局域网。
    static bool requestRadioReset(Esp8266BaseRadioResetSource source);
    static bool consumeRadioResetRequest(Esp8266BaseRadioResetSource& source);
    static void executeRadioReset(Esp8266BaseRadioResetSource source);

    // AP 模式的 SSID（格式：ESP8266-Config-XXXX）
    static const char* apSSID();

private:
    static Esp8266BaseWiFiState _state;        // 1B
    static uint32_t             _connectStart; // 4B：连接尝试开始时刻；0 表示等待下次重试
    static uint32_t             _retryAt;      // 4B：下次重试的绝对 millis
    static uint8_t              _retryCount;   // 1B：已重试次数（用于区分首次/慢速）
    static bool                 _stuckRestarted; // 1B：本轮 attempt 是否已做过 stuck restart
    static uint8_t              _failuresSinceRadioReset; // 1B：距上次 radio reset 的连续失败次数
    static uint16_t             _attemptCount; // 2B：本次启动累计 WiFi.begin 次数
    static uint8_t              _radioResetCount; // 1B：本次启动完整 radio reset 次数
    static uint8_t              _radioResetRequest; // 1B：0=无 pending，否则为 RadioResetSource
    static Esp8266BaseRadioResetSource _lastRadioResetSource; // 最近一次 reset 来源
    static char                 _apSSID[28];   // 28B："ESP8266-Config-XXXX\0"
    static char                 _ip[16];       // 16B：点分十进制 IP
    static char                 _staSSID[64];  // 64B：缓存的 STA SSID，避免重连时重读 Flash
    static char                 _staPass[64];  // 64B：缓存的 STA 密码

    static void _startSTA(const char* ssid, const char* pass, bool keepAP = false);
    static void _beginSTA(const char* ssid, const char* pass, bool keepAP);
    static void _startAP();
    static void _handleConnected();
    static void _scheduleRetry();
    static bool _isDue(uint32_t now, uint32_t due);
    static void _updateIP();
    static const char* _statusName(uint8_t status);
};
