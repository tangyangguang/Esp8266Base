/**
 * sleep_watchdog — 软件看门狗 + 深度睡眠完整示例
 *
 * 功能演示：
 *   1. WDT 检测：启动后如果上次是 WDT 触发重启，打印警告 + 计数
 *   2. WDT 超时测试：串口发送 "freeze" 命令触发主循环卡死，验证 WDT 重启
 *   3. 深度睡眠循环：WiFi 连接后运行 30 秒，然后进入 10 秒 deep sleep，
 *                    醒来后打印唤醒原因并重新连接
 *   4. 计数器持久化：每次唤醒递增 test_counter 并通过 Config 持久化，
 *                    验证 deep sleep 前 flush 不丢数据
 *
 * 硬件要求：
 *   - deep sleep 需要 GPIO16 连接 RST 引脚
 *   - NodeMCU 开发板通常已连接，ESP-12F 裸板需手动飞线
 *
 * 串口命令（115200 波特率）：
 *   freeze    — 触发主循环卡死（验证 WDT 超时重启）
 *   sleep     — 立即进入 deep sleep
 *   diag      — 打印诊断信息
 *   clrwdt    — 清零 WDT 重启计数
 *
 * 烧录方法（PlatformIO）：
 *   pio run -e esp12f -t upload
 */

#include <Arduino.h>
#include "Esp8266Base.h"

// ── 配置 ────────────────────────────────────────────────────
static const uint32_t AWAKE_DURATION_MS     = 30000;  // 连接后保持清醒时间
static const uint32_t DEEP_SLEEP_DURATION_S = 10;     // 深度睡眠时间（秒）

// ── 运行状态 ─────────────────────────────────────────────────
static uint32_t connectedAt   = 0;
static bool     sleepArmed    = false;
static bool     freezeTest    = false;

// ── 串口命令处理 ─────────────────────────────────────────────
static char    serialBuf[32];
static uint8_t serialLen = 0;

static void processCommand(const char* cmd) {
    if (strcmp(cmd, "freeze") == 0) {
        ESP8266BASE_LOG_W("App ", "Freeze test: blocking loop for 5s (WDT should fire)");
        freezeTest = true;
    } else if (strcmp(cmd, "sleep") == 0) {
        ESP8266BASE_LOG_I("App ", "Manual deep sleep triggered");
        Esp8266BaseSleep::deepSleep(DEEP_SLEEP_DURATION_S);
    } else if (strcmp(cmd, "diag") == 0) {
        Esp8266Base::logDiagnostics();
        ESP8266BASE_LOG_I("App ", "WDT running=%d paused=%d",
                          (int)Esp8266BaseWatchdog::isRunning(),
                          (int)Esp8266BaseWatchdog::isPaused());
        ESP8266BASE_LOG_I("App ", "Sleep wake=%s deep_wake=%d",
                          Esp8266BaseSleep::wakeReason(),
                          (int)Esp8266BaseSleep::isDeepSleepWake());
        ESP8266BASE_LOG_I("App ", "Counter=%d",
                          Esp8266BaseConfig::getInt("test_counter"));
    } else if (strcmp(cmd, "clrwdt") == 0) {
        Esp8266BaseWatchdog::clearResetCount();
        ESP8266BASE_LOG_I("App ", "WDT reset count cleared");
    } else {
        ESP8266BASE_LOG_W("App ", "Unknown cmd: %s", cmd);
        Serial.println(F("Commands: freeze | sleep | diag | clrwdt"));
    }
}

static void handleSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialLen > 0) {
                serialBuf[serialLen] = '\0';
                processCommand(serialBuf);
                serialLen = 0;
            }
        } else if (serialLen < (sizeof(serialBuf) - 1)) {
            serialBuf[serialLen++] = c;
        }
    }
}

// ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();

    Esp8266Base::setFirmwareInfo("sleep-wdt", "0.2.0");

    Esp8266Base::begin();

    // 启动后立即检查 WDT 重启记录
    if (Esp8266BaseWatchdog::wasWatchdogReset()) {
        ESP8266BASE_LOG_W("App ", "*** WDT reset detected! Total WDT resets: %u ***",
                          (unsigned)Esp8266BaseWatchdog::resetCount());
    }

    // 打印唤醒原因
    ESP8266BASE_LOG_I("App ", "Wake reason: %s", Esp8266BaseSleep::wakeReason());

    // 深睡唤醒后打印持久化计数器
    if (Esp8266BaseSleep::isDeepSleepWake()) {
        ESP8266BASE_LOG_I("App ", "Deep sleep wake #%d",
                          Esp8266BaseConfig::getInt("test_counter"));
    }

    Serial.println(F("Commands: freeze | sleep | diag | clrwdt"));
}

void loop() {
    handleSerial();

    // freeze 测试：主动阻塞，触发 WDT
    if (freezeTest) {
        ESP8266BASE_LOG_W("App ", "Blocking now — WDT should restart in ~2s");
        freezeTest = false;  // WDT disabled builds should not stay stuck forever.
        delay(5000);  // 超过 WDT timeout（默认 2500ms），触发重启
    }

    Esp8266Base::handle();

    // WiFi 连接后计时，到时进入 deep sleep
    if (!sleepArmed && Esp8266BaseWiFi::isConnected()) {
        if (connectedAt == 0) {
            connectedAt = millis();
            ESP8266BASE_LOG_I("App ", "WiFi connected, will sleep in %us",
                              (unsigned)(AWAKE_DURATION_MS / 1000));
        }

        uint32_t elapsed = millis() - connectedAt;
        if (elapsed >= AWAKE_DURATION_MS) {
            sleepArmed = true;

            // 递增持久化计数器（验证 deepSleep 前 flush 不丢数据）
            int cnt = Esp8266BaseConfig::getInt("test_counter") + 1;
            Esp8266BaseConfig::setIntDeferred("test_counter", cnt);

            ESP8266BASE_LOG_I("App ", "Awake %us, counter=%d, entering deep sleep %ds",
                              (unsigned)(elapsed / 1000), cnt,
                              (unsigned)DEEP_SLEEP_DURATION_S);

            // deepSleep() 内部自动 flush + WiFi disconnect，不会返回
            Esp8266BaseSleep::deepSleep(DEEP_SLEEP_DURATION_S);
        }
    }
}
