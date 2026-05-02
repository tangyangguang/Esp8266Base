#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseWiFi — STA 自动连接 + AP 配网
//
// 状态机：
//   IDLE → CONNECTING → CONNECTED
//               ↓ 首次连接超时
//           AP_CONFIG ← connect() 保存新凭证后重试
//           CONNECTED → 掉线 → CONNECTING (慢速重试，不回 AP)
//
// RAM 预算：<= 256B（全局静态，不含 WiFi SDK 内部）
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_WIFI_CONNECT_TIMEOUT
#define ESP8266BASE_WIFI_CONNECT_TIMEOUT 15000   // ms：首次连接超时
#endif

#ifndef ESP8266BASE_WIFI_RETRY_FAST
#define ESP8266BASE_WIFI_RETRY_FAST 15000        // ms：掉线后首次重试间隔
#endif

#ifndef ESP8266BASE_WIFI_RETRY_SLOW
#define ESP8266BASE_WIFI_RETRY_SLOW 60000        // ms：后续慢速重试间隔
#endif

// WiFi 状态枚举
enum class Esp8266BaseWiFiState : uint8_t {
    IDLE       = 0,
    CONNECTING = 1,
    CONNECTED  = 2,
    AP_CONFIG  = 3,
    FAILED     = 4
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

    // 当前状态机状态
    static Esp8266BaseWiFiState state();

    // AP 模式的 SSID（格式：ESP8266-Config-XXXX）
    static const char* apSSID();

private:
    static Esp8266BaseWiFiState _state;        // 1B
    static uint32_t             _connectStart; // 4B：进入 CONNECTING 的时刻
    static uint32_t             _retryAt;      // 4B：下次重试的绝对 millis
    static uint8_t              _retryCount;   // 1B：已重试次数（用于区分首次/慢速）
    static bool                 _everConnected;// 1B：是否曾经连上（区分首次/断线重连）
    static char                 _apSSID[28];   // 28B："ESP8266-Config-XXXX\0"
    static char                 _ip[16];       // 16B：点分十进制 IP
    static char                 _staSSID[64];  // 64B：缓存的 STA SSID，避免重连时重读 Flash
    static char                 _staPass[64];  // 64B：缓存的 STA 密码

    static void _startSTA(const char* ssid, const char* pass);
    static void _startAP();
    static void _updateIP();
};
