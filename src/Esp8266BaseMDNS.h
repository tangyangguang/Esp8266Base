#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseMDNS — mDNS 服务
//
// WiFi 连接后由 Esp8266Base::handle() 首次触发 begin()
// 广播 hostname.local + _http._tcp:80
// hostname 由 Esp8266Base 启动时解析，最长 32 字符
//
// RAM 预算：<= 96B
// ----------------------------------------------------------------------------

class Esp8266BaseMDNS {
public:
    // 启动 mDNS，必须在 WiFi 连接后调用
    static bool begin(const char* hostname);

    // 确定性停止 mDNS 并释放其自持的 Core 内部 UdpContext（不在 WiFiUDP::stopAll
    // 链表里）。radio reset 切 WIFI_OFF 前必须调用，移除 netif 摘除时最疑似的
    // 活跃 UDP PCB。重连后由 begin() 重新启动。边界：Core 的 netif 状态回调不会
    // 因此注销，WIFI_OFF 仍可能 queue 一次内部 restart，故其余残留面仍靠真机证据。
    static void stop();

    // MDNS.update()，每轮调用
    static void handle();

    static bool isRunning();

private:
    static bool _running;  // 1B
    static bool _retryPending;
    static uint32_t _lastFailureMs;
};
