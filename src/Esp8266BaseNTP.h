#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------------------
// Esp8266BaseNTP — 可信网络对时（自研硬化 UDP 客户端为唯一设钟权威）
//
// 安全不变量：
//  - 不启动 lwIP 内置 SNTP（不调用 configTime/sntp_init），从结构上拒绝现场
//    路由器经 DHCP option 42 下发的时间源；另显式 sntp_servermode_dhcp(0)
//    作为纵深。
//  - 系统墙钟在每次启动内至多设置一次，且只在首次 MQTT/TLS 放行前，由两个
//    解析到不同 IP 的钉死权威源在有界 RTT 内、归一到同一单调时刻后结果一致
//    时才设置（见 Esp8266BaseNtpCore.h）。任何单源/同源/高延迟/不合理时间值
//    一律不采信（fail-closed）。
//  - WiFi 掉线重连不清除“已可信”闘锁、不再设钟；运行中只周期性双源校验，
//    发现偏差仅置可疑标志并记录日志，绝不回拨/前拨墙钟。
//
// WiFi 连接后由 Esp8266Base::handle() 调用 begin()；对时成功后切换日志时间戳。
// 默认：UTC+8，ntp.aliyun.com / ntp.tencent.com（第三源默认空，不参与）。
//
// RAM 预算：见 docs/04_memory_budget.md（ELF 实测）。
// ----------------------------------------------------------------------------

#ifndef ESP8266BASE_NTP_TIMEZONE
#define ESP8266BASE_NTP_TIMEZONE 28800   // UTC+8 = 8 * 3600（POSIX 串内部反号）
#endif

#ifndef ESP8266BASE_NTP_SYNC_INTERVAL
#define ESP8266BASE_NTP_SYNC_INTERVAL 3600  // 运行中只校验的周期，秒
#endif

#ifndef ESP8266BASE_NTP_SERVER_1
#define ESP8266BASE_NTP_SERVER_1 "ntp.aliyun.com"
#endif

#ifndef ESP8266BASE_NTP_SERVER_2
#define ESP8266BASE_NTP_SERVER_2 "ntp.tencent.com"
#endif

// 第三源默认空：双源固定使用前两个，移除了质量参差的公共池；保留宏仅为不破坏
// 编译期覆盖契约，非空时当前版本仍不参与信任判定。
#ifndef ESP8266BASE_NTP_SERVER_3
#define ESP8266BASE_NTP_SERVER_3 ""
#endif

class Esp8266BaseNTP {
public:
    // 配置时区/拒绝 DHCP 时间源；WiFi 每次（重）连接调用。已获可信时间后
    // 不会因此重新设钟。
    static bool begin();

    // 驱动双源采集（未可信时设钟；已可信后周期只校验）。
    static void handle();

    // WiFi 断开时取消在途采集并释放 UDP；不清除已可信闘锁和已设墙钟。
    static void reset();

    static bool     isSynced();   // 本次启动是否已获可信时间（闘锁，重连不清）
    static uint32_t timestamp();  // Unix 时间戳，未可信返回 0
    static bool     formatTo(char* out, size_t len, const char* fmt); // strftime

    static bool     hasMeasuredUncertainty();
    static uint32_t lastSyncRttMs();
    static uint16_t estimatedUncertaintyMs();

private:
    enum class Mode : uint8_t { ACQUIRE = 0, VERIFY = 1 };

    static bool     _clockTrusted;   // 1B：本次启动已获可信时间（latch）
    static bool     _timeSuspect;    // 1B：运行中双源一致但与本地钟显著不符
    static bool     _logSwitched;    // 1B：日志时间格式是否已切换
    static bool     _uncertaintyMeasured; // 1B
    static bool     _waiting;        // 1B：正在等待某个源的 UDP 应答
    static uint8_t  _mode;           // 1B：Mode
    static uint8_t  _serverIndex;    // 1B：当前等待的源 0/1
    static uint8_t  _attempts;       // 1B：采集失败退避计数
    static uint16_t _estimatedUncertaintyMs; // 2B：一致窗，不伪装硬上界
    static uint32_t _startedMs;      // 4B：本次 begin 时间
    static uint32_t _lastCheckMs;    // 4B：handle 节流
    static uint32_t _lastPendingLogMs; // 4B：未可信日志节流
    static uint32_t _nextActionMs;   // 4B：下次启动采集/运行校验时刻
    static uint32_t _sentMs;         // 4B：当前源请求发送时刻（RTT 起算，排除 DNS）
    static uint32_t _lastSyncRttMs;  // 4B：两源平均 RTT

    static bool _isDue(uint32_t now, uint32_t due);
    static void _startCycle(uint32_t now, Mode mode);
    static bool _requestServer(uint8_t index, uint32_t now);
    static bool _pollResponse();   // 收到合法响应并入样，返回 true
    static void _advanceOrFinish(uint32_t now);
    static void _finishAcquire(uint32_t now);
    static void _finishVerify(uint32_t now);
    static void _abortCycle(uint32_t now, const char* reason);
    static const char* _serverName(uint8_t index);  // PROGMEM 读到栈

    static void _finishSync(time_t t);
    static const char* _timeStr();
};
