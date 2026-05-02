# Esp8266Base API 参考

> 版本：1.0.0  
> 平台：ESP8266 Arduino Core（仅限）  
> 前缀：`Esp8266Base` / `ESP8266BASE_`  
> 所有模块均为静态类，无需实例化。

---

## 目录

- [Esp8266Base — 主入口](#1-esp8266base--主入口)
- [Esp8266BaseLog — 日志](#2-esp8266baselog--日志)
- [Esp8266BaseConfig — 配置存储](#3-esp8266baseconfig--配置存储)
- [Esp8266BaseWiFi — WiFi 管理](#4-esp8266basewifi--wifi-管理)
- [Esp8266BaseWeb — Web 服务](#5-esp8266baseweb--web-服务)
- [Esp8266BaseOTA — OTA 更新](#6-esp8266baseota--ota-更新)
- [Esp8266BaseNTP — 网络对时](#7-esp8266basentp--网络对时)
- [Esp8266BaseMDNS — mDNS 服务](#8-esp8266basemdns--mdns-服务)
- [Esp8266BaseSleep — 睡眠管理](#9-esp8266basesleep--睡眠管理)
- [Esp8266BaseWatchdog — 看门狗](#10-esp8266basewatchdog--看门狗)
- [编译宏](#11-编译宏)

---

## 1. Esp8266Base — 主入口

头文件：`Esp8266Base.h`

### 函数

```cpp
static bool begin();
```
初始化所有已启用的模块，严格按固定顺序：Log → Sleep → Config → WiFi → Watchdog → Web → OTA。输出启动诊断日志。返回 `false` 仅表示 Config（LittleFS）初始化失败，其余模块仍会继续初始化。

```cpp
static void handle();
```
在 `loop()` 中每轮调用。推进所有模块的非阻塞状态机，调用顺序：Config → WiFi → NTP触发 → mDNS触发 → NTP → mDNS → Web → Watchdog。

```cpp
static void setFirmwareInfo(const char* name, const char* version);
```
设置固件名称和版本号（显示在启动日志和 Web 标题中）。必须在 `begin()` 前调用。

```cpp
static void setHostname(const char* hostname);
```
设置设备 hostname，用于 AP SSID 后缀、mDNS 名称、Web 页面标题。最长 24 字符。必须在 `begin()` 前调用。

```cpp
static void enableWeb(bool enabled);
static void enableOTA(bool enabled);
static void enableNTP(bool enabled);
static void enableMDNS(bool enabled);
static void enableWatchdog(bool enabled);
```
在 `begin()` 前调用，控制各模块是否启用。默认全部启用。

```cpp
static const char* firmwareName();
static const char* firmwareVersion();
static const char* hostname();
```
查询已设置的固件信息和 hostname。

### 使用示例

```cpp
#include "Esp8266Base.h"

void setup() {
    Serial.begin(115200);
    Esp8266Base::setFirmwareInfo("fan-ctrl", "1.0.0");
    Esp8266Base::setHostname("esp-fan");
    Esp8266Base::begin();

    Esp8266BaseWeb::addPage("/fan", handleFanPage);
    Esp8266BaseWeb::addApi("/api/fan", handleFanApi);
}

void loop() {
    Esp8266Base::handle();
}
```

---

## 2. Esp8266BaseLog — 日志

头文件：`Esp8266BaseLog.h`

### 函数

```cpp
static void begin(uint8_t level = ESP8266BASE_LOG_LEVEL);
```
初始化日志模块，设置初始日志等级。

```cpp
static void setLevel(uint8_t level);
```
运行时修改日志等级。等级值：`0`=DEBUG, `1`=INFO, `2`=WARN, `3`=ERROR, `4`=关闭。

```cpp
typedef const char* (*TimeProviderFn)();
static void setTimeProvider(TimeProviderFn fn);
```
注入时间字符串回调。NTP 同步成功后，`Esp8266BaseNTP` 自动调用此函数切换为绝对时间格式。

### 日志宏

```cpp
ESP8266BASE_LOG_D(tag, fmt, ...)   // DEBUG
ESP8266BASE_LOG_I(tag, fmt, ...)   // INFO
ESP8266BASE_LOG_W(tag, fmt, ...)   // WARN
ESP8266BASE_LOG_E(tag, fmt, ...)   // ERROR
```

`tag`：最长 12 字符，输出固定 4 字符宽度。  
`fmt`：printf 格式字符串，内部栈缓冲 128B，超长截断。  
低于当前等级的宏在编译期完全消除，零运行时开销。

### 日志格式

NTP 对时前（millis 时间戳）：
```
[1234][I][WiFi] Connected ip=192.168.1.100 rssi=-65
```

NTP 对时后（绝对时间）：
```
[14:32:01][I][NTP ] Synced timestamp=1746087121
```

### 使用示例

```cpp
ESP8266BASE_LOG_I("App ", "Starting fan-ctrl v1.0.0");
ESP8266BASE_LOG_D("Fan ", "Speed=%d rpm", speed);
ESP8266BASE_LOG_W("WiFi", "Signal weak rssi=%d", WiFi.RSSI());
ESP8266BASE_LOG_E("Cfg ", "Failed to save key=%s", key);
```

---

## 3. Esp8266BaseConfig — 配置存储

头文件：`Esp8266BaseConfig.h`

### 函数

```cpp
static bool begin();
```
挂载 LittleFS。首次挂载失败时自动 format 后重试。返回 `false` 表示文件系统不可用。

```cpp
static bool setStr(const char* key, const char* value);
static bool getStr(const char* key, char* out, size_t len, const char* def = "");
```
字符串 KV。key 最长 24 字符，value 最长 96 字符（超过返回 false）。写入前比较旧值，无变化不写 Flash。`getStr` 失败时将 `def` 写入 `out`。

```cpp
static bool setInt(const char* key, int32_t value);
static int32_t getInt(const char* key, int32_t def = 0);
```
int32 KV（内部以十进制字符串存储）。

```cpp
static bool setBool(const char* key, bool value);
static bool getBool(const char* key, bool def = false);
```
bool KV（存储为 `"1"` / `"0"`）。

```cpp
static bool setIntDeferred(const char* key, int32_t value);
static bool setBoolDeferred(const char* key, bool value);
```
延迟写入。写入进入内存队列（4条），由 `handle()` 逐步刷入 Flash。同一 key 重复写入只保留最新值。队列满时返回 `false`。

```cpp
static void handle();
```
每轮最多刷 1 条 pending 写入。通过 `Esp8266Base::handle()` 自动调用。

```cpp
static bool flush();
```
强制写完所有 pending 写入。在 deep sleep 或 `ESP.restart()` 前调用。

```cpp
static uint8_t pendingCount();
static bool isReady();
```
查询 deferred 队列中待写条数，及文件系统是否已就绪。

### 约束

- key 最长 24 字符
- string 值最长 96 字符
- deferred 队列 4 条，满则拒绝新写入
- 每次写入后自动 `yield()`
- OTA 期间不触发新的 deferred 写入

### 保留 key

以下 key 由库内部使用，应用代码不得覆盖：

| Key | 用途 |
|-----|------|
| `wifi_ssid` | WiFi STA SSID |
| `wifi_pass` | WiFi STA 密码 |
| `ap_pass` | AP 配网密码 |
| `hostname` | 设备 hostname |
| `web_user` | Web Auth 用户名 |
| `web_pass` | Web Auth 密码 |
| `wdt_count` | WDT 重启累计次数 |
| `wdt_pending` | 上次是否 WDT 重启 |
| `boot_count` | 启动次数 |

### 使用示例

```cpp
// 高频写入用 deferred（如启动计数）
int32_t cnt = Esp8266BaseConfig::getInt("boot_count", 0) + 1;
Esp8266BaseConfig::setIntDeferred("boot_count", cnt);

// 低频配置立即写
Esp8266BaseConfig::setStr("wifi_ssid", ssid);

// 重启前强制刷盘
Esp8266BaseConfig::flush();
ESP.restart();
```

---

## 4. Esp8266BaseWiFi — WiFi 管理

头文件：`Esp8266BaseWiFi.h`

### 枚举

```cpp
enum class Esp8266BaseWiFiState : uint8_t {
    IDLE, CONNECTING, CONNECTED, AP_CONFIG, FAILED
};
```

### 函数

```cpp
static bool begin();
```
从 Config 读取凭证并缓存到内存，启动 WiFi 状态机（非阻塞）。无凭证时直接进入 AP_CONFIG。

```cpp
static void handle();
```
状态机推进，通过 `Esp8266Base::handle()` 每轮调用。重连时使用内存缓存凭证，不读 Flash。

```cpp
static bool connect(const char* ssid, const char* pass);
```
保存新凭证到 Config 并更新内存缓存，立即尝试连接。Web `/wifi` POST 时调用。

```cpp
static bool clearCredentials();
```
清除保存的 WiFi 凭证。下次重启将进入 AP 配网。

```cpp
static bool isConnected();
static const char* ip();
static Esp8266BaseWiFiState state();
static const char* apSSID();
```
状态查询。`ip()` 未连接时返回空字符串。`apSSID()` 格式：`ESP8266-Config-XXXX`（后4位为 ChipID）。

### 默认参数

| 参数 | 默认值 |
|------|--------|
| STA 连接超时 | 15000ms（`ESP8266BASE_WIFI_CONNECT_TIMEOUT`） |
| 首次重试间隔 | 15000ms（`ESP8266BASE_WIFI_RETRY_FAST`） |
| 慢速重试间隔 | 60000ms（`ESP8266BASE_WIFI_RETRY_SLOW`） |
| AP SSID | `ESP8266-Config-<ChipID后4位>` |
| AP 密码 | 空（开放），可通过 key `ap_pass` 配置 |

---

## 5. Esp8266BaseWeb — Web 服务

头文件：`Esp8266BaseWeb.h`

### 类型定义

```cpp
typedef void (*Esp8266BaseWebHandler)();
```

### 函数

```cpp
static bool begin();
```
启动 ESP8266WebServer（端口 80），注册所有内置路由。

```cpp
static void handle();
```
`server.handleClient()`，每轮调用。

```cpp
static bool addPage(const char* path, Esp8266BaseWebHandler handler);
static bool addApi (const char* path, Esp8266BaseWebHandler handler);
```
注册应用路由。必须在 `Esp8266Base::begin()` 之后调用。`addPage` 注册 GET，`addApi` 同时响应 GET 和 POST。路径最长 24 字符，上限分别 4 / 6 个。

```cpp
static void sendHeader();
static void sendFooter();
static void sendContent_P(PGM_P content);
static void sendChunk(const char* content);
```
页面输出辅助函数。`sendContent_P` 单遍从 PROGMEM 读取，无需预计算长度。

```cpp
static bool checkAuth();
```
验证 Basic Auth，失败时自动发送 401 并返回 `false`。在所有自定义 handler 开头调用。

```cpp
static bool verifyAuth();
```
仅验证 Basic Auth，不发送 401 响应（供 OTA 上传 handler 在接收数据前检测认证）。

```cpp
static void setAuth(const char* user, const char* pass);
static void setTitle(const char* hostname, const char* fw, const char* ver);
static ESP8266WebServer& server();
static bool isRunning();
```

### 内置路由

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 首页（WiFi 状态 + 功能链接） |
| `/wifi` | GET | WiFi 设置表单 |
| `/wifi` | POST | 保存凭证并重连 |
| `/ota` | GET | OTA 上传页面 |
| `/ota` | POST | 接收固件（由 Esp8266BaseOTA 处理） |
| `/reboot` | GET | 延迟 500ms 后重启 |
| `/health` | GET | JSON 健康信息（heap/maxBlock/ip/uptime/wifi，无需认证） |

### 使用示例

```cpp
static const char SENSOR_PAGE[] PROGMEM =
    "<h2>Sensor</h2>";

void handleSensorPage() {
    if (!Esp8266BaseWeb::checkAuth()) return;
    Esp8266BaseWeb::sendHeader();
    Esp8266BaseWeb::sendContent_P(SENSOR_PAGE);
    char buf[48];
    snprintf(buf, sizeof(buf), "<p>Temp: %.1f C</p>", getTemperature());
    Esp8266BaseWeb::sendChunk(buf);
    Esp8266BaseWeb::sendFooter();
}
```

---

## 6. Esp8266BaseOTA — OTA 更新

头文件：`Esp8266BaseOTA.h`

OTA 模块由 `Esp8266Base` 在内部初始化，应用代码通常无需直接调用。

### 函数

```cpp
static bool begin();
```
注册 POST /ota 上传处理。必须在 `Esp8266BaseWeb::begin()` 之后调用。

```cpp
static bool isInProgress();
```
OTA 上传是否正在进行。

### OTA 过程行为

1. 收到 POST /ota 请求时，立即校验 Basic Auth（使用 `Esp8266BaseWeb::verifyAuth()`）
2. 认证失败：丢弃所有数据块，上传结束后返回 HTTP 401
3. 认证成功：`Esp8266BaseWatchdog::pause()`，分块写入固件，每块后 `yield()`
4. 上传完成：`Esp8266BaseWatchdog::resume()`，延迟 500ms 后 `ESP.restart()`
5. 上传失败：`Esp8266BaseWatchdog::resume()`，返回简短错误信息
6. 上传中止：`Update.end()`，`Esp8266BaseWatchdog::resume()`

OTA 使用 `ESP.getFreeSketchSpace()` 作为写入空间，不使用 `UPDATE_SIZE_UNKNOWN`（该常量仅 ESP32 有效）。

---

## 7. Esp8266BaseNTP — 网络对时

头文件：`Esp8266BaseNTP.h`

### 函数

```cpp
static bool begin();
```
配置 NTP 服务器和时区，启动对时。由 `Esp8266Base::handle()` 在 WiFi 首次连接时自动调用。

```cpp
static void handle();
```
检查同步状态，成功后自动调用 `Esp8266BaseLog::setTimeProvider()` 切换日志时间格式。

```cpp
static bool isSynced();
static uint32_t timestamp();
```
`timestamp()` 返回当前 Unix 时间戳，未同步时返回 0。

```cpp
static bool formatTo(char* out, size_t len, const char* fmt);
```
使用 `strftime` 格式化当前时间。返回 `false` 表示未同步或 buf 太小。

### 默认配置

| 参数 | 默认值 |
|------|--------|
| 时区偏移 | 28800（UTC+8，`ESP8266BASE_NTP_TIMEZONE`） |
| NTP 服务器 | ntp.aliyun.com, ntp.tencent.com, cn.pool.ntp.org |
| 重新同步间隔 | 3600s（`ESP8266BASE_NTP_SYNC_INTERVAL`） |

### 使用示例

```cpp
char timeStr[20];
if (Esp8266BaseNTP::formatTo(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S")) {
    ESP8266BASE_LOG_I("App ", "Time: %s", timeStr);
}
```

---

## 8. Esp8266BaseMDNS — mDNS 服务

头文件：`Esp8266BaseMDNS.h`

### 函数

```cpp
static bool begin(const char* hostname);
```
启动 mDNS，广播 `_http._tcp`（端口 80）。由 `Esp8266Base::handle()` 在 WiFi 连接时自动调用。WiFi 掉线后会重置，重连时自动重新启动。

```cpp
static void handle();
```
`MDNS.update()`，每轮调用。

```cpp
static bool isRunning();
```

### 约束

- hostname 最长 24 字符，来自 `Esp8266Base::setHostname()`
- 不支持运行时改名并自动重启
- 不支持复杂 TXT 记录管理

---

## 9. Esp8266BaseSleep — 睡眠管理

头文件：`Esp8266BaseSleep.h`

### 函数

```cpp
static bool begin();
```
读取并记录唤醒原因，在 `Esp8266Base::begin()` 的最早期调用。

```cpp
static void modemSleep();
```
关闭 WiFi radio，CPU 继续运行（电流 ~70mA → ~15mA）。WiFi 将断开。

```cpp
static void wakeModem();
```
重新启用 WiFi radio。需要外部重新发起 WiFi 连接。

```cpp
static void deepSleep(uint32_t sleepSec);
```
进入 deep sleep（单位：秒，最大 3600）。内部自动执行：

1. `Esp8266BaseWatchdog::pause()`
2. `Esp8266BaseConfig::flush()`
3. `WiFi.disconnect()` + `WiFi.mode(WIFI_OFF)`
4. 延迟 100ms
5. `ESP.deepSleep(sleepSec * 1000000ULL, WAKE_RF_DEFAULT)`

> **硬件要求**：deep sleep 需要 GPIO16 连接到 RST 引脚。

```cpp
static const char* wakeReason();
static bool isDeepSleepWake();
```

`wakeReason()` 返回值：

| 返回字符串 | 触发原因 |
|-----------|---------|
| `"power-on"` | 上电或外部复位（DEFAULT_RST / EXT_SYS_RST） |
| `"deep-sleep"` | deep sleep 到期唤醒 |
| `"soft-restart"` | 软件调用 ESP.restart() |
| `"wdt-reset"` | Soft WDT 或硬件 WDT 超时重启 |
| `"unknown"` | 其他原因 |

### 使用示例

```cpp
void setup() {
    Esp8266Base::begin();
    if (Esp8266BaseSleep::isDeepSleepWake()) {
        ESP8266BASE_LOG_I("App ", "Woke from deep sleep");
    }
}

// 采集后进入 deep sleep
void collectAndSleep() {
    float temp = readSensor();
    ESP8266BASE_LOG_I("App ", "Temp=%.1f, sleeping 60s", temp);
    Esp8266BaseSleep::deepSleep(60);
}
```

---

## 10. Esp8266BaseWatchdog — 看门狗

头文件：`Esp8266BaseWatchdog.h`

### 函数

```cpp
static bool begin(uint32_t timeoutMs = ESP8266BASE_WDT_TIMEOUT_MS);
```
初始化，设置超时时间（范围 1000–3000ms，超出自动截断）。读取上次 WDT 重启记录。

```cpp
static void handle();
static void feed();
```
`handle()` 检查主循环活性，超时时写入重启记录后执行 `ESP.restart()`。`feed()` 重置计时器。两者均由 `Esp8266Base::handle()` 自动调用。长阻塞操作中需手动调用 `feed()`。

```cpp
static void pause();
static void resume();
```
暂停/恢复看门狗检查。OTA 上传期间自动调用，长阻塞操作前也应调用。

```cpp
static bool wasWatchdogReset();
static uint32_t resetCount();
static void clearResetCount();
```
WDT 重启历史查询与清零（从 Config key `wdt_pending` / `wdt_count` 读取）。

### 默认配置

| 参数 | 默认值 |
|------|--------|
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` |
| 最小超时 | 1000ms |
| 最大超时 | 3000ms（不超过 ESP8266 Soft WDT） |

### 使用示例

```cpp
void setup() {
    Esp8266Base::begin();
    if (Esp8266BaseWatchdog::wasWatchdogReset()) {
        ESP8266BASE_LOG_W("WDT ", "WDT reset count=%lu",
                          (unsigned long)Esp8266BaseWatchdog::resetCount());
    }
}

void loop() {
    Esp8266Base::handle();  // 内部自动喂狗

    // 长操作时手动喂狗
    for (int i = 0; i < 500; i++) {
        doSlowWork(i);
        if (i % 50 == 0) {
            Esp8266BaseWatchdog::feed();
            yield();
        }
    }
}
```

---

## 11. 编译宏

在 `platformio.ini` 的 `build_flags` 中设置：

| 宏 | 默认值 | 说明 |
|---|---|---|
| `ESP8266BASE_LOG_LEVEL` | `1` | 日志等级：0=D, 1=I, 2=W, 3=E, 4=关闭 |
| `ESP8266BASE_WEB_MAX_APP_PAGES` | `4` | 应用页面最大注册数 |
| `ESP8266BASE_WEB_MAX_APP_APIS` | `6` | 应用 API 最大注册数 |
| `ESP8266BASE_WEB_AUTH_USER` | `"admin"` | Basic Auth 用户名 |
| `ESP8266BASE_WEB_AUTH_PASS` | `"esp8266"` | Basic Auth 密码 |
| `ESP8266BASE_NTP_TIMEZONE` | `28800` | 时区偏移秒（UTC+8 = 8×3600） |
| `ESP8266BASE_NTP_SYNC_INTERVAL` | `3600` | NTP 重新同步间隔（秒） |
| `ESP8266BASE_WDT_TIMEOUT_MS` | `2500` | 看门狗超时毫秒 |
| `ESP8266BASE_CFG_DEFERRED_SIZE` | `4` | deferred 写入队列长度 |
| `ESP8266BASE_CFG_KEY_MAX` | `24` | Config key 最大字符数 |
| `ESP8266BASE_CFG_STR_MAX` | `96` | Config string value 最大字节数 |
| `ESP8266BASE_WIFI_CONNECT_TIMEOUT` | `15000` | WiFi STA 连接超时 ms |
| `ESP8266BASE_WIFI_RETRY_FAST` | `15000` | WiFi 首次重试间隔 ms |
| `ESP8266BASE_WIFI_RETRY_SLOW` | `60000` | WiFi 慢速重试间隔 ms |
| `ESP8266BASE_SLEEP_MAX_DEEP_SEC` | `3600` | deepSleep 最大秒数上限 |
