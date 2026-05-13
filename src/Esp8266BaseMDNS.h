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

    // MDNS.update()，每轮调用
    static void handle();

    static bool isRunning();

private:
    static bool _running;  // 1B
};
